#!/usr/bin/env python3
"""Behavior-preserving regression checks for xemu Debug Tools.

This suite intentionally avoids the full xemu build.  It compiles the real
Type-F0 assembler as a small host-native test and checks the project ownership
rules that protect our minimal upstream integration layout.
"""
from __future__ import annotations

import argparse
import pathlib
import py_compile
import shutil
import subprocess
import sys
import tempfile


def run(cmd: list[str], cwd: pathlib.Path) -> None:
    print("+", " ".join(cmd))
    subprocess.run(cmd, cwd=cwd, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".", help="xemu source root")
    args = parser.parse_args()

    root = pathlib.Path(args.root).resolve()
    debug_tools = root / "ui/xui/debug-tools"
    tests = debug_tools / "tests"

    run([sys.executable, str(debug_tools / "validate-project-layout.py"),
         "--root", str(root)], root)

    # Syntax-check the Python helpers that are part of the source package.
    for helper in (
        debug_tools / "validate-project-layout.py",
        debug_tools / "restore-executable-bits.py",
        tests / "run-regression-tests.py",
        tests / "allocator-golden.py",
        tests / "search-compare-golden.py",
        tests / "f0-steady-state-golden.py",
        tests / "memory-format-golden.py",
        tests / "inject-golden.py",
        tests / "dump-ram-golden.py",
        tests / "debugger-conditions-registers-golden.py",
        tests / "current-game-disc-golden.py",
        tests / "labels-ui-golden.py",
    ):
        py_compile.compile(str(helper), doraise=True)

    # Syntax-check the Capstone helper without downloading or building anything.
    bash = shutil.which("bash")
    if bash:
        run([bash, "-n", str(debug_tools / "build-capstone-windows.sh")], root)

    compilers = []
    for name in ("g++", "clang++"):
        compiler = shutil.which(name)
        if compiler and compiler not in compilers:
            compilers.append(compiler)
    if not compilers:
        print("ERROR: regression tests require g++ or clang++", file=sys.stderr)
        return 2

    run([sys.executable, str(tests / "f0-steady-state-golden.py"),
         "--root", str(root)], root)
    run([sys.executable, str(tests / "inject-golden.py"),
         "--root", str(root)], root)
    run([sys.executable, str(tests / "dump-ram-golden.py"),
         "--root", str(root)], root)
    run([sys.executable, str(tests / "debugger-conditions-registers-golden.py"),
         "--root", str(root)], root)
    run([sys.executable, str(tests / "current-game-disc-golden.py"),
         "--root", str(root)], root)
    run([sys.executable, str(tests / "labels-ui-golden.py"),
         "--root", str(root)], root)

    with tempfile.TemporaryDirectory(prefix="xemu-debug-tools-tests-") as tmp:
        for index, compiler in enumerate(compilers):
            exe = pathlib.Path(tmp) / f"assembler-golden-{index}"
            run([
                compiler,
                "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(debug_tools),
                str(tests / "assembler-golden.cpp"),
                str(debug_tools / "x86-cheat-assembler.cc"),
                "-o", str(exe),
            ], root)
            run([str(exe)], root)

            conditions_exe = pathlib.Path(tmp) / f"conditions-golden-{index}"
            run([
                compiler,
                "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(debug_tools),
                str(tests / "conditions-golden.cpp"),
                str(debug_tools / "breakpoint-conditions.cc"),
                "-o", str(conditions_exe),
            ], root)
            run([str(conditions_exe)], root)

            xdvdfs_exe = pathlib.Path(tmp) / f"xdvdfs-golden-{index}"
            run([
                compiler,
                "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(debug_tools),
                str(tests / "xdvdfs-golden.cpp"),
                str(debug_tools / "xdvdfs-disc.cc"),
                "-o", str(xdvdfs_exe),
            ], root)
            run([str(xdvdfs_exe)], root)

            labels_exe = pathlib.Path(tmp) / f"xbe-labels-golden-{index}"
            run([
                compiler,
                "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(debug_tools),
                str(tests / "xbe-labels-golden.cpp"),
                str(debug_tools / "xbe-labels.cc"),
                "-o", str(labels_exe),
            ], root)
            run([str(labels_exe)], root)

            run([sys.executable, str(tests / "allocator-golden.py"),
                 "--root", str(root), "--compiler", compiler], root)
            run([sys.executable, str(tests / "search-compare-golden.py"),
                 "--root", str(root), "--compiler", compiler], root)
            run([sys.executable, str(tests / "memory-format-golden.py"),
                 "--root", str(root), "--compiler", compiler], root)

    print("PASS: Debug Tools regression suite")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
