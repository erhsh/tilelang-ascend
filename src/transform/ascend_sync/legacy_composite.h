// Copyright (c) Tile-AI Organization.
// Licensed under the MIT License.

#pragma once

#include <tvm/target/target.h>
#include <tvm/tir/function.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "upstream/ir_structure.h"

namespace tvm::tl::ascend_sync {

struct LegacyCompositeBufferEffect {
  // A/B/C order. Keep the original pointer and its element units; the physical
  // resolver conservatively covers its complete allocation, including padding.
  tir::Call access_ptr;
  bool read{false};
  bool write{false};
  // Set only by the verified static Cube pipeline audit. Actual pointers are
  // independently checked against their clock-selected slot by the resolver.
  Optional<tir::Var> logical_ring;
  int64_t required_bytes{0};
};

struct LegacyCompositeOperation {
  tir::Call call;
  CoreMask execution_owner{kCoreCube};
  ResourcePipe acquire{ResourcePipe::kMTE1};
  ResourcePipe completion{ResourcePipe::kCube};
  std::vector<LegacyCompositeBufferEffect> buffers;
  int64_t m{0};
  int64_t n{0};
  int64_t k{0};
  int64_t k_l0{0};
  Optional<PrimExpr> pipeline_clock;
};

struct LegacyCompositePlan {
  // Device helper protocols differ even though the typed TIR ABI is shared.
  // Keep the audited profile explicit; the adapter's acquire/completion API
  // and the upstream engine remain unchanged.
  enum class HelperProfile { kAscendC, kPtoA3Flat };
  HelperProfile helper_profile{HelperProfile::kAscendC};
  std::unordered_map<const tir::CallNode *, LegacyCompositeOperation>
      operations;
  // Reserved for the complete function, including helper entry/exit and both
  // ping-pong slots. Existing manual events must not use these IDs either.
  std::map<std::string, std::set<int>> reserved_intra_ids;
  // Internal analysis result, never a public attribute admission switch.
  bool static_cube_pipeline{false};
  Optional<tir::Var> cube_pipeline_loop;
  std::unordered_map<const tir::CallNode *, std::pair<tir::Var, int64_t>>
      cube_ring_copies;

  const LegacyCompositeOperation *Find(const tir::CallNode *call) const;
};

// Audit typed gemm_v0 without rewriting or duplicating the calculation. The
// contract supports flat AscendC/A3 calls, or one static positive serial K loop
// containing full A/B GM-to-L1 refreshes and one gemm(clear=k==0), or one
// audited static physical SIP (trip>=3, actual lead1, independent L1 A/B rings
// B2, aligned fp16 tiles, no transpose/internal K splitting, one final FIX).
// Shapes are static and user L0A/B storage is excluded. Other nested/dynamic
// For, If/While, loop-internal FIX, rings and helper-owned manual event
// collisions are rejected. A gemm's execution
// owner is Cube; this does not assert support for mixed C/V functions as a
// whole. Functions without composite helpers return an empty plan unchanged.
// PTO/A3 has a separate flat profile: one FP16->FP32 M=N=16, K=128/256,
// kL0Size=128, clear=true, n_actual=N, no transpose/base-pointer B1 call,
// complete A/B GM refreshes and final FIX. Its hidden single L0 slot and six
// event directions use ID1; AscendC's ping-pong reservations are not reused.
LegacyCompositePlan AnalyzeLegacyComposites(const tir::PrimFunc &function,
                                            const Target &target,
                                            const String &platform);

} // namespace tvm::tl::ascend_sync
