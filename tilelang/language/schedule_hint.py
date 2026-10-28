"""The currently consumed subset of Ascend task/stage markers."""

from __future__ import annotations

from tvm import tir
from tvm.script.ir_builder.tir import attr

__all__ = ["Task", "Stage"]


def Task(latency: int | None = None, ii: int | None = None):
    """Mark one audited Vector call as one task, without launching a core.

    S3 supports a single float16/float32 GM/UB copy, fill or add. Nested tasks,
    multiple calls, control flow, manual synchronization and compound helpers
    are diagnosed. Cost overrides require a future consuming planner.
    """
    if latency is not None or ii is not None:
        raise ValueError(
            "Task latency/ii overrides are unsupported until a consuming planner exists"
        )
    return attr({}, "tl.ascend_task", tir.IntImm("int32", 1))


def Stage(stage: int):
    """Mark complete explicit tasks as stage zero, preserving serial behavior.

    Nonzero stages, offset scheduling and legacy stage/order combinations are
    unsupported. This marker does not enable software or wave pipelining.
    """
    if isinstance(stage, bool) or not isinstance(stage, int) or stage != 0:
        raise ValueError("S3 Stage requires the constant integer 0; nonzero stages are unsupported")
    return attr(tir.IntImm("int64", stage), "tl.ascend_stage", tir.IntImm("int32", 1))
