#!/usr/bin/env python3
"""Source-level invariants for x86 Debugger Inject > NOP / Change / CodeCave."""
from __future__ import annotations

import argparse
import pathlib
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    args = parser.parse_args()

    root = pathlib.Path(args.root).resolve()
    dbg = root / "ui/xui/debug-tools"
    memory = (dbg / "memory-tools.cc").read_text(encoding="utf-8")
    bridge = (dbg / "cheat-engine-memory.c").read_text(encoding="utf-8")
    engine = (dbg / "cheat-engine.cc").read_text(encoding="utf-8")
    assembler = (dbg / "x86-cheat-assembler.cc").read_text(encoding="utf-8")

    require(memory, 'ImGui::BeginMenu("Inject")', "Inject submenu")
    require(memory, 'ImGui::MenuItem("NOP"', "NOP action")
    require(memory, 'ImGui::MenuItem("Change"', "Change action")
    require(memory, 'ImGui::MenuItem("CodeCave"', "CodeCave action")
    require(memory, 'xemu_cheat_assemble_x86_32_change_instruction(',
            "Change uses address-aware production assembler helper")
    require(memory, 'm_change_instruction_preview_bytes.resize(m_change_instruction_span, 0x90)',
            "short replacement NOP padding")
    require(memory, 'Use Inject > CodeCave for a longer replacement.',
            "oversize replacement guard")
    require(memory, 'guest bytes no longer match the bytes this window applied',
            "safe Change restore guard")
    require(memory, 'm_instruction_change_history',
            "persistent Change original-history state")
    require(memory, 'Original instruction (remembered)',
            "Change original bytes/ASM display")
    require(memory, 'REVERT TO ORIGINAL',
            "Change reopen-safe revert action")
    require(memory, 'RESET TO CURRENT',
            "Change current-instruction reset action")
    require(memory, 'std::memset(nops, 0x90, row.size)',
            "instruction-sized NOP patch")
    require(memory, 'xemu_cheat_patch_virtual(row.virtual_address, nops, row.size)',
            "transactional NOP write")
    require(memory, 'm_code_cave_overwrite_length < 5u',
            "whole-instruction JMP sizing")
    require(memory, 'cheat_engine_window.InstallDebuggerF0(',
            "CodeCave production Type-F route")
    require(memory, 'cheat_engine_window.RemoveDebuggerF0(',
            "CodeCave explicit restore")

    require(bridge, 'const bool was_running = runstate_is_running()',
            "running-state preservation")
    require(bridge, 'vm_stop(RUN_STATE_PAUSED)', "transactional patch pause")
    require(bridge, 'if (was_running) {\n        vm_start();',
            "transactional patch resume")

    require(engine, 'InstallFHook(kDebuggerFHookOwner, kDebuggerFHookKey',
            "shared Type-F hook installer")
    require(engine, 'ActiveFHookOwnsAddress', "active cave ownership guard")

    require(assembler, 'target.kind == OperandKind::Label || target.kind == OperandKind::Imm',
            "JMP/CALL direct-address targets")
    require(assembler, 'target must be an address or label',
            "Jcc/LOOP direct-address targets")

    print("PASS: x86 Debugger Inject invariants")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
