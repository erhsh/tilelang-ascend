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
constexpr const char *kAscendPipelineCostModel =
    "tl.ascend_pipeline_cost_model";
constexpr const char *kAscendS5TaskHint = "tl.ascend_s5_task";
constexpr const char *kAscendS5LoopId = "tl.ascend_s5_loop_id";
constexpr const char *kAscendS5Versions = "tl.ascend_s5_versions";
constexpr const char *kAscendS5BufferRemap = "tl.ascend_s5_buffer_remap";
constexpr const char *kAscendPersistentGuard = "tl.ascend_persistent_guard";
// Shared with the existing allocator; this is its compiler budget, not a new
// independently maintained scheduling capacity table.
constexpr int64_t kAscendA2A3UBBudget = 196352;

tir::PrimFunc PlanAscendConstraintPipeline(tir::PrimFunc function,
                                           tvm::transform::PassContext context);
Map<String, ObjectRef> AnalyzeAscendScheduleEffects(tir::PrimFunc function,
                                                    String platform);

} // namespace tl
} // namespace tvm
