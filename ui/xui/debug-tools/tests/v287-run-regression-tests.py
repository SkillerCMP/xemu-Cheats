#!/usr/bin/env python3
# v2.87 current regression ownership.
"""Behavior-preserving regression checks for xemu Debug Tools.

The suite intentionally avoids a full xemu build.  It provides three independently
runnable phases so fast source guards, host-native parser/assembler coverage, and
heavy randomized stress tests can be validated separately without weakening the
default full run.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import fnmatch
import os
import pathlib
import shutil
import shlex
import subprocess
import sys
import tempfile
import time


PHASES = ("static", "native", "heavy")
HEAVY_PYTHON_TESTS = {
    "v287-allocator-golden.py",
    "v287-search-compare-golden.py",
    "v287-memory-format-golden.py",
}
HEAVY_MODEL_TESTS = {
    "v287-debugger-streamlining-golden.py",
    "v287-f0-steady-state-golden.py",
}

# Legacy project-layout sentinels. Static tests are discovered automatically;
# these names remain literal so the older ownership validator can verify that
# historically critical guards have not disappeared from the packaged suite.
REQUIRED_DISCOVERY_SENTINELS = (
    "v287-f0-steady-state-golden.py",
    "v287-memory-tools-structural-refactor-golden.py",
    "v287-audit-pruning-cleanup-golden.py",
)


def run(cmd: list[str], cwd: pathlib.Path) -> None:
    print("+", " ".join(cmd), flush=True)
    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    started = time.perf_counter()
    subprocess.run(cmd, cwd=cwd, check=True, env=env)
    print(f"  -> {time.perf_counter() - started:.3f}s", flush=True)


def syntax_check_python(path: pathlib.Path) -> None:
    """Compile Python source in memory so validation never creates __pycache__."""
    source = path.read_bytes()
    compile(source, str(path), "exec")


def selected_phases(values: list[str]) -> tuple[str, ...]:
    if not values or "all" in values:
        return PHASES
    requested = set(values)
    return tuple(phase for phase in PHASES if phase in requested)


def discover_compilers(requested: list[str]) -> list[str]:
    names = requested or ["g++", "clang++"]
    compilers: list[str] = []
    for name in names:
        compiler = shutil.which(name)
        if compiler is None and pathlib.Path(name).is_file():
            compiler = str(pathlib.Path(name).resolve())
        if compiler and compiler not in compilers:
            compilers.append(compiler)
    return compilers


def test_selected(name: str, patterns: list[str]) -> bool:
    if not patterns:
        return True
    stem = pathlib.Path(name).stem
    return any(fnmatch.fnmatch(name, pattern) or fnmatch.fnmatch(stem, pattern)
               for pattern in patterns)


def run_static_phase(root: pathlib.Path, debug_tools: pathlib.Path,
                     tests: pathlib.Path, patterns: list[str], jobs: int) -> None:
    run([sys.executable, str(debug_tools / "validate-project-layout.py"),
         "--root", str(root)], root)

    # Syntax-check every packaged Python helper/test automatically.  This avoids
    # the old manual list getting stale when a new golden test is added.
    python_helpers = sorted(debug_tools.glob("*.py")) + sorted(tests.glob("*.py"))
    for helper in python_helpers:
        syntax_check_python(helper)

    # Syntax-check source-owned dependency helpers without downloading/building.
    bash = shutil.which("bash")
    if bash:
        run([bash, "-n", str(debug_tools / "build-capstone.sh")], root)
        run([bash, "-n", str(debug_tools / "build-capstone-windows.sh")], root)
        run([bash, "-n", str(debug_tools / "build-keystone.sh")], root)
        run([bash, "-n", str(debug_tools / "prepare-build-dependencies.sh")], root)
        run([bash, "-n", str(debug_tools / "build-xemu.sh")], root)
        run([bash, "-n", str(debug_tools / "docker-build-windows.sh")], root)

    # All Python goldens without a compiler parameter are source/model guards.
    # The three compiler-driven randomized tests are reserved for the heavy phase.
    static_tests = [
        test for test in sorted(tests.glob("*-golden.py"))
        if test.name not in HEAVY_PYTHON_TESTS and test_selected(test.name, patterns)
    ]
    if jobs <= 1:
        for test in static_tests:
            run([sys.executable, str(test), "--root", str(root)], root)
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            futures = [
                pool.submit(run, [sys.executable, str(test), "--root", str(root)], root)
                for test in static_tests
            ]
            for future in futures:
                future.result()


def compile_and_run_native(compiler: str, root: pathlib.Path,
                           debug_tools: pathlib.Path, tests: pathlib.Path,
                           tmp: pathlib.Path, compiler_index: int,
                           patterns: list[str]) -> None:
    common_flags = ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror"]
    include_debug_tools = ["-I", str(debug_tools)]

    # xbe-labels.cc is linked by six native tests. Compile it once per compiler
    # instead of recompiling the identical source for every executable.
    xbe_labels_obj = tmp / f"xbe-labels-{compiler_index}.o"
    fatx_hdd_obj = tmp / f"fatx-hdd-{compiler_index}.o"
    kernel_rpc_fs_obj = tmp / f"kernel-rpc-filesystem-{compiler_index}.o"
    kernel_rpc_fs_stream_obj = tmp / f"kernel-rpc-filesystem-stream-{compiler_index}.o"

    # Keystone is source-bootstrapped by the Debug Tools build wrapper for real
    # xemu builds. The regression runner intentionally runs before that bootstrap in the legacy
    # local Builder v1.04 path, so compile the assembler golden when a native
    # Keystone development package is already visible and otherwise skip only
    # that one native executable. Static/source guards still validate the F0
    # ownership and the real xemu build will bootstrap and link Keystone.
    keystone_cflags: list[str] = []
    keystone_libs: list[str] = []
    pkg_config = shutil.which("pkg-config")
    if pkg_config:
        cflags_probe = subprocess.run(
            [pkg_config, "--cflags", "keystone"], cwd=root, env=os.environ.copy(),
            text=True, capture_output=True,
        )
        libs_probe = subprocess.run(
            [pkg_config, "--libs", "--static", "keystone"],
            cwd=root, env=os.environ.copy(), text=True, capture_output=True,
        )
        if cflags_probe.returncode == 0 and libs_probe.returncode == 0:
            keystone_cflags = shlex.split(cflags_probe.stdout)
            keystone_libs = shlex.split(libs_probe.stdout)

    native_tests: list[tuple[str, list[pathlib.Path], list[str]]] = [
        (
            "guest-pause-guard-golden",
            [tests / "guest-pause-guard-golden.cpp",
             debug_tools / "guest-pause-guard.cc"],
            ["-I", str(tests / "stubs"), *include_debug_tools],
        ),
        (
            "guest-pause-fstream-order-golden",
            [tests / "guest-pause-fstream-order-golden.cpp"],
            ["-I", str(tests / "stubs"), *include_debug_tools],
        ),
        (
            "tab-style-stack-golden",
            [tests / "tab-style-stack-golden.cpp"],
            ["-I", str(tests / "tab-style-stubs"), *include_debug_tools],
        ),
        (
            "register-copy-golden",
            [tests / "register-copy-golden.cpp"],
            include_debug_tools,
        ),
        (
            "kernel-rpc-golden",
            [tests / "kernel-rpc-golden.cpp"],
            include_debug_tools,
        ),
        (
            "kernel-rpc-filesystem-golden",
            [tests / "kernel-rpc-filesystem-golden.cpp",
             kernel_rpc_fs_obj, kernel_rpc_fs_stream_obj, fatx_hdd_obj],
            include_debug_tools,
        ),
        (
            "windows-transferkind-macro-golden",
            [tests / "windows-transferkind-macro-golden.cpp"],
            include_debug_tools,
        ),
        (
            "filesystem-transfer-contract-golden",
            [tests / "filesystem-transfer-contract-golden.cpp",
             kernel_rpc_fs_obj, kernel_rpc_fs_stream_obj, fatx_hdd_obj],
            include_debug_tools,
        ),
        (
            "fatx-hdd-golden",
            [tests / "fatx-hdd-golden.cpp", fatx_hdd_obj],
            include_debug_tools,
        ),
        (
            "label-symbol-utils-golden",
            [tests / "label-symbol-utils-golden.cpp"],
            include_debug_tools,
        ),
        (
            "conditions-golden",
            [tests / "conditions-golden.cpp", debug_tools / "addons/memory-tools/breakpoint-conditions.cc"],
            include_debug_tools,
        ),
        (
            "xdvdfs-golden",
            [tests / "xdvdfs-golden.cpp", debug_tools / "xdvdfs-disc.cc"],
            include_debug_tools,
        ),
        (
            "xbe-labels-golden",
            [tests / "xbe-labels-golden.cpp", xbe_labels_obj],
            include_debug_tools,
        ),
        (
            "label-packs-golden",
            [tests / "label-packs-golden.cpp", debug_tools / "label-packs.cc", xbe_labels_obj],
            include_debug_tools,
        ),
        (
            "label-batch-golden",
            [tests / "label-batch-golden.cpp", xbe_labels_obj],
            include_debug_tools,
        ),
        (
            "xdk-labels-golden",
            [tests / "xdk-labels-golden.cpp", debug_tools / "xdk-labels.cc", xbe_labels_obj],
            include_debug_tools,
        ),
        (
            "map-labels-golden",
            [tests / "map-labels-golden.cpp", debug_tools / "map-labels.cc", xbe_labels_obj],
            include_debug_tools,
        ),
        (
            "pdb-labels-golden",
            [tests / "pdb-labels-golden.cpp", debug_tools / "pdb-labels.cc", xbe_labels_obj],
            include_debug_tools,
        ),
    ]
    if keystone_libs:
        native_tests.append((
            "assembler-golden",
            [tests / "assembler-golden.cpp", debug_tools / "x86-cheat-assembler.cc",
             debug_tools / "x86-cheat-assembler-keystone.cc"],
            [*include_debug_tools, *keystone_cflags],
        ))
    elif test_selected("assembler-golden", patterns):
        print("SKIP assembler-golden: native Keystone pkg-config package is not available; "
              "the normal xemu build bootstraps Keystone before Meson configuration.",
              flush=True)

    selected = [entry for entry in native_tests if test_selected(entry[0], patterns)]
    if any(fatx_hdd_obj in sources for _, sources, _ in selected):
        run([compiler, *common_flags, *include_debug_tools, "-c",
             str(debug_tools / "addons/hdd/fatx-hdd.cc"), "-o", str(fatx_hdd_obj)], root)
    if any(kernel_rpc_fs_obj in sources for _, sources, _ in selected):
        run([compiler, *common_flags, *include_debug_tools, "-c",
             str(debug_tools / "addons/hdd/kernel-rpc-filesystem.cc"),
             "-o", str(kernel_rpc_fs_obj)], root)
    if any(kernel_rpc_fs_stream_obj in sources for _, sources, _ in selected):
        run([compiler, *common_flags, *include_debug_tools, "-c",
             str(debug_tools / "addons/hdd/kernel-rpc-filesystem-stream.cc"),
             "-o", str(kernel_rpc_fs_stream_obj)], root)
    if any(xbe_labels_obj in sources for _, sources, _ in selected):
        run([
            compiler, *common_flags, *include_debug_tools,
            "-c", str(debug_tools / "xbe-labels.cc"),
            "-o", str(xbe_labels_obj),
        ], root)

    for name, sources, include_flags in selected:
        exe = tmp / f"{name}-{compiler_index}"
        link_flags = keystone_libs if name == "assembler-golden" else []
        run([
            compiler, *common_flags, *include_flags,
            *(str(source) for source in sources),
            *link_flags, "-o", str(exe),
        ], root)
        run([str(exe)], root)


def run_native_phase(compilers: list[str], root: pathlib.Path,
                     debug_tools: pathlib.Path, tests: pathlib.Path,
                     patterns: list[str]) -> None:
    with tempfile.TemporaryDirectory(prefix="xemu-debug-tools-native-") as tmp_name:
        tmp = pathlib.Path(tmp_name)
        for index, compiler in enumerate(compilers):
            compile_and_run_native(compiler, root, debug_tools, tests, tmp, index, patterns)


def run_heavy_phase(compilers: list[str], root: pathlib.Path,
                    tests: pathlib.Path, patterns: list[str]) -> None:
    for name in sorted(HEAVY_MODEL_TESTS):
        if test_selected(name, patterns):
            run([sys.executable, str(tests / name),
                 "--root", str(root), "--heavy"], root)
    for compiler in compilers:
        for name in sorted(HEAVY_PYTHON_TESTS):
            if not test_selected(name, patterns):
                continue
            run([sys.executable, str(tests / name),
                 "--root", str(root), "--compiler", compiler], root)


def main() -> int:
    sys.dont_write_bytecode = True

    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".", help="xemu source root")
    parser.add_argument(
        "--phase", action="append", choices=("all", *PHASES), default=[],
        help="run only the selected phase; repeat to combine phases (default: all)",
    )
    parser.add_argument(
        "--compiler", action="append", default=[],
        help="compiler name/path for native/heavy phases; repeat for more than one "
             "(default: g++ and clang++ when available)",
    )
    parser.add_argument(
        "--test", action="append", default=[],
        help="run only matching golden test name/stem (shell-style glob; repeatable)",
    )
    parser.add_argument(
        "--jobs", type=int, default=1,
        help="parallel jobs for independent static golden tests (default: 1)",
    )
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")

    root = pathlib.Path(args.root).resolve()
    debug_tools = root / "ui/xui/debug-tools"
    tests = debug_tools / "tests"
    phases = selected_phases(args.phase)

    compilers: list[str] = []
    if "native" in phases or "heavy" in phases:
        compilers = discover_compilers(args.compiler)
        if not compilers:
            print("ERROR: native/heavy regression phases require g++ or clang++",
                  file=sys.stderr)
            return 2

    started = time.perf_counter()
    for phase in phases:
        phase_started = time.perf_counter()
        print(f"=== Debug Tools regression phase: {phase} ===", flush=True)
        if phase == "static":
            run_static_phase(root, debug_tools, tests, args.test, args.jobs)
        elif phase == "native":
            run_native_phase(compilers, root, debug_tools, tests, args.test)
        elif phase == "heavy":
            run_heavy_phase(compilers, root, tests, args.test)
        print(f"PASS: {phase} phase ({time.perf_counter() - phase_started:.2f}s)",
              flush=True)

    print(f"PASS: Debug Tools regression suite ({time.perf_counter() - started:.2f}s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
