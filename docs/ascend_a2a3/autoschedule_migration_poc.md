# A2/A3 Reuse of Main-line A5 Auto-Schedule: POC Findings

Branch: `feat/a2a3-reuse-a5-autosync`
Probe script: `/tmp/opencode/probe_autoschedule.py`

## Goal

Verify whether the A2/A3 backend (`tilelang/ascend/a2a3`, `src/ascend/a2a3`)
can reuse the main-line A5 auto-scheduler and its `InsertSync` pass
(`src/ascend/transform/auto_schedule/*`, `insert_sync.cc`) for automatic
synchronization insertion.

## Approach

The A5 auto-schedule passes are already compiled into the shared
`libtilelang.so` (the parent `src/ascend/CMakeLists.txt` globs
`src/ascend/transform/*.cc` and `.../auto_schedule/*.cc`) and registered as
`tl.transform.*`. The A2/A3 pipeline was given an opt-in branch gated by
`PassConfigKey.TL_ENABLE_AUTO_SCHEDULE` (default `False`) that calls the A5
chain verbatim:

```
NormalizeControlFlowForSchedule -> NormalizeConflictHints
-> MaterializeScheduleUnits -> AssignCore -> AnnotateMultiBufferEligible
-> EstimateLatency -> AutoSchedule -> PrepareMultiBuffer -> ResolveCore
-> InsertSync -> MaterializeMultiBuffer -> LowerScheduledTIR
-> RestoreWhileLoops
```

This is a technology-verification probe only (see
`_lower_a2a3_autoschedule` in `tilelang/ascend/a2a3/pipeline.py`).

## Finding 1: Whole-pipeline reuse is blocked by the dialect boundary

Probing an A2/A3 `gemm_v0` kernel (`T.Scope("C")`, `T.Pipelined`) with
`TL_ENABLE_AUTO_SCHEDULE=True` fails at the first substantive pass:

```
MaterializeScheduleUnits: Check failed: (IsScheduleGuardAttribute(op->attr_key))
  is false: ... Unsupported attr_key: resource_scope
```

- `T.Scope("C"/"V")` emits a `resource_scope` `AttrStmt` during eager tracing
  (`src/ascend/a2a3/ir.cc:240`): `Scope("C") -> 0`, `Scope("V") -> 1`. This is
  the TA dialect's cube/vector separation, consumed throughout lowering
  (`lower_tile_op`, `ascend_sync_insert`, `combinecv`).
- The A5 scheduled-TIR codec only accepts `tilelang_assume` /
  `kAssumeRequiresRuntimeCheck` as scheduling guards
  (`src/ascend/transform/auto_schedule/scheduled_tir.h:62`); it has no notion
  of `resource_scope`.

Porting the whole chain therefore requires translating the TA `T.Scope` CV
model into the A5 `T.Stage` + `tl.schedule_unit` + `tl.vf_latency` model, which
is equivalent to rewriting the A2/A3 scheduling frontend — out of scope and
against the "no main-line changes" constraint.

## Finding 2: Capability alignment has limited value (sync vs async model)

The A5 `FlagAllocator` (lifetime reuse via `lifetime_begin/end` + `CanReuse`),
cross-iter classification (`edge.distance`), and cross-core 16x2 flag banks are
all designed for A5's **asynchronous** model, where `set_flag`/`wait_flag` are
separated by many statements and flags live across statements.

The A2/A3 passes emit each `EventPair` as an **adjacent set+wait pair**
(`InsertSynchronization` in both `ascend_sync_insert.cc` and
`ascend_sync_insert_vs.cc`), so every flag has an instantaneous lifetime.
Because the TA dialect manages pipelining explicitly via `T.Pipelined` +
`InjectSoftwarePipeline`, this synchronous model is sufficient.

## Finding 3: Cross-iteration flag reuse is correct (no bug)

Audit of the `% 8` round-robin `AllocateEventId` under loop-carried
(back-edge) dependencies:

- `AscendSyncInsert` unrolls each loop into two synthetic iterations
  (`ForLoopUnroller`, iter1/iter2) and discovers back-edges via
  `first_iteration_history`; `LoopRebuilder::MergeSyncSequences` re-merges them
  (LCS) back into the loop.
- `AscendSyncInsertVS` uses a two-pass `is_revisit_pass_` traversal, marking
  the loop-exit history `is_back_edge`.
- In both, `set_flag` and `wait_flag` are always inserted back-to-back, so a
  flag's lifetime is instantaneous. Even if a single site needs more than 8
  distinct event pairs (the `% 8` wrap-around), the previous pair's `wait` has
  already completed, so reuse is safe. Back-edge pairs rely on AscendC's
  in-order issue within a pipe and remain "set then immediately wait" per
  iteration.

Consequently, porting A5's lifetime-aware flag allocation into A2/A3 would not
fix any correctness issue; it would only matter for A5's asynchronous model.

## Conclusion

1. Whole-pipeline reuse of the A5 auto-scheduler is **not feasible** without
   rewriting the A2/A3 dialect scheduling model (blocked by `resource_scope`
   vs scheduled-TIR guards).
2. Most A5 sync capabilities are specific to its asynchronous model and have
   **no direct benefit** for the A2/A3 synchronous model.
3. The A2/A3 `% 8` flag allocation is **correct** for its model, so no
   correctness-motivated port is needed.

The `TL_ENABLE_AUTO_SCHEDULE` probe branch and its pass-chain list are retained
on `feat/a2a3-reuse-a5-autosync` as evidence; they are not a production path.
