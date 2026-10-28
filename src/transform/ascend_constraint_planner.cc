// Copyright (c) Tile-AI Corporation.
// Licensed under the MIT License.

#include <tvm/arith/analyzer.h>
#include <tvm/runtime/registry.h>
#include <tvm/tir/stmt_functor.h>

#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "./common/ascend_constraint.h"
#include "./common/ascend_pipeline_structure.h"
#include "./common/ascend_schedule_hints.h"

namespace tvm {
namespace tl {

TVM_REGISTER_PASS_CONFIG_OPTION(kAscendPipelinePlanner, String);

namespace {
using namespace tir;
using Record = Map<String, ObjectRef>;

int64_t IntegerField(const Record &record, const char *key) {
  const auto *value = record.at(key).as<IntImmNode>();
  ICHECK(value && value->dtype.is_int())
      << "Malformed constraint field: " << key;
  return value->value;
}

struct Candidate {
  int loop_id;
  Array<Integer> units;
  Array<ObjectRef> paths;
  Array<Integer> order;
};

void ValidateUBDeclaration(DataType dtype, const Array<PrimExpr> &extents) {
  ICHECK(dtype.is_float() && dtype.is_scalar() &&
         (dtype.bits() == 16 || dtype.bits() == 32))
      << "Constraint planner supports float16/float32 UB allocations only";
  const int64_t element_budget = kAscendA2A3UBBudget / dtype.bytes();
  int64_t elements = 1;
  for (const PrimExpr &extent : extents) {
    const auto *value = extent.as<IntImmNode>();
    // Validate each declaration dimension before multiplication. A capacity
    // already folded by TIR arithmetic may have wrapped back to a small value.
    ICHECK(value && !value->dtype.is_bool() && value->value > 0 &&
           value->value <= element_budget / elements)
        << "Constraint UB allocation extent/product exceeds the A3 UB budget "
           "or is not a positive static extent";
    elements *= value->value;
  }
}

class ConstraintInputs : public StmtExprVisitor {
public:
  explicit ConstraintInputs(PrimFunc function) {
    size_t units = 0;
    PostOrderVisit(function->body, [&](const ObjectRef &node) {
      if (const auto *block = node.as<BlockNode>()) {
        for (const Buffer &buffer : block->alloc_buffers) {
          if (buffer.scope() == "shared.ub")
            ValidateUBDeclaration(buffer->dtype, buffer->shape);
        }
      } else if (const auto *allocation = node.as<AllocateNode>()) {
        const auto *pointer =
            allocation->buffer_var->type_annotation.as<PointerTypeNode>();
        if (pointer && pointer->storage_scope == "shared.ub")
          ValidateUBDeclaration(allocation->dtype, allocation->extents);
      }
      const auto *evaluate = node.as<EvaluateNode>();
      if (evaluate && evaluate->value.as<CallNode>())
        ICHECK_LE(++units, 4096U)
            << "Constraint hardware source-unit limit exceeded";
    });
    graph = AnalyzeAscendSchedule(function, "A3");
    tasks = Downcast<Array<ObjectRef>>(graph.at("tasks"));
    loops = Downcast<Array<ObjectRef>>(graph.at("loops"));
    storages = Downcast<Array<ObjectRef>>(graph.at("storages"));
    for (size_t i = 0; i < tasks.size(); ++i) {
      Record task = Downcast<Record>(tasks[i]);
      const Stmt &statement = Downcast<Stmt>(task.at("statement"));
      task_ids.emplace(statement.get(), i);
      ICHECK(is_one(Downcast<PrimExpr>(task.at("guard"))))
          << "Constraint planner does not support guarded hardware calls";
    }
    for (const ObjectRef &launch :
         Downcast<Array<ObjectRef>>(graph.at("launch_domains"))) {
      Record record = Downcast<Record>(launch);
      IterVar variable = Downcast<IterVar>(record.at("iter_var"));
      PrimExpr extent = Downcast<PrimExpr>(record.at("extent"));
      const auto *constant = extent.as<IntImmNode>();
      ICHECK(constant && constant->value > 0)
          << "Constraint planner requires static launch domains";
      analyzer.Bind(variable->var,
                    Range::FromMinExtent(make_zero(variable->var.dtype()),
                                         cast(variable->var.dtype(), extent)));
    }
    VisitStmt(function->body);
    ICHECK_EQ(next_loop_, loops.size()) << "Constraint loop identity mismatch";
    ICHECK(!candidates.empty())
        << "Constraint planner requires at least one num_stages=1 candidate";
    ValidateFootprints(function);
  }

  Record graph;
  Array<ObjectRef> tasks, loops, storages;
  std::unordered_map<const Object *, int> task_ids;
  std::vector<Candidate> candidates;
  std::unordered_set<int> empty_loops;

private:
  void VisitStmt_(const ForNode *loop) final {
    int loop_id = next_loop_++;
    ICHECK(loop->kind == ForKind::kSerial)
        << "Constraint planner supports static serial loops only";
    DataType dtype = loop->loop_var.dtype();
    ICHECK(dtype.is_int() && dtype.is_scalar() &&
           (dtype.bits() == 32 || dtype.bits() == 64))
        << "Constraint planner requires signed int32/int64 scalar loop "
           "variables";
    const auto *minimum = loop->min.as<IntImmNode>();
    const auto *extent = loop->extent.as<IntImmNode>();
    int64_t dtype_max = dtype.bits() == 32
                            ? std::numeric_limits<int32_t>::max()
                            : std::numeric_limits<int64_t>::max();
    int64_t dtype_min = dtype.bits() == 32
                            ? std::numeric_limits<int32_t>::min()
                            : std::numeric_limits<int64_t>::min();
    ICHECK(minimum && extent && extent->value >= 0 &&
           extent->value <= dtype_max && minimum->value >= dtype_min &&
           minimum->value <= dtype_max - extent->value)
        << "Constraint planner requires representable static loop domains";
    Record loop_record = Downcast<Record>(loops[loop_id]);
    ICHECK(Downcast<Var>(loop_record.at("var")).same_as(loop->loop_var))
        << "Constraint loop source order mismatch";
    if (bound_.insert(loop->loop_var.get()).second) {
      analyzer.Bind(loop->loop_var,
                    Range::FromMinExtent(loop->min, loop->extent));
    }
    for (const auto &annotation : loop->annotations) {
      std::string key = annotation.first;
      ICHECK(key != "tl_pipeline_order" && key != "tl_pipeline_stage" &&
             key != "tl_pipeline_sync" && key != "tl_pipeline_group" &&
             key != "enable_offset" && key.rfind("software_pipeline_", 0) != 0)
          << "Constraint planner rejects manual pipeline controls: " << key;
      if (key == "tl_cross_interval") {
        const auto *value = annotation.second.as<IntImmNode>();
        ICHECK(value && value->value == 1)
            << "Constraint planner rejects cross-core intervals";
      }
    }
    auto requested = loop->annotations.Get("num_stages");
    int64_t stage = 0;
    if (requested.defined()) {
      const auto *value = requested.value().as<IntImmNode>();
      ICHECK(value && value->dtype.is_int())
          << "Malformed constraint num_stages";
      stage = value->value;
      ICHECK(stage == 0 || stage == 1)
          << "Constraint planner supports num_stages=1 only; multi-stage "
             "requires S5";
    }
    std::vector<int> members = Members(loop->body);
    if (members.empty()) {
      empty_loops.insert(loop_id);
    } else if (stage == 1) {
      ICHECK_EQ(candidate_depth_, 0)
          << "Nested constraint candidates are unsupported";
      ICHECK_GT(extent->value, 0)
          << "Constraint candidate must have a positive trip count";
      Stmt body = loop->body;
      if (const auto *realize = body.as<BlockRealizeNode>()) {
        ICHECK(is_one(realize->predicate))
            << "Guarded constraint block is unsupported";
        body = realize->block->body;
      }
      const auto *sequence = body.as<SeqStmtNode>();
      ICHECK(sequence)
          << "Constraint candidate body must be an injector SeqStmt";
      Candidate candidate{loop_id, {}, {}, {}};
      for (const auto &element :
           CollectAscendPipelineGroups(GetRef<SeqStmt>(sequence))) {
        if (element.allocation.defined())
          continue;
        auto unit = Members(element.statement);
        ICHECK_EQ(unit.size(), 1U) << "Each constraint injector unit must "
                                      "contain exactly one hardware call";
        bool nested = false;
        PostOrderVisit(element.statement, [&](const ObjectRef &node) {
          nested |= node.as<ForNode>() != nullptr;
        });
        ICHECK(!nested) << "Constraint units cannot contain nested loops";
        candidate.units.push_back(Integer(unit[0]));
        candidate.paths.push_back(element.source_path);
      }
      ICHECK_EQ(candidate.units.size(), members.size())
          << "Constraint unit coverage mismatch";
      candidates.push_back(candidate);
      ++candidate_depth_;
      StmtExprVisitor::VisitStmt_(loop);
      --candidate_depth_;
      return;
    }
    StmtExprVisitor::VisitStmt_(loop);
  }

  std::vector<int> Members(const Stmt &body) const {
    std::vector<int> members;
    PostOrderVisit(body, [&](const ObjectRef &node) {
      auto found = task_ids.find(node.get());
      if (found != task_ids.end())
        members.push_back(found->second);
    });
    return members;
  }

  void ValidateFootprints(const PrimFunc &function) {
    struct Allocation {
      DataType dtype;
      Array<PrimExpr> shape;
    };
    std::unordered_map<const VarNode *, Allocation> declarations;
    PostOrderVisit(function->body, [&](const ObjectRef &node) {
      if (const auto *block = node.as<BlockNode>()) {
        for (const Buffer &buffer : block->alloc_buffers)
          declarations.emplace(buffer->data.get(),
                               Allocation{buffer->dtype, buffer->shape});
      } else if (const auto *allocation = node.as<AllocateNode>()) {
        declarations.emplace(
            allocation->buffer_var.get(),
            Allocation{allocation->dtype, allocation->extents});
      }
    });
    auto initial =
        function->GetAttr<Map<Var, Array<PrimExpr>>>("initial_buffer_shapes")
            .value_or(Map<Var, Array<PrimExpr>>());
    int64_t reserved_bytes = 0;
    for (const ObjectRef &object : storages) {
      Record storage = Downcast<Record>(object);
      if (!Downcast<Bool>(storage.at("physical_allocation"))->value)
        continue;
      ICHECK(Downcast<String>(storage.at("scope")) == "shared.ub")
          << "Constraint planner supports UB allocations only";
      ICHECK(!storage.count("explicit_address"))
          << "Constraint planner does not support explicit address placement";
      int64_t bytes = IntegerField(storage, "capacity_bytes");
      ICHECK(bytes > 0 && bytes <= kAscendA2A3UBBudget)
          << "Constraint allocation exceeds the A3 UB budget";
      Var data = Downcast<Var>(storage.at("data"));
      ICHECK(declarations.count(data.get()))
          << "Missing constraint allocation declaration";
      const Allocation &allocation = declarations.at(data.get());
      ICHECK(allocation.dtype.is_float() && allocation.dtype.is_scalar() &&
             (allocation.dtype.bits() == 16 || allocation.dtype.bits() == 32))
          << "Constraint planner supports float16/float32 UB allocations only";
      Array<PrimExpr> shape =
          initial.count(data) ? initial.at(data) : allocation.shape;
      PrimExpr inner_expr = shape.empty() ? Integer(1) : shape.back();
      PrimExpr simplified_inner = analyzer.Simplify(inner_expr);
      const auto *inner_value = simplified_inner.as<IntImmNode>();
      ICHECK(inner_value && inner_value->value > 0)
          << "Constraint allocation layout must be static and positive";
      int64_t inner = inner_value->value;
      int64_t element_bytes = allocation.dtype.bytes();
      ICHECK(bytes % element_bytes == 0 &&
             inner <= kAscendA2A3UBBudget / element_bytes)
          << "Constraint allocation layout exceeds the A3 UB budget";
      int64_t elements = bytes / element_bytes;
      int64_t aligned_bytes;
      if (inner == 1) {
        // Flatten2DBuffer swaps this case to one contiguous UB row.
        aligned_bytes = (bytes + 31) / 32 * 32;
      } else {
        int64_t rows = elements / inner + (elements % inner != 0);
        int64_t row_bytes = (inner * element_bytes + 31) / 32 * 32;
        ICHECK(rows <= kAscendA2A3UBBudget / row_bytes)
            << "Constraint allocation layout exceeds the A3 UB budget";
        aligned_bytes = rows * row_bytes;
      }
      ICHECK(reserved_bytes <= kAscendA2A3UBBudget - aligned_bytes)
          << "Constraint allocations exceed the A3 UB budget without reuse";
      reserved_bytes += aligned_bytes;
    }
    for (const ObjectRef &object : tasks) {
      Record task = Downcast<Record>(object);
      for (const char *kind : {"reads", "writes"}) {
        for (const ObjectRef &access_object :
             Downcast<Array<ObjectRef>>(task.at(kind))) {
          Record access = Downcast<Record>(access_object);
          Record storage =
              Downcast<Record>(storages[IntegerField(access, "storage_id")]);
          PrimExpr start = Downcast<PrimExpr>(access.at("start"));
          PrimExpr end = Downcast<PrimExpr>(access.at("end"));
          PrimExpr capacity = Downcast<PrimExpr>(storage.at("capacity_bytes"));
          ICHECK(
              analyzer.CanProve(start >= 0 && end >= start && end <= capacity))
              << "Constraint planner cannot prove access bounds in the static "
                 "loop domain";
        }
      }
    }
  }

  arith::Analyzer analyzer;
  std::unordered_set<const VarNode *> bound_;
  size_t next_loop_{0};
  int candidate_depth_{0};
};

void SolveCandidate(Candidate *candidate, const ConstraintInputs &inputs) {
  const runtime::PackedFunc *solve =
      runtime::Registry::Get("tl.ascend.solve_schedule");
  ICHECK(solve)
      << "Constraint planner requires the optional ascend-scheduler solver; "
         "use the Python PipelinePlanning wrapper to register it";
  size_t count = candidate->units.size();
  Array<Integer> costs, flags;
  std::unordered_map<int, int> local;
  for (size_t i = 0; i < count; ++i) {
    local[candidate->units[i]->value] = i;
    costs.push_back(Integer(1));
    Record task = Downcast<Record>(inputs.tasks[candidate->units[i]->value]);
    String pipe = Downcast<String>(task.at("pipe"));
    ICHECK(pipe == "PIPE_MTE2" || pipe == "PIPE_V" || pipe == "PIPE_MTE3")
        << "Unsupported constraint pipe";
    flags.push_back(Integer(pipe == "PIPE_MTE2" ? 1
                            : pipe == "PIPE_V"  ? 2
                                                : 4));
  }
  std::set<std::pair<int, int>> edges;
  for (const ObjectRef &object :
       Downcast<Array<ObjectRef>>(inputs.graph.at("dependencies"))) {
    Record dependency = Downcast<Record>(object);
    int src = IntegerField(dependency, "src"),
        dst = IntegerField(dependency, "dst");
    if (local.count(src) && local.count(dst))
      edges.emplace(local.at(src), local.at(dst));
  }
  Array<Array<Integer>> dependencies;
  for (const auto &edge : edges)
    dependencies.push_back(
        {Integer(edge.first), Integer(edge.second), Integer(0), Integer(1)});
  Record problem{{"schema_version", Integer(1)}, {"mode", String("straight")},
                 {"latencies", costs},           {"iis", costs},
                 {"resource_flags", flags},      {"data_deps", dependencies}};
  Record result = (*solve)(problem);
  ICHECK_EQ(IntegerField(result, "schema_version"), 1)
      << "Malformed constraint solver schema";
  String status = Downcast<String>(result.at("status"));
  ICHECK_EQ(status, "SAT") << "Constraint solver did not produce SAT: "
                           << status << ", "
                           << result.Get("reason").value_or(String(""));
  Array<Integer> execution = Downcast<Array<Integer>>(result.at("order"));
  ICHECK_EQ(execution.size(), count)
      << "Malformed constraint solver order size";
  std::vector<int> ranks(count, -1);
  for (size_t rank = 0; rank < count; ++rank) {
    ICHECK(execution[rank].dtype().is_int())
        << "Constraint solver order must contain integer task IDs, not bool";
    int64_t unit = execution[rank]->value;
    ICHECK(unit >= 0 && unit < static_cast<int64_t>(count) && ranks[unit] < 0)
        << "Constraint solver order must be a permutation";
    ranks[unit] = rank;
  }
  for (const auto &edge : edges)
    ICHECK_LT(ranks[edge.first], ranks[edge.second])
        << "Constraint solver order reverses an original logical dependency";
  // Only order is consumed by the single-stage bridge. Numeric start times
  // remain the S2 callback's independently verified abstract scheduling model.
  for (int rank : ranks)
    candidate->order.push_back(Integer(rank));
}

class StableTaskMarker : public StmtExprMutator {
public:
  explicit StableTaskMarker(const ConstraintInputs &inputs) : inputs_(inputs) {}

private:
  Stmt VisitStmt_(const ForNode *loop) final {
    int id = next_loop_++;
    stack_.push_back({id, loop->loop_var});
    Stmt result = StmtExprMutator::VisitStmt_(loop);
    stack_.pop_back();
    return result;
  }
  Stmt VisitStmt_(const EvaluateNode *statement) final {
    auto found = inputs_.task_ids.find(statement);
    if (found == inputs_.task_ids.end())
      return GetRef<Stmt>(statement);
    Stmt body = GetRef<Stmt>(statement);
    for (auto loop = stack_.rbegin(); loop != stack_.rend(); ++loop)
      body = AttrStmt(IntImm(DataType::Int(64), loop->first), kAscendS4Iter,
                      cast(DataType::Int(64), loop->second), body);
    return AttrStmt(IntImm(DataType::Int(64), found->second), kAscendS4Task,
                    IntImm(DataType::Int(32), 1), body);
  }
  const ConstraintInputs &inputs_;
  int next_loop_{0};
  std::vector<std::pair<int, Var>> stack_;
};

class AnnotationEmitter : public StmtExprMutator {
public:
  explicit AnnotationEmitter(const ConstraintInputs &inputs) : inputs_(inputs) {
    for (const Candidate &candidate : inputs.candidates)
      candidates_.emplace(candidate.loop_id, &candidate);
  }

private:
  Stmt VisitStmt_(const ForNode *loop) final {
    int id = next_loop_++;
    For result = Downcast<For>(StmtExprMutator::VisitStmt_(loop));
    if (inputs_.empty_loops.count(id)) {
      result.CopyOnWrite()->annotations.erase("num_stages");
    } else if (candidates_.count(id)) {
      const Candidate &candidate = *candidates_.at(id);
      Map<String, ObjectRef> annotations = result->annotations;
      annotations.erase("num_stages");
      Array<Integer> stages;
      for (size_t i = 0; i < candidate.units.size(); ++i)
        stages.push_back(Integer(0));
      annotations.Set(tir::attr::software_pipeline_stage, stages);
      annotations.Set(tir::attr::software_pipeline_order, candidate.order);
      result.CopyOnWrite()->annotations = annotations;
    }
    return result;
  }
  const ConstraintInputs &inputs_;
  std::unordered_map<int, const Candidate *> candidates_;
  int next_loop_{0};
};

} // namespace

PrimFunc PlanAscendConstraintPipeline(PrimFunc function,
                                      tvm::transform::PassContext context) {
  auto target = function->GetAttr<Target>(tvm::attr::kTarget);
  auto platform = function->GetAttr<String>("npu_platform");
  ICHECK(target.defined() && platform.defined() && platform.value() == "A3")
      << "Constraint pipeline planner supports explicitly identified A3 only";
  String model = target.value()->GetAttr<String>("model", String("")).value();
  ICHECK(model == "ascendc" || model == "auto")
      << "Constraint pipeline planner requires AscendC/auto";
  ICHECK(
      context->GetConfig<Bool>("tl.ascend_auto_sync", Bool(false))
          .value()
          ->value &&
      context->GetConfig<Bool>("tl.ascend_memory_planning", Bool(false))
          .value()
          ->value &&
      !context->GetConfig<Bool>("tl.ascend_auto_cross_core_sync", Bool(false))
           .value()
           ->value &&
      !context->GetConfig<Bool>("tl.ascend_auto_sync_vs", Bool(false))
           .value()
           ->value)
      << "Constraint planner requires auto_sync=true, memory_planning=true, "
         "auto_cross_core_sync=false and auto_sync_vs=false";
  ICHECK(!function->GetAttr<ObjectRef>(kAscendS4Manifest).defined())
      << "Constraint pipeline is already planned";
  ConstraintInputs inputs(function);
  for (Candidate &candidate : inputs.candidates)
    SolveCandidate(&candidate, inputs);
  PrimFunc source = function;
  source.CopyOnWrite()->body = StableTaskMarker(inputs)(function->body);
  Array<ObjectRef> candidates;
  for (size_t i = 0; i < inputs.candidates.size(); ++i) {
    const Candidate &candidate = inputs.candidates[i];
    candidates.push_back(Record{{"id", Integer(i)},
                                {"loop_id", Integer(candidate.loop_id)},
                                {"unit_ids", candidate.units},
                                {"source_paths", candidate.paths},
                                {"order", candidate.order}});
  }
  Record manifest{{"schema_version", Integer(1)},
                  {"source_graph", inputs.graph},
                  {"source_function", source},
                  {"candidates", candidates}};
  PrimFunc planned = source;
  planned.CopyOnWrite()->body = AnnotationEmitter(inputs)(source->body);
  return WithAttr(planned, kAscendS4Manifest, manifest);
}

} // namespace tl
} // namespace tvm
