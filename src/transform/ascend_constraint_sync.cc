// Copyright (c) Tile-AI Corporation.
// Licensed under the MIT License.

#include <tvm/arith/analyzer.h>
#include <tvm/node/structural_equal.h>
#include <tvm/runtime/registry.h>
#include <tvm/target/target.h>
#include <tvm/tir/builtin.h>
#include <tvm/tir/stmt_functor.h>
#include <tvm/tir/transform.h>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include "common/ascend_constraint.h"
#include "common/ascend_lifecycle.h"
#include "common/ascend_resource.h"
#include "common/ascend_schedule_hints.h"

namespace tvm {
namespace tl {
namespace {
using namespace tir;
using Record = Map<String, ObjectRef>;
using Key = std::vector<int64_t>;
using LaunchCase = std::map<std::string, std::pair<int64_t, int64_t>>;
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
  TraceBuilder(PrimFunc function, Array<ObjectRef> units, bool source,
               const LaunchCase &launch_case = {}, bool strict_launch = false,
               Map<Var, PrimExpr> scalar_bindings = {},
               bool checked_arithmetic = false)
      : function_(function), units_(units), source_(source),
        launch_case_(launch_case), strict_launch_(strict_launch),
        bindings_(scalar_bindings), checked_arithmetic_(checked_arithmetic) {
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
    if (checked_arithmetic_)
      return EvaluateAscendLifetimeInteger(expr, bindings_);
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
        auto coordinate = launch_case_.find(axis->thread_tag);
        ICHECK(!strict_launch_ || coordinate != launch_case_.end())
            << "Unmapped S5 physical launch domain";
        if (coordinate != launch_case_.end()) {
          ICHECK(coordinate->second.second == extent &&
                 coordinate->second.first >= 0 &&
                 coordinate->second.first < extent)
              << "S5 launch domain changed after planning";
          bindings_.Set(axis->var,
                        IntImm(axis->var.dtype(), coordinate->second.first));
        } else if (extent == 1)
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
      PrimExpr value;
      if (checked_arithmetic_ &&
          (let->var.dtype().is_int() || let->var.dtype().is_uint())) {
        int64_t integer = EvaluateAscendLifetimeInteger(
            Cast(let->var.dtype(), let->value), bindings_);
        value = IntImm(let->var.dtype(), integer);
      } else {
        value = Resolve(let->value);
      }
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
        if (checked_arithmetic_) {
          int64_t bytes = buffer->dtype.bytes();
          for (const PrimExpr &extent : buffer->shape) {
            int64_t count = IntegerValue(extent);
            ICHECK(count > 0 &&
                   bytes <= std::numeric_limits<int64_t>::max() / count)
                << "Lifetime final allocation extent overflow";
            bytes *= count;
          }
        }
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
        int64_t bytes = allocation->dtype.bytes();
        for (const PrimExpr &extent : allocation->extents) {
          int64_t count = IntegerValue(extent);
          ICHECK_GT(count, 0) << "Invalid constraint allocation";
          if (checked_arithmetic_) {
            ICHECK(bytes <= std::numeric_limits<int64_t>::max() / count)
                << "Lifetime final allocation extent overflow";
            bytes *= count;
          }
          extents.push_back(Resolve(extent));
        }
        copy.CopyOnWrite()->extents = extents;
        allocations_.push_back(copy);
      }
      Walk(allocation->body);
    } else if (const auto *evaluate = statement.as<EvaluateNode>()) {
      if (checked_arithmetic_ && evaluate->value.as<CallNode>())
        ValidateAscendLifetimeCallIndices(Downcast<Call>(evaluate->value),
                                          bindings_);
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
  LaunchCase launch_case_;
  const bool strict_launch_;
  int active_task_{-1}, scope_{-1};
  size_t visits_{0};
  arith::Analyzer analyzer_;
  Map<Var, PrimExpr> bindings_;
  std::map<int64_t, int64_t> iterations_;
  std::set<const VarNode *> allocation_vars_, launch_vars_;
  Array<Buffer> buffers_;
  std::vector<Allocate> allocations_;
  std::vector<AttrStmt> launches_;
  const bool checked_arithmetic_;
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

struct VersionRemap {
  Var logical, physical;
  int64_t loop, versions, stride, span, total, origin, logical_begin,
      logical_end;
};

class ProtocolVerifier {
public:
  explicit ProtocolVerifier(PrimFunc function) : function_(function) {}

  void Run() {
    auto attribute = function_->GetAttr<Record>(kAscendS4Manifest);
    ICHECK(attribute.defined()) << "Missing constraint source manifest";
    Record manifest = attribute.value();
    int64_t schema = Downcast<Integer>(manifest["schema_version"])->value;
    ICHECK(schema == 1 || schema == 2 || schema == 3)
        << "Unsupported constraint manifest version";
    versions_enabled_ = schema >= 2;
    lifetimes_enabled_ = schema == 3;
    Record source_graph = Downcast<Record>(manifest["source_graph"]);
    PrimFunc source_function = Downcast<PrimFunc>(manifest["source_function"]);
    ICHECK(!source_function->GetAttr<Record>(kAscendS4Manifest).defined())
        << "Recursive constraint source manifest";
    if (lifetimes_enabled_) {
      ICHECK(manifest.count("lifetime_contract"))
          << "Missing source lifetime contract";
      // Source syntax is the semantic reference. Do not trust a serialized
      // PASS bit or previously computed producer/coverage records.
      lifetime_contract_ = RebuildAscendLifetimeContract(source_function);
      Record stored = Downcast<Record>(manifest["lifetime_contract"]);
      ICHECK_EQ(StaticInteger(stored.at("schema_version"), "lifetime schema"),
                1)
          << "Unsupported source lifetime contract version";
      CheckLifetimeCatalog(
          Downcast<Record>(stored.at("storage_catalog")),
          Downcast<Record>(lifetime_contract_.at("storage_catalog")));
      source_graph = Downcast<Record>(lifetime_contract_["source_graph"]);
      source_function =
          Downcast<PrimFunc>(lifetime_contract_["source_function"]);
    }
    if (versions_enabled_)
      LoadVersionRemaps(source_graph);
    Array<ObjectRef> units = Downcast<Array<ObjectRef>>(source_graph["tasks"]);
    if (lifetimes_enabled_) {
      for (const ObjectRef &item :
           Downcast<Array<ObjectRef>>(lifetime_contract_["cases"])) {
        Record selection = Downcast<Record>(item);
        LaunchCase launch;
        for (const auto &entry : Downcast<Record>(selection["launches"])) {
          Record domain = Downcast<Record>(entry.second);
          launch[entry.first] = {
              StaticInteger(domain["coordinate"], "launch coordinate"),
              StaticInteger(domain["extent"], "launch extent")};
        }
        CheckCase(source_function, units, launch, selection);
      }
      return;
    }
    std::vector<LaunchCase> cases{{}};
    if (versions_enabled_) {
      std::map<std::string, int64_t> domains;
      for (const ObjectRef &item :
           Downcast<Array<ObjectRef>>(source_graph["launch_domains"])) {
        Record domain = Downcast<Record>(item);
        IterVar axis = Downcast<IterVar>(domain["iter_var"]);
        int64_t extent = StaticInteger(domain["extent"], "launch extent");
        std::string tag = axis->thread_tag;
        ICHECK(!tag.empty() && extent > 0) << "Invalid S5 static launch domain";
        auto previous = domains.find(tag);
        if (previous != domains.end()) {
          ICHECK_EQ(previous->second, extent)
              << "Conflicting S5 launch domains";
          continue;
        }
        ICHECK(extent <= int64_t(kTaskLimit / cases.size()))
            << "S5 launch-case verification limit exceeded (4096)";
        domains[tag] = extent;
        std::vector<LaunchCase> expanded;
        for (const LaunchCase &binding : cases) {
          for (int64_t index = 0; index < extent; ++index) {
            LaunchCase copy = binding;
            copy[tag] = {index, extent};
            expanded.push_back(copy);
          }
        }
        cases = std::move(expanded);
      }
    }
    for (const LaunchCase &launch : cases)
      CheckCase(source_function, units, launch);
  }

private:
  bool LifetimeMetadataEqual(const ObjectRef &left, const ObjectRef &right) {
    if (left.as<VarNode>() || right.as<VarNode>())
      return left.as<VarNode>() && right.as<VarNode>() && left.same_as(right);
    if (left.as<PrimExprNode>() && right.as<PrimExprNode>()) {
      PrimExpr a = Downcast<PrimExpr>(left), b = Downcast<PrimExpr>(right);
      if ((a.dtype().is_int() || a.dtype().is_uint()) &&
          (b.dtype().is_int() || b.dtype().is_uint())) {
        if (a.dtype().lanes() != 1 || b.dtype().lanes() != 1 ||
            (a.dtype().is_uint() && a.dtype().bits() >= 64) ||
            (b.dtype().is_uint() && b.dtype().bits() >= 64))
          return false;
        return Equal(cast(DataType::Int(64), a), cast(DataType::Int(64), b));
      }
    }
    if (left.as<runtime::ArrayNode>() && right.as<runtime::ArrayNode>()) {
      auto a = Downcast<Array<ObjectRef>>(left),
           b = Downcast<Array<ObjectRef>>(right);
      if (a.size() != b.size())
        return false;
      for (size_t i = 0; i < a.size(); ++i)
        if (!LifetimeMetadataEqual(a[i], b[i]))
          return false;
      return true;
    }
    return StructuralEqual()(left, right);
  }
  void CheckLifetimeCatalog(const Record &stored, const Record &rebuilt) {
    ICHECK_EQ(StaticInteger(stored.at("schema_version"), "catalog schema"), 1)
        << "Unsupported lifetime catalog version";
    auto compare = [&](const char *section,
                       const std::vector<const char *> &fields) {
      auto left = Downcast<Array<ObjectRef>>(stored.at(section)),
           right = Downcast<Array<ObjectRef>>(rebuilt.at(section));
      ICHECK_EQ(left.size(), right.size())
          << "Lifetime catalog owner/view count differs from source";
      std::vector<bool> matched(right.size(), false);
      for (const ObjectRef &object : left) {
        Record a = Downcast<Record>(object);
        bool found = false;
        for (size_t i = 0; i < right.size() && !found; ++i) {
          if (matched[i])
            continue;
          Record b = Downcast<Record>(right[i]);
          bool same = true;
          for (const char *field : fields) {
            if (a.count(field) != b.count(field) ||
                (a.count(field) &&
                 !LifetimeMetadataEqual(a.at(field), b.at(field)))) {
              same = false;
              break;
            }
          }
          if (same) {
            found = true;
            matched[i] = true;
          }
        }
        ICHECK(found) << "Lifetime catalog owner/storage/view metadata differs "
                         "from source";
      }
    };
    compare("storages", {"id", "data", "source_data", "scope", "capacity_bytes",
                         "physical_allocation", "allocation_site_id",
                         "parent_loop_ids", "origin", "explicit_address"});
    compare("aliases",
            {"data", "root", "storage_id", "byte_offset", "kind",
             "binding_site_id", "parent_loop_ids", "parent_loop_vars"});
    compare("views", {"storage_id", "data", "dtype", "shape", "strides",
                      "elem_offset", "byte_begin", "byte_end"});
  }
  void CheckCase(const PrimFunc &source_function, const Array<ObjectRef> &units,
                 const LaunchCase &launch, Record lifetime_case = {}) {
    Map<Var, PrimExpr> source_scalars, final_scalars;
    if (lifetimes_enabled_) {
      source_scalars =
          Downcast<Map<Var, PrimExpr>>(lifetime_case["bool_bindings"]);
      ICHECK_EQ(source_function->params.size(), function_->params.size())
          << "Lifetime parameter ABI changed";
      for (size_t i = 0; i < source_function->params.size(); ++i) {
        Var parameter = source_function->params[i];
        if (!source_scalars.count(parameter))
          continue;
        ICHECK(function_->params[i].dtype().is_bool())
            << "Lifetime boolean parameter ABI changed";
        final_scalars.Set(function_->params[i], source_scalars[parameter]);
      }
    }
    TraceBuilder source(source_function, units, true, launch, versions_enabled_,
                        source_scalars, lifetimes_enabled_);
    TraceBuilder final(function_, units, false, launch, versions_enabled_,
                       final_scalars, lifetimes_enabled_);
    Record original_graph = source.Analyze();
    Record final_graph = final.Analyze();
    auto original = Instances(original_graph, false);
    auto actual = Instances(final_graph, true);
    if (lifetimes_enabled_) {
      for (const Instance &task : actual)
        for (const Effect &read : task.reads)
          for (const Effect &write : task.writes) {
            if (read.scope != "shared.ub" || write.scope != "shared.ub")
              continue;
            ICHECK(
                !Overlap(read, write, true, false) ||
                (Equal(read.physical_begin.value(),
                       write.physical_begin.value()) &&
                 Equal(read.physical_end.value(), write.physical_end.value())))
                << "Unsupported same-task shifted/partial physical read-write "
                   "overlap";
          }
    }
    if (versions_enabled_)
      CheckActualVersionAllocations(final_graph, actual);
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
      SameTask(original[i], actual[found->second], source.keys[i]);
    }
    std::vector<std::set<int>> requirements(actual.size());
    bool noalias = source_function->HasNonzeroAttr("tir.noalias");
    for (size_t i = 0; i < original.size(); ++i) {
      for (size_t j = i + 1; j < original.size(); ++j) {
        if (Hazard(original[i], original[j], false, noalias, versions_enabled_))
          requirements[correspondence[j]].insert(correspondence[i]);
      }
    }
    for (size_t i = 0; i < actual.size(); ++i) {
      for (size_t j = i + 1; j < actual.size(); ++j) {
        if (Hazard(actual[i], actual[j], true, noalias))
          requirements[j].insert(i);
      }
    }
    if (lifetimes_enabled_)
      CheckLifetimeReaders(original, actual, source.keys, correspondence,
                           lifetime_case, &requirements);
    else if (versions_enabled_)
      CheckReachingWriters(original, actual, source.keys, correspondence,
                           &requirements);
    CheckEvents(final.events, actual, requirements);
  }

  int64_t StaticInteger(const ObjectRef &object, const char *field) {
    const auto *value = object.as<IntImmNode>();
    ICHECK(value && value->dtype.is_int() && value->dtype.bits() <= 64)
        << "Non-static S5 version metadata: " << field;
    return value->value;
  }
  int64_t ShapeBytes(const ObjectRef &object, int bytes, const char *field,
                     bool logical) {
    Array<PrimExpr> shape = Downcast<Array<PrimExpr>>(object);
    ICHECK(!shape.empty()) << "Empty S5 version shape: " << field;
    int64_t product = bytes;
    for (size_t i = 0; i < shape.size(); ++i) {
      int64_t extent = StaticInteger(shape[i], field);
      ICHECK(extent > 0 && extent <= kAscendA2A3UBBudget / product)
          << "S5 version shape exceeds the native UB budget";
      if (logical && i + 1 < shape.size())
        ICHECK_EQ(extent, 1)
            << "S5 versioned UB must be one contiguous logical row";
      product *= extent;
    }
    return product;
  }
  void LoadVersionRemaps(const Record &source_graph) {
    auto attribute = function_->GetAttr<Array<ObjectRef>>(kAscendS5BufferRemap);
    ICHECK(attribute.defined()) << "Missing actual S5 buffer remap metadata";
    for (const ObjectRef &item : attribute.value()) {
      Record record = Downcast<Record>(item);
      auto number = [&](const char *field) {
        return StaticInteger(record[field], field);
      };
      Var logical = Downcast<Var>(record["logical_data"]);
      Var physical = Downcast<Var>(record["physical_data"]);
      const auto *pointer = logical->type_annotation.as<PointerTypeNode>();
      const auto *element =
          pointer ? pointer->element_type.as<PrimTypeNode>() : nullptr;
      ICHECK(element && (element->dtype == DataType::Float(16) ||
                         element->dtype == DataType::Float(32)))
          << "S5 version remap requires float16/float32 UB storage";
      int64_t count = number("versions"),
              stride = number("version_stride_bytes"),
              span = number("version_span_bytes"),
              total = number("total_bytes");
      ICHECK(count >= 1 && count <= 3 && stride > 0 && span > 0 &&
             span <= stride && stride % 32 == 0 && total > 0 &&
             total <= kAscendA2A3UBBudget && __int128(count) * stride == total)
          << "Invalid S5 physical version geometry";
      int64_t logical_bytes =
          ShapeBytes(record["logical_shape"], element->dtype.bytes(),
                     "logical_shape", true);
      int64_t physical_bytes =
          ShapeBytes(record["physical_shape"], element->dtype.bytes(),
                     "physical_shape", false);
      ICHECK_EQ(physical_bytes, total) << "S5 physical shape/total mismatch";
      int64_t offset = number("logical_elem_offset");
      ICHECK(offset >= 0 &&
             __int128(offset) * element->dtype.bytes() + logical_bytes <= span)
          << "S5 logical elem_offset/region exceeds one physical version";
      int64_t loop = number("loop_id"), origin = number("selector_origin");
      auto loops = Downcast<Array<ObjectRef>>(source_graph["loops"]);
      ICHECK(loop >= 0 && size_t(loop) < loops.size())
          << "Invalid S5 remap loop identity";
      Record owner = Downcast<Record>(loops[loop]);
      ICHECK_EQ(StaticInteger(owner["min"], "source loop minimum"), origin)
          << "S5 version selector origin differs from its source loop";
      for (const VersionRemap &old : remaps_)
        ICHECK(!old.logical.same_as(logical) ||
               (old.loop != loop && old.versions == 1 && count == 1))
            << "Unsupported overlapping/nested S5 version ownership";
      int64_t begin = offset * element->dtype.bytes();
      remaps_.push_back({logical, physical, loop, count, stride, span, total,
                         origin, begin, begin + logical_bytes});
    }
  }
  void CheckActualVersionAllocations(const Record &graph,
                                     const std::vector<Instance> &actual) {
    auto storages = Downcast<Array<ObjectRef>>(graph["storages"]);
    for (const VersionRemap &remap : remaps_) {
      bool used = false;
      for (const Instance &task : actual)
        for (const auto *effects : {&task.reads, &task.writes})
          for (const Effect &effect : *effects)
            used |= effect.data.same_as(remap.physical);
      if (!used)
        continue; // Zero-trip allocations may be legally eliminated.
      bool found = false;
      for (const ObjectRef &item : storages) {
        Record storage = Downcast<Record>(item);
        if (!Downcast<Var>(storage["data"]).same_as(remap.physical))
          continue;
        ICHECK(Downcast<Bool>(storage["physical_allocation"])->value &&
               Downcast<String>(storage["scope"]) == "shared.ub" &&
               Equal(Downcast<PrimExpr>(storage["capacity_bytes"]),
                     IntImm(DataType::Int(64), remap.total)))
            << "Actual allocation shape does not match the S5 version remap";
        found = true;
      }
      ICHECK(found) << "Missing actual S5 version allocation";
    }
  }
  bool Iteration(const Key &key, int64_t loop, int64_t *value) const {
    for (size_t i = 1; i + 1 < key.size(); i += 2) {
      if (key[i] == loop) {
        *value = key[i + 1];
        return true;
      }
    }
    return false;
  }
  const VersionRemap *Remap(const Effect &effect, const Key &key) const {
    const VersionRemap *found = nullptr;
    bool versioned = false;
    for (const VersionRemap &remap : remaps_) {
      if (!remap.logical.same_as(effect.data))
        continue;
      versioned |= remap.versions > 1;
      int64_t iteration;
      if (Iteration(key, remap.loop, &iteration)) {
        ICHECK(!found) << "Ambiguous S5 version remap";
        found = &remap;
      }
    }
    ICHECK(found || !versioned)
        << "Versioned UB used outside its owning pipeline";
    return found;
  }
  bool Renamed(const Effect &effect) const {
    for (const VersionRemap &remap : remaps_)
      if (remap.versions > 1 && remap.logical.same_as(effect.data))
        return true;
    return false;
  }
  Key LifetimeKey(const ObjectRef &object) {
    Key result;
    for (Integer value : Downcast<Array<Integer>>(object))
      result.push_back(value->value);
    return result;
  }
  void CheckLifetimeReaders(const std::vector<Instance> &source,
                            const std::vector<Instance> &final,
                            const std::vector<Key> &keys,
                            const std::vector<int> &correspondence,
                            const Record &selection,
                            std::vector<std::set<int>> *requirements) {
    std::map<Key, int> source_ids;
    for (size_t i = 0; i < keys.size(); ++i)
      ICHECK(source_ids.emplace(keys[i], i).second)
          << "Duplicate lifetime source task";
    auto expected_keys = Downcast<Array<ObjectRef>>(selection.at("task_keys"));
    ICHECK_EQ(expected_keys.size(), keys.size())
        << "Rebuilt lifetime task coverage changed";
    for (size_t i = 0; i < keys.size(); ++i)
      ICHECK(LifetimeKey(expected_keys[i]) == keys[i])
          << "Rebuilt lifetime task identity changed";
    auto storages = Downcast<Array<ObjectRef>>(
        Downcast<Record>(lifetime_contract_.at("storage_catalog"))
            .at("storages"));
    for (const ObjectRef &object :
         Downcast<Array<ObjectRef>>(selection.at("reads"))) {
      Record record = Downcast<Record>(object);
      Key consumer_key = LifetimeKey(record.at("consumer_key"));
      auto consumer = source_ids.find(consumer_key);
      ICHECK(consumer != source_ids.end()) << "Missing lifetime consumer";
      int source_consumer = consumer->second,
          final_consumer = correspondence[source_consumer];
      int64_t operand = StaticInteger(record.at("read_index"), "read index");
      ICHECK(operand >= 0 &&
             size_t(operand) < source[source_consumer].reads.size())
          << "Invalid lifetime read operand";
      const Effect &logical_read = source[source_consumer].reads[operand];
      const Effect &physical_read = final[final_consumer].reads[operand];
      ICHECK_EQ(physical_read.scope, "shared.ub")
          << "Internal GM workspace lifetime materialization is unsupported";
      int64_t storage = StaticInteger(record.at("storage_id"), "storage id");
      ICHECK(storage >= 0 && size_t(storage) < storages.size());
      Record allocation = Downcast<Record>(storages[storage]);
      ICHECK(
          Downcast<Var>(allocation.at("data")).same_as(logical_read.data) &&
          Equal(logical_read.begin, Downcast<PrimExpr>(record.at("begin"))) &&
          Equal(logical_read.end, Downcast<PrimExpr>(record.at("end"))))
          << "Lifetime logical read footprint changed";
      int64_t logical_begin = StaticInteger(
          analyzer_.Simplify(logical_read.begin), "logical read begin");
      int64_t physical_begin = StaticInteger(
          analyzer_.Simplify(physical_read.physical_begin.value()),
          "physical read begin");
      for (const ObjectRef &item :
           Downcast<Array<ObjectRef>>(record.at("segments"))) {
        Record segment = Downcast<Record>(item);
        auto producer =
            source_ids.find(LifetimeKey(segment.at("producer_key")));
        ICHECK(producer != source_ids.end()) << "Missing lifetime producer";
        int source_producer = producer->second,
            final_producer = correspondence[source_producer];
        int64_t write_index =
            StaticInteger(segment.at("write_index"), "write index");
        ICHECK(write_index >= 0 &&
               size_t(write_index) < source[source_producer].writes.size())
            << "Invalid lifetime producer operand";
        const Effect &logical_write =
            source[source_producer].writes[write_index];
        int64_t begin = StaticInteger(segment.at("begin"), "segment begin"),
                end = StaticInteger(segment.at("end"), "segment end");
        ICHECK(begin < end && logical_write.data.same_as(logical_read.data) &&
               analyzer_.CanProve(logical_write.begin <= Integer(begin) &&
                                  Integer(end) <= logical_write.end &&
                                  logical_read.begin <= Integer(begin) &&
                                  Integer(end) <= logical_read.end))
            << "Invalid complete lifetime read definition";
        (*requirements)[final_consumer].insert(final_producer);
        // Inspect the nearest writer separately for every byte segment. A
        // completed write from another logical owner still changes the value.
        std::vector<std::pair<int64_t, int64_t>> missing{
            {physical_begin + begin - logical_begin,
             physical_begin + end - logical_begin}};
        for (int previous = final_consumer - 1;
             previous >= 0 && !missing.empty(); --previous) {
          for (size_t index = 0; index < final[previous].writes.size();
               ++index) {
            const Effect &write = final[previous].writes[index];
            if (write.scope != "shared.ub")
              continue;
            int64_t write_begin = StaticInteger(
                        analyzer_.Simplify(write.physical_begin.value()),
                        "physical writer begin"),
                    write_end = StaticInteger(
                        analyzer_.Simplify(write.physical_end.value()),
                        "physical writer end");
            std::vector<std::pair<int64_t, int64_t>> remaining;
            for (auto interval : missing) {
              int64_t left = std::max(interval.first, write_begin),
                      right = std::min(interval.second, write_end);
              if (left >= right) {
                remaining.push_back(interval);
                continue;
              }
              ICHECK(previous == final_producer &&
                     int64_t(index) == write_index)
                  << "Lifetime read observes the wrong reaching writer or "
                     "overwritten physical owner";
              if (interval.first < left)
                remaining.push_back({interval.first, left});
              if (right < interval.second)
                remaining.push_back({right, interval.second});
            }
            missing = std::move(remaining);
          }
        }
        ICHECK(missing.empty())
            << "Lifetime read has no initialized physical producer";
      }
    }
  }
  void CheckReachingWriters(const std::vector<Instance> &source,
                            const std::vector<Instance> &final,
                            const std::vector<Key> &keys,
                            const std::vector<int> &correspondence,
                            std::vector<std::set<int>> *requirements) {
    std::map<const VarNode *, int> writer;
    for (size_t index = 0; index < source.size(); ++index) {
      for (size_t operand = 0; operand < source[index].reads.size();
           ++operand) {
        const Effect &read = source[index].reads[operand];
        const VersionRemap *remap = Remap(read, keys[index]);
        if (!remap)
          continue;
        ICHECK(Equal(read.begin, Integer(remap->logical_begin)) &&
               Equal(read.end, Integer(remap->logical_end)))
            << "S5 versioned reads must cover one fixed whole logical region";
        auto definition = writer.find(read.data.get());
        ICHECK(definition != writer.end())
            << "Versioned UB read has no source full writer";
        const Key &current = keys[index], &producer = keys[definition->second];
        ICHECK(current.size() == producer.size() &&
               std::equal(current.begin() + 1, current.end(),
                          producer.begin() + 1))
            << "Loop-carried versioned UB accumulators require a later "
               "ownership model";
        int consumer_id = correspondence[index],
            producer_id = correspondence[definition->second];
        (*requirements)[consumer_id].insert(producer_id);
        const Effect &physical_read = final[consumer_id].reads[operand];
        bool matched = false;
        for (int previous = consumer_id - 1; previous >= 0 && !matched;
             --previous) {
          for (const Effect &write : final[previous].writes) {
            if (!Overlap(write, physical_read, true, false))
              continue;
            ICHECK(
                previous == producer_id &&
                analyzer_.CanProve(write.physical_begin.value() <=
                                       physical_read.physical_begin.value() &&
                                   physical_read.physical_end.value() <=
                                       write.physical_end.value()))
                << "S5 versioned read observes the wrong reaching writer or "
                   "overwritten slot";
            matched = true;
            break;
          }
        }
        ICHECK(matched)
            << "S5 versioned read observes an uninitialized physical slot";
      }
      for (const Effect &write : source[index].writes) {
        const VersionRemap *remap = Remap(write, keys[index]);
        if (!remap)
          continue;
        ICHECK(Equal(write.begin, Integer(remap->logical_begin)) &&
               Equal(write.end, Integer(remap->logical_end)))
            << "S5 versioned writes must cover one fixed whole logical region";
        writer[write.data.get()] = index;
      }
    }
  }
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
  void SameTask(const Instance &source, const Instance &final, const Key &key) {
    ICHECK(source.pipe == final.pipe && source.operation == final.operation)
        << "Constraint task operation/PIPE identity changed";
    SamePayload(source.call, final.call);
    for (bool writes : {false, true}) {
      const auto &left = writes ? source.writes : source.reads;
      const auto &right = writes ? final.writes : final.reads;
      ICHECK_EQ(left.size(), right.size())
          << "Constraint task effect count changed";
      for (size_t i = 0; i < left.size(); ++i) {
        const VersionRemap *remap =
            versions_enabled_ && left[i].scope == "shared.ub"
                ? Remap(left[i], key)
                : nullptr;
        PrimExpr shift = IntImm(DataType::Int(64), 0);
        Var expected_data = left[i].data;
        if (remap) {
          int64_t iteration;
          ICHECK(Iteration(key, remap->loop, &iteration))
              << "Missing S5 iteration identity";
          __int128 normalized = __int128(iteration) - remap->origin;
          int64_t slot =
              int64_t((normalized % remap->versions + remap->versions) %
                      remap->versions);
          shift = IntImm(DataType::Int(64), slot * remap->stride);
          expected_data = remap->physical;
          auto sizes = function_->GetAttr<Map<Var, PrimExpr>>("size_map");
          ICHECK(sizes.defined() && sizes.value().count(expected_data) &&
                 analyzer_.CanProve(sizes.value()[expected_data] >=
                                    Integer(remap->total)))
              << "Actual allocation does not contain the S5 version remap";
        }
        ICHECK(expected_data.same_as(right[i].data) &&
               left[i].scope == right[i].scope &&
               Equal(left[i].begin + shift, right[i].begin) &&
               Equal(left[i].end + shift, right[i].end))
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
  bool Hazard(const Instance &a, const Instance &b, bool physical, bool noalias,
              bool renamed = false) {
    auto overlap = [&](const std::vector<Effect> &left,
                       const std::vector<Effect> &right) {
      for (const Effect &x : left)
        for (const Effect &y : right)
          if (renamed && !physical && x.scope == "shared.ub" &&
              y.scope == "shared.ub" && (Renamed(x) || Renamed(y)))
            continue;
          else if (Overlap(x, y, physical, noalias))
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
  bool versions_enabled_{false};
  bool lifetimes_enabled_{false};
  Record lifetime_contract_;
  std::vector<VersionRemap> remaps_;
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
