// Copyright (c) Tile-AI Corporation.
// Licensed under the MIT License.

#include <tvm/arith/analyzer.h>
#include <tvm/node/structural_equal.h>
#include <tvm/runtime/registry.h>
#include <tvm/target/target.h>
#include <tvm/tir/builtin.h>
#include <tvm/tir/stmt_functor.h>
#include <tvm/tir/transform.h>

#include <array>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include "common/ascend_constraint.h"
#include "common/ascend_resource.h"
#include "common/ascend_schedule_hints.h"

namespace tvm {
namespace tl {
namespace {
using namespace tir;
using Record = Map<String, ObjectRef>;
using Key = std::vector<int64_t>;
constexpr size_t kTaskLimit = 4096;
constexpr size_t kControlLimit = 65536;

int PipeIndex(const std::string &pipe) {
  if (pipe == "PIPE_MTE2")
    return 0;
  if (pipe == "PIPE_V")
    return 1;
  if (pipe == "PIPE_MTE3")
    return 2;
  LOG(FATAL) << "Unsupported constraint sync PIPE: " << pipe;
  return -1;
}

struct TraceEvent {
  enum Kind { kTask, kSet, kWait, kBarrier } kind;
  int task{-1}, src{-1}, dst{-1}, slot{-1};
};

// Expand the actual IR, not a reconstructed scheduling formula. Iteration
// marker values travel through the injector's Substitute and loop unrolling.
class TraceBuilder {
public:
  TraceBuilder(PrimFunc function, Array<ObjectRef> units, bool source)
      : function_(function), units_(units), source_(source) {
    Walk(function->body);
  }

  Record Analyze() {
    Stmt body = calls.empty()       ? Stmt(Evaluate(0))
                : calls.size() == 1 ? calls[0]
                                    : Stmt(SeqStmt(calls));
    body = AttrStmt(Integer(0), "resource_scope", Integer(1), body);
    for (auto it = allocations_.rbegin(); it != allocations_.rend(); ++it) {
      Allocate allocation = *it;
      allocation.CopyOnWrite()->body = body;
      body = allocation;
    }
    if (!buffers_.empty()) {
      body = BlockRealize(
          {}, Bool(true),
          Block({}, {}, {}, "tilelang_root", body, NullOpt, buffers_));
    }
    for (auto it = launches_.rbegin(); it != launches_.rend(); ++it) {
      AttrStmt launch = *it;
      launch.CopyOnWrite()->body = body;
      body = launch;
    }
    PrimFunc probe = function_;
    probe.CopyOnWrite()->body = body;
    return AnalyzeAscendScheduleEffects(probe, "A3");
  }

  std::vector<Key> keys;
  std::vector<TraceEvent> events;
  Array<Stmt> calls;

private:
  PrimExpr Resolve(const PrimExpr &expr) {
    return analyzer_.Simplify(Substitute(expr, bindings_));
  }
  int64_t IntegerValue(const PrimExpr &expr) {
    PrimExpr value = Resolve(expr);
    const auto *integer = value.as<IntImmNode>();
    ICHECK(integer && value.dtype().lanes() == 1 &&
           (value.dtype().is_int() || value.dtype().is_uint()))
        << "Constraint sync verification requires a static integer: " << value;
    ICHECK(!value.dtype().is_uint() || value.dtype().bits() < 64)
        << "Unsupported uint64 constraint sync value";
    return integer->value;
  }
  void Tick() {
    ICHECK_LT(visits_++, kControlLimit)
        << "Constraint sync control-visit limit exceeded (65536)";
  }
  void Walk(const Stmt &statement) {
    Tick();
    if (const auto *seq = statement.as<SeqStmtNode>()) {
      for (const Stmt &child : seq->seq)
        Walk(child);
    } else if (const auto *attr = statement.as<AttrStmtNode>()) {
      if (attr->attr_key == kAscendS4Task) {
        const auto *id = attr->node.as<IntImmNode>();
        ICHECK(id && id->dtype == DataType::Int(64) && id->value >= 0 &&
               size_t(id->value) < units_.size() &&
               attr->value.dtype() == DataType::Int(32) && is_one(attr->value))
            << "Malformed constraint task marker";
        ICHECK_LT(active_task_, 0) << "Nested constraint task markers";
        active_task_ = id->value;
        size_t before = calls.size();
        Walk(attr->body);
        ICHECK_EQ(calls.size(), before + 1)
            << "Constraint task marker must contain exactly one hardware call";
        active_task_ = -1;
      } else if (attr->attr_key == kAscendS4Iter) {
        const auto *id = attr->node.as<IntImmNode>();
        ICHECK(id && id->dtype == DataType::Int(64) && id->value >= 0 &&
               attr->value.dtype().is_int() && attr->value.dtype().bits() <= 64)
            << "Malformed constraint iteration marker";
        ICHECK(!iterations_.count(id->value))
            << "Duplicate constraint iteration marker";
        iterations_[id->value] = IntegerValue(attr->value);
        Walk(attr->body);
        iterations_.erase(id->value);
      } else if (attr->attr_key == "resource_scope") {
        int previous = scope_;
        scope_ = IntegerValue(attr->value);
        ICHECK(scope_ == 0 || scope_ == 1)
            << "Invalid constraint resource scope";
        Walk(attr->body);
        scope_ = previous;
      } else if (attr->attr_key == "thread_extent") {
        const auto *axis = attr->node.as<IterVarNode>();
        ICHECK(axis) << "Malformed constraint launch domain";
        int64_t extent = IntegerValue(attr->value);
        ICHECK_GT(extent, 0) << "Invalid constraint launch extent";
        Map<Var, PrimExpr> saved = bindings_;
        if (extent == 1)
          bindings_.Set(axis->var, make_zero(axis->var.dtype()));
        if (launch_vars_.insert(axis->var.get()).second)
          launches_.push_back(GetRef<AttrStmt>(attr));
        Walk(attr->body);
        bindings_ = saved;
      } else {
        LOG(FATAL) << "Unsupported final constraint attribute: "
                   << attr->attr_key;
      }
    } else if (const auto *loop = statement.as<ForNode>()) {
      ICHECK(loop->kind == ForKind::kSerial || loop->kind == ForKind::kUnrolled)
          << "Unsupported constraint sync loop kind";
      int64_t minimum = IntegerValue(loop->min);
      int64_t extent = IntegerValue(loop->extent);
      ICHECK(extent >= 0 && uint64_t(extent) <= kControlLimit)
          << "Constraint sync loop exceeds static control limit";
      __int128 end = __int128(minimum) + extent;
      ICHECK(end <= std::numeric_limits<int64_t>::max())
          << "Constraint sync loop index overflow";
      DataType dtype = loop->loop_var.dtype();
      ICHECK(dtype.is_int() && dtype.bits() <= 64)
          << "Unsupported constraint loop index dtype";
      if (dtype.bits() < 64) {
        int64_t limit = int64_t(1) << (dtype.bits() - 1);
        ICHECK(minimum >= -limit && minimum < limit && end >= -limit &&
               end < limit)
            << "Constraint loop endpoint/update cannot be represented";
      }
      Map<Var, PrimExpr> saved = bindings_;
      for (int64_t index = 0; index < extent; ++index) {
        int64_t value = minimum + index;
        if (dtype.bits() < 64) {
          int64_t limit = int64_t(1) << (dtype.bits() - 1);
          ICHECK(value >= -limit && value < limit)
              << "Constraint loop index cannot be represented";
        }
        bindings_.Set(loop->loop_var, IntImm(dtype, value));
        Walk(loop->body);
      }
      bindings_ = saved;
    } else if (const auto *let = statement.as<LetStmtNode>()) {
      PrimExpr value = Resolve(let->value);
      PostOrderVisit(value, [](const ObjectRef &node) {
        ICHECK(!node.as<BufferLoadNode>() && !node.as<CallNode>())
            << "Unsupported memory-dependent constraint scalar binding";
      });
      Map<Var, PrimExpr> saved = bindings_;
      bindings_.Set(let->var, value);
      Walk(let->body);
      bindings_ = saved;
    } else if (const auto *branch = statement.as<IfThenElseNode>()) {
      if (IntegerValue(branch->condition))
        Walk(branch->then_case);
      else if (branch->else_case.defined())
        Walk(branch->else_case.value());
    } else if (const auto *realize = statement.as<BlockRealizeNode>()) {
      ICHECK(realize->block->iter_vars.empty() &&
             realize->block->match_buffers.empty() &&
             !realize->block->init.defined())
          << "Unsupported constraint block";
      if (!IntegerValue(realize->predicate))
        return;
      for (const Buffer &buffer : realize->block->alloc_buffers) {
        if (allocation_vars_.insert(buffer->data.get()).second)
          buffers_.push_back(buffer);
      }
      Walk(realize->block->body);
    } else if (const auto *allocation = statement.as<AllocateNode>()) {
      ICHECK_EQ(IntegerValue(allocation->condition), 1)
          << "Unsupported conditional constraint allocation";
      if (allocation_vars_.insert(allocation->buffer_var.get()).second) {
        Allocate copy = GetRef<Allocate>(allocation);
        Array<PrimExpr> extents;
        for (const PrimExpr &extent : allocation->extents) {
          ICHECK_GT(IntegerValue(extent), 0) << "Invalid constraint allocation";
          extents.push_back(Resolve(extent));
        }
        copy.CopyOnWrite()->extents = extents;
        allocations_.push_back(copy);
      }
      Walk(allocation->body);
    } else if (const auto *evaluate = statement.as<EvaluateNode>()) {
      PrimExpr value = Resolve(evaluate->value);
      if (is_zero(value))
        return;
      const auto *call = value.as<CallNode>();
      ICHECK(call) << "Unsupported final constraint scalar statement";
      ICHECK_EQ(scope_, 1)
          << "Constraint sync requires explicit Vector ownership";
      if (Sync(call))
        return;
      ICHECK_GE(active_task_, 0)
          << "Unmarked hardware call in final constraint IR";
      ICHECK_LT(keys.size(), kTaskLimit)
          << "Constraint sync hardware-task instance limit exceeded (4096)";
      Record unit = Downcast<Record>(units_[active_task_]);
      auto loops = Downcast<Array<Integer>>(unit["loop_ids"]);
      ICHECK_EQ(loops.size(), iterations_.size())
          << "Constraint task iteration identity mismatch";
      Key key{active_task_};
      for (const Integer &id : loops) {
        ICHECK(iterations_.count(id->value))
            << "Missing constraint iteration identity";
        key.push_back(id->value);
        key.push_back(iterations_.at(id->value));
      }
      keys.push_back(key);
      events.push_back({TraceEvent::kTask, int(keys.size() - 1)});
      calls.push_back(Evaluate(value));
    } else {
      LOG(FATAL) << "Unsupported final constraint statement: "
                 << statement->GetTypeKey();
    }
  }

  bool Sync(const CallNode *call) {
    bool set = call->op.same_as(ascend_auto_set_flag());
    bool wait = call->op.same_as(ascend_auto_wait_flag());
    bool barrier = call->op.same_as(ascend_auto_barrier());
    if (!set && !wait && !barrier)
      return false;
    ICHECK(!source_) << "Manual synchronization in constraint source manifest";
    std::string operation;
    AscendResource resource = ResourceForCall(call, &operation);
    ICHECK(resource != AscendResource::kCube)
        << "Cube synchronization in Vector constraint kernel";
    ICHECK_EQ(call->args.size(), barrier ? 1U : 2U)
        << "Malformed automatic constraint sync ABI";
    const auto *name = call->args[0].as<StringImmNode>();
    ICHECK(name) << "Dynamic automatic constraint sync domain";
    if (barrier) {
      int pipe = name->value == "PIPE_ALL" ? -1 : PipeIndex(name->value);
      events.push_back({TraceEvent::kBarrier, -1, pipe});
    } else {
      std::string event = name->value;
      size_t separator = event.find('_');
      ICHECK(separator != std::string::npos)
          << "Malformed automatic event domain";
      std::string src = "PIPE_" + event.substr(0, separator);
      std::string dst = "PIPE_" + event.substr(separator + 1);
      auto mapping = GetEventMapping().find(src + "_" + dst);
      ICHECK(mapping != GetEventMapping().end() && mapping->second == event)
          << "Unsupported automatic event domain";
      int64_t slot = IntegerValue(call->args[1]);
      ICHECK(call->args[1].dtype() == DataType::Int(32) && slot >= 0 &&
             slot < 8)
          << "Invalid compiler event slot; expected int32 ID 0..7";
      events.push_back({set ? TraceEvent::kSet : TraceEvent::kWait, -1,
                        PipeIndex(src), PipeIndex(dst), int(slot)});
    }
    return true;
  }

  PrimFunc function_;
  Array<ObjectRef> units_;
  bool source_;
  int active_task_{-1}, scope_{-1};
  size_t visits_{0};
  arith::Analyzer analyzer_;
  Map<Var, PrimExpr> bindings_;
  std::map<int64_t, int64_t> iterations_;
  std::set<const VarNode *> allocation_vars_, launch_vars_;
  Array<Buffer> buffers_;
  std::vector<Allocate> allocations_;
  std::vector<AttrStmt> launches_;
};

struct Effect {
  Var data;
  std::string scope;
  PrimExpr begin, end;
  Optional<PrimExpr> physical_begin, physical_end;
};
struct Instance {
  std::string pipe, operation;
  Call call;
  std::vector<Effect> reads, writes;
};

class ProtocolVerifier {
public:
  explicit ProtocolVerifier(PrimFunc function) : function_(function) {}

  void Run() {
    auto attribute = function_->GetAttr<Record>(kAscendS4Manifest);
    ICHECK(attribute.defined()) << "Missing constraint source manifest";
    Record manifest = attribute.value();
    ICHECK_EQ(Downcast<Integer>(manifest["schema_version"])->value, 1)
        << "Unsupported constraint manifest version";
    Record source_graph = Downcast<Record>(manifest["source_graph"]);
    Array<ObjectRef> units = Downcast<Array<ObjectRef>>(source_graph["tasks"]);
    PrimFunc source_function = Downcast<PrimFunc>(manifest["source_function"]);
    ICHECK(!source_function->GetAttr<Record>(kAscendS4Manifest).defined())
        << "Recursive constraint source manifest";
    TraceBuilder source(source_function, units, true);
    TraceBuilder final(function_, units, false);
    Record original_graph = source.Analyze();
    Record final_graph = final.Analyze();
    auto original = Instances(original_graph, false);
    auto actual = Instances(final_graph, true);
    ICHECK_EQ(original.size(), actual.size())
        << "Constraint task coverage mismatch";
    std::map<Key, int> final_ids;
    for (size_t i = 0; i < final.keys.size(); ++i)
      ICHECK(final_ids.emplace(final.keys[i], i).second)
          << "Duplicate final constraint task instance";
    std::set<Key> source_keys;
    std::vector<int> correspondence;
    for (size_t i = 0; i < source.keys.size(); ++i) {
      ICHECK(source_keys.insert(source.keys[i]).second)
          << "Duplicate source constraint task instance";
      auto found = final_ids.find(source.keys[i]);
      ICHECK(found != final_ids.end())
          << "Missing final constraint task instance";
      correspondence.push_back(found->second);
      SameTask(original[i], actual[found->second]);
    }
    std::vector<std::set<int>> requirements(actual.size());
    bool noalias = source_function->HasNonzeroAttr("tir.noalias");
    for (size_t i = 0; i < original.size(); ++i) {
      for (size_t j = i + 1; j < original.size(); ++j) {
        if (Hazard(original[i], original[j], false, noalias))
          requirements[correspondence[j]].insert(correspondence[i]);
      }
    }
    for (size_t i = 0; i < actual.size(); ++i) {
      for (size_t j = i + 1; j < actual.size(); ++j) {
        if (Hazard(actual[i], actual[j], true, noalias))
          requirements[j].insert(i);
      }
    }
    CheckEvents(final.events, actual, requirements);
  }

private:
  std::vector<Instance> Instances(const Record &graph, bool physical) {
    auto storages = Downcast<Array<ObjectRef>>(graph["storages"]);
    std::vector<Instance> result;
    for (const ObjectRef &item : Downcast<Array<ObjectRef>>(graph["tasks"])) {
      Record task = Downcast<Record>(item);
      Instance instance;
      instance.pipe = Downcast<String>(task["pipe"]);
      instance.operation = Downcast<String>(task["operation"]);
      instance.call =
          Downcast<Call>(Downcast<Evaluate>(task["statement"])->value);
      PipeIndex(instance.pipe);
      for (bool writes : {false, true}) {
        for (const ObjectRef &entry :
             Downcast<Array<ObjectRef>>(task[writes ? "writes" : "reads"])) {
          Record access = Downcast<Record>(entry);
          int storage = Downcast<Integer>(access["storage_id"])->value;
          ICHECK(storage >= 0 && size_t(storage) < storages.size())
              << "Invalid constraint storage ID";
          Record allocation = Downcast<Record>(storages[storage]);
          Effect effect{Downcast<Var>(allocation["data"]),
                        Downcast<String>(allocation["scope"]),
                        Downcast<PrimExpr>(access["start"]),
                        Downcast<PrimExpr>(access["end"])};
          if (physical && effect.scope != "global") {
            auto interval = Physical(effect);
            effect.physical_begin = interval.first;
            effect.physical_end = interval.second;
          }
          (writes ? instance.writes : instance.reads).push_back(effect);
        }
      }
      result.push_back(instance);
    }
    return result;
  }
  bool Equal(const PrimExpr &a, const PrimExpr &b) {
    return analyzer_.CanProve(a == b);
  }
  void SamePayload(const Call &source, const Call &final) {
    ICHECK(source->op.same_as(final->op) &&
           source->args.size() == final->args.size())
        << "Constraint task payload operation/arity changed";
    for (size_t i = 0; i < source->args.size(); ++i) {
      const PrimExpr &a = source->args[i], &b = final->args[i];
      const auto *left = a.as<CallNode>();
      const auto *right = b.as<CallNode>();
      bool pointer_a = left && left->op.same_as(builtin::tvm_access_ptr());
      bool pointer_b = right && right->op.same_as(builtin::tvm_access_ptr());
      ICHECK_EQ(pointer_a, pointer_b)
          << "Constraint task pointer payload changed";
      if (pointer_a) {
        ICHECK(left->args[0].dtype() == right->args[0].dtype() &&
               Equal(left->args[4], right->args[4]))
            << "Constraint task pointer dtype/access payload changed";
      } else if ((a.dtype().is_int() || a.dtype().is_uint()) &&
                 (b.dtype().is_int() || b.dtype().is_uint())) {
        ICHECK(Equal(cast(DataType::Int(64), a), cast(DataType::Int(64), b)))
            << "Constraint task scalar/count payload changed";
      } else {
        ICHECK(StructuralEqual()(a, b) ||
               (a.dtype() == b.dtype() && a.dtype().is_float() && Equal(a, b)))
            << "Constraint task scalar/string payload changed";
      }
    }
  }
  void SameTask(const Instance &source, const Instance &final) {
    ICHECK(source.pipe == final.pipe && source.operation == final.operation)
        << "Constraint task operation/PIPE identity changed";
    SamePayload(source.call, final.call);
    for (bool writes : {false, true}) {
      const auto &left = writes ? source.writes : source.reads;
      const auto &right = writes ? final.writes : final.reads;
      ICHECK_EQ(left.size(), right.size())
          << "Constraint task effect count changed";
      for (size_t i = 0; i < left.size(); ++i) {
        ICHECK(left[i].data.same_as(right[i].data) &&
               left[i].scope == right[i].scope &&
               Equal(left[i].begin, right[i].begin) &&
               Equal(left[i].end, right[i].end))
            << "Unsupported constraint task logical effect remapping";
      }
    }
  }
  std::pair<PrimExpr, PrimExpr> Physical(const Effect &effect) {
    ICHECK_EQ(effect.scope, "shared.ub")
        << "Constraint sync supports only physical UB storage";
    auto addresses = function_->GetAttr<Map<Var, PrimExpr>>("address_map");
    auto sizes = function_->GetAttr<Map<Var, PrimExpr>>("size_map");
    ICHECK(addresses.defined() && sizes.defined() &&
           addresses.value().count(effect.data) &&
           sizes.value().count(effect.data))
        << "Missing final constraint physical allocation metadata";
    const auto *address = addresses.value()[effect.data].as<IntImmNode>();
    const auto *size = sizes.value()[effect.data].as<IntImmNode>();
    ICHECK(address && size && address->value >= 0 && size->value > 0 &&
           address->value % 32 == 0 && size->value % 32 == 0 &&
           __int128(address->value) + size->value <= kAscendA2A3UBBudget)
        << "Invalid final constraint physical allocation interval";
    ICHECK(analyzer_.CanProve(effect.begin >= 0 && effect.end >= effect.begin &&
                              effect.end <=
                                  IntImm(DataType::Int(64), size->value)))
        << "Unproven final constraint physical access bounds";
    PrimExpr base = IntImm(DataType::Int(64), address->value);
    return {analyzer_.Simplify(base + effect.begin),
            analyzer_.Simplify(base + effect.end)};
  }
  bool Overlap(const Effect &a, const Effect &b, bool physical, bool noalias) {
    if (a.scope != b.scope)
      return false;
    PrimExpr begin_a = a.begin, end_a = a.end, begin_b = b.begin, end_b = b.end;
    if (physical) {
      if (a.scope == "global")
        return false;
      begin_a = a.physical_begin.value();
      end_a = a.physical_end.value();
      begin_b = b.physical_begin.value();
      end_b = b.physical_end.value();
    } else if (!a.data.same_as(b.data)) {
      return a.scope == "global" && !noalias;
    }
    return !analyzer_.CanProve(end_a <= begin_b || end_b <= begin_a);
  }
  bool Hazard(const Instance &a, const Instance &b, bool physical,
              bool noalias) {
    auto overlap = [&](const std::vector<Effect> &left,
                       const std::vector<Effect> &right) {
      for (const Effect &x : left)
        for (const Effect &y : right)
          if (Overlap(x, y, physical, noalias))
            return true;
      return false;
    };
    return overlap(a.writes, b.reads) || overlap(a.reads, b.writes) ||
           overlap(a.writes, b.writes);
  }
  void CheckEvents(const std::vector<TraceEvent> &events,
                   const std::vector<Instance> &tasks,
                   const std::vector<std::set<int>> &requirements) {
    std::array<std::set<int>, 3> issued, completed;
    std::map<std::array<int, 3>, std::set<int>> pending;
    for (const TraceEvent &event : events) {
      if (event.kind == TraceEvent::kTask) {
        int pipe = PipeIndex(tasks[event.task].pipe);
        for (int producer : requirements[event.task]) {
          ICHECK(completed[pipe].count(producer))
              << "Uncovered constraint logical/physical RAW/WAR/WAW dependency "
                 "from instance "
              << producer << " to " << event.task;
        }
        issued[pipe].insert(event.task);
      } else if (event.kind == TraceEvent::kSet) {
        std::array<int, 3> key{event.src, event.dst, event.slot};
        ICHECK(!pending.count(key))
            << "Duplicate set on active constraint event slot";
        std::set<int> snapshot = completed[event.src];
        snapshot.insert(issued[event.src].begin(), issued[event.src].end());
        pending.emplace(key, snapshot);
      } else if (event.kind == TraceEvent::kWait) {
        std::array<int, 3> key{event.src, event.dst, event.slot};
        auto token = pending.find(key);
        ICHECK(token != pending.end())
            << "Constraint wait without matching earlier set";
        completed[event.dst].insert(token->second.begin(), token->second.end());
        pending.erase(token);
      } else if (event.src < 0) {
        std::set<int> snapshot;
        for (size_t pipe = 0; pipe < issued.size(); ++pipe) {
          snapshot.insert(issued[pipe].begin(), issued[pipe].end());
          snapshot.insert(completed[pipe].begin(), completed[pipe].end());
        }
        for (auto &completion : completed)
          completion.insert(snapshot.begin(), snapshot.end());
      } else {
        completed[event.src].insert(issued[event.src].begin(),
                                    issued[event.src].end());
      }
    }
    ICHECK(pending.empty())
        << "Undrained constraint event slots at kernel exit";
  }

  PrimFunc function_;
  arith::Analyzer analyzer_;
};
} // namespace

tvm::transform::Pass AscendVerifyConstraintSync(Target target,
                                                String platform) {
  auto pass = [target, platform](PrimFunc function, IRModule,
                                 tvm::transform::PassContext ctx) {
    String strategy =
        ctx->GetConfig<String>(kAscendPipelinePlanner, String("legacy"))
            .value();
    if (strategy != "constraint")
      return function;
    ICHECK(platform == "A3" &&
           (target->GetAttr<String>("model").value_or("") == "ascendc" ||
            target->GetAttr<String>("model").value_or("") == "auto"))
        << "Constraint sync verification requires A3 AscendC/auto";
    ProtocolVerifier(function).Run();
    return function;
  };
  return tir::transform::CreatePrimFuncPass(
      pass, 0, "tl.AscendVerifyConstraintSync", {});
}

TVM_REGISTER_GLOBAL("tl.transform.AscendVerifyConstraintSync")
    .set_body_typed(AscendVerifyConstraintSync);
} // namespace tl
} // namespace tvm
