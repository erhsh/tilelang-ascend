# ascend_sync_insert.cc 自动同步插入分析

## 实现步骤梳理

### 1. 初始化配置
- **Lines 52-81**: 从 `PassContext` 读取 `tl.ascend_auto_sync` 开关
- **Lines 114-126**: 从 PrimFunc 获取 `address_map` 和 `size_map`,加载操作配置表 (`OperationConfig`) 和事件映射表 (`event_mapping_`)

### 2. For 循环预处理 (`ForLoopUnroller`)
- **Lines 299-383**: 将每个 `ForNode` 展开为 2 次迭代 (iter1/iter2)
- 用 `iteration_start/iteration_end` AttrStmt 标记边界
- 用 `unrolled_loop` AttrStmt 包裹,以便后续识别和恢复

### 3. 同步分析与插入 (核心 IRMutator)
- **`VisitStmt_(EvaluateNode)`** (150-196): 分析 `call_extern` 的操作,提取 buffer 访问信息
- **`AnalyzeStmtAccesses`** (1118-1236): 根据 `operation_config_` 提取每个 buffer 的读写类型、所属 pipeline
- **`FindRelatedBuffers`** (1380-1430): 通过物理地址+size 查找共享内存的 buffer
- **`HasDataDependency`** (1291-1317): 检测 WAW/RAW/WAR 依赖
- **`GetRequiredSyncType`** (1326-1340): 判断需要 `PipeBarrier` 还是 `EventPair`
- **`OptimizeSyncRequirements`** (1432-1476): 去重 + 同步图覆盖检测
- **`InsertSynchronization`** (1527-1544): 生成 `ascend_auto_barrier/set_flag/wait_flag`
- **`UpdateSyncStatesAfterSync`** (1500-1525): 更新同步图的传递闭包

### 4. 循环重建 (`LoopRebuilder`)
- **Lines 385-833**: 识别 `unrolled_loop` AttrStmt,恢复原始 `ForNode`
- **`MergeIterations`** (446-508): 合并两次迭代的同步语句
- **`MergeStatementSequences`** (559-634): 提取执行语句,合并前后所需同步

---

## 已实现的功能点

| 功能 | 说明 |
|------|------|
| For 循环展开/重建 | `ForLoopUnroller` + `LoopRebuilder` |
| Buffer 访问分析 | 读写类型、Pipeline、物理地址提取 |
| 数据依赖检测 | WAW/RAW/WAR 三种依赖类型 |
| 物理地址重叠检测 | 通过 `address_map + size_map` 计算区间交集 |
| 同步图 (SyncGraph) | BFS 路径查询 + 传递闭包合并 |
| PipeBarrier 插入 | 按 Pipeline 粒度 |
| EventPair 插入 | SetFlag + WaitFlag 配对 |
| IfThenElse 同步 | 无条件插入 `PipeBarrier_ALL` |
| 资源作用域隔离 | `resource_scope` AttrStmt 时清空访问历史 |
| A5 平台特异性 | 跳过 `PIPE_V` 的 PipeBarrier |
| Op 类 API 支持 | `call_intrin` (如 `tl.ascend_mma`) 也支持 |

---

## 缺失/问题点

| 问题 | 位置 | 说明 |
|------|------|------|
| **循环展开固定 2 次** | 326-342 | 每个 For 循环都被展开为 2 份迭代体来捕获跨迭代同步需求。当 `extent>2` 时合并结果作为单次循环体执行 `extent` 次,逻辑正确。但当 `extent=1` 时不需要跨迭代同步,合并后可能多插同步指令 |
| **事件 ID 循环模 8,无防冲突机制** | 1546-1549 | `AllocateEventId()` 在 0-7 之间循环分配,不追踪事件何时被释放。若同时活跃的 EventPair 超过 8 个会复用 ID 导致冲突 |
| **IfThenElse 的 PipeBarrier_ALL 不追踪** | 273-289 | `IfThenElse` 前后无条件插入 `PipeBarrier_ALL`,但插入后没有调用 `UpdateSyncStatesAfterSync`,导致后续操作无法感知已同步而可能产生冗余 barrier |
| **BufferStore 只记录不检查依赖** | 198-215 | `VisitStmt_(BufferStoreNode)` 仅调用 `UpdateLatestAccessHistory`,不检查依赖也不插入同步。对于标量写操作合理,但依赖会在后续读操作中由读取方的分析路径触发 |
| **LetStmt 中 BufferLoad 全标记 sliced** | 1048-1059 | `ExprAccessAnalyzer::VisitExpr_(BufferLoadNode)` 将所有 BufferLoad 无条件标记为 sliced,导致 LetStmt 前必插 `PipeBarrier_ALL`。实际影响限于 LetStmt 路径(主路径 EvaluateNode 不经过此逻辑) |
| **不区分 ForKind,统一展开** | 308 | `ForLoopUnroller` 对所有 For 循环一视同仁,包括 `kThreadBinding` (如 `threadIdx.x`) 类型的循环。并行线程循环的跨迭代同步语义不同,可能产生不正确的同步 |
| **EventPair 的 set_flag/wait_flag 紧挨插入** | 1541-1542 | `InsertSynchronization` 中每次 EventPair 都是 set_flag 紧接 wait_flag。可保证正确性,但可能引入不必要的流水线 stall |
| **BlockNode/BlockRealizeNode 无覆写** | — | 没有 `VisitStmt_(BlockNode)` 和 `VisitStmt_(BlockRealizeNode)` 覆写。若此 pass 运行在 BlockRealize 已 lower 后的阶段则不受影响;否则 Block 内的操作会被跳过 |
| **StmtFlattener 无条件为 BufferStore 插入 PIPE_ALL barrier** | 823-827 | `StmtFlattener::VisitStmt_(BufferStoreNode)` 在展开阶段就硬编码插入了 `PIPE_ALL` barrier,该 barrier 会作为普通同步参与后续合并去重,最终在循环体中产生冗余的全管道 barrier |
| **OptimizeSyncRequirements 只优化 EventPair 类型** | 1432-1498 | `IsSyncSatisfiedByGraph` 只处理 `EventPair_` 前缀的同步类型,对 `PipeBarrier_` 和 `PipeBarrier_ALL` 直接返回 `false`。虽然 `UpdateSyncStatesAfterSync` 维护了 `pipe_barriers` 字段,但该字段从未在优化阶段被查询,导致冗余 PipeBarrier 无法去重 |
| **cross-flag 自动插入未实现** | — | `ascend.h` 中定义了 `ascend_auto_set_cross_flag` / `ascend_auto_wait_cross_flag`,但 sync insert 代码未使用。cross-flag 目前仍需手动指定 |
| **循环结束后访问历史未清理** | — | `ForNode` 处理完 body 后(在 `MergeIterations` 返回时),没有清空 `current_access_history_`,导致循环内部的访问记录泄漏到外层作用域,可能在后续操作中产生不必要的同步 |
