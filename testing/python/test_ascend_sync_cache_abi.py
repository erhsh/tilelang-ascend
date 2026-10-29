# Copyright (c) Tile-AI Organization.
# Licensed under the MIT License.
"""Stable native engine ABI gates upstream compilation-cache reconstruction."""

import pytest

from tilelang import tvm
from tilelang.autotuner.param import CompileArgs
from tilelang.cache.kernel_cache import KernelCache
from tilelang.cache.tuner_cache import AutoTunerCache
from tilelang.jit.kernel import JITKernel
from tilelang.jit.ascend_runtime_contract import (
    build_cached_auto_gm_manifest,
    prepare_auto_gm_contract,
    sync_engine_cache_abi,
    validate_cached_auto_gm_manifest,
)

_UPSTREAM = {"tl.ascend_sync_backend": "upstream"}
_ABI_FUNCTION = "tl.ascend_sync.EngineCacheABI"


def _function():
    return tvm.tir.PrimFunc([], tvm.tir.Evaluate(0)).with_attr("global_symbol", "main")


@pytest.fixture
def native_abi(monkeypatch):
    state = {"version": "upstream-test-v1", "queries": 0}
    original = tvm.get_global_func

    def lookup(name, allow_missing=False):
        if name != _ABI_FUNCTION:
            return original(name, allow_missing=allow_missing)
        state["queries"] += 1
        return lambda: state["version"]

    monkeypatch.setattr(tvm, "get_global_func", lookup)
    return state


def _key(kind, config):
    # These methods only calculate keys; exercise them without initializing or
    # mutating their process-wide cache singletons.
    if kind == "kernel":
        return KernelCache._generate_key(
            None, _function(), [], [], [], args=(), platform="A3", pass_configs=config
        )
    if kind == "tuner":
        return AutoTunerCache._generate_key(None, _function(), [], args=(), pass_configs=config)
    return hash(CompileArgs(target="ascendc", platform="A3", pass_configs=config))


@pytest.mark.parametrize("kind", ["kernel", "tuner", "compile_args"])
def test_same_native_abi_keeps_keys_stable_and_changed_abi_invalidates(kind, native_abi):
    first = _key(kind, _UPSTREAM)
    assert _key(kind, _UPSTREAM) == first
    native_abi["version"] = "upstream-test-v2"
    assert _key(kind, _UPSTREAM) != first


@pytest.mark.parametrize("kind", ["kernel", "tuner", "compile_args"])
@pytest.mark.parametrize("config", [None, {}, {"tl.ascend_sync_backend": "legacy"}])
def test_nonupstream_keys_keep_their_old_fields_and_do_not_query_native_abi(
    kind, config, native_abi
):
    first = _key(kind, config)
    native_abi["version"] = "different-version"
    assert _key(kind, config) == first
    assert native_abi["queries"] == 0


@pytest.mark.parametrize("kind", ["kernel", "tuner", "compile_args"])
def test_missing_abi_requires_a_native_rebuild_for_upstream_only(kind, monkeypatch):
    monkeypatch.setattr(tvm, "get_global_func", lambda *args, **kwargs: None)
    with pytest.raises(RuntimeError, match="rebuild the native TileLang"):
        _key(kind, _UPSTREAM)
    assert sync_engine_cache_abi(None) is None


@pytest.mark.parametrize("version", [None, "", 1, False])
def test_native_abi_result_must_be_a_nonempty_version(version, native_abi):
    native_abi["version"] = version
    with pytest.raises(RuntimeError, match="rebuild TileLang"):
        sync_engine_cache_abi(_UPSTREAM)


@pytest.mark.parametrize("fresh", [False, True])
def test_upstream_manifest_binds_the_engine_even_without_a_fresh_allocation_marker(
    fresh, native_abi
):
    function = _function()
    if fresh:
        function = prepare_auto_gm_contract(function, "cython", "ascendc")
    manifest = build_cached_auto_gm_manifest(function, "cython", [], 0, _UPSTREAM)
    assert manifest["schema"] == 2
    assert manifest["sync_engine_abi"] == "upstream-test-v1"
    validate_cached_auto_gm_manifest(function, "cython", [], 0, manifest, _UPSTREAM)
    native_abi["version"] = "upstream-test-v2"
    with pytest.raises(ValueError, match="engine cache ABI does not match"):
        validate_cached_auto_gm_manifest(function, "cython", [], 0, manifest, _UPSTREAM)


@pytest.mark.parametrize("fresh", [False, True])
def test_direct_database_reconstruction_rejects_another_engine_before_loading_a_library(
    fresh, native_abi
):
    function = _function()
    if fresh:
        function = prepare_auto_gm_contract(function, "cython", "ascendc")
    manifest = build_cached_auto_gm_manifest(function, "cython", [], 0, _UPSTREAM)
    native_abi["version"] = "upstream-test-v2"
    with pytest.raises(ValueError, match="engine cache ABI does not match"):
        JITKernel.from_database(
            function,
            "",
            "/must/not/be/loaded.so",
            [],
            "ascendc",
            None,
            "A3",
            [],
            None,
            [],
            "cython",
            pass_configs=_UPSTREAM,
            auto_gm_manifest=manifest,
        )


def test_legacy_fresh_manifest_stays_schema_one_and_never_queries_native_abi(native_abi):
    function = prepare_auto_gm_contract(_function(), "cython", "ascendc")
    manifest = build_cached_auto_gm_manifest(function, "cython", [], 0)
    assert manifest["schema"] == 1 and "sync_engine_abi" not in manifest
    validate_cached_auto_gm_manifest(function, "cython", [], 0, manifest)
    assert native_abi["queries"] == 0


@pytest.mark.parametrize("metadata", [None, {}, {"schema": 1}])
def test_unmarked_upstream_database_still_requires_the_engine_manifest(metadata, native_abi):
    with pytest.raises(ValueError, match="complete cache manifest"):
        validate_cached_auto_gm_manifest(_function(), "ctypes", [], 0, metadata, _UPSTREAM)
