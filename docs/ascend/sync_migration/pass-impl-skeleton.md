# Ascend InsertSync 后向移植实现骨架

状态：已落地可选非流水后端、真实标量快照和静态双缓冲首片；本页保留完整目标的接口
骨架，部分伪接口未逐字采用。
实际入口、已验证范围和未完成项见[实现进展](implementation-status.md)及源码头文件。
语义与范围以相邻[迁移设计](pass-design.md)为准，论证和成本见[调研报告](research-report.md)。

## 1. 文件清单

以下新增文件名是拟议接口，不表示已经存在。

| 文件 | 变更 |
| --- | --- |
| `src/transform/ascend_insert_sync.cc` | 新统一 Pass 入口、配置汇总与幂等验证 |
| `src/transform/ascend_sync/upstream/insert_sync.{cc,h}` | 主仓同步核心，暴露窄接口并切断新 IR codec/op 检测依赖 |
| `src/transform/ascend_sync/upstream/dependency_analysis.{cc,h}` | 主仓依赖分析，适配证明与存储身份 |
| `src/transform/ascend_sync/upstream/ir_structure.{cc,h}` | 主仓任务/控制树必要子集 |
| `src/transform/ascend_sync/upstream/multi_buffer.h` | owner/version/epoch 模型 |
| `src/transform/ascend_sync/upstream/buffer_lifetime_analysis.h` | 主仓生命周期及完成关系模型 |
| `src/transform/ascend_sync/upstream/buffer_alias_context.h` | 提取核心继承的 context/类型编译依赖，关闭 storage-alias 规划及输出，不关闭同步或 flag 安全证明 |
| `src/transform/ascend_sync/upstream/constraints.{cc,h}` | 旧 TIR 上下文、guard、mutable-read snapshot 兼容 |
| `src/transform/ascend_sync/legacy_adapter.{cc,h}` | 导入/导出传统 TIR，不决定依赖或分配事件 |
| `src/transform/ascend_sync/physical_storage.{cc,h}` | maps、scope、byte footprint、物理冲突 oracle；与逻辑 owner/epoch 身份分离 |
| `src/transform/ascend_sync/target_policy.{cc,h}` | V/S、同 pipe、容量、core/bank、动态 ID 支持策略 |
| `src/transform/ascend_sync/proof_adapter.{cc,h}` | 将同步上下文送入 Z3ProofService，旧 Analyzer 辅助简化、int64 区间运算和未证明结果处理 |
| `src/transform/ascend_sync/z3_proof_service.{cc,h}`、`z3_proof_service_off.cc` | 回移主仓 Z3 核心及禁用 stub，适配旧 TIR/容器/hash，保留 SMT 翻译和上下文 journal |
| `src/transform/ascend_sync/provenance.{cc,h}` | 流水元数据协议及生命周期验证 |
| `src/transform/common/operation_config.h` | 补齐 importer 所需 effect，不建立第二套操作分类目录 |
| `src/transform/inject_pipeline.cc` | S3 发布实际流水 provenance，不改变既有版本算法 |
| `src/transform/flatten_buffer.cc`、`ascend_storage_rewrite.cc` 等 | S3 在实际改写 buffer/任务处传播 provenance，逐处测试 |
| `CMakeLists.txt`、Z3 查找/打包构建辅助 | 显式加入新目录源文件、发现并链接 Z3、验证安装后运行库路径 |
| `tilelang/transform/__init__.py` | 新 `AscendInsertSync` 封装，最终两个旧 API 成为弃用 alias |
| `tilelang/transform/pass_config.py` | 过渡 backend 配置，最终删除；保留现有 ordinary/VS 配置 |
| `tilelang/engine/phase.py` | 过渡互斥分派，最终单入口替换两个调用 |
| `src/transform/ascend_sync_insert.cc`、`ascend_sync_insert_vs.cc` | 对照期保留，完整验收后删除算法 |
| `docs/ascend/sync_migration/upstream-sources.md` | S1 记录逐文件来源 hash 与语义适配说明 |

`upstream` 目录保留原许可和来源。内部 namespace 使用 `tvm::tl::ascend_sync`，避免与
旧枚举、函数、配置注册重名。必要的 buffer-version/core-mask 小类型随模型迁移，
不引入主仓 target/runtime/layout 模块。Z3 证明核心单独回移到 TileLang，不改全局
Analyzer ABI；与 AutoSchedule 的 Z3 调度求解器没有绑定关系。

## 2. 类与接口骨架

下述为设计签名，无完整方法实现体。Task/Control、SyncPoint、Allocation 等类型沿用
移植后的主仓模型，统一使用旧 `tir` 的 Stmt、PrimExpr 和 ObjectRef。

```cpp
struct LegacySyncContext {
  tir::PrimFunc func;
  Target target;
  std::string platform;
  Map<tir::Var, PrimExpr> address_map;
  Map<tir::Var, PrimExpr> size_map;
  bool ordinary_sync;
  bool vector_scalar_sync;
};

struct ImportedSyncIR {
  std::vector<std::shared_ptr<IRStructure>> roots;
  MultiBufferPlan multibuffer;
  LegacyRewriteMap original_nodes;
  // owns physical storage views, metadata and snapshots for the whole run
};

class LegacySyncImporter : private tir::StmtExprVisitor {
 public:
  ImportedSyncIR Import(const LegacySyncContext& context);
 private:
  void VisitStmt_(const tir::ForNode* op) final;
  void VisitStmt_(const tir::IfThenElseNode* op) final;
  void VisitStmt_(const tir::LetStmtNode* op) final;
  void VisitStmt_(const tir::AttrStmtNode* op) final;
  void VisitStmt_(const tir::EvaluateNode* op) final;
  void VisitStmt_(const tir::BufferStoreNode* op) final;
  void VisitExpr_(const tir::BufferLoadNode* op) final;
  void VisitExpr_(const tir::CallNode* op) final;
};

class PhysicalStorageResolver {
 public:
  explicit PhysicalStorageResolver(const LegacySyncContext& context);
  PhysicalAccess Resolve(const LegacyAccess& access);
  void ValidateFootprints() const;
};

class Z3ProofService {
 public:
  Z3ProofService();
  void Bind(const tir::Var& var, const Range& range, bool allow_override);
  void Bind(const tir::Var& var, const PrimExpr& value, bool allow_override);
  std::function<void()> EnterConstraint(const PrimExpr& constraint,
                                       bool is_assume);
  bool CanProve(const PrimExpr& predicate);
  void SetRLimit(unsigned limit);
  void SetTimeoutMs(unsigned timeout);
  // Owns an independent solver/context and preserves upstream state semantics.
};

class LegacyProofAdapter {
 public:
  ProofResult Prove(const ConstrSet& context, const PrimExpr& predicate);
  bool RegionsMayConflict(const PhysicalAccess& a, const PhysicalAccess& b);
 private:
  Z3ProofService prover_;
};

class TargetSyncPolicy {
 public:
  static TargetSyncPolicy FromTarget(const Target& target,
                                    const std::string& platform);
  SyncAction RequiredAction(const SyncEdge& edge) const;
  EventLimits Limits(const EventDomain& domain) const;
  void ValidateExplicitEvents(const ExplicitEvents& events) const;
};

class LegacySyncExporter {
 public:
  tir::Stmt Export(const ImportedSyncIR& ir, const TargetSyncPolicy& policy);
  void ValidateOutput(const tir::Stmt& body) const;
 private:
  tir::Stmt EmitLiteralEventDispatch(const EventCall& event);
};

// These operate on the imported model, preserving upstream planning logic.
SyncResult RunUpstreamSync(ImportedSyncIR& ir,
                          const TargetSyncPolicy& policy,
                          const ExplicitEvents& reserved);

class AscendInsertSync {
 public:
  static tir::PrimFunc Substitute(tir::PrimFunc func, Target target,
                                  const std::string& platform,
                                  tvm::transform::PassContext context);
};
```

Importer 的可变状态包括当前 scope/core、约束栈、父控制节点、任务索引和原节点映射；
物理 Resolver 持有最终 maps、target 地址域、分析 storage views；Exporter 保留原 kernel
及控制流外壳，只在合法 site 插入同步。三者不拥有另一份同步决策算法。

LegacyProofAdapter 从 ConstrSet 显式输入 ranges、bindings、guards 和 mutable-read
snapshots；查询退出时恢复约束，异常路径也必须恢复。服务返回“未证明”不能被当成
不相交。未编译 Z3 或运行库缺失时，新后端报错；不以旧 Analyzer 静默替代。

## 3. Substitute 入口流程

```text
1. 解析 backend、既有 ordinary/VS 配置及 target policy。
2. 验证 proof marker：相同引擎/配置/target/整个 PrimFunc 则幂等返回；失配则诊断。
   Hash 排除 proof 本身但包括 maps、buffer_map、provenance；无 proof 且全关闭才原样返回。
3. 验证输入 maps、操作 effects、资源 scope 与流水 provenance。
   验证新后端具备 Z3ProofService，并建立独立约束上下文。
4. 导入原 TIR，解析物理访问与版本/owner/epoch，收集并预留已有事件。
5. 调用主仓依赖分析、同步点规划与 ValidateSyncPoints。
6. 调用主仓 FlagAllocator 和物化逻辑，以 target policy 决定具体同步动作。
7. 导出旧 TIR ABI；有限动态 ID 静态分派；验证 guard、配对与作用域。
8. 确认地址/版本/计算序列未变化，记录 proof marker，返回新 PrimFunc。
```

失败不返回部分物化结果。upstream 模式不调用旧算法、不静默跳过未知有副作用调用。

## 4. 输入输出 facts

| fact | 来源 | 消费或输出 | 缺失/失效处理 |
| --- | --- | --- | --- |
| storage scope、resource_scope | infer/combine 或显式 Scope | importer/core policy | 有硬件操作但归属不明时报错；CPU 独立核内 fixture 可显式提供 context |
| `address_map`、`size_map` | MemoryPlanning | PhysicalStorageResolver | 需要的局部存储缺失时报错；不按名称猜 |
| 操作 effects、pipe、workspace | OperationConfig、实际指针 mask | TaskNode | 未知 side effect 报错 |
| guard、Let、loop range、mutable reads | 当前 TIR | constraints、dependency analysis | 保留原语义，不能证明不相交则保守冲突 |
| Z3 服务及其查询上下文 | 回移证明核心、ConstrSet adapter | LegacyProofAdapter | 缺少服务时报错；超时/unknown 不作为消除依赖的证据 |
| 流水 owner/slot/epoch/segment | injector provenance 及传播 | MultiBufferPlan/domain adapter | 流水元数据缺失/失效时报错 |
| 已有同步及显式 flag | 当前输入 | reservation 和 happens-before | 不可界定资源范围时报错 |
| `tl.ascend_sync_proof` | 本 Pass | 重复调用检查 | 不匹配时报错；整个 PrimFunc 排除 proof 后计算 hash |

本 Pass 不发布新的地址规划、buffer versions 或 storage-alias permission，也不修改已有
maps。分析视图不能逃逸为真实 buffer。物理 alias group 不合并逻辑 storage/owner/epoch，
依赖记录保留两端各自的时钟；不把某一端 ring 宽冒用于跨 storage 依赖。任何事实采集
后若对应 IR 改变，重新构建分析。guard/epoch/覆盖无法证明时不能套用保守冲突规则，
必须选择已证明合法的同步位置或拒绝。

## 5. 注册、配置、Python 和 pipeline

- C++ factory：`tvm::transform::Pass AscendInsertSync(Target, String)`。
- 旧 FFI 注册：`TVM_REGISTER_GLOBAL("tl.transform.AscendInsertSync")`。
- Python：`def AscendInsertSync(target: Target, platform: str)`，返回 `_ffi_api` Pass。
- 保留配置：`tl.ascend_auto_sync`、`tl.ascend_auto_sync_vs`；VS 的默认逻辑保持公共配置解析。
- 过渡配置：`tl.ascend_sync_backend: String`，只接受 `legacy/upstream`；完整验收后删除。
- 生效位置：MemoryPlanning 后、ResourceScopeVerify 前；详见设计中的集成代码。
- 最终两个旧 wrapper 调同一配置驱动入口；proof marker 确保连续调用不重复分配。

## 6. 实现阶段与最小冒烟

按设计 S1→S5 实施，逐模块编译，不一次性修改全部源文件后再检查。

1. 来源清单及核心模型适配完成后，将新目录显式加入 CMake 并构建 `tilelang`。
   Z3 核心完成后先验证 bind/约束恢复、表达式语义、独立实例及安装后的库加载。
2. 注册后验证 Python import、factory 和 C++ FFI 一致，确认加载的是重建产物。
3. 使用已有 CPU 同步回归对应的 IR 跑新引擎，检查物理 alias 和完成关系。
4. pipeline 互斥接入后，对无流水最小例执行完整 lowering/编译。
5. VS/流水分别完成后跑其既有聚焦回归；不能用只测 S1 的结果宣布 S3 完成。
6. 按设计设备矩阵验证，最后默认切换并删除旧算法。

本骨架不生成 UT/ST；测试方案已在设计文档中列出，实施后的测试工作另行完成。

## 7. 文档与实施状态

已采纳“主仓同步引擎 + TileLang 侧原 Z3 证明核心”的技术方向；原先仅用旧 Analyzer
保守替代的方案不再作为最终交付。本文列出的是拟议实现接口，不表示接口已经存在。
证明服务和非流水引擎已构建并有聚焦设备验证；静态双缓冲已有主机验证，通用 pipeline、
混合 C/V 和复合 helper 仍未完成。
