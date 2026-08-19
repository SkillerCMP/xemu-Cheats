#!/usr/bin/env python3
"""Behavior-preserving regression checks for xemu Debug Tools.

The suite intentionally avoids a full xemu build.  It provides three independently
runnable phases so fast source guards, host-native parser/assembler coverage, and
heavy randomized stress tests can be validated separately without weakening the
default full run.
"""
from __future__ import annotations

import argparse
import fnmatch
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time


PHASES = ("static", "native", "heavy")
HEAVY_PYTHON_TESTS = {
    "allocator-golden.py",
    "search-compare-golden.py",
    "memory-format-golden.py",
}

# Legacy project-layout sentinels. Static tests are discovered automatically;
# these names remain literal so the older ownership validator can verify that
# historically critical guards have not disappeared from the packaged suite.
REQUIRED_DISCOVERY_SENTINELS = (
    "f0-steady-state-golden.py",
    "pass3-structural-refactor-golden.py",
    "pass9-audit-cleanup-golden.py",
)


def run(cmd: list[str], cwd: pathlib.Path) -> None:
    print("+", " ".join(cmd), flush=True)
    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    subprocess.run(cmd, cwd=cwd, check=True, env=env)


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
                     tests: pathlib.Path, patterns: list[str]) -> None:
    run([sys.executable, str(debug_tools / "validate-project-layout.py"),
         "--root", str(root)], root)

    # Syntax-check every packaged Python helper/test automatically.  This avoids
    # the old manual list getting stale when a new golden test is added.
    python_helpers = sorted(debug_tools.glob("*.py")) + sorted(tests.glob("*.py"))
    for helper in python_helpers:
        syntax_check_python(helper)

    # Syntax-check the Capstone helper without downloading or building anything.
    bash = shutil.which("bash")
    if bash:
        run([bash, "-n", str(debug_tools / "build-capstone-windows.sh")], root)

    # All Python goldens without a compiler parameter are source/model guards.
    # The three compiler-driven randomized tests are reserved for the heavy phase.
    for test in sorted(tests.glob("*-golden.py")):
        if test.name in HEAVY_PYTHON_TESTS or not test_selected(test.name, patterns):
            continue
        run([sys.executable, str(test), "--root", str(root)], root)


def compile_and_run_native(compiler: str, root: pathlib.Path,
                           debug_tools: pathlib.Path, tests: pathlib.Path,
                           tmp: pathlib.Path, compiler_index: int,
                           patterns: list[str]) -> None:
    common_flags = ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror"]
    include_debug_tools = ["-I", str(debug_tools)]

    # xbe-labels.cc is linked by six native tests. Compile it once per compiler
    # instead of recompiling the identical source for every executable.
    xbe_labels_obj = tmp / f"xbe-labels-{compiler_index}.o"

    native_tests: list[tuple[str, list[pathlib.Path], list[str]]] = [
        (
            "guest-pause-guard-golden",
            [tests / "guest-pause-guard-golden.cpp"],
            ["-I", str(tests / "stubs"), *include_debug_tools],
        ),
        (
            "register-copy-golden",
            [tests / "register-copy-golden.cpp"],
            include_debug_tools,
        ),
        (
            "fatx-hdd-golden",
            [tests / "fatx-hdd-golden.cpp", debug_tools / "fatx-hdd.cc"],
            include_debug_tools,
        ),
        (
            "assembler-golden",
            [tests / "assembler-golden.cpp", debug_tools / "x86-cheat-assembler.cc"],
            include_debug_tools,
        ),
        (
            "label-symbol-utils-golden",
            [tests / "label-symbol-utils-golden.cpp"],
            include_debug_tools,
        ),
        (
            "conditions-golden",
            [tests / "conditions-golden.cpp", debug_tools / "breakpoint-conditions.cc"],
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

    selected = [entry for entry in native_tests if test_selected(entry[0], patterns)]
    if any(xbe_labels_obj in sources for _, sources, _ in selected):
        run([
            compiler, *common_flags, *include_debug_tools,
            "-c", str(debug_tools / "xbe-labels.cc"),
            "-o", str(xbe_labels_obj),
        ], root)

    for name, sources, include_flags in selected:
        exe = tmp / f"{name}-{compiler_index}"
        run([
            compiler, *common_flags, *include_flags,
            *(str(source) for source in sources),
            "-o", str(exe),
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
    args = parser.parse_args()

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
            run_static_phase(root, debug_tools, tests, args.test)
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
