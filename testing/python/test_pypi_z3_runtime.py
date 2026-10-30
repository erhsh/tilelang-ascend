# Copyright (c) Tile-AI Organization.
# Licensed under the MIT License.
"""Python-package Z3 resolution and real ELF handoff, without importing TileLang."""

import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys

import pytest


_HELPER = Path(__file__).resolve().parents[2] / "tilelang" / "_native_runtime.py"
_SPEC = importlib.util.spec_from_file_location("pypi_z3_runtime_helper", _HELPER)
runtime = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(runtime)


def _manifest(directory, entries):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / runtime.NATIVE_RUNTIME_MANIFEST).write_text("".join(f"{entry}\n" for entry in entries))


def _marker(directory, enabled):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / runtime.PYPI_RUNTIME_CONFIG).write_text(json.dumps({"enabled": enabled}))


@pytest.mark.parametrize("enabled", [False, True])
def test_current_manifest_controls_package_without_environment_override(tmp_path, enabled):
    build, package = tmp_path / "build", tmp_path / "package"
    build.mkdir()
    library = build / "libz3.so"
    library.write_bytes(b"current-build-provider")
    _manifest(build, [library] if enabled else [])
    package.mkdir()
    (package / "libz3.so").write_bytes(b"stale")
    (package / "libz3.so.4.15").symlink_to(tmp_path / "missing")
    (package / "libunrelated.so").write_bytes(b"preserve")
    assert runtime.configure_pypi_runtime([build], [package]) is enabled
    assert runtime._pypi_runtime_enabled(package) is enabled
    assert not list(package.glob("libz3.so*"))
    assert library.read_bytes() == b"current-build-provider"
    assert (package / "libunrelated.so").read_bytes() == b"preserve"


def test_runtime_configuration_requires_the_configured_build_and_protects_sources(tmp_path):
    with pytest.raises(FileNotFoundError, match="manifest"):
        runtime.configure_pypi_runtime([tmp_path], [tmp_path / "package"])
    _manifest(tmp_path, [])
    with pytest.raises(ValueError, match="overwrite CMake"):
        runtime.configure_pypi_runtime([tmp_path], [tmp_path])


@pytest.mark.parametrize("contents", ['{"enabled": 1}', "{}", "null", "not-json"])
def test_malformed_marker_does_not_silently_become_native_or_off(tmp_path, contents):
    (tmp_path / runtime.PYPI_RUNTIME_CONFIG).write_text(contents)
    with pytest.raises(RuntimeError, match="Invalid native runtime configuration"):
        runtime.preload_pypi_z3_runtime(tmp_path / "libtilelang_module.so")


@pytest.mark.parametrize("marker", [None, False])
def test_off_and_native_builds_do_not_query_or_load_pypi_z3(tmp_path, monkeypatch, marker):
    if marker is not None:
        _marker(tmp_path, marker)

    def forbidden(*args, **kwargs):
        pytest.fail("OFF/native builds must not query or load Python-package Z3")

    monkeypatch.setattr(runtime.metadata, "distribution", forbidden)
    monkeypatch.setattr(runtime.ctypes, "CDLL", forbidden)
    assert runtime.preload_pypi_z3_runtime(tmp_path / "libtilelang_module.so") is None


def test_missing_pypi_dependency_is_actionable(tmp_path, monkeypatch):
    _marker(tmp_path, True)

    def missing(name):
        raise runtime.metadata.PackageNotFoundError(name)

    monkeypatch.setattr(runtime.metadata, "distribution", missing)
    with pytest.raises(RuntimeError, match="Install z3-solver>=4.13.0,<4.15.5"):
        runtime.preload_pypi_z3_runtime(tmp_path / "libtilelang_module.so")


@pytest.mark.parametrize("version", ["4.12.6.0", "4.15.5.0", "4.16.0", "broken"])
def test_outside_main_version_range_is_rejected_before_loading(tmp_path, monkeypatch, version):
    _marker(tmp_path, True)
    distribution = type("Distribution", (), {"version": version})()
    monkeypatch.setattr(runtime.metadata, "distribution", lambda name: distribution)
    monkeypatch.setattr(runtime.ctypes, "CDLL", lambda *args, **kwargs: pytest.fail("loaded"))
    with pytest.raises(RuntimeError, match="Requires z3-solver"):
        runtime.preload_pypi_z3_runtime(tmp_path / "libtilelang_module.so")


@pytest.mark.parametrize("version", [(4, 13, 0, 0), (4, 15, 4, 0), (4, 15, 5, 0)])
def test_native_version_checked_and_provider_handle_retained(tmp_path, monkeypatch, version):
    _marker(tmp_path / "package", True)
    binary = tmp_path / "z3/lib/libz3.so"
    binary.parent.mkdir(parents=True)
    binary.write_bytes(b"provider")
    distribution = type(
        "Distribution",
        (),
        {"version": "4.15.4.0", "locate_file": lambda self, name: tmp_path / name},
    )()
    monkeypatch.setattr(runtime.metadata, "distribution", lambda name: distribution)

    class GetVersion:
        def __call__(self, *pointers):
            for pointer, value in zip(pointers, version):
                ctypes.cast(pointer, ctypes.POINTER(ctypes.c_uint))[0] = value

    handle = type("Provider", (), {"Z3_get_version": GetVersion()})()
    calls = []

    def load(path, *, mode):
        calls.append((path, mode))
        return handle

    monkeypatch.setattr(runtime.ctypes, "CDLL", load)
    if version[:3] >= (4, 15, 5):
        with pytest.raises(RuntimeError, match="Native Z3 version"):
            runtime.preload_pypi_z3_runtime(tmp_path / "package/libtilelang_module.so")
    else:
        assert runtime.preload_pypi_z3_runtime(tmp_path / "package/libtilelang_module.so") is handle
        assert runtime.preload_pypi_z3_runtime(tmp_path / "package/libtilelang_module.so") is handle
        assert calls == [(str(binary.resolve()), ctypes.RTLD_GLOBAL)]


@pytest.fixture
def native_build(tmp_path):
    compiler = shutil.which("cc")
    if not sys.platform.startswith("linux") or not compiler or not shutil.which("patchelf"):
        pytest.skip("real ELF check requires Linux, a C compiler and patchelf")
    build = tmp_path / "native-build"
    build.mkdir()
    provider = build / "libz3.so"
    body = """
void Z3_get_version(unsigned *a, unsigned *b, unsigned *c, unsigned *d) {
  *a = 4; *b = 15; *c = 4; *d = 0;
}
"""
    subprocess.run(
        [
            compiler,
            "-shared",
            "-fPIC",
            "-x",
            "c",
            "-",
            "-Wl,-soname,libz3.so.4.15",
            "-o",
            str(provider),
        ],
        input=body,
        text=True,
        check=True,
        capture_output=True,
    )
    body = """
extern void Z3_get_version(unsigned *, unsigned *, unsigned *, unsigned *);
int probe(void) {
  unsigned a, b, c, d;
  Z3_get_version(&a, &b, &c, &d);
  return a * 10000 + b * 100 + c;
}
"""
    consumer = build / "consumer.so"
    subprocess.run(
        [
            compiler,
            "-shared",
            "-fPIC",
            "-x",
            "c",
            "-",
            "-x",
            "none",
            str(provider),
            "-o",
            str(consumer),
        ],
        input=body,
        text=True,
        check=True,
        capture_output=True,
    )
    for name in runtime.NATIVE_LIBRARIES:
        shutil.copy2(consumer, build / name)
    _manifest(build, [provider])
    return build


def test_real_elf_copy_and_cross_soname_runtime_resolution(native_build, tmp_path):
    before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in native_build.glob("*.so")}
    package = tmp_path / "package"
    runtime.configure_pypi_runtime([native_build], [package])
    runtime.stage_packaged_libraries([native_build], [package])
    for name in runtime.NATIVE_LIBRARIES:
        needed = subprocess.check_output(["patchelf", "--print-needed", str(package / name)], text=True)
        assert "libz3" not in needed
        assert subprocess.check_output(["patchelf", "--print-rpath", str(package / name)], text=True).strip() == "$ORIGIN"
    assert before == {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in native_build.glob("*.so")}
    assert not list(package.glob("libz3.so*"))

    # An independently installed runtime provider has a different SONAME.
    installed = tmp_path / "installed"
    provider = installed / "z3/lib/libz3.so"
    provider.parent.mkdir(parents=True)
    subprocess.run(
        [
            shutil.which("cc"),
            "-shared",
            "-fPIC",
            "-x",
            "c",
            "-",
            "-Wl,-soname,libz3.so.4.13",
            "-o",
            str(provider),
        ],
        input="void Z3_get_version(unsigned*a,unsigned*b,unsigned*c,unsigned*d){*a=4;*b=13;*c=0;*d=0;}",
        text=True,
        check=True,
        capture_output=True,
    )
    info = installed / "z3_solver-4.13.0.0.dist-info"
    info.mkdir()
    (info / "METADATA").write_text("Metadata-Version: 2.1\nName: z3-solver\nVersion: 4.13.0.0\n")
    script = """
import ctypes, importlib.util, sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
spec = importlib.util.spec_from_file_location('helper', sys.argv[2])
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
library = sys.argv[3]
try:
    ctypes.CDLL(library)
except OSError as error:
    assert 'Z3_get_version' in str(error), error
else:
    raise AssertionError('unprovided symbol was unexpectedly resolved')
provider = helper.preload_pypi_z3_runtime(library)
assert provider is not None
consumer = ctypes.CDLL(library)
consumer.probe.restype = ctypes.c_int
assert consumer.probe() == 41300
assert str(Path(sys.argv[1]) / 'z3/lib/libz3.so') in Path('/proc/self/maps').read_text()
"""
    subprocess.run(
        [
            sys.executable,
            "-I",
            "-B",
            "-c",
            script,
            str(installed),
            str(_HELPER),
            str(package / "libtilelang_module.so"),
        ],
        check=True,
        capture_output=True,
        text=True,
    )


def test_off_marker_cannot_publish_an_on_binary(native_build, tmp_path):
    _manifest(native_build, [])
    package = tmp_path / "package"
    runtime.configure_pypi_runtime([native_build], [package])
    with pytest.raises(RuntimeError, match="OFF runtime configuration disagrees"):
        runtime.stage_packaged_libraries([native_build], [package])


def test_missing_tool_is_reported_without_modifying_library(tmp_path, monkeypatch):
    _marker(tmp_path, True)
    library = tmp_path / "libtilelang_module.so"
    library.write_bytes(b"preserve")
    monkeypatch.setattr(runtime.shutil, "which", lambda name: None)
    with pytest.raises(RuntimeError, match="patchelf is required"):
        runtime.patch_packaged_library(library)
    assert library.read_bytes() == b"preserve"


def test_old_wheel_payload_cleanup_preserves_raw_outputs_and_unrelated_files(tmp_path):
    package_root, raw = tmp_path / "build-lib", tmp_path / "build-temp/native"
    package_root.mkdir()
    raw.mkdir(parents=True)
    for name in [*runtime.NATIVE_LIBRARIES, "libz3.so", "libz3.so.4.15"]:
        (package_root / name).write_bytes(b"stale")
        (raw / name).write_bytes(b"raw")
    (package_root / "unrelated.so").write_bytes(b"keep")
    runtime.remove_legacy_package_outputs(package_root, [raw])
    assert sorted(p.name for p in package_root.iterdir()) == ["unrelated.so"]
    assert all(p.read_bytes() == b"raw" for p in raw.iterdir())
    with pytest.raises(ValueError, match="Refusing to clean"):
        runtime.remove_legacy_package_outputs(raw, [raw])
