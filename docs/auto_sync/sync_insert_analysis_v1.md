# 自动同步插入全景分析

## 一、背景与目标

### 1.1 为什么需要自动同步

Ascend NPU 采用多核异构架构，硬件上存在多个并行流水线（PIPE_MTE1/MTE2/MTE3/PIPE_M/PIPE_V/PIPE_S/PIPE_FIX），这些流水线之间通过**显式同步指令**协调执行：

- **PipeBarrier**：同一流水线内的屏障同步（如等待前一条 MTE 指令完成）
- **EventPair（SetFlag/WaitFlag）**：跨流水线的异步同步机制（如 Cube 计算完成后通知 Vec 开始计算）

手动编写同步代码存在以下痛点：
1. **易出错**：流水线间依赖关系复杂，容易漏插或错插
2. **维护成本高**：算子逻辑变更时同步代码需要同步调整
3. **性能调优难**：同步指令的粒度和位置直接影响流水线并行度

### 1.2 自动同步的目标

```
用户代码（无同步）  →  自动分析数据依赖  →  自动插入最优同步  →  正确的并行执行代码
```

核心原则：
- **正确性优先**：确保所有数据依赖都被正确同步，不遗漏
- **性能尽量优**：避免过度同步，保持流水线并行度
- **对用户透明**：用户无需关心同步细节

---

## 二、Pass 流水线总览

### 2.1 Pass 执行顺序

```
OptimizeForTarget（phase.py）
│
├─ 1. CrossCorePipeline    ── 核间 Pipeline：拆分 cube/vec 阶段循环
│
├─ 2. CombineCV            ── CV 分离：拆分为 cube 代码 + vec 代码
│     └─ AutoInsertCrossCoreSync ── 核间同步自动插入（子功能）
│
├─ 3. PipelinePlanning     ── 核内 Pipeline：规划 software pipeline
│
├─ 4. InjectSoftwarePipeline ── 核内 Pipeline：注入 software pipeline
│
├─ ...（中间辅助 Pass：LowerOpaqueBlock、FlattenBuffer、VectorizeLoop、StorageRewrite 等）
│
├─ 5. AscendMemoryPlanning ── 内存复用：计算 buffer 物理地址布局
│
└─ 6. AscendSyncInsert     ── 核内同步：分析数据依赖，自动插入同步指令
```

### 2.2 配置开关

所有 Pass 均受 PassContext 配置控制，默认关闭，需显式开启：

| 配置项 | 控制目标 | 默认值 |
|--------|---------|--------|
| `tl.ascend_auto_cv_combine` | CombineCV | False |
| `tl.ascend_auto_cross_core_sync` | 核间同步（CombineCV 子功能） | False |
| `tl.ascend_auto_sync` | AscendSyncInsert | False |
| `tl.ascend_memory_planning` | AscendMemoryPlanning | False |

---

## 三、核内同步（AscendSyncInsert）

### 3.1 核心流程

```
输入 IR（含 for 循环）
    │
    ▼
┌──────────────────────────┐
│  Step 1: 循环展开         │  ForLoopUnroller
│  将所有 For 循环展开为     │  - 每个循环展开为 iter1 + iter2
│  两次迭代 + 标记           │  - 添加 iteration_start/end 标记
│                          │  - 添加 unrolled_loop 标记
└──────────────────────────┘
    │
    ▼
┌──────────────────────────┐
│  Step 2: 逐语句分析       │  VisitStmt_(EvaluateNode)
│  提取每条语句的 buffer     │  - AnalyzeStmtAccesses：提取 buffer 访问信息
│  访问信息（读/写/pipeline） │  - 匹配 operation_config 获取配置
└──────────────────────────┘
    │
    ▼
┌──────────────────────────┐
│  Step 3: 依赖检测         │  HasDataDependency
│  对比当前语句与历史访问     │  - 检查 RAW（写后读）
│  判断是否存在数据依赖      │  - 检查 WAW（写后写）
│                          │  - 检查 WAR（读后写）
│                          │  - 基于物理地址判断内存重叠
└──────────────────────────┘
    │
    ▼
┌──────────────────────────┐
│  Step 4: 同步类型决策      │  GetRequiredSyncType
│  根据依赖双方 pipeline      │  - 同 pipeline → PipeBarrier（如 PIPE_M → PIPE_M）
│  决定同步方式              │  - 跨 pipeline → EventPair（如 PIPE_M → PIPE_V）
│                          │  - 查询 event_mapping 确定事件类型
└──────────────────────────┘
    │
    ▼
┌──────────────────────────┐
│  Step 5: 同步优化         │  OptimizeSyncRequirements
│  利用 SyncGraph 消除冗余   │  - 传递闭包：已有 A→B + B→C 则跳过 A→C
│  同步指令                  │  - 去重：合并相同的同步需求
└──────────────────────────┘
    │
    ▼
┌──────────────────────────┐
│  Step 6: 插入同步指令      │  InsertSynchronization
│  生成实际的同步 IR 节点     │  - PipeBarrier → ascend_auto_barrier(pipe)
│                          │  - EventPair → ascend_auto_set_flag + ascend_auto_wait_flag
│                          │  - A5 平台自动跳过 PIPE_V barrier
└──────────────────────────┘
    │
    ▼
┌──────────────────────────┐
│  Step 7: 循环重建         │  LoopRebuilder
│  合并两次迭代的同步语句     │  - 合并 iter1/iter2 的同步指令
│  重建 For 循环结构         │  - 去重相同同步操作
│                          │  - 恢复 For 循环节点
└──────────────────────────┘
    │
    ▼
输出 IR（含同步指令）
```

### 3.2 已支持功能

- 支持一般场景（即顺序场景、无分支、循环）下数据依赖间的自动插同步
- 支持通过 SyncGraph 传递闭包优化消除冗余的跨 pipeline EventPair 同步（已有 A→B + B→C 则不插 A→C）
- A5 平台的 `PIPE_V` PipeBarrier 自动跳过（AIC 架构不需要）
- 支持基于物理地址的内存重叠检测（通过 AscendMemoryPlanning 的 address_map + size_map）
- 支持 60+ 种 AscendC 操作的自动 pipeline 识别和 buffer 访问分析

### 3.3 待完善

- 嵌套for循环场景下的依赖分析和同步插入**尚不完善**（已有展开/合并/重建框架，但嵌套层级间的同步信息会丢失）
- 分支语句IF_THEN_ELSE场景**已有保守处理**（分支前后插入全量 PipeBarrier_ALL），但缺乏精细的分支内依赖分析，可能引入冗余同步
- 基于Buffer区间维度的细粒度自动插同步（当前检测到切片操作后直接插入 PipeBarrier_ALL，属于粗粒度处理）

---

## 四、核间同步（CombineCV + AutoInsertCrossCoreSync）

### 4.1 核心流程

```
输入 IR（tilelang_root block）
    │
    ▼
┌──────────────────────────┐
│  Step 1: CV 分离          │  CVCombineEmitter
│  根据操作类型将代码拆分为   │  - cube 操作：gemm, copy_l1_to_l0a, copy_gm_to_l1...
│  cube 代码和 vec 代码      │  - vec 操作：AscendC::*, copy_gm_to_ub, copy_ub_to_gm...
│                          │  - 不属于当前 scope 的操作替换为 Evaluate(0)
└──────────────────────────┘
    │
    ▼
┌──────────────────────────┐
│  Step 2: 同步点收集        │  CrossCoreSyncCollector
│  收集 cube/vec 侧的 GM     │  - 识别 GM 读写操作（copy_gm_to_l1, copy_ub_to_gm 等）
│  读写操作作为同步点         │  - 提取 workspace 名称、pipe 类型、读写方向
│                          │  - 记录所在循环层级（parent_for_nodes）
└──────────────────────────┘
    │
    ▼
┌──────────────────────────┐
│  Step 3: 同步点匹配        │  AutoInsertCrossCoreSync
│  按 workspace 分组匹配     │  - 校验 cube/vec 侧同步点数量一致
│  cube/vec 同步点对         │  - 校验读写方向相反（一写一读）
│                          │  - 分配统一的 sync_flag_id
│                          │  - FindTargetLoopDepth：确定同步附着的目标循环
└──────────────────────────┘
    │
    ▼
┌──────────────────────────┐
│  Step 4: 同步插入          │  CrossCoreSyncInserter
│  写入侧插入 set_flag       │  - 写操作后：ascend_auto_set_cross_flag(model_id, pipe, flag_id)
│  读取侧插入 wait_flag      │  - 读操作前：ascend_auto_wait_cross_flag(flag_id, pipe)
│                          │  - cross_interval > 1 时生成条件同步
└──────────────────────────┘
    │
    ▼
┌──────────────────────────┐
│  Step 5: 添加 resource_scope │
│  为 cube/vec 代码添加作用域  │  - cube: AttrStmt("resource_scope", 0)
│  标记                       │  - vec:  AttrStmt("resource_scope", 1)
└──────────────────────────┘
    │
    ▼
输出 IR（cube_body + vec_body + 核间同步指令）
```

### 4.2 已支持功能

- 支持 CV 核间同步自动插入（cube/vec 通过 workspace 共享数据时的同步）
- 支持智能目标循环选择（FindTargetLoopDepth）
- 支持 6 种 GM 操作类型识别：copy_gm_to_l1, copy_l0c_to_gm, copy_gm_to_ub, copy_ub_to_gm, atomic_add_ub_to_gm, atomic_add_l0c_to_gm


---

## 五、问题收集与维度分析

### 5.1 日常问题汇总

| 问题 | 模块 | 根因 | 影响 |
|------|------|------|------|
| 事件ID简单模8 | 核内同步 | `event_id_counter_ = (counter+1) % 8`，仅 8 个 ID 循环复用 | 不同事件对复用同一 ID，造成伪依赖，可能阻塞流水线 |
| set_flag/wait_flag 紧挨 | 核内同步 | EventPair 总是连续插入 SetFlag + WaitFlag | 中间无法穿插有效计算，降低流水线并行度 |
| pipeline 后同步非最优 | 核内同步 | Sync insertion 在 Software Pipeline 之后执行 | 基于拍平序列分析，无法感知 pipeline 并行性，可能过度同步 |
| 封装函数 buffer 依赖不可见 | 核内同步 | gemm_v0 仅识别顶层 3 个 buffer 参数 | 无法感知内部隐式 buffer 依赖（如 L0A/L0B） |
| workspace 读写次数不匹配 | 核间同步 | cube/vec 侧同步点数量不一致 | 直接报 FATAL 终止编译 |
| 核间同步与 CombineCV 耦合 | 核间同步 | 逻辑内嵌在 ascend_combinecv.cc 中 | 无法独立开关和维护 |

### 5.2 Issue 跟踪（共 4 项）

| Issue | 类别 | 描述 |
|-------|------|------|
| #1123 | 架构层 | PTO Codegen 不应插入同步（同步插入应在更高层次完成） |
| #943 | 漏插 | AUTO_CV_SYNC pass 在 for 循环场景下漏插同步 |
| #482 | 错插 | auto_insert_sync pass 在不该插入的位置插入了错误的同步 |
| #110 | 漏插 | 双缓冲场景下自动同步不正确 |

### 5.3 问题维度

```
问题分类
├── 架构层（#1123）
│   └── 同步插入应该在哪个编译阶段执行？当前在所有 loop transform 之后，
│       但这导致无法感知 pipeline 并行性
│
├── 漏插（#943、#110）
│   └── 该插的地方没插
│       ├── 循环内跨迭代的同步丢失（嵌套循环展开/合并机制不完善）
│       └── 双缓冲场景下缓冲区切换时的同步缺失
│
└── 错插（#482）
    └── 插在了不该插的位置
        ├── 保守策略（如 IF_THEN_ELSE 的 PipeBarrier_ALL）引入冗余同步
        └── Event ID 复用导致伪依赖
```

---


## 六、总结与展望

### 6.1 当前能力总结

| 能力 | 核内同步 | 核间同步 |
|------|---------|---------|
| 顺序场景 | ✅ 完整支持 | ✅ 完整支持 |
| 单层循环 | ✅ 展开+合并 | ✅ 目标循环选择 |
| 嵌套循环 | ⚠️ 框架已有，不完善 | ✅ 支持 |
| 分支语句 | ⚠️ 保守处理（全量barrier） | N/A |
| 跨 pipeline | ✅ PipeBarrier + EventPair | ✅ SetFlag + WaitFlag |
| 同步优化 | ✅ SyncGraph 传递闭包 | N/A |
| 切片操作 | ⚠️ 粗粒度 PipeBarrier_ALL | N/A |

### 6.2 改进方向

1. **嵌套循环完善**：改进迭代合并策略，正确处理嵌套层级间的同步传播
2. **分支精细分析**：在 IF_THEN_ELSE 内做精确的依赖分析，替代全量 PipeBarrier_ALL
3. **事件 ID 管理优化**：扩大 event ID 空间或引入动态分配策略，避免伪依赖
4. **同步与 pipeline 协同**：将 sync insertion 提前到 pipeline planning 阶段，感知并行性
5. **核间同步解耦**：将核间同步逻辑从 CombineCV 中抽离为独立 Pass
6. **Buffer 区间粒度**：实现基于区间的精确依赖分析，替代切片场景的粗粒度处理
