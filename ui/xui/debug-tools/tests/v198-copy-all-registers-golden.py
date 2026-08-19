#!/usr/bin/env python3
"""v1.98 guard: Current Registers COPY ALL clipboard QoL."""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import sys

EXPECTED_OTHER_MEMORYTOOLS_METHODS_COUNT = 101
EXPECTED_OTHER_MEMORYTOOLS_METHODS_SHA256 = (
    "d2c67abc3450c5c97ace59aab81c3749777674aa5463dbbee7a974c4c8609709"
)


def digest_other_methods(implementation: str, tests: pathlib.Path) -> tuple[int, str]:
    sys.path.insert(0, str(tests))
    from source_test_utils import extract_member_functions
    functions = sorted(
        (item for item in extract_member_functions(implementation, "MemoryToolsWindow")
         if item[0] != "DrawRegisters"),
        key=lambda item: item[0],
    )
    records = [f"{name}#{index}\n{body}" for index, (name, body) in enumerate(functions)]
    blob = "\n\0\n".join(records)
    return len(records), hashlib.sha256(blob.encode("utf-8")).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    root = pathlib.Path(parser.parse_args().root).resolve()
    debug = root / "ui/xui/debug-tools"
    tests = debug / "tests"

    sys.path.insert(0, str(tests))
    from source_test_utils import extract_function, read_memory_tools_implementation

    implementation = read_memory_tools_implementation(debug)
    registers = extract_function(
        implementation, "void MemoryToolsWindow::DrawRegisters(")
    helper = (debug / "register-copy-utils.hh").read_text(encoding="utf-8")
    runner = (tests / "run-regression-tests.py").read_text(encoding="utf-8")

    # Last BP reserves matching tab/button-row geometry, then returns before the
    # Current Registers COPY ALL action. This preserves cross-pane register-row
    # alignment while keeping clipboard ownership current/live only.
    if 'ImGui::BeginTable("break_register_tabs_row", 2' not in registers:
        raise AssertionError("Last BP no longer reserves matching COPY ALL row geometry")
    if registers.index("return;") > registers.index('ImGui::Button("COPY ALL"'):
        raise AssertionError("COPY ALL escaped the Current Registers-only path")
    for needle in (
        'ImGui::BeginTable("current_register_tabs_row", 2',
        'ImGui::Button("COPY ALL", ImVec2(-FLT_MIN, 0.0f))',
        "ImGuiCol_Button",
        "ImGuiCol_ButtonHovered",
        "ImGuiCol_ButtonActive",
        '"Register<TAB>Value lines."',
        "xemu_cheat_get_x86_registers(&copy_regs)",
        "xemu_cheat_get_x86_extra_registers(&copy_extra)",
        "m_registers = copy_regs;",
        "m_extra_registers = copy_extra;",
        "BuildAllCurrentRegistersText(copy_regs,",
        "ImGui::SetClipboardText(text.c_str());",
    ):
        if needle not in registers:
            raise AssertionError(f"COPY ALL invariant missing: {needle}")

    # The on-demand extra read must be click-driven only; the established Pass-6
    # General-tab steady-state path remains elsewhere in DrawDebugger/DrawRegisters.
    click = registers.index("if (copy_all_clicked)")
    extra = registers.index("xemu_cheat_get_x86_extra_registers(&copy_extra)")
    if extra < click:
        raise AssertionError("COPY ALL extra-register fetch became steady-state work")

    # Exact clipboard contract: one register/value pair per line with a real TAB
    # separator, matching the existing UI hexadecimal widths/order for all tabs.
    for needle in (
        '"%s\\t%08X\\n"',
        '"%s\\t%016llX\\n"',
        '"%s\\t%04X%016llX\\n"',
        '"XMM%u\\t%08X %08X %08X %08X\\n"',
        '"TOP\\t%u\\n"',
        'append_u32("EAX", regs.eax);',
        'append_u32("GS", regs.gs);',
        'append_u32("MXCSR", extra.mxcsr);',
        "text.pop_back();",
    ):
        if needle not in helper:
            raise AssertionError(f"register clipboard format changed: {needle}")

    if '"register-copy-golden"' not in runner:
        raise AssertionError("native register clipboard formatter test is not packaged")

    # Apart from DrawRegisters, every MemoryTools member remains exactly v1.97.
    count, digest = digest_other_methods(implementation, tests)
    if count != EXPECTED_OTHER_MEMORYTOOLS_METHODS_COUNT or \
            digest != EXPECTED_OTHER_MEMORYTOOLS_METHODS_SHA256:
        raise AssertionError(
            "v1.98 COPY ALL changed a MemoryTools method outside DrawRegisters "
            f"(count={count}, sha256={digest})")

    print("PASS: v1.98 Current Registers COPY ALL guard")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
