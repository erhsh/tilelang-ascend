# 主仓 Ascend 调度实践迁移到 A2/A3 的技术参考

目标是在 A2/A3（910B/910C）上逐步引入主仓的任务依赖分析、自动调度、
缓冲区生命周期管理和同步验证，并增强现有 Parallel、Pipelined、Persistent。
推荐先为现有规划器增加约束求解策略，输出现有软件流水注解，复用专仓的流水展开与同步生成；
随后再迁移 owner/epoch、版本管理及新的同步协议。每个阶段独立验收。

分阶段交付、验证脚本与人工审核见 [migration-plan.md](migration-plan.md)。
下列新接口和 Pass 为设计提案。

## 1. 基线与范围

| 项目 | 源码基线 | 用途 |
| --- | --- | --- |
| 主仓 tilelang | 1c0f402e166a7d0a055c991d2f82d2175a7917b7 | Ascend 任务模型、调度器、生命周期与测试合同的来源 |
| 专仓 tilelang-ascend | 43b0b69720f479894a13c02746599eccee5c5769 | A2/A3 迁移和回归比较的固定源码基线 |

tilelang-ascend 的源码基线不随工作分支推进或提交合并而改变；如需调整，
按迁移计划的审核规则单独确认。

首轮以 AscendC + Cython 为执行基线。保留 A2/A3 的向量指令、GM workspace
跨 C/V 交接及现有代码生成器；新调度采用显式启用方式。现有 Expert 与
Developer 内核继续使用 legacy 路径，默认行为不变。

本迁移不要求升级 TVM 到 TIRX，不迁入 950 的 SIMD/SIMT VF 执行、直接 C/V
搬运、专属指令或 flag spill 协议，也不以切换 TVM-FFI 为前置条件。
PTO 的新调度支持另设验收，不从 AscendC 结果推导其可用性。

### 1.1 已有能力与真实缺口

| 能力 | 专仓已有实现 | 需要迁移的合同 |
| --- | --- | --- |
| Parallel | 1D/2D Vector lowering；部分表达式串行回退；3D 拒绝 | 可选 annotations、显式任务边界；保留现有向量执行方式 |
| Pipelined | num_stages/order/stage/sync/group/cross_interval | 兼容追加 annotations、新规划策略；保留已有跨核参数 |
| Persistent | 固定 core 按步长遍历 tile 域，含 partial-wave 保护 | num_stages/annotations、非整除 group 的 padding 和有效 tile 谓词 |
| 自动同步 | AscendSyncInsert/VS；CombineCV 的跨核 workspace 同步 | 统一读写/别名/生命周期描述、guard 与 token 验证、资源预算诊断 |
| 自动调度 | PipelinePlanning + CrossCorePipeline | 可验证的任务图、资源约束和成本模型、约束调度器 |
| Task/Stage | 缺少对应公开接口和消费链 | 标记、结构验证、消费器和未支持情形诊断 |
| 缓冲区版本 | 旧 InjectSoftwarePipeline 的多版本展开 | 后续加入显式 owner/epoch、iteration/counter 版本时钟 |

Persistent 在此表示固定数量的 core 持续遍历多个 tile，不是编译缓存，
首轮也不引入全局原子领取任务的动态队列。

### 1.2 现有编译合同

| 项目 | 源码设施 | 实施要求 |
| --- | --- | --- |
| Phase 2 末端 | ResourceScopeVerify，A2/A3 AscendC 的 InstructionSelection/MaskLegalize | 保留调用顺序和末端边界 |
| C/V 自动划分 | AUTO_CV_COMBINE 默认 true，其余自动同步/内存策略默认 false | 新策略校验所需配置，保持 legacy 默认值 |
| 操作分类 | 共用 ResourceForCall/资源验证及扩展 OperationConfig | 复用分类接口，审计实际访问足迹和复合副作用 |
| Phase 1/2 顺序 | scope 推断先于临时缓冲区；CombineCV 后重算 allocation placement | 以 phase.py 为准 |
| Persistent | 参数检查和 partial-single-wave 保护 | 回归已有合同，再增强注解/非整除 group |
| 软件流水 | stage/order ABI 和 L1 分形多版本修复 | 复用一次 injector，验证最终版本和物理容量 |
| 测试 | 已有保护性回归 | 核对覆盖及实际运行结果 |

## 2. 为什么分阶段适配

主仓在 TileOp lowering 之前提取任务并运行调度链；专仓在 Phase 1 已完成
Parallel、TileOp 和虚拟 workspace lowering，Phase 2 才做流水和同步。
主仓的 task/IR 结构还依赖 TIRX、较新的 FFI、VF、TileOp 访问区间和 950 成本模型。
因此首轮在专仓已降低的硬件语义调用上构造适配层，不直接追加主仓整套 Pass。

最小接入点是现有 PipelinePlanning 内部的策略选择：CombineCV 之后、
InjectSoftwarePipeline 之前。抽取共享输入，保留旧启发式并增加约束求解策略；
不复制一份大型规划 Pass。新策略输出同一 software-pipeline 注解 ABI，
每个循环只经过一次规划和一次流水展开。

### 2.1 推荐的桥接流水线

```text
专仓 Phase 1：scope/Vector/Layout/TileOp/workspace lowering
  → CrossCorePipeline（首片不接受其处理过的跨核循环）
  → CombineCV
  → allocation placement
  → PipelinePlanning（legacy 或 constraint 策略，二选一）
  → 原 InjectSoftwarePipeline（执行一次）
  → 展平、存储重写等现有变换
  → 原 AscendMemoryPlanning
  → 原 AscendSyncInsert / AscendSyncInsertVS
  → constraint 模式的旧同步协议只读验证
  → ResourceScopeVerify
  → A2/A3 InstructionSelection / MaskLegalize
  → 现有 AscendC codegen 与执行
```

这是首轮桥接模式。新规划器只决定允许的重排和流水注解，旧同步器仍是实际
flag/barrier 生成者。后续启用新同步生成器时，必须切换同一区域的同步所有权，
不叠加两套自动同步；同理，主仓 MaterializeMultiBuffer 不能与旧 injector
同时管理同一个缓冲区。

### 2.2 配置与集成提案

拟新增 `TL_ASCEND_PIPELINE_PLANNER = "tl.ascend_pipeline_planner"`，
取值为 legacy 或 constraint，默认 legacy。
它选择首轮桥接规划策略，不宣称具有主仓完整 AutoSchedule 的全部语义。
新增配置必须进入编译缓存身份；现有 pass_configs 缓存路径需有回归验证。

Phase 2 的 Python 接入点保持原有调用，不增加第二个规划 Pass：

```python
mod = tilelang.transform.CrossCorePipeline()(mod)
mod = tilelang.transform.CombineCV()(mod)
mod = tir.transform.PlanAndUpdateBufferAllocationLocation()(mod)
mod = tilelang.transform.PipelinePlanning()(mod)
mod = tilelang.transform.InjectSoftwarePipeline()(mod)
```

PipelinePlanning 内部拟议分发逻辑如下，方法名为设计接口：

```text
strategy = PassContext[tl.ascend_pipeline_planner, default=legacy]
validate strategy
if strategy == legacy:
    return ExistingPipelinePlanning(loop)
inputs = CollectPipelineInputs(loop)
ValidateConstraintMode(inputs, target, platform)
plan = PlanWithConstraintSolver(inputs, capabilities)
VerifyPipelinePlan(inputs, plan)
return EmitLegacyPipelineAnnotations(loop, plan)
```

新增严格验证只作用于 constraint 分支，不给 legacy 增加新的拒绝条件。
共享 helper 的抽取需用回归证明 legacy 行为一致；可以先保留完整旧入口，
再逐步合并经过验证的抽取逻辑。

选择 constraint 时，入口统一校验 A2/A3、自动同步及内存规划的所需配置。
首轮要求使用已有自动同步配置；显式关闭与新模式冲突时给出诊断，不能静默跳过
同步，也不能擅自清除用户手写 flag。legacy 路径不增加新默认值。

首轮只接受完全受支持的内核/循环；遇到不支持的调用或控制流明确报错。
后续按区域回退时，必须证明区域边界的依赖和同步完整，不能静默局部回退。

bridge 模式另增一个只读验证 Pass，位置在现有同步生成和最后一次同步相关简化
之后、AscendResourceScopeVerify 之前；仅 constraint 策略调用它。它不产生事件，
因此不会与旧同步器形成双重同步插入。后端 InstructionSelection/MaskLegalize
之后仍不增加 TIR 变换。

## 3. 任务与依赖合同

### 3.1 拟议实现组件

| 组件 | 职责 | 主要方法 |
| --- | --- | --- |
| A2A3ScheduleCapabilities | 提供存储预算、合法搬运/PIPE/core、事件域和成本输入 | ValidateTarget、MemoryBudget、EventBudget |
| LegacyOperationAdapter | 将审计过的 lowered call 转换为显式操作效果 | ClassifyCall、CollectEffects |
| ScheduleTaskGraphBuilder | 构造任务、读写、控制依赖及保守别名关系 | Build、VisitStmt、AddDependence |
| TaskScheduleVerifier | 独立检查求解结果的合法性 | VerifyCoverage、VerifyDependencies、VerifyResources |
| LegacySyncProtocolVerifier | 在 bridge 的最终同步 IR 上检查事件复用合同 | VerifyEventLifetimes、VerifySetWaitPairs |
| LegacyPipelineAnnotationEmitter | 按旧 injector ABI 生成 stage/order | Emit、ValidateAnnotationShape |
| PipelinePlanning 的策略分发 | 编排共享抽取、旧启发式/新求解器、验证和注解写回 | CollectPipelineInputs、PlanWithConstraintSolver |

拟议 C++ 类采用专仓 `tvm.tir`、旧 FFI 和 C++17。收集器使用 StmtExprVisitor；
注解写回器使用 StmtExprMutator。保持原 PipelinePlanning 的 TVM_REGISTER_GLOBAL
与 Python wrapper；solver 通过旧 tvm.register_func 接入，不引入主仓 BackendContext。

新增 solver 的纯数字约束逻辑使用独立 Python 模块，避免依赖设备运行时。
Z3 作为明确的可选依赖声明，legacy 初始化不应导入它。constraint 模式缺少依赖时
给出安装提示；保留超时、无解和异常的可区分结果，并独立验证整数调度输出。

任务至少记录：稳定 ID、原始语句顺序、core 归属、PIPE/复合资源、读写 storage
及区间、标量/配置状态副作用、控制 guard、循环域、latency/II 和用户约束。
storage identity 以 Buffer/data Var 与显式 alias 关系为准，不以名字为准。

OperationConfig 可复用操作分类与 buffer 参数读写信息，但不能独自证明完整
DMA 物理足迹、复合 helper 的多 PIPE 使用或隐式状态副作用。首片只支持经过审计
的调用 allow-list；无法精确恢复访问区间时保守使用整个 buffer。

未知 extern/raw code、手写 mask/flag、未经审计的标量配置必须拒绝或作为已证明
安全的顺序边界。gemm_v0 等内部包含 MTE1/M 的 helper 不得仅因 default_pipeline
为 M 就作为单 PIPE 指令自由重排；可后续作为不可拆的复合任务建模。

### 3.2 依赖不变量

1. 保留 RAW、WAR、WAW 和影响执行语义的控制/配置状态依赖。
2. 读写区间按实际 DMA、padding、layout 和 workspace 足迹计算。
3. 同一 data Var 的不同视图、偏移和 dtype 视图必须进入同一别名分析。
4. 尚未建立完整 epoch 模型时，不假定一次区域写入覆盖所有后续读取。
5. 不跨未知副作用重排，不把手工 flag 当成普通无副作用调用。
6. 求解失败、预算不足或缺少 solver 时明确诊断；不返回看似成功的未验证调度。

区间分析须先将 min/extent 内部索引算术提升到 int64，再构造半开区间端点，
不能只对已溢出的 int32 结果做外层转换。DMA 和连续切片必须累加原 elem_offset，
把非零基址、视图和最终物理别名纳入同一访问足迹。

HF32、MMAD 方向、padding、atomic mode 等适用配置寄存器只有一份物理状态，
不能按 buffer 版本复制。开放手动 Stage 且 enable_offset 时，读者应消费逻辑阶段
顺序中的 reaching setter，并先于下一次物理覆盖；启动时的入口旧值也须受保护。
主仓 solver 的可选 register_phase_deps 使用 (u, v, physical_wrap, gap)，
wrap 只取 0/1，要求 r[v] + wrap * II >= r[u] + gap。移植旧 FFI 时需同步协议与
独立 verifier；条件 setter、opaque effects 和尚未支持的组合保持保守依赖或拒绝。

### 3.3 输入输出伪 IR

以下是合同示意，不是可执行 TIR。首轮静态循环的输入为已审计硬件语义调用：

```text
for k in serial(K):
  load_gm_to_ub(A[k], x)       # MTE2, write x
  vector_add(x, b, y)         # V, read x/b, write y
  store_ub_to_gm(y, C[k])     # MTE3, read y
```

规划器保持这些语句的计算语义，产生与直接子语句一一对应的注解：

```text
for k in serial(K), annotations={
    software_pipeline_stage: [s_load, s_compute, s_store],
    software_pipeline_order: [o_load, o_compute, o_store]}:
  load_gm_to_ub(A[k], x)
  vector_add(x, b, y)
  store_ub_to_gm(y, C[k])
```

stage 为非负且满足依赖距离；order 必须是 0..N-1 的唯一排列，N 为 injector
实际恢复的直接子 Block 数，注解数组按原语句索引对应。
CollectPipelineInputs 必须镜像旧 injector 对嵌套 BlockRealize/SeqStmt 的展开规则，
不能只按 planner 当前表层 SeqStmt 长度计算 N；需专门测试这种嵌套结构。
示例符号表示求解结果，不预先规定具体阶段。M1 限定单阶段/单版本；M2 才允许
跨迭代展开。PIPE 异步不等同 TVM async queue：首片不凭名称生成 async-stage 注解，
只保留当前 A2/A3 路径实际使用的合同，不能因引入自动调度而随意标记异步队列。
重写 SeqStmt 后必须重新校验注解长度和语句索引，不能沿用失效的旧数组。

旧 injector 负责 prologue/body/epilogue 和物理多版本；规划器不能另外添加一层
stage 维度。M2 必须核对实际展开后的版本数、对齐和存储占用，不能仅验证 solver
预测的逻辑大小。

## 4. A2/A3 硬件适配边界

主仓算法可以复用，容量、资源与延迟参数必须由 A2/A3 适配层提供。
当前两仓的编译器预算已经不同，例如专仓 codegen 中 UB=196352、
L0C=131072、L1=524032 字节；主仓 scheduler 使用 UB=248 KiB、L0C=256 KiB
等 950 预算。这些是源码预算，不能替代工具链/设备的最终可用资源合同。

能力输入至少包括：

- 每 core 的 UB/L1/L0A/L0B/L0C 可用预算、保留空间和对齐。
- 操作到 C/V、PIPE 的映射，复合操作资源占用及合法 copy 路由。
- 核内事件域、跨核 flag bank/模式/范围、用户已占用 ID 和可用槽位。
- GM workspace 分配、访问区间、可见性及生产/消费两向通知协议。
- core/vid 分工、launch 与 persistent tile 分工的对应关系。
- A2/A3 的 latency/II 来源和校准参数。

旧 SyncInsert/VS 存在按模 8 分配 ID 的实现，不能据此证明新调度下事件复用安全。
M1 只支持审计过的即时 set/wait 协议，并在旧同步器输出之后增加 constraint 专属的
只读协议验证；遇到同一事件槽仍存活就重新 set、不可配对 wait 或无法证明的控制流，
拒绝该调度。依赖边数不是事件峰值，验证必须按实际事件域、set/wait 和活跃区间计算。
验证还须将受支持任务的 RAW/WAR/WAW 和最终内存地址复用边映射到最终代码，
确认每条依赖由合法 PIPE 隐式顺序、barrier 或事件完成关系覆盖；事件配对本身
不能证明消费者等待生产完成、下一次写入等待前次消费结束。
该检查位于最后的同步相关简化之后、ResourceScopeVerify 之前，不改变 legacy 路径。
M2 跨迭代流水还需扩展验证器覆盖版本和 epoch，不沿用单迭代的证明。

完整事件分配器和按域报告所需/可用槽位的预算诊断在 M4 交付；不迁用未经 A2/A3
验证的 950 get_buf/rls_buf spill。成本估计先采用明确、保守的模型或经验证的用户提示，
不把 950 延迟表当成 910B/910C 的实测数据。

专仓原 annotate_address 与主仓 annotate_buffer_addresses 的合同须分别审计。
若新策略引入精确 placement，需显式正版本数，容量包含全部物理版本与 layout padding；
同一 placed pool 中不同 storage 的物理区间不能重叠，整个 pool 不做生命周期存储复用；
未指定地址的分配使用对齐的空闲区间。placement 不代替同步，不改 legacy 地址语义。
地址、alias/view、raw storage 指针和分配元数据应随最终物理改写保持一致。
A2/A3 的对齐、容量及可用 pool 由能力表决定，不复制 950/SS/SIMT 预留常量。
此能力独立于首片 bridge，不提前开放新地址 API。

## 5. 分阶段交付与验收

| 阶段 | 可独立评审的交付 | 完成标准 |
| --- | --- | --- |
| M0 | 配置/能力合同、审计过的操作效果、只读任务图、solver 协议与独立 verifier | legacy 无行为变化；依赖、别名、预算和未支持情况能被构造测试验证 |
| M1 | 新规划器替换单核静态循环的旧规划策略，单阶段/单版本，复用旧 injector/同步器并验证最终 set/wait 协议 | 单 Vector 路径正确；无双规划/双展开；无活跃事件槽覆盖；默认行为不变；A2/A3 数值与重复运行通过 |
| M2 | 接入 A2/A3 latency/II、跨迭代阶段与旧 injector 多版本，扩展最终协议验证 | 短循环、尾块、0/1/N 次循环、存储预算、事件复用和最终展开结果验证通过 |
| M3 | 迁入 owner/epoch、alias-aware 版本计划和 readiness/reuse 验证 | 初始化、消费、复用和退出路径完整；可空/条件循环不会生成无对应 set 的 wait |
| M4 | 在固定、保守调度下接入新同步生成，再逐步开放重排 | 新旧同步按区域单一所有权；A2/A3 事件预算/跨核 workspace 协议经实机验证 |
| M5 | 完整任务调度与新版本管理接管选定区域，扩展混合 C/V | 退出旧 injector/同步路径的区域有完整新后端合同；混合算子与旧 Expert 回归通过 |

Persistent/循环接口增强可与 M0 并行，但单独验收；Task/Stage 的执行语义应与
其消费器一起交付。首个可运行调度切片建议为 GM→UB→Vector→GM，不以复杂
FlashAttention 或隐藏多 PIPE 的 GEMM helper 作为唯一验收样例。

### 5.1 旧新组件的所有权

| 模式 | 规划 | 版本/流水展开 | 实际同步生成 |
| --- | --- | --- | --- |
| legacy | 旧 PipelinePlanning | 旧 InjectSoftwarePipeline | 旧 SyncInsert/VS 与既有跨核协议 |
| M1/M2 bridge | 新规划器 | 旧 injector，一次 | 旧同步器，一套 |
| M3 分析阶段 | bridge 规划并校验 epoch | 旧 injector，验证其结果 | 旧同步器，验证其 token 合同 |
| M4 同步试验 | 固定保守调度 | 已验证版本路径 | 选定区域仅新同步生成器；其余仍 legacy |
| M5 完整路径 | 新任务调度 | 新版本管理 | 新同步生成器 |

M3/M4 的 bridge 不自动取得任意混合区域的支持。区域混用需定义明确边界，
在覆盖不足时采用内核级拒绝，不能让两个生成器分别保护同一依赖。
VS/标量相关同步只有在新模型覆盖对应副作用后才能退出旧生成器。

M4 的新同步器必须消费最终 AscendMemoryPlanning 的 address_map/size_map，
或从等价的最终分配重建物理别名与复用依赖。StorageRewrite/MemoryPlanning
可能让不同逻辑 buffer 复用同一地址，因此不能直接沿用 M0/M1 的逻辑 storage 图。
物理地址 WAR/WAW 复用边必须在最终 MemoryPlanning 之后、ResourceScopeVerify
之前补齐并验证；固定保守调度也不能替代这一分析。

## 6. Parallel、Pipelined、Persistent 与 Task/Stage

### 6.1 独立循环接口增强

Pipelined 以兼容方式追加 annotations，保留 sync/group/cross_interval。
旧 order/stage 与新 Stage 约束混用必须明确拒绝或验证一致，不静默覆盖。
新 annotations 中只有已实现的语义可以启用，未消费的调度字段必须诊断。

Persistent 增加 num_stages/annotations 并传到生成的 wave loop；对末维 group
非整除情形采用 padded tile 域和有效性 guard。保留现有参数正数、index 检查及
partial-single-wave 保护。静态空 domain/零 extent 继续按已有合同报错；
符号域先覆盖合法正值，不把运行时零 extent 自动解释为受支持的空循环。
少于 core 数、跨 wave 和符号域分别测试。

传递 num_stages 注解只代表接口贯通，不等于 Persistent wave 流水已经可用。
必须另测带有效 tile guard 的 wave body 是否被旧 planner/injector 正确接受，
并验证尾 wave 的依赖和同步；接口 PR 不提前宣称完成其流水执行语义。

Parallel 保持 A2/A3 当前 Vector lowering。annotations 是额外的调度元信息，
不将主仓 SimtVF 线程域引入 A2/A3；现有不支持的维数仍需明确诊断。
新任务/布局注解只挂在最外层 Parallel loop，避免专仓当前逐维复制注解时重复
引入调度作用域；已有 coalesced_width 的行为单独保持并回归。

### 6.2 Task/Stage 标记

先实现 Task() 与 Stage(n) 的 AttrStmt 合同：Task 只包含同一 PIPE、同一 C/V
归属的操作，不隐式 launch、不提供任务体内跨 PIPE 同步；Stage 必须包住完整任务，
不可嵌在 Task/Parallel 内部。首版拒绝 Stage 与旧 order/stage 同时使用。
Stage 约束完整任务和每 PIPE 的顺序。未经审计的复合 helper 不假装
满足单 PIPE Task 合同。非零 Stage、enable_offset 与 num_stages 的语义需与消费器
同时实现；初期可拒绝未支持组合，而不是让标记静默失效。

PerCoreTask、条件 exact-once 分派和完整 stateful/grouped tile scheduler 后置。
后续支持 Task 内部条件时，应保留整个任务的同步边界并汇总所有分支的资源/effects，
区分内部条件与外层候选选择；单 PIPE/core 约束和 exact-once 合同不因此取消。
主仓 AscendTileScheduler 依赖 meta_class/inline/BaseTileScheduler，专仓没有该前端
体系；先增强现有 Persistent，避免为一个调度类迁入整个 eager/meta 前端。

## 7. 文件和依赖清单

以下新增路径及名字是实施提案；当前不包含对应实现。

| 位置 | 计划改动 |
| --- | --- |
| src/transform/pipeline_planning.cc | 抽取共享输入，保留 legacy 并增加 constraint 策略分发 |
| src/transform/common/ascend_schedule_analysis.{h,cc} | 拟新增 post-lower 任务/效果适配与结果验证 |
| src/transform/common/ascend_schedule_contract.h | 拟新增任务/依赖/能力与 solver 数据合同 |
| src/transform/ascend_schedule_sync_verify.cc | 拟新增 bridge 最终 set/wait 协议只读验证 Pass |
| src/transform/common/operation_config.h | 复用已有操作分类；补审计过的显式效果，不另建矛盾分类表 |
| tilelang/transform/pass_config.py | 拟议策略配置，默认 legacy |
| tilelang/engine/phase.py | 保留原规划/展开调用；constraint 在最终同步后增加只读协议验证，后续接管时显式分支 |
| tilelang/transform/ascend_z3_scheduler.py | 拟新增独立数据协议、solver 和结果验证；避免导入设备运行时 |
| tilelang/transform/__init__.py、可选依赖声明 | 以旧 FFI 注册 solver；legacy 不强制加载 Z3 |
| tilelang/language/pipeline.py、parallel.py、persistent.py | 兼容追加注解；Persistent 参数与尾块语义 |
| src/ir.cc 及对应 FFI 声明 | 同步修改 native builder 与 Python 签名 |
| tilelang/language/schedule_hint.py | 拟新增 Task/Stage，随验证/消费器交付 |
| testing/python/language/ 与新调度测试目录 | 复用旧回归，添加依赖、注解、容量和循环覆盖测试 |

主仓可复用来源按依赖分组：

| 来源 | 复用方式 |
| --- | --- |
| Ascend auto_schedule.cc、Python loop scheduler | 迁移求解思想和数据协议；替换 TIRX/新 FFI 与 A2/A3 成本输入 |
| dependency_analysis.cc、src/transform/common/int64_promoter.h | 区间算术提升后再比较端点；补大索引与动态尾界回归 |
| buffer_addresses.h、merge_ub_allocations.cc、remove_no_op.cc | 参考固定地址/版本校验、指针改写与分配元数据维护；不照搬 950 pool 参数 |
| memory_detector/task_analysis/ir_structure/scheduled_tir | 参考任务/依赖/控制流合同；首片使用专仓 post-lower 适配，后续逐项迁移 |
| annotate_multi_buffer_eligible、prepare/materialize_multi_buffer | 后续迁移 owner/epoch 和版本时钟，不与旧 injector 同时改同一存储 |
| insert_sync、rewrite_flag_to_buf | 迁移依赖/token 验证实践；同步发射和事件资源必须适配 A2/A3 |
| language/loop.py、Ascend schedule_hint.py、src/ir.cc | 兼容增强循环/标记 API；保留专仓扩展参数 |
| testing/ascend/auto_schedule | 迁移行为与边界测试，替换 950 VF/直连等硬件夹具 |

## 8. 验证方案

| 层次 | 关键覆盖 | 验证目标 |
| --- | --- | --- |
| 数据协议/solver | DAG、RAW/WAR/WAW、资源冲突、配置寄存器 phase、不可满足约束 | 结果由独立 verifier 校验，不以 solver 成功等同合法 |
| 操作与别名 | 多视图、int32 边界、elem_offset、连续切片、workspace、配置状态 | 正确物理足迹和内部位宽；不按名字判断独立 |
| 流水 ABI | stage/order 长度、唯一顺序、async 注解、单次展开 | 规划器输出与 injector 真实消费合同一致 |
| 控制流 | 0/1/N trip、trip 小于 stage 数、条件 producer、嵌套/连续循环 | prologue/epilogue 和等待路径完整，复用不越过生命周期 |
| 资源 | UB/L1/L0C 边界、对齐、多版本、事件域上限与超限 | 核算实际物理分配与事件活跃区间，超限可诊断 |
| 显式地址 | 正版本数、全版本大小、非零基址、raw pointer、dead/unknown storage | placement/版本/别名一致，重叠拒绝，未知项不被静默丢弃 |
| 循环域 | Persistent 静态空域拒绝、partial wave、非整除 group、合法正值 symbolic | 非法域保持诊断；每个有效 tile 恰好一次，无无效 tile 的副作用 |
| native lowering/codegen | A2、A3、默认/bridge、AscendC；PTO 另列 | 无 950 专属调用；C/V scope、mask 末端合同保持 |
| 实机 | copy-vector-copy、GEMM、归约、混合 C/V、尾块与随机 scratch | 数值正确，无未初始化读和跨 tile 污染 |
| 压力/性能 | 重复 launch、多 core、并发 stream、长流水 | 无死锁/串扰；分开记录编译、host 和设备耗时 |

测试验证依赖和协议不变量，不固定具体 flag ID 或整份生成源码。可能挂起的
硬件测试用独立进程隔离超时，不能仅用线程超时后继续复用同一设备任务。
性能只在正确性和稳定性验收之后判断，不预设新规划器一定更快。

优先复用专仓 test_ascend_sync_insert.py、zero_trip/one_trip、Persistent、
gemm_v0_pipeline 和现有 Developer/Expert 示例。迁移主仓 sync_boundaries、
cross_core_flags、flag_allocation 等测试的行为合同，改成 A2/A3 可执行夹具。
同时参考 test_special_registers、test_ascend_buffer_addresses、DMA offset 与
strided-slice 回归，替换 950 专属指令和资源参数。

## 9. 验证环境

纯数据与 solver 测试使用独立主机环境；TIR、FFI、lowering 和 codegen 测试
要求与源码匹配的 TVM/专仓原生构建。设备验收要求 Linux/CANN、Bisheng、
兼容的 torch_npu 及实际 A2/A3 设备，两代设备分别记录结果。
仅编译生成代码不能替代数值、同步稳定性和性能验证。

## 10. 源码核对入口

- 专仓：tilelang/engine/phase.py；src/transform/pipeline_planning.cc；
  src/transform/inject_pipeline.cc；src/transform/ascend_sync_insert.cc；
  src/transform/ascend_combinecv.cc；src/transform/ascend_workspace_reduction.cc。
- 专仓：tilelang/language/{parallel,pipeline,persistent}.py；src/ir.cc；
  src/target/codegen_ascend.cc；testing/python/language/。
- 主仓：tilelang/ascend/pipeline.py；src/ascend/transform/auto_schedule.cc；
  annotate_multi_buffer_eligible.cc；prepare_multi_buffer.cc；insert_sync.cc。
- 主仓：tilelang/ascend/language/schedule_hint.py；tilelang/language/loop.py；
  src/ir.cc；testing/ascend/auto_schedule/。

设计采用当前源码确定的顺序；遇到旧 reference 与 phase.py 不一致时，以实际
输入/输出合同和源码为准。新增变换不得跨越最终 MemoryPlanning、ResourceScopeVerify、
InstructionSelection 与 MaskLegalize 的依赖边界。
