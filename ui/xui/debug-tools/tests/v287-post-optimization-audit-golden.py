#!/usr/bin/env python3
# v2.87 current regression ownership.
"""v1.96 Pass 11 guard: runtime source freeze + regression-harness cleanup."""
from __future__ import annotations

import argparse
import hashlib
import pathlib


# v2.43 changes the previously protected XDVDFS parser only for separately
# guarded read-only Disc Contents export/re-resolution. The remaining original
# Pass-11 runtime set stays frozen to this digest.
V287_PROTECTED_RUNTIME_SHA256 = (
    # v2.90 removes the old hand-written x86-cheat-assembler-encode.cc from
    # this historical protected set. The 16 surviving unrelated runtime files
    # remain byte-for-byte identical to v2.89.1.
    "d5c6201271febf2717a5c7537bd5c586da17e5118c61fb416520591ff9a3596a"
)

LATER_SCOPED_RUNTIME_FILES = {
    # v2.91 modular Debug Tools facade/addition ownership.
    "debug-tools.cc",
    "debug-tools.hh",
    "debug-tools-module.hh",
    "build-profile.txt",
    "read-build-profile.py",
    "addons/hdd/debug-tools-hdd-addon.cc",
    "addons/memory-tools/debug-tools-memory-tools-addon.cc",
    "addons/stubs/debug-tools-hdd-addon-stub.cc",
    "addons/stubs/debug-tools-memory-tools-addon-stub.cc",
    # v2.86 Phase 11 final production-audit ownership.
    "cheat-engine-memory.h",
    "addons/hdd/guest-kernel-rpc.cc",
    "addons/hdd/guest-kernel-rpc-ui.cc",
    "addons/hdd/guest-kernel-rpc-completion.cc",
    "addons/hdd/guest-kernel-rpc-filesystem.cc",
    "addons/hdd/guest-kernel-rpc.hh",
    "addons/hdd/guest-kernel-rpc-status.hh",
    "addons/hdd/kernel-rpc-utils.hh",
    "label-packs.cc",
    "guest-pause-guard.hh",
    "addons/hdd/hdd-directory.hh",
    "addons/hdd/hdd-export-service.cc",
    "host-export-utils.hh",
    "xdvdfs-export-service.cc",
    "meson.build",
    # v2.83 Combined Phase 8 Inject UI + x86 assembler ownership cleanup.
    "addons/memory-tools/memory-tools-inject.cc",
    "addons/memory-tools/memory-tools-inject-ui.cc",
    "x86-cheat-assembler.cc",
    "x86-cheat-assembler-keystone.cc",
    "x86-cheat-assembler-internal.hh",
    "meson.build",
    # v2.82 Combined Phase 7 Search/Dump UI + label-parser helper cleanup.
    "addons/memory-tools/memory-tools-search.cc",
    "addons/memory-tools/memory-tools-search-ui.cc",
    "addons/memory-tools/memory-tools-dump.cc",
    "addons/memory-tools/memory-tools-dump-ui.cc",
    "label-symbol-utils.hh",
    "map-labels.cc",
    "pdb-labels.cc",
    "xdk-labels.cc",
    "meson.build",
    # v2.81 Phase-6 Labels UI/core ownership split.
    "addons/memory-tools/memory-tools-labels.cc",
    "addons/memory-tools/memory-tools-labels-ui.cc",
    # v2.74 exact Inject Restore / Change runtime surface.
    "addons/memory-tools/memory-tools-inject.cc",
    "addons/memory-tools/memory-tools.hh",
    # v2.73 Phase-1 duplicate parser-helper consolidation. The dedicated
    # v2.73 guard fingerprints this exact surface and native tests exercise it.
    "binary-utils.hh",
    "label-symbol-utils.hh",
    "map-labels.cc",
    "pdb-labels.cc",
    "xbe-labels.cc",
    "xdk-labels.cc",
    # v2.84 Phase 9 Cheat Engine core ownership split; Phase 2 UI and public header
    # remain independently guarded by their dedicated lineage tests.
    "cheat-engine.cc",
    "cheat-engine-source.cc",
    "cheat-engine-fhooks.cc",
    "cheat-engine-execute.cc",
    "cheat-engine-ui.cc",
    "cheat-engine.hh",
    # PREENTRY/Patch documentation for the separately guarded feature lineage.
    "README.md",
    "CHANGELOG.md",
    # v2.62 separately guarded mixed-state group checkbox helper.
    "mixed-checkbox.hh",
    # v2.70 separately guarded executable-bit temporary-index validation path.
    "restore-executable-bits.py",
    # v2.87 test naming cleanup updates only current test filenames referenced by
    # the project-layout validator; it is test/build infrastructure, not runtime.
    "validate-project-layout.py",
    # v2.89/v2.90 source-owned target-aware Capstone/Keystone bootstraps.
    "build-capstone.sh",
    "build-capstone-windows.sh",
    "build-keystone.sh",
    "prepare-build-dependencies.sh",
    "build-xemu.sh",
    "docker-build-windows.sh",
    # v2.90 public assembler API documents the Keystone single-backend path.
    "x86-cheat-assembler.hh",
    "detached-tools.cc",
    "detached-tools.hh",
    "addons/memory-tools/memory-tools-debugger.cc",
    "addons/memory-tools/memory-tools-debugger-ui.cc",
    # v2.79 separately guarded Memory Viewer UI/core ownership split.
    "addons/memory-tools/memory-tools-memory.cc",
    "addons/memory-tools/memory-tools-memory-ui.cc",
    "tab-style.hh",
    "addons/memory-tools/register-copy-utils.hh",
    # v1.99 detached Current Game + read-only HDD Directory feature.
    "current-game.cc",
    "current-game-ui.cc",
    "current-game.hh",
    "addons/hdd/fatx-hdd.cc",
    "addons/hdd/fatx-hdd.hh",
    "addons/hdd/hdd-directory.cc",
    "addons/hdd/hdd-directory-ui.cc",
    "addons/hdd/hdd-directory.hh",
    "meson.build",
    # v2.00/v2.71 separately guarded QEMU include ownership/C-linkage path.
    "guest-pause-guard.hh",
    "guest-pause-guard.cc",
    # v2.04 restricted FATX delete BlockBackend write/flush bridge.
    "disc-block-io.c",
    "disc-block-io.h",
    # v2.06 separately guarded harmless Guest Kernel RPC foundation.
    "addons/hdd/guest-kernel-rpc.cc",
    "addons/hdd/guest-kernel-rpc.hh",
    "addons/hdd/guest-kernel-rpc-memory.c",
    "addons/hdd/guest-kernel-rpc-memory.h",
    "addons/hdd/kernel-rpc-utils.hh",
    # v2.14 separately guarded reusable Kernel RPC filesystem planning/preflight layer.
    "addons/hdd/kernel-rpc-filesystem.cc",
    "addons/hdd/kernel-rpc-filesystem-stream.cc",
    "addons/hdd/kernel-rpc-filesystem-internal.hh",
    "addons/hdd/kernel-rpc-filesystem.hh",
    # v2.17 separately guarded production filesystem executor/snapshot ownership split.
    "addons/hdd/guest-kernel-rpc-filesystem.cc",
    "addons/hdd/hdd-snapshot-service.cc",
    "addons/hdd/hdd-snapshot-service.hh",
    # v2.31 separately guarded read-only HDD export service extraction.
    "addons/hdd/hdd-export-service.cc",
    "addons/hdd/hdd-export-service.hh",
    # v2.41 separately guarded Guest Kernel RPC completion-handler split.
    "addons/hdd/guest-kernel-rpc-completion.cc",
    # v2.43 separately guarded read-only Disc Contents export service and
    # XDVDFS read-side identity/re-resolution support.
    "xdvdfs-export-service.cc",
    "xdvdfs-export-service.hh",
    "xdvdfs-disc.cc",
    "xdvdfs-disc.hh",
}


def non_test_debug_tools_digest(debug_tools: pathlib.Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    count = 0

    # v2.91.5 physically relocates optional Memory Tools sources below its
    # add-on directory without changing these historically protected file
    # bodies. Normalize those legacy paths (and sort by the normalized path)
    # so this old Pass-11 byte freeze continues measuring behavior rather than
    # directory ownership.
    legacy_path = {
        "addons/memory-tools/breakpoint-conditions.cc": "breakpoint-conditions.cc",
        "addons/memory-tools/breakpoint-conditions.hh": "breakpoint-conditions.hh",
        "addons/memory-tools/memory-tools-internal.hh": "memory-tools-internal.hh",
        "addons/memory-tools/memory-tools.cc": "memory-tools.cc",
    }
    candidates = []
    for path in (p for p in debug_tools.rglob("*") if p.is_file()):
        relative = path.relative_to(debug_tools)
        rel = relative.as_posix()
        if "tests" in relative.parts or rel in LATER_SCOPED_RUNTIME_FILES:
            continue
        candidates.append((legacy_path.get(rel, rel), rel, path))

    for normalized_rel, rel, path in sorted(candidates):
        rel_bytes = normalized_rel.encode()
        data = path.read_bytes()
        if rel == "addons/memory-tools/memory-tools.cc":
            # v2.49 adds only Debug-Tools-local tab styling to this otherwise
            # protected historical file. Normalize those exact lines out so
            # Pass 11 continues freezing every other byte of memory-tools.cc.
            text = data.decode("utf-8")
            include = '#include "tab-style.hh"\n'
            if text.count(include) != 1:
                raise AssertionError("Debug Tools memory-tools tab-style include changed unexpectedly")
            text = text.replace(include, "", 1)
            scoped = '    XemuDebugUi::ScopedTabStyle tab_style;\n'
            restore = '    tab_style.Restore();\n'
            push = '    XemuDebugUi::PushTabStyle();\n'
            pop = '    XemuDebugUi::PopTabStyle();\n'
            if text.count(scoped) == 1 and push not in text and pop not in text:
                text = text.replace(scoped, "", 1)
                if text.count(restore) == 1:
                    text = text.replace(restore, "", 1)
            elif text.count(push) == 1 and text.count(pop) == 1 and scoped not in text:
                text = text.replace(push, "", 1).replace(pop, "", 1)
            else:
                raise AssertionError("Debug Tools memory-tools tab-style ownership changed unexpectedly")
            data = text.encode("utf-8")
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
    runner = (debug_tools / "tests/v287-run-regression-tests.py").read_text()

    count, digest = non_test_debug_tools_digest(debug_tools)
    if count != 16 or digest != V287_PROTECTED_RUNTIME_SHA256:
        raise AssertionError(
            "Pass 11 protected Debug Tools runtime files changed outside the "
            "separately guarded feature/refactor scoped runtime changes "
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
