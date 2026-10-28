# A2/A3 Ascend 调度能力迁移计划与进展

将主仓 Ascend 的任务依赖分析、自动同步、AutoSchedule 和循环调度实践，
逐步迁入 tilelang-ascend 的 A2/A3（910B/910C）执行路径。每个独立大步骤
完成后更新本文件、提交并推送，等待用户审核与明确的继续指令。

当前有效验收口径：用户已明确本环境为 A3，后续跳过 A2 专属测试，A2 不再作为各步
验收门槛。保留既有 A2 兼容代码和历史测试事实；共享源码数据不表示 A2 设备已验收。

## 1. 仓库与基线

| 项目 | 固定信息 |
| --- | --- |
| 推送 remote | erhsh |
| remote 地址 | https://github.com/erhsh/tilelang-ascend.git |
| 工作分支 | codex/ascend-a2a3-scheduler-migration |
| tilelang-ascend 固定源码基线 | 43b0b69720f479894a13c02746599eccee5c5769（来源：origin/ascendc_pto） |
| 主仓参考 | tile-ai/tilelang，1c0f402e166a7d0a055c991d2f82d2175a7917b7 |
| 技术参考文件 | [pass-design.md](pass-design.md) |

该完整提交号是迁移与回归比较的固定依据，不随分支 HEAD、提交合并或文档修订而改变。
变更源码基线须单独说明影响并经用户确认。

实施基线的自动 C/V 分域默认开启，跨核同步、核内同步和自动内存规划策略默认关闭。
源码已有资源归属验证、
Vector 指令选择、mask 合法化、Persistent 参数/partial-wave 保护及 L1 多版本修复。
这些设施的具体合同与验证范围以 S1 审计和后续记录为准，优先复用而非重复迁入。

## 2. 提交与人工审核规则

1. 一次只开展用户明确允许的一个大步骤。技术上可以并行的任务，也不因此获得
   跨步骤自动执行的授权。
2. 每步包含该步实现、必要回归、文档和本计划进展更新。一个大步骤可以包含
   多个紧密相关的文件或局部修改，不按每个小编辑单独打断用户。
   每步末尾还须设计验证脚本；S1–S8 实施交付时，实际提交该步可独立运行的
   2–3 个验证入口，覆盖正常用例、边界或负例。涉及设备执行的步骤须提供
   A3 NPU 验证入口。脚本可封装已有 pytest 用例，不重复实现同一套断言。
3. 完成后检查提交范围，单独 commit 并 push 到本分支；同时报告提交号、
   变更摘要、真实验证结果及剩余问题。每步至少有一个可独立审核的提交。
4. 推送后停止。没有反馈、经过一段时间或仅收到一般性确认，都不代表可以开始
   下一步；收到用户明确要求继续后，才启动下一步。
5. 用户指出问题时，先在当前步骤修复、重跑受影响检查、更新计划并推送修复提交，
   再等待复核。没有必要时不重复无关测试。
6. 缺少硬件/native 验证时，可以推送供审阅的候选实现，但必须标成“待环境验证”，
   不标为该步验收完成。当前只要求 A3 结果，A2 历史结果保留；测试范围调整不删除
   A2 兼容代码，也不构成 A2 新能力的验证结论。
7. 不自动开启新策略为默认，不自动合并到默认分支。未经用户明确授权，不重写
   已审核提交或强制推送。基线升级、默认行为改变或新增目标平台需等待用户指令。

计划只保留当前范围、进展、验收证据及有效决策；提交详情查看 Git 历史。

## 3. 当前进展

| 步骤 | 独立交付 | 实现状态 | 验证状态 | 人工审核 |
| --- | --- | --- | --- | --- |
| S0 | 迁移计划与技术参考入仓 | 文档已完成 | 文档静态检查通过 | 用户已确认，并明确授权 S1 |
| S1 | fork 基线审计与前置合同 | 审计、局部修复及三个入口已交付 | 主机通过；历史 A2/A3 native 各 105 项通过；A3 14 项各 100 轮通过 | 用户已明确授权 S2；A2 不再阻塞验收 |
| S2 | 能力、任务图与 solver 合同（M0） | 显式分析、能力模型、solver/verifier 和三个入口已交付 | solver 105、capability 42、native 171 及 runner 31 项通过；无新增设备执行路径 | 用户已明确授权 S3 |
| S3 | Parallel/Pipelined/Persistent 接口与安全性 | 循环接口、域保护、受限 Task/Stage 和三个入口已交付 | native 117 + 29、兼容回归 170、runner 44 项通过；A3 9 项、1000 次 launch/golden 通过 | 用户已明确授权 S4 |
| S4 | 单核约束规划与同步协议验证（M1） | opt-in constraint、最终协议验证和三个入口已交付 | 原生 45 + 50、兼容 170 + 117 + 29、runner 77 项通过；A3 8 项、800 次 launch/golden 通过 | 用户已明确授权 S5 |
| S5 | 跨迭代流水与多缓冲（M2） | A3有效成本、实际版本/配置证明及guarded波流水已交付 | 校准32、原生234、兼容411项通过；A3 26用例/2600次launch与golden、4组性能已记录 | 本次S5提交，等待用户审核 |
| S6 | owner/epoch、别名与生命周期（M3） | 未开始 | 未验证 | 未提交 |
| S7 | A2/A3 新同步生成器（M4） | 未开始 | 未验证 | 未提交 |
| S8 | 完整自动调度与混合 C/V 接管（M5） | 未开始 | 未验证 | 未提交 |

当前停止点：S5 实现与 A3 验收已完成；更新计划并提交推送后等待用户审核。
S6 尚未获得执行授权。
实施状态与验证状态分列：“已实现”不等于“native/NPU 验收通过”。

## 4. 分步交付与验收

### S0 计划和技术参考

- 将本计划与技术设计提交到指定工作分支。
- 明确实施基线、参考提交、审核流程、验证口径和停止点；基线改变时同步校正。
- 验收：提交只包含上述文档；远端分支存在且提交号与本地一致；报告链接。
- 停止：等待用户对计划的反馈以及是否继续 S1 的明确指令。

验证脚本设计（文档步骤，无需 NPU）：`s0_check_docs.py` 检查计划状态、链接、
代码围栏和步骤完整性；`s0_check_publication.py` 检查 remote/分支、提交范围与
本地/远端提交一致性。

### S1 fork 基线审计与前置合同

- 核对 43b0b697 的实际 Pass、默认配置、FFI、代码生成与现有回归，更新技术参考。
- 审计资源分类/操作效果、可选 workspace 参数、未知调用和手写状态的处理；
  决定后续规划器所需的最小前置保护及可共享接口。
- 核对 DMA、切片与 Buffer view 的 elem_offset，以及旧 annotate_address 的地址合同。
  记录实际 Bisheng 构建身份、有效编译参数和缓存层次，区分真实编译与缓存恢复。
- 回归已有 Persistent、L1 分形多版本、zero/one-trip 和物理地址复用合同。
  只修复确认的缺口，不重复移植已有设施。
- 验收：形成逐项源文件/配置/测试映射，明确需补入的保护及其实际位置；
  如果本步补生产修复，须有相应 native 回归，涉及设备行为则另列 A3 结果。
- 后续依赖：S2、S3 均以本步确认后的合同为准。

验证脚本设计：

| 入口 | 环境 | 独立验证内容与通过条件 |
| --- | --- | --- |
| s1_check_baseline.py | 主机 Python + Git | 核对实际基线、Pass/默认配置及测试映射，区分已有能力与真实缺口 |
| s1_check_legacy_lowering.py | 已构建 TVM/专仓，无需 NPU | 基础 Vector/GEMM/CV 的 legacy lowering 与确认后的基线回归；按支持的合同验证生成代码 |
| s1_run_baseline_npu.py | A3 NPU | 运行本步确认的基准算子，数值与重复 launch 正确；保留 S1 历史记录 |

### S2 A3 能力、任务图与 solver 合同

- 明确 UB/L1/L0 各池可用预算、合法 core/PIPE/搬运路由和事件域；
  不复用 950 容量、延迟或 flag spill 参数。
- 在专仓 lowered call 上构造受审计的任务、读写区间、别名和副作用模型；
  共享旧规划器可用的抽取逻辑，不复制一套矛盾分类。
- 区间端点形成前先提升内部索引算术位宽，保留 elem_offset 和切片偏移；
  配置寄存器 effects 与普通 buffer 依赖分开建模，不假定其状态可以多版本化。
- 迁移纯数字约束求解、旧 FFI 接入和独立结果 verifier；新依赖按需加载，
  无 solver、无解和超时给出明确诊断，legacy 不增加拒绝条件。
- 验收：RAW/WAR/WAW、资源冲突、未知调用、别名及无解/超时测试通过；
  静态 TIR 和 native 注册测试通过；默认/显式 legacy 行为保持一致。

验证脚本设计：

| 入口 | 环境 | 独立验证内容与通过条件 |
| --- | --- | --- |
| s2_check_solver.py | 主机 Python + solver 依赖，无需 TVM/NPU | DAG、RAW/WAR/WAW、资源冲突、配置寄存器 phase 的纯协议/约束、无解和受控超时；结果通过独立 verifier |
| s2_check_task_ir.py | 已构建 TVM/专仓，无需 NPU | 任务 effects、int32 边界、动态尾界、非零 elem_offset/连续切片、别名及 legacy 隔离；错误结果被拒绝 |
| s2_check_capabilities.py | 主机 Python，无需 NPU | 验证 A3 能力模型的预算、对齐、PIPE/搬运与事件域一致性；不把模型检查当作设备实测 |

### S3 循环接口与 Persistent 安全性

- 保留 Parallel 的 A2/A3 Vector lowering；追加外层调度注解时避免逐维重复。
- Pipelined 保留现有 num_stages/order/stage/sync/group/cross_interval，兼容补 annotations。
- Persistent 回归已有参数合法性与 partial-single-wave 保护，再补 num_stages/annotations
  和非整除 group 的有效 tile 谓词；只修复确认的安全缺口，静态非法域保留明确诊断。
- Task/Stage 的标记只与验证和消费器一起开放；不静默接受尚未实现的调度语义。
  Task 内部条件与候选任务选择分别分析，保持完整任务同步边界及单 PIPE/core 合同；
  未支持的控制流明确拒绝，PerCoreTask 仍后置。
- 验收：FFI/IR 注解与任务域覆盖测试通过，旧调用兼容；Persistent 的已有安全合同
  与新增尾块在 A3 做数值验证。注解透传不代表 wave 流水已通过，后者归 S5。

验证脚本设计：

| 入口 | 环境 | 独立验证内容与通过条件 |
| --- | --- | --- |
| s3_check_loop_ir.py | 已构建 TVM/专仓，无需 NPU | Parallel/Pipelined 参数兼容、注解位置、Task/Stage 合法与非法组合 |
| s3_check_persistent_domain.py | 已构建 TVM/专仓，无需 NPU | 从实际生成循环核对 tile 恰好覆盖一次；检查少于 core、非整除 group、单/多 wave 和非法域诊断 |
| s3_run_loop_npu.py | A3 NPU | 验证 Persistent 安全合同、尾块、有效 tile 输出及重复调用；不据此宣称 wave 流水已支持 |

### S4 单核约束规划与最终同步协议验证

- 在现有 PipelinePlanning 内增加显式选择的 constraint 策略，默认 legacy。
- 当前首片限 A3 AscendC、单 Vector、静态循环、单 stage/单 version；
  使用审计过的 GM→UB→Vector→GM，拒绝 mixed C/V、opaque/raw 和手写 flag 歧义。
- 输出现有 stage/order ABI：任务展开必须与 injector 一致；只运行一个 planner、
  一次旧 injector、一套旧同步，不引入额外异步队列语义。
- 在最终旧同步输出后验证事件 set/wait 配对和活跃区间；不把模分配 ID 视作安全证明。
  回归当前基线的同步行为，不为验证方便插入已确认冗余的等待。
- 将受支持任务图的 RAW/WAR/WAW 及最终地址复用依赖映射到最终代码，逐条确认
  由合法 PIPE 隐式顺序、barrier 或 set/wait 完成关系覆盖；配对正确不能代替依赖覆盖。
- 验收：策略配置和缓存隔离、ABI、拒绝条件、最终事件协议及 legacy 回归通过；
  A3 完成数值、尾块和重复 launch 验证后，才能标记本步验收完成。

验证脚本设计：

| 入口 | 环境 | 独立验证内容与通过条件 |
| --- | --- | --- |
| s4_check_pipeline_ir.py | 已构建 TVM/专仓，无需 NPU | strategy/cache 隔离、stage/order 的展开 ABI、单次规划/注入及不支持输入的诊断 |
| s4_check_sync_coverage.py | 已构建 TVM/专仓，无需 NPU | 最终依赖覆盖、事件配对和活跃槽复用；注入缺 wait/重复 set 等负例必须失败 |
| s4_run_vector_npu.py | A3 NPU | GM→UB→Vector→GM，覆盖数值、尾块和重复 launch；legacy 与 constraint 分别执行并比较 |

### S5 跨迭代流水和多缓冲

- 引入经 A3 校准的 latency/II、多个 stage 和旧 injector 的多版本展开。
- 回归已有 L1 分形 tail/stride 修复；仅补入 S1 确认仍缺失的必要修复。
- 计算真实 padding、对齐、临时区及物理版本占用；验证跨迭代事件活跃期。
- 开放手动 Stage/enable_offset 前，验证适用配置寄存器的 reaching setter、入口值
  和下一次物理覆盖顺序；条件 setter 或未知 effects 保持保守依赖。
- 单独验收带 guard 的 Persistent wave 流水，不由 S3 的接口测试推导其正确性。
- 验收：0/1/N trip、trip 少于 stage 数、非整除尾块、1/2/3 版本及容量边界通过；
  A3 数值、稳定性与同条件性能记录完整，性能收益不预设。

验证脚本设计：

| 入口 | 环境 | 独立验证内容与通过条件 |
| --- | --- | --- |
| s5_check_versions_ir.py | 已构建 TVM/专仓，无需 NPU | 实际版本/容量、L1 tail/stride、事件活跃期及配置寄存器跨 Stage/迭代覆盖；错误状态被拒绝 |
| s5_run_pipeline_npu.py | A3 NPU | 0/1/N trip、短循环、1/2/3 版本及尾块；数值正确且无复用污染/挂起 |
| s5_run_persistent_pipeline_npu.py | A3 NPU | 独立验证带 guard 的单/多 wave 流水，全部有效 tile 恰好执行一次且尾 wave 同步正确 |

### S6 生命周期与 owner/epoch

- 迁移 alias-aware owner、完整 write-before-read epoch 和版本索引合同。
- 校验初始化、生产完成、消费完成、下一次写入和退出路径；处理条件、嵌套、
  连续循环及共享存储的多视图，不将 buffer 名称当作独立性证明。
- 显式地址参与版本、布局和指针改写时，保持 storage/view 的基址与偏移一致，
  同步更新或清理分配元数据。精确 placement 是可选扩展，不作为 S4 的前置条件；
  如采用主仓新合同，须显式正版本数并验证完整物理区间，不能改变 legacy 地址语义。
- 本步先在 bridge 上分析和校验，不自动从旧 injector/同步器夺取生成所有权。
- 验收：readiness/reuse、guard、zero-trip 和跨 tile 状态测试通过；
  新旧逻辑与最终物理别名关系一致，A3 重复运行和复用压力测试通过。

验证脚本设计：

| 入口 | 环境 | 独立验证内容与通过条件 |
| --- | --- | --- |
| s6_check_owner_ir.py | 已构建 TVM/专仓，无需 NPU | owner/epoch、地址/版本/别名、raw pointer 与分配元数据；错误 owner 被拒绝，可选精确 placement 启用时拒绝未知项 |
| s6_check_lifecycle_guards.py | 已构建 TVM/专仓，无需 NPU | 条件、可空、嵌套/连续循环及退出路径的 readiness/reuse；不存在无来源 wait |
| s6_run_reuse_npu.py | A3 NPU | 随机 scratch、跨 tile、多次 launch 复用压力；数值正确且无跨轮污染 |

### S7 新同步生成器

- 先在固定保守调度下引入新生成器，再逐步开放重排；按 A3 事件域分配并诊断。
- 消费最终内存规划的地址/大小，重建物理别名和复用边；不能沿用早期逻辑图代替。
- 覆盖 GM workspace 的生产通知和消费释放；同一依赖只由一个自动同步器负责。
  VS/标量副作用未覆盖前，不移除其旧保护。
- 验收：核内/跨核 set/wait、事件预算、显式 ID 冲突、初始化与 drain、workspace
  双向握手通过；A3 长流水、多 core、重复 launch 无死锁或串扰。

验证脚本设计：

| 入口 | 环境 | 独立验证内容与通过条件 |
| --- | --- | --- |
| s7_check_event_budget.py | 已构建 TVM/专仓，无需 NPU | 事件域上限前/恰好上限/超限、显式 ID 冲突和物理地址别名；正确分配或明确拒绝 |
| s7_check_handoff_ir.py | 已构建 TVM/专仓，无需 NPU | GM workspace 生产通知/消费释放、初始化/drain、VS/标量保护及同步单一所有权 |
| s7_run_sync_stress_npu.py | A3 NPU | 长流水、多 core、重复 launch 与并发 stream；进程级超时检测，无挂起/串扰 |

### S8 完整调度与混合 C/V 接管

- 在已证明的任务、版本与同步合同上接入完整约束调度，扩展混合 C/V。
- 明确接管边界：同一区域退出旧 injector/旧自动同步，不叠加新旧版本管理。
- 保留 A2/A3 GM workspace 路径，不把 950 直连、VF 或 flag spill 当作可移植能力。
- 验收：GEMM、归约、GEMM+Vector/Attention 等代表性混合算子与旧 Expert 回归通过；
  A3 记录编译、host 与设备耗时及回归情况。PTO 若纳入，另列独立验收。
- 完成后仍保持 opt-in；是否改变默认策略另由用户决定。

验证脚本设计：

| 入口 | 环境 | 独立验证内容与通过条件 |
| --- | --- | --- |
| s8_check_integration_ir.py | 已构建 TVM/专仓，无需 NPU | 接管区域只保留一套规划/版本/同步；保留 A2/A3 搬运与 legacy 合同，不出现 950 专属调用 |
| s8_run_workloads_npu.py | A3 NPU | GEMM、归约、混合 C/V 和旧 Expert 回归；所有定义输出满足 golden，重复调用稳定 |
| s8_benchmark_npu.py | A3 NPU | 正确性前置通过后，在同输入/设备/计时法下分别记录编译、host、设备耗时与回归；不预设提速结论 |

## 5. 验证脚本交付规范

第 4 节的脚本名均为计划入口，拟位于 `scripts/ascend_migration/`。
每步实施时交付对应脚本和最短运行命令，并更新实际路径、用例与结果；
入口重命名也须同步更新。

- 每个脚本可从仓库根目录单独运行，不依赖先跑其他验证脚本或推进下一步骤。
  允许依赖本步已构建代码及明确列出的 Python/TVM/CANN 环境，不自动安装、
  重编译或修改用户的全局配置；验证用例可显式选择 legacy/constraint 等配置，
  并在结果中记录。环境问题应清楚报告。
- 纯主机、native 无 NPU、NPU 三类依赖分别说明。`--help` 不应初始化 NPU；
  复用已有 pytest 时精确选择本步用例，保留原断言而非再造一套相同测试。
  各入口可共享薄 runner 的环境检查、参数和报告逻辑，不重复建设测试框架；
  纯主机入口不导入 tilelang/TVM，native/NPU 入口要求原生构建与当前源码匹配。
- NPU 入口接受显式 `--platform a3`、`--device`、`--seed`、`--repeat`、
  `--timeout` 和 `--output`。本环境使用 a3；保留 a2 参数兼容的旧入口也不得仅改标签
  就将同一设备结果记成两代通过。主机入口只要求与其验证有关的参数。
- 退出码约定：0 为 PASS，1 为验证失败，2 为环境/参数错误，3 为整体 SKIP，
  124 为超时。包装 pytest 时需明确转换；未收集用例或全部跳过不能报告 PASS。
- PASS 仅表示该入口全部必跑用例实际通过。部分必跑用例未执行、仅 xfail、
  无设备或脚本未实现均不能记成 PASS；报告区分 FAIL、BLOCKED、SKIP 和
  NOT_IMPLEMENTED，整步验收还要核对所有要求的入口及 A3 结果。
- `--output` 写入机器可读 JSON，记录步骤、实际提交号、命令、平台/设备、
  源码脏状态、调度配置、Python/TVM/CANN/torch_npu 等适用环境版本、随机种子、
  用例通过/失败/跳过数和错误摘要。
  涉及设备编译时记录所选 Bisheng 路径与完整构建身份、解析后的有序 flags 及缓存模式；
  编译验证使用隔离缓存或显式绕过缓存，冷热缓存/恢复测试另列结果。
  不假定某一层缓存加入工具链身份就会使全部缓存自动失效，也不因此迁入主仓运行链。
  纯 IR 测试不要求不适用的设备工具链；基于实际编译的证据与缓存命中结果分别记录。
  大日志/生成源码可保存到旁边的用户指定目录，不提交二进制或运行产物。
- 脚本既要覆盖正常结果，也要有缺依赖、非法输入或错误调度等可判定的失败路径。
  脚本只检查自身文件存在、或能 import，而不验证本步合同，不算完成验收入口。
- 每步最终报告列出脚本、完整命令、真实结果及未执行原因。缺少 NPU 时仍交付
  可在目标环境运行的入口，但状态保持“未运行/待环境验证”。
- 可能挂起的 NPU 用例由独立进程隔离；超时后停止该组验证，不能继续复用尚未
  确认健康的设备上下文，也不把线程超时当成 NPU 任务已经终止。

以下命令仅说明拟议接口；须在相应步骤实际提交脚本后才能运行：

```bash
# 已构建 TVM/专仓，无需 NPU
python scripts/ascend_migration/s4_check_pipeline_ir.py --platform a3 --output /tmp/s4-ir.json

# 在实际 A3 环境运行；平台参数必须与设备匹配
python scripts/ascend_migration/s4_run_vector_npu.py --platform a3 --device 0 --seed 0 --repeat 100 --timeout 120 --output /tmp/s4-a3.json
```

## 6. 统一验证与进度记录

每次进展更新都填写以下信息；仅写实际执行结果：

| 记录项 | 必填内容 |
| --- | --- |
| 当前步骤 | S 编号、范围、实现状态和人工审核状态 |
| 变更 | 实际文件与行为，必要的技术方案调整 |
| 检查 | 真实命令或工具、环境、结果、失败原因 |
| 验证脚本 | 本步实际交付的入口、最短运行命令、适用环境及 JSON/日志路径；设计、已交付、已执行分别记录 |
| native | 构建/注册/lowering/codegen 是否验证，不能由 Python 语法检查代替 |
| A3 | 实际 CANN、设备、用例和结果；没有运行则标明未运行；A2 历史记录不作为门槛 |
| 发布 | 远端分支、本步骤交付链接与推送结果；提交详情查看 Git 历史 |
| 审核 | 用户反馈、修复内容、是否明确允许下一步 |

验证逐层推进：数据/solver → 静态 TIR → native 构建/lowering → 代码生成 →
A3 数值与同步压力 → 性能。优先验证真实依赖、协议和覆盖范围，不固定具体
flag 编号或整份生成源码。可能挂起的设备用例使用独立进程控制超时。

各步骤在对应的主机、native 或 Linux/CANN A3 环境中验收；缺少相应运行结果时
保持“待环境验证”。进展随交付更新第 3 节，并按本节记录实际验证证据。

## 7. S1 实际交付与验证记录

### 7.1 范围与有效决策

- 用户已确认 S0 计划并授权 S1。工作分支和固定源码基线保持第 1 节所列值；
  S1 交付时未开始 S2，也未新增调度策略、solver 或 Task/Stage 接口，默认配置不变。
- 技术参考第 11 节补齐实际 Pass 顺序、配置、旧 FFI、资源归属、workspace 参数、
  Persistent、L1 版本及地址复用的源码/回归映射。已有资源归属和 workspace 保护复用，
  不重复迁入；未知调用、复合 PIPE、寄存器状态及完整 effects 合同留给后续步骤。
- 确认并局部修复非零 `elem_offset` 被重复累加：DMA 源/目标/tmp、atomic 源/目标，
  以及三个 Python BufferRegion 指针 helper，在传给 `access_ptr` 前去掉已经计入的
  view 基址。回归覆盖零偏移、两端非零偏移、连续 view、1D/2D stride 和公开 fill 路径。
- 旧 `annotate_address` 仍是字节地址/复用合同；不引入新 placement API，不统一原有
  auto 对齐与 linear 保留地址的差异。非对齐地址的 native 数学检查不表示设备支持；
  设备偏移用例使用满足相应搬运/向量约束的对齐切片。

### 7.2 环境、构建和缓存证据

| 项目 | 实际记录 |
| --- | --- |
| 主机 | Python 3.12.9；torch 2.12.0+cpu；torch_npu 2.12.0；CANN 9.1.0 |
| TVM | 固定子模块 `c2921fdaf795b1103d21abc962e83a209c7258d7` 的临时副本，应用仓库自带两份补丁；包含 patched Python parser；原仓子模块保持干净 |
| 构建 | 显式 CMake/Ninja，Ascend 开启，CUDA/ROCM/LLVM/GTest/libbacktrace 关闭；构建产物在 `build/`，不覆盖包内旧库、不安装 Python 包、不修改全局配置 |
| 身份记录 | `/tmp/ascend-migration-s1/build/native-build.json`；核验源码、TVM Python/依赖、补丁、CMakeCache、构建日志及三个 `.so` 哈希；同时核对实际加载路径 |
| 真实设备 | A3 `Ascend910_9362`，逻辑 device 0；本机未提供 A2，不能据此宣称 A2 设备验收通过 |
| Bisheng | PATH 选择 `/home/developer/Ascend/cann-9.1.0/bin/bisheng`，realpath 为同一 CANN 的 `tools/bisheng_compiler/bin/bisheng`；clang 15.0.5，构建时间 2026-04-30T17:11:05+08:00 |
| 编译器哈希 | SHA256 `7ffc97205a0582ce00a9ec163530696bf3971b24669c43c1c02cc9c7b56f541f`；完整 `--version`、resource dir 和 CANN version.info 写入设备 JSON |
| 编译与缓存 | 每个设备用例新进程、新 KernelCache 目录并 disable_cache，直接编译新 kernel；记录实际 Bisheng argv、返回码、源码/产物哈希、有效有序 flags 和实际 pass_configs；Cython host wrapper 缓存另记，不当作设备编译证据 |

预装库虽可导入，但没有可信的当前源码构建记录，未作为最终 native 验收依据。
可信未修复新库的新增回归为 21 failed / 15 passed，证实偏移问题；修复后新增回归
A2/A3 各 36 项通过，并纳入下列各 105 项的完整 native 入口。
构建准备和 manifest 生成命令见技术参考第 11.5 节；实际配置、构建及复现日志保存在
`/tmp/ascend-migration-s1/build/`。首次 8 并发构建以 130 中断后用 16 并发续建成功；
一次与链接重叠的检查出现 `file too short`，等待构建成功结束后重新验证通过，未计作通过证据。

### 7.3 已交付入口与运行结果

以下保留 S1 当时执行的命令，含 A2 历史记录；后续不要求重跑 A2 专属检查。
运行前在仓库根目录执行 `source set_env.sh`。native/NPU 入口使用该次显式构建的记录：

```bash
export TL_ASCEND_MIGRATION_BUILD_MANIFEST=/tmp/ascend-migration-s1/build/native-build.json

python scripts/ascend_migration/s1_check_baseline.py --output /tmp/ascend-migration-s1/host.json
python scripts/ascend_migration/s1_check_legacy_lowering.py --platform a3 --output /tmp/ascend-migration-s1/native-a3.json
python scripts/ascend_migration/s1_check_legacy_lowering.py --platform a2 --output /tmp/ascend-migration-s1/native-a2.json
python scripts/ascend_migration/s1_run_baseline_npu.py --platform a3 --device 0 --seed 0 --repeat 100 --timeout 120 --output /tmp/ascend-migration-s1/s1-a3.json

# 负例：本机实际为 A3，必须拒绝误标成 A2，不是 A2 设备验收
python scripts/ascend_migration/s1_run_baseline_npu.py --platform a2 --device 0 --seed 0 --repeat 100 --timeout 120 --output /tmp/ascend-migration-s1/platform-a2-negative.json
```

| 检查 | 实际结果 | 证据 |
| --- | --- | --- |
| 纯主机基线审计 | PASS，18 项；不导入 TileLang/TVM/NPU | `host.json` |
| native，参数 a3 | PASS，105 passed / 0 failed / 0 skipped；必跑节点完整 | `native-a3.json` 及其 artifacts/JUnit |
| native，参数 a2 | PASS，105 passed / 0 failed / 0 skipped；必跑节点完整 | `native-a2.json` 及其 artifacts/JUnit |
| runner 主机回归 | PASS，20 项；含 skip/xfail、零收集、遗漏必跑节点、超时杀进程组、缺构建记录、旧源码/库身份及优化模式负例 | `runner-tests.xml` |
| 偏移用例定向设备复验 | PASS，DMA/atomic/fill 各实编一次、同一 kernel 100 轮 launch/golden | `offset-repeat100.json` |
| A3 NPU | PASS，单次完整入口 14/14、0 failed/skip；14 次真实冷编译，同一 kernel 各 100 次 launch/golden，共 1400 次 | `s1-a3.json`，artifacts 路径见该报告 |
| 实际 A3 误标为 A2 的负例 | 按预期 BLOCKED、退出 2；0 编译/launch，全部 14 项未执行 | `platform-a2-negative.json` |
| A2 NPU | 历史未运行；用户已取消本环境的 A2 验收门槛 | 不作为后续执行要求 |

上表证据文件均相对 `/tmp/ascend-migration-s1/`。

native 两个参数分别作用于新增 S1 fixtures；既有 fixtures 保留各自的 A2/A3/PTO
目标与配置，报告逐类注明。它们是主机 TIR/lowering/codegen 验证，没有调用 Bisheng
或执行 kernel，不能替代两代设备结果。

设备预检最初尝试将已有 IR-only 标量 C Persistent fixture 用于数值测试，发现部分
输出未写，随即停止该组；只确认该载体不适合本次数值验收，未据此认定生产 Persistent
算法有缺陷，也未把缓存行竞争推断当成已证实根因。设备入口改为复用现有
`test_tilelang_ascend_language_l1_to_l0.py` 的 GEMM Persistent kernel，显式匹配
launch core 数和 wave_size，分别检查 8 tile/8 core、4 tile/8 core 和 9 tile/4 core。
原有 IR fixture 和生产 Persistent 实现均保持原样；初次失败的 `s1-a3-smoke.json`
及修正后剩余用例预检的 `s1-a3-remainder-smoke.json` 均保留，不充作正式整组通过证据。

首次 100 轮设备运行在 DMA 偏移用例发现数值失败，前 11 项已通过，后续用例未执行。
该地址 factory 原为 native 生成代码检查设计，未提供 GM→UB→GM 的运行时同步；
首次 smoke 偶然通过不构成安全证明。仅在设备入口对 DMA/atomic/fill 偏移用例
显式开启已有 `tl.ascend_auto_sync` 和 `tl.ascend_memory_planning`，生产默认值和
native 地址断言不变；失败批次另存为诊断，修正后重新验证完整正式入口。
对照生成代码的 GM 地址仍为 `a[40]` / `o[24]`，两次 copy 之间新增既有
`MTE2_MTE3` set/wait 完成关系；该配置下三个偏移用例各 100 轮定向检查通过。
该失败批次保存在 `s1-a3-copy-unsynchronized-failure.json`，原 artifacts 目录保留；
后续每次入口调用创建独立 artifacts 目录，避免复跑覆盖旧日志。

runner 回归命令：

```bash
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 python -m pytest -q testing/python/ascend_migration/test_s1_runners.py --junitxml=/tmp/ascend-migration-s1/runner-tests.xml
```

格式检查限定本步文件。Python lint 通过；C++ 全文件 clang-format 检查只有固定基线
已有第 370 行的三条相同诊断，本步修改行没有新增诊断，未为此改写无关代码。
日志分别为 `build/clang-format-check.log` 和 `build/clang-format-baseline.log`。
完整格式检查记录为 `format-report.json` 和 `format-report.md`。
JSON/JUnit、生成源码、二进制和大日志均不提交。

### 7.4 审核与剩余验证

S1 实现、入口和审计记录已交付；A3 已完成上述验收用例。用户已明确后续只要求 A3
验证并授权开始 S2；历史 A2 设备未运行，不再作为 S1 的待验收条件。
本步提交只推送到 `erhsh/codex/ascend-a2a3-scheduler-migration`，提交详情查看该分支
Git 历史，推送结果在最终交付报告列出。不创建 PR、不合并、不强推；推送后停止，
等待用户审核和明确的下一步指令。

## 8. S2 实际交付与验证记录

### 8.1 范围和接口

- 用户已明确授权 S2，并取消后续 A2 专属测试与验收门槛；实现起点为 S1 提交
  `685bce5e`，第 1 节固定源码基线和主仓参考提交均不改变。
- 新增显式只读 `AnalyzeAscendSchedule(PrimFunc, platform="A3") -> Map` 和旧注册
  `tl.transform.AnalyzeAscendSchedule`。图保留 task/group/loop/guard/storage 与依赖，
  不修改原 IR，不接入 `phase.py`，不新增 PassContext 策略或默认行为。
- 共用原有资源归属分类、workspace 解析和 injector 分组规则；共享抽取保持旧逻辑。
  首片严格区分受审计的可调度组、保守分析结果与未知 effects 的明确拒绝。
- 新增独立 A3 capability profile，以及纯数字 straight/modulo solver 与不依赖 Z3
  的结果 verifier。整数协议不等于已生成正确的多版本、流水或同步代码；S3/S4 及
  后续执行接管不属于 S2。具体边界见技术参考第 12 节及实现骨架。
- Z3 只作为 `ascend-scheduler` 可选 extra；legacy 导入不探测 Z3。当前测试依赖安装
  在 `/tmp/ascend-migration-s2/venv`，未修改全局 Python 环境。

### 8.2 审计限制与修正记录

能力数据来自当前 AscendC 编译器预算和 CANN 9.1/dav-2201 声明，不是设备测量。
跨核事件的四位编码不能证明可自由分配的槽位，因此本步仍不支持跨核调度预算。
旧 auto allocator 的 UB 超量探针曾退出 139，根因未定位；正式容量负例验证新增
capability verifier 的结构化拒绝，不把该崩溃当成通过，不修改 default linear 行为。

实现评审复现过一个纯数字假 UNSAT：带 int64 完成时间上界和固定 manual stage 时，
II=1 可行，但二分搜索 `[1,16]` 会错误跳过该解。现改为全局 deadline 内升序搜索，
只有完整指定域均不可满足才返回 UNSAT，并永久保留该回归。还区分了 Z3 真正缺失、
导入损坏、UNKNOWN 和 TIMEOUT，独立 verifier 对故意漏掉资源对的候选仍能拒绝。

原生分析评审针对 managed GM operand、未声明 local pointer、view 扩张 allocation、
显式窄化 cast、DMA 参数隐藏内存/opaque effects、未知语义 attr 和条件 allocation
补入拒绝或保守处理。循环图仅声明单迭代分析，要求顺序迭代，不冒称已覆盖跨迭代
readiness/reuse；这些情况不影响 legacy 接受范围。

### 8.3 验证入口与结果

以下为本步交付的三个入口，均可从仓库根目录独立运行；native 入口要求显式构建后
的源码身份 manifest，脚本不自动重编译、安装依赖或修改全局配置。

```bash
source set_env.sh
export TL_ASCEND_MIGRATION_BUILD_MANIFEST=/tmp/ascend-migration-s2/native-build.json

/tmp/ascend-migration-s2/venv/bin/python scripts/ascend_migration/s2_check_solver.py --output /tmp/ascend-migration-s2/solver.json
/tmp/ascend-migration-s2/venv/bin/python scripts/ascend_migration/s2_check_capabilities.py --platform a3 --output /tmp/ascend-migration-s2/capabilities-a3.json
/tmp/ascend-migration-s2/venv/bin/python scripts/ascend_migration/s2_check_task_ir.py --platform a3 --output /tmp/ascend-migration-s2/task-ir-a3.json
```

| 检查 | 当前实际结果 | 证据 |
| --- | --- | --- |
| 纯 solver / verifier | PASS，105 项，含 13 项独立小规模穷举 oracle | `solver.json` |
| A3 capability | PASS，42 项，包含模型、预算、对齐、路由和事件域负例 | `capabilities-a3.json` |
| runner 隔离回归 | PASS，S1 20 项及 S2 11 项；覆盖必跑空文件不能躲在其他通过项后 | `runner-tests.xml` |
| native Task IR / FFI / legacy | PASS，171 项：Task IR 41、capability IR 16、solver FFI 9、S1 回归 105；无失败/跳过/漏必跑项 | `task-ir-a3.json` |

上表证据位于 `/tmp/ascend-migration-s2/`。原生构建沿用固定 TVM 提交的已补丁临时
副本，manifest 核验 C++/TVM 源码、patched Python、CMakeCache、日志和实际库路径。
新 API 不产生设备执行代码，本步未运行新的 NPU 调度验收，也不要求 A2 验证。
原生 source SHA256 为
`46aea1e2d37fee143fe4e3eb77b7c1575d7be6ee952075965b628334c9236ca3`，
153 个 C++/header 等构建输入；最终构建日志为
`build/native-final-boundary-build.log`，最终 JUnit/日志目录为
`task-ir-a3-artifacts-xxbb1dwu`。验证期间源码处于 S1 提交上的 dirty 工作区，
报告保留当时的 commit/dirty 状态与源码哈希；不能把报告中的旧提交号误当成
最终提交前没有修改源码。

初次 native 运行的测试 IR 构造错误（单元素 SeqStmt、缺少 IfThenElse 的 else 参数）
诊断保留在 `task-ir-a3-first-failure.json`；修正后 166 项通过的中间结果保存在
`task-ir-a3-before-final-boundary-checks.json`。最终 171 项另包含 nested Block 初始化/地址提示
守恒、Float/Bool count 拒绝和 cold legacy/Z3 隔离，不用较早报告替代最终验收。

runtime 依赖为原环境 Python 3.12.9，隔离 venv 中实际 Z3 为 4.15.4.0；真实缺 Z3
的原环境入口另验证为 BLOCKED/2，见 `solver-missing.json`。runner 回归命令为：

```bash
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 /tmp/ascend-migration-s2/venv/bin/python -m pytest -q testing/python/ascend_migration/test_s1_runners.py testing/python/ascend_migration/test_s2_entrypoints.py --junitxml=/tmp/ascend-migration-s2/runner-tests.xml
```

修改的 Python 文件 lint、新 Python 的 100 列格式检查，以及三个新 C++ 文件的
clang-format 检查均通过；`git diff --check` 通过。格式记录为
`format-report.json` / `format-report.md`，产物不入库。
本步只提交推送 S2，提交详情查看分支 Git 历史，推送结果由最终报告列出；
S2 交付后已收到用户明确的 S3 执行指令，按本计划继续单步实施。

## 9. S3 实际交付与验证记录

### 9.1 范围与有效决策

- 实施起点为 S2 提交 `a56a4d13`，固定源码基线和主仓参考不变；按用户要求只做
  A3 验收，不新增 A2 专属测试。保留旧循环调用和 native FFI 参数兼容。
- Parallel/Pipelined/Persistent 增加 annotations，新增 metadata 只挂外层，保留
  原有 coalesced_width 行为；Python/native 双层拒绝保留调度字段注入。
- Persistent 支持末维非整除 group 的 padding 与有效 tile guard，内部索引算术
  提升至 int64；静态溢出明确诊断，符号正值的运行时范围仍是调用者合同。
- Persistent 的 num_stages 默认 0；正值只允许可证明无 guard 的单个完整 wave。
  多 wave、partial、非整除或无法证明上述条件的符号输入拒绝正值；wave 流水仍归 S5。
- Task 只允许一个 S2 已审计的 Vector call，Stage 只允许 0。早期检查结构、后期
  复用 S2 分析审计并消费；不开放 cost override、多调用/内部条件/Cube/手写同步、
  非零 Stage 或旧 stage/order 混用。不含标记的函数维持 legacy 路径。
- 实现位置、精确边界和未来扩展见技术参考第 13 节及公开语言说明。

### 9.2 独立入口与原生结果

```bash
source set_env.sh
export TL_ASCEND_MIGRATION_BUILD_MANIFEST=/tmp/ascend-migration-s3/native-build.json

/tmp/ascend-migration-s2/venv/bin/python scripts/ascend_migration/s3_check_loop_ir.py --platform a3 --output /tmp/ascend-migration-s3/loop-ir.json
/tmp/ascend-migration-s2/venv/bin/python scripts/ascend_migration/s3_check_persistent_domain.py --platform a3 --output /tmp/ascend-migration-s3/domain.json
python scripts/ascend_migration/s3_run_loop_npu.py --platform a3 --device 0 --seed 0 --repeat 100 --timeout 120 --output /tmp/ascend-migration-s3/s3-a3.json
```

manifest 必须由当前源码显式构建生成，不能复用 S2 manifest；创建方法沿用技术参考
第 11.5 节。入口不自动安装依赖或编译库。当前 native 使用既有隔离 venv，设备使用
原 Python 3.12.9、torch/torch_npu 2.12.0、CANN 9.1.0。

| 检查 | 实际结果 | 证据（相对 `/tmp/ascend-migration-s3/`） |
| --- | --- | --- |
| 循环与 Task/Stage IR | PASS，117 项：循环 74、标记 43；0 fail/skip | `loop-ir.json` |
| 实际 Persistent 域 | PASS，29 项：静态/符号正值、解释器故障注入及非法参数 | `domain.json` |
| S1/S2 native 兼容回归 | PASS，170 项，0 fail/skip/遗漏必跑项 | `s2-regression-a3.json` |
| runner 主机回归 | PASS，44 项：S1 20、S2 11、S3 13 | `runner-tests.xml` |
| A3 NPU | PASS，9/9 项；9 次真实冷编译、1000 次 launch/golden，0 fail/skip | `s3-a3.json` |

兼容回归比 S2 的 171 项少一项，是旧非整除 group 拒绝用例现已合法，改由 S3 的
真实域 exact-once 与数值验证覆盖；没有通过 skip 隐藏旧失败。大域溢出测试只检查
实际 IR 的位宽和首尾坐标，不枚举全部 tile。

最终 native source SHA256：
`daa2899ad727495a13162ad3083f7f6f37e9319204ad80d226e002acb430b940`，
155 个构建输入；manifest 为 `native-build.json`，构建日志为
`build-pending-final.log`。继续使用固定 TVM 提交及已补丁临时副本；验证报告记录
S2 HEAD 上的 dirty 工作区、源码哈希和实际库路径。报告中的提交号不是最终 S3
提交号，匹配依据是已核验的源码/库身份。

早期测试输出与中间构建仅作诊断；最终两个 native 入口及兼容回归均在上述冻结
manifest 下运行。另有针对 pending 后端门禁的回归：显式简化删除 dead marker 后
仍拒绝 PTO/A5；这证明跨改写的守恒，不把它表述为已复现标准 pipeline 绕过。

设备为实际 A3 `Ascend910_9362`、逻辑 device 0。六个静态 Persistent 域覆盖完整/少于
core/多 wave、内部 padding 和尾 wave，以及单 core：`[2,4]/8/g2`、`[2,2]/8/g2`、
`[3,3]/4/g8`、`[1,5]/8/g2`、`[3,10]/8/g4`、`[1,5]/1/g2`，各 100 轮。
符号 M 的同一已编译 kernel 以 M=128/384 两个正值绑定各运行 100 轮。
另有无 guard 单满 wave 的 num_stages=1 与消费 Task/Stage(0) 的 Vector add，
各 100 轮；全部逐次重置输出 sentinel 并比较 golden。前置 smoke 9/9 也通过，
但以完整 `s3-a3.json` 的 1000 次为本步设备证据，不把 smoke 次数叠加。

最终设备 artifacts 为 `s3-a3-artifacts-0k0wmop1`；每项保存真实编译 argv、有序
flags、resolved pass_configs、生成源码/产物 hash、工具链和实际加载身份。
所选 Bisheng 为 `/home/developer/Ascend/cann-9.1.0/bin/bisheng`，实际工具链
clang 15.0.5，compiler SHA256 为
`7ffc97205a0582ce00a9ec163530696bf3971b24669c43c1c02cc9c7b56f541f`。
各 worker 独立目录且 kernel 内存/磁盘缓存禁用；独立 host wrapper 缓存不充当
设备编译证据。所有设备 case 均匹配上述最终 source/库 manifest。

本步 Python lint、新 Python 的 100 列格式检查、四个修改/新增 C++/header 的
clang-format 检查，以及 `git diff --check` 均通过；文档相对链接和围栏检查通过。
记录为 `format-report.json` / `format-report.md`，日志、JSON、JUnit 和二进制不提交。

### 9.3 审核边界

S3 提交包含循环、验证/消费、测试、三个入口与文档；不接入 S4 constraint 策略，
不宣称跨 wave 流水、owner/epoch 或新同步器已实现。S3 已完成上述验证并按约推送；
用户随后明确授权 S4，按本计划继续单步实施。

## 10. S4 实际交付与验证记录

### 10.1 范围和配置

- 用户明确授权 S4，实施起点为 S3 提交 `8aa8b494`；固定源码基线、主仓参考和推送
  分支不变。当前 constraint 首片支持 A3 AscendC/auto，A2 保留 legacy，不做 A2 专属验收。
- `TL_ASCEND_PIPELINE_PLANNER` 选择 legacy/constraint，默认 legacy。原有 planner
  类保持，constraint 在同一入口分发，Python 仅该分支懒注册可选 solver。
- constraint 要求 auto_sync/memory_planning 开启、cross-core/VS 同步关闭；全内核
  受审计，至少一个静态 serial、num_stages=1 候选。只输出 stage 0/version 1 和
  旧 order ABI，不引入第二个 planner/injector/同步器。
- 最终只读验证位于最后 Simplify 与 ResourceScopeVerify 之间，通过稳定 task/iter
  身份检查原 logical 依赖和最终 UB 物理复用；按 set 时刻的完成快照审计事件配对、
  活跃槽、传递关系及 drain。相关 V→V 依赖需要显式完成证明，不加冗余等待来过检查。
- 静态 UB f16/f32 allocation 先逐维检查真实声明的乘积，再计算行 padding 和保守总预算，
  复用旧 allocator 的 196352 字节常量；最终地址/大小和访问范围再核验。手工 placement、未知 effects、
  多 stage/version、动态 padding DMA、跨 dataVar remap 和无法证明的控制流拒绝。
- 有限展开限额为每 trace 4096 硬件实例、65536 控制访问，单 loop extent≤65536。
  solver 的单位 latency/II 只用于抽象顺序，不是 A3 时延测量或性能收益承诺。
  完整合同见技术参考第 14 节与公开语言说明。

### 10.2 别名、尾块与验证载体

普通未声明 noalias 的 GM 参数保持保守；已确认 copy/add/copy 循环可能缺少上一轮
GM store 到下一轮未知别名 GM load 的完成路径，这类输入明确拒绝。
设备 fixture 显式声明 tir.noalias=True，A/B/O 三个实际独立 torch 分配满足该合同；
native 同时保留 noalias=False 的拒绝测试，不把独立分配事实推广给普通调用者。

尾块采用完整对齐短 tile：full192=64×3、tail160=64×2+32，后者独立尾 allocation
不自动代表物理 reuse。native 另用最终全/部分重叠地址的正负例证明 reuse 覆盖，
并验证物理顺序与原 logical 方向不能混淆。设备按 f16/f32、两种形状和两种策略
分别编译，同 CPU seed 生成输入，逐次重置输出 sentinel、比较 golden，并在入口
末端比较跨策略实际输入/输出 hash；仅相同 seed 或缓存禁用不能充当一致性证明。

### 10.3 独立入口和验证结果

```bash
source set_env.sh
export TL_ASCEND_MIGRATION_BUILD_MANIFEST=/tmp/ascend-migration-s4/native-build.json

/tmp/ascend-migration-s2/venv/bin/python scripts/ascend_migration/s4_check_pipeline_ir.py --platform a3 --output /tmp/ascend-migration-s4/pipeline-ir.json
/tmp/ascend-migration-s2/venv/bin/python scripts/ascend_migration/s4_check_sync_coverage.py --platform a3 --output /tmp/ascend-migration-s4/sync-coverage.json
/tmp/ascend-migration-s2/venv/bin/python scripts/ascend_migration/s4_run_vector_npu.py --platform a3 --device 0 --seed 0 --repeat 100 --timeout 120 --output /tmp/ascend-migration-s4/s4-a3.json
```

复用 S2 的隔离 venv，实际 Z3 为 4.15.4.0，仍继承原 torch/torch_npu/CANN 环境；
未安装全局依赖。三个入口的缺失/损坏 Z3 预检在 runtime 导入前给出 BLOCKED/2。
manifest 要由当前源码显式构建，不能复用 S3；方法沿用技术参考第 11.5 节。

| 检查 | 当前实际结果 | 证据（相对 `/tmp/ascend-migration-s4/`） |
| --- | --- | --- |
| 规划 ABI、拒绝边界和真实 cache key/路由 | PASS，45 项：planner 42、cache 3；0 fail/skip | `pipeline-ir.json` |
| 最终协议与 logical/physical 覆盖 | PASS，50 项，0 fail/skip | `sync-coverage.json` |
| S1/S2/S3 native 兼容 | PASS，170 + 117 + 29 项，0 fail/skip/漏必跑项 | `s2-regression-a3.json`、`s3-loop-regression-a3.json`、`s3-domain-regression-a3.json` |
| runner 主机回归 | PASS，77 项：S1 20、S2 11、S3 13、S4 33 | `runner-tests.xml` |
| A3 NPU | PASS，8/8 项；8 次真实冷编译、800 次 launch/golden，4 组跨策略输入/输出 hash 一致，0 fail/skip | `s4-a3.json` |

预算溢出修复后的最终 native source SHA256：
`a129ba375514d31697cddcad83ba67d681f38a6cbcf41abb29b1e6f1a55be5fb`，
158 个构建输入，`build-final.log` exit 0，`native-build.json` 已写入并核验。
旧 manifest 与报告以 `*-before-budget-overflow-fix.json` 保留为诊断。
固定 TVM 及已补丁临时副本沿用 S1，报告保存 S3 HEAD 上 dirty 工作区、源码/库身份与
真实加载路径；提交号不是最终 S4 提交号，匹配依据是已验证的 source/库 manifest。

首轮诊断发现迭代 marker 的 value 被旧 NarrowDataType 合法缩位后遭误拒，现允许合法
整数缩位并逐实例匹配数值；原始 ID node 保持 int64。synthetic fixtures 的旧 TVM
AttrStmt/call 构造错误也已修正，诊断日志保留，不用中间分项通过替代正式整组结果。

最终复核另确认巨大多维 UB shape 的容量可在旧 TVM 常量乘法中回绕为 16384 字节，
绕过分析后的预算检查。修复把逐维除法前置检查放在 Analyze 之前；Buffer 与 Allocate
声明各保留一个精确负例，均在进入旧 allocator 前拒绝。修复前已通过的原生和设备
报告以 `*-before-budget-overflow-fix.json` 保存为诊断，不计入修复后的最终验收。

最终设备 artifacts 为 `s4-a3-artifacts-a95k7t67`，实际 SoC `Ascend910_9362`、逻辑
device 0；Python 3.12.9、torch/torch_npu 2.12.0、CANN 9.1.0，Z3 API 版本 4.15.4。
PATH 所选 Bisheng 为 `/home/developer/Ascend/cann-9.1.0/bin/bisheng`，实际 clang
15.0.5，compiler SHA256 为
`7ffc97205a0582ce00a9ec163530696bf3971b24669c43c1c02cc9c7b56f541f`。
每项各 100 次 attempted/completed/golden，保存真实编译 argv、有序 flags、resolved
配置和生成源码/产物 hash；全部匹配最终 source/库 manifest。独立 worker 与缓存目录、
禁用 kernel 内存/磁盘缓存的控制沿用 S1/S3，host wrapper 缓存不计为设备编译。
上述 800 次仅统计预算修复后的正式入口，不叠加修复前或 smoke 的次数。

修改文件的 Python lint、8 个新 Python 的 100 列格式检查、6 个 C++/header 的
clang-format、`git diff --check` 及文档相对链接/围栏检查通过。记录为
`format-report.json` / `format-report.md`，JSON/JUnit、日志和二进制均不提交。

### 10.4 提交和审核边界

只提交 S4 的实现、永久测试、三个入口与文档；不推进 S5 的跨迭代流水、多版本、
硬件时延校准或新同步器。上述正式原生、A3 与格式检查已完成，按约提交推送到既定分支，
然后停止等待用户审核与明确的 S5 指令。

## 11. S5 实施记录

S5 实现、CPU 与 A3 设备验收已完成。本节记录实际证据与性能限制；未执行 A2 专项
测试。交付后停止等待用户审核，S6 尚未开始。

### 11.1 实现范围与边界

- 保持 legacy 默认与原规划/注入/同步流水。constraint 多 stage 接收完整、带摘要的
  `tl.ascend_pipeline_cost_model` Map；内部 PassConfig 使用该版 TVM 支持的 DictAttrs。
  Python loader 校验模型内容，模型内容进入既有缓存键。legacy 不导入 Z3。
- `num_stages=1/2/3` 是版本搜索上限；规划器与旧 injector 共用实际版本、shape、stride
  推导，并对修订后的实际 N 再次验证。最终 remap 与 Allocate 记录真实占用，不从请求数
  推断版本。版本基址统一使用 `(logical_iteration-min) % N`。
- 旧 injector 的 trip1、stage[0,1,2] 中间 unit 重复执行已复现并修复；分段不重叠，
  steady 不产生负 extent。0-trip 是显式 no-op，不宣称执行了多版本分配。
- 非零手动 Stage 需要 constraint 正候选及显式 `enable_offset=True`。Stage 保持源程序
  imperative 值语义；冲突排程必须 UNSAT。任一显式 Stage 使候选未标注 unit 取 Stage0；
  候选外非零 Stage 拒绝，Stage0 可按原顺序消费。Task 成本 override 仍拒绝。
- 仅支持 loop-private、单行连续、每版 32B 对齐的 f16/f32 UB。每迭代须先 full-write
  后读；full-write 后的完整 in-place 更新仍需 producer/slot 证明。外部只读 UB、
  共享写 UB、跨迭代 carry 不在本片范围，不扩展为 S6 通用 owner/epoch。
- 最终证明分别检查静态 core/vid 的源 producer 与实际同 slot 最近 writer、物理覆盖及
  事件完成关系。managed mask 使用每个 Vector 域的一份状态，Legalize 末端只读检查
  reaching setter/入口未知/下一次覆盖；不复制 mask 到 buffer version，不额外插同步。
  未审计配置寄存器、opaque 或条件 stateful 操作仍拒绝。
- guarded Persistent 使用纯 If-noelse 保留每 logical tile 的 padding/末 wave guard，
  不把 loop_break 搬入 stage。原 `num_stages=0` 语义保留。
- staging horizon 为 64，窗口×unit 上限 65536、任务实例上限 4096；整数域与容量在
  展开前检查。既有 L1 分形 tail/stride 仅做 legacy 回归，不开放 constraint Cube。

### 11.2 A3 有效成本的测量政策

校准在独立 S4 源码/三库快照中运行 legacy 微基准，SoC 为 Ascend910_9362，CANN 9.1.0。
manifest schema 2 明确标记 `relocated_verified_build`，保留原构建命令和
CMAKE_HOME_DIRECTORY，逐字节验证原 manifest/cache/log、相对源码及三库；不冒称快照
重新构建。普通 schema 1 仍严格匹配当前源码和库。

矩阵为四类 GM→UB、UB→GM、fill、add × f16/f32 × tile32/64 × dependent/independent，
共 32 个微基准，对应 16 条成本记录。采用同一 `msprof op` BasicInfo-only 政策：
N=0/32/128/256/512，每 N 每 round 5 次预热、20 次测量，3 rounds；每个微基准先逐 N
做 3 次数值校验。独立模式使用固定 128-tile arena，在有界批次复用处 drain。
每次应用调用有 journal，并与 main_kernel 的唯一 BasicInfo 行一一匹配；PMU replay
不计为独立样本。频率、SoC、CANN、bisheng SHA 和有序 flags 均入证据。

多 N 斜率包含 issue、同步、批次 drain 的有效成本；它不是原始指令延迟。
hierarchical bootstrap 纳入 round 漂移，要求 95% CI 下界正、相对半宽≤20%、
相对残差≤10%。最多两批，重试必须合并全部原始样本与 rounds 再判定，保留首批失败；
不丢弃失败批、不 clamp 为 1。取 CI 上界向上取整至 ns，`quantum_ns=1`。
小 N 的早期噪声试验与 PMU 诊断不混入正式成本表。

完整校准已实际通过：32/32，16 records；12750 次 profiling 应用调用、10200 个
measured samples。GM→UB 的 f32/tile32 与 f32/tile64 independent 各两批，均以合并
全部样本后的结果通过；其余单批。两项 add/f32/tile64 复用已接受的同政策 pilot，
已核对生成源码、工具链及身份，不重新计为额外样本。
模型为 `/tmp/ascend-migration-s5/a3-effective-cost-model.json`，canonical SHA256
`f2aa7ecfa4e592199c81801140903352f6e34fc1cee7be220987d6c5753a13fd`，
生产 loader 独立复核通过。实际 ordered flags 为 `[-O2]`，current/rated 频率均1650MHz。
bisheng SHA256 为
`7ffc97205a0582ce00a9ec163530696bf3971b24669c43c1c02cc9c7b56f541f`。
`calibration-acceptance.json` 逐项核对32份 accuracy、34份 profile worker 的引用 SHA、
编译及调用计数：480 次 accuracy launch/golden，profiling 的510次 post-group
golden，接受证据内66次真实 bisheng 编译。profiling 不冒称每次调用均做独立 golden。
模型文件 SHA256 为
`5549a92dc630e629252945d31dbefb73145570f7e6b7487a41083282dae3b569`。
本次矩阵由 `/tmp/ascend-migration-s5/run-calibration-matrix.py` 分阶段驱动；永久入口的
`--calibrate-profile` 封装同一 worker、BasicInfo parser、合并拟合及导出政策，用于后续
重现。公开控制器的失败路径与导出合同通过 host 测试。

### 11.3 对照基线

普通非零 trip 用同一逻辑 kernel 比较两种 planner。Persistent 的 legacy 基线使用
`num_stages=0` 的串行 wave；constraint 使用 2/3，因为 legacy 的既定合同拒绝 guarded
positive stages。两侧逻辑域、输入、golden 和编译 flags 相同，报告保留各自 directive，
不宣称两侧 stage 参数相同。

零次 legacy 原夹具留下死 UB allocation，旧 codegen 因缺 address_map 拒绝；该失败
作为诊断保留。专用 serial zero baseline 在前缀用 fill(3)、fill(4)、add、copy 产生7；
constraint 仍保留独立 anchor 和原零次 candidate。两者有效输出/未写 canary 相同，
元数据明确源程序差异；不通过共享候选 UB 弱化新模式的 loop-private 门禁。

### 11.4 最终 CPU 验证

当前构建证据为 `/tmp/ascend-migration-s5/native-build.json`，project source SHA256
`e125218f60dc09de220c5a3041c0854575ef6aa520d8bfc702cc9eb529f127c1`，
module SHA256 `118b8e2f4c4dcc17c3e71901ea611c8d7cd221a8a6407f5e8ae92ac37364347f`。
这是 guarded 仿射边界精确归一化后的构建：旧 TVM 未能从联合 guard 证明展开的字节
上界，修复利用精确整数余数归一化等价比较，仍要求 CanProve。9×1 合法域完整 lowering
及去 guard/真实越界拒绝3项已通过。

| 已执行检查 | 结果 | 证据 |
| --- | --- | --- |
| S5 原生 IR/最终同步/mask/成本协议/快照/cache/最终 trace | 234 PASS，0 skip | versions-ir.json |
| S1/S2 兼容 | 170 PASS，0 skip | legacy-s1-s2-ir.json |
| S3 loop 兼容 | 117 PASS，0 skip | legacy-s3-loop-ir.json |
| S1–S5 入口 host 回归 | 全量132 PASS；末次受影响18项复验PASS，去重133项 | cpu-validation-summary.json；两次实际命令/XML与节点均保留 |
| S3 domain 兼容 | 29 PASS，0 skip | legacy-s3-domain.json |
| S4 pipeline/sync 兼容 | 45 + 50 PASS，0 skip | legacy-s4-pipeline-ir.json、legacy-s4-sync.json |
| 完整真实成本模型预检 | 24 PASS | real-profile-ir.json；实际版本、完整lowering与最终GM写域 |

以上 JSON 位于 `/tmp/ascend-migration-s5/`。原生项含明确标记的 synthetic 成本协议
fixture，不构成硬件校准结果。真实成本矩阵与设备验收的独立证据见11.2、11.6。
旧523构建的12份正式报告/日志原字节保存在
`diagnostic-523/`，archive-index记录哈希；旧 manifest 身份也保留在旧报告内。
本地临时日志/JSON/二进制不提交。

`cpu-validation-summary.json` 汇总全部实际公开入口命令和节点。clang-format15检查7个
整文件及 ir.cc/inject_pipeline.cc 的改动范围通过；全部变更 Python 的 ruff检查通过。

真实模型预检中，steady1/2 分别物化1/2版本；steady3 的 manual(0,0,3,3) 加显式 offset
物化 A/B/C 各3份、每个缓冲区768B。trip1/2 的 constraint 夹具也实际物化3份；zero
为无 remap 的 no-op。这些是注入/分配证据，设备结果另行统计。

### 11.5 复核入口

使用既有 venv（含 Z3 4.15.4.0）与 CANN 环境；实际库加载路径由 manifest 预检核对。
以下入口均只接受 A3：

```bash
source set_env.sh
migration_python=/tmp/ascend-migration-s2/venv/bin/python
"$migration_python" scripts/ascend_migration/s5_check_versions_ir.py \
  --platform a3 --build-manifest /tmp/ascend-migration-s5/native-build.json \
  --output /tmp/ascend-migration-s5/review-versions-ir.json
"$migration_python" scripts/ascend_migration/s5_run_pipeline_npu.py \
  --platform a3 --device 0 --repeat 100 --timeout 180 \
  --build-manifest /tmp/ascend-migration-s5/native-build.json \
  --cost-profile /tmp/ascend-migration-s5/a3-effective-cost-model.json \
  --output /tmp/ascend-migration-s5/review-pipeline-npu.json
"$migration_python" scripts/ascend_migration/s5_run_persistent_pipeline_npu.py \
  --platform a3 --device 0 --repeat 100 --timeout 180 \
  --build-manifest /tmp/ascend-migration-s5/native-build.json \
  --cost-profile /tmp/ascend-migration-s5/a3-effective-cost-model.json \
  --output /tmp/ascend-migration-s5/review-persistent-npu.json
```

若需重新采集完整成本矩阵，可使用当前匹配构建或显式 `--calibration-source-root`
指定独立且经 manifest 验证的快照；该模式无需已有模型：

```bash
"$migration_python" scripts/ascend_migration/s5_run_pipeline_npu.py \
  --platform a3 --device 0 --timeout 180 --calibration-timeout 180 \
  --build-manifest /tmp/ascend-migration-s5/native-build.json \
  --calibrate-profile /tmp/ascend-migration-s5/review-cost-model.json \
  --output /tmp/ascend-migration-s5/review-calibration.json
```

### 11.6 A3 设备与性能结果

普通流水正式父入口 `pipeline-npu.json` 已 PASS（进程退出0）：18/18 用例、
18次真实 bisheng 编译、1800次同 kernel 调用及1800次逐轮 golden。8组实际输出
pair 的输入/golden/输出摘要一致；两项既有 L1 tail/parallel 各100轮也通过。
真实配置、kernel源码、实际 remap/Allocate 与加载库身份保存在逐用例证据。

Persistent 正式父入口 `persistent-npu.json` 已 PASS（进程退出0）：8/8 用例、8次真实
bisheng 编译、800次同 kernel 调用及800次逐轮 golden。single、partial f16、3×10
group tail 和修复后的9×1多 wave 两侧均通过；4组输入/golden/输出摘要一致，实际最终
GM store trace 的 valid tile exact-once 与 NaN canary 同时验证。

两入口总计26个数值用例、26次真实编译、2600次调用/逐轮 golden。另8个性能 worker
各独立编译，3 rounds ×（5预热+20测量）=75次应用调用，合计600次调用、480个
measured samples、24次 post-group golden；不把性能调用当作逐轮数值断言。
正式执行使用 device0、seed0、repeat100、timeout120 与本节 manifest/model，两个
父入口严格串行。`device-validation-summary.json` 汇总两父报告与逐 worker 证据。

| 夹具 | legacy median ns（95% CI） | constraint median ns（95% CI） | 观测 legacy/constraint |
| --- | --- | --- | --- |
| 普通 steady2 | 5901（5281–6781） | 5651（5401–5971） | 1.04423996 |
| 普通 tail_f16 | 4671（4141–5661） | 4431（4081–4821） | 1.05416385 |
| Persistent group_tail | 5781（5481–5911） | 6681（6561–6901） | 0.86528963 |
| Persistent partial_f16 | 4771（4691–4831） | 4761（4701–4971） | 1.00210040 |

性能来自 `msprof op` BasicInfo-only Task Duration；两侧输入/输出摘要、SoC、flags与
1650MHz频率一致。每策略独立冷编译，输入以同政策预热，不做 timed cache flush。
CI 为各策略 median 的 hierarchical bootstrap 区间；表中比值是观测 median 比值。
group_tail 的观测耗时增加约15.6%，作为本阶段已知性能退化保留；其余三组的区间有
重叠，不据此宣称稳健提速。性能收益不预设的验收约定保持，数值/覆盖/同步正确性与
性能观测分别记录。性能 helper 源SHA记录在父/子证据并核对一致。

### 11.7 交付停止点

本次交付仅包含 S5 实现、永久测试、三个公开入口及文档；legacy 仍为默认。
完整校准模型、原始采样、manifest、JSON和二进制保留于 `/tmp/ascend-migration-s5/`，
通过本节命令可复核。提交推送至 `erhsh/codex/ascend-a2a3-scheduler-migration` 后
停止，等待用户审核；S6 owner/epoch、通用别名和生命周期尚未获得执行授权。
