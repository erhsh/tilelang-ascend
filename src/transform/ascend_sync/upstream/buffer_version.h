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

// Ported from tile-ai/tilelang 1c0f402e166a7d0a055c991d2f82d2175a7917b7.
// Source: src/ascend/transform/buffer_version.h. Legacy adaptation: version
// counts are pass-local integers; TIR attribute serialization is
// importer-owned.

#pragma once

#include <tvm/node/structural_equal.h>
#include <tvm/runtime/container/array.h>
#include <tvm/runtime/container/map.h>
#include <tvm/runtime/logging.h>
#include <tvm/tir/expr.h>

#include <optional>
#include <unordered_map>
#include <utility>

namespace tvm::tl::ascend_sync {

inline constexpr const char *kBufferVersionsMap = "tl.buffer_versions_map";
inline constexpr const char *kManualMultiBuffer = "tl.manual_multi_buffer";
// kMultiBufferEligible belongs to ir_structure.h; do not register it twice.

// Legacy TVM Map requires ObjectRef values. Keep the upstream integer-valued
// model local to analysis, with its small Get/Set API preserved for consumers.
class BufferVersionMap
    : public std::unordered_map<tir::Var, int, ObjectPtrHash, ObjectPtrEqual> {
public:
  using Base = std::unordered_map<tir::Var, int, ObjectPtrHash, ObjectPtrEqual>;
  using Base::Base;

  std::optional<int> Get(const tir::Var &storage) const {
    auto it = find(storage);
    return it == end() ? std::nullopt : std::optional<int>(it->second);
  }
  void Set(const tir::Var &storage, int versions) {
    ICHECK_GT(versions, 0) << "Buffer version counts must be positive";
    insert_or_assign(storage, versions);
  }
};

using L0SFBindings = Map<tir::Var, tir::Var>;

// Data and scale-factor storage keep separate logical accesses. Only physical
// allocation/owner metadata uses their common representative, as upstream.
class L0StorageGroups {
public:
  explicit L0StorageGroups(L0SFBindings bindings = {})
      : bindings_(std::move(bindings)) {
    for (const auto &[sf, data] : bindings_)
      scales_.Set(data, sf);
  }

  tir::Var Representative(const tir::Var &storage) const {
    return bindings_.Get(storage).value_or(storage);
  }
  Array<tir::Var> Members(const tir::Var &storage) const {
    tir::Var data = Representative(storage);
    if (auto sf = scales_.Get(data))
      return {data, sf.value()};
    return {data};
  }
  const L0SFBindings &Bindings() const { return bindings_; }

private:
  L0SFBindings bindings_;
  L0SFBindings scales_;
};

inline BufferVersionMap
ExpandL0StorageGroupValues(BufferVersionMap values,
                           const L0StorageGroups &groups,
                           const char *description) {
  auto original = values;
  for (const auto &[storage, value] : original) {
    for (const tir::Var &member : groups.Members(storage)) {
      if (auto existing = values.Get(member)) {
        ICHECK_EQ(existing.value(), value)
            << "Conflicting " << description << " for bound L0 data/SF group "
            << groups.Representative(storage)->name_hint;
      }
      values.Set(member, value);
    }
  }
  return values;
}

template <typename T>
Map<tir::Var, T> ExpandL0StorageGroupValues(Map<tir::Var, T> values,
                                            const L0StorageGroups &groups,
                                            const char *description) {
  auto original = values;
  for (const auto &[storage, value] : original) {
    for (const tir::Var &member : groups.Members(storage)) {
      if (auto existing = values.Get(member)) {
        ICHECK(StructuralEqual()(existing.value(), value))
            << "Conflicting " << description << " for bound L0 data/SF group "
            << groups.Representative(storage)->name_hint;
      }
      values.Set(member, value);
    }
  }
  return values;
}

} // namespace tvm::tl::ascend_sync
