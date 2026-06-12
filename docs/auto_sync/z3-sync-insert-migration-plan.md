# TileLang-Ascend 自动插同步 Z3 移植分析方案

> **背景**: 对比分析 TileLang GPU (`src/transform/thread_storage_sync.cc`) 和 TileLang-Ascend (`src/transform/ascend_sync_insert.cc`) 两套自动插同步机制，评估将 TileLang GPU 的 Z3 求解器能力移植到 TileLang-Ascend 的可行性、收益与实施计划。

---

## 一、现状对比分析

### 1.1 两套方案架构对照

| 维度 | TileLang-Ascend (NPU) | TileLang-GPU (CUDA) |
|------|----------------------|---------------------|
| **文件** | `src/transform/ascend_sync_insert.cc` | `src/transform/thread_storage_sync.cc` |
| **硬件模型** | 多管线 pipeline + 事件对 | 共享内存 + `__syncthreads` |
| **同步类型** | `PipeBarrier_{pipeline}` 或 `EventPair_{X_Y}` (30种) | 单一 `tvm_storage_sync(scope)` |
| **依赖分析** | `HasDataDependency()` 物理地址重叠 + 读写掩码 | `FindConflict()` Z3 求解器证明不相交 |
| **同步消除** | `SyncGraph` BFS 可达性 + Floyd-Warshall 传递闭包 | Z3 证明约束等价性 (双向蕴含) |
| **循环处理** | 物理展开 2 次迭代 → 线性分析 → 合并重建 | 符号化迭代偏移 (`loop_var+1`) + Z3 判断 |
| **线程模型** | 单核模型，不区分线程 | 区分 `tx/ty/tz`，不同变量建模跨线程 |
| **条件分支** | 保守策略：if 前后各插 `PipeBarrier_ALL` | `ConditionThreadPropertyChecker` 精判是否需 hoist |
| **执行架构** | 单次 IRMutator 遍历，同时分析 + 插入 | 两阶段：Planner 收集 sync 点 → Inserter 插入 |

### 1.2 TileLang-Ascend 当前的关键缺陷

| 场景 | 当前方案 | 缺陷类型 | 后果 |
|------|---------|---------|------|
| **For 循环** | 物理展开 2 次迭代 + 启发式合并 | 精度不足 | 丢失符号信息；无法检测距离>1的依赖；展开/合并代码约 400 行 |
| **If/Else** | 前后无条件各插 `PipeBarrier_ALL` | 过度保守 | Flash Attention 等大量分支算子产生冗余 barrier |
| **切片访问** | `is_sliced` 无条件触发 `PipeBarrier_ALL` | 粒度粗 | offset/extent 已知时仍插全量 barrier |
| **依赖检测** | `HasDataDependency()` 物理地址重叠 | 假阳性 | 同 buffer 不同切片也被判为重叠，插入冗余同步 |

---

## 二、Z3 求解器移植可行性

### 2.1 技术基础设施差距

| 组件 | TileLang-GPU | TileLang-Ascend | 差距 |
|------|-------------|-----------------|------|
| `arith::Analyzer::z3_prover` | ✅ 完整实现（`Z3Prover` 类） | ❌ 不存在 | 核心缺失 |
| `ConstrSet` / `ConstrVisitor` | ✅ 完整实现（`constr_visitor.h`） | ❌ 不存在 | 约束收集缺失 |
| Z3 库链接 | ✅ TVM 内置 | ❌ 未链接 | 编译配置缺失 |
| `arith::Analyzer::CanProve` | ✅ 可用 | ✅ 可用 | 可直接用 |
| `int_set` / `const_int_bound` | ✅ 可用 | ✅ 可用 | 可直接用 |

### 2.2 GPU 版本 Z3 的核心调用点

#### 调用点 1：约束等价性证明 (`thread_storage_sync.cc` L1661-1697)

```cpp
// 证明 prev_constr ⟺ curr_constr（双向蕴含）
// 如果成立 → 同一组线程执行两次访问 → 不需要跨线程 barrier

PrimExpr prev_constr = prev.cset.ToConjunction();  // 前一个访问的约束合取
PrimExpr curr_constr = curr.cset.ToConjunction();

bool prev_implies_curr = analyzer.z3_prover.CanProve(
    tirx::Or(tirx::Not(prev_constr), curr_constr));  // P => C
bool curr_implies_prev = analyzer.z3_prover.CanProve(
    tirx::Or(tirx::Not(curr_constr), prev_constr));  // C => P

if (prev_implies_curr && curr_implies_prev)
    return false;  // 约束等价，不需要同步
```

#### 调用点 2：符号化迭代偏移 (`thread_storage_sync.cc` L1626-1636)

```cpp
// 跨迭代分析: 将 curr 的 loop_var 替换为 loop_var+1 表示下一迭代
Map<Var, PrimExpr> loop_shift_sub;
loop_shift_sub.Set(loop->loop_var, loop->loop_var + step);

// 用 Z3 证明 prev_index(loop_var) != curr_index(loop_var+1) 在合法范围内恒成立
```

#### 调用点 3：指针访问不重叠检测 (`thread_storage_sync.cc` L1435-1480)

```cpp
// 两个 tvm_access_ptr 指向的字节范围是否有交集
bool provably_disjoint = analyzer.CanProve(
    lhs_max < rhs_min, arith::ProofStrength::kSymbolicBound);
```

#### 调用点 4：线程计数枚举 (`thread_storage_sync.cc` L352-353)

```cpp
// AllSAT：精确计算满足条件的线程数量（如 `tx % 4 == 0` → 32 个线程）
int64_t z3_count = analyzer_->z3_prover.CountSatisfyingValues(iv->var, extent);
```

### 2.3 技术路线选择

| 路线 | 方案 | 优点 | 风险 |
|------|------|------|------|
| **A（推荐）** | Backport `Z3Prover` 补丁到 Ascend TVM 子模块 | 与现有代码风格一致，`arith::Analyzer` 接口无缝衔接 | TVM 版本冲突 |
| **B（备选）** | 独立 Z3 封装，CMake 直接链接 Z3 库 | 不依赖 TVM 版本 | 需自行实现约束管理，代码量更大 |

### 2.4 Ascend 与 GPU 的语义差异（需注意）

| GPU 关注点 | Ascend 不适用原因 | 处理方式 |
|-----------|-----------------|---------|
| `tx/ty/tz` 线程变量建模 | 单核模型，无线程概念 | 去掉线程变量替换逻辑 |
| `is_atomic` 标记 | Ascend 上无 atomic 语义 | 去掉 |
| `is_async_copy` (TMA/cp.async) | Async 语义不同 | 适配为 Ascend 的 DMA 语义 |
| `shared.dyn` alias 分析 | Ascend 无动态共享内存 | 去掉 |
| **Z3 约束求解能力** | 通用，与硬件无关 | ✅ 直接可用 |

---

## 三、移植收益场景详解

### 3.1 场景一：For 循环 ⭐⭐⭐ 最深远的收益

#### 当前方案（`ascend_sync_insert.cc`）

**阶段 1**: `ForLoopUnroller`（L308-357）将每个循环展开为 2 次迭代的物理序列：

```
ForNode(loop_var, body) →
  AttrStmt("iteration_start", "loop_iter1")
  <body>                                    // iter1
  AttrStmt("iteration_end",   "loop_iter1")
  AttrStmt("iteration_start", "loop_iter2")
  <body>                                    // iter2
  AttrStmt("iteration_end",   "loop_iter2")
```

**阶段 2**: 标准同步插入逻辑将迭代当作线性语句处理，跨迭代依赖自然触发

**阶段 3**: `LoopRebuilder.MergeIterations`（L385-833，约 450 行代码）将两次迭代的同步取并集后重建 For

**缺陷**:
- 展开后丢失 `loop_var` 的符号信息
- 只能检测相邻 2 次迭代，漏掉距离 > 1 的依赖
- 合并启发式缺乏语义理解
- 代码复杂度约 450 行

#### Z3 方案：符号化迭代偏移

```cpp
// 例 1：循环内访问 A[i]，检查 A[i] vs A[i+1]（下一迭代）
prev_write_index = loop_var;        // 写：A[i]
curr_read_index = loop_var + 1;     // 读：A[i+1]（替换 loop_var → loop_var+1）

bool disjoint = analyzer.CanProve(loop_var != loop_var + 1);
// → 恒成立，可证不重叠，不插跨迭代同步

// 例 2：循环内访问 A[i]，下一迭代读 A[i-1]
prev_write_index = loop_var;        // 写：A[i]
curr_read_index = loop_var;         // 读：A[(i+1)-1] = A[i]（替换后）

bool overlap = !analyzer.CanProve(loop_var != loop_var);
// → 重叠，需要插跨迭代同步

// 例 3：复杂模运算缓冲
for (i = 0; i < N; i++) {
    buf[i % 2] = ...;   // 写
}
// 跨迭代：buf[i%2] vs buf[(i+1)%2]
// Z3 可证：((i+1)%2) == (1 - i%2) != (i%2) → 不重叠
```

**收益**:
- 高精度判断跨迭代依赖（精度从"中"提升到"高"）
- 支持任意距离（替换为 `loop_var+k`，覆盖距离>1场景）
- 消除展开/合并复杂代码，减少约 450 行

---

### 3.2 场景二：If/Else ⭐⭐⭐ 最直接的收益

#### 当前方案（`ascend_sync_insert.cc` L272-290）

```cpp
Stmt VisitStmt_(const IfThenElseNode *op) override {
    std::vector<Stmt> stmts;
    InsertSynchronization("PipeBarrier_ALL", stmts);  // if 前
    current_access_history_.clear();
    Stmt then_case = VisitStmt(op->then_case);
    // ...
    InsertSynchronization("PipeBarrier_ALL", stmts);  // if 后
    current_access_history_.clear();
}
```

**后果**：每个 if 无条件产生 2 个全量 barrier。Flash Attention 中大量分支导致显著性能损失。

#### Z3 方案：精判分支条件与 pipeline 冲突

```cpp
Stmt VisitStmt_(const IfThenElseNode *op) override {
    // 1. 收集 if 前的访问上下文
    auto pre_if_access = current_access_history_.back();

    // 2. 进入 then 分支，收集所有访问
    std::vector<BufferAccess> then_accesses;
    auto saved_history = current_access_history_;
    current_access_history_.clear();
    Stmt then_case = VisitStmt(op->then_case);

    // 3. Z3 证明 pre_if_access 写 vs then_accesses 读不冲突
    bool conflict_before = false;
    for (auto &then_acc : then_accesses) {
        if (then_acc.is_read && pre_if_access.is_write) {
            bool provably_disjoint = analyzer.CanProve(
                pre_if_access.byte_range.max < then_acc.byte_range.min ||
                then_acc.byte_range.max < pre_if_access.byte_range.min,
                arith::ProofStrength::kSymbolicBound
            );
            conflict_before = conflict_before || !provably_disjoint;
        }
    }

    // 4. 按需精确插入对应 pipeline 的 barrier（非全量）
    if (conflict_before) InsertSynchronization("PipeBarrier_PIPE_M", stmts);
    // ... body ...
    if (conflict_after) InsertSynchronization("PipeBarrier_PIPE_V", stmts);
}
```

**收益**:
- 预估 70-90% 的 if 可完全消除冗余 barrier
- 精确到具体 pipeline 的 barrier（而非全量 `PipeBarrier_ALL`）
- Flash Attention 等大量分支的算子显著受益

---

### 3.3 场景三：切片访问 ⭐⭐ 最容易实施

#### 当前方案（`ascend_sync_insert.cc` L155-158）

```cpp
// 任何标记为 is_sliced 的访问 → 无条件 PipeBarrier_ALL
if (current_access.is_sliced) {
    sync_requirements.push_back({"PipeBarrier_ALL", current_access.buffer_name});
}
```

#### Z3 方案：字节范围精确分析

```cpp
// 当前访问: tvm_access_ptr(buf, offset=0, extent=128)
// 历史访问: tvm_access_ptr(buf, offset=128, extent=128)

if (current_access.is_sliced) {
    auto curr_range = GetAccessByteRange(current_access);
    auto prev_range = GetAccessByteRange(latest_access);

    bool disjoint = analyzer.CanProve(
        curr_range.max < prev_range.min ||
        prev_range.max < curr_range.min,
        arith::ProofStrength::kSymbolicBound
    );

    if (!disjoint) {
        sync_requirements.push_back({"PipeBarrier_ALL", current_access.buffer_name});
    }
}
```

**收益**: 精确消除不重叠切片的冗余同步

---

### 3.4 场景四：依赖检测精度提升 ⭐⭐

#### 当前方案：`HasDataDependency()`（L1291-1317）

```cpp
// 只比较 buffer_name 和物理地址区间
bool shares_memory = (prev.buffer_name == curr.buffer_name) ||
                     (prev.physical_address < curr_end && curr.physical_address < prev_end);
```

**缺陷**: 同一 buffer 的不同切片（`buf[0:128]` vs `buf[128:256]`）被判为重叠（假阳性）

#### Z3 方案

```cpp
// 用符号化分析精确判断
bool overlap = !analyzer.CanProve(
    prev.byte_range.max < curr.byte_range.min ||
    curr.byte_range.max < prev.byte_range.min
);
```

---

## 四、预期收益总结

| 场景 | 当前方案 | Z3 增强后 | 改善幅度 |
|------|---------|----------|---------|
| **For 循环 - 相邻不重叠** | 物理展开（精度中）| 符号化证明 | 高精度 |
| **For 循环 - 跨多迭代** | 无法检测 | 可扩展任意距离 | 覆盖盲区 |
| **If/Else - 访问不冲突** | 插 2 个 `PipeBarrier_ALL` | 按需精确插入 | 预估 70-90% if 可消除 |
| **切片 - offset/extent 已知** | 无条件 `PipeBarrier_ALL` | 字节范围精确分析 | 精确消除 |
| **依赖检测** | 整体 buffer 粒度 | 符号化字节粒度 | 减少假阳性 |
| **代码复杂度** | 展开/合并约 450 行 | 符号化分析约 250 行 | -45% |
| **Flash Attention 性能** | 大量冗余 barrier | 分支同步精准 | 预估 5-15% 提升 |

---

## 五、实施计划

### 阶段零：验证阶段（1 周）

**目标**: 确认标准 `arith::Analyzer::CanProve()` 是否已满足需求

**任务**:
1. 编写测试用例验证标准 API 证明能力：
   - `i != i+1`（简单循环偏移）
   - `i%2 != (i+1)%2`（模运算）
   - 字节范围不重叠

2. 输出标准 API 覆盖度评估报告，决定是否需引入 Z3

### 阶段一：If/Else 同步优化（2-3 周）⭐ 优先

**目标**: 替代 `VisitStmt_(IfThenElseNode)` 中保守的 `PipeBarrier_ALL` 策略

**改造文件**: `src/transform/ascend_sync_insert.cc` L272-290

**步骤**:
1. 添加 if 前访问历史保存逻辑
2. 收集 then/else 分支的所有访问
3. 用 `CanProve` 证明字节范围不重叠 → 按需插入 barrier
4. Z3 失败时回退保守策略（保证正确性）
5. 编写回归测试，验证 Flash Attention 等算子

**验收标准**: Flash Attention 等算子的 barrier 数量减少 50%+

### 阶段二：切片精确分析（1-2 周）⭐ 容易实施

**目标**: 替代 `is_sliced` 的无条件 `PipeBarrier_ALL`（L155-158）

**步骤**:
1. 提取 `tvm_access_ptr` 的 `offset`/`extent`
2. 用 `CanProve` 证明字节范围不重叠
3. Z3 失败时回退保守策略

**验收标准**: 切片访问场景的 barrier 数量减少 30%+

### 阶段三：符号化跨迭代分析（3-4 周）⭐ 收益最深远

**目标**: 替代物理展开方案

**改造内容**:
- 移除 `ForLoopUnroller`（L308-357）
- 移除 `LoopRebuilder`（L385-833）
- 添加符号化迭代偏移分析逻辑

**步骤**:
1. 收集循环体内访问的 index 表达式
2. 用 `loop_var → loop_var+1` 替换后与原始表达式比较
3. 用 Z3/CanProve 证明不重叠
4. 保留当前展开方案作为 fallback

**验收标准**: 所有循环测试通过，代码行数减少 40%+

---

## 六、风险评估与缓解

| 风险 | 严重度 | 概率 | 缓解措施 |
|------|--------|------|---------|
| **编译性能下降**（Z3 求解耗时） | 中 | 中 | `SetTimeoutMs(50)` 超时回退保守策略；缓存已求解的约束对 |
| **Z3 求解超时** | 中 | 中 | `SetRLimit` 限制资源消耗；限制复杂表达式 |
| **TVM 子模块版本冲突** | 高 | 中 | 优先评估 GPU TVM 补丁兼容性；不可行时选路线 B |
| **正确性回退** | 高 | 低 | Z3 失败时回退保守策略；每阶段编写回归测试验证 |
| **Ascend 单线程模型差异** | 低 | 高 | 去掉 GPU 的线程变量替换逻辑，仅复用 Z3 约束求解能力 |
| **依赖链复杂** | 中 | 低 | 分阶段实施，每阶段独立验证 |

---

## 七、结论

### 整体评估：**部分可行，推荐渐进式移植**

### 核心结论

| 场景 | 可行性 | 价值 | 实施难度 |
|------|--------|------|---------|
| If/Else 同步消除 | ✅ 高 | ⭐⭐⭐ 最高 | 低 |
| 切片精确分析 | ✅ 高 | ⭐⭐ 中高 | 低 |
| 符号化跨迭代 | ✅ 高 | ⭐⭐ 中 | 中 |
| 依赖检测精度提升 | ✅ 高 | ⭐⭐ 中 | 低 |
| 完整 Z3 集成 | ⚠️ 需先解决 TVM 基础设施 | - | 高 |

### 推荐执行路径

```
Week 1      验证标准 CanProve 能力，评估是否需引入 Z3
Week 2-4    阶段一：If/Else 同步优化（Flash Attention 等重度受益）
Week 5-6    阶段二：切片精确分析（实现简单，收益清晰）
Week 7-10   阶段三：符号化跨迭代分析（替代物理展开，可选）
```

**关键原则**:
1. **先验证后实施**: 每一步先验证标准 `arith::Analyzer::CanProve()` 的证明能力
2. **渐进式移植**: 先高价值场景（If/Else），再易实施场景（切片），最后深收益场景（循环）
3. **保守策略兜底**: Z3 求解失败或超时时，自动回退到当前保守策略，保证正确性优先
4. **性能安全阈**: `SetTimeoutMs(50)` 防止 Z3 拖慢编译

---

## 附录

### A. 关键代码位置索引

| 文件 | 行号范围 | 内容 |
|------|---------|------|
| `ascend_sync_insert.cc` | L150-196 | `VisitStmt_(EvaluateNode*)`: 核心同步插入逻辑 |
| `ascend_sync_insert.cc` | L272-290 | `VisitStmt_(IfThenElseNode)`: 保守全量 barrier |
| `ascend_sync_insert.cc` | L308-357 | `ForLoopUnroller`: 循环展开为 2 次迭代 |
| `ascend_sync_insert.cc` | L385-833 | `LoopRebuilder`: 合并两次迭代并重建 For |
| `ascend_sync_insert.cc` | L1291-1317 | `HasDataDependency()`: 物理地址重叠检测 |
| `ascend_sync_insert.cc` | L1432-1476 | `OptimizeSyncRequirements()`: SyncGraph 消除冗余 |
| `ascend_sync_insert.cc` | L835-935 | `SyncGraph`: 有向图数据结构 |
| `thread_storage_sync.cc` | L1586-1851 | `FindConflict()`: Z3 冲突检测核心 |
| `thread_storage_sync.cc` | L1435-1480 | `PointerAccessIsDisjoint()`: 指针访问不重叠检测 |
| `thread_storage_sync.cc` | L1197-1382 | `Summarize()`: 含循环依赖分析的摘要逻辑 |

### B. 术语表

| 术语 | 含义 |
|------|------|
| `PipeBarrier` | 同管线内的 barrier（如 `PIPE_ALL`、`PIPE_M`、`PIPE_V`） |
| `EventPair` | 跨管线的同步事件对（`set_flag` + `wait_flag`，30 种组合） |
| `SyncGraph` | 有向图，跟踪 pipeline 间已有的 event 同步关系 |
| `WAW/RAW/WAR` | Write-After-Write / Read-After-Write / Write-After-Read 数据依赖 |
| `RAR` | Read-After-Read（不冲突，无同步需求） |
| `tvm_access_ptr` | TIR 中标记 buffer 访问范围的内建调用 |
| `ConstrSet` | 约束集合，收集从语句入口到访问点的 if 条件、循环范围等上下文 |
| `Floyd-Warshall 传递闭包` | 计算图中所有节点对的可达性，用于消除冗余同步 |
