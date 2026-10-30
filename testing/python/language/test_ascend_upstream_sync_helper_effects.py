# Copyright (c) Tile-AI Organization.
# Licensed under the MIT License.
"""CPU admission checks for helper paths with hidden composite effects.

The GM-to-UB ABI matches AscendCopy and CopyCodegen: source, destination,
stride, row mask, column mask, pad, followed by optional physical-shape
metadata. No legacy sync output or hardware execution is used as an oracle.
"""

import pytest

import tilelang
from tilelang import tvm

tir = tvm.tir


@pytest.fixture(autouse=True)
def _requires_proof_service() -> None:
    if not tvm.get_global_func("tl.ascend_sync.Z3ProofServiceAvailable")():
        pytest.skip("requires USE_ASCEND_SYNC_Z3=ON and the migrated sync engine")


def _function(
    helper: str,
    masks: tuple[int | tir.PrimExpr, ...],
    *,
    scalars: tuple[tir.Var, ...] = (),
    guard: tir.PrimExpr | None = None,
    binding: tuple[tir.Var, tir.PrimExpr] | None = None,
    metadata: tuple[int, ...] = (),
) -> tir.PrimFunc:
    source = tir.decl_buffer((64,), "float32", name="input")
    destination = tir.decl_buffer((64,), "float32", name="output")
    local = tir.decl_buffer((64,), "float32", name="local", scope="shared.ub")
    load = tir.Evaluate(
        tir.call_extern(
            "handle",
            helper,
            source.access_ptr("r"),
            local.access_ptr("w"),
            *masks,
            *metadata,
        )
    )
    store = tir.Evaluate(tir.call_extern("handle", "copy_ub_to_gm", local.access_ptr("r"), destination.access_ptr("w")))
    body = tir.SeqStmt([load, store])
    if binding is not None:
        body = tir.LetStmt(*binding, body)
    if guard is not None:
        body = tir.IfThenElse(guard, body, None)
    body = tir.AttrStmt(tir.IntImm("int32", 0), "resource_scope", 1, body)
    body = tir.Allocate(local.data, local.dtype, [64], tir.const(True, "bool"), body)
    return (
        tir.PrimFunc(
            [source.data, destination.data, *scalars],
            body,
            buffer_map={source.data: source, destination.data: destination},
        )
        .with_attr("address_map", {local.data: 0})
        .with_attr("size_map", {local.data: 256})
        .with_attr("tir.noalias", True)
    )


def _run(function: tir.PrimFunc) -> tir.PrimFunc:
    target = tvm.target.Target({"kind": "llvm", "model": "ascendc"})
    with tvm.transform.PassContext(config={"tl.ascend_auto_sync": True, "tl.ascend_auto_sync_vs": True}):
        result = tilelang.transform.AscendInsertSync(target, "A3")(tvm.IRModule({"main": function}))
    return result["main"]


def _assert_admitted(function: tir.PrimFunc) -> None:
    result = _run(function)
    assert result.attrs.get("tl.ascend_sync_proof") is not None
    events = []

    def collect(node: object) -> None:
        if (
            isinstance(node, tir.Call)
            and isinstance(node.op, tvm.ir.Op)
            and node.op.name in ("tl.ascend_auto_set_flag", "tl.ascend_auto_wait_flag")
        ):
            events.append(node)

    tir.stmt_functor.post_order_visit(result.body, collect)
    # The independent RAW oracle is this fixture's GM->UB then UB->GM chain.
    assert events
    assert any(event.args[0].value == "MTE2_MTE3" for event in events)


@pytest.mark.parametrize(
    ("helper", "arguments", "metadata"),
    [
        ("copy_gm_to_ub<float, 64>", (64, 1, 64, 0), (64,)),
        ("tl::ascend::copy_gm_to_ub<float, 32, 2>", (32, 2, 32, 0), (2, 32)),
        # Omitted masks use the helper's full-tile C++ defaults.
        ("copy_gm_to_ub<float, 64>", (), ()),
        ("copy_gm_to_ub<float, 64>", (64,), ()),
        ("copy_gm_to_ub<float, 64>", (64, 1), ()),
        ("copy_gm_to_ub", (), ()),
    ],
)
def test_full_mask_copy_preserves_the_single_mte2_effect_contract(
    helper: str, arguments: tuple[int, ...], metadata: tuple[int, ...]
) -> None:
    _assert_admitted(_function(helper, arguments, metadata=metadata))


@pytest.mark.parametrize("row_mask,column_mask", [(1, 32), (2, 31)])
def test_tail_copy_uses_the_audited_helper_contract(row_mask: int, column_mask: int) -> None:
    _assert_admitted(_function("copy_gm_to_ub<float, 32, 2>", (32, row_mask, column_mask, 0), metadata=(2, 32)))


@pytest.mark.parametrize("row_mask,column_mask", [(0, 32), (2, 0)])
def test_zero_mask_copy_is_rejected(row_mask: int, column_mask: int) -> None:
    original = _function("copy_gm_to_ub<float, 32, 2>", (32, row_mask, column_mask, 0), metadata=(2, 32))
    with pytest.raises(tvm.error.TVMError, match="GM-to-UB tail-copy contract"):
        _run(original)


def test_symbolic_full_masks_are_proven_in_their_actual_guard_context() -> None:
    rows = tir.Var("rows", "int32")
    columns = tir.Var("columns", "int32")
    original = _function(
        "copy_gm_to_ub<float, 32, 2>",
        (32, rows, columns, 0),
        scalars=(rows, columns),
        guard=tir.And(rows == 2, columns == 32),
        metadata=(2, 32),
    )
    _assert_admitted(original)


def test_let_bound_full_mask_is_proven() -> None:
    columns = tir.Var("columns", "int32")
    original = _function(
        "copy_gm_to_ub<float, 64>",
        (64, 1, columns, 0),
        binding=(columns, tir.IntImm("int32", 64)),
    )
    _assert_admitted(original)


@pytest.mark.parametrize("guard_kind", ["none", "insufficient"])
def test_symbolic_masks_without_bounds_are_rejected(guard_kind: str) -> None:
    columns = tir.Var("columns", "int32")
    guard = {"none": None, "insufficient": columns <= 64}[guard_kind]
    original = _function(
        "copy_gm_to_ub<float, 64>",
        (64, 1, columns, 0),
        scalars=(columns,),
        guard=guard,
    )
    with pytest.raises(tvm.error.TVMError, match="GM-to-UB tail-copy contract"):
        _run(original)


def test_symbolic_tail_mask_is_proven_in_its_actual_guard_context() -> None:
    columns = tir.Var("columns", "int32")
    _assert_admitted(
        _function(
            "copy_gm_to_ub<float, 64>",
            (64, 1, columns, 0),
            scalars=(columns,),
            guard=columns == 63,
        )
    )


def test_explicit_masks_without_template_dimensions_are_rejected() -> None:
    with pytest.raises(tvm.error.TVMError, match="GM-to-UB tail-copy contract"):
        _run(_function("copy_gm_to_ub", (64, 1, 64, 0)))


def test_narrow_mask_cannot_truncate_the_expected_template_dimension() -> None:
    # A uint8 value of zero is not a full mask for dstN=256. Constructing the
    # expected dimension in uint8 would incorrectly turn this comparison true.
    original = _function("copy_gm_to_ub<float, 256>", (256, 1, tir.IntImm("uint8", 0), 0))
    with pytest.raises(tvm.error.TVMError, match="GM-to-UB tail-copy contract"):
        _run(original)
