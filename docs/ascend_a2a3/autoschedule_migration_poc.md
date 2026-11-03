# A2/A3 复用主线 A5 自动调度：POC 技术验证结论

分支：`feat/a2a3-reuse-a5-autosync`
探针脚本：`/tmp/opencode/probe_autoschedule.py`

## 背景与目标

验证 A2/A3 后端（`tilelang/ascend/a2a3`、`src/ascend/a2a3`）能否复用主线
A5 的自动调度器及其 `InsertSync` 同步插入 pass（
`src/ascend/transform/auto_schedule/*`、`insert_sync.cc`）来完成自动插同步。

## 方法

A5 的自动调度 pass 已经被编译进共享的 `libtilelang.so`（父级
`src/ascend/CMakeLists.txt` 通过 `file(GLOB)` 把 `src/ascend/transform/*.cc`
和 `.../auto_schedule/*.cc` 全部纳入），并以 `tl.transform.*` 注册。于是给
A2/A3 的流水线加了一个由 `PassConfigKey.TL_ENABLE_AUTO_SCHEDULE`（默认
`False`）控制的可选分支，逐字调用 A5 链：

```
NormalizeControlFlowForSchedule -> NormalizeConflictHints
-> MaterializeScheduleUnits -> AssignCore -> AnnotateMultiBufferEligible
-> EstimateLatency -> AutoSchedule -> PrepareMultiBuffer -> ResolveCore
-> InsertSync -> MaterializeMultiBuffer -> LowerScheduledTIR
-> RestoreWhileLoops
```

这只是技术验证探针（见 `tilelang/ascend/a2a3/pipeline.py` 中的
`_lower_a2a3_autoschedule`），并非生产路径。

## 结论一：整套搬迁被方言边界硬阻塞

对 A2/A3 的 `gemm_v0` kernel（`T.Scope("C")`、`T.Pipelined`）开启
`TL_ENABLE_AUTO_SCHEDULE=True` 编译，在第一个实质性 pass 即失败：

```
MaterializeScheduleUnits: Check failed: (IsScheduleGuardAttribute(op->attr_key))
  is false: ... Unsupported attr_key: resource_scope
```

- `T.Scope("C"/"V")` 在 eager trace 阶段就产生 `resource_scope` 的
  `AttrStmt`（`src/ascend/a2a3/ir.cc:240`）：`Scope("C") -> 0`、`Scope("V")
  -> 1`。这是 TA 方言的 cube/vector 分离语义，贯穿整个 lowering（
  `lower_tile_op`、`ascend_sync_insert`、`combinecv` 都消费它）。
- A5 的 scheduled-TIR 编解码器只认可 `tilelang_assume` /
  `kAssumeRequiresRuntimeCheck` 两种调度 guard（
  `src/ascend/transform/auto_schedule/scheduled_tir.h:62`），完全没有
  `resource_scope` 概念。

因此，整套搬迁需要先把 TA 的 `T.Scope` CV 模型翻译成 A5 的 `T.Stage` +
`tl.schedule_unit` + `tl.vf_latency` 模型，这等价于重写 A2/A3 的调度前端，
超出范围，也违背"不改主线"的约束。

### 重写前后的差异（以 GEMM 为例）

**重写前（a2a3 `gemm_v0_pipeline.py`，TA 方言）**

```python
with T.Kernel(m_num * n_num, is_npu=True) as (cid, vid):
    with T.Scope("C"):                       # ① cube/vec 分离、资源标注
        for k in T.Pipelined(loop_k, num_stages=3):   # ② 显式多 buffer
            T.barrier_all()                  # ③ 手写同步
            T.copy(A[bx*block_M, k*block_K], A_L1)
            T.copy(B[k*block_K, by*block_N], B_L1)
            if k == 0:
                T.gemm_v0(A_L1, B_L1, C_L0, init=True)  # ④ TA 独有 op
            else:
                T.gemm_v0(A_L1, B_L1, C_L0)
            T.barrier_all()
        T.copy(C_L0, C[...])
```

**重写后（A5 `example_gemm.py`，同一逻辑的等价形态）**

```python
with T.Kernel(NUM_BLOCKS) as bx:
    res = T.alloc_l0c(...)
    for tile_idx in T.Persistent([OUT_TILES], NUM_BLOCKS, bx):
        for kt in T.Pipelined(K_TILES, num_stages=NUM_STAGES):
            with T.Task():                              # ⑤ 调度任务边界
                T.copy(X[...], x_l1)
            with T.Task():
                T.copy(W[...], w_l1)
            with T.Task():
                T.gemm(x_l1, w_l1, res, transpose_B=True,
                       clear_accum=kt == 0, ...)        # ⑥ A5 的 T.gemm
```

五个差异点的对应关系：

| # | 现状（a2a3） | 重写后（A5） | 本质差异 |
|---|---|---|---|
| ① | `T.Scope("C"/"V")` → `Attr(resource_scope=0/1)` | `T.Task()`/`T.PerCoreTask()` → `tl.schedule_unit` | CV 分离的 IR 载体不同，**正是 `MaterializeScheduleUnits` 崩溃点** |
| ② | `T.Pipelined(num_stages=3)` 由 `InjectSoftwarePipeline` 显式展开成 3 份 buffer | `AutoSchedule` Z3 求解 `tl.buffer_versions_map`，`Prepare/MaterializeMultiBuffer` 再展开 | 多 buffer 是"写死"还是"自动求解" |
| ③ | 手写 `T.barrier_all()`，最后靠 `AscendSyncInsert` 补齐 | 不手写，靠 `InsertSync` + `AssignCore`/`ResolveCore` 自动插入 | 同步的生成算法 |
| ④/⑥ | `T.gemm_v0`（TA 语义） | `T.gemm`（不同参数/语义） | op 也不通用 |
| ⑤ | 无（TA 方言没有任务边界） | `T.Task` + 可选 `T.Stage(n)` + `tl.vf_latency`（`SimtVF/SimdVf`） | 调度单元、阶段、延迟模型 |

**IR 产物层的差异**

重写前（`MaterializeScheduleUnits` 在这里崩溃）：

```
Attr(resource_scope=0) {            # A5 只认 assume/guard，不认这个
  For k in [0, 3):                   # InjectSoftwarePipeline 已显式三分 buffer
    copy A -> A_L1_{0,1,2}
    copy B -> B_L1_{0,1,2}
    gemm_v0 A_L1_x, B_L1_x -> C_L0
  ...
}
```

重写后的 A5 scheduled-TIR 形态：

```
tilelang_root {
  tl.schedule_unit { Task<MTE2>: copy X -> x_l1 }    # 每个 unit 一个 task
  tl.schedule_unit { Task<MTE2>: copy W -> w_l1 }
  tl.schedule_unit { Task<CUBE>: gemm x_l1,w_l1 -> res }
  ...  # 另有 stage 元数据 + tl.buffer_versions_map + core 归属
}
```

**所以"重写"不是改几个 pass 名，而是换掉整条生产 IR 的前端合约**：

1. 去掉 `resource_scope`：`T.Scope("C"/"V")` 的 cube/vec 语义要么改造成
   `T.Task`/`T.PerCoreTask` + `AssignCore` 的 AIC/AIV 归属，要么给 A5 codec
   加 `resource_scope` 支持（后者动了主线）。
2. 去掉 `InjectSoftwarePipeline` 显式多 buffer，换 `AutoSchedule` 求解。
3. 补上调度输入标注：`T.Stage`、`Task(latency/ii)`、`tl.vf_latency`——这些
   a2a3 完全没有，而 `EstimateLatency`/`AutoSchedule` 强依赖。
4. 同步 pass 从 `AscendSyncInsert`（TIRX 上）换 `InsertSync`（ScheduledTIR
   /IRStructure 树上），连带 op 约定、跨核 flag bank。

即让 A2/A3 的 kernel 作者换一套方言编程，并重写方言到 IR 的 lowering 前端；
工作量等同于"新建一个 A5 风格的 A2/A3 前端"，而非"补一个 pass"。

## 结论二：能力对齐收益有限（同步 vs 异步模型）

A5 的 `FlagAllocator`（基于 `lifetime_begin/end` + `CanReuse` 的生命周期复用）、
跨迭代分类（`edge.distance`）、以及跨核 16×2 flag bank，**全部**是为 A5 的
**异步**模型服务的——在异步模型里 `set_flag`/`wait_flag` 相隔很多语句，flag
需要跨语句存活。

而 A2/A3 的两个 pass 都把每个 `EventPair` 发射成**紧邻的 set+wait 对**（两个
`ascend_sync_insert.cc` / `ascend_sync_insert_vs.cc` 里的
`InsertSynchronization`），因此每个 flag 的生命周期是瞬时的。由于 TA 方言通过
`T.Pipelined` + `InjectSoftwarePipeline` **显式**管理流水，这种同步式模型已足够。

## 结论三：跨迭代 flag 复用是安全的（无 bug）

对循环携带（back-edge）依赖下的 `% 8` 轮转 `AllocateEventId` 的审计：

- `AscendSyncInsert` 把每个循环展开成两次合成迭代（`ForLoopUnroller`，iter1
  /iter2），通过 `first_iteration_history` 发现 back-edge；
  `LoopRebuilder::MergeSyncSequences` 再用 LCS 把它们合并回循环。
- `AscendSyncInsertVS` 用两遍遍历的 `is_revisit_pass_`，把循环出口历史标记为
  `is_back_edge`。
- 两者中 `set_flag` 与 `wait_flag` 始终紧邻插入，flag 生命周期瞬时。即便某处
  需要超过 8 种不同 event pair（触发 `% 8` 回绕），前一对的 `wait` 已完成，
  复用也安全。back-edge 的 set/wait 依赖 AscendC 同 pipe 顺序发射语义，每个
  迭代仍是"先 set 后立即 wait"。

因此，把 A5 的生命周期感知 flag 分配移植进 A2/A3 不会修复任何正确性问题——
它只对 A5 的异步模型有意义。

## 总结

1. **整套复用 A5 自动调度不可行**：被 `resource_scope` 与 scheduled-TIR guard
   之间的方言鸿沟阻塞，除非重写 A2/A3 方言调度模型。
2. A5 的大部分同步能力是其异步模型专属，对 A2/A3 同步模型**没有直接收益**。
3. A2/A3 的 `% 8` flag 分配在其模型下**是正确的**，无需以正确性为由移植。

`TL_ENABLE_AUTO_SCHEDULE` 探针分支及其 pass 链清单保留在
`feat/a2a3-reuse-a5-autosync` 分支上作为证据，它们不是生产路径。
