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


UPSTREAM_BUILD_SH_SHA256 = "b6ccae50441b2a953bce072b7e36e63a3dac401488c74ef20ceb2291c39a2b84"
BUILD_SH_HOOK = b'[[ "${XEMU_DEBUG_TOOLS_BUILD_WRAPPED:-0}" == "1" || ! -f "${project_source_dir}/ui/xui/debug-tools/build-xemu.sh" ]] || exec bash "${project_source_dir}/ui/xui/debug-tools/build-xemu.sh" "$@"\n'
CAPSTONE_HELPER = "ui/xui/debug-tools/build-capstone.sh"
CAPSTONE_WINDOWS_WRAPPER = "ui/xui/debug-tools/build-capstone-windows.sh"
KEYSTONE_HELPER = "ui/xui/debug-tools/build-keystone.sh"
DEPENDENCY_PREPARE = "ui/xui/debug-tools/prepare-build-dependencies.sh"
DEBUG_TOOLS_BUILD_WRAPPER = "ui/xui/debug-tools/build-xemu.sh"
DOCKER_BUILD_DRIVER = "ui/xui/debug-tools/docker-build-windows.sh"
EXECUTABLE_HELPER = "ui/xui/debug-tools/restore-executable-bits.py"
VALIDATOR = "ui/xui/debug-tools/validate-project-layout.py"
TESTS_DIR = "ui/xui/debug-tools/tests"
REGRESSION_RUNNER = "ui/xui/debug-tools/tests/v287-run-regression-tests.py"
ASSEMBLER_GOLDEN = "ui/xui/debug-tools/tests/assembler-golden.cpp"
ALLOCATOR_GOLDEN = "ui/xui/debug-tools/tests/v287-allocator-golden.py"
SEARCH_COMPARE_GOLDEN = "ui/xui/debug-tools/tests/v287-search-compare-golden.py"
F0_STEADY_GOLDEN = "ui/xui/debug-tools/tests/v287-f0-steady-state-golden.py"
MEMORY_FORMAT_GOLDEN = "ui/xui/debug-tools/tests/v287-memory-format-golden.py"
PASS3_STRUCTURAL_GOLDEN = "ui/xui/debug-tools/tests/v287-memory-tools-structural-refactor-golden.py"
PASS9_AUDIT_GOLDEN = "ui/xui/debug-tools/tests/v287-audit-pruning-cleanup-golden.py"

HDD_ADDON_DIR = "ui/xui/debug-tools/addons/hdd"
MEMORY_ADDON_DIR = "ui/xui/debug-tools/addons/memory-tools"
HDD_ADDON_SENTINEL = HDD_ADDON_DIR + "/hdd-directory.cc"
MEMORY_ADDON_SENTINEL = MEMORY_ADDON_DIR + "/memory-tools.cc"

# These implementation families belong physically to optional additions. Keep
# the Debug Tools root reserved for the main Current Game/Cheat Engine layer,
# shared services, facade, build helpers, and tests.
ROOT_ADDON_PREFIXES = (
    "hdd-", "fatx-hdd", "guest-kernel-rpc", "kernel-rpc-filesystem",
    "kernel-rpc-utils", "memory-tools", "breakpoint-conditions",
    "register-copy-utils",
)

UPSTREAM_WORKFLOW_SHA256 = {
    ".github/workflows/build.yml": "44ada01c457ecf5f0195e71b71db17a0df4700ceb5ef3f424ce5d3dc5e1d616e",
    ".github/workflows/build-windows.yml": "fa6e7bdb576d4f9491ee1a12da2c8394849a9ca1a6e7e12bb4b003a5c0f54604",
    ".github/workflows/build-linux.yml": "86bf7d1b97b42258e193e5a319ad58fbde01a94a29bace186d25014530969ea7",
    ".github/workflows/build-macos.yml": "21bed54422c484dd9dffb604c8e68985b0c3370113d963d92d4d50857d240aa9",
    ".github/workflows/release.yml": "1ebd3de33da42c27cceb5a40c57b1727eff2c3193dd31a2046a3bb83c42b025f",
}


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
    capstone_windows = require_file(root, CAPSTONE_WINDOWS_WRAPPER, errors)
    keystone = require_file(root, KEYSTONE_HELPER, errors)
    dependency_prepare = require_file(root, DEPENDENCY_PREPARE, errors)
    build_wrapper = require_file(root, DEBUG_TOOLS_BUILD_WRAPPER, errors)
    docker_driver = require_file(root, DOCKER_BUILD_DRIVER, errors)
    executable = require_file(root, EXECUTABLE_HELPER, errors)
    require_file(root, VALIDATOR, errors)
    require_file(root, HDD_ADDON_SENTINEL, errors)
    require_file(root, MEMORY_ADDON_SENTINEL, errors)

    debug_root = root / "ui/xui/debug-tools"
    if debug_root.is_dir():
        leaked = sorted(
            path.name for path in debug_root.iterdir()
            if path.is_file() and path.name.startswith(ROOT_ADDON_PREFIXES)
        )
        if leaked:
            errors.append(
                "optional-addition implementation leaked back into Debug Tools root: "
                + ", ".join(leaked)
            )

    # Tests are an optional source-side addition. A release/fork may omit the
    # entire tests directory and still build normally. If the directory is
    # present, keep validating the complete regression harness so a partial or
    # damaged test package cannot silently pass ownership validation.
    tests_dir = root / TESTS_DIR
    tests_present = tests_dir.is_dir()
    if tests_dir.exists() and not tests_present:
        errors.append(f"optional tests path exists but is not a directory: {TESTS_DIR}")
    if tests_present:
        require_file(root, REGRESSION_RUNNER, errors)
        require_file(root, ASSEMBLER_GOLDEN, errors)
        require_file(root, ALLOCATOR_GOLDEN, errors)
        require_file(root, SEARCH_COMPARE_GOLDEN, errors)
        require_file(root, F0_STEADY_GOLDEN, errors)
        require_file(root, MEMORY_FORMAT_GOLDEN, errors)
        require_file(root, PASS3_STRUCTURAL_GOLDEN, errors)
        require_file(root, PASS9_AUDIT_GOLDEN, errors)

    build_sh = require_file(root, "build.sh", errors)
    workflow_paths = {rel: require_file(root, rel, errors)
                      for rel in UPSTREAM_WORKFLOW_SHA256}

    obsolete_helper = root / "scripts/restore-executable-bits.py"
    if obsolete_helper.exists():
        errors.append(
            "obsolete scripts/restore-executable-bits.py returned; keep this helper under ui/xui/debug-tools/"
        )

    if build_sh.is_file():
        build_bytes = build_sh.read_bytes()
        if build_bytes.count(BUILD_SH_HOOK) != 1:
            errors.append("build.sh must contain exactly one Debug Tools redirect hook line")
        else:
            upstream_bytes = build_bytes.replace(BUILD_SH_HOOK, b"", 1)
            actual = hashlib.sha256(upstream_bytes).hexdigest()
            if actual != UPSTREAM_BUILD_SH_SHA256:
                errors.append(
                    "build.sh contains changes beyond the single Debug Tools redirect hook "
                    f"(upstream-with-hook-removed expected {UPSTREAM_BUILD_SH_SHA256}, got {actual})"
                )

    # Fork policy: preserve xemu's reusable multi-platform workflows exactly.
    # Top-level automation entry points are retained but are manual-only in this fork.
    for rel, expected in UPSTREAM_WORKFLOW_SHA256.items():
        path = workflow_paths[rel]
        if path.is_file():
            actual = sha256(path)
            if actual != expected:
                errors.append(
                    f"{rel}: upstream workflow changed "
                    f"(expected {expected}, got {actual})"
                )

    manual_only_workflows = (
        ".github/workflows/build-xemu-win64-toolchain.yml",
        ".github/workflows/bump-subproject-wraps.yml",
        ".github/workflows/ci.yml",
        ".github/workflows/prerelease.yml",
        ".github/workflows/release-on-tag.yml",
        ".github/workflows/release-published.yml",
        ".github/workflows/release-on-dispatch.yml",
    )
    for rel in manual_only_workflows:
        path = require_file(root, rel, errors)
        if not path.is_file():
            continue
        lines = path.read_text(encoding="utf-8").splitlines()
        try:
            start = next(i for i, line in enumerate(lines) if line == "on:") + 1
        except StopIteration:
            errors.append(f"{rel}: missing workflow trigger block")
            continue
        trigger_lines = []
        for line in lines[start:]:
            if line and not line.startswith((" ", "\t", "#")):
                break
            trigger_lines.append(line)
        trigger = "\n".join(trigger_lines)
        if "  workflow_dispatch:" not in trigger:
            errors.append(f"{rel}: fork workflow must be manually dispatchable")
        for forbidden in ("  push:", "  pull_request:", "  schedule:", "  release:"):
            if forbidden in trigger:
                errors.append(f"{rel}: automatic GitHub Actions trigger is forbidden: {forbidden.strip()}")

    if tests_present:
        regression_runner = root / REGRESSION_RUNNER
        regression_text = read_text(regression_runner, errors) if regression_runner.is_file() else ""
        for required_test in ("v287-allocator-golden.py", "v287-search-compare-golden.py",
                              "v287-f0-steady-state-golden.py", "v287-memory-format-golden.py",
                              "v287-memory-tools-structural-refactor-golden.py",
                              "v287-audit-pruning-cleanup-golden.py"):
            if required_test not in regression_text:
                errors.append(f"{REGRESSION_RUNNER}: required test `{required_test}` is not invoked")

    # Make sure the source-owned target-aware Capstone bootstrap stays generic
    # and the legacy Windows helper remains only a compatibility wrapper.
    if capstone.is_file():
        capstone_text = read_text(capstone, errors)
        if not capstone_text.startswith("#!/usr/bin/env bash"):
            errors.append(f"{CAPSTONE_HELPER}: expected bash shebang")
        for token in (
            "CAPSTONE_X86_SUPPORT=ON",
            "win64-cross",
            "Darwin",
            "Linux",
            "DEB_HOST_GNU_TYPE",
            "CMAKE_OSX_ARCHITECTURES",
            "CAPSTONE_PKG_CONFIG",
            "probe_capstone",
        ):
            if token not in capstone_text:
                errors.append(f"{CAPSTONE_HELPER}: target-aware bootstrap token missing: {token}")
    if capstone_windows.is_file():
        wrapper_text = read_text(capstone_windows, errors)
        if "build-capstone.sh" not in wrapper_text:
            errors.append(f"{CAPSTONE_WINDOWS_WRAPPER}: must delegate to build-capstone.sh")
        if "cmake -S" in wrapper_text or "capstone-engine/capstone" in wrapper_text:
            errors.append(f"{CAPSTONE_WINDOWS_WRAPPER}: implementation duplicated instead of delegating")

    if keystone.is_file():
        keystone_text = read_text(keystone, errors)
        if not keystone_text.startswith("#!/usr/bin/env bash"):
            errors.append(f"{KEYSTONE_HELPER}: expected bash shebang")
        for token in (
            "KEYSTONE_VERSION",
            "KS_ARCH_X86",
            "LLVM_TARGETS_TO_BUILD=X86",
            "CMAKE_CXX_STANDARD=14",
            "win64-cross",
            "Darwin",
            "Linux",
            "DEB_HOST_GNU_TYPE",
            "CMAKE_OSX_ARCHITECTURES",
            "KEYSTONE_PKG_CONFIG",
            'KEYSTONE_PKGCONFIG_MIN_VERSION="${KEYSTONE_PKGCONFIG_MIN_VERSION:-0.9}"',
            "pkg-config reports Keystone version:",
            "probe_keystone",
            "patch_windows_keystone_pkgconfig",
            'smoke_output="${smoke_dir}/keystone-smoke"',
            'smoke_output="${smoke_output}.exe"',
            "expected output is missing or empty:",
            "Libs.private: ",
            "-lshell32",
            "-lole32",
            "-luuid",
            "keystone_source_root",
            "$source_dir/src",
            "native CMake source root was not found",
            "#include <cstdint>",
            "cmake_minimum_required(VERSION 3.10.0)",
        ):
            if token not in keystone_text:
                errors.append(f"{KEYSTONE_HELPER}: target-aware bootstrap token missing: {token}")

    if build_sh.is_file():
        build_text = read_text(build_sh, errors)
        for token in ("debug_tools_capstone_helper", "debug_tools_keystone_helper",
                      "XEMU_DEBUG_TOOLS_SKIP_CAPSTONE_BOOTSTRAP",
                      "XEMU_DEBUG_TOOLS_SKIP_KEYSTONE_BOOTSTRAP"):
            if token in build_text:
                errors.append(f"build.sh: Debug Tools dependency code leaked back into upstream file: {token}")

    if dependency_prepare.is_file():
        prepare_text = read_text(dependency_prepare, errors)
        for token in (
            'build-capstone.sh', 'build-keystone.sh',
            'XEMU_CAPSTONE_PKG_CONFIG_PATH', 'XEMU_KEYSTONE_PKG_CONFIG_PATH',
            'PKG_CONFIG_PATH', 'XEMU_DEBUG_TOOLS_DEPENDENCIES_READY',
        ):
            if token not in prepare_text:
                errors.append(f"{DEPENDENCY_PREPARE}: dependency ownership token missing: {token}")

    if build_wrapper.is_file():
        wrapper_text = read_text(build_wrapper, errors)
        for token in (
            'prepare-build-dependencies.sh', '--enable-capstone',
            'exec bash "$project_root/build.sh"',
            'export XEMU_DEBUG_TOOLS_BUILD_WRAPPED=1',
            'XEMU_DEBUG_TOOLS_SKIP_DEPENDENCY_BOOTSTRAP',
        ):
            if token not in wrapper_text:
                errors.append(f"{DEBUG_TOOLS_BUILD_WRAPPER}: wrapper token missing: {token}")

    if docker_driver.is_file():
        docker_text = read_text(docker_driver, errors)
        if 'bash ./build.sh -p win64-cross' not in docker_text:
            errors.append(f"{DOCKER_BUILD_DRIVER}: Docker build must enter through normal root build.sh")
        if 'build-capstone-windows.sh' in docker_text or 'build-xemu.sh -p win64-cross' in docker_text:
            errors.append(f"{DOCKER_BUILD_DRIVER}: Docker build must exercise the one-line root Debug Tools hook")
        for token in (
            '$OUT/source/${PKG_NAME}-source.zip',
            '$OUT/source/${PKG_NAME}.tar.zst',
            'zipfile.ZipFile',
            'source-zip.sha256',
            "printf '%s\\n' \"$XEMU_DEBUG_TOOLS_PROFILE\" > ./ui/xui/debug-tools/build-profile.txt",
            'git add -- "$helper"',
        ):
            if token not in docker_text:
                errors.append(f"{DOCKER_BUILD_DRIVER}: source-package output token missing: {token}")

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
    print(f"  Windows compatibility wrapper: {CAPSTONE_WINDOWS_WRAPPER}")
    print(f"  Keystone helper: {KEYSTONE_HELPER}")
    print(f"  Dependency prepare: {DEPENDENCY_PREPARE}")
    print(f"  Debug Tools build wrapper: {DEBUG_TOOLS_BUILD_WRAPPER}")
    print(f"  Docker build driver: {DOCKER_BUILD_DRIVER}")
    print(f"  HDD addition ownership: {HDD_ADDON_DIR}")
    print(f"  Memory addition ownership: {MEMORY_ADDON_DIR}")
    print("  Docker source package: ZIP + GitHub-style tar.zst emitted with each build")
    print(f"  Executable-bit helper: {EXECUTABLE_HELPER}")
    print(f"  upstream build.sh SHA-256 after removing one hook line: {UPSTREAM_BUILD_SH_SHA256}")
    print("  root build.sh custom footprint: exactly one Debug Tools redirect line")
    tests_dir = root / TESTS_DIR
    if tests_dir.is_dir():
        print(f"  Optional regression tests: present ({TESTS_DIR})")
    else:
        print(f"  Optional regression tests: absent; build is allowed ({TESTS_DIR})")
    print("  Reusable upstream GitHub workflows: preserved; fork entry points: manual-only")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
