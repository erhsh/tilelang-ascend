# S2 实现骨架：显式分析与数字协议

确认依据：已批准的 S2 实施方案及本轮“Implement the plan”。本步包括实现、测试、
原生构建与验证，不额外设置骨架确认停点。

## 原生分析

- `AnalyzeAscendSchedule(PrimFunc, platform="A3") -> Map`，注册名
  `tl.transform.AnalyzeAscendSchedule`；不构造 Pass，不接入 `phase.py`。
- `src/transform/ascend_schedule_analysis.cc` 使用只读 visitor 与 analyzer，收集
  task/storage/group/loop/dependency，内部算术先提升 int64，再计算字节半开区间。
- task 对应硬件调用，group 保留 injector 的不可拆直接子语句，loop/guard/原语句引用
  不丢失。ID 按源码遍历生成，storage 依据 data Var 身份，而非名字。
- 共用 `common/ascend_resource.h` 的既有 owner 分类，以及
  `common/ascend_pipeline_structure.h` 的既有 injector 分组规则；workspace 参数继续
  通过 `ResolveOperationConfig` 解析。共享抽取保持 legacy 语义。
- 首片只允许受审计的整块、连续、合法对齐 GM↔UB 与 managed fill/add。未知 effects
  明确报错；可完整分析但尚不能调度的复杂 group 显式标记不可调度。
- 返回版本、平台、tasks、storages、groups、loops、dependencies；保留 TVM 的
  Var/Stmt/PrimExpr，不能返回伪装成成功的部分图，也不生成 JSON 版 TIR。

## 能力与求解

- `ascend_schedule_capabilities.py` 提供纯标准库 A3 profile；区分编译器预算、工具链
  声明和设备验证，跨核预算未知。对齐、容量、路由与核内事件域验证仅用于显式新接口。
- `ascend_z3_scheduler.py` 为独立单文件协议、solver 与整数 verifier。mode 为 straight
  或 modulo；数据是 latency/II、资源、依赖、容量和 phase 等普通整数，不接受符号 TIR。
- Z3 仅首次求解时加载；结果区分 SAT、UNSAT、TIMEOUT、UNKNOWN、缺依赖和错误。
  SAT 保留原始 starts，必须通过不依赖 Z3 的整数 verifier；无未经验证的兜底调度。
- `ascend_schedule_solver_ffi.py` 显式注册 `tl.ascend.solve_schedule`；旧 containers
  严格转成 plain data 后调用 solver，不自动从分析接口求解。
- `setup.py` 只新增可选 extra `ascend-scheduler`，不改变基础安装依赖。

## 消费与生成边界

消费 lowered calls、buffer/view、guard 和循环域；不更改输入 IR、现有 attrs、地址、
pipeline 注解、版本或同步。原有默认配置不变；A2 兼容代码保留，验收仅记录 A3。
S3 循环接口、S4 planner 接管及后续同步生成仍不在本步实施。

## 验证和交付

先做注册/直线图冒烟，再完成作用域、别名、动态范围和错误路径；修改 C++ 后重建
本地库并更新源码身份 manifest。三个独立入口为 `s2_check_solver.py`、
`s2_check_task_ir.py`、`s2_check_capabilities.py`，复用既有薄 runner。
本轮同时交付永久测试，覆盖独立 verifier 拒绝损坏解、受控错误状态、分析只读性、
能力边界和 legacy 隔离。完成后更新迁移记录，单独提交推送 S2 并等待审核。
