#!/usr/bin/env python3
"""Source-level invariants for x86 Debugger Inject > NOP / Change / CodeCave."""
from __future__ import annotations

import argparse
import pathlib
import sys
from source_test_utils import read_memory_tools_implementation


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    args = parser.parse_args()

    root = pathlib.Path(args.root).resolve()
    dbg = root / "ui/xui/debug-tools"
    memory = read_memory_tools_implementation(dbg)
    bridge = (dbg / "cheat-engine-memory.c").read_text(encoding="utf-8")
    bridge_h = (dbg / "cheat-engine-memory.h").read_text(encoding="utf-8")
    external = (dbg / "external-code-memory.c").read_text(encoding="utf-8")
    backend = (dbg / "backend/xemu-dbg.c").read_text(encoding="utf-8")
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
    require(bridge, 'cpu_synchronize_state(cpu);',
            "executable patch current-CR3 synchronization")
    require(bridge, '(void)xemu_dbg_flush_guest_translation();',
            "executable patch accelerator translation synchronization")
    require(bridge, 'xemu_cheat_notify_code_patch();',
            "executable patch generation notification")
    require(bridge_h, 'uint64_t xemu_cheat_code_patch_generation(void);',
            "code patch generation bridge declaration")
    require(memory, 'code_patch_generation != m_code_patch_generation',
            "debugger executable-patch generation tracking")
    require(memory, 'refresh_disassembly = true;',
            "automatic disassembly invalidation")
    if external.count('xemu_cheat_notify_code_patch();') < 2:
        raise AssertionError("external cave write/free paths do not notify debugger refresh")

    require(backend, '#include "exec/tb-flush.h"',
            "TCG translation-block flush API")
    require(backend, 'if (!xemu_dbg_pause_for_accel_update(&was_running))',
            "TCG TB flush serial-context guard")
    require(backend, 'tlb_flush(cpu);',
            "TCG software TLB invalidation")
    require(backend, 'tb_flush__exclusive_or_serial();',
            "TCG translated-code invalidation")
    tcg_flush = backend.find('if (tcg_enabled()) {',
                             backend.find('int xemu_dbg_flush_guest_translation(void)'))
    if tcg_flush < 0:
        raise AssertionError("TCG executable-patch synchronization block missing")
    tcg_end = backend.find('return 1;', tcg_flush)
    if tcg_end < 0 or backend.find('tlb_flush(cpu);', tcg_flush, tcg_end) < 0 or \
            backend.find('tb_flush__exclusive_or_serial();', tcg_flush, tcg_end) < 0:
        raise AssertionError("TCG synchronization must flush both TLB and translated code")

    require(engine, 'InstallFHook(kDebuggerFHookOwner, kDebuggerFHookKey',
            "shared Type-F hook installer")
    require(engine, 'ActiveFHookOwnsAddress', "active cave ownership guard")
    require(engine, 'xemu_cheat_patch_virtual(hook_address, hook,',
            "F0 hook synchronized executable write")
    require(engine, 'xemu_cheat_patch_virtual(state.hook_address,',
            "F0 restore synchronized executable write")

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
