"""Record and verify the source and library identity of an explicit local build."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
SOURCE_PATHS = ("CMakeLists.txt", "config.cmake", "src", "include", "cmake", "3rdparty/patches")
TVM_PATHS = ("CMakeLists.txt", "config.cmake", "src", "include", "cmake", "3rdparty", "python")
TVM_COMMIT = "c2921fdaf795b1103d21abc962e83a209c7258d7"
TVM_PATCHES = (
    "3rdparty/patches/tvm_backtrace_symbol_name_length.patch",
    "3rdparty/patches/tvm_slice_step_fix.patch",
)
LIBRARY_PATHS = (
    "build/libtilelang_module.so",
    "build/tvm/libtvm.so",
    "build/tvm/libtvm_runtime.so",
)


def file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def tvm_provenance(tvm_source: Path) -> dict[str, Any]:
    try:
        commit = subprocess.check_output(
            ["git", "-C", str(tvm_source), "rev-parse", "HEAD"], text=True
        ).strip()
        for patch in TVM_PATCHES:
            subprocess.run(
                ["git", "-C", str(tvm_source), "apply", "--reverse", "--check", str(ROOT / patch)],
                check=True,
                capture_output=True,
                text=True,
            )
    except subprocess.CalledProcessError as error:
        raise ValueError("Actual TVM source does not have the recorded base and patches") from error
    if commit != TVM_COMMIT:
        raise ValueError(f"TVM reference checkout changed: {commit}")
    return {
        "reference_checkout": str(tvm_source),
        "reference_commit": commit,
        "applied_patches": [
            {"path": name, "sha256": file_digest(ROOT / name)} for name in TVM_PATCHES
        ],
    }


def source_identity(root: Path, paths: tuple[str, ...]) -> dict[str, Any]:
    """Hash source contents and logical names, including linked dependency headers."""
    files: list[Path] = []
    for name in paths:
        start = root / name
        if start.is_file():
            files.append(start)
        elif start.is_dir():
            visited: set[Path] = set()
            for current, directories, names in os.walk(start, followlinks=True):
                resolved = Path(current).resolve()
                if resolved in visited:
                    directories[:] = []
                    continue
                visited.add(resolved)
                directories[:] = sorted(
                    item for item in directories if item not in {".git", "__pycache__"}
                )
                files.extend(Path(current) / item for item in names if item != ".git")
    if not files:
        raise ValueError(f"No source inputs found under {root}")
    digest = hashlib.sha256()
    for path in sorted(files):
        digest.update(str(path.relative_to(root)).encode())
        digest.update(b"\0")
        digest.update(file_digest(path).encode())
        digest.update(b"\n")
    return {"algorithm": "sha256", "digest": digest.hexdigest(), "files": len(files)}


def verify_cmake_source(cache: Path, tvm_source: Path) -> None:
    entries = {}
    for line in cache.read_text().splitlines():
        if not line.startswith(("#", "//")) and ":" in line and "=" in line:
            name, value = line.split("=", 1)
            entries[name.split(":", 1)[0]] = value
    for key, expected in (("TVM_SOURCE_DIR", tvm_source), ("CMAKE_HOME_DIRECTORY", ROOT)):
        if key not in entries or Path(entries[key]).resolve() != expected.resolve():
            raise ValueError(f"CMakeCache {key} does not identify the recorded build source")


def write_manifest(
    output: str | Path,
    *,
    tvm_source: str | Path,
    build_command: list[str],
    build_log: str | Path,
) -> dict[str, Any]:
    """Call only after the recorded build command has completed successfully."""
    tvm_source = Path(tvm_source).resolve()
    build_log = Path(build_log).resolve()
    libraries = []
    for name in LIBRARY_PATHS:
        path = (ROOT / name).resolve()
        libraries.append({"path": str(path), "sha256": file_digest(path)})
    cache = ROOT / "build/CMakeCache.txt"
    verify_cmake_source(cache, tvm_source)
    report = {
        "schema_version": 1,
        "repo_root": str(ROOT),
        "project_source": source_identity(ROOT, SOURCE_PATHS),
        "tvm_source_root": str(tvm_source),
        "tvm_source": source_identity(tvm_source, TVM_PATHS),
        "tvm_provenance": tvm_provenance(tvm_source),
        "cmake_cache": {"path": str(cache), "sha256": file_digest(cache)},
        "libraries": libraries,
        "build": {
            "command": build_command,
            "log": str(build_log),
            "log_sha256": file_digest(build_log),
        },
    }
    destination = Path(output)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(report, indent=2) + "\n")
    return report


def verify_manifest(path: str | Path) -> dict[str, Any]:
    """Reject stale source, library, configure, or build-log identities."""
    path = Path(path).resolve()
    report = json.loads(path.read_text())
    if not isinstance(report, dict):
        raise ValueError("Native build manifest must be a JSON object")
    if report.get("schema_version") != 1 or report.get("repo_root") != str(ROOT):
        raise ValueError("Native build manifest schema or repository does not match")
    if report["project_source"] != source_identity(ROOT, SOURCE_PATHS):
        raise ValueError("Native build manifest does not match the current project sources")
    tvm_source = Path(report["tvm_source_root"])
    verify_cmake_source(Path(report["cmake_cache"]["path"]), tvm_source)
    if report["tvm_provenance"] != tvm_provenance(tvm_source):
        raise ValueError("Native build manifest does not match the TVM commit and patches")
    if report["tvm_source"] != source_identity(tvm_source, TVM_PATHS):
        raise ValueError("Native build manifest does not match the actual TVM build sources")
    expected = {str((ROOT / name).resolve()) for name in LIBRARY_PATHS}
    if {item["path"] for item in report["libraries"]} != expected:
        raise ValueError("Native build manifest does not identify all required local libraries")
    for item in [*report["libraries"], report["cmake_cache"]]:
        if file_digest(Path(item["path"])) != item["sha256"]:
            raise ValueError(f"Native build input or library changed: {item['path']}")
    build = report["build"]
    if not build.get("command") or file_digest(Path(build["log"])) != build["log_sha256"]:
        raise ValueError("Native build command or log identity is missing or changed")
    return {"path": str(path), **report}
