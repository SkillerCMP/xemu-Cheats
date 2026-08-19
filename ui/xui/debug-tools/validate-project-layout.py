#!/usr/bin/env python3
"""Validate xemu RAW Cheat Engine / Debugger ownership invariants.

This is intentionally a source-layout/build regression guard. It does not inspect
or change guest/runtime behavior.  Keep substantive custom debugger/cheat build
logic under ui/xui/debug-tools/ and leave upstream files as small integration
hooks.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path


EXPECTED_BUILD_SH_SHA256 = "b6ccae50441b2a953bce072b7e36e63a3dac401488c74ef20ceb2291c39a2b84"
CAPSTONE_HELPER = "ui/xui/debug-tools/build-capstone-windows.sh"
EXECUTABLE_HELPER = "ui/xui/debug-tools/restore-executable-bits.py"
VALIDATOR = "ui/xui/debug-tools/validate-project-layout.py"
REGRESSION_RUNNER = "ui/xui/debug-tools/tests/run-regression-tests.py"
ASSEMBLER_GOLDEN = "ui/xui/debug-tools/tests/assembler-golden.cpp"
ALLOCATOR_GOLDEN = "ui/xui/debug-tools/tests/allocator-golden.py"
SEARCH_COMPARE_GOLDEN = "ui/xui/debug-tools/tests/search-compare-golden.py"
F0_STEADY_GOLDEN = "ui/xui/debug-tools/tests/f0-steady-state-golden.py"
MEMORY_FORMAT_GOLDEN = "ui/xui/debug-tools/tests/memory-format-golden.py"
PASS3_STRUCTURAL_GOLDEN = "ui/xui/debug-tools/tests/pass3-structural-refactor-golden.py"
PASS9_AUDIT_GOLDEN = "ui/xui/debug-tools/tests/pass9-audit-cleanup-golden.py"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_text(path: Path, errors: list[str]) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        errors.append(f"cannot read {path}: {exc}")
        return ""


def require_file(root: Path, rel: str, errors: list[str]) -> Path:
    path = root / rel
    if not path.is_file():
        errors.append(f"required file is missing: {rel}")
    return path


def require_once(text: str, needle: str, where: str, errors: list[str]) -> None:
    count = text.count(needle)
    if count != 1:
        errors.append(f"{where}: expected exactly one `{needle}` reference, found {count}")


def validate(root: Path) -> list[str]:
    errors: list[str] = []

    capstone = require_file(root, CAPSTONE_HELPER, errors)
    executable = require_file(root, EXECUTABLE_HELPER, errors)
    require_file(root, VALIDATOR, errors)
    require_file(root, REGRESSION_RUNNER, errors)
    require_file(root, ASSEMBLER_GOLDEN, errors)
    require_file(root, ALLOCATOR_GOLDEN, errors)
    require_file(root, SEARCH_COMPARE_GOLDEN, errors)
    require_file(root, F0_STEADY_GOLDEN, errors)
    require_file(root, MEMORY_FORMAT_GOLDEN, errors)
    require_file(root, PASS3_STRUCTURAL_GOLDEN, errors)
    require_file(root, PASS9_AUDIT_GOLDEN, errors)
    build_sh = require_file(root, "build.sh", errors)
    windows_yml = require_file(root, ".github/workflows/build-windows.yml", errors)
    build_yml = require_file(root, ".github/workflows/build.yml", errors)

    obsolete_helper = root / "scripts/restore-executable-bits.py"
    if obsolete_helper.exists():
        errors.append(
            "obsolete scripts/restore-executable-bits.py returned; keep this helper under ui/xui/debug-tools/"
        )

    if build_sh.is_file():
        actual = sha256(build_sh)
        if actual != EXPECTED_BUILD_SH_SHA256:
            errors.append(
                "build.sh no longer matches the accepted upstream baseline "
                f"(expected {EXPECTED_BUILD_SH_SHA256}, got {actual})"
            )

    windows_text = read_text(windows_yml, errors) if windows_yml.is_file() else ""
    build_text = read_text(build_yml, errors) if build_yml.is_file() else ""
    regression_runner = root / REGRESSION_RUNNER
    regression_text = read_text(regression_runner, errors) if regression_runner.is_file() else ""
    for required_test in ("allocator-golden.py", "search-compare-golden.py",
                          "f0-steady-state-golden.py", "memory-format-golden.py",
                          "pass3-structural-refactor-golden.py",
                          "pass9-audit-cleanup-golden.py"):
        if required_test not in regression_text:
            errors.append(f"{REGRESSION_RUNNER}: required test `{required_test}` is not invoked")

    capstone_call = f"bash ./{CAPSTONE_HELPER}"
    require_once(windows_text, capstone_call, str(windows_yml.relative_to(root)), errors)

    # Source must exist before the workflow calls a helper stored inside it.
    extract_marker = "tar -xf xemu-${{ inputs.pkg_version }}.tar.zst --strip-components=2"
    if extract_marker not in windows_text:
        errors.append("build-windows.yml: source extraction step is missing")
    elif capstone_call in windows_text and windows_text.index(extract_marker) > windows_text.index(capstone_call):
        errors.append("build-windows.yml: Capstone helper is called before the source package is extracted")

    # The workflow may name the step, but substantive Capstone setup belongs in
    # build-capstone-windows.sh. These tokens catch the previously regressed
    # inline implementation without caring about the display name.
    forbidden_capstone_workflow_tokens = (
        "capstone-engine/capstone",
        "CAPSTONE_VERSION=",
        "CAPSTONE_PREFIX=",
        "CAPSTONE_CC=",
        "CAPSTONE_AR=",
        "CAPSTONE_RANLIB=",
        "CAPSTONE_PKG_CONFIG=",
        "/tmp/capstone-build",
        "CAPSTONE_X86_SUPPORT",
        "capstone-header-smoke.c",
    )
    for token in forbidden_capstone_workflow_tokens:
        if token in windows_text:
            errors.append(
                f"build-windows.yml contains Capstone implementation token `{token}`; move it back to {CAPSTONE_HELPER}"
            )

    regression_call = f"python3 ./{REGRESSION_RUNNER} --root ."
    require_once(build_text, regression_call, str(build_yml.relative_to(root)), errors)
    package_marker = "bash ./scripts/archive-source.sh"
    if regression_call in build_text and package_marker in build_text and \
            build_text.index(regression_call) > build_text.index(package_marker):
        errors.append("build.yml: Debug Tools regression suite runs after source packaging")

    executable_call = f"python3 ./{EXECUTABLE_HELPER} --root . --update-git-index"
    require_once(build_text, executable_call, str(build_yml.relative_to(root)), errors)

    # Guard against the old inline executable-bit Python block coming back.
    forbidden_build_workflow_tokens = (
        "git update-index --chmod=+x",
        "os.walk(root)",
        "stat.S_IXUSR",
        "read(2) == b\"#!\"",
    )
    for token in forbidden_build_workflow_tokens:
        if token in build_text:
            errors.append(
                f"build.yml contains executable-bit implementation token `{token}`; keep that logic in {EXECUTABLE_HELPER}"
            )

    # Make sure the helper files are non-empty and still have the expected
    # interpreter identity. This catches accidental empty/renamed replacements.
    if capstone.is_file():
        capstone_text = read_text(capstone, errors)
        if not capstone_text.startswith("#!/usr/bin/env bash"):
            errors.append(f"{CAPSTONE_HELPER}: expected bash shebang")
        if "CAPSTONE_X86_SUPPORT=ON" not in capstone_text:
            errors.append(f"{CAPSTONE_HELPER}: x86-only Capstone configuration is missing")
        if '"${CAPSTONE_PKG_CONFIG}" --modversion capstone' not in capstone_text:
            errors.append(f"{CAPSTONE_HELPER}: target pkg-config verification is missing")

    if executable.is_file():
        executable_text = read_text(executable, errors)
        if not executable_text.startswith("#!/usr/bin/env python3"):
            errors.append(f"{EXECUTABLE_HELPER}: expected python3 shebang")
        if "--update-git-index" not in executable_text:
            errors.append(f"{EXECUTABLE_HELPER}: Git index update option is missing")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=None, help="xemu source tree root")
    args = parser.parse_args()

    root = Path(args.root).resolve() if args.root else Path(__file__).resolve().parents[3]
    errors = validate(root)
    if errors:
        print("Debug-tools ownership/layout validation FAILED:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print("Debug-tools ownership/layout validation passed.")
    print(f"  Capstone helper: {CAPSTONE_HELPER}")
    print(f"  Executable-bit helper: {EXECUTABLE_HELPER}")
    print(f"  build.sh baseline SHA-256: {EXPECTED_BUILD_SH_SHA256}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
