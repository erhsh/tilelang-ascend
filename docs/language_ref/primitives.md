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
keys, task/stage markers and unimplemented layout/offset scheduling keys cannot
be injected through this map. Existing Vector lowering and dimensional limits
remain in force.

`Persistent` requires positive integer domain extents, wave size and group size,
with `0 <= index < wave_size`. The last domain extent may be non-divisible by the
effective group size: the compiler pads the scheduling domain and guards the
body so that only valid tile coordinates execute. Internal wave/index arithmetic
uses int64; returned coordinates retain the domain's integer dtype. Symbolic
arguments must satisfy the positive-domain, worker-range and representability
contracts at runtime.

`num_stages=0` keeps Persistent wave execution serial. In this migration stage,
positive Persistent stages can only be attached to a provably unguarded, exactly
one-full-wave domain with divisible grouping. Guarded, partial or multi-wave
combinations, including symbolic inputs for which these conditions cannot be
proved, are rejected with a diagnostic directing callers to use zero stages.
Annotation transport and single-wave checks do not establish
cross-wave pipelining support.

## Ascend Task and Stage markers

`with T.Task():` marks one complete operation; it does not launch a core or add
internal synchronization. The current A3 AscendC subset permits a single audited
float16/float32 Vector call: a supported single-row GM/UB copy, managed fill or
managed add. Multi-call bodies, internal control flow, nested tasks, manual
synchronization, Cube tasks and compound helpers are rejected. `Task` accepts
optional `latency` and `ii` keywords for signature compatibility, but values other
than `None` are rejected until a planner consumes them.

`with T.Stage(0):` surrounds complete explicit tasks and records stage zero.
It cannot be placed inside Task or Parallel. Nonzero stages, offset scheduling,
legacy stage/order combinations, and enclosing positive `num_stages` are rejected;
the existing planner cannot silently redefine an explicit zero stage.

The compiler checks marker structure before frontend rewrites, then validates and
consumes supported markers after lowering operations and before scheduling or
C/V splitting. The explicit read-only schedule analysis retains task/group and
stage metadata. Kernels without markers retain their previous behavior.
