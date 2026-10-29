// Copyright (c) Tile-AI Organization.
// Licensed under the MIT License.

#include "legacy_composite.h"

#include <tvm/ir/type.h>
#include <tvm/tir/analysis.h>
#include <tvm/tir/builtin.h>
#include <tvm/tir/op.h>
#include <tvm/tir/op_attr_types.h>
#include <tvm/tir/stmt_functor.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <limits>
#include <utility>

#include "../common/pipeline_ring_provenance.h"
#include "upstream/constraints.h"

namespace tvm::tl::ascend_sync {

using namespace tir;

namespace {

std::string Trim(const std::string &text) {
  const size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return "";
  const size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

std::string OperationName(const CallNode *call) {
  std::string name;
  if (call->op.same_as(builtin::call_extern()) ||
      call->op.same_as(builtin::call_pure_extern())) {
    if (!call->args.empty()) {
      if (const auto *symbol = call->args[0].as<StringImmNode>())
        name = symbol->value;
    }
  } else if (const auto *op = call->op.as<OpNode>()) {
    name = op->name;
  }
  if (const size_t angle = name.find('<'); angle != std::string::npos)
    name.erase(angle);
  for (const std::string prefix : {"tl::ascend::", "tl.", "ascend_"}) {
    if (name.rfind(prefix, 0) == 0)
      name.erase(0, prefix.size());
  }
  return name;
}

std::string StorageScope(const Var &data) {
  const auto *pointer = data->type_annotation.as<PointerTypeNode>();
  return pointer == nullptr ? "" : std::string(pointer->storage_scope);
}

bool IsUserL0AB(const Var &data) {
  const std::string scope = StorageScope(data);
  return scope == "wmma.matrix_a" || scope == "wmma.matrix_b" ||
         scope == "shared.l0a" || scope == "shared.l0b" ||
         scope == "shared.l0a.dyn" || scope == "shared.l0b.dyn" ||
         scope == "shared.l0a.sf" || scope == "shared.l0b.sf";
}

bool IsPipelineKey(const String &key) {
  const std::string name = key;
  return name == "num_stages" || name.rfind("software_pipeline_", 0) == 0 ||
         name.rfind("tl_pipeline_", 0) == 0;
}

int64_t PositiveInteger(const std::string &text) {
  const std::string token = Trim(text);
  uint64_t value = 0;
  const auto parsed =
      std::from_chars(token.data(), token.data() + token.size(), value);
  ICHECK(!token.empty() && parsed.ec == std::errc() &&
         parsed.ptr == token.data() + token.size() && value > 0 &&
         value <= std::numeric_limits<uint32_t>::max())
      << "Composite gemm_v0 requires static positive M/N/K/kL0Size: " << text;
  return static_cast<int64_t>(value);
}

DataType ElementType(const std::string &token) {
  if (token == "half")
    return DataType::Float(16);
  if (token == "bfloat16_t")
    return DataType::BFloat(16);
  if (token == "float")
    return DataType::Float(32);
  if (token == "int8_t")
    return DataType::Int(8);
  if (token == "int32_t")
    return DataType::Int(32);
  LOG(FATAL) << "Composite gemm_v0 has an unsupported element type: " << token;
  return DataType::Void();
}

std::vector<std::string> TemplateArguments(const CallNode *call) {
  ICHECK_EQ(call->args.size(), 6U)
      << "Malformed typed composite gemm_v0 arguments";
  const auto *symbol = call->args[0].as<StringImmNode>();
  ICHECK(symbol != nullptr)
      << "Composite gemm_v0 requires a literal helper template";
  const std::string name = Trim(symbol->value);
  ICHECK(name.rfind("gemm_v0<", 0) == 0 && name.back() == '>')
      << "Composite gemm_v0 helper template does not match its typed op";
  const size_t begin = name.find('<') + 1;
  const size_t end = name.size() - 1;
  std::vector<std::string> arguments;
  size_t position = begin;
  while (position <= end) {
    const size_t comma = name.find(',', position);
    const size_t next = comma == std::string::npos ? end : comma;
    arguments.push_back(Trim(name.substr(position, next - position)));
    position = next + 1;
  }
  ICHECK_EQ(arguments.size(), 8U)
      << "Composite gemm_v0 requires its eight canonical template arguments";
  for (size_t index : {5U, 6U})
    ICHECK(arguments[index] == "true" || arguments[index] == "false")
        << "Composite gemm_v0 transpose flags must be static booleans";
  return arguments;
}

LegacyCompositeOperation AnalyzeOperation(const Call &call) {
  const auto arguments = TemplateArguments(call.get());
  LegacyCompositeOperation result;
  result.call = call;
  result.m = PositiveInteger(arguments[2]);
  result.n = PositiveInteger(arguments[3]);
  result.k = PositiveInteger(arguments[4]);
  result.k_l0 = PositiveInteger(arguments[7]);
  ICHECK(result.k_l0 % 16 == 0 && result.k_l0 <= 4095)
      << "Composite gemm_v0 kL0Size must be 16-aligned and at most 4095";
  const DataType input = ElementType(arguments[0]);
  const DataType output = ElementType(arguments[1]);
  ICHECK((input == DataType::Int(8) && output == DataType::Int(32)) ||
         ((input == DataType::Float(16) || input == DataType::BFloat(16) ||
           input == DataType::Float(32)) &&
          output == DataType::Float(32)))
      << "Composite gemm_v0 input/accumulator dtype combination is unsupported";
  const auto *n_actual = call->args[5].as<IntImmNode>();
  ICHECK(n_actual != nullptr && n_actual->value == result.n)
      << "Composite gemm_v0 currently requires static n_actual == N";
  ICHECK(call->args[4].dtype().is_scalar() &&
         SideEffect(call->args[4]) <= CallEffectKind::kPure)
      << "Composite gemm_v0 clear condition must be a pure scalar";
  const auto *clear = call->args[4].as<IntImmNode>();
  const bool clears_accumulator = clear != nullptr && clear->value == 1;

  for (size_t index = 1; index <= 3; ++index) {
    const auto *pointer = call->args[index].as<CallNode>();
    ICHECK(pointer != nullptr &&
           pointer->op.same_as(builtin::tvm_access_ptr()) &&
           pointer->args.size() == 5U)
        << "Composite gemm_v0 requires canonical A/B/C access_ptr operands";
    const auto *data = pointer->args[1].as<VarNode>();
    ICHECK(data != nullptr)
        << "Composite gemm_v0 requires explicit buffer Var identities";
    const std::string scope = StorageScope(GetRef<Var>(data));
    ICHECK(index <= 2 ? scope == "shared.l1" : scope == "wmma.accumulator")
        << "Composite gemm_v0 requires A/B in L1 and C in L0C";
    const auto *annotation = pointer->args[0].as<CallNode>();
    ICHECK(annotation != nullptr &&
           annotation->op.same_as(Op::Get("tir.type_annotation")) &&
           annotation->dtype == (index <= 2 ? input : output))
        << "Composite gemm_v0 pointer dtype disagrees with its helper template";
    const auto *mask = pointer->args[4].as<IntImmNode>();
    ICHECK(mask != nullptr && mask->value > 0 && mask->value <= 3)
        << "Composite gemm_v0 requires literal valid access masks";
    ICHECK(SideEffect(pointer->args[2]) <= CallEffectKind::kPure &&
           SideEffect(pointer->args[3]) <= CallEffectKind::kPure)
        << "Composite gemm_v0 pointer offset/extent must be pure";
    bool read = (mask->value & 1) != 0 || index <= 2 ||
                (index == 3 && !clears_accumulator);
    bool write = (mask->value & 2) != 0 || index == 3;
    result.buffers.push_back({GetRef<Call>(pointer), read, write});
  }
  return result;
}

void ValidateAllocationFootprints(LegacyCompositeOperation &operation,
                                  const PrimFunc &function,
                                  const PhysicalRingProvenance &rings = {}) {
  const auto sizes = function->GetAttr<Map<Var, PrimExpr>>("size_map");
  ICHECK(sizes.defined())
      << "Composite gemm_v0 requires final allocation size_map metadata";
  const int64_t rows[] = {operation.m, operation.k, operation.m};
  const int64_t columns[] = {operation.k, operation.n, operation.n};
  for (size_t index = 0; index < operation.buffers.size(); ++index) {
    const Call &pointer = operation.buffers[index].access_ptr;
    const auto *offset = pointer->args[2].as<IntImmNode>();
    const auto *extent = pointer->args[3].as<IntImmNode>();
    ICHECK_LE(rows[index],
              std::numeric_limits<int64_t>::max() / columns[index]);
    ICHECK(extent != nullptr && extent->value == rows[index] * columns[index])
        << "Composite gemm_v0 access extent disagrees with its static shape";
    const DataType dtype = pointer->args[0].dtype();
    const int64_t bytes = dtype.bits() / 8;
    // A conservative upper bound covers both zN and nZ orientation, plus
    // L0C's fractal padding. This first contract may reject tighter legal
    // layouts; it never shrinks an opaque helper to its logical pointer span.
    const int64_t quantum = std::max<int64_t>(16, 32 / bytes);
    const int64_t padded_rows = (rows[index] + quantum - 1) / quantum * quantum;
    const int64_t padded_columns =
        (columns[index] + quantum - 1) / quantum * quantum;
    ICHECK_LE(padded_rows,
              std::numeric_limits<int64_t>::max() / padded_columns / bytes);
    const int64_t required_bytes = padded_rows * padded_columns * bytes;
    const Var data = Downcast<Var>(pointer->args[1]);
    auto &effect = operation.buffers[index];
    effect.required_bytes = required_bytes;
    if (index < 2 && !rings.empty()) {
      for (const auto &[id, fields] : rings) {
        ValidatePhysicalRingRecord(fields);
        if (!fields[kPhysicalRingRoot].same_as(data))
          continue;
        ICHECK(!effect.logical_ring.defined())
            << "Cube pipeline does not support multiple rings in one root";
        const auto *stride = fields[kPhysicalRingStrideBits].as<IntImmNode>();
        ICHECK(stride != nullptr && stride->value > 0 &&
               stride->value % 8 == 0 && required_bytes <= stride->value / 8)
            << "Cube pipeline GEMM physical footprint exceeds its ring slot";
        effect.logical_ring = id;
      }
      ICHECK(effect.logical_ring.defined())
          << "Cube pipeline GEMM operand is missing its logical ring";
      continue; // Clock/base/offset validation uses the real task context.
    }
    ICHECK(offset != nullptr && offset->value == 0)
        << "Composite gemm_v0 currently requires base allocation pointers; "
           "subviews/rings need a separate physical footprint contract";
    const auto size = sizes.value().Get(data);
    const auto *constant_size =
        size.defined() ? size.value().as<IntImmNode>() : nullptr;
    ICHECK(constant_size != nullptr && constant_size->value >= required_bytes)
        << "Composite gemm_v0 allocation does not cover its padded physical "
           "footprint: "
        << data << " requires at least " << required_bytes << " bytes";
  }
}

// Do not flatten or rewrite executable control. This walker only audits the
// three calculation leaves admitted in the first ordinary K-loop contract.
void CollectKLoopLeaves(const Stmt &statement, int resource,
                        std::vector<Call> *leaves) {
  const char *diagnostic =
      "Composite gemm_v0 external For contract requires only full A/B "
      "GM-to-L1 copies followed by one gemm; loop-internal FIX, nested control "
      "and other operations require separate validation";
  if (const auto *sequence = statement.as<SeqStmtNode>()) {
    for (const Stmt &child : sequence->seq)
      CollectKLoopLeaves(child, resource, leaves);
  } else if (const auto *binding = statement.as<LetStmtNode>()) {
    ICHECK(SideEffect(binding->value) <= CallEffectKind::kPure) << diagnostic;
    CollectKLoopLeaves(binding->body, resource, leaves);
  } else if (const auto *attribute = statement.as<AttrStmtNode>()) {
    ICHECK(attribute->attr_key == "resource_scope") << diagnostic;
    const auto *owner = attribute->value.as<IntImmNode>();
    ICHECK(owner != nullptr && owner->value == 0) << diagnostic;
    CollectKLoopLeaves(attribute->body, 0, leaves);
  } else if (const auto *evaluation = statement.as<EvaluateNode>()) {
    if (const auto *zero = evaluation->value.as<IntImmNode>()) {
      ICHECK_EQ(zero->value, 0) << diagnostic;
      return;
    }
    ICHECK(resource == -1 || resource == 0) << diagnostic;
    const auto *call = evaluation->value.as<CallNode>();
    ICHECK(call != nullptr) << diagnostic;
    leaves->push_back(GetRef<Call>(call));
  } else {
    LOG(FATAL) << diagnostic;
  }
}

void ValidateKLoopCopy(const Call &copy, const Call &gemm_pointer,
                       const std::string &element_type, int64_t rows,
                       int64_t columns, bool physical_ring = false) {
  const char *diagnostic =
      "Composite gemm_v0 external For requires full base-allocation A/B "
      "GM-to-L1 refreshes before each gemm";
  ICHECK(copy->op.same_as(builtin::call_extern()) && copy->args.size() == 8U &&
         OperationName(copy.get()) == "copy_gm_to_l1")
      << diagnostic;
  const auto *symbol = copy->args[0].as<StringImmNode>();
  ICHECK(symbol != nullptr) << diagnostic;
  std::string compact = symbol->value;
  compact.erase(std::remove_if(compact.begin(), compact.end(),
                               [](unsigned char c) { return std::isspace(c); }),
                compact.end());
  ICHECK_EQ(compact, "tl::ascend::copy_gm_to_l1<" + element_type + "," +
                         std::to_string(rows) + "," + std::to_string(columns) +
                         ">")
      << diagnostic;
  const auto *source = copy->args[1].as<CallNode>();
  const auto *destination = copy->args[2].as<CallNode>();
  for (const CallNode *pointer : {source, destination}) {
    ICHECK(pointer != nullptr &&
           pointer->op.same_as(builtin::tvm_access_ptr()) &&
           pointer->args.size() == 5U)
        << diagnostic;
    const auto *extent = pointer->args[3].as<IntImmNode>();
    ICHECK(extent != nullptr && extent->value == rows * columns) << diagnostic;
    ICHECK(pointer->args[0].dtype() == gemm_pointer->args[0].dtype())
        << diagnostic;
  }
  const auto *source_data = source->args[1].as<VarNode>();
  ICHECK(source_data != nullptr) << diagnostic;
  const std::string source_scope = StorageScope(GetRef<Var>(source_data));
  ICHECK(source_scope.empty() || source_scope == "global") << diagnostic;
  ICHECK(destination->args[1].same_as(gemm_pointer->args[1])) << diagnostic;
  const auto *offset = destination->args[2].as<IntImmNode>();
  ICHECK(physical_ring || (offset != nullptr && offset->value == 0))
      << diagnostic;
  for (const auto &[pointer, mask] :
       {std::pair<const CallNode *, int>{source, 1}, {destination, 2}}) {
    const auto *literal = pointer->args[4].as<IntImmNode>();
    ICHECK(literal != nullptr && literal->value == mask) << diagnostic;
  }
  // These are the real/full row and column masks emitted by AscendCopy. The
  // stride may vary with the original GM tensor, but every tile is complete.
  for (size_t argument : {4U, 6U}) {
    const auto *mask = copy->args[argument].as<IntImmNode>();
    ICHECK(mask != nullptr && mask->value == rows) << diagnostic;
  }
  for (size_t argument : {5U, 7U}) {
    const auto *mask = copy->args[argument].as<IntImmNode>();
    ICHECK(mask != nullptr && mask->value == columns) << diagnostic;
  }
}

constexpr const char *kPtoProfile = "PTO flat gemm_v0 profile";

void CollectPtoFlatLeaves(const Stmt &statement, std::vector<Call> *leaves) {
  if (const auto *sequence = statement.as<SeqStmtNode>()) {
    for (const Stmt &child : sequence->seq)
      CollectPtoFlatLeaves(child, leaves);
  } else if (const auto *attribute = statement.as<AttrStmtNode>()) {
    ICHECK(SideEffect(attribute->value) <= CallEffectKind::kPure)
        << kPtoProfile << " requires pure lexical metadata";
    if (attribute->attr_key == "resource_scope") {
      const auto *scope = attribute->value.as<IntImmNode>();
      ICHECK(scope != nullptr && scope->value == 0)
          << kPtoProfile << " requires Cube-only execution";
    }
    CollectPtoFlatLeaves(attribute->body, leaves);
  } else if (const auto *binding = statement.as<LetStmtNode>()) {
    ICHECK(SideEffect(binding->value) <= CallEffectKind::kPure)
        << kPtoProfile << " cannot import mutable scalar definitions";
    CollectPtoFlatLeaves(binding->body, leaves);
  } else if (const auto *allocation = statement.as<AllocateNode>()) {
    CollectPtoFlatLeaves(allocation->body, leaves);
  } else if (const auto *declaration = statement.as<DeclBufferNode>()) {
    CollectPtoFlatLeaves(declaration->body, leaves);
  } else if (const auto *evaluate = statement.as<EvaluateNode>()) {
    if (evaluate->value.as<IntImmNode>())
      return;
    const auto *call = evaluate->value.as<CallNode>();
    ICHECK(call != nullptr) << kPtoProfile << " requires canonical call leaves";
    const std::string name = OperationName(call);
    if (name == "set_flag" || name == "wait_flag" || name == "auto_set_flag" ||
        name == "auto_wait_flag")
      return; // Independently balanced and checked against hidden reservations.
    ICHECK(name == "copy_gm_to_l1" || name == "gemm_v0" ||
           name == "copy_l0c_to_gm")
        << kPtoProfile
        << " rejects extra compute, mixed C/V and unknown helpers";
    leaves->push_back(GetRef<Call>(call));
  } else {
    LOG(FATAL) << kPtoProfile
               << " requires flat execution without For/If/rings";
  }
}

void ValidatePtoFlat(const PrimFunc &function,
                     const LegacyCompositePlan &plan) {
  ICHECK_EQ(plan.operations.size(), 1U)
      << kPtoProfile << " requires one helper";
  const LegacyCompositeOperation &operation = plan.operations.begin()->second;
  const Call &gemm = operation.call;
  const auto arguments = TemplateArguments(gemm.get());
  const auto *clear = gemm->args[4].as<IntImmNode>();
  ICHECK(operation.m == 16 && operation.n == 16 &&
         (operation.k == 128 || operation.k == 256) && operation.k_l0 == 128 &&
         arguments[0] == "half" && arguments[1] == "float" &&
         arguments[5] == "false" && arguments[6] == "false" &&
         clear != nullptr && clear->value == 1)
      << kPtoProfile
      << " requires FP16->FP32 M=N=16, K=128/256, kL0Size=128, "
         "clear=true and no transpose";
  // PTO codegen rounds M/N to 16 and K to a 32-byte column. In this profile
  // these are identities. Both TEXTRACT destinations use address 0; the real
  // per-step L0A/B spans are 16*128*2 = 4096 bytes, one slot in each 64KiB
  // hardware pool. There is no N tiling or AscendC two-slot scratch contract.
  const auto shapes =
      function->GetAttr<Map<Var, Array<PrimExpr>>>("buffer_shapess");
  ICHECK(shapes.defined()) << kPtoProfile
                           << " requires final descriptor shapes";
  const int64_t rows[] = {16, operation.k, 16};
  const int64_t columns[] = {operation.k, 16, 16};
  for (size_t index = 0; index < 3; ++index) {
    const Var data =
        Downcast<Var>(operation.buffers[index].access_ptr->args[1]);
    const auto shape = shapes.value().Get(data);
    ICHECK(shape.defined() &&
           (shape.value().size() == 2U || shape.value().size() == 4U))
        << kPtoProfile << " requires canonical complete descriptor shapes";
    for (size_t axis = 0; axis < shape.value().size(); ++axis) {
      const auto *dimension = shape.value()[axis].as<IntImmNode>();
      ICHECK(dimension != nullptr &&
             dimension->value == (axis % 2 == 0 ? rows[index] : columns[index]))
          << kPtoProfile
          << " descriptor shape disagrees with actual helper footprint";
    }
  }
  std::vector<Call> leaves;
  CollectPtoFlatLeaves(function->body, &leaves);
  ICHECK(leaves.size() == 4U && leaves[2].same_as(gemm) &&
         OperationName(leaves[3].get()) == "copy_l0c_to_gm")
      << kPtoProfile << " requires A/B full GM-to-L1, one gemm and final FIX";
  ValidateKLoopCopy(leaves[0], operation.buffers[0].access_ptr, "half", 16,
                    operation.k);
  ValidateKLoopCopy(leaves[1], operation.buffers[1].access_ptr, "half",
                    operation.k, 16);
  ICHECK_GE(leaves[3]->args.size(), 6U)
      << kPtoProfile << " requires canonical final FIX operands";
  const auto *source = leaves[3]->args[1].as<CallNode>();
  ICHECK(source != nullptr && source->op.same_as(builtin::tvm_access_ptr()) &&
         source->args.size() == 5U &&
         source->args[1].same_as(operation.buffers[2].access_ptr->args[1]))
      << kPtoProfile << " final FIX must consume the audited accumulator";
  const auto *annotation = source->args[0].as<CallNode>();
  ICHECK(annotation != nullptr && annotation->dtype == DataType::Float(32))
      << kPtoProfile << " final FIX requires an FP32 accumulator";
  for (const auto &[value, expected] :
       {std::pair<PrimExpr, int64_t>{source->args[2], 0},
        {source->args[3], 256},
        {leaves[3]->args[4], 16},
        {leaves[3]->args[5], 16}}) {
    const auto *constant = value.as<IntImmNode>();
    ICHECK(constant != nullptr && constant->value == expected)
        << kPtoProfile
        << " final FIX footprint must be the full base accumulator";
  }
}

// This is a certificate/admission audit of materialized legacy SIP, not a
// second scheduler. It neither changes clocks/B nor rewrites calculation IR.
class StaticCubePipelineAudit {
public:
  StaticCubePipelineAudit(const PrimFunc &function, LegacyCompositePlan &plan)
      : function_(function), plan_(plan) {}

  void Run() {
    const auto owners =
        function_->GetAttr<PhysicalPipelineOwners>(kPhysicalPipelineOwners);
    const auto rings =
        function_->GetAttr<PhysicalRingProvenance>(kPhysicalRingProvenance);
    ICHECK(owners.defined() && owners.value().size() == 1 && rings.defined() &&
           rings.value().size() == 2)
        << Error() << "requires one owner and two independent L1 rings";
    owner_ = (*owners.value().begin()).first;
    const auto fields = (*owners.value().begin()).second;
    ICHECK(fields.size() == 4 && Constant(fields[0]) == 1 &&
           Constant(fields[1]) == 1 && Constant(fields[2]) == 0 &&
           Constant(fields[3]) == 3)
        << Error() << "requires the actual lead=1, minimum=0 and three orders";
    const auto &first = plan_.operations.begin()->second;
    const auto arguments = TemplateArguments(first.call.get());
    // The first physical Cube contract has one full, aligned helper tile.
    ICHECK(arguments[0] == "half" && arguments[1] == "float" &&
           arguments[5] == "false" && arguments[6] == "false" &&
           first.m % 16 == 0 && first.n % 16 == 0 && first.k % 16 == 0 &&
           first.k_l0 == first.k)
        << Error()
        << "requires full aligned fp16 tiles without transpose or "
           "internal K splitting";
    a_ = first.buffers[0].logical_ring.value();
    b_ = first.buffers[1].logical_ring.value();
    c_ = Downcast<Var>(first.buffers[2].access_ptr->args[1]);
    ICHECK(!a_.same_as(b_)) << Error() << "A and B must have distinct lineage";
    for (const auto &[id, record] : rings.value()) {
      ValidatePhysicalRingRecord(record);
      ICHECK((id.same_as(a_) || id.same_as(b_)) &&
             Constant(record[kPhysicalRingVersions]) == 2 &&
             Constant(record[kPhysicalRingOriginalMin]) == 0 &&
             StorageScope(Downcast<Var>(record[kPhysicalRingRoot])) ==
                 "shared.l1")
          << Error() << "requires true B=2 in independent L1 allocations";
    }
    ICHECK(!rings.value().at(a_)[kPhysicalRingRoot].same_as(
        rings.value().at(b_)[kPhysicalRingRoot]))
        << Error() << "multiple logical rings sharing one root are unsupported";
    WalkOuter(function_->body, {});
    ICHECK(saw_owner_ && saw_steady_ && final_stores_ == 1 &&
           orders_[0] == std::vector<int>({0, 1}) &&
           orders_[1] == std::vector<int>({0, 1, 2}) &&
           orders_[2] == std::vector<int>({2}))
        << Error() << "incomplete or reordered prologue/steady/epilogue/FIX";
    ICHECK_EQ(gemms_, plan_.operations.size())
        << Error() << "unaccounted GEMM operation";
    plan_.cube_pipeline_loop = steady_;
  }

private:
  static const char *Error() { return "Static Cube pipeline contract: "; }
  static int64_t Constant(const PrimExpr &value) {
    const auto *number = value.as<IntImmNode>();
    ICHECK(number != nullptr && number->value >= 0 &&
           number->value <= std::numeric_limits<int32_t>::max())
        << Error() << "requires static nonnegative metadata/bounds";
    return number->value;
  }
  static void Pure(const PrimExpr &value) {
    ICHECK(SideEffect(value) <= CallEffectKind::kPure)
        << Error() << "control, clocks and bindings must be pure";
  }
  Var Identity(const ObjectRef &node) const {
    const auto ids = Downcast<Array<Var>>(node);
    ICHECK(ids.size() == 1 && ids[0].same_as(owner_))
        << Error() << "owner identity mismatch";
    return ids[0];
  }
  void Attribute(const AttrStmtNode *op, ConstrSet *context) const {
    Pure(op->value);
    if (op->node.as<PrimExprNode>())
      Pure(Downcast<PrimExpr>(op->node));
    ICHECK(std::string(op->attr_key).rfind("async_", 0) != 0)
        << Error() << "async scopes require another contract";
    if (op->attr_key == "resource_scope")
      ICHECK_EQ(Constant(op->value), 0) << Error() << "requires Cube scope";
    if (op->attr_key == "thread_extent" || op->attr_key == "virtual_thread") {
      const IterVar iteration = Downcast<IterVar>(op->node);
      context->AddConstr(
          iteration->var,
          Range::FromMinExtent(make_zero(op->value.dtype()), op->value));
    } else if ((op->attr_key == "tl.assume" ||
                op->attr_key == "tilelang_assume") &&
               op->node.as<PrimExprNode>()) {
      context->AddConstr(Downcast<PrimExpr>(op->node), true);
    }
  }
  void WalkOuter(const Stmt &statement, ConstrSet context) {
    if (const auto *sequence = statement.as<SeqStmtNode>()) {
      for (const Stmt &child : sequence->seq)
        WalkOuter(child, context);
    } else if (const auto *attribute = statement.as<AttrStmtNode>()) {
      Attribute(attribute, &context);
      if (attribute->attr_key == kPhysicalPipelineExtent) {
        Identity(attribute->node);
        ICHECK(!saw_owner_) << Error() << "duplicate owner";
        saw_owner_ = true;
        trip_count_ = Constant(attribute->value);
        ICHECK(trip_count_ >= 3 &&
               trip_count_ <= std::numeric_limits<int32_t>::max())
            << Error() << "requires static K trip count >= 3";
        WalkOwner(attribute->body, context, -1);
        owner_finished_ = true;
      } else {
        ICHECK(!IsPhysicalPipelineMetadata(attribute->attr_key))
            << Error() << "orphan pipeline metadata";
        WalkOuter(attribute->body, context);
      }
    } else if (const auto *allocation = statement.as<AllocateNode>()) {
      Pure(allocation->condition);
      for (const PrimExpr &extent : allocation->extents)
        Pure(extent);
      WalkOuter(allocation->body, context);
    } else if (const auto *declaration = statement.as<DeclBufferNode>()) {
      for (const PrimExpr &extent : declaration->buffer->shape)
        Pure(extent);
      for (const PrimExpr &stride : declaration->buffer->strides)
        Pure(stride);
      Pure(declaration->buffer->elem_offset);
      WalkOuter(declaration->body, context);
    } else if (const auto *binding = statement.as<LetStmtNode>()) {
      Pure(binding->value);
      context.AddConstr(binding->var, binding->value);
      WalkOuter(binding->body, context);
    } else if (const auto *evaluation = statement.as<EvaluateNode>()) {
      if (is_zero(evaluation->value))
        return;
      const auto *call = evaluation->value.as<CallNode>();
      ICHECK(owner_finished_ && call != nullptr &&
             OperationName(call) == "copy_l0c_to_gm" && call->args.size() >= 3)
          << Error() << "requires only one final FIX after its owner";
      const auto *pointer = call->args[1].as<CallNode>();
      ICHECK(pointer && pointer->op.same_as(builtin::tvm_access_ptr()) &&
             pointer->args.size() == 5 && pointer->args[1].same_as(c_) &&
             is_zero(pointer->args[2]))
          << Error() << "final FIX must consume the same base accumulator";
      ++final_stores_;
    } else {
      LOG(FATAL) << Error() << "outer semantic control/extra work unsupported";
    }
  }
  void WalkOwner(const Stmt &statement, ConstrSet context, int segment) {
    if (const auto *sequence = statement.as<SeqStmtNode>()) {
      for (const Stmt &child : sequence->seq)
        WalkOwner(child, context, segment);
    } else if (const auto *attribute = statement.as<AttrStmtNode>()) {
      Attribute(attribute, &context);
      if (attribute->attr_key == kPhysicalPipelineSegment) {
        Identity(attribute->node);
        ICHECK_EQ(segment, -1) << Error() << "nested segment";
        const int role = Constant(attribute->value);
        ICHECK(role <= 2 && !segments_[role])
            << Error() << "invalid/duplicate segment";
        segments_[role] = true;
        WalkOwner(attribute->body, context, role);
      } else if (attribute->attr_key == kPhysicalPipelineStage) {
        const auto fields = Downcast<Array<PrimExpr>>(attribute->node);
        ICHECK(fields.size() == 5 && fields[0].same_as(owner_) &&
               Constant(fields[1]) == segment && segment >= 0 && segment <= 2)
            << Error() << "stage owner/segment mismatch";
        const int stage = Constant(fields[2]), order = Constant(fields[3]);
        ICHECK(order <= 2 && stage == (order == 2 ? 1 : 0) &&
               Constant(fields[4]) == (segment == 1 ? 0 : 1))
            << Error() << "stage/order/check certificate changed";
        const PrimExpr physical =
            cast(DataType::Int(64), attribute->value) + Integer(stage);
        ICHECK(segment != 1 || steady_.defined())
            << Error() << "steady frame has no real loop";
        const PrimExpr expected =
            segment == 0 ? PrimExpr(Integer(0))
            : segment == 2
                ? PrimExpr(IntImm(DataType::Int(64), trip_count_))
                : cast(DataType::Int(64), steady_.value()) + Integer(1);
        ICHECK(context.CanProve(physical == expected))
            << Error() << "stage clock disagrees with physical execution";
        orders_[segment].push_back(order);
        int work = 0;
        WalkStage(attribute->body, context, {}, attribute->value, order, &work);
        ICHECK_EQ(work, 1) << Error() << "each stage requires one operation";
      } else {
        ICHECK(!IsPhysicalPipelineMetadata(attribute->attr_key))
            << Error() << "unexpected owner metadata";
        WalkOwner(attribute->body, context, segment);
      }
    } else if (const auto *loop = statement.as<ForNode>()) {
      Pure(loop->min);
      Pure(loop->extent);
      ICHECK(segment == 1 && !saw_steady_ && loop->kind == ForKind::kSerial &&
             Constant(loop->min) == 0 &&
             Constant(loop->extent) == trip_count_ - 1)
          << Error() << "requires the original static serial steady loop";
      for (const auto &[key, value] : loop->annotations)
        ICHECK(key == "tl_cross_interval" &&
               Downcast<IntImm>(value)->value == 1)
            << Error() << "unsupported steady-loop annotation";
      saw_steady_ = true;
      steady_ = loop->loop_var;
      context.AddConstr(steady_.value(),
                        Range::FromMinExtent(loop->min, loop->extent));
      WalkOwner(loop->body, context, segment);
    } else if (const auto *binding = statement.as<LetStmtNode>()) {
      Pure(binding->value);
      context.AddConstr(binding->var, binding->value);
      WalkOwner(binding->body, context, segment);
    } else {
      const auto *evaluation = statement.as<EvaluateNode>();
      ICHECK(evaluation && is_zero(evaluation->value))
          << Error() << "unmarked work or control inside pipeline";
    }
  }
  void WalkStage(const Stmt &statement, ConstrSet context,
                 Map<Var, PrimExpr> clocks, const PrimExpr &clock, int order,
                 int *work) {
    if (const auto *attribute = statement.as<AttrStmtNode>()) {
      Attribute(attribute, &context);
      if (attribute->attr_key == kPhysicalRingClock) {
        const Var id = PhysicalRingClockId(attribute->node);
        ICHECK((id.same_as(a_) || id.same_as(b_)) &&
               context.CanProve(attribute->value == clock))
            << Error() << "ring clock disagrees with its stage clock";
        clocks.Set(id, attribute->value);
      } else {
        ICHECK(!IsPhysicalPipelineMetadata(attribute->attr_key))
            << Error() << "nested stage/owner metadata";
      }
      WalkStage(attribute->body, context, clocks, clock, order, work);
    } else if (const auto *binding = statement.as<LetStmtNode>()) {
      Pure(binding->value);
      context.AddConstr(binding->var, binding->value);
      WalkStage(binding->body, context, clocks, clock, order, work);
    } else {
      const auto *evaluation = statement.as<EvaluateNode>();
      const auto *call =
          evaluation ? evaluation->value.as<CallNode>() : nullptr;
      ICHECK(call != nullptr) << Error() << "stage control/work unsupported";
      const auto &first = plan_.operations.begin()->second;
      const auto arguments = TemplateArguments(first.call.get());
      if (order < 2) {
        const Var id = order == 0 ? a_ : b_;
        ICHECK(clocks.count(id)) << Error() << "copy is missing ring clock";
        ValidateKLoopCopy(GetRef<Call>(call), first.buffers[order].access_ptr,
                          arguments[0], order == 0 ? first.m : first.k,
                          order == 0 ? first.k : first.n, true);
        plan_.cube_ring_copies.emplace(
            call, std::make_pair(id, first.buffers[order].required_bytes));
      } else {
        const auto *operation = plan_.Find(call);
        ICHECK(operation && clocks.count(a_) && clocks.count(b_))
            << Error() << "GEMM template or ring clocks changed";
        // StringImm identities need not be shared by distinct SIP clones.
        ICHECK(Downcast<StringImm>(operation->call->args[0])->value ==
                   Downcast<StringImm>(first.call->args[0])->value &&
               operation->buffers[0].logical_ring.value().same_as(a_) &&
               operation->buffers[1].logical_ring.value().same_as(b_) &&
               operation->buffers[2].access_ptr->args[1].same_as(c_))
            << Error() << "GEMM lineage changed";
        ProofAnalyzer analyzer;
        context.Populate(analyzer);
        ICHECK(analyzer.CanProveEqual(call->args[4], clock == 0))
            << Error() << "GEMM clear must equal logical clock == 0";
        plan_.operations.at(call).pipeline_clock = clock;
        ++gemms_;
      }
      ++*work;
    }
  }

  PrimFunc function_;
  LegacyCompositePlan &plan_;
  Var owner_, a_, b_, c_;
  Optional<Var> steady_;
  int64_t trip_count_{0};
  bool saw_owner_{false}, owner_finished_{false}, saw_steady_{false};
  int final_stores_{0};
  size_t gemms_{0};
  std::array<bool, 3> segments_{};
  std::array<std::vector<int>, 3> orders_;
};

class CompositeCollector : public StmtExprVisitor {
public:
  LegacyCompositePlan Run(const PrimFunc &function, const Target &target,
                          const String &platform) {
    for (const auto &[parameter, buffer] : function->buffer_map)
      user_l0ab_ |= IsUserL0AB(buffer->data);
    for (const Var &parameter : function->params)
      user_l0ab_ |= IsUserL0AB(parameter);
    operator()(function->body);
    if (calls_.empty())
      return {};
    // The model selects device codegen. The target kind describes host-side
    // lowering and is llvm in the actual compilation path (or c in IR tests).
    const String model =
        target.defined() ? target->GetAttr<String>("model").value_or(String())
                         : String();
    const bool pto = model == "pto";
    ICHECK(platform == "A3" && (model == "ascendc" || model == "auto" || pto))
        << "Composite gemm_v0 currently requires AscendC on A3";
    const auto rings =
        function->GetAttr<PhysicalRingProvenance>(kPhysicalRingProvenance)
            .value_or(PhysicalRingProvenance{});
    const bool cube_pipeline = !rings.empty();
    if (pto)
      ICHECK(!cube_pipeline && !has_pipeline_ && loops_.empty())
          << kPtoProfile << " requires B1 flat execution without For/rings";
    ICHECK(cube_pipeline || !has_pipeline_)
        << "Composite gemm_v0 software pipelines require certified Cube rings";
    ICHECK(!has_conditional_)
        << "Composite gemm_v0 external If/While control requires separate "
           "hardware validation; the first contract is flat";
    ICHECK(!user_l0ab_ && !has_mma_)
        << "Composite gemm_v0 owns hidden L0A/L0B pools; user L0A/B and bare "
           "mma mixing is unsupported";
    for (const char *name : {"buffer_versions", "tl.buffer_versions_map"}) {
      if (auto versions = function->GetAttr<Map<Var, PrimExpr>>(name)) {
        for (const auto &[storage, version] : versions.value()) {
          const auto *constant = version.as<IntImmNode>();
          const auto record = rings.Get(storage);
          ICHECK(constant != nullptr &&
                 (constant->value == 1 ||
                  (cube_pipeline && constant->value == 2 && record.defined() &&
                   ConstantRingVersion(record.value()) == 2)))
              << "Composite gemm_v0 physical B>1/version inputs require a "
                 "later Cube-ring contract";
        }
      }
    }

    LegacyCompositePlan plan;
    // common.h:1252,1375-1439. The helper primes/drains both slots even
    // when its internal K/N loop has only one iteration.
    if (pto) {
      plan.helper_profile = LegacyCompositePlan::HelperProfile::kPtoA3Flat;
      // pto/common.h:145-232: EVENT_ID0==0, hence the literal war_event_id
      // is 1. Every K step reuses ONE L0A/B slot at address zero. Each pair
      // is balanced; there are no AscendC ping-pong prime/drain credits.
      plan.reserved_intra_ids = {
          {"MTE2_MTE1", {1}}, {"FIX_M", {1}},     {"M_MTE1", {1}},
          {"MTE1_M", {1}},    {"MTE1_MTE2", {1}}, {"M_FIX", {1}},
      };
    } else {
      plan.reserved_intra_ids = {
          {"MTE2_MTE1", {0}}, {"FIX_M", {0}},     {"M_MTE1", {0, 1}},
          {"MTE1_M", {0, 1}}, {"MTE1_MTE2", {0}}, {"M_FIX", {0}},
      };
    }
    for (const Call &call : manual_flags_)
      CheckManualFlag(call.get(), plan.reserved_intra_ids);
    for (const Call &call : calls_) {
      LegacyCompositeOperation operation = AnalyzeOperation(call);
      ValidateAllocationFootprints(operation, function, rings);
      plan.operations.emplace(call.get(), std::move(operation));
    }
    if (pto) {
      ValidatePtoFlat(function, plan);
    } else if (cube_pipeline) {
      StaticCubePipelineAudit(function, plan).Run();
      plan.static_cube_pipeline = true;
    } else {
      ValidateKLoop(plan);
    }
    return plan;
  }

private:
  static int64_t ConstantRingVersion(const Array<PrimExpr> &record) {
    ValidatePhysicalRingRecord(record);
    return Downcast<IntImm>(record[kPhysicalRingVersions])->value;
  }
  void ValidateKLoop(const LegacyCompositePlan &plan) const {
    if (loops_.empty())
      return;
    const char *diagnostic =
        "Composite gemm_v0 external For contract requires one static positive "
        "serial K loop with min=0, one gemm and clear=(k == 0)";
    ICHECK(loops_.size() == 1U && calls_.size() == 1U) << diagnostic;
    const auto &[loop, resource] = loops_.front();
    const auto *minimum = loop->min.as<IntImmNode>();
    const auto *extent = loop->extent.as<IntImmNode>();
    ICHECK(loop->kind == ForKind::kSerial && minimum != nullptr &&
           minimum->value == 0 && extent != nullptr && extent->value > 0 &&
           loop->annotations.empty())
        << diagnostic;
    std::vector<Call> leaves;
    CollectKLoopLeaves(loop->body, resource, &leaves);
    ICHECK(leaves.size() == 3U && leaves.back().same_as(calls_.front()))
        << "Composite gemm_v0 external For contract requires two A/B "
           "GM-to-L1 refreshes followed by one gemm";
    // Legacy Analyzer::CanProveEqual lowers equality to lhs-rhs==0 and
    // cannot prove equality between boolean predicates, even k==0 and k==0.
    // Use the existing proof-only adapter and the actual iteration domain;
    // neither the clear expression nor emitted control is rewritten.
    ProofAnalyzer analyzer;
    analyzer.Bind(loop->loop_var,
                  Range::FromMinExtent(loop->min, loop->extent));
    ICHECK(analyzer.CanProveEqual(calls_.front()->args[4], loop->loop_var == 0))
        << diagnostic;
    const LegacyCompositeOperation &operation =
        *plan.Find(calls_.front().get());
    const auto arguments = TemplateArguments(calls_.front().get());
    const bool transpose_a = arguments[5] == "true";
    const bool transpose_b = arguments[6] == "true";
    ValidateKLoopCopy(leaves[0], operation.buffers[0].access_ptr, arguments[0],
                      transpose_a ? operation.k : operation.m,
                      transpose_a ? operation.m : operation.k);
    ValidateKLoopCopy(leaves[1], operation.buffers[1].access_ptr, arguments[0],
                      transpose_b ? operation.n : operation.k,
                      transpose_b ? operation.k : operation.n);
  }

  static void
  CheckManualFlag(const CallNode *call,
                  const std::map<std::string, std::set<int>> &reservations) {
    const std::string name = OperationName(call);
    const size_t offset = call->op.same_as(builtin::call_extern()) ||
                                  call->op.same_as(builtin::call_pure_extern())
                              ? 1
                              : 0;
    const bool automatic = name == "auto_set_flag" || name == "auto_wait_flag";
    const size_t count = automatic ? 2 : 3;
    ICHECK_EQ(call->args.size(), offset + count)
        << "Malformed manual event near composite gemm_v0";
    auto pipe = [&](size_t index) {
      const auto *literal = call->args[offset + index].as<StringImmNode>();
      ICHECK(literal != nullptr)
          << "Composite gemm_v0 requires literal manual event directions";
      std::string value = literal->value;
      if (value.rfind("PIPE_", 0) == 0)
        value.erase(0, 5);
      return value;
    };
    const std::string event = automatic ? pipe(0) : pipe(0) + "_" + pipe(1);
    const auto *id = call->args[offset + count - 1].as<IntImmNode>();
    ICHECK(id != nullptr && id->value >= 0 && id->value < 8)
        << "Composite gemm_v0 requires bounded literal manual event IDs";
    if (auto it = reservations.find(event); it != reservations.end())
      ICHECK(!it->second.count(static_cast<int>(id->value)))
          << "Manual event conflicts with composite gemm_v0 hidden event pool: "
          << event << " ID " << id->value;
  }

  void VisitStmt_(const ForNode *op) final {
    loops_.emplace_back(op, resource_);
    for (const auto &[key, value] : op->annotations)
      has_pipeline_ |= IsPipelineKey(key);
    StmtExprVisitor::VisitStmt_(op);
  }

  void VisitStmt_(const AllocateNode *op) final {
    user_l0ab_ |= IsUserL0AB(op->buffer_var);
    for (const auto &[key, value] : op->annotations)
      has_pipeline_ |= IsPipelineKey(key);
    StmtExprVisitor::VisitStmt_(op);
  }

  void VisitStmt_(const DeclBufferNode *op) final {
    user_l0ab_ |= IsUserL0AB(op->buffer->data);
    StmtExprVisitor::VisitStmt_(op);
  }

  void VisitStmt_(const IfThenElseNode *op) final {
    has_conditional_ = true;
    StmtExprVisitor::VisitStmt_(op);
  }

  void VisitStmt_(const WhileNode *op) final {
    has_conditional_ = true;
    StmtExprVisitor::VisitStmt_(op);
  }

  void VisitStmt_(const BlockNode *op) final {
    for (const Buffer &buffer : op->alloc_buffers)
      user_l0ab_ |= IsUserL0AB(buffer->data);
    for (const auto &[key, value] : op->annotations)
      has_pipeline_ |= IsPipelineKey(key);
    StmtExprVisitor::VisitStmt_(op);
  }

  void VisitStmt_(const BufferStoreNode *op) final {
    user_l0ab_ |= IsUserL0AB(op->buffer->data);
    StmtExprVisitor::VisitStmt_(op);
  }

  void VisitExpr_(const BufferLoadNode *op) final {
    user_l0ab_ |= IsUserL0AB(op->buffer->data);
    StmtExprVisitor::VisitExpr_(op);
  }

  void VisitStmt_(const AttrStmtNode *op) final {
    has_pipeline_ |= IsPipelineKey(op->attr_key);
    const int previous = resource_;
    if (op->attr_key == "resource_scope") {
      const auto *resource = op->value.as<IntImmNode>();
      resource_ = resource == nullptr ? -2 : resource->value;
    }
    StmtExprVisitor::VisitStmt_(op);
    resource_ = previous;
  }

  void VisitExpr_(const CallNode *op) final {
    const std::string name = OperationName(op);
    if (name == "gemm_v1")
      LOG(FATAL) << "Composite gemm_v1 has no audited helper contract";
    if (name == "gemm_v0") {
      ICHECK(op->op.same_as(Op::Get("tl.ascend_gemm_v0")))
          << "Composite gemm_v0 only supports its canonical typed operation";
      ICHECK(resource_ == -1 || resource_ == 0)
          << "Composite gemm_v0 execution owner must be Cube";
      calls_.push_back(GetRef<Call>(op));
    }
    has_mma_ |= name == "mma";
    if (name == "set_flag" || name == "wait_flag" || name == "auto_set_flag" ||
        name == "auto_wait_flag")
      manual_flags_.push_back(GetRef<Call>(op));
    if (op->op.same_as(builtin::tvm_access_ptr()) && op->args.size() == 5U) {
      if (const auto *data = op->args[1].as<VarNode>())
        user_l0ab_ |= IsUserL0AB(GetRef<Var>(data));
    }
    StmtExprVisitor::VisitExpr_(op);
  }

  std::vector<Call> calls_;
  std::vector<Call> manual_flags_;
  std::vector<std::pair<const ForNode *, int>> loops_;
  bool has_pipeline_{false};
  bool has_conditional_{false};
  bool user_l0ab_{false};
  bool has_mma_{false};
  int resource_{-1};
};

} // namespace

const LegacyCompositeOperation *
LegacyCompositePlan::Find(const CallNode *call) const {
  const auto it = operations.find(call);
  return it == operations.end() ? nullptr : &it->second;
}

LegacyCompositePlan AnalyzeLegacyComposites(const PrimFunc &function,
                                            const Target &target,
                                            const String &platform) {
  return CompositeCollector().Run(function, target, platform);
}

} // namespace tvm::tl::ascend_sync
