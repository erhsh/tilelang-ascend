# Copyright (c) Tile-AI Organization.
# Licensed under the MIT License.
"""Copy optional native dependencies without importing the TileLang runtime."""

from __future__ import annotations

import re
import shutil
from collections.abc import Iterable
from pathlib import Path


NATIVE_RUNTIME_MANIFEST = "ascend_sync_z3_runtime_files.txt"
_Z3_LIBRARY_NAME = re.compile(r"libz3\.so(?:\.[0-9]+)*\Z")


def copy_native_notices(source_root: str | Path, package_dirs: Iterable[str | Path]) -> list[Path]:
    """Include the notices for the compiler and bundled native dependencies."""
    source_root = Path(source_root)
    notices = (
        ("3rdparty/tvm/LICENSE", "licenses/tvm/LICENSE"),
        ("3rdparty/tvm/NOTICE", "licenses/tvm/NOTICE"),
        ("src/transform/ascend_sync/NOTICE", "licenses/ascend_sync/NOTICE"),
        ("src/transform/ascend_sync/licenses/Z3-LICENSE.txt", "licenses/z3/LICENSE.txt"),
    )
    for original, _destination in notices:
        if not (source_root / original).is_file():
            raise FileNotFoundError(f"Missing native dependency notice: {source_root / original}")
    copied = []
    seen = set()
    for package_dir in package_dirs:
        package_dir = Path(package_dir)
        for original, relative_target in notices:
            target = package_dir / relative_target
            if target.resolve() in seen:
                continue
            seen.add(target.resolve())
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source_root / original, target)
            copied.append(target)
    return copied


def copy_native_runtime_files(
    search_dirs: Iterable[str | Path],
    destination_dirs: Iterable[str | Path],
    *,
    required: bool = False,
) -> list[Path]:
    """Copy the first build manifest's files to each package library directory.

    An empty manifest means the feature is disabled and stops the search. Entries
    must be absolute paths to Z3 shared libraries, including any SONAME alias.
    Missing manifests are allowed for older builds unless ``required`` is true.
    Explicitly requiring the feature also rejects an empty OFF manifest.
    An OFF manifest removes stale Z3 files from the destination directories,
    except directories also used to search for build manifests.
    All entries are validated before copying; source files are never removed.
    The returned paths name files actually copied, excluding files already in place.
    """
    manifest = None
    searched = []
    search_paths = [Path(directory) for directory in search_dirs]
    for directory in search_paths:
        candidate = directory / NATIVE_RUNTIME_MANIFEST
        searched.append(str(candidate))
        if candidate.exists():
            manifest = candidate
            break
    if manifest is None:
        if required:
            raise FileNotFoundError(
                "Native runtime manifest not found; searched: " + ", ".join(searched)
            )
        return []
    if not manifest.is_file():
        raise ValueError(f"Native runtime manifest is not a file: {manifest}")

    sources = []
    basenames = set()
    for line_number, line in enumerate(manifest.read_text(encoding="utf-8").splitlines(), 1):
        entry = line.strip()
        if not entry:
            continue
        source = Path(entry)
        if (
            not source.is_absolute()
            or ".." in source.parts
            or _Z3_LIBRARY_NAME.fullmatch(source.name) is None
        ):
            raise ValueError(f"Invalid native runtime entry at {manifest}:{line_number}: {entry}")
        if not source.is_file():
            raise FileNotFoundError(
                f"Native runtime file listed at {manifest}:{line_number} is missing: {source}"
            )
        if source.name in basenames:
            raise ValueError(f"Duplicate native runtime basename in {manifest}: {source.name}")
        basenames.add(source.name)
        sources.append(source)

    if required and not sources:
        raise ValueError(f"Native runtime was required, but the OFF manifest is empty: {manifest}")
    if not sources:
        protected_dirs = {directory.resolve() for directory in search_paths}
        protected_dirs.add(manifest.parent.resolve())
        for directory in destination_dirs:
            destination = Path(directory)
            if destination.resolve() in protected_dirs or not destination.is_dir():
                continue
            for candidate in destination.iterdir():
                if _Z3_LIBRARY_NAME.fullmatch(candidate.name) is not None and (
                    candidate.is_file() or candidate.is_symlink()
                ):
                    candidate.unlink()
        return []

    copied = []
    destinations = set()
    for directory in destination_dirs:
        destination = Path(directory)
        for source in sources:
            target = destination / source.name
            resolved_target = target.resolve()
            if resolved_target in destinations:
                continue
            destinations.add(resolved_target)
            if source.resolve() == resolved_target:
                continue
            destination.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            copied.append(target)
    return copied
