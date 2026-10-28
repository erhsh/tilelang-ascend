# Copyright (c) Tile-AI Corporation.
# Licensed under the MIT License.
"""Validate user loop metadata without changing scheduling controls."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any


_RESERVED_KEYS = frozenset(
    {
        "num_stages",
        "tl_pipeline_order",
        "tl_pipeline_stage",
        "tl_pipeline_sync",
        "tl_pipeline_group",
        "tl_cross_interval",
        "tl.ascend_task",
        "tl.ascend_stage",
        "enable_offset",
        "parallel_loop_layout",
    }
)


def loop_annotations(annotations: Mapping[str, Any] | None) -> dict[str, Any]:
    """Copy metadata; scheduling keys require their dedicated supported API."""
    if annotations is None:
        return {}
    if not isinstance(annotations, Mapping):
        raise TypeError("loop annotations must be a mapping with string keys")
    result = dict(annotations)
    for key in result:
        if not isinstance(key, str):
            raise TypeError("loop annotations must have string keys")
        if key in _RESERVED_KEYS or key.startswith("software_pipeline_"):
            raise ValueError(
                f"loop annotation {key!r} is reserved; use its dedicated supported API"
            )
    return result
