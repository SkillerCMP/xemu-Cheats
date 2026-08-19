//
// xemu Memory Viewer / Search / x86 Debugger
//
// Copyright (C) 2026 xemu contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#include "memory-tools.hh"
#include "memory-tools-internal.hh"
#include "current-game.hh"
#include "cheat-engine-memory.h"
#include "cheat-engine.hh"
#include "../font-manager.hh"
#include "../misc.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

#include <glib.h>
#include <glib/gstdio.h>

using namespace xemu_memory_tools_internal;

bool MemoryToolsWindow::InjectNop(const XemuCheatDisasmRow &row)
{
    if (row.size == 0 || row.size > sizeof(row.bytes)) {
        m_debug_status = "Inject NOP: selected instruction has an invalid size";
        return false;
    }
    if (cheat_engine_window.ActiveFHookOwnsAddress(row.virtual_address)) {
        m_debug_status =
            "Inject NOP: address is owned by an active Type-F hook/cave; restore it first";
        return false;
    }

    uint8_t nops[15];
    std::memset(nops, 0x90, row.size);
    if (!xemu_cheat_patch_virtual(row.virtual_address, nops, row.size)) {
        m_debug_status = "Inject NOP: failed to patch guest instruction bytes";
        return false;
    }

    char address[16];
    std::snprintf(address, sizeof(address), "%08X", row.virtual_address);
    m_debug_status = std::string("Injected ") + std::to_string(row.size) +
                     " NOP byte" + (row.size == 1 ? "" : "s") +
                     " at " + address;
    m_inject_disasm_refresh_pending = true;
    return true;
}

void MemoryToolsWindow::OpenInstructionChanger(const XemuCheatDisasmRow &row)
{
    if (row.size == 0 || row.size > sizeof(row.bytes) ||
        row.size > sizeof(m_change_instruction_original_bytes)) {
        m_debug_status = "Inject Change: selected instruction has an invalid size";
        return;
    }

    size_t record_index = (size_t)-1;
    for (size_t i = 0; i < m_instruction_change_history.size(); ++i) {
        if (m_instruction_change_history[i].address == row.virtual_address) {
            record_index = i;
            break;
        }
    }

    if (record_index == (size_t)-1) {
        InstructionChangeRecord record;
        record.address = row.virtual_address;
        record.span = row.size;
        std::memcpy(record.original_bytes, row.bytes, row.size);
        record.original_text = row.mnemonic;
        if (row.operands[0] != '\0') {
            if (!record.original_text.empty()) {
                record.original_text += " ";
            }
            record.original_text += row.operands;
        }
        m_instruction_change_history.push_back(std::move(record));
        record_index = m_instruction_change_history.size() - 1;
    }

    InstructionChangeRecord &record = m_instruction_change_history[record_index];
    if (record.span == 0 || record.span > sizeof(m_change_instruction_original_bytes)) {
        m_debug_status = "Inject Change: saved original instruction state is invalid";
        return;
    }

    for (uint32_t i = 0; i < record.span; ++i) {
        if (cheat_engine_window.ActiveFHookOwnsAddress(record.address + i)) {
            m_debug_status =
                "Inject Change: selected instruction overlaps an active Type-F hook/cave; restore it first";
            return;
        }
    }

    uint8_t current[15] = {};
    if (!Read(AddressSpace::Virtual, record.address, current, record.span)) {
        m_debug_status = "Inject Change: failed to read the current guest instruction bytes";
        return;
    }

    bool matches_original =
        std::memcmp(current, record.original_bytes, record.span) == 0;
    bool matches_applied =
        record.active && record.last_applied_bytes.size() == record.span &&
        std::memcmp(current, record.last_applied_bytes.data(), record.span) == 0;

    if (matches_original) {
        record.active = false;
        record.last_applied_bytes.clear();
    } else if (!record.active) {
        // No Change patch is active, so a different live instruction means the
        // old history entry is stale (for example after loading another title
        // at the same virtual address). Re-baseline only when there is no
        // tracked patch to preserve/revert.
        record.span = row.size;
        std::memcpy(record.original_bytes, row.bytes, row.size);
        record.original_text = row.mnemonic;
        if (row.operands[0] != '\0') {
            if (!record.original_text.empty()) {
                record.original_text += " ";
            }
            record.original_text += row.operands;
        }
        std::memcpy(current, row.bytes, row.size);
        matches_original = true;
        matches_applied = false;
    }

    m_change_instruction_record_index = record_index;
    m_change_instruction_address = record.address;
    m_change_instruction_span = record.span;
    std::memcpy(m_change_instruction_original_bytes, record.original_bytes, record.span);
    m_change_instruction_original_text = record.original_text;
    m_change_instruction_current_bytes.assign(current, current + record.span);
    m_change_instruction_current_text = row.mnemonic;
    if (row.operands[0] != '\0') {
        if (!m_change_instruction_current_text.empty()) {
            m_change_instruction_current_text += " ";
        }
        m_change_instruction_current_text += row.operands;
    }
    m_change_instruction_source = m_change_instruction_current_text;
    m_change_instruction_preview_bytes.clear();
    m_change_instruction_applied_bytes = matches_applied ? record.last_applied_bytes
                                                         : std::vector<uint8_t>();
    m_change_instruction_preview_valid = false;
    m_change_instruction_applied = matches_applied;
    m_change_instruction_status.clear();
    if (record.active && !matches_applied) {
        m_change_instruction_status =
            "Change: the saved original is still remembered, but live bytes no longer match the last Change patch. Revert is blocked to avoid overwriting a newer/external patch.";
    } else if (matches_applied) {
        m_change_instruction_status =
            "Change: this address has an active saved patch. REVERT TO ORIGINAL will restore the first captured bytes.";
    }
    m_change_instruction_open = true;
    m_change_instruction_focus_requested = true;
    BuildInstructionChangePreview();
    if (record.active && !matches_applied) {
        m_change_instruction_status +=
            " Saved original retained; live bytes differ from the last tracked Change patch, so APPLY/REVERT are safety-blocked.";
    } else if (matches_applied) {
        m_change_instruction_status +=
            " Saved original is available; REVERT TO ORIGINAL remains available after reopening this window.";
    }
}

bool MemoryToolsWindow::BuildInstructionChangePreview()
{
    m_change_instruction_preview_valid = false;
    m_change_instruction_preview_bytes.clear();

    if (m_change_instruction_span == 0 ||
        m_change_instruction_span > sizeof(m_change_instruction_original_bytes)) {
        m_change_instruction_status = "Change: no valid instruction is selected.";
        return false;
    }
    if (m_change_instruction_source.empty()) {
        m_change_instruction_status = "Change: enter one replacement x86 instruction.";
        return false;
    }

    XemuCheatAsmResult assembled;
    if (!xemu_cheat_assemble_x86_32_change_instruction(
            m_change_instruction_source, m_change_instruction_address,
            m_change_instruction_span, assembled)) {
        m_change_instruction_status = "Change assembler error";
        if (assembled.error_line > 0) {
            m_change_instruction_status += " on line " +
                                           std::to_string(assembled.error_line);
        }
        if (!assembled.error.empty()) {
            m_change_instruction_status += ": " + assembled.error;
        }
        return false;
    }

    if (assembled.uses_preserve || assembled.uses_temp || !assembled.data.empty()) {
        m_change_instruction_status =
            "Change: PRESERVE/T-register/data directives are CodeCave features and cannot replace one instruction in place.";
        return false;
    }
    if (assembled.bytes.empty()) {
        m_change_instruction_status =
            "Change: the replacement did not assemble to an executable instruction.";
        return false;
    }
    if (assembled.bytes.size() > m_change_instruction_span) {
        m_change_instruction_status =
            "Change: replacement is " + std::to_string(assembled.bytes.size()) +
            " bytes but the selected instruction is only " +
            std::to_string(m_change_instruction_span) +
            " bytes. Use Inject > CodeCave for a longer replacement.";
        return false;
    }

    m_change_instruction_preview_bytes = assembled.bytes;
    m_change_instruction_preview_bytes.resize(m_change_instruction_span, 0x90);
    m_change_instruction_preview_valid = true;
    const size_t padding = m_change_instruction_span - assembled.bytes.size();
    m_change_instruction_status =
        "Preview ready: " + std::to_string(assembled.bytes.size()) +
        " replacement byte" + (assembled.bytes.size() == 1 ? "" : "s");
    if (padding != 0) {
        m_change_instruction_status += ", padded with " + std::to_string(padding) +
                                       " NOP byte" + (padding == 1 ? "" : "s");
    }
    m_change_instruction_status += ".";
    return true;
}

bool MemoryToolsWindow::ApplyInstructionChange()
{
    if (!m_change_instruction_preview_valid && !BuildInstructionChangePreview()) {
        return false;
    }

    for (uint32_t i = 0; i < m_change_instruction_span; ++i) {
        if (cheat_engine_window.ActiveFHookOwnsAddress(m_change_instruction_address + i)) {
            m_change_instruction_status =
                "Change: selected instruction overlaps an active Type-F hook/cave; restore it first.";
            return false;
        }
    }

    uint8_t current[15] = {};
    if (!Read(AddressSpace::Virtual, m_change_instruction_address, current,
              m_change_instruction_span)) {
        m_change_instruction_status = "Change: failed to read the current guest instruction bytes.";
        return false;
    }

    const uint8_t *expected = m_change_instruction_original_bytes;
    if (m_change_instruction_applied) {
        if (m_change_instruction_applied_bytes.size() != m_change_instruction_span) {
            m_change_instruction_status = "Change: internal applied-byte state is invalid.";
            return false;
        }
        expected = m_change_instruction_applied_bytes.data();
    }
    if (std::memcmp(current, expected, m_change_instruction_span) != 0) {
        m_change_instruction_status =
            "Change: guest bytes changed since this window captured them. Close/reopen Change on the live instruction before applying.";
        return false;
    }

    if (!xemu_cheat_patch_virtual(m_change_instruction_address,
                                  m_change_instruction_preview_bytes.data(),
                                  m_change_instruction_preview_bytes.size())) {
        m_change_instruction_status = "Change: failed to patch guest instruction bytes.";
        return false;
    }

    m_change_instruction_applied_bytes = m_change_instruction_preview_bytes;
    m_change_instruction_current_bytes = m_change_instruction_preview_bytes;
    m_change_instruction_current_text = m_change_instruction_source;
    m_change_instruction_applied = true;
    if (m_change_instruction_record_index < m_instruction_change_history.size()) {
        InstructionChangeRecord &record =
            m_instruction_change_history[m_change_instruction_record_index];
        if (record.address == m_change_instruction_address &&
            record.span == m_change_instruction_span) {
            record.last_applied_bytes = m_change_instruction_preview_bytes;
            record.active = true;
        }
    }
    char address[16];
    std::snprintf(address, sizeof(address), "%08X", m_change_instruction_address);
    m_change_instruction_status = std::string("Change applied at ") + address + ".";
    m_debug_status = m_change_instruction_status;
    m_inject_disasm_refresh_pending = true;
    return true;
}

bool MemoryToolsWindow::RestoreInstructionChange()
{
    if (!m_change_instruction_applied) {
        m_change_instruction_status = "Change: no tracked Change patch is currently active at this address.";
        return true;
    }
    if (m_change_instruction_applied_bytes.size() != m_change_instruction_span) {
        m_change_instruction_status = "Change: internal applied-byte state is invalid.";
        return false;
    }

    uint8_t current[15] = {};
    if (!Read(AddressSpace::Virtual, m_change_instruction_address, current,
              m_change_instruction_span)) {
        m_change_instruction_status = "Change: failed to read the current guest instruction bytes.";
        return false;
    }
    if (std::memcmp(current, m_change_instruction_applied_bytes.data(),
                    m_change_instruction_span) != 0) {
        m_change_instruction_status =
            "Change: guest bytes no longer match the bytes this window applied; RESTORE was blocked to avoid overwriting a newer patch.";
        return false;
    }

    if (!xemu_cheat_patch_virtual(m_change_instruction_address,
                                  m_change_instruction_original_bytes,
                                  m_change_instruction_span)) {
        m_change_instruction_status = "Change: failed to restore the original instruction bytes.";
        return false;
    }

    m_change_instruction_applied = false;
    m_change_instruction_applied_bytes.clear();
    m_change_instruction_current_bytes.assign(m_change_instruction_original_bytes,
                                              m_change_instruction_original_bytes +
                                                  m_change_instruction_span);
    m_change_instruction_current_text = m_change_instruction_original_text;
    if (m_change_instruction_record_index < m_instruction_change_history.size()) {
        InstructionChangeRecord &record =
            m_instruction_change_history[m_change_instruction_record_index];
        if (record.address == m_change_instruction_address &&
            record.span == m_change_instruction_span) {
            record.last_applied_bytes.clear();
            record.active = false;
        }
    }
    char address[16];
    std::snprintf(address, sizeof(address), "%08X", m_change_instruction_address);
    m_change_instruction_status = std::string("Original instruction restored at ") + address + ".";
    m_debug_status = m_change_instruction_status;
    m_inject_disasm_refresh_pending = true;
    return true;
}

void MemoryToolsWindow::DrawInstructionChanger()
{
    if (!m_change_instruction_open) {
        return;
    }

    if (m_change_instruction_focus_requested) {
        ImGui::SetNextWindowFocus();
        ImGui::SetNextWindowSize(ImVec2(650.0f, 390.0f), ImGuiCond_Appearing);
        m_change_instruction_focus_requested = false;
    }

    if (!ImGui::Begin("x86 Change Instruction", &m_change_instruction_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::Text("Address: %08X", m_change_instruction_address);
    ImGui::SameLine();
    ImGui::TextDisabled("[%u byte%s]", m_change_instruction_span,
                        m_change_instruction_span == 1 ? "" : "s");

    char original_bytes[64];
    format_disassembly_bytes(original_bytes, sizeof(original_bytes),
                             m_change_instruction_original_bytes,
                             m_change_instruction_span);
    ImGui::TextUnformatted("Original instruction (remembered)");
    if (!m_detached_rendering) {
        ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    }
    ImGui::Text("%-38s %s", original_bytes,
                m_change_instruction_original_text.c_str());
    if (!m_detached_rendering) {
        ImGui::PopFont();
    }

    char current_bytes[64] = {};
    if (!m_change_instruction_current_bytes.empty()) {
        format_disassembly_bytes(current_bytes, sizeof(current_bytes),
                                 m_change_instruction_current_bytes.data(),
                                 m_change_instruction_current_bytes.size());
    }
    ImGui::TextUnformatted("Current instruction");
    if (!m_detached_rendering) {
        ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    }
    ImGui::Text("%-38s %s", current_bytes,
                m_change_instruction_current_text.c_str());
    if (!m_detached_rendering) {
        ImGui::PopFont();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Replacement instruction");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##change_instruction_source",
                         &m_change_instruction_source,
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        BuildInstructionChangePreview();
    }
    if (ImGui::IsItemEdited()) {
        BuildInstructionChangePreview();
    }

    ImGui::TextUnformatted("Replacement bytes");
    if (m_change_instruction_preview_valid) {
        char preview_bytes[64];
        format_disassembly_bytes(preview_bytes, sizeof(preview_bytes),
                                 m_change_instruction_preview_bytes.data(),
                                 m_change_instruction_preview_bytes.size());
        if (!m_detached_rendering) {
            ImGui::PushFont(g_font_mgr.m_fixed_width_font);
        }
        ImGui::TextUnformatted(preview_bytes);
        if (!m_detached_rendering) {
            ImGui::PopFont();
        }
    } else {
        ImGui::TextDisabled("No valid replacement preview.");
    }

    ImGui::Separator();
    if (!m_change_instruction_preview_valid) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("APPLY", ImVec2(90.0f, 0.0f))) {
        ApplyInstructionChange();
    }
    if (!m_change_instruction_preview_valid) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (!m_change_instruction_applied) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("REVERT TO ORIGINAL", ImVec2(155.0f, 0.0f))) {
        RestoreInstructionChange();
    }
    if (!m_change_instruction_applied) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (ImGui::Button("RESET TO CURRENT")) {
        m_change_instruction_source = m_change_instruction_current_text;
        BuildInstructionChangePreview();
    }
    ImGui::SameLine();
    if (ImGui::Button("USE ORIGINAL")) {
        m_change_instruction_source = m_change_instruction_original_text;
        BuildInstructionChangePreview();
    }

    if (!m_change_instruction_status.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", m_change_instruction_status.c_str());
    }

    ImGui::End();
}

bool MemoryToolsWindow::BuildCodeCaveTemplate(uint32_t hook_address)
{
    XemuCheatDisasmRow rows[16] = {};
    size_t row_count = 0;
    m_code_cave_original_rows.clear();
    m_code_cave_overwrite_length = 0;

    if (xemu_cheat_disassemble_paired(hook_address,
                                      (int)(sizeof(rows) / sizeof(rows[0])),
                                      rows, sizeof(rows) / sizeof(rows[0]),
                                      &row_count) != XEMU_CHEAT_DISAS_OK ||
        row_count == 0) {
        m_code_cave_status =
            "Code Cave: could not disassemble the selected virtual hook address.";
        return false;
    }

    for (size_t i = 0; i < row_count && m_code_cave_overwrite_length < 5u; ++i) {
        if (rows[i].size == 0 || rows[i].size > sizeof(rows[i].bytes) ||
            m_code_cave_overwrite_length + rows[i].size > 32u) {
            break;
        }
        m_code_cave_original_rows.push_back(rows[i]);
        m_code_cave_overwrite_length += rows[i].size;
    }
    if (m_code_cave_overwrite_length < 5u ||
        m_code_cave_overwrite_length > 32u) {
        m_code_cave_status =
            "Code Cave: could not find a complete 5-32 byte instruction span for the JMP hook.";
        m_code_cave_original_rows.clear();
        m_code_cave_overwrite_length = 0;
        return false;
    }

    char header[64];
    std::snprintf(header, sizeof(header), "$F0000000 %08X\n", hook_address);
    m_code_cave_source = header;
    m_code_cave_source +=
        "$// Auto-copied instructions that the 5-byte JMP will replace.\n"
        "$// Edit/add your cave code, but preserve any original behavior you still need.\n";

    bool copied_control_flow = false;
    for (const XemuCheatDisasmRow &row : m_code_cave_original_rows) {
        DebugFlowInfo flow;
        copied_control_flow |= AnalyzeControlFlow(row, flow);
        m_code_cave_source += "$";
        m_code_cave_source += row.mnemonic;
        if (row.operands[0] != '\0') {
            m_code_cave_source += " ";
            m_code_cave_source += row.operands;
        }
        m_code_cave_source += "\n";
    }
    m_code_cave_source += "$DEADCODE\n";

    m_code_cave_status =
        "Template ready: RUN will replace " +
        std::to_string(m_code_cave_overwrite_length) + " complete instruction bytes and return at ";
    char return_address[16];
    std::snprintf(return_address, sizeof(return_address), "%08X",
                  hook_address + m_code_cave_overwrite_length);
    m_code_cave_status += return_address;
    if (copied_control_flow) {
        m_code_cave_status +=
            ". WARNING: the copied span contains JMP/Jcc/CALL/RET/LOOP control flow; review it before RUN.";
    } else {
        m_code_cave_status += ".";
    }
    return true;
}

void MemoryToolsWindow::OpenCodeCaveBuilder(const XemuCheatDisasmRow &row)
{
    const bool new_hook = m_code_cave_hook_address != row.virtual_address;
    m_code_cave_hook_address = row.virtual_address;
    m_code_cave_builder_open = true;
    m_code_cave_builder_focus_requested = true;

    if (new_hook || m_code_cave_source.empty() ||
        m_code_cave_original_rows.empty()) {
        BuildCodeCaveTemplate(row.virtual_address);
    }
}

void MemoryToolsWindow::DrawCodeCaveBuilder()
{
    if (!m_code_cave_builder_open) {
        return;
    }

    if (m_code_cave_builder_focus_requested) {
        ImGui::SetNextWindowFocus();
        ImGui::SetNextWindowSize(ImVec2(680.0f, 570.0f), ImGuiCond_Appearing);
        m_code_cave_builder_focus_requested = false;
    }

    if (!ImGui::Begin("x86 Code Cave Builder", &m_code_cave_builder_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    CheatEngineWindow::DebuggerF0HookInfo active_info;
    const bool have_debugger_hook =
        cheat_engine_window.GetDebuggerF0HookInfo(active_info) &&
        active_info.installed;
    const bool active_here = have_debugger_hook &&
                             active_info.hook_address == m_code_cave_hook_address;

    ImGui::Text("Hook: %08X", m_code_cave_hook_address);
    ImGui::SameLine();
    if (active_here) {
        ImGui::TextDisabled("[RUNNING -> %08X]", active_info.cave_address);
    } else if (have_debugger_hook) {
        ImGui::TextDisabled("[another debugger cave is running at %08X]",
                            active_info.hook_address);
    } else {
        ImGui::TextDisabled("[not running]");
    }

    ImGui::TextWrapped(
        "RUN uses the normal Type-F0 engine. The selected hook is replaced by a JMP, "
        "whole x86 instructions are overwritten until at least 5 bytes are available, "
        "and DEADCODE jumps back immediately after that span.");

    ImGui::Separator();
    ImGui::TextUnformatted("F0 Source");
    ImGui::InputTextMultiline("##debugger_code_cave_source", &m_code_cave_source,
                              ImVec2(-FLT_MIN, 225.0f),
                              ImGuiInputTextFlags_AllowTabInput);

    ImGui::Separator();
    ImGui::TextUnformatted("Disassembly Preview");
    if (m_code_cave_original_rows.empty() || m_code_cave_overwrite_length < 5u) {
        ImGui::TextDisabled("No valid hook preview is available.");
    } else {
        ImGui::Text("Current instructions replaced by RUN (%u bytes):",
                    m_code_cave_overwrite_length);
        if (!m_detached_rendering) {
            ImGui::PushFont(g_font_mgr.m_fixed_width_font);
        }
        for (const XemuCheatDisasmRow &row : m_code_cave_original_rows) {
            char bytes[64];
            format_disassembly_bytes(bytes, sizeof(bytes), row.bytes,
                                     std::min<size_t>(row.size, sizeof(row.bytes)));
            ImGui::Text("%08X  %-32s %-8s %s", row.virtual_address, bytes,
                        row.mnemonic, row.operands);
        }
        if (!m_detached_rendering) {
            ImGui::PopFont();
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Hook site after RUN:");
        if (active_here) {
            uint8_t hook[32];
            std::memset(hook, 0x90, active_info.overwrite_length);
            hook[0] = 0xE9;
            const uint32_t rel = active_info.cave_address -
                                 (active_info.hook_address + 5u);
            hook[1] = (uint8_t)(rel & 0xFFu);
            hook[2] = (uint8_t)((rel >> 8) & 0xFFu);
            hook[3] = (uint8_t)((rel >> 16) & 0xFFu);
            hook[4] = (uint8_t)((rel >> 24) & 0xFFu);
            char bytes[128];
            format_disassembly_bytes(bytes, sizeof(bytes), hook,
                                     active_info.overwrite_length);
            ImGui::Text("%08X  %-32s JMP %08X", active_info.hook_address,
                        bytes, active_info.cave_address);
            ImGui::Text("Cave returns to %08X", active_info.return_address);
        } else {
            ImGui::Text("%08X  E9 ?? ?? ?? ?? + NOP padding  JMP <allocated cave>",
                        m_code_cave_hook_address);
            ImGui::Text("Cave returns to %08X",
                        m_code_cave_hook_address + m_code_cave_overwrite_length);
        }
    }

    ImGui::Separator();
    const bool owned_by_other_hook =
        cheat_engine_window.ActiveFHookOwnsAddress(m_code_cave_hook_address) &&
        !active_here;
    if (owned_by_other_hook) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("RUN", ImVec2(90.0f, 0.0f))) {
        CheatEngineWindow::DebuggerF0HookInfo installed;
        if (cheat_engine_window.InstallDebuggerF0(
                m_code_cave_hook_address, m_code_cave_source,
                installed, m_code_cave_status)) {
            m_debug_status = m_code_cave_status;
            m_inject_disasm_refresh_pending = true;
        }
    }
    if (owned_by_other_hook) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (!have_debugger_hook) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("RESTORE", ImVec2(90.0f, 0.0f))) {
        if (cheat_engine_window.RemoveDebuggerF0(m_code_cave_status)) {
            m_debug_status = m_code_cave_status;
            m_inject_disasm_refresh_pending = true;
        }
    }
    if (!have_debugger_hook) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (ImGui::Button("RESET TEMPLATE")) {
        BuildCodeCaveTemplate(m_code_cave_hook_address);
    }

    if (!m_code_cave_status.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", m_code_cave_status.c_str());
    }

    ImGui::End();
}

void MemoryToolsWindow::DrawAddressContextMenu(
    AddressSpace space, uint32_t address, ContextOrigin origin,
    const char *copy_value, const XemuCheatDisasmRow *disasm_row,
    bool address_valid, bool have_breakpoint_virtual_override,
    uint32_t breakpoint_virtual_override)
{
    if (!ImGui::BeginPopupContextItem()) {
        return;
    }

    /* A debugger context-click is also a navigation selection. This makes
     * Follow -> Back return to the instruction whose menu the user opened,
     * even when it was not left-click selected first. */
    if (origin == ContextOrigin::Debugger && disasm_row != nullptr) {
        m_have_disasm_selection = true;
        m_selected_disasm_virtual = disasm_row->virtual_address;
        m_selected_disasm_physical_valid = disasm_row->physical_valid != 0;
        if (m_selected_disasm_physical_valid) {
            m_selected_disasm_physical = disasm_row->physical_address;
        }
    }

    if (address_valid) {
        ImGui::TextDisabled("%s %08X",
                            space == AddressSpace::Virtual ? "Virtual" : "Physical",
                            address);
    } else {
        ImGui::TextDisabled("Physical address unmapped");
    }
    ImGui::Separator();

    if (ImGui::BeginMenu("Dump Ram")) {
        if (ImGui::MenuItem("Current Page", nullptr, false, address_valid)) {
            DumpCurrentPage(space, address);
            SetContextStatus(origin, space, m_dump_status);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("DUMP PHYSICAL")) {
            DumpPhysicalRam();
            SetContextStatus(origin, space, m_dump_status);
        }
        if (ImGui::MenuItem("DUMP MAPPED VIRTUAL RAM")) {
            DumpMappedVirtualRam();
            SetContextStatus(origin, space, m_dump_status);
        }
        if (ImGui::MenuItem("DUMP PHYSICAL + DUMP MAPPED VIRTUAL RAM")) {
            DumpFullRam();
            SetContextStatus(origin, space, m_dump_status);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Copy")) {
        if (ImGui::MenuItem("Address", nullptr, false, address_valid)) {
            char text[16];
            std::snprintf(text, sizeof(text), "%08X", address);
            ImGui::SetClipboardText(text);
            SetContextStatus(origin, space,
                             std::string("Copied address ") + text);
        }

        std::string value_text;
        bool have_value = false;
        if (copy_value != nullptr && copy_value[0] != '\0') {
            value_text = copy_value;
            have_value = true;
        } else if (disasm_row != nullptr && disasm_row->size != 0) {
            char byte_text[64] = {};
            size_t used = 0;
            for (size_t i = 0;
                 i < disasm_row->size && i < sizeof(disasm_row->bytes); ++i) {
                const int wrote = std::snprintf(
                    byte_text + used, sizeof(byte_text) - used,
                    i == 0 ? "%02X" : " %02X", disasm_row->bytes[i]);
                if (wrote <= 0 || (size_t)wrote >= sizeof(byte_text) - used) {
                    break;
                }
                used += (size_t)wrote;
            }
            value_text = byte_text;
            have_value = !value_text.empty();
        } else if (address_valid) {
            uint8_t value = 0;
            if (Read(space, address, &value, sizeof(value))) {
                char byte_text[4];
                std::snprintf(byte_text, sizeof(byte_text), "%02X", value);
                value_text = byte_text;
                have_value = true;
            }
        }

        if (ImGui::MenuItem("Value", nullptr, false, have_value)) {
            ImGui::SetClipboardText(value_text.c_str());
            SetContextStatus(origin, space, "Copied value");
        }

        const bool can_disassemble =
            disasm_row != nullptr || address_valid ||
            have_breakpoint_virtual_override;
        if (ImGui::MenuItem("x86 Instructions", nullptr, false,
                            can_disassemble)) {
            if (CopyContextInstruction(space, address, disasm_row,
                                       have_breakpoint_virtual_override,
                                       breakpoint_virtual_override)) {
                SetContextStatus(origin, space, "Copied x86 instruction");
            } else {
                SetContextStatus(origin, space,
                                 "Could not disassemble/copy x86 instruction");
            }
        }
        ImGui::EndMenu();
    }

    if (origin == ContextOrigin::Debugger && disasm_row != nullptr &&
        ImGui::BeginMenu("Inject")) {
        CheatEngineWindow::DebuggerF0HookInfo debugger_hook;
        const bool debugger_hook_here =
            cheat_engine_window.GetDebuggerF0HookInfo(debugger_hook) &&
            debugger_hook.installed &&
            debugger_hook.hook_address == disasm_row->virtual_address;
        const bool hook_owned =
            cheat_engine_window.ActiveFHookOwnsAddress(disasm_row->virtual_address);
        const bool can_inject = disasm_row->size > 0 &&
                                (!hook_owned || debugger_hook_here);

        if (ImGui::MenuItem("NOP", nullptr, false,
                            disasm_row->size > 0 && !hook_owned)) {
            InjectNop(*disasm_row);
        }
        if (ImGui::MenuItem("Change", nullptr, false,
                            disasm_row->size > 0 && !hook_owned)) {
            OpenInstructionChanger(*disasm_row);
        }
        if (ImGui::MenuItem("CodeCave", nullptr, false, can_inject)) {
            OpenCodeCaveBuilder(*disasm_row);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Break Point")) {
        const bool can_break = address_valid || have_breakpoint_virtual_override;
        const char *labels[] = {"Exe", "Read", "Write", "Read/Write"};
        for (int kind = 0; kind < 4; ++kind) {
            if (ImGui::MenuItem(labels[kind], nullptr, false, can_break)) {
                uint32_t virtual_address = 0;
                if (ResolveContextVirtualAddress(
                        space, address, have_breakpoint_virtual_override,
                        breakpoint_virtual_override, virtual_address)) {
                    AddBreakpointByKind(virtual_address, kind);
                }
            }
        }
        ImGui::EndMenu();
    }

    if (origin == ContextOrigin::Debugger && disasm_row != nullptr) {
        DebugFlowInfo flow;
        const bool have_flow = AnalyzeControlFlow(*disasm_row, flow);
        uint32_t resolved_target = 0;
        const bool target_resolved =
            have_flow && ResolveControlFlowTarget(*disasm_row, resolved_target);

        if (ImGui::BeginMenu("Follow")) {
            const bool branch_or_call =
                have_flow && flow.kind != DebugFlowKind::Return;
            if (ImGui::MenuItem("Branch / Call Target", nullptr, false,
                                branch_or_call && target_resolved)) {
                m_debug_nav_pending_action = 1;
                m_debug_nav_pending_address = resolved_target;
                m_debug_nav_pending_status = "Followed branch/call target";
            }
            if (ImGui::MenuItem("Fall Through", nullptr, false,
                                have_flow && flow.fallthrough_valid)) {
                m_debug_nav_pending_action = 1;
                m_debug_nav_pending_address = flow.fallthrough;
                m_debug_nav_pending_status = "Followed fall-through";
            }
            if (ImGui::MenuItem("Return Target", nullptr, false,
                                have_flow &&
                                    flow.kind == DebugFlowKind::Return &&
                                    target_resolved)) {
                m_debug_nav_pending_action = 1;
                m_debug_nav_pending_address = resolved_target;
                m_debug_nav_pending_status = "Followed return target";
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Navigation")) {
            const bool can_back = m_have_debug_nav_history &&
                                  !m_debug_nav_history.empty() &&
                                  m_debug_nav_index > 0;
            const bool can_forward = m_have_debug_nav_history &&
                                     m_debug_nav_index + 1 <
                                         m_debug_nav_history.size();
            if (ImGui::MenuItem("Back", "Left", false, can_back)) {
                m_debug_nav_pending_action = 2;
                m_debug_nav_pending_status = "Debugger navigation: Back";
            }
            if (ImGui::MenuItem("Forward", "Alt+Right", false,
                                can_forward)) {
                m_debug_nav_pending_action = 3;
                m_debug_nav_pending_status = "Debugger navigation: Forward";
            }
            ImGui::EndMenu();
        }
    }

    if (ImGui::BeginMenu("View In")) {
        if (origin == ContextOrigin::Memory ||
            origin == ContextOrigin::Search) {
            if (ImGui::MenuItem("x86 Debugger")) {
                uint32_t virtual_address = 0;
                if (ResolveContextVirtualAddress(
                        space, address, have_breakpoint_virtual_override,
                        breakpoint_virtual_override, virtual_address)) {
                    NavigateDebuggerAddress(virtual_address);
                    m_request_debugger_tab = true;
                }
            }
        }

        if (origin == ContextOrigin::Search ||
            origin == ContextOrigin::Debugger) {
            const bool can_view_memory =
                address_valid || have_breakpoint_virtual_override;
            if (ImGui::MenuItem("Memory", nullptr, false, can_view_memory)) {
                AddressSpace target_space = space;
                uint32_t target_address = address;
                if (!address_valid && have_breakpoint_virtual_override) {
                    target_space = AddressSpace::Virtual;
                    target_address = breakpoint_virtual_override;
                }
                JumpViewerTo(target_space, target_address);
                SelectMemoryByte(target_space, target_address);
                m_request_memory_tab = true;
            }
        }
        ImGui::EndMenu();
    }

    ImGui::EndPopup();
}
