// Copyright (c) Tile-AI Corporation.
// Licensed under the MIT License.
#pragma once

#include <tvm/ir/transform.h>
#include <tvm/tir/function.h>

namespace tvm {
namespace tl {

constexpr const char *kAscendPipelinePlanner = "tl.ascend_pipeline_planner";
constexpr const char *kAscendS4Task = "tl.ascend_s4_task";
constexpr const char *kAscendS4Iter = "tl.ascend_s4_iter";
constexpr const char *kAscendS4Manifest = "tl.ascend_s4_manifest";
// Shared with the existing allocator; this is its compiler budget, not a new
// independently maintained scheduling capacity table.
constexpr int64_t kAscendA2A3UBBudget = 196352;

tir::PrimFunc PlanAscendConstraintPipeline(tir::PrimFunc function,
                                           tvm::transform::PassContext context);
Map<String, ObjectRef> AnalyzeAscendScheduleEffects(tir::PrimFunc function,
                                                    String platform);

} // namespace tl
} // namespace tvm
