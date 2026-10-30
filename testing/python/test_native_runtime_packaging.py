# Copyright (c) Tile-AI Organization.
# Licensed under the MIT License.
"""Packaging checks that do not import TileLang or execute setup.py."""

import importlib.util
from pathlib import Path

import pytest


_HELPER_PATH = Path(__file__).resolve().parents[2] / "tilelang" / "_native_runtime.py"
_SPEC = importlib.util.spec_from_file_location("tilelang_native_runtime_packaging", _HELPER_PATH)
_NATIVE_RUNTIME = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_NATIVE_RUNTIME)


def _write_manifest(directory, entries):
    directory.mkdir(parents=True, exist_ok=True)
    manifest = directory / _NATIVE_RUNTIME.NATIVE_RUNTIME_MANIFEST
    manifest.write_text("".join(f"{entry}\n" for entry in entries), encoding="utf-8")
    return manifest


def test_on_copies_library_and_soname_to_wheel_and_editable(tmp_path):
    binary_dir = tmp_path / "cmake-build"
    output_dir = tmp_path / "native-output"
    output_dir.mkdir()
    sources = [output_dir / name for name in ("libz3.so", "libz3.so.4.15")]
    for source in sources:
        source.write_bytes(b"native-z3")
    _write_manifest(binary_dir, sources)
    destinations = [tmp_path / "wheel" / "tilelang" / "lib", tmp_path / "editable" / "lib"]

    copied = _NATIVE_RUNTIME.copy_native_runtime_files([tmp_path / "no-manifest", binary_dir], destinations, required=True)

    expected = {directory / source.name for directory in destinations for source in sources}
    assert set(copied) == expected
    for source in sources:
        assert source.read_bytes() == b"native-z3"
        for directory in destinations:
            assert (directory / source.name).read_bytes() == b"native-z3"


def test_off_first_manifest_stops_search_despite_stale_libraries(tmp_path):
    current = tmp_path / "current"
    stale = tmp_path / "stale"
    _write_manifest(current, [])
    stale.mkdir()
    stale_library = stale / "libz3.so"
    stale_library.write_bytes(b"stale-z3")
    _write_manifest(stale, [stale_library])
    (current / "libz3.so").write_bytes(b"leftover-from-on-build")
    destination = tmp_path / "package" / "lib"

    assert _NATIVE_RUNTIME.copy_native_runtime_files([current, stale], [destination]) == []
    assert not destination.exists()


def test_first_manifest_selects_its_own_absolute_files(tmp_path):
    sources = []
    for name in ("first", "second"):
        directory = tmp_path / name
        directory.mkdir()
        source = directory / "libz3.so"
        source.write_text(name, encoding="utf-8")
        _write_manifest(directory, [source])
        sources.append(source)
    destination = tmp_path / "package" / "lib"

    _NATIVE_RUNTIME.copy_native_runtime_files([source.parent for source in sources], [destination])

    assert (destination / "libz3.so").read_text(encoding="utf-8") == "first"


def test_missing_native_file_fails_before_copying(tmp_path):
    binary_dir = tmp_path / "build"
    binary_dir.mkdir()
    existing = binary_dir / "libz3.so"
    existing.write_bytes(b"z3")
    missing = binary_dir / "libz3.so.4.15"
    _write_manifest(binary_dir, [existing, missing])
    destination = tmp_path / "package" / "lib"

    with pytest.raises(FileNotFoundError, match="Native runtime file.*is missing"):
        _NATIVE_RUNTIME.copy_native_runtime_files([binary_dir], [destination])
    assert not destination.exists()
    assert existing.exists()


@pytest.mark.parametrize("entry", ["libz3.so", "../libz3.so", "/tmp/not-z3.so", "/tmp/../libz3.so"])
def test_invalid_native_path_entry_is_rejected(tmp_path, entry):
    _write_manifest(tmp_path, [entry])

    with pytest.raises(ValueError, match="Invalid native runtime entry"):
        _NATIVE_RUNTIME.copy_native_runtime_files([tmp_path], [tmp_path / "destination"])


def test_missing_manifest_allows_older_default_build(tmp_path):
    assert _NATIVE_RUNTIME.copy_native_runtime_files([tmp_path], [tmp_path / "destination"]) == []


def test_missing_manifest_is_an_error_when_required(tmp_path):
    with pytest.raises(FileNotFoundError, match="Native runtime manifest not found"):
        _NATIVE_RUNTIME.copy_native_runtime_files([tmp_path], [tmp_path / "destination"], required=True)


def test_required_runtime_rejects_first_off_manifest(tmp_path):
    current = tmp_path / "current"
    stale = tmp_path / "stale"
    _write_manifest(current, [])
    stale.mkdir()
    source = stale / "libz3.so"
    source.write_bytes(b"stale-z3")
    _write_manifest(stale, [source])

    with pytest.raises(ValueError, match="OFF manifest is empty"):
        _NATIVE_RUNTIME.copy_native_runtime_files([current, stale], [tmp_path / "destination"], required=True)


def test_copy_does_not_remove_or_recopy_colocated_source(tmp_path):
    source = tmp_path / "libz3.so"
    source.write_bytes(b"native-z3")
    _write_manifest(tmp_path, [source])

    assert _NATIVE_RUNTIME.copy_native_runtime_files([tmp_path], [tmp_path, tmp_path]) == []
    assert source.read_bytes() == b"native-z3"


def test_on_then_off_removes_staged_z3_and_preserves_sources(tmp_path):
    binary_dir = tmp_path / "build"
    binary_dir.mkdir()
    sources = [binary_dir / name for name in ("libz3.so", "libz3.so.4.15")]
    for source in sources:
        source.write_bytes(b"native-z3")
    _write_manifest(binary_dir, sources)
    destinations = [tmp_path / "wheel" / "tilelang" / "lib", tmp_path / "editable" / "lib"]
    _NATIVE_RUNTIME.copy_native_runtime_files([binary_dir], destinations, required=True)

    _write_manifest(binary_dir, [])
    assert _NATIVE_RUNTIME.copy_native_runtime_files([binary_dir], destinations) == []

    for source in sources:
        assert source.read_bytes() == b"native-z3"
        for destination in destinations:
            assert not (destination / source.name).exists()


def test_off_cleanup_only_removes_matching_direct_files(tmp_path):
    binary_dir = tmp_path / "build"
    _write_manifest(binary_dir, [])
    destination = tmp_path / "package" / "lib"
    destination.mkdir(parents=True)
    preserved = ["libtilelang.so", "libz3.so.debug", "libz3.so.4.15.backup", "notes.txt"]
    for name in preserved:
        (destination / name).write_bytes(b"keep")
    nested = destination / "libz3.so.999"
    nested.mkdir()
    (nested / "libz3.so").write_bytes(b"nested-keep")
    (destination / "libz3.so").write_bytes(b"stale")
    (destination / "libz3.so.4.15").symlink_to("missing-native-file")

    assert _NATIVE_RUNTIME.copy_native_runtime_files([binary_dir], [destination]) == []

    for name in preserved:
        assert (destination / name).read_bytes() == b"keep"
    assert (nested / "libz3.so").read_bytes() == b"nested-keep"
    assert not (destination / "libz3.so").exists()
    assert not (destination / "libz3.so.4.15").is_symlink()


def test_off_cleanup_preserves_search_directories_and_aliases(tmp_path):
    binary_dir = tmp_path / "current-build"
    _write_manifest(binary_dir, [])
    other_source_dir = tmp_path / "other-native-source"
    other_source_dir.mkdir()
    for directory in (binary_dir, other_source_dir):
        (directory / "libz3.so").write_bytes(b"source-library")
    alias = tmp_path / "source-alias"
    alias.symlink_to(binary_dir, target_is_directory=True)

    assert _NATIVE_RUNTIME.copy_native_runtime_files([binary_dir, other_source_dir], [binary_dir, other_source_dir, alias]) == []

    for directory in (binary_dir, other_source_dir, alias):
        assert (directory / "libz3.so").read_bytes() == b"source-library"
