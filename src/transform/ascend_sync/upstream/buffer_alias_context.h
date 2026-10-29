/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

// Extracted from tile-ai/tilelang 1c0f402e166a7d0a055c991d2f82d2175a7917b7.
// Source: src/ascend/transform/auto_schedule/buffer_alias_analysis.h:56-79.
// Synchronization owns this query interface independently of address reuse.
// The legacy adapter does not invoke the upstream post-sync alias planner.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ir_structure.h"

namespace tvm::tl::ascend_sync {

// A candidate address period, which still needs a region-periodicity proof.
struct StorageIterationPeriod {
  int64_t iterations{1};
  bool implicit_slots{false};
};

// Scope-aware view of guaranteed synchronization. The context owns site and
// clock identities and must outlive the BufferAliasAnalyzer that reads it.
class BufferAliasAnalysisContext {
public:
  virtual ~BufferAliasAnalysisContext() = default;

  virtual std::vector<size_t>
  ResolveLifetimeEndpoints(const tir::Var &storage, TaskNode *task,
                           bool at_beginning, ControlNode *scope) const = 0;
  virtual bool HappensBefore(ControlNode *scope, size_t src, size_t dst,
                             int64_t distance) const = 0;
  virtual std::optional<int64_t> MinimumDistance(ControlNode *scope, size_t src,
                                                 size_t dst) const = 0;
  virtual std::optional<StorageIterationPeriod>
  StoragePeriod(const tir::Var &storage, ControlNode *scope) const = 0;
  virtual size_t PromoteLifetimeSite(const tir::Var &storage, size_t site_id,
                                     ControlNode *control) const = 0;
};

} // namespace tvm::tl::ascend_sync
