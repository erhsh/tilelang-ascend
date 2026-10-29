# 自动同步迁移：实现与验证

本页记录实际实现与验收，设计和来源分别见[迁移设计](pass-design.md)、
[实现骨架](pass-impl-skeleton.md)、[来源清单](upstream-sources.md)。

## 当前结论

已落地显式可选的 `upstream` 后端：实际运行移植的主仓依赖分析、
同步规划、覆盖验证和事件分配；不是旧 SyncInsert/VS 的包装。
独立 Z3ProofService 使用主仓证明核心，并修复移植测试发现的状态和位运算问题。
没有整体升级 TVM，也没有引入 AutoSchedule 调度求解器。

目前 v8 覆盖核内 ordinary/VS、控制流与快照，以及受限的 UB/L1 流水、
typed GEMM、既有 C→V 发布及静态 C-prefix K-loop、Persistent、受限 Persistent+SIP
和 GM→UB tail helper；新增 A3 PTO GEMM 的 K128/256 窄 profile 也已单独设备通过。
v8 完整主机/编译回归、A3 设备组合及完整旧 VS 编译回归已通过。
冻结 v8 的真实 clean wheel 与独立 PEP660 editable 安装验收均已通过。
它们各有明确的输入契约，不能推导为
通用流水、任意混合 C/V 或全部复合 helper 已受支持。
**不是完整替换完成**：S3/S4 仍是部分验收，S5 未执行。默认仍为 legacy；
旧算法、过渡 backend 配置和原来的同步开关有效默认值均未移除。

## 构建和使用

构建开关 `USE_ASCEND_SYNC_Z3` 默认 OFF。显式根目录优先，不会混用其他环境的 Z3。
本机验证过的根目录为 `/home/developer/.local/lib/python3.12/site-packages/z3`，
原生库版本 4.15.4；可替换为具有 `include/z3++.h` 与 `lib/libz3.so` 的兼容目录。
不要求运行 TileLang 的 Python 环境安装 Python `z3` 模块。

```bash
cmake -S . -B build \
  -DUSE_ASCEND_SYNC_Z3=ON \
  -DZ3_ROOT=/home/developer/.local/lib/python3.12/site-packages/z3
cmake --build build --target tilelang tilelang_module tilelang_static --parallel 12
```

对已有 PrimFunc `program` 显式启用：

```python
kernel = tilelang.compile(
    program,
    target="ascendc",
    platform="A3",
    pass_configs={
        "tl.ascend_sync_backend": "upstream",
        "tl.ascend_auto_sync": True,
        "tl.ascend_auto_sync_vs": True,
    },
    compile_flags=["--cce-auto-sync=off"],
)
```

新增公共入口为 `tilelang.transform.AscendInsertSync(target, platform)`，backend 配置
也可使用 `PassConfigKey.TL_ASCEND_SYNC_BACKEND`。phase 中新旧整条同步链互斥。
缺少证明服务、未知副作用或无法证明的协议会诊断，不静默调用旧算法。

ordinary/VS 配置含义保持：VS-only 覆盖 V→V 和 S↔(V/MTE2/MTE3)，不会顺带启用所有
Vector/DMA 同步。一般 Developer 自动同步应同时开启 ordinary 与 VS。

## 当前支持与拒绝边界

已验证范围：

- 非流水的单 C 或单 V 图，基本 copy/fill、读写冲突及普通 For。
- 可变 If 条件、已有 Let 和硬件标量参数中的 BufferLoad 在原求值位置形成真实 Let
  快照；S definition task 参与读写依赖，值不会被后续内存覆盖重新解释。
- 非 ring 的 If 使用零或一次的控制节点分析；If/else 先真实捕获一次条件，再分别保留
  两个互斥分支及独立插入锚点。条件不成立时仍有正确的外部完成链，不借可选 producer
  消除直接依赖。
- 不同 Var 共用最终地址的 RAW/WAR/WAW；DMA footprint 使用保守 allocation 范围。
- 独立逻辑 storage 与物理冲突身份、原始语句顺序和词法作用域保持。
- A2/A3 V barrier、A5 V_V event 的主机策略矩阵；S 与 Vector/DMA 的主机配置矩阵。
- 普通顺向、平衡且 literal ID 的已有手动事件预留；容量不足明确失败。
- 默认 AutoCV 下的小尺寸 abs、Parallel add 和原地 ReLU，内存规划开关两种配置。
- scalar-index copy 的 S→DMA 同步，ordinary 和 VS-only 两种配置。
- 同一 PrimFunc 对象重复调用幂等；maps、配置或其他同步输入改变时拒绝旧 proof
  marker。当前 structural hash 不承诺跨序列化重建的 free Var 身份稳定；不能把该
  marker 当作可跨进程携带的证明证书。
- 单 V 的已物化 UB ring：injector 发布真实 B、包含 padding 的
  slot stride 和每个访问的时钟；StorageRewrite 传播 root/base bits，MemoryPlanning
  不把元数据壳误作生命周期 scope。当前审计访问限完整块 GM→UB、abs 与 B1 GM store。
  实际 B2/B3 静态和动态 SIP 已有独立 token/完成关系检查及设备验证；动态输入限单个
  顶层、injector 认证的 owner/segment/stage family，短 trip、空 steady 和长循环分别
  保持完整 seed/drain 协议。使用真实 manual clock 和版本表，不构造自动 owner plan。
- A3 AscendC/`auto` 下受限 typed `gemm_v0`：审计 MTE1 acquire、Cube completion、
  实际 A/B/C footprint 和隐藏 L0 事件；支持已审核的静态 full-tile 外部 K-loop。
  另有 A3 AscendC/`auto` 的受限静态 Cube SIP：真实 lead=1/min=0、K trip≥3，独立
  L1 A/B 双 ring B2、对齐 fp16→fp32、无 transpose 或内部 K splitting，只有一个
  最终 FIX。使用保守的 strong-0 完成 fence 保持 helper 隐藏事件语义，可能减少重叠，
  不承诺提速。这不包含 `gemm_v1` 或任意 scratch/流水组合。
- v8 增加 A3 PTO 的 flat/B1 `gemm_v0`：仅 M=N16、K128/256、kL0=128、fp16→fp32、
  clear=True、无 transpose、n_actual=N。它使用一个隐藏 L0A/L0B slot 和 ID1，
  已分别审计 acquire/completion、内部反馈及事件预留；修正原 helper 重复置位问题。
  两种 K 各重复三次设备调用通过，CCE 自动同步关闭。For/ring/CV 组合仍拒绝；
  来源、原协议反例与共享 helper/cache 边界见[来源记录](upstream-sources.md)。
- flat/B1/C1V2 的既有单次 mode-2 FIX C→V 发布：保留原 Set/Wait，验证真实跨资源
  RAW/WAR/WAW 后，C/V 分别运行核内引擎。支持 Expert `blockIdx.y=2` 与具有
  `cv_1_2` 契约的 Developer `threadIdx.x=2`，不生成新的跨核协议。
- v5 增加同一发布前的单个静态正 trip C-only K-loop：保留完整 Control 树和原计算
  锚点，发布必须在整个循环及其最终 FIX 之后，V epilogue 保持 flat。跨核验证使用
  真实 leaf 与控制区间；全 GM 参数 shadow 仅用于释放/获得证明，防止循环变量逃出
  词法域。无整函数 noalias 的输出复用 A/B 场景也已设备验收。
- 上述发布可使用显式 `tir.noalias`；Cython JIT 也可通过版本化 fresh workspace 契约
  运行无整函数 noalias 的首片：单工作组、V GM 只读可信 workspace 集合、单一外部 GM
  输出且真实 DMA/vid footprint 不交叠；WAR shadow 保证 C 输入读完后才覆盖输出。
  原始 lower/AOT/其他 adapter 不从裸 `auto_gm_indices` 推断 fresh。
- Cython 契约进入 cache key；manifest 核对原函数源码 hash、lowered 参数数目及真实
  自动分配索引。原函数 3 参数、lowered artifact 4 参数且 workspace 索引为 `[3]`
  是合法情况；缺失或不匹配的表不变成 `[]`。旧独立 AutoTunerCache 若未在 key 前准备
  契约，保守跳过该 fresh-sensitive artifact 的磁盘发布，保留当前内存 adapter。
  engine cache ABI 加固已在 v4 原生构建验收：仅 upstream key
  查询 `tl.ascend_sync.EngineCacheABI`，将权威 native 版本纳入 KernelCache、Tuner 和
  CompileArgs key；upstream manifest 升为 schema2 并绑定同一 ABI，直接 database
  重建也核对。legacy/nonupstream key 不查该 FFI，原 fresh manifest schema1 保留。
  每次依赖分析、importer、policy 或物化语义改变都必须 bump native engine 版本；
  包版本或 git HEAD 不覆盖同 HEAD 的工作树重建，原函数源码 hash 也不能替代该版本。
  helper 协议变更同样须 bump。另已审计活动 Cython/ctypes 链：新 adapter 每次实际
  运行 Bisheng 并产生唯一临时库，没有按 kernel source 另读一个低层缓存；通用
  `.cycache` 扩展不 include PTO helper。因此 ABI miss 后会重新编译修正后的头文件。
- 单 C/单 V 的 Persistent：规范领先 break 收紧成等价的每 worker trip count；识别
  Simplify 的 `cid//d` 及经原始范围证明与 LE 等价的 EQ，保留计算、顺序及数组词法
  作用域。部分首 wave 与动态 predicated loop 沿用原控制模型，支持内部普通嵌套循环。
- v6 增加 A3 AscendC/`auto` 的受限 Persistent+SIP：FP32、完整 64 元素块、单个
  规范静态多 wave Persistent、显式 noalias，内部 UB B2/lead2 且静态 trip≥3，
  ordinary 与 VS 同时启用。内部和外部均运行真实依赖分析、验证及分配；每个 wave
  六对事件封口，额外 12 次 flag 调用，外层 wave 串行而内层 SIP 保留。原计算、分配、
  true B 与 clock 均保留；没有清空版本表或使用虚构 epoch。这不代表通用多 owner。
- A2/A3 AscendC/`auto` 下已审计的 GM→UB tail-copy envelope：保持一次原 helper 调用，
  以真实内部握手验证统一 MTE2 acquire/completion，覆盖完整 Duplicate footprint，
  预留隐藏 event-0 并拒绝显式碰撞；动态 mask 不重复求值。设备验收在 A3。

尚不支持，明确拒绝：

- 缺失或不一致的流水 provenance、非 UB ring、任意用户分支 ring、多 owner。
  ring 与其他逻辑 allocation 的别名、缺失 clock、跨槽 helper footprint 均明确拒绝。
- 除上述已认证 C-prefix 外的混合 C/V 循环、分支或多次发布，新跨核协议和
  PerCoreTask/新 scheduled-TIR 输入。
- 未审计的 GEMM 类型/形状/footprint、`gemm_v1`、动态或其他形态的 Cube ring/软件流水；不能仅用
  单 PIPE_M 元数据推导复合完成语义。通用 GEMM 验收仍未完成。
- 直接读取可变内存的 For bounds、未知状态读取和 opaque 表达式；真实快照仅覆盖显式
  BufferLoad，纯符号动态 extent 不因此被禁止。
- 非规范 break、While，以及上述 v6 契约以外的 Persistent 与 SIP 组合；没有把普通
  嵌套 For 的结果当作嵌套 ring/owner 生命周期的证明。
- 反向 manual wait、跨控制流借用 token 或未平衡事件；仅预留 ID 不能证明无死锁。
- 未建模的操作和动态 manual ID；不能靠扩大 barrier 或取模掩盖未知协议。
- 未证明合法范围、零 mask、未支持类型或 footprint 的 tail copy，以及 tail 与
  ring/mixed C/V 的组合。full-copy 既有路径与 tail 首片的 target 支持范围分别处理。

原 Z3 翻译保留数学整数索引模型，不宣称完整机器整数回绕语义。
非保值 cast、非法或无法证明合法范围的 shift 显式诊断；超时/unknown 不作为消除依赖的证据。

## 已执行验证

以下为本次实现的实测；不是调研阶段旧实现基线的重复引用。

最近完整 v8 检查点为 **1035 passed、1 skipped、2 xfailed、30 subtests passed**，
设备组合 **75 passed / 322.17 秒**。其中 74 项覆盖迁移路径，另 1 项是关闭
ordinary/VS 的六对封口原语 litmus。受限 Persistent+SIP 的 8 项和 PTO GEMM 的 2 项
均包含在该设备组合中，不能重复累计。完整记录及文件集合见
[v8 检查点](validation/v8-checkpoint.json)。
两项 xfail 严格对应 A5/AscendC 未提供 `tl.ascend_abs` lowering，legacy 同样失败；
同步错误不会因该已知限制被隐藏。A2/A3 AscendC/PTO 与 A5 PTO 的两类 Vector
程序共 10 项实际 Bisheng 编译通过；没有在 A2/A5 设备执行。

此外，完整旧 `test_ascend_sync_insert_vs.py` 在 v8 实际得到 **101 passed /
383.90 秒**，未过滤 marker。它只执行编译及 IR/源码检查，不启动 NPU，也不包含在
前述 1035 项集合内。该结果验证共享 injector/storage rewrite 等改动后的 legacy
兼容性，不能据此声称新后端支持旧 suite 的任意组合输入。

此前 v6 检查点为 **986 passed、1 skipped、2 xfailed、30 subtests passed**，
设备 **73 passed / 299.87 秒**，不含后续 PTO GEMM。保留
[v6 记录](validation/v6-checkpoint.json)作为历史，不与 v8 累加。

此前 v4 完整主机 checkpoint 为 **903 passed、1 skipped、30 subtests passed**：包含
新增 host、旧 ordinary/one-trip/Persistent 的 51 项回归、Python 契约/缓存/打包，以及
真实 Bisheng 缓存重建、ABI/key/directDB、新 scope 负例和 Cube 软件流水测试。
skip 为 OFF 专用诊断。
额外完整旧 VS 编译 suite 的尝试因耗时超出 240 秒而结束，没有报告通过，不能计入
该结果。设备组合的最新结果另行记录；不能把仅生成源码算作设备执行。
设备证据分批记录：较早同一构建的七文件组合 **45 passed / 179.41 秒**，包含全部
9 个 Persistent 配置；v4 在相关 CPU 检查通过后，新增 Cube SIP 设备 **6 passed /
30.57 秒**。前 45 项早于 header 作用域、ABI/schema2 与 Cube 后续改动；不能称作
当前 v4 的 51 项一次性全量设备通过。

新增模块的分项检查点如下。它们与主机 checkpoint 和下方历史范围有重叠，不能相加
作为总测试数。

| 后续分项 | 结果与边界 |
| --- | --- |
| v4 ON 三目标构建 | 两共享库及静态库构建通过；native getter 与 Python 均确认 `upstream-1c0f402e-legacy-v4` |
| v5 ON 三目标及组合检查 | 两共享库及静态库构建通过；ABI 为 `upstream-1c0f402e-legacy-v5`；944 passed、1 skipped、2 xfailed、30 subtests passed；设备 65 passed |
| v6 ON 三目标及组合检查 | ABI 为 `upstream-1c0f402e-legacy-v6`；986 passed、1 skipped、2 xfailed、30 subtests passed；设备 73 passed |
| v8 ON 三目标及完整检查 | ABI 为 `upstream-1c0f402e-legacy-v8`；1035 passed、1 skipped、2 xfailed、30 subtests passed；A3 设备组合 75 passed |
| v8 PTO 专项 | 49 新增 CPU + 143 相关回归，合并 192 passed；K128/256 两项 NPU passed；fixture 导入 lint 修正后协议 14 项再次通过 |
| v8 OFF 三目标及回归 | 三目标构建通过；同一 1038 项主机范围为 326 passed、712 skipped、30 subtests passed；设备 74 skipped，另 1 项关闭 ordinary/VS 的独立封口原语执行通过 |
| v8 恢复 ON | 三目标构建通过；证明服务/ABI/PTO 专项 125 passed、1 skipped；随后完整 A3 设备组合也通过 |
| v8 完整旧 VS | 101 passed / 383.90 秒，仅 legacy 编译和 IR/源码检查；不作为新后端的额外覆盖计数 |
| v6 Persistent+SIP | 42 CPU、258 相关回归、8 NPU passed；均与完整检查点重叠，不重复累计；外层串行封口有额外开销，尚未给出性能结论 |
| v5 C-prefix 发布专项 | 131 CPU、8 NPU passed；K tiles=2/3，分别覆盖 noalias、无 noalias 的独立输出及输出复用 A/B，均重复调用三次 |
| A3 PTO 设备 | 5 passed：同 V 依赖、静态/动态 B2/B3；动态同 kernel 反复运行 0/1/2/3/5/33 等边界 |
| v4 OFF 三目标及回归 | 两共享库及静态库构建通过；相同 904 项范围为 312 passed、592 skipped、30 subtests passed；设备 51 项全部跳过，没有编译或执行新 kernel |
| v4 恢复 ON | 三目标构建通过，原生证明服务重新可用；修正两处模块级测试门禁后，动态 UB/Cube 流水 157 项再次通过 |
| 较早一致 A3 AscendC 设备 checkpoint | 45 passed：基本 15、控制 4、GEMM 4、C/V 4、UB pipeline 8、tail 1、Persistent 9；CCE 自动补同步关闭，完整运行一次通过 |
| v4 Cube SIP 设备 | 6 passed；在 Cube 专项 CPU 检查通过后执行，不与历史数字重复相加 |
| Python 契约/缓存/打包检查点 | 161 passed、30 subtests passed；三文件范围，不冒充全部 native 或设备验收 |
| 动态 UB SIP/协议检查点 | 192 passed；包括 B2/B3、短 trip、独立完成关系及破坏协议负例 |
| Persistent CPU | 63 passed；含真实 C-only/单 V lowering、普通嵌套、各 worker 覆盖等价、cid//d/EQ 证书与 header 取证不受 body 重绑定影响的负例 |
| Tail-copy CPU | 54 passed；包括 mask/stride/footprint/类型及隐藏事件碰撞，未证明输入拒绝 |
| v4 真实 Bisheng cache | 1 passed：原函数 3 参数→artifact 4 参数及 `[3]`，实际 schema2 store→disk hit；缺失/损坏 metadata、合法但错误的 `[]` / `[2]`、source hash/engine ABI 不匹配均 miss，恢复后再次 hit；未启动 NPU kernel。模拟未来 ABI 的 key miss 不冒充第二次 native rebuild |
| v4 ABI/Persistent/缓存专项 | 278 passed、30 subtests：实际 v4 getter、真实 cache、Persistent63、ABI27、原 helper86、三 loader 及旧 recovery；包含 mock 的负例不冒充硬件执行 |

历史首片 checkpoint 为 ON 297 passed/1 skipped（280 host、17 NPU），对应 OFF
77 passed/221 skipped；OFF 结果仅适用于当时的源码与测试范围，不是新增模块的 OFF
验收。另有 alias/copy/fill 与静态双缓冲在 A2 AscendC、A2/A3 PTO 的 Bisheng 编译证据，
没有执行这些平台配置。原生依赖 staging、临时安装/模拟 wheel 的 `$ORIGIN` RUNPATH、
SONAME 解析、无 Python z3 加载、无效显式 Z3_ROOT 拒绝及打包 ON→OFF 清理均曾通过。
完整旧 VS 的五项 pipeline/CV/memory-planning 集成编译检查另已通过；它们使用 legacy，
不作为新后端在 PTO 上的设备认证。

全部新增设备验收显式关闭 Bisheng 自动补同步。测试受进程超时保护；未以未终止的进程
或仅生成源码计作设备通过。Developer 的 memory ON/OFF 源在这三个小例子中相同，
因此只能证明开关兼容；真正地址复用的证据来自单独的 alias 用例。
已保存 [A3 性能检查原始数据](validation/a3-performance.json)：固定 warmup、输入、
CCE 自动同步关闭，在 v4 对 vector abs、静态 B3 pipeline、K-loop GEMM、Cube B2 SIP
比较两个后端。测量包含 host launch gaps，约 104.5–107.3 微秒，只有四个小 kernel；不能据此推导提速或
设备执行时间优势。尚未完成 A2/A5 设备及通用 pipeline/GEMM 认证。
v8 对相同四个用例重新执行完整正确性与性能对照，见
[v8 原始数据](validation/a3-performance-v8.json)。两个后端的中位数约 107.3–111.5
微秒，upstream/legacy 比值约 0.990–1.010；仍是含 host launch gaps 的小用例测量，
不能据此判断内核执行时间收益，Persistent+SIP 的额外封口开销也未由这四例覆盖。
安装库及未经 `patchelf` 处理的构建库均实测优先从同目录加载 Z3。
v4 已真实完成隔离 clean `bdist_wheel`、独立安装及原生证明服务加载，
详见[产物、哈希与验证元数据](validation/wheel-v4.json)。约 295 秒是整个
`bdist_wheel` 命令的耗时。安装前移开隔离源码/build 路径，并用 `/proc/self/maps`
确认 TileLang/TVM/Z3 来自 wheel。默认可选 GTest 发现与本机安装不兼容，
仅在隔离 CMake cache 关闭该发现后重试；没有修改 setup.py 或 TVM 源来绕过。
TVM 原有修改的 HEAD、diff、两个文件哈希在前后完全一致。该原始产物严格来自 v4
冻结快照，不包含后续 C/V 循环适配；当时 editable 的 ELF 路径和完整许可证分发检查
发现缺口，不能算完整通过。

随后在独立 v4 快照上仅应用四项打包文件变更，重新完成真实 clean wheel、独立安装
和 PEP660 editable 全流程，两者均通过。四个原生库的 RUNPATH 均为 `$ORIGIN`，
实际加载路径和正反 Z3 查询均核验，TVM LICENSE/NOTICE、移植 NOTICE 与原版 Z3
LICENSE 四份文本在 wheel 和 editable 中按字节核对。详见
[打包修正验收](validation/packaging-fix-audit.json)。该证据验证打包修正，不代表 v6
或后续编译器源码已重新执行完整 wheel 构建。

最终 v8 已使用当前冻结的完整编译器和打包源码重新执行两条独立流程，均通过：
真实 clean wheel 构建 313.2 秒、隔离安装 4.5 秒、原生验证 8.5 秒；真实 PEP660
editable 构建安装 290.8 秒、原生验证 9.4 秒。两种安装均核对四库 `$ORIGIN`、
实际原生来源、ABI8、Z3 正反查询及四份文本；41,510 个源文件构建前后 hash 不变，
原 TVM 修改也完全保留。完整配置、manifest/wheel 哈希与证据路径见
[v8 最终产物验收](validation/packaging-v8-audit.json)。它取代旧 v4 产物作为当前
opt-in 阶段的发布验证，不消除本页列出的输入与设备范围限制。

v6 另执行真实 `cmake --install`，发现并修正安装闭包遗漏 `libtvm.so` 后，安装目录的
编译器、TVM、Z3 加载、原生证明查询及四份许可证文本均通过。该安装检查与前述
wheel/editable 检查分别记录，不混用编译器版本。
v8 已从恢复 ON 的三目标再次执行真实安装，见
[v8 安装记录](validation/cmake-install-v8.json)：compiler 的实际原生库均来自新
prefix，ABI8、Z3 正反查询、四份文本及 `$ORIGIN` 通过。附加的 runtime-only 独立
加载检查发现 `IRBuilderFrameNode` RTTI 缺失，且在旧 v6 prefix 同样复现；该附加
检查未通过，不把 compiler 验证扩称为 standalone runtime 验证。
安装的静态库没有新增 CMake 导出包，外部独立
使用归档时仍需显式链接 Z3；build tree 的 `tilelang_static` target 已传播该依赖。

v8 主机/编译检查的精确文件集合保存在
[31 个文件的清单](validation/v8-host-files.json)。可直接复验：

```bash
python - <<'PY'
import json
import subprocess
import sys
from pathlib import Path

files = json.loads(
    Path("docs/ascend/sync_migration/validation/v8-host-files.json").read_text()
)
subprocess.run(
    [sys.executable, "-m", "pytest", "-q", *files, "-p", "no:cacheprovider"],
    check=True,
)
PY
```

其中 codegen 矩阵和真实缓存重建需要 Bisheng/CANN，但不启动 NPU kernel。
该组合不包含设备文件和完整旧 VS suite。构建 OFF 后复用同一清单，再恢复 ON，
可复验本页记录的门禁行为。

此前 v5 主机/编译检查的固定文件集合如下。其中真实缓存与 codegen 矩阵需要 Bisheng/CANN，
但不会启动 NPU kernel。该命令不包含完整旧 VS 编译 suite。

```bash
timeout --kill-after=10s 240s python -m pytest -q \
  testing/python/language/test_ascend_upstream_sync_codegen_matrix.py \
  testing/python/language/test_ascend_upstream_sync_composite.py \
  testing/python/language/test_ascend_upstream_sync_composite_loop.py \
  testing/python/language/test_ascend_upstream_sync_cross_core.py \
  testing/python/language/test_ascend_upstream_sync_cross_core_loop.py \
  testing/python/language/test_ascend_upstream_sync_cube_pipeline.py \
  testing/python/language/test_ascend_upstream_sync_dynamic_pipeline.py \
  testing/python/language/test_ascend_upstream_sync_guards.py \
  testing/python/language/test_ascend_upstream_sync_helper_effects.py \
  testing/python/language/test_ascend_upstream_sync_insert.py \
  testing/python/language/test_ascend_upstream_sync_persistent.py \
  testing/python/language/test_ascend_upstream_sync_physical_rings.py \
  testing/python/language/test_ascend_upstream_sync_pipeline.py \
  testing/python/language/test_ascend_upstream_sync_policy.py \
  testing/python/language/test_ascend_upstream_sync_runtime_contract.py \
  testing/python/language/test_ascend_upstream_sync_runtime_contract_cache_compile.py \
  testing/python/language/test_ascend_upstream_sync_snapshot.py \
  testing/python/language/test_ascend_upstream_sync_tail_copy.py \
  testing/python/language/test_ascend_upstream_sync_wave_seal.py \
  testing/python/language/test_ascend_z3_proof_service.py \
  testing/python/language/test_ascend_pipeline_ring_provenance.py \
  testing/python/language/test_ascend_sync_insert.py \
  testing/python/language/test_ascend_sync_insert_one_trip.py \
  testing/python/language/test_tilelang_ascend_language_persistent.py \
  testing/python/test_ascend_runtime_contract.py \
  testing/python/test_ascend_sync_cache_abi.py \
  testing/python/cache/test_ascend_runtime_contract_cache.py \
  testing/python/test_native_runtime_packaging.py \
  -p no:cacheprovider
```

真实缓存重建也可单独执行：

```bash
python -m pytest -q \
  testing/python/language/test_ascend_upstream_sync_runtime_contract_cache_compile.py
```

全部新增设备文件可一次检查。v8 实际得到 75 项通过，精确文件集合也保存在
v8 检查点中：

```bash
timeout --kill-after=10s 540s python -m pytest -q \
  testing/python/language/test_ascend_upstream_sync_*device.py \
  -p no:cacheprovider
```

完整旧 VS 编译回归可独立复验（没有 marker 过滤，不启动 NPU）：

```bash
timeout --kill-after=10s 900s python -m pytest -q \
  testing/python/language/test_ascend_sync_insert_vs.py -p no:cacheprovider
```

此前 v5 按下列文件集合实际得到 65 项通过：

```bash
timeout --kill-after=10s 480s python -m pytest -q \
  testing/python/language/test_ascend_upstream_sync_composite_device.py \
  testing/python/language/test_ascend_upstream_sync_control_device.py \
  testing/python/language/test_ascend_upstream_sync_cross_core_device.py \
  testing/python/language/test_ascend_upstream_sync_cross_core_loop_device.py \
  testing/python/language/test_ascend_upstream_sync_cube_pipeline_device.py \
  testing/python/language/test_ascend_upstream_sync_device.py \
  testing/python/language/test_ascend_upstream_sync_persistent_device.py \
  testing/python/language/test_ascend_upstream_sync_pipeline_device.py \
  testing/python/language/test_ascend_upstream_sync_pto_device.py \
  testing/python/language/test_ascend_upstream_sync_tail_copy_device.py \
  testing/python/language/test_ascend_upstream_sync_wave_seal_device.py \
  -p no:cacheprovider
```

纯主机检查可从聚焦命令中移除 device 文件。OFF 验证将构建开关设 OFF 后运行相同测试；
迁移设备文件的模块级服务门禁先于编译 fixture 执行，不会在 OFF 环境意外编译或
执行新同步路径。独立 wave-seal 原语用例关闭 ordinary/VS，不依赖证明服务，因此
在 v8 OFF 组合中仍实际执行 1 项；其余 74 项跳过。随后重新构建 ON。

## 下一阶段门槛

1. 按旧公开前端实际生成的输入逐项补齐剩余 Cube 流水、跨资源循环和已启用 target/codegen
   组合；受限模块通过不等于它们任意组合也通过，不能删除门禁充数。
2. 扩大受限 Persistent+SIP 之外的真实嵌套 owner/epoch 生命周期；automatic counter/
   multi-owner 与 manual 模式分开建模，不把 version 表清空来规避协议。
3. 扩大 ring helper 白名单前，逐个验证真实 DMA/Vector footprint、隐藏事件池与完成语义。
4. 后续每次扩大适配范围后，重新执行完整 ON/OFF、设备、真实缓存重建和发布产物检查；
   v8 已完成本阶段检查，但不能豁免新增组合的验收，不用历史或分项计数替代它们。
5. 按 S4 验收声明支持的 target/codegen 与性能回归后，才进入 S5：构建依赖与默认切换、
   两个旧 API alias 的统一语义/幂等测试、删除旧算法及过渡分派。默认目前不变。
   届时还必须将 native ABI salt 从“显式 upstream 配置”改为“实际生效的 Ascend 后端”，
   避免省略配置的新默认路径复用旧缓存；不能影响无关 target 的 key。
