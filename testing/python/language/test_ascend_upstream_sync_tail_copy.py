# Copyright (c) Tile-AI Organization.
# Licensed under the MIT License.
"""Host checks for the real GM-to-UB padding helper's MTE2 boundary.

Expected dependencies come from the stated V write, MTE3 read, whole-tile
padding write and strided GM read. Full/default-mask ABI admission and
symbolic bounds are checked alongside the hidden helper effects. No legacy
synchronization output is used.
"""

from collections.abc import Callable
from math import prod

import pytest

import tilelang
from tilelang import tvm

tir = tvm.tir


@pytest.fixture(autouse=True)
def _requires_proof_service() -> None:
    if not tvm.get_global_func("tl.ascend_sync.Z3ProofServiceAvailable")():
        pytest.skip("requires USE_ASCEND_SYNC_Z3=ON and the migrated sync engine")


def _buffers(dtype: str = "float32", local_elements: int = 64) -> dict[str, tir.Buffer]:
    return {
        "source": tir.decl_buffer((256,), dtype, name="source"),
        "output": tir.decl_buffer((256,), dtype, name="output"),
        "local": tir.decl_buffer((local_elements,), dtype, name="local", scope="shared.ub"),
    }


def _copy(
    buffers: dict[str, tir.Buffer],
    columns: int | tir.PrimExpr = 33,
    *,
    rows: int | tir.PrimExpr = 1,
    stride: int | tir.PrimExpr = 64,
    template: str = "float, 64",
    source_offset: int = 0,
    destination_offset: int = 0,
    source_extent: int = 64,
    destination_extent: int = 64,
    source_mask: str = "r",
    destination_mask: str = "w",
    pad: tir.PrimExpr | int = 0,
) -> tir.Evaluate:
    return tir.Evaluate(
        tir.call_extern(
            "handle",
            f"tl::ascend::copy_gm_to_ub<{template}>",
            buffers["source"].access_ptr(source_mask, offset=source_offset, extent=source_extent),
            buffers["local"].access_ptr(destination_mask, offset=destination_offset, extent=destination_extent),
            stride,
            rows,
            columns,
            pad,
        )
    )


def _fill(buffer: tir.Buffer) -> tir.Evaluate:
    return tir.Evaluate(tir.call_extern("handle", "AscendC::Duplicate", buffer.access_ptr("w")))


def _store(buffers: dict[str, tir.Buffer]) -> tir.Evaluate:
    return tir.Evaluate(
        tir.call_extern(
            "handle",
            "copy_ub_to_gm",
            buffers["local"].access_ptr("r"),
            buffers["output"].access_ptr("w"),
        )
    )


def _function(
    buffers: dict[str, tir.Buffer],
    statements: list[tir.Stmt],
    *,
    scalars: tuple[tir.Var, ...] = (),
    guard: tir.PrimExpr | None = None,
    binding: tuple[tir.Var, tir.PrimExpr] | None = None,
    address: int = 0,
    size: int | None = None,
) -> tir.PrimFunc:
    body = statements[0] if len(statements) == 1 else tir.SeqStmt(statements)
    if guard is not None:
        body = tir.IfThenElse(guard, body, None)
    if binding is not None:
        body = tir.LetStmt(*binding, body)
    body = tir.AttrStmt(tir.IntImm("int32", 0), "resource_scope", 1, body)
    local = buffers["local"]
    elements = prod(int(extent) for extent in local.shape)
    allocation_bytes = elements * (tvm.DataType(local.dtype).bits // 8)
    body = tir.Allocate(local.data, local.dtype, [elements], tir.const(True, "bool"), body)
    parameters = [buffer for name, buffer in buffers.items() if name != "local"]
    return (
        tir.PrimFunc(
            [buffer.data for buffer in parameters] + list(scalars),
            body,
            buffer_map={buffer.data: buffer for buffer in parameters},
        )
        .with_attr("tir.noalias", True)
        .with_attr("address_map", {local.data: address})
        .with_attr("size_map", {local.data: allocation_bytes if size is None else size})
    )


def _run(function: tir.PrimFunc, *, model: str = "ascendc", platform: str = "A3") -> tir.PrimFunc:
    target = tvm.target.Target(f"c -keys=ascend -model={model}")
    with tvm.transform.PassContext(config={"tl.ascend_auto_sync": True, "tl.ascend_auto_sync_vs": True}):
        return tilelang.transform.AscendInsertSync(target, platform)(tvm.IRModule({"main": function}))["main"]


def _calls(function: tir.PrimFunc) -> list[tir.Call]:
    calls = []

    def collect(node: object) -> None:
        if isinstance(node, tir.Call):
            calls.append(node)

    tir.stmt_functor.post_order_visit(function.body, collect)
    return calls


def _events(function: tir.PrimFunc) -> list[tir.Call]:
    return [
        call
        for call in _calls(function)
        if isinstance(call.op, tvm.ir.Op) and call.op.name in {"tl.ascend_auto_set_flag", "tl.ascend_auto_wait_flag"}
    ]


def _helper_calls(function: tir.PrimFunc) -> list[tir.Call]:
    return [call for call in _calls(function) if call.op == tvm.ir.Op.get("tir.call_extern") and "copy_gm_to_ub<" in call.args[0].value]


def _assert_hidden_slots_reserved(function: tir.PrimFunc) -> None:
    for event in _events(function):
        if event.args[0].value in {"MTE2_V", "MTE3_V", "V_MTE2"}:
            assert isinstance(event.args[1], tir.IntImm)
            assert int(event.args[1]) != 0


@pytest.mark.parametrize("platform", ["A2", "A3"])
@pytest.mark.parametrize("model", ["ascendc", "auto"])
def test_tail_copy_orders_prior_vector_and_mte3_accesses_through_mte2(platform: str, model: str) -> None:
    buffers = _buffers()
    copy = _copy(buffers)
    original = _function(buffers, [_fill(buffers["local"]), _store(buffers), copy, _store(buffers)])
    result = _run(original, platform=platform, model=model)
    directions = {event.args[0].value for event in _events(result)}
    # The first store already waits for V, so V->MTE3->MTE2 is a sufficient
    # transitive acquire for the copy. Requiring another V->MTE2 is redundant.
    assert {"V_MTE3", "MTE3_MTE2", "MTE2_MTE3"} <= directions
    ordered = _calls(result)
    positions = {
        name: [index for index, call in enumerate(ordered) if call.op == tvm.ir.Op.get("tir.call_extern") and name in call.args[0].value]
        for name in ("Duplicate", "copy_ub_to_gm", "copy_gm_to_ub")
    }
    handoffs = (
        (positions["Duplicate"][0], positions["copy_ub_to_gm"][0], "V_MTE3"),
        (positions["copy_ub_to_gm"][0], positions["copy_gm_to_ub"][0], "MTE3_MTE2"),
        (positions["copy_gm_to_ub"][0], positions["copy_ub_to_gm"][1], "MTE2_MTE3"),
    )
    for producer, consumer, event in handoffs:
        sets = [
            (index, call)
            for index, call in enumerate(ordered)
            if isinstance(call.op, tvm.ir.Op) and call.op.name == "tl.ascend_auto_set_flag" and call.args[0].value == event
        ]
        assert any(
            producer < set_index < wait_index < consumer
            for set_index, set_call in sets
            for wait_index, wait_call in enumerate(ordered)
            if isinstance(wait_call.op, tvm.ir.Op)
            and wait_call.op.name == "tl.ascend_auto_wait_flag"
            and wait_call.args[0].value == event
            and int(wait_call.args[1]) == int(set_call.args[1])
        )
    helpers = _helper_calls(result)
    assert len(helpers) == 1
    assert tvm.ir.structural_equal(helpers[0], copy.value)
    _assert_hidden_slots_reserved(result)


def test_dynamic_full_or_tail_keeps_one_original_call_and_an_mte2_acquire() -> None:
    columns = tir.Var("columns", "int32")
    buffers = _buffers()
    copy = _copy(buffers, columns)
    original = _function(
        buffers,
        [_fill(buffers["local"]), copy, _store(buffers)],
        scalars=(columns,),
        guard=tir.And(columns >= 1, columns <= 64),
    )
    result = _run(original)
    assert len(_helper_calls(result)) == 1
    assert any(event.args[0].value == "V_MTE2" for event in _events(result))
    _assert_hidden_slots_reserved(result)


@pytest.mark.parametrize(
    "dtype,template",
    [
        ("float16", "half, 64"),
        ("bfloat16", "bfloat16_t, 64"),
        ("int16", "int16_t, 64"),
        ("uint16", "uint16_t, 64"),
        ("int32", "int32_t, 64"),
        ("uint32", "uint32_t, 64"),
    ],
)
def test_duplicate_supported_types_have_the_same_completion_contract(dtype: str, template: str) -> None:
    buffers = _buffers(dtype)
    result = _run(_function(buffers, [_copy(buffers, template=template), _store(buffers)]))
    assert len(_helper_calls(result)) == 1
    assert any(event.args[0].value == "MTE2_MTE3" for event in _events(result))


def test_nominal_pointer_extent_cannot_hide_the_strided_gm_read() -> None:
    buffers = _buffers(local_elements=128)
    copy = _copy(
        buffers,
        16,
        rows=2,
        stride=128,
        template="float, 64, 2",
        source_extent=1,
        destination_extent=1,
    )
    # Row two reads source[128:144], outside the deliberately tiny access_ptr
    # extent. This scalar overwrite must wait for that GM read to complete.
    overwrite = tir.BufferStore(buffers["source"], tir.const(42, "float32"), [128])
    result = _run(_function(buffers, [copy, overwrite, _store(buffers)]))
    assert any(event.args[0].value == "MTE2_S" for event in _events(result))


@pytest.mark.parametrize("direction", ["MTE2_V", "MTE3_V", "V_MTE2"])
@pytest.mark.parametrize("position", ["before", "after"])
@pytest.mark.parametrize("api", ["auto", "manual"])
def test_explicit_event_zero_conflicts_even_before_the_first_tail_copy(direction: str, position: str, api: str) -> None:
    buffers = _buffers()
    source, destination = direction.split("_")
    arguments = (
        [tir.StringImm(direction), 0] if api == "auto" else [tir.StringImm(f"PIPE_{source}"), tir.StringImm(f"PIPE_{destination}"), 0]
    )
    manual = [
        tir.Evaluate(tir.Call("handle", tvm.ir.Op.get(name), arguments))
        for name in (
            ("tl.ascend_auto_set_flag", "tl.ascend_auto_wait_flag") if api == "auto" else ("tl.ascend_set_flag", "tl.ascend_wait_flag")
        )
    ]
    computation = [_copy(buffers), _store(buffers)]
    statements = manual + computation if position == "before" else computation + manual
    with pytest.raises(tvm.error.TVMError, match="explicit event conflicts with hidden"):
        _run(_function(buffers, statements))


def test_other_explicit_slot_is_preserved_and_excluded_from_automatic_allocation() -> None:
    buffers = _buffers()
    manual = [
        tir.Evaluate(tir.Call("handle", tvm.ir.Op.get(name), [tir.StringImm("V_MTE2"), 1]))
        for name in ("tl.ascend_auto_set_flag", "tl.ascend_auto_wait_flag")
    ]
    result = _run(_function(buffers, manual + [_fill(buffers["local"]), _copy(buffers)]))
    sets = [event for event in _events(result) if event.op.name == "tl.ascend_auto_set_flag" and event.args[0].value == "V_MTE2"]
    assert len(sets) >= 2
    assert sum(int(event.args[1]) == 1 for event in sets) == 1
    assert all(int(event.args[1]) != 0 for event in sets)


@pytest.mark.parametrize("columns", [0, -1, 65, tir.IntImm("uint32", 2**32 - 1)])
def test_out_of_range_masks_are_rejected(columns: int | tir.PrimExpr) -> None:
    buffers = _buffers()
    with pytest.raises(tvm.error.TVMError, match="bounded masks"):
        _run(_function(buffers, [_copy(buffers, columns)]))


@pytest.mark.parametrize("rows", [0, -1, 2])
def test_invalid_row_masks_are_rejected(rows: int) -> None:
    buffers = _buffers()
    with pytest.raises(tvm.error.TVMError, match="bounded masks"):
        _run(_function(buffers, [_copy(buffers, rows=rows)]))


@pytest.mark.parametrize("stride", [32, -1, 2**32, 2**32 - 1])
def test_invalid_or_wrapping_dma_strides_are_rejected(stride: int) -> None:
    buffers = _buffers()
    with pytest.raises(tvm.error.TVMError, match="bounded masks|nonwrapping DMA stride"):
        _run(_function(buffers, [_copy(buffers, stride=tir.IntImm("int64", stride))]))


def test_symbolic_masks_need_a_proof_of_the_complete_valid_range() -> None:
    columns = tir.Var("columns", "int32")
    buffers = _buffers()
    with pytest.raises(tvm.error.TVMError, match="bounded masks"):
        _run(_function(buffers, [_copy(buffers, columns)], scalars=(columns,)))


@pytest.mark.parametrize(
    "change,message",
    [
        (lambda: {"source_offset": 250}, "strided GM read is in bounds"),
        (lambda: {"destination_offset": 1}, "misaligned or exceeds"),
        (lambda: {"template": "float, 63"}, "32-byte-aligned row pitch"),
        (lambda: {"template": "half, 64"}, "element types disagree"),
        (lambda: {"source_mask": "rw"}, "exact source-read/destination-write"),
        (lambda: {"destination_mask": "rw"}, "exact source-read/destination-write"),
    ],
)
def test_tail_footprint_and_pointer_contracts_fail_closed(change: Callable[[], dict[str, object]], message: str) -> None:
    buffers = _buffers()
    with pytest.raises(tvm.error.TVMError, match=message):
        _run(_function(buffers, [_copy(buffers, **change())]))


def test_whole_duplicate_tile_must_fit_even_when_dma_mask_is_small() -> None:
    buffers = _buffers()
    with pytest.raises(tvm.error.TVMError, match="whole padded UB tile"):
        _run(_function(buffers, [_copy(buffers, 1)], size=128))


@pytest.mark.parametrize("dtype,template", [("int8", "int8_t, 64"), ("uint8", "uint8_t, 64")])
def test_types_without_the_duplicate_padding_path_are_not_misclassified(dtype: str, template: str) -> None:
    buffers = _buffers(dtype)
    with pytest.raises(tvm.error.TVMError, match="type supported by Duplicate"):
        _run(_function(buffers, [_copy(buffers, template=template)]))


@pytest.mark.parametrize("model,platform", [("pto", "A2"), ("pto", "A5"), ("ascendc", "A5")])
def test_tail_support_is_not_inferred_for_other_codegen_targets(model: str, platform: str) -> None:
    buffers = _buffers()
    with pytest.raises(tvm.error.TVMError, match="requires AscendC on A2/A3"):
        _run(_function(buffers, [_copy(buffers)]), model=model, platform=platform)


def test_pto_tail_copy_requires_authoritative_shape_descriptors() -> None:
    buffers = _buffers()
    with pytest.raises(tvm.error.TVMError, match="PTO tail-copy profile: requires final GM/UB physical shape metadata"):
        _run(_function(buffers, [_copy(buffers)]), model="pto")


def test_pto_tail_copy_accepts_complete_a3_descriptors() -> None:
    buffers = _buffers()
    buffers["source"] = tir.decl_buffer((4, 64), "float32", name="source")
    buffers["local"] = tir.decl_buffer((1, 64), "float32", name="local", scope="shared.ub")
    original = _function(buffers, [_copy(buffers), _store(buffers)]).with_attr(
        "logic_buffer_shapes",
        {buffers["source"].data: [4, 64], buffers["local"].data: [1, 64, 1, 64]},
    )
    result = _run(original, model="pto")
    assert result.attrs.get("tl.ascend_sync_proof") is not None
    assert "MTE2_MTE3" in {event.args[0].value for event in _events(result)}
    tvm.ir.assert_structural_equal(_helper_calls(original)[0], _helper_calls(result)[0])
    tvm.ir.assert_structural_equal(result, _run(result, model="pto"))


def test_proven_full_masks_keep_the_existing_pto_path() -> None:
    buffers = _buffers()
    result = _run(_function(buffers, [_copy(buffers, 64), _store(buffers)]), model="pto")
    assert len(_helper_calls(result)) == 1


def test_mutable_mask_is_captured_once_and_reused_by_the_original_call() -> None:
    buffers = _buffers()
    buffers["control"] = tir.decl_buffer((1,), "int32", name="control")
    columns = tir.Var("captured_columns", "int32")
    # Guard the captured value, rather than assert any equality between that
    # value and a future reread of the control buffer.
    original = _function(
        buffers,
        [_copy(buffers, columns), _store(buffers)],
        guard=tir.And(columns >= 1, columns <= 64),
        binding=(columns, tir.BufferLoad(buffers["control"], [0])),
    )
    result = _run(original)
    loads = []

    def collect(node: object) -> None:
        if isinstance(node, tir.BufferLoad) and node.buffer.data.same_as(buffers["control"].data):
            loads.append(node)

    tir.stmt_functor.post_order_visit(result.body, collect)
    assert len(loads) == 1
    helper = _helper_calls(result)[0]
    assert isinstance(helper.args[5], tir.Var)
    assert helper.args[5].same_as(columns)


@pytest.mark.parametrize("template", ["float, 1073741824, 3", "float, 64, 65536"])
def test_template_counts_cannot_overflow_the_native_duplicate_or_dma_abi(template: str) -> None:
    buffers = _buffers()
    with pytest.raises(tvm.error.TVMError, match="Duplicate/DMA count limits"):
        _run(_function(buffers, [_copy(buffers, template=template)]))


def test_vector_padding_value_is_rejected_before_hardware_codegen() -> None:
    buffers = _buffers()
    with pytest.raises(tvm.error.TVMError, match="padding value must be a captured arithmetic scalar"):
        _run(
            _function(
                buffers,
                [_copy(buffers, pad=tir.Broadcast(tir.const(0, "float32"), 4))],
            )
        )


def test_mutable_padding_scalar_is_read_once_at_its_original_call_boundary() -> None:
    buffers = _buffers()
    buffers["pad"] = tir.decl_buffer((1,), "float32", name="pad")
    result = _run(
        _function(
            buffers,
            [_copy(buffers, pad=tir.BufferLoad(buffers["pad"], [0])), _store(buffers)],
        )
    )
    loads = []

    def collect(node: object) -> None:
        if isinstance(node, tir.BufferLoad) and node.buffer.data.same_as(buffers["pad"].data):
            loads.append(node)

    tir.stmt_functor.post_order_visit(result.body, collect)
    assert len(loads) == 1
    assert isinstance(_helper_calls(result)[0].args[6], tir.Var)


# Full/default-mask helper admission retains its original ABI fixture and target.
def _admission_function(
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


def _run_admission(function: tir.PrimFunc) -> tir.PrimFunc:
    target = tvm.target.Target({"kind": "llvm", "model": "ascendc"})
    with tvm.transform.PassContext(config={"tl.ascend_auto_sync": True, "tl.ascend_auto_sync_vs": True}):
        result = tilelang.transform.AscendInsertSync(target, "A3")(tvm.IRModule({"main": function}))
    return result["main"]


def _assert_admitted(function: tir.PrimFunc) -> None:
    result = _run_admission(function)
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
    _assert_admitted(_admission_function(helper, arguments, metadata=metadata))


@pytest.mark.parametrize("row_mask,column_mask", [(1, 32), (2, 31)])
def test_tail_copy_uses_the_audited_helper_contract(row_mask: int, column_mask: int) -> None:
    _assert_admitted(_admission_function("copy_gm_to_ub<float, 32, 2>", (32, row_mask, column_mask, 0), metadata=(2, 32)))


@pytest.mark.parametrize("row_mask,column_mask", [(0, 32), (2, 0)])
def test_zero_mask_copy_is_rejected(row_mask: int, column_mask: int) -> None:
    original = _admission_function("copy_gm_to_ub<float, 32, 2>", (32, row_mask, column_mask, 0), metadata=(2, 32))
    with pytest.raises(tvm.error.TVMError, match="GM-to-UB tail-copy contract"):
        _run_admission(original)


def test_symbolic_full_masks_are_proven_in_their_actual_guard_context() -> None:
    rows = tir.Var("rows", "int32")
    columns = tir.Var("columns", "int32")
    original = _admission_function(
        "copy_gm_to_ub<float, 32, 2>",
        (32, rows, columns, 0),
        scalars=(rows, columns),
        guard=tir.And(rows == 2, columns == 32),
        metadata=(2, 32),
    )
    _assert_admitted(original)


def test_let_bound_full_mask_is_proven() -> None:
    columns = tir.Var("columns", "int32")
    original = _admission_function(
        "copy_gm_to_ub<float, 64>",
        (64, 1, columns, 0),
        binding=(columns, tir.IntImm("int32", 64)),
    )
    _assert_admitted(original)


@pytest.mark.parametrize("guard_kind", ["none", "insufficient"])
def test_symbolic_masks_without_bounds_are_rejected(guard_kind: str) -> None:
    columns = tir.Var("columns", "int32")
    guard = {"none": None, "insufficient": columns <= 64}[guard_kind]
    original = _admission_function(
        "copy_gm_to_ub<float, 64>",
        (64, 1, columns, 0),
        scalars=(columns,),
        guard=guard,
    )
    with pytest.raises(tvm.error.TVMError, match="GM-to-UB tail-copy contract"):
        _run_admission(original)


def test_symbolic_tail_mask_is_proven_in_its_actual_guard_context() -> None:
    columns = tir.Var("columns", "int32")
    _assert_admitted(
        _admission_function(
            "copy_gm_to_ub<float, 64>",
            (64, 1, columns, 0),
            scalars=(columns,),
            guard=columns == 63,
        )
    )


def test_explicit_masks_without_template_dimensions_are_rejected() -> None:
    with pytest.raises(tvm.error.TVMError, match="GM-to-UB tail-copy contract"):
        _run_admission(_admission_function("copy_gm_to_ub", (64, 1, 64, 0)))


def test_narrow_mask_cannot_truncate_the_expected_template_dimension() -> None:
    # A uint8 value of zero is not a full mask for dstN=256. Constructing the
    # expected dimension in uint8 would incorrectly turn this comparison true.
    original = _admission_function("copy_gm_to_ub<float, 256>", (256, 1, tir.IntImm("uint8", 0), 0))
    with pytest.raises(tvm.error.TVMError, match="GM-to-UB tail-copy contract"):
        _run_admission(original)
