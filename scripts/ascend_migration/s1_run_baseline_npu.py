#!/usr/bin/env python3
"""Run S1's existing numerical regressions on one verified A2/A3 device."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import traceback

from _common import ROOT, base_report, finish, run_process


CASES = (
    "vector_abs",
    "gemm_c128",
    "cv_workspace",
    "persistent_full",
    "persistent_partial",
    "persistent_multi",
    "l1_tail_stages2",
    "l1_tail_parallel",
    "alias_trip0",
    "alias_trip1",
    "alias_trip3",
    "copy_elem_offset",
    "atomic_elem_offset",
    "tile_fill_elem_offset",
)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--platform", required=True, choices=("a2", "a3"))
    result.add_argument("--device", type=int, default=0, help="Logical NPU device index")
    result.add_argument("--seed", type=int, default=0)
    result.add_argument("--repeat", type=int, default=100)
    result.add_argument("--timeout", type=float, default=120, help="Seconds per isolated case")
    result.add_argument(
        "--output", required=True, help="JSON result; artifacts are saved beside it"
    )
    result.add_argument("--_case", choices=("preflight", *CASES), help=argparse.SUPPRESS)
    return result


def digest(path: str | Path) -> str:
    sha = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def toolchain_identity() -> dict:
    selected = shutil.which("bisheng")
    if selected is None:
        raise RuntimeError("bisheng is not available on PATH")
    resolved = str(Path(selected).resolve())
    version = subprocess.check_output([selected, "--version"], text=True, timeout=10)
    resource = subprocess.check_output(
        [selected, "-print-resource-dir"], text=True, timeout=10
    ).strip()
    ascend_home = os.environ.get("ASCEND_HOME_PATH") or os.environ.get("ASCEND_HOME")
    if not ascend_home:
        raise RuntimeError("ASCEND_HOME_PATH or ASCEND_HOME must identify CANN")
    version_file = Path(ascend_home) / "opp/version.info"
    return {
        "selected_path": selected,
        "realpath": resolved,
        "sha256": digest(resolved),
        "version": version,
        "resource_directory": resource,
        "cann_home": str(Path(ascend_home).resolve()),
        "cann_version_info": version_file.read_text(),
    }


def prepare_worker(args, evidence: dict) -> tuple:
    """Import the device runtime only inside an isolated worker, after parsing."""
    from _native_build import verify_manifest

    evidence["run_mode"] = os.environ.get("TL_RUN_MODE", "npu")
    if evidence["run_mode"] != "npu":
        raise RuntimeError("Real-device acceptance requires TL_RUN_MODE=npu")
    manifest = os.environ.get(
        "TL_ASCEND_MIGRATION_BUILD_MANIFEST",
        str(ROOT / "build/ascend_migration_native_build.json"),
    )
    evidence["native_build"] = verify_manifest(manifest)
    libraries = {
        Path(item["path"]).name: Path(item["path"])
        for item in evidence["native_build"]["libraries"]
    }
    os.environ["TVM_LIBRARY_PATH"] = str(libraries["libtvm.so"].parent)
    os.environ["TILELANG_LIBRARY_PATH"] = str(libraries["libtilelang_module.so"].parent)
    tvm_python = Path(evidence["native_build"]["tvm_source_root"]) / "python"
    os.environ["TVM_IMPORT_PYTHON_PATH"] = str(tvm_python)
    os.environ["PYTHONPATH"] = str(tvm_python) + os.pathsep + os.environ.get("PYTHONPATH", "")
    sys.path.insert(0, str(ROOT))
    sys.path.insert(0, str(ROOT / "testing/python/language"))
    sys.path.insert(0, str(tvm_python))
    os.environ["ACL_OP_INIT_MODE"] = "1"
    import acl
    import torch
    import torch_npu  # noqa: F401
    import tilelang
    from tilelang import tvm

    if Path(tilelang.__file__).resolve() != (ROOT / "tilelang/__init__.py").resolve():
        raise RuntimeError(f"TileLang Python source is outside the repository: {tilelang.__file__}")
    if Path(tvm.__file__).resolve() != (tvm_python / "tvm/__init__.py").resolve():
        raise RuntimeError(f"TVM Python source does not match the verified build: {tvm.__file__}")
    evidence["loaded_tvm_python"] = tvm.__file__
    loaded = {
        str(Path(tilelang._LIB_PATH).resolve()),
        str(Path(tvm._ffi.base._LIB._name).resolve()),
    }
    expected = {item["path"] for item in evidence["native_build"]["libraries"]}
    if not loaded.issubset(expected):
        raise RuntimeError(f"Loaded native libraries do not match the verified build: {loaded}")
    evidence["loaded_libraries"] = sorted(loaded)
    evidence["versions"] = {
        "torch": torch.__version__,
        "torch_npu": torch_npu.__version__,
        "tvm": tvm.__version__,
        "tvm_git": tvm.support.libinfo().get("GIT_COMMIT_HASH"),
    }
    if not torch.npu.is_available():
        raise RuntimeError("The NPU runtime reports no available devices")
    count = torch.npu.device_count()
    if args.device >= count:
        raise RuntimeError(f"Logical device {args.device} is unavailable; device_count={count}")
    torch.npu.set_device(args.device)
    name = acl.get_soc_name()
    upper = name.upper()
    actual = "a2" if "910B" in upper else "a3" if "910_93" in upper or "910C" in upper else None
    evidence["actual_device"] = {
        "soc": name,
        "platform": actual,
        "logical_device": args.device,
        "name": torch.npu.get_device_name(args.device),
        "device_count": count,
    }
    if actual != args.platform:
        raise RuntimeError(
            f"Requested {args.platform}, but actual SoC {name!r} identifies {actual!r}"
        )
    evidence["inherited_platform_override"] = os.environ.get("TL_PLATFORM")
    os.environ["TL_PLATFORM"] = actual.upper()
    tilelang.disable_cache()
    torch.manual_seed(args.seed)
    torch.npu.manual_seed_all(args.seed)
    evidence["toolchain"] = toolchain_identity()
    from tilelang.jit.adapter.cython import adapter

    evidence["cache"] = {
        "kernel_memory_and_disk": "disabled via tilelang.disable_cache()",
        "kernel_directory": os.environ["TILELANG_CACHE_DIR"],
        "jit_object": "fresh process and directly compiled kernels",
        "autotuner": "not used",
        "cython_host_wrapper": {
            "directory": str(adapter.get_cache_dir()),
            "source_sha256": digest(adapter.cython_wrapper_path),
            "library_sha256": digest(adapter.library_path),
            "mode": "independent host wrapper cache; not NPU kernel compilation evidence",
        },
    }
    return tilelang, torch


def run_case(name: str, args, torch) -> None:
    """Reuse existing kernels and their golden assertions, including every launch."""
    if name == "vector_abs":
        from test_tilelang_ascend_language_elementwise import run_test_abs

        run_test_abs(256, 256, 128, 256, "float", "ascendc", repeat=args.repeat)
    elif name == "gemm_c128":
        from test_tilelang_ascend_language_gemm_v0 import _run_precision_case

        _run_precision_case("ascendc", 128, "C", repeat=args.repeat)
    elif name == "cv_workspace":
        from test_tilelang_ascend_language_copy_cv import _ub_to_l1_case, _ub_to_l1_kernel

        _ub_to_l1_case(_ub_to_l1_kernel, target="ascendc", repeat=args.repeat)
    elif name.startswith("persistent_"):
        from test_tilelang_ascend_language_l1_to_l0 import run_test_persistent_gemm

        tiles, cores = {
            "persistent_full": (8, 8),
            "persistent_partial": (4, 8),
            "persistent_multi": (9, 4),
        }[name]
        run_test_persistent_gemm(
            tiles * 128,
            128,
            128,
            128,
            128,
            64,
            cores,
            "float16",
            "float",
            "ascendc",
            repeat=args.repeat,
            seed=args.seed,
            launch_core_num=cores,
        )
    elif name == "l1_tail_stages2":
        from test_tilelang_ascend_language_gemm_v0_k_tail import test_gemm_v0_ktail_pipeline

        test_gemm_v0_ktail_pipeline(200, 2, repeat=args.repeat, seed=args.seed)
    elif name == "l1_tail_parallel":
        from test_tilelang_ascend_language_gemm_v0_k_tail import (
            test_gemm_v0_ktail_pipeline_parallel,
        )

        test_gemm_v0_ktail_pipeline_parallel(200, repeat=args.repeat, seed=args.seed)
    elif name.startswith("alias_trip"):
        from test_ascend_sync_insert_zero_trip import (
            _optional_loop_kernel,
            test_optional_loop_preserves_the_previous_alias_value,
        )

        kernel = _optional_loop_kernel(args.platform.upper())
        test_optional_loop_preserves_the_previous_alias_value(
            kernel, int(name[-1]), repeat=args.repeat
        )
    elif name == "copy_elem_offset":
        import tilelang
        from ascend_repeat import repeat_launch
        from test_ascend_migration_baseline import make_copy_offset_kernel

        program = make_copy_offset_kernel(src_start=8, dst_start=8)
        kernel = tilelang.compile(
            program,
            out_idx=[],
            target="ascendc",
            platform=args.platform.upper(),
            pass_configs={
                tilelang.PassConfigKey.TL_ASCEND_AUTO_SYNC: True,
                tilelang.PassConfigKey.TL_ASCEND_MEMORY_PLANNING: True,
            },
        )
        source = torch.arange(128, dtype=torch.float32, device="npu")
        output = torch.empty_like(source)
        expected = torch.full((128,), float("nan"), dtype=torch.float32)
        expected[24:88] = source.cpu()[40:104]
        repeat_launch(
            kernel,
            (source, output),
            (output,),
            lambda: torch.testing.assert_close(
                output.cpu(), expected, rtol=0, atol=0, equal_nan=True
            ),
            args.repeat,
        )
    elif name == "atomic_elem_offset":
        import tilelang
        from ascend_repeat import repeat_launch
        from test_ascend_migration_baseline import make_atomic_offset_kernel

        kernel = tilelang.compile(
            make_atomic_offset_kernel(),
            out_idx=[],
            target="ascendc",
            platform=args.platform.upper(),
            pass_configs={
                tilelang.PassConfigKey.TL_ASCEND_AUTO_SYNC: True,
                tilelang.PassConfigKey.TL_ASCEND_MEMORY_PLANNING: True,
            },
        )
        source = torch.arange(128, dtype=torch.float32, device="npu")
        output = torch.empty_like(source)
        expected = torch.full((128,), -99.0, dtype=torch.float32)
        expected[16:80] = 7.0 + 2 * source.cpu()[16:80]

        def reset_atomic_output():
            output.fill_(-99.0)
            output[16:80].fill_(7.0)

        repeat_launch(
            kernel,
            (source, output),
            (output,),
            lambda: torch.testing.assert_close(output.cpu(), expected, rtol=0, atol=0),
            args.repeat,
            reset=reset_atomic_output,
        )
    elif name == "tile_fill_elem_offset":
        import tilelang
        from ascend_repeat import repeat_launch
        from test_ascend_migration_baseline import make_tile_fill_offset_kernel

        kernel = tilelang.compile(
            make_tile_fill_offset_kernel(region_start=8),
            out_idx=[],
            target="ascendc",
            platform=args.platform.upper(),
            pass_configs={
                tilelang.PassConfigKey.TL_ASCEND_AUTO_SYNC: True,
                tilelang.PassConfigKey.TL_ASCEND_MEMORY_PLANNING: True,
            },
        )
        output = torch.empty((128,), dtype=torch.float32, device="npu")
        expected = torch.full((128,), -99.0, dtype=torch.float32)
        expected[24:88] = 1.0
        repeat_launch(
            kernel,
            (output,),
            (output,),
            lambda: torch.testing.assert_close(output.cpu(), expected, rtol=0, atol=0),
            args.repeat,
        )


def worker(args) -> int:
    evidence = {
        "case": args._case,
        "status": "BLOCKED",
        "compilations": [],
        "kernels": [],
        "launches": {},
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)

    def save():
        with tempfile.NamedTemporaryFile(mode="w", dir=output.parent, delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(evidence, stream, indent=2, default=str)
            stream.write("\n")
        temporary.replace(output)

    save()
    try:
        tilelang, torch = prepare_worker(args, evidence)
        save()
    except Exception as exc:
        evidence.update(error=str(exc), traceback=traceback.format_exc())
        save()
        return 2
    if args._case == "preflight":
        evidence["status"] = "PASS"
        save()
        return 0

    from tilelang.jit.adapter import libgen
    from tilelang.jit.kernel import JITKernel

    real_run = subprocess.run
    real_compile = tilelang.compile
    real_call = JITKernel.__call__

    def observed_run(command, *pargs, **kwargs):
        is_bisheng = bool(command) and Path(command[0]).name == "bisheng"
        record = None
        if is_bisheng:
            record = {"argv": list(map(str, command)), "status": "RUNNING"}
            record["compiler_realpath"] = str(Path(shutil.which(command[0])).resolve())
            record["source_sha256"] = [
                {"path": str(part), "sha256": digest(part)}
                for part in command
                if str(part).endswith(".cpp") and Path(part).is_file()
            ]
            evidence["compilations"].append(record)
            save()
        started = time.monotonic()
        try:
            result = real_run(command, *pargs, **kwargs)
            if record is not None:
                record.update(
                    returncode=result.returncode,
                    status="PASS" if result.returncode == 0 else "FAIL",
                )
                if result.returncode == 0 and "-o" in command:
                    artifact = command[command.index("-o") + 1]
                    record["artifact"] = {"path": artifact, "sha256": digest(artifact)}
            return result
        except Exception as exc:
            if record is not None:
                record.update(status="FAIL", error=str(exc))
            raise
        finally:
            if record is not None:
                record["elapsed_seconds"] = time.monotonic() - started
                save()

    def observed_compile(program, *pargs, **kwargs):
        kwargs["platform"] = args.platform.upper()
        record = {
            "target": kwargs.get("target", "auto"),
            "platform": args.platform.upper(),
            "pass_configs": {str(k): v for k, v in (kwargs.get("pass_configs") or {}).items()},
        }
        evidence["kernels"].append(record)
        kernel = real_compile(program, *pargs, **kwargs)
        record["resolved_flags"] = kernel.compile_flags
        record["resolved_pass_configs"] = dict(kernel.pass_configs)
        # These audited legacy controls have C++ defaults, rather than being
        # materialized by Python's process_default_pass_config().
        controls = {
            "tl.ascend_auto_cv_combine": True,
            "tl.ascend_auto_sync": False,
            "tl.ascend_auto_cross_core_sync": False,
            "tl.ascend_memory_planning": False,
            "tl.ascend_auto_sync_vs": False,
        }
        record["effective_legacy_controls"] = {
            key: kernel.pass_configs.get(key, default) for key, default in controls.items()
        }
        record["kernel_id"] = str(id(kernel))
        evidence["launches"][str(id(kernel))] = {"attempted": 0, "completed": 0}
        save()
        return kernel

    def observed_call(kernel, *pargs, **kwargs):
        counts = evidence["launches"][str(id(kernel))]
        counts["attempted"] += 1
        result = real_call(kernel, *pargs, **kwargs)
        counts["completed"] += 1
        return result

    libgen.subprocess.run = observed_run
    tilelang.compile = observed_compile
    JITKernel.__call__ = observed_call
    try:
        run_case(args._case, args, torch)
        if len(evidence["compilations"]) != 1:
            raise AssertionError("Each case must invoke real Bisheng exactly once")
        if len(evidence["launches"]) != 1 or any(
            count["completed"] != args.repeat for count in evidence["launches"].values()
        ):
            raise AssertionError("Each case must launch the same compiled kernel repeat times")
        evidence["status"] = "PASS"
        evidence["golden_checks"] = args.repeat
        evidence["output_sentinel"] = "NaN for floating point, -99 for integer, reset each launch"
        if args._case == "atomic_elem_offset":
            evidence["output_sentinel"] = (
                "-99 outside atomic destination; destination reset to 7 each launch"
            )
        save()
        return 0
    except Exception as exc:
        evidence.update(status="FAIL", error=str(exc), traceback=traceback.format_exc())
        save()
        return 1
    finally:
        libgen.subprocess.run = real_run
        tilelang.compile = real_compile
        JITKernel.__call__ = real_call


def main() -> int:
    args = parser().parse_args()
    report = base_report("s1_run_baseline_npu", args)
    if sys.flags.optimize:
        return finish(args, report, "BLOCKED", "Python optimization disables mandatory assertions")
    if (
        args.device < 0
        or args.seed < 0
        or args.repeat < 1
        or not math.isfinite(args.timeout)
        or args.timeout <= 0
    ):
        return finish(
            args,
            report,
            "BLOCKED",
            "device/seed must be nonnegative; repeat/timeout must be positive",
        )
    if args._case:
        return worker(args)
    # Keep previous failures and their logs intact when rerunning an output path.
    destination = Path(args.output).resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    artifacts = Path(
        tempfile.mkdtemp(prefix=destination.stem + "-artifacts-", dir=destination.parent)
    )
    report["artifacts"] = str(artifacts)
    report["required_cases"] = list(CASES)
    report["cases"] = []
    report["counts"]["total"] = len(CASES)
    report["timeout_policy"] = (
        "separate process group per case; stop the group on first failure or timeout"
    )
    for name in ("preflight", *CASES):
        case_output = artifacts / f"{name}.json"
        case_output.unlink(missing_ok=True)
        env = dict(os.environ)
        env["TILELANG_CACHE_DIR"] = tempfile.mkdtemp(prefix=f"{name}-cache-", dir=artifacts)
        env.pop("TILELANG_CLEAR_CACHE", None)
        command = [
            sys.executable,
            str(Path(__file__).resolve()),
            "--platform",
            args.platform,
            "--device",
            str(args.device),
            "--seed",
            str(args.seed),
            "--repeat",
            str(args.repeat),
            "--timeout",
            str(args.timeout),
            "--output",
            str(case_output),
            "--_case",
            name,
        ]
        try:
            result = run_process(command, artifacts, name, env=env, timeout=args.timeout)
        except OSError as exc:
            result = {"command": command, "exit_code": 2, "status": "BLOCKED", "error": str(exc)}
        if case_output.is_file():
            try:
                result["evidence"] = json.loads(case_output.read_text())
            except (OSError, json.JSONDecodeError) as exc:
                result["evidence_error"] = str(exc)
        evidence = result.get("evidence", {})
        if name == "preflight":
            report["preflight"] = result
        else:
            report["cases"].append({"name": name, **result})
        if result["exit_code"] or evidence.get("status") != "PASS":
            if name != "preflight":
                report["counts"]["failed"] += 1
            report["not_run"] = [
                item for item in CASES if item not in {case["name"] for case in report["cases"]}
            ]
            status = (
                "TIMEOUT"
                if result["exit_code"] == 124
                else "BLOCKED"
                if result["exit_code"] == 2
                else "FAIL"
            )
            error = (
                evidence.get("error")
                or result.get("error")
                or (f"{name} did not pass; see {result.get('log', 'process launch error')}")
            )
            if status == "TIMEOUT":
                error = f"{name} exceeded {args.timeout}s; its entire process group was stopped"
            return finish(
                args,
                report,
                status,
                error,
            )
        if name != "preflight":
            report["counts"]["passed"] += 1
    return finish(args, report, "PASS")


if __name__ == "__main__":
    raise SystemExit(main())
