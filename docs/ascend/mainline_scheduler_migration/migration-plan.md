# A2/A3 Ascend 调度能力迁移计划与进展

将主仓 Ascend 的任务依赖分析、自动同步、AutoSchedule 和循环调度实践，
逐步迁入 tilelang-ascend 的 A2/A3（910B/910C）执行路径。每个独立大步骤
完成后更新本文件、提交并推送，等待用户审核与明确的继续指令。

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
这些表示源码设施已存在，不表示已在本迁移环境通过 native 或 A2/A3 验收。
S1 核对实际合同与回归覆盖，优先复用这些设施。

## 2. 提交与人工审核规则

1. 一次只开展用户明确允许的一个大步骤。技术上可以并行的任务，也不因此获得
   跨步骤自动执行的授权。
2. 每步包含该步实现、必要回归、文档和本计划进展更新。一个大步骤可以包含
   多个紧密相关的文件或局部修改，不按每个小编辑单独打断用户。
   每步末尾还须设计验证脚本；S1–S8 实施交付时，实际提交该步可独立运行的
   2–3 个验证入口，覆盖正常用例、边界或负例。涉及设备执行的步骤须提供
   A2/A3 NPU 验证入口。脚本可封装已有 pytest 用例，不重复实现同一套断言。
3. 完成后检查提交范围，单独 commit 并 push 到本分支；同时报告提交号、
   变更摘要、真实验证结果及剩余问题。每步至少有一个可独立审核的提交。
4. 推送后停止。没有反馈、经过一段时间或仅收到一般性确认，都不代表可以开始
   下一步；收到用户明确要求继续后，才启动下一步。
5. 用户指出问题时，先在当前步骤修复、重跑受影响检查、更新计划并推送修复提交，
   再等待复核。没有必要时不重复无关测试。
6. 缺少硬件/native 验证时，可以推送供审阅的候选实现，但必须标成“待环境验证”，
   不标为该步验收完成。A2 与 A3 的结果分别记录；缩小支持范围需用户明确同意。
7. 不自动开启新策略为默认，不自动合并到默认分支。未经用户明确授权，不重写
   已审核提交或强制推送。基线升级、默认行为改变或新增目标平台需等待用户指令。

计划只保留当前范围、进展、验收证据及有效决策；提交详情查看 Git 历史。

## 3. 当前进展

| 步骤 | 独立交付 | 实现状态 | 验证状态 | 人工审核 |
| --- | --- | --- | --- | --- |
| S0 | 迁移计划与技术参考入仓 | 文档已完成 | 文档静态检查通过 | 待用户确认并明确继续 |
| S1 | fork 基线审计与前置合同 | 未开始 | 未验证 | 未提交 |
| S2 | A2/A3 能力、任务图与 solver 合同（M0） | 未开始 | 未验证 | 未提交 |
| S3 | Parallel/Pipelined/Persistent 接口与安全性 | 未开始 | 未验证 | 未提交 |
| S4 | 单核约束规划与同步协议验证（M1） | 未开始 | 未验证 | 未提交 |
| S5 | 跨迭代流水与多缓冲（M2） | 未开始 | 未验证 | 未提交 |
| S6 | owner/epoch、别名与生命周期（M3） | 未开始 | 未验证 | 未提交 |
| S7 | A2/A3 新同步生成器（M4） | 未开始 | 未验证 | 未提交 |
| S8 | 完整自动调度与混合 C/V 接管（M5） | 未开始 | 未验证 | 未提交 |

当前停止点：S0 待审核；S1 仅在用户明确要求继续后启动。
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
  如果本步补生产修复，须有相应 native 回归，涉及设备行为则另列 A2/A3 结果。
- 后续依赖：S2、S3 均以本步确认后的合同为准。

验证脚本设计：

| 入口 | 环境 | 独立验证内容与通过条件 |
| --- | --- | --- |
| s1_check_baseline.py | 主机 Python + Git | 核对实际基线、Pass/默认配置及测试映射，区分已有能力与真实缺口 |
| s1_check_legacy_lowering.py | 已构建 TVM/专仓，无需 NPU | 基础 Vector/GEMM/CV 的 legacy lowering 与确认后的基线回归；按支持的合同验证生成代码 |
| s1_run_baseline_npu.py | A2/A3 NPU | 运行本步确认的基准算子，数值与重复 launch 正确；分别保存 A2、A3 的基线记录 |

### S2 A2/A3 能力、任务图与 solver 合同

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
| s2_check_capabilities.py | 主机 Python，无需 NPU | 分别验证 A2/A3 能力模型的预算、对齐、PIPE/搬运与事件域一致性；不把模型检查当作设备实测 |

### S3 循环接口与 Persistent 安全性

- 保留 Parallel 的 A2/A3 Vector lowering；追加外层调度注解时避免逐维重复。
- Pipelined 保留现有 num_stages/order/stage/sync/group/cross_interval，兼容补 annotations。
- Persistent 回归已有参数合法性与 partial-single-wave 保护，再补 num_stages/annotations
  和非整除 group 的有效 tile 谓词；只修复确认的安全缺口，静态非法域保留明确诊断。
- Task/Stage 的标记只与验证和消费器一起开放；不静默接受尚未实现的调度语义。
  Task 内部条件与候选任务选择分别分析，保持完整任务同步边界及单 PIPE/core 合同；
  未支持的控制流明确拒绝，PerCoreTask 仍后置。
- 验收：FFI/IR 注解与任务域覆盖测试通过，旧调用兼容；Persistent 的已有安全合同
  与新增尾块分别在 A2/A3 做数值验证。注解透传不代表 wave 流水已通过，后者归 S5。

验证脚本设计：

| 入口 | 环境 | 独立验证内容与通过条件 |
| --- | --- | --- |
| s3_check_loop_ir.py | 已构建 TVM/专仓，无需 NPU | Parallel/Pipelined 参数兼容、注解位置、Task/Stage 合法与非法组合 |
| s3_check_persistent_domain.py | 已构建 TVM/专仓，无需 NPU | 从实际生成循环核对 tile 恰好覆盖一次；检查少于 core、非整除 group、单/多 wave 和非法域诊断 |
| s3_run_loop_npu.py | A2/A3 NPU | 验证 Persistent 安全合同、尾块、有效 tile 输出及重复调用；不据此宣称 wave 流水已支持 |

### S4 单核约束规划与最终同步协议验证

- 在现有 PipelinePlanning 内增加显式选择的 constraint 策略，默认 legacy。
- 首片限 A2/A3 AscendC、单 Vector、静态循环、单 stage/单 version；
  使用审计过的 GM→UB→Vector→GM，拒绝 mixed C/V、opaque/raw 和手写 flag 歧义。
- 输出现有 stage/order ABI：任务展开必须与 injector 一致；只运行一个 planner、
  一次旧 injector、一套旧同步，不引入额外异步队列语义。
- 在最终旧同步输出后验证事件 set/wait 配对和活跃区间；不把模分配 ID 视作安全证明。
  回归当前基线的同步行为，不为验证方便插入已确认冗余的等待。
- 将受支持任务图的 RAW/WAR/WAW 及最终地址复用依赖映射到最终代码，逐条确认
  由合法 PIPE 隐式顺序、barrier 或 set/wait 完成关系覆盖；配对正确不能代替依赖覆盖。
- 验收：策略配置和缓存隔离、ABI、拒绝条件、最终事件协议及 legacy 回归通过；
  A2、A3 分别完成数值、尾块和重复 launch 验证后，才能标记本步验收完成。

验证脚本设计：

| 入口 | 环境 | 独立验证内容与通过条件 |
| --- | --- | --- |
| s4_check_pipeline_ir.py | 已构建 TVM/专仓，无需 NPU | strategy/cache 隔离、stage/order 的展开 ABI、单次规划/注入及不支持输入的诊断 |
| s4_check_sync_coverage.py | 已构建 TVM/专仓，无需 NPU | 最终依赖覆盖、事件配对和活跃槽复用；注入缺 wait/重复 set 等负例必须失败 |
| s4_run_vector_npu.py | A2/A3 NPU | GM→UB→Vector→GM，覆盖数值、尾块和重复 launch；legacy 与 constraint 分别执行并比较 |

### S5 跨迭代流水和多缓冲

- 引入经 A2/A3 校准的 latency/II、多个 stage 和旧 injector 的多版本展开。
- 回归已有 L1 分形 tail/stride 修复；仅补入 S1 确认仍缺失的必要修复。
- 计算真实 padding、对齐、临时区及物理版本占用；验证跨迭代事件活跃期。
- 开放手动 Stage/enable_offset 前，验证适用配置寄存器的 reaching setter、入口值
  和下一次物理覆盖顺序；条件 setter 或未知 effects 保持保守依赖。
- 单独验收带 guard 的 Persistent wave 流水，不由 S3 的接口测试推导其正确性。
- 验收：0/1/N trip、trip 少于 stage 数、非整除尾块、1/2/3 版本及容量边界通过；
  A2/A3 数值、稳定性与同条件性能记录完整，性能收益不预设。

验证脚本设计：

| 入口 | 环境 | 独立验证内容与通过条件 |
| --- | --- | --- |
| s5_check_versions_ir.py | 已构建 TVM/专仓，无需 NPU | 实际版本/容量、L1 tail/stride、事件活跃期及配置寄存器跨 Stage/迭代覆盖；错误状态被拒绝 |
| s5_run_pipeline_npu.py | A2/A3 NPU | 0/1/N trip、短循环、1/2/3 版本及尾块；数值正确且无复用污染/挂起 |
| s5_run_persistent_pipeline_npu.py | A2/A3 NPU | 独立验证带 guard 的单/多 wave 流水，全部有效 tile 恰好执行一次且尾 wave 同步正确 |

### S6 生命周期与 owner/epoch

- 迁移 alias-aware owner、完整 write-before-read epoch 和版本索引合同。
- 校验初始化、生产完成、消费完成、下一次写入和退出路径；处理条件、嵌套、
  连续循环及共享存储的多视图，不将 buffer 名称当作独立性证明。
- 显式地址参与版本、布局和指针改写时，保持 storage/view 的基址与偏移一致，
  同步更新或清理分配元数据。精确 placement 是可选扩展，不作为 S4 的前置条件；
  如采用主仓新合同，须显式正版本数并验证完整物理区间，不能改变 legacy 地址语义。
- 本步先在 bridge 上分析和校验，不自动从旧 injector/同步器夺取生成所有权。
- 验收：readiness/reuse、guard、zero-trip 和跨 tile 状态测试通过；
  新旧逻辑与最终物理别名关系一致，A2/A3 重复运行和复用压力测试通过。

验证脚本设计：

| 入口 | 环境 | 独立验证内容与通过条件 |
| --- | --- | --- |
| s6_check_owner_ir.py | 已构建 TVM/专仓，无需 NPU | owner/epoch、地址/版本/别名、raw pointer 与分配元数据；错误 owner 被拒绝，可选精确 placement 启用时拒绝未知项 |
| s6_check_lifecycle_guards.py | 已构建 TVM/专仓，无需 NPU | 条件、可空、嵌套/连续循环及退出路径的 readiness/reuse；不存在无来源 wait |
| s6_run_reuse_npu.py | A2/A3 NPU | 随机 scratch、跨 tile、多次 launch 复用压力；数值正确且无跨轮污染 |

### S7 新同步生成器

- 先在固定保守调度下引入新生成器，再逐步开放重排；按 A2/A3 事件域分配并诊断。
- 消费最终内存规划的地址/大小，重建物理别名和复用边；不能沿用早期逻辑图代替。
- 覆盖 GM workspace 的生产通知和消费释放；同一依赖只由一个自动同步器负责。
  VS/标量副作用未覆盖前，不移除其旧保护。
- 验收：核内/跨核 set/wait、事件预算、显式 ID 冲突、初始化与 drain、workspace
  双向握手通过；A2/A3 长流水、多 core、重复 launch 无死锁或串扰。

验证脚本设计：

| 入口 | 环境 | 独立验证内容与通过条件 |
| --- | --- | --- |
| s7_check_event_budget.py | 已构建 TVM/专仓，无需 NPU | 事件域上限前/恰好上限/超限、显式 ID 冲突和物理地址别名；正确分配或明确拒绝 |
| s7_check_handoff_ir.py | 已构建 TVM/专仓，无需 NPU | GM workspace 生产通知/消费释放、初始化/drain、VS/标量保护及同步单一所有权 |
| s7_run_sync_stress_npu.py | A2/A3 NPU | 长流水、多 core、重复 launch 与并发 stream；进程级超时检测，无挂起/串扰 |

### S8 完整调度与混合 C/V 接管

- 在已证明的任务、版本与同步合同上接入完整约束调度，扩展混合 C/V。
- 明确接管边界：同一区域退出旧 injector/旧自动同步，不叠加新旧版本管理。
- 保留 A2/A3 GM workspace 路径，不把 950 直连、VF 或 flag spill 当作可移植能力。
- 验收：GEMM、归约、GEMM+Vector/Attention 等代表性混合算子与旧 Expert 回归通过；
  A2/A3 各自记录编译、host 与设备耗时及回归情况。PTO 若纳入，另列独立验收。
- 完成后仍保持 opt-in；是否改变默认策略另由用户决定。

验证脚本设计：

| 入口 | 环境 | 独立验证内容与通过条件 |
| --- | --- | --- |
| s8_check_integration_ir.py | 已构建 TVM/专仓，无需 NPU | 接管区域只保留一套规划/版本/同步；保留 A2/A3 搬运与 legacy 合同，不出现 950 专属调用 |
| s8_run_workloads_npu.py | A2/A3 NPU | GEMM、归约、混合 C/V 和旧 Expert 回归；所有定义输出满足 golden，重复调用稳定 |
| s8_benchmark_npu.py | A2/A3 NPU | 正确性前置通过后，在同输入/设备/计时法下分别记录编译、host、设备耗时与回归；不预设提速结论 |

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
- NPU 入口接受显式 `--platform a2|a3`、`--device`、`--seed`、`--repeat`、
  `--timeout` 和 `--output`。核对实际设备与平台，A2/A3 独立运行，不能仅改标签
  就将同一设备结果记成两代通过。主机入口只要求与其验证有关的参数。
- 退出码约定：0 为 PASS，1 为验证失败，2 为环境/参数错误，3 为整体 SKIP，
  124 为超时。包装 pytest 时需明确转换；未收集用例或全部跳过不能报告 PASS。
- PASS 仅表示该入口全部必跑用例实际通过。部分必跑用例未执行、仅 xfail、
  无设备或脚本未实现均不能记成 PASS；报告区分 FAIL、BLOCKED、SKIP 和
  NOT_IMPLEMENTED，整步验收还要核对所有要求的入口及 A2/A3 结果。
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
python scripts/ascend_migration/s4_check_pipeline_ir.py --platform a2 --output /tmp/s4-ir.json

# 分别在实际 A2 和 A3 环境运行；平台参数必须与设备匹配
python scripts/ascend_migration/s4_run_vector_npu.py --platform a2 --device 0 --seed 0 --repeat 100 --timeout 120 --output /tmp/s4-a2.json
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
| A2 / A3 | 各自的 CANN、设备、用例和结果；没有运行则标明未运行 |
| 发布 | 远端分支、本步骤交付链接与推送结果；提交详情查看 Git 历史 |
| 审核 | 用户反馈、修复内容、是否明确允许下一步 |

验证逐层推进：数据/solver → 静态 TIR → native 构建/lowering → 代码生成 →
A2/A3 数值与同步压力 → 性能。优先验证真实依赖、协议和覆盖范围，不固定具体
flag 编号或整份生成源码。可能挂起的设备用例使用独立进程控制超时。

各步骤在对应的主机、native 或 Linux/CANN A2/A3 环境中验收；缺少相应运行结果时
保持“待环境验证”。进展随交付更新第 3 节，并按本节记录实际验证证据。
