Tile Language: Primitives
=========================

## Ascend compilation options and C/V scopes

Ascend kernels contain work for Cube and Vector execution units. By default, the
compiler identifies which unit each operation needs and places it in the matching
scope. This also applies to kernels that use only Cube or only Vector operations.

The following `tilelang.PassConfigKey` options can be passed through `pass_configs`
to `@tilelang.jit` or `tilelang.compile`:

| Option | Default | Effect |
| --- | --- | --- |
| `TL_ASCEND_AUTO_CV_COMBINE` | `True` | Identify and separate Cube and Vector work |
| `TL_ASCEND_AUTO_SYNC` | `False` | Insert synchronization within each execution unit |
| `TL_ASCEND_MEMORY_PLANNING` | `False` | Reuse on-chip memory according to buffer lifetimes |
| `TL_ASCEND_AUTO_CV_SYNC` | `False` | Insert synchronization for recognized Cube/Vector GM workspace handoffs; requires CombineCV |

Omit `TL_ASCEND_AUTO_CV_COMBINE` to use automatic scope placement. You may also write
`with T.Scope("C")` or `with T.Scope("V")`: valid explicit scopes are preserved, so
handwritten scopes do not require disabling this option. The operations inside a
scope must belong to that unit; nesting C inside V, or V inside C, is rejected.

Set `TL_ASCEND_AUTO_CV_COMBINE=False` when you want to supply all scopes yourself.
Every operation tied to Cube or Vector must then be inside its matching scope.
Operations whose unit cannot be inferred, including opaque external calls and raw
source code, require an explicit scope even with automatic placement enabled.

GM scalar assignments such as `Output[i] = value` also require an explicit
`T.Scope("C")` or `T.Scope("V")`. Choose the side that owns the assignment's
state and indices; the compiler neither infers this owner from surrounding work
nor defaults the write to Cube.

`T.barrier_all()` and `T.pipe_barrier` with `"ALL"`, `"MTE2"`, or `"MTE3"` can
use automatic placement when the surrounding region's other resource-specific
work belongs to one side, or the nearest concrete operations on both sides have
the same owner. Otherwise, supply an explicit scope for the barrier.

Automatic scope placement does not enable the other three options. In particular,
placing a producer and consumer on the correct units does not synchronize them;
their data dependencies still need automatic or explicit synchronization.

## Ascend loop metadata and Persistent domains

The loop interfaces retain their existing positional arguments:

```python
T.Parallel(*extents, coalesced_width=None, annotations=None)
T.Pipelined(start, stop=None, num_stages=0, order=None, stage=None,
            sync=None, group=None, cross_interval=1, *, annotations=None)
T.Persistent(domain, wave_size, index, group_size=8, num_stages=0, *, annotations=None)
```

`annotations` adds non-reserved loop metadata without modifying the supplied map.
New Parallel metadata is attached only to the outermost loop. The existing
`coalesced_width` behavior is retained on every generated Parallel dimension;
supplying conflicting widths through the keyword and metadata is an error.
Use the dedicated arguments for pipeline controls: generated stage/order/sync
keys, task/stage markers and internal layout/version controls cannot be injected
through this map. Pipelined alone accepts a boolean `enable_offset` consumed by
constraint planning. Existing Vector lowering and dimensional limits
remain in force.

`Persistent` requires positive integer domain extents, wave size and group size,
with `0 <= index < wave_size`. The last domain extent may be non-divisible by the
effective group size: the compiler pads the scheduling domain and guards the
body so that only valid tile coordinates execute. Internal wave/index arithmetic
uses int64; returned coordinates retain the domain's integer dtype. Symbolic
arguments must satisfy the positive-domain, worker-range and representability
contracts at runtime.

`num_stages=0` keeps Persistent wave execution serial. Legacy positive stages
require a provably unguarded, exactly one-full-wave domain with divisible
grouping. The calibrated constraint subset below additionally supports static
guarded wave loops. It predicates each operation with its logical tile's
validity and verifies actual per-worker coverage after pipeline expansion.

## Ascend Task and Stage markers

`with T.Task():` marks one complete operation; it does not launch a core or add
internal synchronization. The current A3 AscendC subset permits a single audited
float16/float32 Vector call: a supported single-row GM/UB copy, managed fill or
managed add. Multi-call bodies, internal control flow, nested tasks, manual
synchronization, Cube tasks and compound helpers are rejected. `Task` accepts
optional `latency` and `ii` keywords for signature compatibility, but values other
than `None` remain unsupported; supply calibrated costs through the model below.

`with T.Stage(n):` surrounds complete explicit tasks. The builder requires a
nonnegative constant signed int32 value and rejects booleans. It cannot be
placed inside Task or Parallel. Legacy retains its Stage(0)-only contract.
Nonzero stages require a supported constraint candidate and explicit
`annotations={"enable_offset": True}`; outside a positive candidate they are
rejected. Source reads retain their imperative meaning, so conflicting manual
stages report an unsatisfiable plan. With any explicit Stage in a candidate,
unmarked units are stage zero; Task alone does not freeze automatic staging.

The compiler checks marker structure before frontend rewrites, then validates and
consumes supported markers after lowering operations and before scheduling or
C/V splitting. The explicit read-only schedule analysis retains task/group and
stage metadata. Kernels without markers retain their previous behavior.

## Opt-in A3 constraint planning

The default pipeline planner remains `legacy`. Select the first constraint
subset explicitly and enable the existing synchronization and memory planner:

```python
pass_configs = {
    tilelang.PassConfigKey.TL_ASCEND_PIPELINE_PLANNER: "constraint",
    tilelang.PassConfigKey.TL_ASCEND_AUTO_SYNC: True,
    tilelang.PassConfigKey.TL_ASCEND_MEMORY_PLANNING: True,
    tilelang.PassConfigKey.TL_ASCEND_AUTO_SYNC_VS: False,
    tilelang.PassConfigKey.TL_ASCEND_AUTO_CV_SYNC: False,
}
```

The single-stage subset requires A3 AscendC/auto, static serial loops, and at least one
`T.Pipelined(..., num_stages=1)` candidate. It supports audited float16/float32
full, aligned GM/UB copies and Vector fill/add. Use a separate full short tile
for a static aligned tail; dynamic padding DMA is outside this subset.
Manual flag/barrier calls, mixed Cube/Vector, raw code, explicit UB address
placement, explicit stage/order arrays and multiple stages or buffer versions
are rejected. UB allocations must be static float16/float32 and fit the native
memory budget, including row padding.

GM aliases remain conservative. When the caller guarantees distinct input and
output storage, declare `T.func_attr({"tir.noalias": True})` in the PrimFunc.
Without that contract, a loop can require cross-iteration GM dependencies that
the existing synchronization does not cover; such a kernel is rejected.

The planner orders operations within one iteration and preserves the source
loop iteration order. The existing injector and synchronization passes still generate
the executable code; a final verifier checks logical dependencies, physical
storage reuse and event lifetimes. Unsupported or unproved cases report an
error. This option requires the optional `ascend-scheduler` solver dependency
when used; legacy does not load it. The strategy participates in the existing
compilation cache key.
Verification expands at most 4096 hardware task instances and 65536 control
visits per trace; larger traces report a limit error.

## Calibrated multi-stage constraint pipelines

Load a measured A3 cost artifact and pass its complete content to the compiler:

```python
cost_model = tilelang.transform.load_ascend_pipeline_cost_model("a3-costs.json")
pass_configs[tilelang.PassConfigKey.TL_ASCEND_PIPELINE_COST_MODEL] = cost_model
```

`T.Pipelined(..., num_stages=2)` or `3` enables the supported modulo path.
The number is a buffer-version search cap. Actual buffer versions can differ;
the compiler validates and records the materialized versions, strides and bytes.
Single-row, contiguous float16/float32 UB buffers must be loop-owned, fully
written before reads in each iteration, and have 32-byte aligned version sizes.
External UB inputs, shared writable UB and cross-iteration accumulation are
outside this subset. A full in-place update after that iteration's full write
must pass the same producer and physical-slot checks.

The profile matches exact operation/dtype/count/geometry and contains measured
effective nanosecond costs including issue and synchronization overhead. It is
not a raw instruction timing guarantee. The loader checks artifact/schema/hash;
planning rejects missing operation geometry. Use the loader's `expected_identity`
argument to check a deployment's SoC/CANN/bisheng/flags. The S5 NPU entries check
those fields against their actual worker before compilation; ordinary planning
does not discover the runtime device. The full content and digest participate in
the existing cache key; a filename alone cannot identify a model.

Automatic multi-stage planning enables offsets by default. Explicit
`enable_offset=False` restricts stages to zero. Manual stages are bounded by a
separate staging horizon of 64 and the expansion limits, rather than by the
requested version cap. A zero-trip candidate is emitted as a verified no-op;
it does not prove a physical multi-buffer allocation was executed.

Static guarded `Persistent` waves use the same audited Vector subset and cost
model. Group padding and the last partial wave remain guarded independently
for every operation. This does not enable Cube or arbitrary stateful helpers.
Managed Vector mask requirements are checked on the actual selected instruction
sequence, with one mask state per Vector domain rather than per buffer version.

Use `scripts/ascend_migration/s5_run_pipeline_npu.py --calibrate-profile` to
generate an artifact with `msprof op`. The migration plan records the complete
command, measurement policy, actual verification results and supported scope.
