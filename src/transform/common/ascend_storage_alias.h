// Copyright (c) Tile-AI Corporation.
// Licensed under the MIT License.
#pragma once

#include <tvm/arith/analyzer.h>
#include <tvm/tir/function.h>

namespace tvm {
namespace tl {

constexpr const char *kAscendS6AliasProvenance =
    "tl.ascend_s6_alias_provenance";
constexpr const char *kAscendS6ViewProvenance = "tl.ascend_s6_view_provenance";
constexpr const char *kAscendS6SourceData = "tl.ascend_s6_source_data";
constexpr const char *kAscendS6OriginalParameters =
    "tl.ascend_s6_original_parameters";

// Internal COW helper; the public pass invokes it only in constraint mode.
tir::PrimFunc CanonicalizeAscendStorageAliases(tir::PrimFunc function);
Map<String, ObjectRef>
BuildAscendStorageCatalog(tir::PrimFunc function,
                          Map<String, ObjectRef> canonical_graph);
Map<String, ObjectRef> AnalyzeAscendStorageOwners(tir::PrimFunc function,
                                                  String platform);
tir::PrimFunc RemapAscendStorageMetadata(tir::PrimFunc function,
                                         Map<tir::Var, tir::Var> replacements);
PrimExpr CheckedAscendStorageByteOffset(PrimExpr elements, int64_t bytes);
PrimExpr AddAscendStorageByteOffsets(PrimExpr left, PrimExpr right);
PrimExpr PromoteAscendStorageIndex(PrimExpr expression,
                                   Map<tir::Var, Range> proof_domains = {});

} // namespace tl
} // namespace tvm
