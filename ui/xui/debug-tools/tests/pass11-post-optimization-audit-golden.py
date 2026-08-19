#!/usr/bin/env python3
"""v1.96 Pass 11 guard: runtime source freeze + regression-harness cleanup."""
from __future__ import annotations

import argparse
import hashlib
import pathlib


V195_PROTECTED_NON_TEST_DEBUG_TOOLS_SHA256 = (
    "4c6a640d5460bd7bf803de20808777f5a321055feab6daeef7a46fc850aa2248"
)

LATER_SCOPED_RUNTIME_FILES = {
    "detached-tools.cc",
    "detached-tools.hh",
    "memory-tools-debugger.cc",
    "register-copy-utils.hh",
    # v1.99 detached Current Game + read-only HDD Directory feature.
    "current-game.cc",
    "current-game.hh",
    "fatx-hdd.cc",
    "fatx-hdd.hh",
    "hdd-directory.cc",
    "hdd-directory.hh",
    "meson.build",
    # v2.00 local QEMU include-order/C-linkage build fix.
    "guest-pause-guard.hh",
}


def non_test_debug_tools_digest(debug_tools: pathlib.Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    count = 0
    for path in sorted(p for p in debug_tools.rglob("*") if p.is_file()):
        relative = path.relative_to(debug_tools)
        if "tests" in relative.parts or relative.as_posix() in LATER_SCOPED_RUNTIME_FILES:
            continue
        rel_bytes = relative.as_posix().encode()
        data = path.read_bytes()
        digest.update(len(rel_bytes).to_bytes(4, "little"))
        digest.update(rel_bytes)
        digest.update(len(data).to_bytes(8, "little"))
        digest.update(data)
        count += 1
    return count, digest.hexdigest()


def require(text: str, needle: str, message: str) -> None:
    if needle not in text:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    args = parser.parse_args()

    root = pathlib.Path(args.root).resolve()
    debug_tools = root / "ui/xui/debug-tools"
    runner = (debug_tools / "tests/run-regression-tests.py").read_text()

    count, digest = non_test_debug_tools_digest(debug_tools)
    if count != 41 or digest != V195_PROTECTED_NON_TEST_DEBUG_TOOLS_SHA256:
        raise AssertionError(
            "Pass 11 protected Debug Tools runtime files changed outside the "
            "separately guarded v1.97/v1.98/v1.99/v2.00 scoped runtime changes "
            f"(files={count}, sha256={digest})"
        )

    if "import py_compile" in runner or "py_compile.compile" in runner:
        raise AssertionError("Pass 11 syntax checks must not create bytecode artifacts")
    require(runner, 'compile(source, str(path), "exec")',
            "Python syntax validation must compile source in memory")
    require(runner, 'env["PYTHONDONTWRITEBYTECODE"] = "1"',
            "child Python tests must suppress __pycache__ output")
    require(runner, 'choices=("all", *PHASES)',
            "regression runner must expose independently selectable phases")
    require(runner, '"--test", action="append"',
            "regression runner must support targeted golden-test selection")
    require(runner, 'debug_tools.glob("*.py")',
            "top-level Python helper discovery must be automatic")
    require(runner, 'tests.glob("*.py")',
            "test Python helper discovery must be automatic")
    require(runner, 'tests.glob("*-golden.py")',
            "static golden-test discovery must be automatic")
    require(runner, 'xbe_labels_obj = tmp /',
            "native matrix must compile shared xbe-labels.cc once per compiler")
    if runner.count('"-c", str(debug_tools / "xbe-labels.cc")') != 1:
        raise AssertionError("shared xbe-labels.cc object should have one compile recipe")

    print("PASS: v1.96 Pass 11 post-optimization audit/test harness guard")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
