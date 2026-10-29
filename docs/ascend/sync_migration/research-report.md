# Ascend 后端对比调研与自动同步迁移路线

本文汇总两个本地仓库当前 checkout 的源码调研，以及本会话已执行的旧实现基线。
调研阶段交付的是设计和骨架，当时未实现新同步引擎或 ProofService。
后续已进入可选非流水后端的实现验证，实时范围和结果见[实现进展](implementation-status.md)。
实施契约见 [迁移设计](pass-design.md)，文件和接口见 [实现骨架](pass-impl-skeleton.md)。

调研记录日期：2026-10-29。源码结论以本文固定的 commit 和已记录工作树差异为边界。

## 1. 结论、范围和证据分级

**已确认事实**：主仓已有完整 Ascend 950 编译、自动调度、同步和设备执行路径。
本仓也已有自动同步、软件流水、Persistent、Parallel、Carver 和 autotuner，迁移是增量改进。
本报告只比较 Ascend，不分析其他硬件后端，不使用其他分支、PR 或网络资料。

**已采纳决策**：先后向移植主仓真实同步引擎，以旧 TIR adapter 对接本仓晚期同步位置；
在 TileLang 侧移植原 Z3 prover 为 ProofService，不整体升级 TVM，不引入 AutoSchedule。
这里的 Z3 负责证明访问关系和控制条件；它与主仓求任务顺序、II 的 Z3 调度器是两个模块。

**推断**：将调度与同步、硬件策略与证明服务分开，可缩小首次迁移的兼容面。
这不证明移植已正确，也不保证减少同步数量或提高性能。
**建议**：按 S0–S5 验收；完整流水与 VS 覆盖之前，不删除旧实现或切换默认。

## 2. 固定分析基线

| 项目 | tilelang-ascend，本仓 | tilelang，主仓 |
| --- | --- | --- |
| 用户指定位置 | `~/workspace/git/github/erhsh/tilelang-ascend` | `~/workspace/git/github/tile-ai/tilelang` |
| HEAD | `43b0b69720f479894a13c02746599eccee5c5769` | `1c0f402e166a7d0a055c991d2f82d2175a7917b7` |
| TVM HEAD | `c2921fdaf795b1103d21abc962e83a209c7258d7` | `4211874e9e4b8770fd20ad0d98ab80afd1df5028` |
| 原生 IR | `tvm.tir` | `tvm.tirx`、`tvm.s_tir` |
| 注册/容器 | `TVM_REGISTER_GLOBAL`、旧 ObjectRef | 新 `tvm.ffi`、reflection、Any |
| Ascend 组织 | 通用 engine 中的 Ascend 路径 | 独立 `tilelang/ascend`、`src/ascend` |

本仓 TVM 另有本地修改：`python/tvm/tir/buffer.py`、`src/runtime/logging.cc`。
这些工作树差异属于分析基线，迁移不得覆盖；单独记录 HEAD 不能完整复现工作树。
后续源文件清单还需记录源 commit、blob hash、目标路径及适配说明。

复核命令在两个仓库分别运行，不切分支、不拉取远端：

```bash
git rev-parse HEAD
git status --short
git -C 3rdparty/tvm rev-parse HEAD
git -C 3rdparty/tvm status --short
```

以下主仓来源均相对其仓库根目录，并固定于上述主仓 HEAD；TVM 路径使用上述 TVM HEAD。
行号用于定位当前快照，不作为将来插入 Pass 的稳定编号。

## 3. Ascend 编译与执行差异

本仓事实来源：[phase.py](../../../tilelang/engine/phase.py)、
[lower.py](../../../tilelang/engine/lower.py)、[target.py](../../../tilelang/utils/target.py)。
其中 `lower.py:226` 用 `llvm` kind 搭配 Ascend model；`:159` 按 model 选择设备 codegen。

```text
本仓 Python DSL / @jit
 → scope 推导、Parallel 向量化、LayoutInference、LowerTileOp
 → CrossCorePipeline → CombineCV
 → PipelinePlanning → InjectSoftwarePipeline
 → flatten / storage rewrite / unroll
 → AscendMemoryPlanning：最终 address_map、size_map
 → AscendSyncInsert → AscendSyncInsertVS
 → ResourceScopeVerify → 受平台限制的 InstructionSelection / MaskLegalize
 → AscendC / PTO codegen → 现有编译和运行适配器 → NPU
```

主仓来源：`tilelang/ascend/pipeline.py:36,103,120,169`，
`tilelang/ascend/backend.py:15,56`，`execution_backend.py:7`，
`src/ascend/runtime/ascend_module.cc:519,637`。

```text
主仓 Ascend 方言 / 原生 ascend target
 → kernel launch、layout / fractal storage / OOB normalization
 → materialize tasks → core assignment → latency estimate → AutoSchedule
 → PrepareMultiBuffer → ResolveCore → InsertSync
 → MaterializeMultiBuffer → LowerScheduledTIR
 → AscendLowerTileOp → flatten / device split
 → MergeUBAllocations → RewriteFlagToBuf
 → AscendC codegen → bisheng / .aibin → TVM-FFI host ABI → ACL launch
```

主仓 `auto` 执行 backend 首选 `tvm_ffi`，运行时通过
`TVMFFIEnvGetStream(kDLExtDev, device_id)` 获取当前 stream。
主仓 PTO 与 AscendC 共用 Ascend pipeline，以 target key/backend manifest 区分。
本轮保留本仓编译、codegen 和运行链，不迁移新的 launch ABI、缓存或 PTO 执行适配器。

最关键差异是内存与同步的契约：

| 维度 | 本仓 | 主仓 |
| --- | --- | --- |
| 同步输入 | lowering 后的指令、最终地址 maps | 较早的任务/语义访问、core/stage/version |
| 内存复用顺序 | 先规划地址，再补同步 | 同步分析产出 alias proof，再允许地址复用 |
| 中间表示 | 原生旧 TIR、流水 annotations | `tl.schedule_unit`、Task core mask、owner/epoch |
| 流水物化 | 旧 injector 生成版本/分段 | MaterializeMultiBuffer 和 LowerScheduledTIR |
| 硬件假设 | A2/A3/A5 等现有平台分支 | 950 / `dav-3510` 为主要实现依据 |

因此不能将主仓 InsertSync 直接插在本仓同名位置，也不能在晚期同步后重新分配地址。
所有新同步必须在 ResourceScopeVerify 前物化，MaskLegalize 后不得增加 TIR 改写。

## 4. 已有能力与迁移增量

| 能力 | 本仓已存在的基础 | 主仓可借鉴的增量 |
| --- | --- | --- |
| 自动同步 | ordinary 与 VS 两个 Pass、OperationConfig、地址冲突分析 | 统一依赖/覆盖证明、guard/epoch、事件分配与预留 |
| 软件流水 | PipelinePlanning、InjectSoftwarePipeline、版本轴 | owner、时钟、跨迭代覆盖、物理 footprint 验证 |
| 自动调度 | 现有流水规划与 layout 推导 | 资源/latency/II/容量约束下的 task scheduling |
| Parallel | AscendLowerParallelToVector | 主仓 VF 内 SIMT/SIMD 映射；需要硬件能力边界 |
| Persistent | 分组域、wave、worker-index 循环 | 非整除域、Ascend tile schedulers、状态/epoch 组合 |
| Carver | Ascend arch/profile、Ascend policy | 动态设备属性、参数化容量和成本数据 |
| autotuner | 参数枚举、编译/校验/计时选优 | 可复用输入、缓存及失败分类等工程实践 |

本仓入口：[Persistent](../../../tilelang/language/persistent.py#L11)、
[Carver Ascend](../../../tilelang/carver/arch/ascend.py#L23)、
[Ascend policy](../../../tilelang/carver/roller/policy/ascend.py)、
[autotuner](../../../tilelang/autotuner/tuner.py)。
主仓入口：`tilelang/ascend/language/tile_schedule.py:28,248,265`、
`tilelang/carver/arch/driver/ascend_driver.py:4,27,35`、`tilelang/autotuner/tuner.py:135,1356`。

auto-schedule 必须分开理解：layout 推导、任务 scheduling、参数 autotune 并非同一算法。
主仓 AutoSchedule 优化 task 起始时间、II 和 buffer versions，不自动搜索 GEMM tile size。
`T.Parallel` 的元素并行不等于 MTE/Cube/Vector 阶段并发；后者由任务资源与依赖决定。
Persistent 表示固定 grid 中的 tile 分配循环，不是新的硬件线程常驻指令。

主仓 `src/ascend/transform/auto_schedule.cc:940–972,1431–1435` 还表明：
`num_stages` 主要限制 buffer-version 搜索；`enable_offset` 默认关闭时 stage 归零。
这与本仓同名参数不能假定同义，迁移前须明确版本数、stage 深度和时钟的映射。

## 5. 已采纳的同步替换架构

```text
旧 TIR + 最终地址 + 流水 provenance + 生效配置
 → LegacySyncImporter / PhysicalStorageResolver
 → 主仓 AnalyzeDependencies → SyncAnalyzer → ValidateSyncPoints
 → 显式事件预留 → 主仓 FlagAllocator
 → 主仓 SyncMaterializer + TargetSyncPolicy
 → LegacySyncExporter → 既有同步 ABI / ResourceScopeVerify
```

这必须实际运行移植的主仓算法；仅改名、包装旧实现或用旧 VS 补洞不算替换完成。
迁移期 backend 二选一，完整新链与完整旧链互斥，禁止混合分配事件或静默回退。
本轮不改变计算顺序、已规划 buffer 地址、版本物化或存储复用策略。

主仓 `InsertSync` 不依赖 Z3 scheduler 的执行结果来源；只要求合法任务/core/stage/version。
其 `testing/ascend/_ir.py:83–94,122–126` 支持 source-order、stage0 的无求解器同步测试。
这证明可以独立接入同步分析，不证明一般流水可丢弃 owner/epoch。

### 5.1 最小源码闭包与边界

主仓以下 `auto_schedule/` 项均位于 `src/ascend/transform/` 下。

| 来源 | 可数规模/位置 | 处理 |
| --- | --- | --- |
| `insert_sync.cc` | 2637 行；`:2531` 为内核入口 | 保留 site、closure、analysis、allocation、materialization |
| `auto_schedule/dependency_analysis.{cc,h}` | 498 + 135 行 | 保留冲突/距离算法，改造物理冲突查询与证明接口 |
| `auto_schedule/ir_structure.{cc,h}` | 406 + 958 行 | 保留 Task/Control、guards、parent/index/context 必要模型 |
| `auto_schedule/multi_buffer.h` | 822 行 | 保留 owner、version、epoch 契约，不重复物化版本 |
| `auto_schedule/buffer_lifetime_analysis.h` | 287 行 | 保留 issue/completion 及带距离的生命周期证明 |
| `transform/common/constr_visitor.h` | 579 行 | 适配旧 TIR、Let、范围与 mutable-read snapshot |
| `core_mask.h`、`buffer_version.h` | 71、95 行 | 保留必要小类型，硬件常量转为 policy |
| `task_analysis.h`、`memory_detector.h`、`ascend_pipe.h` | 648、1414、363 行 | 用本仓 lowered-call adapter 替换高层 op 检测边界 |
| `scheduled_tir.{cc,h}`、`task_annotations.h`、`kernel_rewriter.h` | codec/遍历层 | 不整体迁移；直接构造任务树、保存原语句并回写 |
| `buffer_alias_analysis.h`、`buffer_alias.h` | 1523、160 行 | 保留必要 context/类型依赖；不输出重新分配地址的 alias 计划 |

仅关闭 alias 优化开关不足以删除编译依赖：SyncAnalyzer 继承 BufferAliasAnalysisContext。
保留必要接口，或单独抽取等价 context；不得连带删掉事件复用及同步覆盖所需证明。
源文件须显式加入 CMake；不能假定现有非递归 glob 会收集新增子目录。

可独立抽取的算法包括 `insert_sync.cc:634–725` 的 DepClosure，
以及 `buffer_lifetime_analysis.h:73–186` 的带距离 reachability/periodic separation。
FlagAllocator（`:1864–2190`）还依赖 site/domain/core/version，不是现成的纯整数分配器。
仅抽两个图工具不能称为完成 InsertSync 移植。

### 5.2 物理 alias 与逻辑 epoch 分离

主仓 `dependency_analysis.cc:151` 根据 data Var 不同排除 storage 冲突。
本仓不同 Var 可能已经复用最终地址，照搬会漏同步；需按 scope、物理归属与字节区间判断。
实际 footprint 包含 DMA 对齐、padding、fractal pitch、dtype/view、workspace 和版本索引。
没有明确 noalias 契约时，不凭 GM 参数 Var 不同就认定不别名。

物理地址冲突与逻辑 owner/epoch 必须分别保存：共用地址不等于共用版本时钟。
分析可建立统一物理 pool/alias group，但不能因 byte-view 合并而丢失两端逻辑 storage。
跨 storage 的依赖不能随意采用一方 ring；时钟关系无法证明时拒绝不支持的协议。
访问不相交证明失败则保守保留冲突；这不等于任意 guard/epoch 失败都可靠多插同步解决。

### 5.3 流水 provenance 是最终替换前提

旧 injector 已生成 skew、版本索引与 prologue/steady/epilogue；晚期 TIR 可能丢失逻辑时钟。
主仓 distance=-1 的解析依赖 ring/owner，因此 stage0 + 空版本表只能作为非流水起点。
不得对所有物理流水清空 versions 后宣称兼容；也不得再次扩展已物化版本。

拟由旧 injector 发布、后续改写传播和验证以下 provenance：

| 字段 | 事实来源 |
| --- | --- |
| storage_id / owner_id | 原逻辑 buffer 与流水 owner |
| logical_iteration / stage / order | 原流水计划及逻辑到物理循环的映射 |
| physical_versions / slot_expr | injector 实际生成的版本数与索引 |
| execution_guard | 任务实际执行条件 |
| segment / original_task_id | 三段流水与原任务关系 |

Flatten、StorageRewrite、Unroll 等改变身份的位置须逐处传播并测试，禁止保留过期表。
upstream 模式遇到缺失/失效 provenance 必须定位诊断；流水验收之前保留旧对照路径。

### 5.4 TargetPolicy、VS、显式事件与动态 ID

主仓 `src/ascend/target_utils.h:17` 写有 950 容量，`insert_sync.cc:2181` 写有 8/16 槽。
SIMT 上下文、AIC:AIV 分组、bank、SS 及 `get_buf/rls_buf` spill 均不能全平台照搬。
同 pipe 是否隐式有序也须由 target policy 决定，不能套用主仓跳过 S/V/M 依赖的规则。

本仓 [VS 实现](../../../src/transform/ascend_sync_insert_vs.cc#L773) 表明 A5 V→V 使用 V_V 事件，
与其他平台的 V barrier 路径不同；统一引擎须完整覆盖 V/S，不能保留旧 VS 算法补洞。
ordinary/VS 两项配置的有效默认值和开关含义仍保留，但事件统一分配。

显式 flag 的合法编号、范围和生命周期参与预留；无法界定动态 ID 时拒绝自动分配。
主仓预留逻辑出处为 `insert_sync.cc:1814–1859,1895–1923`。
本仓输出 ABI 部分仅接受 literal ID：有限合法集合可导出等价条件分派，保持 set/wait guard。
不得通过截断、取模或默认 PIPE_ALL 掩盖无法表达的同步；容量耗尽须明确诊断。
主仓 `:839` 也明确拒绝未覆盖的 AIV0→AIV1 依赖，不自动创建 AIC relay。

## 6. TileLang 侧原 Z3 ProofService

已采纳独立移植原 prover，适配旧 TIR，不将整个新 Analyzer/TVM 带入本仓。
可先调用旧 Analyzer 简化，再调用独立 Z3 证明；所有 bind/range/guard 必须显式 replay。
启用新同步后端必须具备 Z3 服务，未编入或缺少运行库时明确报错，不能静默替代为旧
Analyzer。已启用服务的不可证明、timeout、unknown 不作为消除依赖的证据；解析失败
必须诊断。guard 匹配、epoch 对齐或同步覆盖无法证明时，选择有证明的位置或拒绝输入。

主仓 TVM 来源：`src/target/z3/z3_prover_on.cc` 967 行、`z3_prover_off.cc` 60 行，
`include/tvm/arith/analyzer.h:655–756` 的 Z3Prover 声明 102 行。
核心只 include TVM 公开 headers、标准库和 `z3++.h`；保存 Analyzer* 但未调用其方法。
因此可局部后向移植，不要求新 TVM 私有实现；967 行只是源码体量，不是完成成本。

需要保留或审计：lazy scope journal/push-pop、副作用 memo、context 生命周期、固定 seed、
rlimit、负数 div/mod、cast/溢出、位运算以及 unknown 的保守语义。
若不挂 Analyzer，可避免改变旧 Analyzer 布局、所有子 analyzer Clone 和全局证明行为。

外部依赖依据主仓 Linux 配置：`z3-solver>=4.13.0,<4.15.5`，自带 headers 与 libz3。
主仓 `CMakeLists.txt:417` 默认 USE_Z3/USE_PYPI_Z3；
TVM `CMakeLists.txt:473–481` 选择 on/off 实现并链接库；
`cmake/pypi-z3/FindZ3.cmake` 为 82 行；安装还需库查找/RPATH，不只是编译时 include。
本节为调研时的可行性判断；后续构建、关闭路径和安装加载验证见[实现进展](implementation-status.md)。

## 7. 模块化实施与验收

| 阶段 | 交付模块 | 必须通过的退出条件 |
| --- | --- | --- |
| S0 | 文档、骨架、来源与旧基线 | 边界确认；来源可追溯；新旧行为区分清楚 |
| S1 | 上游同步核心、旧 TIR adapter、物理冲突、ProofService | 独立构建；非流水依赖/guard/alias 正负例；显式 opt-in |
| S2 | ordinary/VS 统一、TargetPolicy、显式事件预留 | 各平台 V/S、动态 ID、容量、未知副作用诊断；无旧算法补洞 |
| S3 | injector provenance、owner/version/epoch | 三段流水、多版本、动态/嵌套循环、Persistent 组合；损坏元数据拒绝 |
| S4 | codegen/编译/NPU/性能对照 | 各启用 target 验收；关闭编译器自动同步仍正确；记录性能与编译时间 |
| S5 | 默认切换、删除旧算法/过渡配置 | 仅新入口；弃用 API alias、幂等性和聚焦回归通过 |

S1 通过不意味着完整替换。S3/S4 未通过的平台不得切换默认。
引擎重复执行须验证 IR、maps、provenance、配置和 target 的完整签名，不能仅凭标记跳过。
同步输入发生变化时，应回到插同步前 IR，不在旧事件上再次分配。

测试按行为所属边界分层：Pass 主机测试、流水契约、codegen/compiler、设备和性能。
结构断言检查访问、guard 支配、事件配对、完成关系和语句身份，不固定任意 flag ID/offset。
零次、一次、多次、分支、不同 Var 同地址、view/dtype、显式事件、资源耗尽均须覆盖。
NPU 测试设置进程超时检测死锁；数值通过与同步覆盖证明分别记录。
性能以相同设备、输入、工具链、warmup 对照，报告分布、事件数、内存和编译时间。

主仓可迁移测试语义来自 `testing/ascend/auto_schedule/` 下的
`test_sync_dependencies.py`、`test_sync_boundaries.py`、`test_sync_access_footprints.py`、
`test_buffer_epochs.py`、`test_epoch_guards.py`、`test_flag_allocation.py`、`test_cross_core_flags.py`。
原 `testing/ascend/README.md:44–85` 要求按行为边界测试、避免绑定任意编号，值得保留。

## 8. 已执行基线与复现命令

本会话 root 实际执行以下现有测试：51 passed、14 warnings、0.72s。
其中 25 个 ordinary CPU、11 个 one-trip CPU、15 个 Persistent IR/源码生成测试。

```bash
python -m pytest -q \
  testing/python/language/test_ascend_sync_insert.py \
  testing/python/language/test_ascend_sync_insert_one_trip.py \
  testing/python/language/test_tilelang_ascend_language_persistent.py \
  -p no:cacheprovider
```

另一个已实际执行的四文件命令结果为 54 passed、14 warnings、6.27s、exit 0：

```bash
python -m pytest -q \
  testing/python/language/test_ascend_sync_insert.py \
  testing/python/language/test_ascend_sync_insert_one_trip.py \
  testing/python/language/test_tilelang_ascend_language_persistent.py \
  testing/python/language/test_ascend_sync_insert_zero_trip.py
```

额外 3 例是 [zero-trip 设备测试](../../../testing/python/language/test_ascend_sync_insert_zero_trip.py#L35)：
trip_count=0/1/3，**不是 num_stages=0/1/3**；AscendC/A3，显式 `--cce-auto-sync=off`。
该文件 `:51–59` 创建 NPU tensor、调用 kernel 并作数值断言，属于设备正确性基线。
VS 测试会调用设备编译器，不能混称为纯 CPU Pass 测试。
以上均测试旧实现；未测试新引擎、ProofService、性能或完整主仓测试套件。
调研文档最初汇总时没有重新运行 kernel；本节耗时仅是此前命令记录，不代表性能 benchmark。
后续新实现的测试另列于[实现进展](implementation-status.md)，不混入本节旧基线。

## 9. 成本、假设与未验证项

以下是规划估算，单位为人周，不是已发生工时或承诺；包含适配与聚焦验证的判断。

| 路线 | 估算 | 主要假设/边界 |
| --- | --- | --- |
| TileLang 独立原 Z3 ProofService | 2–4 | 旧 TIR、Linux Z3 依赖、必要 API；不含完整 InsertSync |
| 将 prover 挂回旧 TVM Analyzer | 4–8 | Bind/scope/fallback、ABI 重编、Python API 与全局回归；不含完整 InsertSync |
| 整体升级 TVM 并恢复本仓 Ascend | 20–40 | 跨 IR/FFI、构建/runtime、Pass/helper/用例恢复；硬件与工具链可用 |

尚未对完整同步替换评估总工期；不得将 ProofService 估算当成 S1–S5 总成本。
整体 TVM 路线同时触及新 IR/FFI/runtime 与旧 Ascend 逻辑，当前未选。
估算需要在 S1 编译与复杂流水样本实测后更新；旧代码本地修改和硬件可用性会影响成本。

### 9.1 整体 TVM 升级的成本依据

当前两个 TVM HEAD 不是祖先关系，已在本仓 TVM 对象库中比较：共同祖先为
`b54f57aa721a4e619dbe187bb4ac0cfd37988c71`，旧、新两侧各有 377、1656 个独有提交。
这只说明分叉跨度，不能把提交数直接换算成人力，也不能据此归因全部 Ascend 历史定制。

本仓 `tilelang/` 和 `src/` 可见源码的静态搜索结果如下；每行按唯一文件计数，各集合
重叠，不能相加。搜索不到显式引用也不代表没有间接依赖。

| 统计范围 | 总文件数 | 显式依赖项 | 命中文件数 |
| --- | --- | --- | --- |
| `tilelang/**/*.py` | 145 | 旧 TIR | 65 |
| 同上 | 145 | 旧 FFI/对象注册 | 19 |
| `src/**/*.{cc,h}` | 142 | 旧 TIR | 98 |
| 同上 | 142 | 旧注册/对象声明宏 | 60 |
| 同上 | 142 | TVM 内部头 | 55 |
| 同上 | 142 | LetStmt/Allocate/Block 等旧节点 | 55 |

主要工作不是更新 import：新 FFI/对象模型、`LetStmt` 到 `Bind` 的绑定作用域、
`Allocate` 到 `AllocBuffer` 的生命周期，以及 tirx/s_tir 变换拆分，都需要逐项验证。
公共 DSL、layout、Pass、codegen、JIT、缓存和安装链被 Ascend 共用，不能只处理文件名
中带 ascend 的部分。编译通过后，仍需验证地址复用、同步、尾块、mask 与设备精度。

工作树中的两处 TVM 改动也须重新核验：动态 slice 的显式单位步长兼容，以及 logging
中符号大小被误用为名称长度的修复。这两处不代表全部历史定制；新 TIR 对显式 slice
step 的处理不同，不能直接覆盖后就认为语义兼容。

20–40 人周按熟悉 TVM/C++ 和 Ascend 的工程师、已有构建和目标设备、恢复当前 Ascend
公共 DSL 与 AscendC/PTO 路径估算。两人约 3–6 个月仅为日历规划参考，受基础层依赖和
设备验证串行部分影响，不能简单除以人数。没有试迁移或全量回归数据支撑确定交期。

## 10. 保留的后续全能力迁移路线

当前同步替换完成后，最初讨论的能力仍按独立模块推进，不在本轮混入：

1. 任务效果/资源/成本接口：共享已验证的访问和 target facts，补容量与延迟校准。
2. AutoSchedule：适配直线任务，再做受限 modulo scheduling；与 autotuner 分开验收。
3. 多 buffer / pipeline parallel：明确旧 stage/version 语义，保证 provenance/epoch 闭环。
4. Persistent：先静态覆盖/无重复与尾域，再 grouped/stateful 和跨 tile 同步生命周期。
5. Parallel/VF：按平台 capability 选择支持面，不将 950 VF 指令强加给旧平台。
6. Carver/autotuner：共享 target profile、合法性筛选、正确性和性能数据，再扩搜索空间。

若未来整体引入主仓 scheduled TIR，必须替换成完整配套链；不能再接旧 injector 重复展开。
每模块需独立开关、最小正负例、失败诊断、设备证据和回滚边界，默认切换必须有数据支持。

## 11. 静态统计与后续复核方法

源码规模是快照事实；下列只读方法可复现，执行目录为主仓或其 TVM 子模块对应位置：

```bash
wc -l src/ascend/transform/insert_sync.cc \
  src/ascend/transform/auto_schedule/dependency_analysis.cc \
  src/ascend/transform/auto_schedule/dependency_analysis.h
wc -l 3rdparty/tvm/src/target/z3/z3_prover_on.cc \
  3rdparty/tvm/src/target/z3/z3_prover_off.cc
rg -n '^#include|Z3Prover::|z3_prover|USE_Z3' \
  3rdparty/tvm/src/target/z3 3rdparty/tvm/include/tvm/arith/analyzer.h \
  3rdparty/tvm/src/arith/analyzer.cc 3rdparty/tvm/CMakeLists.txt
```

闭包核对先列直接 include，再递归解析相对路径及 `src/` 路径，区分源码、header 和外部库。
统计不会证明这些文件都必须原样搬运，也不能替代编译、链接、协议验证或设备测试。
实施前以实际 phase.py、OperationConfig、helper/codegen 和固定源快照再次核对契约。

TVM 历史比较使用本地已有对象，不切换分支：

```bash
git -C 3rdparty/tvm merge-base \
  c2921fdaf795b1103d21abc962e83a209c7258d7 \
  4211874e9e4b8770fd20ad0d98ab80afd1df5028
git -C 3rdparty/tvm rev-list --left-right --count \
  c2921fdaf795b1103d21abc962e83a209c7258d7...4211874e9e4b8770fd20ad0d98ab80afd1df5028
```

源码统计先用 `rg --files tilelang src` 限定可见文件，再分别筛选 `.py`、`.cc/.h`，
以 `rg -l` 按每种模式独立计数。主要模式：Python 的 `from tvm import ... tir` /
`from tvm.tir` / `tvm.tir`；C++ 的 `tvm/tir/` / `namespace tir` / `tir::`；内部头路径
前缀 `tir/`、`runtime/`、`arith/`、`target/`、`support/`、`node/`、`ir/`、`meta_schedule/`。
数字反映本次快照和搜索模式，不是所有间接依赖的完整清单。
