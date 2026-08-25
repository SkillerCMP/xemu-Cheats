#!/usr/bin/env python3
# v2.87 current regression ownership.
"""v2.87 current ownership/structure regression guard.

This replaces the stale per-phase v2.72-v2.85 rebuild fingerprints with one
current semantic ownership check. Exact whole-production fingerprint ownership
remains in v287-final-production-audit-golden.py.
"""
from __future__ import annotations

import argparse
import pathlib
import re

from v287_source_test_utils import extract_member_functions


def require(text: str, needle: str, what: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {what}: {needle}")


def forbid(text: str, needle: str, what: str) -> None:
    if needle in text:
        raise AssertionError(f"unexpected {what}: {needle}")


def member_names(text: str, cls: str) -> list[str]:
    return [name for name, _ in extract_member_functions(text, cls)]


def require_meson_pair(meson: str, core: str, ui: str) -> None:
    require(meson, f"'{core}',", f"Meson core owner {core}")
    require(meson, f"'{ui}',", f"Meson UI owner {ui}")


def main() -> int:
    ap = argparse.ArgumentParser(); ap.add_argument("--root", default=".")
    root = pathlib.Path(ap.parse_args().root).resolve()
    debug = root / "ui/xui/debug-tools"
    tests = debug / "tests"

    read = lambda rel: (debug / rel).read_text(encoding="utf-8")
    meson = read("meson.build")
    source_utils = (tests / "v287_source_test_utils.py").read_text(encoding="utf-8")
    runner = (tests / "v287-run-regression-tests.py").read_text(encoding="utf-8")

    # Shared parser/symbol helpers remain centralized instead of growing local
    # duplicate implementations back into the split subsystems.
    binary = read("binary-utils.hh")
    labels = read("label-symbol-utils.hh")
    for helper in ("read_le16", "read_le32", "range_inside"):
        if not re.search(rf"inline\s+[^\n]*\b{helper}\s*\(", binary):
            raise AssertionError(f"shared binary helper missing: {helper}")
    require(labels, "inline std::string upper_ascii(std::string value)",
            "shared ASCII uppercase helper")
    require(labels, "lower_ascii", "shared lowercase symbol helper")
    require(labels, "compiler_internal_symbol", "shared compiler-symbol filter")
    require(labels, "display_microsoft_symbol", "shared Microsoft symbol display helper")

    current = read("current-game.cc")
    require(current, '#include "binary-utils.hh"', "Current Game shared binary ownership")
    require(current, "using XemuDebugBinaryUtils::range_inside;",
            "Current Game shared range validation")
    xdk = read("xdk-labels.cc"); mapcc = read("map-labels.cc"); pdb = read("pdb-labels.cc")
    forbid(xdk, "static std::string lower_ascii(", "duplicate XDK lowercase helper")
    require(xdk, "using XemuLabelSymbolUtils::lower_ascii;", "XDK shared lowercase helper")
    forbid(mapcc, "static bool compiler_internal_symbol(", "duplicate MAP compiler filter")
    forbid(pdb, "static bool compiler_internal_symbol(", "duplicate PDB compiler filter")
    require(mapcc, "XemuLabelSymbolUtils::display_microsoft_symbol(symbol.name)",
            "MAP shared display helper")
    require(pdb, "XemuLabelSymbolUtils::display_microsoft_symbol(name)",
            "PDB shared display helper")

    # Cheat Engine ownership: Tick stays the coordinator, source/PREENTRY,
    # F-hook lifecycle, RAW execution and rendering each have a focused owner.
    cheat_core = read("cheat-engine.cc")
    cheat_source = read("cheat-engine-source.cc")
    cheat_hooks = read("cheat-engine-fhooks.cc")
    cheat_exec = read("cheat-engine-execute.cc")
    cheat_ui = read("cheat-engine-ui.cc")
    if member_names(cheat_core, "CheatEngineWindow") != ["Tick"]:
        raise AssertionError("Cheat Engine coordinator ownership changed")
    require(cheat_source, 'static const std::string kPreEntryPrefix = ":PREENTRY:";',
            "PREENTRY parser marker in source owner")
    require(cheat_hooks, "DeactivateFHook", "F-hook lifecycle owner")
    require(cheat_hooks, "state.original_bytes.data()", "captured F-hook restore owner")
    require(cheat_exec, "bool CheatEngineWindow::ExecuteTypeF(", "Type-F execution owner")
    expected_cheat_ui = {
        "SetGroupSelected", "SetPatchGroupSelected", "DisableAllCheats",
        "SetLiveCheatsEnabled", "CountGroupSelection", "CountPatchGroupSelection",
        "DrawCheat", "DrawPatch", "DrawGroup", "DrawPatchGroup",
        "DrawMenuBar", "DrawHelpPopup", "Draw",
    }
    if set(member_names(cheat_ui, "CheatEngineWindow")) != expected_cheat_ui:
        raise AssertionError("Cheat Engine UI method ownership changed")
    for rel, text in (("cheat-engine.cc", cheat_core), ("cheat-engine-source.cc", cheat_source),
                      ("cheat-engine-fhooks.cc", cheat_hooks), ("cheat-engine-execute.cc", cheat_exec)):
        forbid(text, "ImGui::", f"direct ImGui rendering in {rel}")
        require(meson, f"'{rel}',", f"Meson Cheat Engine owner {rel}")
        require(source_utils, f'"{rel}",', f"combined Cheat Engine source owner {rel}")
    require(meson, "'cheat-engine-ui.cc',", "Meson Cheat Engine UI owner")
    require(source_utils, '"cheat-engine-ui.cc",', "combined Cheat Engine UI owner")

    # Core/UI splits: current tests care about final ownership, not the old
    # phase-specific byte hashes or historical line-count windows.
    split_contracts = (
        ("addons/memory-tools/memory-tools-debugger.cc", "addons/memory-tools/memory-tools-debugger-ui.cc", "MemoryToolsWindow",
         {"DrawBreakpointConditionEditor", "DrawGeneralRegisterTable", "DrawExtraRegisterTable",
          "DrawRegisters", "DrawF0TempRegisters", "DrawBreakpoints", "DrawBreakpointContents",
          "DrawChanges", "DrawDisassemblyPane", "DrawDebugger"},
         "// Debugger rendering/UI methods are owned by memory-tools-debugger-ui.cc.", True),
        ("addons/memory-tools/memory-tools-memory.cc", "addons/memory-tools/memory-tools-memory-ui.cc", "MemoryToolsWindow",
         {"PrepareMemoryByteEdit", "DrawScrollableMemoryPane", "DrawMemoryMapPane", "DrawMemoryWorkspace"},
         "// Memory Viewer rendering/UI methods are owned by memory-tools-memory-ui.cc.", False),
        ("current-game.cc", "current-game-ui.cc", "CurrentGameManager",
         {"DrawInlineSummary", "DrawGameInfoTab", "DrawDiscEntry", "DrawDiscContentsTab", "Draw"},
         "// Current Game rendering/UI methods are owned by current-game-ui.cc.", False),
        ("addons/memory-tools/memory-tools-labels.cc", "addons/memory-tools/memory-tools-labels-ui.cc", "MemoryToolsWindow",
         {"DrawLabelBrowser"}, "// Labels rendering/UI is owned by memory-tools-labels-ui.cc.", False),
        ("addons/memory-tools/memory-tools-search.cc", "addons/memory-tools/memory-tools-search-ui.cc", "MemoryToolsWindow",
         {"DrawSearch"}, "// Memory Search rendering/UI is owned by memory-tools-search-ui.cc.", False),
        ("addons/memory-tools/memory-tools-dump.cc", "addons/memory-tools/memory-tools-dump-ui.cc", "MemoryToolsWindow",
         {"DrawDumpRam"}, "// RAM Dump rendering/UI is owned by memory-tools-dump-ui.cc.", False),
        ("addons/memory-tools/memory-tools-inject.cc", "addons/memory-tools/memory-tools-inject-ui.cc", "MemoryToolsWindow",
         {"DrawInstructionChanger", "DrawCodeCaveBuilder", "DrawAddressContextMenu"}, None, False),
        ("addons/hdd/hdd-directory.cc", "addons/hdd/hdd-directory-ui.cc", "HddDirectoryWindow",
         {"DrawDeleteConfirmation", "DrawExportContext", "DrawImportMenuItems", "DrawRootImportButton",
          "DrawMoveConfirmation", "DrawRenamePopup", "DrawNewFolderPopup", "DrawImportConfirmation",
          "DrawKernelStatus", "DrawEntries", "DrawCurrentGameArea", "DrawCurrentGameHdd", "Draw"}, None, False),
    )
    for core_rel, ui_rel, cls, expected_ui, marker, allow_core_imgui in split_contracts:
        core_text = read(core_rel); ui_text = read(ui_rel)
        actual_ui = set(member_names(ui_text, cls))
        if actual_ui != expected_ui:
            raise AssertionError(f"unexpected {ui_rel} method ownership: {sorted(actual_ui)}")
        for name in expected_ui:
            if f"::{name}(" in core_text:
                raise AssertionError(f"{name} returned to {core_rel}")
        if not allow_core_imgui:
            forbid(core_text, "ImGui::", f"direct ImGui rendering in {core_rel}")
        if marker:
            require(core_text, marker, f"ownership boundary marker in {core_rel}")
        require_meson_pair(meson, core_rel, ui_rel)
        if core_rel.startswith("memory-tools-"):
            require(source_utils, f'"{ui_rel}",', f"combined MemoryTools source owner {ui_rel}")

    # Labels retains exactly one core method and one UI method in its focused units.
    if member_names(read("addons/memory-tools/memory-tools-labels.cc"), "MemoryToolsWindow") != ["DumpLabels"]:
        raise AssertionError("Labels core ownership changed")
    if member_names(read("addons/memory-tools/memory-tools-labels-ui.cc"), "MemoryToolsWindow") != ["DrawLabelBrowser"]:
        raise AssertionError("Labels UI ownership changed")

    # Search/Dump retain execution in core and only their drawing entrypoint in UI.
    for rel, required in (
        ("addons/memory-tools/memory-tools-search.cc", "bool MemoryToolsWindow::MatchTarget("),
        ("addons/memory-tools/memory-tools-dump.cc", "void MemoryToolsWindow::DumpRam("),
    ):
        require(read(rel), required, f"execution/state ownership in {rel}")

    # Inject action state stays in core. Keystone is the sole generic IA-32
    # encoder; F0-specific directive/temp/layout ownership stays in the frontend.
    inject_core = read("addons/memory-tools/memory-tools-inject.cc")
    for name in ("InjectNop", "ApplyInstructionChange", "RestoreInstructionChange", "OpenCodeCaveBuilder"):
        require(inject_core, f"MemoryToolsWindow::{name}", f"Inject core owner {name}")
    asm_front = read("x86-cheat-assembler.cc")
    asm_encode = read("x86-cheat-assembler-keystone.cc")
    asm_internal = read("x86-cheat-assembler-internal.hh")
    forbid(asm_front, "struct MemoryOperand", "low-level assembler operand implementation in frontend")
    forbid(asm_front, "static bool encode_instruction", "low-level assembler encoder in frontend")
    require(asm_front, "using namespace xemu_cheat_assembler_internal;", "assembler frontend/internal bridge")
    require(asm_encode, '#include <keystone/keystone.h>', "Keystone assembler owner")
    require(asm_encode, "ks_open(KS_ARCH_X86, KS_MODE_32", "Keystone IA-32 engine")
    require(asm_encode, "ks_asm(engine.get()", "single generic x86 encoder")
    if (debug / "x86-cheat-assembler-encode.cc").exists():
        raise AssertionError("retired hand-written assembler encoder returned")
    for api in ("trim", "upper", "strip_comment", "parse_register", "reg32", "valid_label",
                "parse_number", "split_operands", "emit_u32", "condition_code", "encode_instruction"):
        if not re.search(rf"\b{api}\s*\(", asm_internal):
            raise AssertionError(f"assembler internal API missing: {api}")
    require(meson, "keystone = dependency('keystone', required: true, static: true)",
            "Meson Keystone dependency")
    require(meson, "'x86-cheat-assembler-keystone.cc'", "Meson Keystone assembler owner")
    require(runner, 'debug_tools / "x86-cheat-assembler-keystone.cc"', "native assembler encoder coverage")

    # HDD/FATX/filesystem Phase-10 structure remains current, without historical
    # per-phase hashes. Production mutation invariants are additionally checked by
    # the final audit and executable filesystem native goldens.
    hdd_core = read("addons/hdd/hdd-directory.cc"); hdd_ui = read("addons/hdd/hdd-directory-ui.cc")
    forbid(hdd_core, "ImGui::", "direct ImGui rendering in HDD action/state core")
    fatx = read("addons/hdd/fatx-hdd.cc")
    require(fatx, '#include "binary-utils.hh"', "FATX shared binary helper include")
    for local, shared in (("ReadLe16", "read_le16"), ("ReadLe32", "read_le32"), ("RangeInside", "range_inside")):
        require(fatx, f"constexpr auto {local} = XemuDebugBinaryUtils::{shared};",
                f"FATX shared helper alias {local}")
    fs_core = read("addons/hdd/kernel-rpc-filesystem.cc")
    fs_stream = read("addons/hdd/kernel-rpc-filesystem-stream.cc")
    forbid(fs_core, "struct ImportHostStream::Impl", "host stream implementation in planning core")
    forbid(fs_core, "void ImportHostStream::Reset()", "host stream Reset in planning core")
    require(fs_stream, "struct ImportHostStream::Impl", "host stream implementation owner")
    require(fs_stream, "void ImportHostStream::Reset()", "host stream Reset owner")
    require(fs_core, '#include "kernel-rpc-filesystem-internal.hh"', "filesystem private metadata helper")
    require(fs_stream, '#include "kernel-rpc-filesystem-internal.hh"', "stream private metadata helper")
    require(meson, "'addons/hdd/kernel-rpc-filesystem-stream.cc',", "Meson filesystem stream owner")
    require(runner, "kernel_rpc_fs_stream_obj", "native filesystem stream object coverage")
    require(runner, 'str(debug_tools / "addons/hdd/kernel-rpc-filesystem-stream.cc")',
            "native filesystem stream compilation")

    # v2.87 test-suite ownership itself: every Python file in tests is explicitly
    # current-version owned. Historical pass-number filenames are forbidden. The
    # six consolidated suites remain present alongside the retained focused tests.
    expected_consolidated_suites = {
        "v287-final-production-audit-golden.py",
        "v287-hdd-krpc-regressions-golden.py",
        "v287-ownership-structure-golden.py",
        "v287-platform-infrastructure-regressions-golden.py",
        "v287-preentry-cheat-regressions-golden.py",
        "v287-ui-runtime-regressions-golden.py",
    }
    python_files = {p.name for p in tests.glob("*.py")}
    bad_python_names = sorted(
        name for name in python_files
        if not (name.startswith("v287-") or name.startswith("v287_"))
    )
    if bad_python_names:
        raise AssertionError(f"unversioned Python test/support filenames: {bad_python_names}")
    if any(re.match(r"v287-pass\d+[-_]", name, re.IGNORECASE) for name in python_files):
        raise AssertionError("historical pass-number wording returned to v2.87 Python filenames")
    golden_python = {p.name for p in tests.glob("*-golden.py")}
    if not all(name.startswith("v287-") for name in golden_python):
        raise AssertionError(f"non-v287 golden Python filename: {sorted(golden_python)}")
    missing_suites = expected_consolidated_suites - golden_python
    if missing_suites:
        raise AssertionError(f"missing consolidated v2.87 suites: {sorted(missing_suites)}")
    top_files = [p for p in tests.iterdir() if p.is_file()]
    if len(top_files) != 56:
        raise AssertionError(f"v2.91 reviewed top-level test/support file count changed: {len(top_files)}")
    for path in sorted(tests.glob("*-golden.py")):
        if "# v2.87 current regression ownership." not in path.read_text(encoding="utf-8").splitlines()[:4]:
            raise AssertionError(f"missing v2.87 ownership marker: {path.name}")
    for path in sorted(tests.glob("*-golden.cpp")):
        if "// v2.87 current regression ownership." not in path.read_text(encoding="utf-8").splitlines()[:4]:
            raise AssertionError(f"missing v2.87 ownership marker: {path.name}")
    if any(p.name == "__pycache__" for p in tests.rglob("__pycache__")):
        raise AssertionError("generated __pycache__ must not be packaged")

    print("PASS: v2.87 current ownership structure replaces stale v2.72-v2.85 phase fingerprints while preserving final split contracts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
