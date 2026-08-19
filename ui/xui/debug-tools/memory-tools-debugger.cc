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
#include "register-copy-utils.hh"
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

void MemoryToolsWindow::LoadDebuggerPreferences()
{
    if (m_debug_preferences_initialized) {
        return;
    }

    const auto &prefs = g_config.display.debug.memory_tools;
    m_disasm_pane_height = std::clamp((float)prefs.disassembly_pane_height,
                                      160.0f, 1200.0f);
    m_disasm_full_page = prefs.disassembly_full_page;
    m_disasm_instruction_count =
        std::clamp(prefs.disassembly_instruction_count, 1, 128);
    m_follow_eip = prefs.follow_eip;
    m_labels_enabled = prefs.labels_enabled;
    m_register_view = std::clamp(prefs.register_view, 0, 3);
    m_register_view_selection_pending = true;
    m_debug_preferences_initialized = true;

    /* Clamp legacy/manually edited config values back to the supported range
     * in memory. xemu's normal exit-time settings save will persist them; no
     * debugger frame performs settings-file I/O. */
    StoreDebuggerPreferences();
}

void MemoryToolsWindow::StoreDebuggerPreferences()
{
    if (!m_debug_preferences_initialized) {
        return;
    }

    /* This function runs at the end of every debugger frame. Keep xemu's
     * in-memory configuration synchronized, but avoid dirtying the same six
     * fields when the UI values are already identical. Disk persistence still
     * belongs exclusively to xemu's normal exit-time settings save. */
    auto &prefs = g_config.display.debug.memory_tools;
    if (prefs.disassembly_pane_height != m_disasm_pane_height) {
        prefs.disassembly_pane_height = m_disasm_pane_height;
    }
    if (prefs.disassembly_full_page != m_disasm_full_page) {
        prefs.disassembly_full_page = m_disasm_full_page;
    }
    if (prefs.disassembly_instruction_count != m_disasm_instruction_count) {
        prefs.disassembly_instruction_count = m_disasm_instruction_count;
    }
    if (prefs.follow_eip != m_follow_eip) {
        prefs.follow_eip = m_follow_eip;
    }
    if (prefs.labels_enabled != m_labels_enabled) {
        prefs.labels_enabled = m_labels_enabled;
    }
    if (prefs.register_view != m_register_view) {
        prefs.register_view = m_register_view;
    }
}

void MemoryToolsWindow::ResetDebuggerPreferences()
{
    /* Display preferences only. Breakpoints/watchpoints, navigation history,
     * Inject state, addresses, guest execution state, and Cheat/F0 state are
     * intentionally untouched. */
    m_disasm_pane_height = 320.0f;
    m_disasm_full_page = true;
    m_disasm_instruction_count = 32;
    m_follow_eip = true;
    m_labels_enabled = true;
    m_register_view = 0;
    m_register_view_selection_pending = true;
    StoreDebuggerPreferences();
}

bool MemoryToolsWindow::RefreshRegisters(XemuCheatX86Registers &regs)
{
    if (!xemu_cheat_get_x86_registers(&regs)) {
        m_debug_status = "Could not read Xbox x86 registers";
        return false;
    }
    return true;
}

bool MemoryToolsWindow::ResolveWatchpointAccessInstruction(
    uint32_t stop_pc, XemuCheatDisasmRow &access_row)
{
    /* x86 hardware data breakpoints report #DB after the accessing
     * instruction. Decode the page containing the stop and resolve the
     * instruction whose end address is exactly the architectural stop EIP.
     * Reuse the debugger page buffer so a watchpoint hit does not allocate a
     * separate ~672 KiB temporary vector. */
    if (m_disassembly_page_scratch.size() != kPageSize) {
        m_disassembly_page_scratch.resize(kPageSize);
    }

    auto resolve_from_page = [&](uint32_t focus) -> bool {
        size_t row_count = 0;
        const int result = xemu_cheat_disassemble_page(
            focus, m_disassembly_page_scratch.data(),
            m_disassembly_page_scratch.size(), &row_count);
        if (result != XEMU_CHEAT_DISAS_OK) {
            return false;
        }

        for (size_t i = row_count; i > 0; --i) {
            const XemuCheatDisasmRow &row = m_disassembly_page_scratch[i - 1];
            const uint64_t row_end =
                (uint64_t)row.virtual_address + std::max<uint8_t>(row.size, 1);
            if (row_end == (uint64_t)stop_pc &&
                std::strcmp(row.mnemonic, "db") != 0) {
                access_row = row;
                return true;
            }
            if (row.virtual_address + 15u < stop_pc) {
                break;
            }
        }
        return false;
    };

    if (resolve_from_page(stop_pc)) {
        return true;
    }

    /* If the stop EIP is exactly at a page boundary, the accessing
     * instruction can live at the end of the previous page. */
    return stop_pc != 0 && resolve_from_page(stop_pc - 1u);
}

void MemoryToolsWindow::RefreshDisassembly()
{
    uint32_t requested_address;
    if (!ParseHexAddress(m_disasm_address_text, requested_address)) {
        m_debug_status = "Invalid disassembly address";
        return;
    }

    /* v0.1.65 exact navigation:
     * First decode from the actual 4 KiB page boundary, not from the user's
     * requested byte. This establishes the instruction boundaries shown by
     * the page view. If the requested byte lands in the middle of an opcode,
     * select that opcode's real start instead of forcing a synthetic decode
     * boundary at the typed address. */
    const uint32_t page_base = requested_address & 0xFFFFF000u;
    /* Full-page mode can decode directly into the displayed vector. Reduced
     * mode still needs a full-page workspace first so an interior byte can be
     * aligned to the containing opcode before the smaller paired decode. */
    std::vector<XemuCheatDisasmRow> &page_rows =
        m_disasm_full_page ? m_disassembly_rows : m_disassembly_page_scratch;
    if (page_rows.size() != kPageSize) {
        page_rows.resize(kPageSize);
    }
    size_t page_row_count = 0;
    int result = xemu_cheat_disassemble_page(
        page_base, page_rows.data(), page_rows.size(), &page_row_count);

    if (result != XEMU_CHEAT_DISAS_OK) {
        m_disassembly_rows.clear();
        m_disassembly_flow_cache.clear();
        m_disassembly_virtual_text.clear();
        m_disassembly_physical_text.clear();
        if (result == XEMU_CHEAT_DISAS_UNMAPPED) {
            m_debug_status = "Virtual address is unmapped";
        } else if (result == XEMU_CHEAT_DISAS_NO_BACKEND) {
            m_debug_status =
                "x86 disassembler backend is not available in this build (Capstone missing)";
        } else {
            m_debug_status = "x86 disassembly failed";
        }
        return;
    }
    uint32_t resolved_address = requested_address;
    const size_t containing_row =
        find_disassembly_row(page_rows.data(), page_row_count, requested_address);
    if (containing_row != (size_t)-1) {
        resolved_address = page_rows[containing_row].virtual_address;
    }

    if (m_disasm_full_page) {
        m_disassembly_rows.resize(page_row_count);
    } else {
        const size_t count =
            (size_t)std::clamp(m_disasm_instruction_count, 1, 128);
        const uint64_t page_end = (uint64_t)page_base + kPageSize;
        bool reused_page_decode = false;

        /* The full-page decode above already established the exact opcode
         * boundary for Count mode. Reuse that decoded slice when every
         * requested instruction has its complete 15-byte x86 decode window
         * inside this page. Near the final 14 bytes, keep the paired path so
         * an instruction that crosses the page boundary is decoded exactly as
         * before rather than appearing as page-end db bytes. */
        if (containing_row != (size_t)-1 &&
            containing_row + count <= page_row_count) {
            const XemuCheatDisasmRow &last =
                page_rows[containing_row + count - 1];
            if ((uint64_t)last.virtual_address + 15u <= page_end) {
                m_disassembly_rows.assign(
                    page_rows.begin() + containing_row,
                    page_rows.begin() + containing_row + count);
                reused_page_decode = true;
            }
        }

        if (!reused_page_decode) {
            size_t row_count = 0;
            m_disassembly_rows.resize(count);
            result = xemu_cheat_disassemble_paired(
                resolved_address, (int)count, m_disassembly_rows.data(),
                m_disassembly_rows.size(), &row_count);
            if (result != XEMU_CHEAT_DISAS_OK) {
                m_disassembly_rows.clear();
                m_disassembly_flow_cache.clear();
                m_disassembly_virtual_text.clear();
                m_disassembly_physical_text.clear();
                if (result == XEMU_CHEAT_DISAS_UNMAPPED) {
                    m_debug_status = "Virtual address is unmapped";
                } else if (result == XEMU_CHEAT_DISAS_NO_BACKEND) {
                    m_debug_status =
                        "x86 disassembler backend is not available in this build (Capstone missing)";
                } else {
                    m_debug_status = "x86 disassembly failed";
                }
                return;
            }
            m_disassembly_rows.resize(row_count);
        }
    }

    RebuildDisassemblyFlowCache();
    RebuildDisassemblyRenderCache();

    m_disasm_address = resolved_address;
    SetHexText(m_disasm_address_text, sizeof(m_disasm_address_text),
               resolved_address);
    m_disasm_scroll_y = 0.0f;
    m_disasm_focus_virtual = resolved_address;
    m_disasm_scroll_to_focus = true;

    /* Keep the typed destination and the highlighted disassembly row unified.
     * This also makes Back/Forward and context-menu navigation land on the
     * exact opcode rather than on an arbitrary byte inside it. */
    m_have_disasm_selection = true;
    m_selected_disasm_virtual = resolved_address;
    m_selected_disasm_physical_valid = false;
    const size_t selected_row = find_disassembly_row(
        m_disassembly_rows.data(), m_disassembly_rows.size(), resolved_address);
    if (selected_row != (size_t)-1 &&
        m_disassembly_rows[selected_row].physical_valid) {
        m_selected_disasm_physical =
            m_disassembly_rows[selected_row].physical_address;
        m_selected_disasm_physical_valid = true;
    }

    if (resolved_address != requested_address) {
        char status[160];
        std::snprintf(status, sizeof(status),
                      "Address %08X is inside an instruction; aligned to opcode start %08X",
                      requested_address, resolved_address);
        m_debug_status = status;
    } else {
        m_debug_status.clear();
    }
}

void MemoryToolsWindow::RebuildDisassemblyFlowCache()
{
    m_disassembly_flow_cache.resize(m_disassembly_rows.size());
    for (size_t i = 0; i < m_disassembly_rows.size(); ++i) {
        DisassemblyFlowCache &cached = m_disassembly_flow_cache[i];
        cached = {};
        if (!AnalyzeControlFlow(m_disassembly_rows[i], cached.flow) ||
            !cached.flow.target_valid) {
            continue;
        }

        auto it = std::lower_bound(
            m_disassembly_rows.begin(), m_disassembly_rows.end(),
            cached.flow.target,
            [](const XemuCheatDisasmRow &row, uint32_t value) {
                return row.virtual_address < value;
            });
        if (it != m_disassembly_rows.end() &&
            it->virtual_address == cached.flow.target) {
            cached.target_index =
                (size_t)std::distance(m_disassembly_rows.begin(), it);
        }
    }
}

void MemoryToolsWindow::RebuildDisassemblyRenderCache()
{
    m_disassembly_virtual_text.resize(m_disassembly_rows.size());
    m_disassembly_physical_text.resize(m_disassembly_rows.size());

    /* Both the decoder rows and label database are ordered by Virtual
     * address. PrimaryLabelAt() performs a binary search, which was repeated
     * once for every decoded row whenever this render cache was rebuilt. Walk
     * the sorted label database forward instead: the first label at an exact
     * address is the same primary label lower_bound() returned previously. */
    const auto &label_database = current_game_manager.Labels();
    auto label_it = label_database.labels.begin();
    const auto label_end = label_database.labels.end();
    if (m_labels_enabled && !m_disassembly_rows.empty()) {
        label_it = std::lower_bound(
            label_it, label_end, m_disassembly_rows.front().virtual_address,
            [](const XemuXbeLabels::Label &label, uint32_t address) {
                return label.virtual_address < address;
            });
    }

    for (size_t i = 0; i < m_disassembly_rows.size(); ++i) {
        const XemuCheatDisasmRow &row = m_disassembly_rows[i];
        char bytes[64];
        format_disassembly_bytes(bytes, sizeof(bytes), row.bytes,
                                 std::min<size_t>(row.size, sizeof(row.bytes)));

        const XemuXbeLabels::Label *label = nullptr;
        if (m_labels_enabled) {
            while (label_it != label_end &&
                   label_it->virtual_address < row.virtual_address) {
                ++label_it;
            }
            if (label_it != label_end &&
                label_it->virtual_address == row.virtual_address) {
                label = &*label_it;
            }
        }

        auto format_line = [&](bool physical, std::string &out) {
            char line[512];
            if (physical) {
                if (row.physical_valid) {
                    std::snprintf(line, sizeof(line),
                                  "%08llX  %-45s %-8s %s",
                                  (unsigned long long)row.physical_address,
                                  bytes, row.mnemonic, row.operands);
                } else {
                    std::snprintf(line, sizeof(line),
                                  "--------  %-45s %-8s %s",
                                  bytes, row.mnemonic, row.operands);
                }
            } else {
                std::snprintf(line, sizeof(line),
                              "%08X  %-45s %-8s %s",
                              row.virtual_address, bytes,
                              row.mnemonic, row.operands);
            }

            if (label != nullptr) {
                const size_t used = std::strlen(line);
                if (used + label->name.size() + 6 < sizeof(line)) {
                    std::snprintf(line + used, sizeof(line) - used,
                                  "  ; %s", label->name.c_str());
                }
            }
            out.assign(line);
        };

        format_line(false, m_disassembly_virtual_text[i]);
        format_line(true, m_disassembly_physical_text[i]);
    }

    m_disassembly_label_generation = current_game_manager.LabelGeneration();
    m_disassembly_cached_labels_enabled = m_labels_enabled;
}

void MemoryToolsWindow::FollowDebuggerAddress(uint32_t address,
                                              bool refresh_disassembly)
{
    m_disasm_address = address;
    SetHexText(m_disasm_address_text, sizeof(m_disasm_address_text), address);

    uint64_t physical = 0;
    xemu_cheat_prepare_virtual_map();
    m_have_disasm_selection = true;
    m_selected_disasm_virtual = address;
    m_selected_disasm_physical_valid =
        xemu_cheat_virtual_to_physical(address, &physical) != 0;
    if (m_selected_disasm_physical_valid) {
        m_selected_disasm_physical = physical;
    }

    if (refresh_disassembly) {
        RefreshDisassembly();
    }
}

void MemoryToolsWindow::NavigateDebuggerAddress(uint32_t address)
{
    const uint32_t current = m_have_disasm_selection
                                 ? m_selected_disasm_virtual
                                 : m_disasm_address;

    if (!m_have_debug_nav_history) {
        m_debug_nav_history.clear();
        m_debug_nav_history.push_back(current);
        m_debug_nav_index = 0;
        m_have_debug_nav_history = true;
    } else if (m_debug_nav_history.empty()) {
        m_debug_nav_history.push_back(current);
        m_debug_nav_index = 0;
    } else {
        /* A single-click changes the active disassembly row without itself
         * navigating. If Follow is then used, that clicked branch instruction
         * must become the Back destination. Do not let an older history node
         * replace the actual branch source. */
        if (m_debug_nav_index >= m_debug_nav_history.size()) {
            m_debug_nav_index = m_debug_nav_history.size() - 1;
        }
        if (m_debug_nav_history[m_debug_nav_index] != current) {
            if (m_debug_nav_index + 1 < m_debug_nav_history.size()) {
                using HistoryDiff = std::vector<uint32_t>::difference_type;
                m_debug_nav_history.erase(
                    m_debug_nav_history.begin() +
                        (HistoryDiff)m_debug_nav_index + 1,
                    m_debug_nav_history.end());
            }
            if (m_debug_nav_history.empty() ||
                m_debug_nav_history.back() != current) {
                m_debug_nav_history.push_back(current);
            }
            m_debug_nav_index = m_debug_nav_history.size() - 1;
        }
    }

    if (m_debug_nav_index + 1 < m_debug_nav_history.size()) {
        using HistoryDiff = std::vector<uint32_t>::difference_type;
        m_debug_nav_history.erase(m_debug_nav_history.begin() +
                                      (HistoryDiff)m_debug_nav_index + 1,
                                  m_debug_nav_history.end());
    }

    if (m_debug_nav_history.empty() || m_debug_nav_history.back() != address) {
        m_debug_nav_history.push_back(address);
    }
    m_debug_nav_index = m_debug_nav_history.size() - 1;
    FollowDebuggerAddress(address, true);
    m_disasm_keyboard_focus_requested = true;
    m_disasm_keyboard_focus_physical = m_disasm_last_keyboard_focus_physical;

    /* RefreshDisassembly may align an interior byte to the containing opcode
     * start. Keep browser-style history on that exact resolved instruction so
     * Back/Forward does not reintroduce the unaligned byte address. */
    if (!m_debug_nav_history.empty() && m_disasm_address != address) {
        m_debug_nav_history.back() = m_disasm_address;
    }
}

bool MemoryToolsWindow::NavigateDebuggerBack()
{
    if (!m_have_debug_nav_history || m_debug_nav_index == 0 ||
        m_debug_nav_history.empty()) {
        return false;
    }

    --m_debug_nav_index;
    FollowDebuggerAddress(m_debug_nav_history[m_debug_nav_index], true);
    m_disasm_keyboard_focus_requested = true;
    m_disasm_keyboard_focus_physical = m_disasm_last_keyboard_focus_physical;
    return true;
}

bool MemoryToolsWindow::NavigateDebuggerForward()
{
    if (!m_have_debug_nav_history || m_debug_nav_history.empty() ||
        m_debug_nav_index + 1 >= m_debug_nav_history.size()) {
        return false;
    }

    ++m_debug_nav_index;
    FollowDebuggerAddress(m_debug_nav_history[m_debug_nav_index], true);
    m_disasm_keyboard_focus_requested = true;
    m_disasm_keyboard_focus_physical = m_disasm_last_keyboard_focus_physical;
    return true;
}

const XemuCheatDisasmRow *MemoryToolsWindow::SelectedDisassemblyRow() const
{
    if (!m_have_disasm_selection) {
        return nullptr;
    }
    auto it = std::lower_bound(
        m_disassembly_rows.begin(), m_disassembly_rows.end(),
        m_selected_disasm_virtual,
        [](const XemuCheatDisasmRow &row, uint32_t value) {
            return row.virtual_address < value;
        });
    return it != m_disassembly_rows.end() &&
                   it->virtual_address == m_selected_disasm_virtual
               ? &*it
               : nullptr;
}

bool MemoryToolsWindow::AnalyzeControlFlow(const XemuCheatDisasmRow &row,
                                           DebugFlowInfo &flow) const
{
    flow = {};
    const char *mnemonic = row.mnemonic;

    if (ascii_equal_case_insensitive(mnemonic, "jmp")) {
        flow.kind = DebugFlowKind::Jump;
    } else if (ascii_equal_case_insensitive(mnemonic, "call")) {
        flow.kind = DebugFlowKind::Call;
        flow.fallthrough_valid = true;
    } else if (ascii_starts_with_case_insensitive(mnemonic, "ret")) {
        flow.kind = DebugFlowKind::Return;
    } else if ((mnemonic[0] != '\0' && ascii_lower((unsigned char)mnemonic[0]) == 'j') ||
               ascii_starts_with_case_insensitive(mnemonic, "loop")) {
        flow.kind = DebugFlowKind::ConditionalJump;
        flow.fallthrough_valid = true;
    } else {
        return false;
    }

    flow.fallthrough = row.virtual_address + std::max<uint8_t>(row.size, 1);

    if (flow.kind == DebugFlowKind::Return || row.operands[0] == '\0') {
        return true;
    }

    char *end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(row.operands, &end, 0);
    if (end != row.operands && errno != ERANGE && parsed <= 0xFFFFFFFFull) {
        while (*end != '\0' && std::isspace((unsigned char)*end)) {
            ++end;
        }
        if (*end == '\0') {
            flow.target_valid = true;
            flow.target = (uint32_t)parsed;
        }
    }
    return true;
}

bool MemoryToolsWindow::RegisterValueByName(const char *name,
                                            uint32_t &value) const
{
    if (name == nullptr || !m_have_registers) {
        return false;
    }
#define MATCH_REG(regname, field) \
    if (g_ascii_strcasecmp(name, regname) == 0) { value = m_registers.field; return true; }
    MATCH_REG("eax", eax)
    MATCH_REG("ebx", ebx)
    MATCH_REG("ecx", ecx)
    MATCH_REG("edx", edx)
    MATCH_REG("esi", esi)
    MATCH_REG("edi", edi)
    MATCH_REG("esp", esp)
    MATCH_REG("ebp", ebp)
    MATCH_REG("eip", eip)
    MATCH_REG("pc", pc)
    MATCH_REG("eflags", eflags)
    MATCH_REG("cr0", cr0)
    MATCH_REG("cr2", cr2)
    MATCH_REG("cr3", cr3)
    MATCH_REG("cr4", cr4)
    MATCH_REG("cs", cs)
    MATCH_REG("ds", ds)
    MATCH_REG("es", es)
    MATCH_REG("fs", fs)
    MATCH_REG("gs", gs)
    MATCH_REG("ss", ss)
#undef MATCH_REG
    return false;
}

bool MemoryToolsWindow::ResolveIndirectControlFlowTarget(
    const char *operand, uint32_t &target) const
{
    if (operand == nullptr || operand[0] == '\0' || !m_have_registers) {
        return false;
    }

    std::string text = operand;
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return (char)std::tolower(ch); });
    text.erase(std::remove_if(text.begin(), text.end(),
                              [](unsigned char ch) { return std::isspace(ch); }),
               text.end());

    /* Register-indirect JMP/CALL, e.g. jmp eax. */
    if (text.find('[') == std::string::npos) {
        return RegisterValueByName(text.c_str(), target);
    }

    const size_t open = text.find('[');
    const size_t close = text.rfind(']');
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        return false;
    }
    /* Segment-base-aware effective addresses require descriptor state that this
     * lightweight navigator intentionally does not guess. */
    if (text.substr(0, open).find(':') != std::string::npos) {
        return false;
    }

    const std::string expr = text.substr(open + 1, close - open - 1);
    int64_t total = 0;
    size_t pos = 0;
    int sign = 1;

    while (pos < expr.size()) {
        if (expr[pos] == '+') {
            sign = 1;
            ++pos;
            continue;
        }
        if (expr[pos] == '-') {
            sign = -1;
            ++pos;
            continue;
        }

        size_t end = pos;
        while (end < expr.size() && expr[end] != '+' && expr[end] != '-') {
            ++end;
        }
        const std::string term = expr.substr(pos, end - pos);
        if (term.empty()) {
            return false;
        }

        uint64_t term_value = 0;
        const size_t star = term.find('*');
        if (star != std::string::npos) {
            uint32_t reg_value = 0;
            if (!RegisterValueByName(term.substr(0, star).c_str(), reg_value)) {
                return false;
            }
            const std::string scale_text = term.substr(star + 1);
            char *scale_end = nullptr;
            const unsigned long scale =
                std::strtoul(scale_text.c_str(), &scale_end, 0);
            if (scale_end == scale_text.c_str() || *scale_end != '\0' ||
                (scale != 1 && scale != 2 && scale != 4 && scale != 8)) {
                return false;
            }
            term_value = (uint64_t)reg_value * scale;
        } else {
            uint32_t reg_value = 0;
            if (RegisterValueByName(term.c_str(), reg_value)) {
                term_value = reg_value;
            } else {
                char *number_end = nullptr;
                errno = 0;
                const unsigned long long number =
                    std::strtoull(term.c_str(), &number_end, 0);
                if (number_end == term.c_str() || *number_end != '\0' ||
                    errno == ERANGE || number > 0xFFFFFFFFull) {
                    return false;
                }
                term_value = number;
            }
        }

        total += sign * (int64_t)term_value;
        sign = 1;
        pos = end;
    }

    const uint32_t effective_address = (uint32_t)total;
    uint8_t pointer_bytes[4];
    if (!Read(AddressSpace::Virtual, effective_address,
              pointer_bytes, sizeof(pointer_bytes))) {
        return false;
    }
    target = load_le(pointer_bytes, sizeof(pointer_bytes));
    return true;
}

bool MemoryToolsWindow::ResolveControlFlowTarget(
    const XemuCheatDisasmRow &row, uint32_t &target) const
{
    DebugFlowInfo flow;
    if (!AnalyzeControlFlow(row, flow)) {
        return false;
    }
    if (flow.target_valid) {
        target = flow.target;
        return true;
    }

    /* Dynamic register/stack state is meaningful only for the instruction the
     * paused CPU is actually about to execute. Do not use Current Registers to
     * guess the target of an arbitrary historical disassembly row. */
    if (!m_have_registers || row.virtual_address != m_registers.pc) {
        return false;
    }

    if (flow.kind == DebugFlowKind::Return) {
        uint8_t return_bytes[4];
        if (!Read(AddressSpace::Virtual, m_registers.esp,
                  return_bytes, sizeof(return_bytes))) {
            return false;
        }
        target = load_le(return_bytes, sizeof(return_bytes));
        return true;
    }
    return ResolveIndirectControlFlowTarget(row.operands, target);
}

void MemoryToolsWindow::HandleDebuggerNavigationKeys()
{
    if (m_debug_nav_key_consumed || ImGui::GetIO().WantTextInput) {
        return;
    }

    const XemuCheatDisasmRow *row = SelectedDisassemblyRow();
    const bool right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);
    const bool left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false);
    const bool shift = ImGui::GetIO().KeyShift;
    const bool alt = ImGui::GetIO().KeyAlt;

    if (alt && right) {
        if (NavigateDebuggerForward()) {
            m_debug_status = "Debugger navigation: Forward";
        }
        m_debug_nav_key_consumed = true;
        return;
    }
    if (left) {
        if (NavigateDebuggerBack()) {
            m_debug_status = "Debugger navigation: Back";
        }
        m_debug_nav_key_consumed = true;
        return;
    }
    if (!right || row == nullptr) {
        return;
    }

    DebugFlowInfo flow;
    if (!AnalyzeControlFlow(*row, flow)) {
        m_debug_status = "Selected instruction has no control-flow target";
        m_debug_nav_key_consumed = true;
        return;
    }

    if (shift) {
        if (flow.fallthrough_valid) {
            NavigateDebuggerAddress(flow.fallthrough);
            m_debug_status = "Followed fall-through";
        } else {
            m_debug_status = "Selected instruction has no fall-through target";
        }
        m_debug_nav_key_consumed = true;
        return;
    }

    uint32_t target = 0;
    if (ResolveControlFlowTarget(*row, target)) {
        NavigateDebuggerAddress(target);
        m_debug_status = "Followed branch/call/return target";
    } else {
        m_debug_status = "Control-flow target is unresolved in the current CPU state";
    }
    m_debug_nav_key_consumed = true;
}

void MemoryToolsWindow::BeginRegisterEdit(const char *name, uint32_t value)
{
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    g_strlcpy(m_register_edit_name, name, sizeof(m_register_edit_name));
    std::snprintf(m_register_edit_text, sizeof(m_register_edit_text), "%08X", value);
    m_register_edit_is_temp = false;
    m_register_edit_temp_address = 0;
    m_register_edit_active = true;
    m_register_edit_focus_requested = true;
}

void MemoryToolsWindow::BeginTempRegisterEdit(const char *name, uint32_t value,
                                              uint32_t storage_address)
{
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    g_strlcpy(m_register_edit_name, name, sizeof(m_register_edit_name));
    std::snprintf(m_register_edit_text, sizeof(m_register_edit_text), "%08X", value);
    m_register_edit_is_temp = true;
    m_register_edit_temp_address = storage_address;
    m_register_edit_active = true;
    m_register_edit_focus_requested = true;
}

bool MemoryToolsWindow::CommitRegisterEdit()
{
    if (!m_register_edit_active) {
        return false;
    }

    uint32_t value = 0;
    if (!ParseHexAddress(m_register_edit_text, value)) {
        m_debug_status = "Invalid register value";
        return false;
    }
    if (m_register_edit_is_temp) {
        if (runstate_is_running()) {
            m_debug_status = "Pause the Xbox before editing F0 temp registers";
            return false;
        }
        uint8_t bytes[4];
        store_le(bytes, sizeof(bytes), value);
        if (!Write(AddressSpace::Virtual, m_register_edit_temp_address,
                   bytes, sizeof(bytes))) {
            m_debug_status = "Could not write F0 temp register storage";
            return false;
        }
        char status[112];
        std::snprintf(status, sizeof(status), "%s changed to %08X",
                      m_register_edit_name, value);
        m_debug_status = status;
        m_register_edit_active = false;
        m_register_edit_focus_requested = false;
        m_register_edit_is_temp = false;
        m_register_edit_temp_address = 0;
        return true;
    }
    if (g_ascii_strcasecmp(m_register_edit_name, "PC") == 0) {
        m_debug_status = "PC is derived from CS:EIP; edit EIP instead";
        return false;
    }
    if (!xemu_cheat_set_x86_register(m_register_edit_name, value)) {
        m_debug_status = runstate_is_running()
                             ? "Pause the Xbox before editing live registers"
                             : "Could not write live x86 register";
        return false;
    }

    XemuCheatX86Registers refreshed = {};
    if (RefreshRegisters(refreshed)) {
        m_registers = refreshed;
        m_have_registers = true;
    }
    char status[96];
    std::snprintf(status, sizeof(status), "%s changed to %08X",
                  m_register_edit_name, value);
    m_debug_status = status;
    m_register_edit_active = false;
    m_register_edit_focus_requested = false;
    m_register_edit_is_temp = false;
    m_register_edit_temp_address = 0;
    return true;
}

bool MemoryToolsWindow::IsEnabledBreakpointAt(uint32_t address) const
{
    return std::any_of(m_breakpoints.begin(), m_breakpoints.end(),
                       [address](const ExecuteBreakpoint &bp) {
                           return bp.enabled && bp.address == address;
                       });
}

bool MemoryToolsWindow::StartDebugStep(DebugStepMode mode)
{
    if (runstate_is_running()) {
        return false;
    }

    m_debug_step_mode = mode;
    m_was_debug_paused = false;

    if (!xemu_cheat_start_single_step()) {
        m_debug_step_mode = DebugStepMode::None;
        m_debug_status = "Could not start x86 single-step";
        return false;
    }
    return true;
}

bool MemoryToolsWindow::ContinueFilteredExecuteBreakpoint(uint32_t address)
{
    const int backend = xemu_cheat_debug_backend();
    m_breakpoint_status = "Breakpoint condition not met; continuing...";

    if (backend == XEMU_CHEAT_DEBUG_BACKEND_TCG) {
        return StartDebugStep(DebugStepMode::ContinuePastBreakpoint);
    }

    if (backend == XEMU_CHEAT_DEBUG_BACKEND_KVM) {
        if (!xemu_cheat_breakpoint_remove(address)) {
            m_breakpoint_status =
                "Breakpoint condition not met, but the KVM execute breakpoint could not be stepped over";
            return false;
        }
        m_resume_breakpoint_restore_pending = true;
        m_resume_breakpoint_restore_address = address;
        if (StartDebugStep(DebugStepMode::ContinuePastBreakpoint)) {
            return true;
        }
        xemu_cheat_breakpoint_insert(address);
        m_resume_breakpoint_restore_pending = false;
        return false;
    }

    /* WHPX performs its proven native breakpoint step-over in whpx_vcpu_run.
     * Any future backend reaching this branch has already delivered the debug
     * stop, so resume normally rather than adding another breakpoint scheme. */
    xemu_cheat_single_step(0);
    m_debug_step_mode = DebugStepMode::None;
    m_was_debug_paused = false;
    vm_start();
    return true;
}

void MemoryToolsWindow::OpenBreakpointConditionEditor(
    const ExecuteBreakpoint &bp)
{
    m_condition_target = BreakpointConditionTarget::Execute;
    m_condition_target_address = bp.address;
    m_condition_target_length = 0;
    m_condition_target_access_flags = 0;
    m_condition_editor_text = bp.condition_text;
    xemu_breakpoint_conditions_parse(m_condition_editor_text,
                                     m_condition_editor_preview,
                                     m_condition_editor_error);
    m_condition_editor_open = true;
    m_condition_editor_focus_requested = true;
}

void MemoryToolsWindow::OpenBreakpointConditionEditor(
    const DataWatchpoint &wp)
{
    m_condition_target = BreakpointConditionTarget::Watchpoint;
    m_condition_target_address = wp.address;
    m_condition_target_length = wp.length;
    m_condition_target_access_flags = wp.access_flags;
    m_condition_editor_text = wp.condition_text;
    xemu_breakpoint_conditions_parse(m_condition_editor_text,
                                     m_condition_editor_preview,
                                     m_condition_editor_error);
    m_condition_editor_open = true;
    m_condition_editor_focus_requested = true;
}

bool MemoryToolsWindow::ApplyBreakpointConditionEditor()
{
    std::vector<XemuBreakpointCondition> parsed;
    std::string error;
    if (!xemu_breakpoint_conditions_parse(m_condition_editor_text,
                                          parsed, error)) {
        m_condition_editor_error = error;
        return false;
    }

    if (m_condition_target == BreakpointConditionTarget::Execute) {
        auto it = std::find_if(
            m_breakpoints.begin(), m_breakpoints.end(),
            [this](const ExecuteBreakpoint &bp) {
                return bp.address == m_condition_target_address;
            });
        if (it == m_breakpoints.end()) {
            m_condition_editor_error = "The execute breakpoint no longer exists.";
            return false;
        }
        it->conditions = parsed;
        it->condition_text = parsed.empty() ? std::string() : m_condition_editor_text;
    } else if (m_condition_target == BreakpointConditionTarget::Watchpoint) {
        auto it = std::find_if(
            m_watchpoints.begin(), m_watchpoints.end(),
            [this](const DataWatchpoint &wp) {
                return wp.address == m_condition_target_address &&
                       wp.length == m_condition_target_length &&
                       wp.access_flags == m_condition_target_access_flags;
            });
        if (it == m_watchpoints.end()) {
            m_condition_editor_error = "The data breakpoint no longer exists.";
            return false;
        }
        it->conditions = parsed;
        it->condition_text = parsed.empty() ? std::string() : m_condition_editor_text;
    } else {
        m_condition_editor_error = "No breakpoint is selected.";
        return false;
    }

    m_condition_editor_preview = parsed;
    m_condition_editor_error.clear();
    char status[128];
    std::snprintf(status, sizeof(status),
                  "%s conditions at %08X (%zu condition%s)",
                  parsed.empty() ? "Cleared" : "Updated",
                  m_condition_target_address, parsed.size(),
                  parsed.size() == 1 ? "" : "s");
    m_breakpoint_status = status;
    return true;
}

void MemoryToolsWindow::ClearBreakpointConditionEditor()
{
    m_condition_editor_text.clear();
    m_condition_editor_preview.clear();
    m_condition_editor_error.clear();
    ApplyBreakpointConditionEditor();
}

void MemoryToolsWindow::DrawBreakpointConditionEditor()
{
    if (!m_condition_editor_open) {
        return;
    }

    if (m_condition_editor_focus_requested) {
        ImGui::SetNextWindowFocus();
        ImGui::SetNextWindowSize(ImVec2(570.0f, 455.0f), ImGuiCond_Appearing);
        m_condition_editor_focus_requested = false;
    }

    if (!ImGui::Begin("Breakpoint Conditions", &m_condition_editor_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    const char *type_text = "Unknown";
    if (m_condition_target == BreakpointConditionTarget::Execute) {
        type_text = "Execute";
    } else if (m_condition_target == BreakpointConditionTarget::Watchpoint) {
        type_text = m_condition_target_access_flags == XEMU_CHEAT_WATCH_READ
                        ? "Read"
                        : (m_condition_target_access_flags == XEMU_CHEAT_WATCH_WRITE
                               ? "Write"
                               : "Read / Write");
    }
    ImGui::Text("Address: %08X", m_condition_target_address);
    ImGui::SameLine();
    ImGui::TextDisabled("Type: %s", type_text);
    ImGui::Separator();

    ImGui::TextUnformatted("Conditions");
    ImGui::TextDisabled("One condition per line. Every non-empty line must be true for the breakpoint to stop.");
    if (ImGui::InputTextMultiline("##breakpoint_condition_text",
                                  &m_condition_editor_text,
                                  ImVec2(-FLT_MIN, 115.0f))) {
        xemu_breakpoint_conditions_parse(m_condition_editor_text,
                                         m_condition_editor_preview,
                                         m_condition_editor_error);
    }

    if (!m_condition_editor_error.empty()) {
        ImGui::TextWrapped("%s", m_condition_editor_error.c_str());
    } else if (m_condition_editor_preview.empty()) {
        ImGui::TextDisabled("No conditions: this breakpoint will stop normally.");
    } else {
        ImGui::TextDisabled("Valid: %zu condition%s (AND)",
                            m_condition_editor_preview.size(),
                            m_condition_editor_preview.size() == 1 ? "" : "s");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Allowed Registers");
    ImGui::TextWrapped(
        "EAX EBX ECX EDX  ESI EDI ESP EBP  EIP PC EFLAGS  CR0 CR2 CR3 CR4  CS DS ES FS GS SS");

    ImGui::TextUnformatted("Allowed Operators (hover for meaning)");
    struct OperatorHelp {
        const char *symbol;
        const char *meaning;
    };
    static const OperatorHelp kOperatorHelp[] = {
        {"==", "Equal to"},
        {"!=", "Not equal to"},
        {"<", "Less than"},
        {"<=", "Less than or equal to"},
        {">", "Greater than"},
        {">=", "Greater than or equal to"},
    };
    for (size_t i = 0; i < IM_ARRAYSIZE(kOperatorHelp); ++i) {
        if (i != 0) {
            ImGui::SameLine();
        }
        ImGui::PushID((int)i);
        ImGui::TextDisabled("%s", kOperatorHelp[i].symbol);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s  =  %s",
                              kOperatorHelp[i].symbol,
                              kOperatorHelp[i].meaning);
        }
        ImGui::PopID();
    }

    ImGui::TextUnformatted("Examples");
    ImGui::TextDisabled("EAX == 12345678");
    ImGui::TextDisabled("ECX != 00000000");
    ImGui::TextDisabled("ESI >= 00001000");
    ImGui::TextDisabled("Values are hexadecimal. 0x12345678 is also accepted.");
    ImGui::TextDisabled("<, <=, >, and >= use unsigned 32-bit register values.");

    ImGui::Separator();
    const bool valid = m_condition_editor_error.empty();
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("APPLY", ImVec2(90.0f, 0.0f))) {
        ApplyBreakpointConditionEditor();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("CLEAR ALL", ImVec2(100.0f, 0.0f))) {
        ClearBreakpointConditionEditor();
    }
    ImGui::SameLine();
    if (ImGui::Button("CANCEL", ImVec2(90.0f, 0.0f))) {
        m_condition_editor_open = false;
    }

    ImGui::End();
}

bool MemoryToolsWindow::AddExecuteBreakpoint(uint32_t address)
{
    auto it = std::find_if(m_breakpoints.begin(), m_breakpoints.end(),
                           [address](const ExecuteBreakpoint &bp) {
                               return bp.address == address;
                           });
    if (it != m_breakpoints.end()) {
        if (!it->enabled) {
            if (!xemu_cheat_breakpoint_insert(address)) {
                m_breakpoint_status = "Could not enable execute breakpoint";
                return false;
            }
            it->enabled = true;
        }
        m_breakpoint_status = "Breakpoint already exists";
        return true;
    }

    if (!xemu_cheat_breakpoint_insert(address)) {
        m_breakpoint_status =
            xemu_cheat_debug_backend() == XEMU_CHEAT_DEBUG_BACKEND_KVM
                ? "Could not insert execute breakpoint (KVM has four shared hardware debug slots)"
                : "Could not insert execute breakpoint";
        return false;
    }
    m_breakpoints.push_back({address, true});

    uint64_t physical = 0;
    char text[128];
    xemu_cheat_prepare_virtual_map();
    if (xemu_cheat_virtual_to_physical(address, &physical)) {
        std::snprintf(text, sizeof(text),
                      "Execute breakpoint added: V %08X -> P %08llX",
                      address, (unsigned long long)physical);
    } else {
        std::snprintf(text, sizeof(text),
                      "Execute breakpoint added at V %08X (currently unmapped)",
                      address);
    }
    m_breakpoint_status = text;
    return true;
}

void MemoryToolsWindow::RemoveExecuteBreakpoint(size_t index)
{
    if (index >= m_breakpoints.size()) {
        return;
    }
    const uint32_t address = m_breakpoints[index].address;
    if (m_breakpoints[index].enabled) {
        xemu_cheat_breakpoint_remove(address);
    }
    if (m_condition_editor_open &&
        m_condition_target == BreakpointConditionTarget::Execute &&
        m_condition_target_address == address) {
        m_condition_editor_open = false;
        m_condition_target = BreakpointConditionTarget::None;
    }
    m_breakpoints.erase(m_breakpoints.begin() + (ptrdiff_t)index);
    m_breakpoint_status = "Breakpoint removed";
}

bool MemoryToolsWindow::AddDataWatchpoint(uint32_t address, uint32_t length,
                                              int access_flags)
{
    auto it = std::find_if(m_watchpoints.begin(), m_watchpoints.end(),
                           [address, length, access_flags](const DataWatchpoint &wp) {
                               return wp.address == address &&
                                      wp.length == length &&
                                      wp.access_flags == access_flags;
                           });
    if (it != m_watchpoints.end()) {
        if (!it->enabled) {
            if (!xemu_cheat_watchpoint_access_supported(access_flags)) {
                m_breakpoint_status =
                    xemu_cheat_debug_backend() == XEMU_CHEAT_DEBUG_BACKEND_KVM &&
                            access_flags == XEMU_CHEAT_WATCH_READ
                        ? "KVM does not provide a reliable Read-only x86 hardware watchpoint; use Read/Write or TCG"
                        : "This watchpoint access type is not supported by the active debugger backend";
                return false;
            }
            if (!xemu_cheat_watchpoint_insert(address, length, access_flags)) {
                const int backend = xemu_cheat_debug_backend();
                m_breakpoint_status =
                    backend == XEMU_CHEAT_DEBUG_BACKEND_WHPX
                        ? "Could not enable data watchpoint (WHPX hardware slots exhausted or range unsupported)"
                        : (backend == XEMU_CHEAT_DEBUG_BACKEND_KVM
                               ? "Could not enable data watchpoint (KVM has four shared hardware debug slots; range may need multiple slots)"
                               : "Could not enable data watchpoint");
                return false;
            }
            it->enabled = true;
        }
        m_breakpoint_status = "Data watchpoint already exists";
        return true;
    }

    if (!xemu_cheat_watchpoint_supported()) {
        m_breakpoint_status = "Read/Write watchpoints are not supported by the active debugger backend";
        return false;
    }
    if (!xemu_cheat_watchpoint_access_supported(access_flags)) {
        m_breakpoint_status =
            xemu_cheat_debug_backend() == XEMU_CHEAT_DEBUG_BACKEND_KVM &&
                    access_flags == XEMU_CHEAT_WATCH_READ
                ? "KVM does not provide a reliable Read-only x86 hardware watchpoint; use Read/Write or run the debugger with TCG"
                : "This watchpoint access type is not supported by the active debugger backend";
        return false;
    }

    if (!xemu_cheat_watchpoint_insert(address, length, access_flags)) {
        const int backend = xemu_cheat_debug_backend();
        m_breakpoint_status =
            backend == XEMU_CHEAT_DEBUG_BACKEND_WHPX
                ? "Could not insert data watchpoint (WHPX has four hardware slots; Read-only can use two per range chunk)"
                : (backend == XEMU_CHEAT_DEBUG_BACKEND_KVM
                       ? "Could not insert data watchpoint (KVM has four shared hardware debug slots; range may need multiple slots)"
                       : "Could not insert data watchpoint");
        return false;
    }

    m_watchpoints.push_back({address, length, access_flags, true});

    uint64_t physical = 0;
    const char *kind = access_flags == XEMU_CHEAT_WATCH_READ
                           ? "Read"
                           : (access_flags == XEMU_CHEAT_WATCH_WRITE ? "Write"
                                                                     : "Read/Write");
    char text[176];
    xemu_cheat_prepare_virtual_map();
    if (xemu_cheat_virtual_to_physical(address, &physical)) {
        std::snprintf(text, sizeof(text),
                      "%s watchpoint added: V %08X -> P %08llX, len %u",
                      kind, address, (unsigned long long)physical, length);
    } else {
        std::snprintf(text, sizeof(text),
                      "%s watchpoint added at V %08X, len %u (currently unmapped)",
                      kind, address, length);
    }
    m_breakpoint_status = text;
    return true;
}

void MemoryToolsWindow::RemoveDataWatchpoint(size_t index)
{
    if (index >= m_watchpoints.size()) {
        return;
    }
    const DataWatchpoint wp = m_watchpoints[index];
    if (wp.enabled) {
        xemu_cheat_watchpoint_remove(wp.address, wp.length, wp.access_flags);
    }
    if (m_condition_editor_open &&
        m_condition_target == BreakpointConditionTarget::Watchpoint &&
        m_condition_target_address == wp.address &&
        m_condition_target_length == wp.length &&
        m_condition_target_access_flags == wp.access_flags) {
        m_condition_editor_open = false;
        m_condition_target = BreakpointConditionTarget::None;
    }
    m_watchpoints.erase(m_watchpoints.begin() + (ptrdiff_t)index);
    m_breakpoint_status = "Data watchpoint removed";
}

bool MemoryToolsWindow::ResolveBreakpointVirtualAddress(
    AddressSpace space, uint32_t address, uint32_t &virtual_address)
{
    if (space == AddressSpace::Virtual) {
        virtual_address = address;
        return true;
    }

    if (!m_memory_map_valid && !RefreshMemoryMap()) {
        m_breakpoint_status =
            "Could not build the Memory Map needed to translate this Physical address";
        return false;
    }

    const size_t index = FindRegionForPhysical(address);
    if (index == (size_t)-1) {
        m_breakpoint_status = "Physical address has no mapped Virtual alias";
        return false;
    }

    const MemoryMapRegion &region = m_memory_map_regions[index];
    const uint64_t resolved =
        region.virtual_start + ((uint64_t)address - region.physical_start);
    if (resolved > 0xFFFFFFFFull) {
        m_breakpoint_status = "Mapped Virtual breakpoint address is out of range";
        return false;
    }

    m_active_map_region = index;
    virtual_address = (uint32_t)resolved;
    return true;
}

bool MemoryToolsWindow::AddBreakpointByKind(uint32_t virtual_address, int kind)
{
    SetHexText(m_breakpoint_address_text,
               sizeof(m_breakpoint_address_text), virtual_address);
    m_breakpoint_kind = std::clamp(kind, 0, 3);

    if (m_breakpoint_kind == 0) {
        return AddExecuteBreakpoint(virtual_address);
    }

    const uint32_t length = (uint32_t)std::max(m_watchpoint_length, 1);
    const int flags =
        m_breakpoint_kind == 1
            ? XEMU_CHEAT_WATCH_READ
            : (m_breakpoint_kind == 2 ? XEMU_CHEAT_WATCH_WRITE
                                      : XEMU_CHEAT_WATCH_ACCESS);
    return AddDataWatchpoint(virtual_address, length, flags);
}

void MemoryToolsWindow::SetContextStatus(ContextOrigin origin,
                                             AddressSpace space,
                                             const std::string &status)
{
    switch (origin) {
    case ContextOrigin::Memory:
        (space == AddressSpace::Virtual ? m_virtual_viewer : m_physical_viewer)
            .status = status;
        break;
    case ContextOrigin::Search:
        m_search_status = status;
        break;
    case ContextOrigin::Debugger:
        m_debug_status = status;
        break;
    }
}

bool MemoryToolsWindow::ResolveContextVirtualAddress(
    AddressSpace space, uint32_t address, bool have_override,
    uint32_t override_address, uint32_t &virtual_address)
{
    if (have_override) {
        virtual_address = override_address;
        return true;
    }
    return ResolveBreakpointVirtualAddress(space, address, virtual_address);
}

bool MemoryToolsWindow::CopyContextInstruction(
    AddressSpace space, uint32_t address,
    const XemuCheatDisasmRow *disasm_row,
    bool have_breakpoint_virtual_override,
    uint32_t breakpoint_virtual_override)
{
    XemuCheatDisasmRow decoded = {};
    const XemuCheatDisasmRow *row = disasm_row;

    if (row == nullptr) {
        uint32_t virtual_address = 0;
        if (!ResolveContextVirtualAddress(space, address,
                                          have_breakpoint_virtual_override,
                                          breakpoint_virtual_override,
                                          virtual_address)) {
            return false;
        }

        size_t row_count = 0;
        if (xemu_cheat_disassemble_paired(virtual_address, 1, &decoded, 1,
                                          &row_count) != XEMU_CHEAT_DISAS_OK ||
            row_count == 0) {
            return false;
        }
        row = &decoded;
    }

    std::string text = row->mnemonic;
    if (row->operands[0] != '\0') {
        if (!text.empty()) {
            text += " ";
        }
        text += row->operands;
    }
    if (text.empty()) {
        return false;
    }

    ImGui::SetClipboardText(text.c_str());
    return true;
}

void MemoryToolsWindow::UpdateBreakpointHitState()
{
    const bool debug_paused = runstate_get() == RUN_STATE_DEBUG;
    if (!debug_paused) {
        if (runstate_is_running()) {
            m_was_debug_paused = false;
        }
        return;
    }

    /* Once a debug stop has been processed, DrawDebugger() already refreshes
     * the live register panes on its 100 ms cadence. Avoid a second backend
     * register fetch on every rendered frame while the VM remains stopped. */
    if (m_was_debug_paused) {
        return;
    }

    XemuCheatX86Registers regs = {};
    if (!RefreshRegisters(regs)) {
        return;
    }
    m_registers = regs;
    m_have_registers = true;

    /* A debugger-requested single step always stops with RUN_STATE_DEBUG.
     * For a normal Step Into we leave the VM paused at the new EIP. For
     * Continue from a breakpoint this stop is intentionally invisible: the
     * breakpoint instruction has now executed once, so disable stepping and
     * immediately continue normal execution. */
    if (m_debug_step_mode != DebugStepMode::None) {
        const DebugStepMode completed_mode = m_debug_step_mode;
        m_debug_step_mode = DebugStepMode::None;
        xemu_cheat_single_step(0);

        if (m_resume_breakpoint_restore_pending) {
            const uint32_t restore_address = m_resume_breakpoint_restore_address;
            m_resume_breakpoint_restore_pending = false;
            if (!xemu_cheat_breakpoint_insert(restore_address)) {
                char status[160];
                std::snprintf(status, sizeof(status),
                              "KVM step completed, but execute breakpoint %08X could not be restored",
                              restore_address);
                m_breakpoint_status = status;
                m_was_debug_paused = true;
                return;
            }
        }

        if (completed_mode == DebugStepMode::ContinuePastBreakpoint) {
            m_breakpoint_status = "Continued cleanly past execute breakpoint";
            m_was_debug_paused = false;
            vm_start();
            return;
        }

        uint64_t physical = 0;
        const bool physical_valid =
            xemu_cheat_virtual_to_physical(regs.pc, &physical) != 0;
        m_have_disasm_selection = true;
        m_selected_disasm_virtual = regs.pc;
        m_selected_disasm_physical_valid = physical_valid;
        if (physical_valid) {
            m_selected_disasm_physical = physical;
        }

        char status[128];
        if (physical_valid) {
            std::snprintf(status, sizeof(status),
                          "Step complete: V %08X -> P %08llX",
                          regs.pc, (unsigned long long)physical);
        } else {
            std::snprintf(status, sizeof(status),
                          "Step complete at V %08X (physical unmapped)",
                          regs.pc);
        }
        m_breakpoint_status = status;

        if (m_follow_eip) {
            FollowDebuggerAddress(regs.pc, true);
        }
        m_was_debug_paused = true;
        return;
    }

    /* QEMU/TCG/WHPX records the exact data watchpoint and byte address that
     * triggered the debug stop. x86 hardware data breakpoints report #DB
     * after the memory-access instruction has completed, so regs.pc is the
     * architectural stop EIP while hit_address is the watched data address. */
    XemuCheatWatchpointHit watch_hit = {};
    const int watch_hit_result = xemu_cheat_watchpoint_get_hit(&watch_hit);
    if (watch_hit_result == XEMU_CHEAT_WATCH_HIT_REPORTED) {
        auto condition_watch = std::find_if(
            m_watchpoints.begin(), m_watchpoints.end(),
            [&watch_hit](const DataWatchpoint &wp) {
                return wp.enabled && wp.address == watch_hit.watch_address &&
                       wp.length == watch_hit.length &&
                       (wp.access_flags & watch_hit.access_flags) != 0;
            });
        if (condition_watch != m_watchpoints.end() &&
            !xemu_breakpoint_conditions_evaluate(condition_watch->conditions, regs)) {
            m_breakpoint_status =
                "Data breakpoint condition not met; continuing...";
            m_was_debug_paused = false;
            vm_start();
            return;
        }

        m_break_registers = regs;
        m_have_break_registers = true;
        CaptureBreakpointExtraRegisters();
        m_last_break_pc = regs.pc;
        m_have_break_highlight = true;
        m_last_break_highlight_pc = regs.pc;
        m_last_break_highlight_is_access = false;
        m_last_break_physical_valid =
            xemu_cheat_virtual_to_physical(regs.pc,
                                           &m_last_break_physical) != 0;

        XemuCheatDisasmRow access_row = {};
        const bool access_resolved =
            ResolveWatchpointAccessInstruction(regs.pc, access_row);
        if (access_resolved) {
            m_last_break_highlight_pc = access_row.virtual_address;
            m_last_break_highlight_is_access = true;
        }

        m_have_disasm_selection = true;
        m_selected_disasm_virtual = m_last_break_highlight_pc;
        if (access_resolved) {
            m_selected_disasm_physical_valid = access_row.physical_valid != 0;
            if (m_selected_disasm_physical_valid) {
                m_selected_disasm_physical = access_row.physical_address;
            }
        } else {
            m_selected_disasm_physical_valid = m_last_break_physical_valid;
            if (m_last_break_physical_valid) {
                m_selected_disasm_physical = m_last_break_physical;
            }
        }

        uint64_t watched_physical = 0;
        const bool watched_physical_valid =
            xemu_cheat_virtual_to_physical(watch_hit.hit_address,
                                           &watched_physical) != 0;
        const char *kind =
            watch_hit.access_flags == XEMU_CHEAT_WATCH_READ
                ? "READ"
                : (watch_hit.access_flags == XEMU_CHEAT_WATCH_WRITE
                       ? "WRITE"
                       : "READ/WRITE");

        char status[320];
        if (access_resolved) {
            if (watched_physical_valid && access_row.physical_valid &&
                m_last_break_physical_valid) {
                std::snprintf(status, sizeof(status),
                              "%s watchpoint hit: data V %08X -> P %08llX; access V %08X -> P %08llX; current EIP V %08X -> P %08llX",
                              kind, watch_hit.hit_address,
                              (unsigned long long)watched_physical,
                              access_row.virtual_address,
                              (unsigned long long)access_row.physical_address,
                              regs.pc,
                              (unsigned long long)m_last_break_physical);
            } else {
                std::snprintf(status, sizeof(status),
                              "%s watchpoint hit: data V %08X; access V %08X; current EIP V %08X",
                              kind, watch_hit.hit_address,
                              access_row.virtual_address, regs.pc);
            }
        } else if (watched_physical_valid && m_last_break_physical_valid) {
            std::snprintf(status, sizeof(status),
                          "%s watchpoint hit: data V %08X -> P %08llX; current EIP V %08X -> P %08llX (access instruction unresolved)",
                          kind, watch_hit.hit_address,
                          (unsigned long long)watched_physical, regs.pc,
                          (unsigned long long)m_last_break_physical);
        } else {
            std::snprintf(status, sizeof(status),
                          "%s watchpoint hit: data V %08X; current EIP V %08X (access instruction unresolved)",
                          kind, watch_hit.hit_address, regs.pc);
        }
        m_breakpoint_status = status;

        if (m_follow_eip) {
            m_disasm_full_page = true;
            FollowDebuggerAddress(m_last_break_highlight_pc, true);
        }
        m_was_debug_paused = true;
        return;
    }

    auto it = std::find_if(m_breakpoints.begin(), m_breakpoints.end(),
                           [&regs](const ExecuteBreakpoint &bp) {
                               return bp.enabled && bp.address == regs.pc;
                           });
    if (it != m_breakpoints.end()) {
        if (!xemu_breakpoint_conditions_evaluate(it->conditions, regs)) {
            if (!ContinueFilteredExecuteBreakpoint(regs.pc)) {
                m_was_debug_paused = true;
            }
            return;
        }

        m_break_registers = regs;
        m_have_break_registers = true;
        CaptureBreakpointExtraRegisters();
        m_last_break_pc = regs.pc;
        m_have_break_highlight = true;
        m_last_break_highlight_pc = regs.pc;
        m_last_break_highlight_is_access = false;
        m_last_break_physical_valid =
            xemu_cheat_virtual_to_physical(regs.pc,
                                           &m_last_break_physical) != 0;

        m_have_disasm_selection = true;
        m_selected_disasm_virtual = regs.pc;
        m_selected_disasm_physical_valid = m_last_break_physical_valid;
        if (m_last_break_physical_valid) {
            m_selected_disasm_physical = m_last_break_physical;
        }

        char status[144];
        if (m_last_break_physical_valid) {
            std::snprintf(status, sizeof(status),
                          "Breakpoint hit: V %08X -> P %08llX",
                          regs.pc,
                          (unsigned long long)m_last_break_physical);
        } else {
            std::snprintf(status, sizeof(status),
                          "Breakpoint hit at V %08X (physical unmapped)",
                          regs.pc);
        }
        m_breakpoint_status = status;

        if (m_follow_eip) {
            m_disasm_full_page = true;
            FollowDebuggerAddress(regs.pc, true);
        }
    }

    m_was_debug_paused = true;
}

void MemoryToolsWindow::DrawGeneralRegisterTable(
    const XemuCheatX86Registers &r, bool breakpoint_snapshot)
{
    const char *table_id = breakpoint_snapshot ? "registers_break" : "registers_current";
    if (ImGui::BeginTable(table_id, 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingFixedFit)) {
        auto reg = [&](const char *name, uint32_t value) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            ImGui::TableNextColumn();
            ImGui::PushID(name);

            const bool is_pc = g_ascii_strcasecmp(name, "PC") == 0;
            const bool can_edit = !breakpoint_snapshot && !is_pc &&
                                  !runstate_is_running();
            const bool editing = !breakpoint_snapshot &&
                                 m_register_edit_active &&
                                 g_ascii_strcasecmp(m_register_edit_name,
                                                    name) == 0;

            if (editing) {
                ImGui::SetNextItemWidth(90.0f);
                if (m_register_edit_focus_requested) {
                    ImGui::SetKeyboardFocusHere();
                    m_register_edit_focus_requested = false;
                }
                const bool enter = ImGui::InputText(
                    "##register_edit", m_register_edit_text,
                    sizeof(m_register_edit_text),
                    ImGuiInputTextFlags_CharsHexadecimal |
                        ImGuiInputTextFlags_EnterReturnsTrue |
                        ImGuiInputTextFlags_AutoSelectAll);
                if (enter) {
                    CommitRegisterEdit();
                } else if (ImGui::IsItemActive() &&
                           ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    m_register_edit_active = false;
                    m_register_edit_focus_requested = false;
                    m_register_edit_is_temp = false;
                    m_register_edit_temp_address = 0;
                    m_debug_status = "Register edit cancelled";
                }
            } else {
                char value_text[16];
                std::snprintf(value_text, sizeof(value_text), "%08X", value);
                if (ImGui::Selectable(value_text, false,
                                      ImGuiSelectableFlags_AllowDoubleClick,
                                      ImVec2(82.0f, 0.0f)) &&
                    can_edit &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    BeginRegisterEdit(name, value);
                }

                if (!breakpoint_snapshot && ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Edit Register Value", nullptr, false,
                                        can_edit)) {
                        BeginRegisterEdit(name, value);
                    }
                    if (ImGui::BeginMenu("Copy")) {
                        if (ImGui::MenuItem("Register")) {
                            ImGui::SetClipboardText(name);
                            m_debug_status = std::string("Copied register ") + name;
                        }
                        if (ImGui::MenuItem("Value")) {
                            ImGui::SetClipboardText(value_text);
                            m_debug_status = std::string("Copied ") + name +
                                             " value " + value_text;
                        }
                        ImGui::EndMenu();
                    }
                    if (ImGui::BeginMenu("View In")) {
                        if (ImGui::MenuItem("Memory")) {
                            JumpViewerTo(AddressSpace::Virtual, value);
                            SelectMemoryByte(AddressSpace::Virtual, value);
                            m_request_memory_tab = true;
                        }
                        if (ImGui::MenuItem("x86 Debugger")) {
                            NavigateDebuggerAddress(value);
                            m_request_debugger_tab = true;
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::EndPopup();
                }

                if (!breakpoint_snapshot && is_pc && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("PC is derived from CS:EIP. Edit EIP to change execution position.");
                }
            }
            ImGui::PopID();
        };

        reg("EAX", r.eax); reg("EBX", r.ebx);
        reg("ECX", r.ecx); reg("EDX", r.edx);
        reg("ESI", r.esi); reg("EDI", r.edi);
        reg("ESP", r.esp); reg("EBP", r.ebp);
        reg("EIP", r.eip); reg("PC", r.pc);
        reg("EFLAGS", r.eflags); reg("CR3", r.cr3);
        reg("CR0", r.cr0); reg("CR2", r.cr2);
        reg("CR4", r.cr4); reg("CS", r.cs);
        reg("DS", r.ds); reg("SS", r.ss);
        reg("ES", r.es); reg("FS", r.fs);
        reg("GS", r.gs);
        ImGui::EndTable();
    }
}

void MemoryToolsWindow::DrawExtraRegisterTable(
    const XemuCheatX86ExtraRegisters &extra, bool have_extra, int view,
    bool breakpoint_snapshot)
{
    const char *suffix = breakpoint_snapshot ? "break" : "current";

    if (view == 1) {
        if (!have_extra) {
            ImGui::TextDisabled(breakpoint_snapshot
                                    ? "No x87/FPU snapshot is available for the last breakpoint."
                                    : "Waiting for floating-point register state...");
            return;
        }
        char table_id[48];
        std::snprintf(table_id, sizeof(table_id), "registers_x87_%s", suffix);
        if (ImGui::BeginTable(table_id, 2,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingFixedFit)) {
            for (unsigned i = 0; i < 8; ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("ST%u", i);
                ImGui::TableNextColumn();
                ImGui::Text("%04X%016llX",
                            (unsigned)extra.st_high[i],
                            (unsigned long long)extra.st_low[i]);
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("FCTRL");
            ImGui::TableNextColumn(); ImGui::Text("%08X", extra.fctrl);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("FSTAT");
            ImGui::TableNextColumn(); ImGui::Text("%08X", extra.fstat);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("TOP");
            ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)extra.fp_top);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("FOP");
            ImGui::TableNextColumn(); ImGui::Text("%08X", extra.fop);
            ImGui::EndTable();
        }
        ImGui::TextDisabled("ST0-ST7 are shown as raw 80-bit hexadecimal values.");
        return;
    }

    if (view == 2) {
        if (!have_extra) {
            ImGui::TextDisabled(breakpoint_snapshot
                                    ? "No MMX snapshot is available for the last breakpoint."
                                    : "Waiting for MMX register state...");
            return;
        }
        char table_id[48];
        std::snprintf(table_id, sizeof(table_id), "registers_mmx_%s", suffix);
        if (ImGui::BeginTable(table_id, 2,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingFixedFit)) {
            for (unsigned i = 0; i < 8; ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("MM%u", i);
                ImGui::TableNextColumn();
                ImGui::Text("%016llX", (unsigned long long)extra.mmx[i]);
            }
            ImGui::EndTable();
        }
        return;
    }

    if (view == 3) {
        if (!have_extra) {
            ImGui::TextDisabled(breakpoint_snapshot
                                    ? "No SSE snapshot is available for the last breakpoint."
                                    : "Waiting for SSE register state...");
            return;
        }
        char table_id[48];
        std::snprintf(table_id, sizeof(table_id), "registers_sse_%s", suffix);
        if (ImGui::BeginTable(table_id, 2,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingFixedFit)) {
            for (unsigned i = 0; i < 8; ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("XMM%u", i);
                ImGui::TableNextColumn();
                ImGui::Text("%08X %08X %08X %08X",
                            extra.xmm[i][3],
                            extra.xmm[i][2],
                            extra.xmm[i][1],
                            extra.xmm[i][0]);
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("MXCSR");
            ImGui::TableNextColumn(); ImGui::Text("%08X", extra.mxcsr);
            ImGui::EndTable();
        }
    }
}

void MemoryToolsWindow::CaptureBreakpointExtraRegisters()
{
    XemuCheatX86ExtraRegisters extra = {};
    if (xemu_cheat_get_x86_extra_registers(&extra)) {
        m_break_extra_registers = extra;
        m_have_break_extra_registers = true;
    } else {
        m_break_extra_registers = {};
        m_have_break_extra_registers = false;
    }
}

void MemoryToolsWindow::DrawRegisters(const XemuCheatX86Registers &r,
                                      bool breakpoint_snapshot)
{
    ImGui::TextUnformatted(breakpoint_snapshot
                               ? "Last BP"
                               : "Current Registers");
    if (breakpoint_snapshot && m_last_break_pc != 0) {
        ImGui::SameLine();
        if (m_last_break_physical_valid) {
            ImGui::TextDisabled("[V %08X -> P %08llX]",
                                m_last_break_pc,
                                (unsigned long long)m_last_break_physical);
        } else {
            ImGui::TextDisabled("[V %08X -> P unmapped]", m_last_break_pc);
        }
    } else if (!breakpoint_snapshot) {
        ImGui::SameLine();
        ImGui::TextDisabled(runstate_is_running()
                                ? "[live - pause/break to edit]"
                                : "[live - editable]");
    }

    if (breakpoint_snapshot) {
        /* The tab controls intentionally live only above Current Registers,
         * but Last BP reserves the identical tab/COPY ALL row geometry. This
         * keeps General EAX, x87 ST0, MM0 and XMM0 horizontally aligned with
         * the live side. */
        if (ImGui::BeginTable("break_register_tabs_row", 2,
                              ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_NoSavedSettings |
                                  ImGuiTableFlags_NoPadOuterX)) {
            ImGui::TableSetupColumn("Tabs", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Copy", ImGuiTableColumnFlags_WidthFixed,
                                    74.0f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Dummy(ImVec2(0.0f, ImGui::GetFrameHeight()));
            ImGui::TableSetColumnIndex(1);
            ImGui::Dummy(ImVec2(74.0f, ImGui::GetFrameHeight()));
            ImGui::EndTable();
        }
        if (m_register_view == 0) {
            DrawGeneralRegisterTable(r, true);
        } else {
            DrawExtraRegisterTable(m_break_extra_registers,
                                   m_have_break_extra_registers,
                                   m_register_view, true);
        }
        return;
    }

    ImGuiContext *const current_context = ImGui::GetCurrentContext();
    if (m_register_view_context != current_context) {
        m_register_view_context = current_context;
        m_register_view_selection_pending = true;
    }

    const int requested_register_view = m_register_view;
    const int previous_register_view = m_register_view;
    const bool select_register_view = m_register_view_selection_pending;

    auto refresh_extra_on_general_exit = [&](int new_view) {
        if (previous_register_view != 0 || new_view == 0) {
            return;
        }
        XemuCheatX86ExtraRegisters extra_regs = {};
        if (xemu_cheat_get_x86_extra_registers(&extra_regs)) {
            m_extra_registers = extra_regs;
            m_have_extra_registers = true;
        }
    };

    bool copy_all_clicked = false;
    if (ImGui::BeginTable("current_register_tabs_row", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_NoSavedSettings |
                              ImGuiTableFlags_NoPadOuterX)) {
        ImGui::TableSetupColumn("Tabs", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Copy", ImGuiTableColumnFlags_WidthFixed,
                                74.0f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);

        if (ImGui::BeginTabBar("current_register_tabs")) {
            if (ImGui::BeginTabItem(
                    "General", nullptr,
                    select_register_view && requested_register_view == 0
                        ? ImGuiTabItemFlags_SetSelected
                        : ImGuiTabItemFlags_None)) {
                m_register_view = 0;
                DrawGeneralRegisterTable(r, false);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(
                    "x87 / FPU", nullptr,
                    select_register_view && requested_register_view == 1
                        ? ImGuiTabItemFlags_SetSelected
                        : ImGuiTabItemFlags_None)) {
                m_register_view = 1;
                refresh_extra_on_general_exit(m_register_view);
                DrawExtraRegisterTable(m_extra_registers,
                                       m_have_extra_registers,
                                       m_register_view, false);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(
                    "MMX", nullptr,
                    select_register_view && requested_register_view == 2
                        ? ImGuiTabItemFlags_SetSelected
                        : ImGuiTabItemFlags_None)) {
                m_register_view = 2;
                refresh_extra_on_general_exit(m_register_view);
                DrawExtraRegisterTable(m_extra_registers,
                                       m_have_extra_registers,
                                       m_register_view, false);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(
                    "SSE", nullptr,
                    select_register_view && requested_register_view == 3
                        ? ImGuiTabItemFlags_SetSelected
                        : ImGuiTabItemFlags_None)) {
                m_register_view = 3;
                refresh_extra_on_general_exit(m_register_view);
                DrawExtraRegisterTable(m_extra_registers,
                                       m_have_extra_registers,
                                       m_register_view, false);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
            m_register_view_selection_pending = false;
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.16f, 0.38f, 0.68f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.20f, 0.47f, 0.82f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.12f, 0.31f, 0.58f, 1.00f));
        copy_all_clicked = ImGui::Button("COPY ALL", ImVec2(-FLT_MIN, 0.0f));
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Copy General, x87/FPU, MMX and SSE current registers as "
                "Register<TAB>Value lines.");
        }
        ImGui::EndTable();
    }

    if (copy_all_clicked) {
        XemuCheatX86Registers copy_regs = {};
        XemuCheatX86ExtraRegisters copy_extra = {};
        if (!xemu_cheat_get_x86_registers(&copy_regs)) {
            m_debug_status = "COPY ALL failed: could not read current registers";
        } else if (!xemu_cheat_get_x86_extra_registers(&copy_extra)) {
            m_debug_status =
                "COPY ALL failed: could not read x87/MMX/SSE registers";
        } else {
            m_registers = copy_regs;
            m_have_registers = true;
            m_extra_registers = copy_extra;
            m_have_extra_registers = true;
            const std::string text =
                xemu_register_copy::BuildAllCurrentRegistersText(copy_regs,
                                                                 copy_extra);
            ImGui::SetClipboardText(text.c_str());
            m_debug_status = "Copied all current registers to clipboard";
        }
    }
}

void MemoryToolsWindow::DrawF0TempRegisters()
{
    /* Hook/T-bank metadata changes only when Type-F lifecycle state changes.
     * CheatEngineWindow owns a sorted cache, so normal debugger frames borrow
     * it directly instead of rebuilding/copying/sorting names every draw. */
    const auto &banks = cheat_engine_window.GetActiveF0TempBanks();
    if (banks.empty()) {
        m_selected_f0_temp_hook = 0;
        if (m_register_edit_active && m_register_edit_is_temp) {
            m_register_edit_active = false;
            m_register_edit_focus_requested = false;
            m_register_edit_is_temp = false;
            m_register_edit_temp_address = 0;
        }
        return;
    }

    if (m_register_edit_active && m_register_edit_is_temp) {
        bool edit_bank_still_active = false;
        for (const auto &bank : banks) {
            if (m_register_edit_temp_address >= bank.temp_address &&
                m_register_edit_temp_address < bank.temp_address + 32u) {
                edit_bank_still_active = true;
                break;
            }
        }
        if (!edit_bank_still_active) {
            m_register_edit_active = false;
            m_register_edit_focus_requested = false;
            m_register_edit_is_temp = false;
            m_register_edit_temp_address = 0;
        }
    }

    /* While paused inside a Type-F cave, automatically select that cave's
     * private T bank. Outside a cave, retain the user's selected active bank. */
    if (m_have_registers) {
        for (const auto &bank : banks) {
            const uint64_t start = bank.cave_address;
            const uint64_t end = start + bank.cave_size;
            if ((uint64_t)m_registers.pc >= start &&
                (uint64_t)m_registers.pc < end) {
                m_selected_f0_temp_hook = bank.hook_address;
                break;
            }
        }
    }

    size_t selected = 0;
    bool found_selected = false;
    for (size_t i = 0; i < banks.size(); ++i) {
        if (banks[i].hook_address == m_selected_f0_temp_hook) {
            selected = i;
            found_selected = true;
            break;
        }
    }
    if (!found_selected) {
        selected = 0;
        m_selected_f0_temp_hook = banks[0].hook_address;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("F0 Temp Registers");
    ImGui::SameLine();
    ImGui::TextDisabled("[persistent T0-T7 + private TFLAGS]");

    if (banks.size() > 1) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##f0_temp_bank",
                              banks[selected].display_name.c_str())) {
            for (size_t i = 0; i < banks.size(); ++i) {
                const bool is_selected = i == selected;
                if (ImGui::Selectable(banks[i].display_name.c_str(),
                                      is_selected)) {
                    selected = i;
                    m_selected_f0_temp_hook = banks[i].hook_address;
                }
                if (is_selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::Text("%s", banks[selected].cheat_name.c_str());
    }

    const auto &bank = banks[selected];
    ImGui::TextDisabled("Hook %08X | Cave %08X | T bank %08X",
                        bank.hook_address, bank.cave_address, bank.temp_address);

    uint8_t raw[36] = {};
    if (!Read(AddressSpace::Virtual, bank.temp_address, raw, sizeof(raw))) {
        ImGui::TextDisabled("T0-T7/TFLAGS storage is temporarily unavailable.");
        return;
    }

    if (ImGui::BeginTable("f0_temp_registers", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingFixedFit)) {
        for (unsigned i = 0; i < 8; ++i) {
            const uint32_t value = load_le(raw + i * 4u, 4u);
            char name[4];
            std::snprintf(name, sizeof(name), "T%u", i);
            const uint32_t storage = bank.temp_address + i * 4u;

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            ImGui::TableNextColumn();
            ImGui::PushID((int)i);

            const bool can_edit = !runstate_is_running();
            const bool editing = m_register_edit_active &&
                                 m_register_edit_is_temp &&
                                 m_register_edit_temp_address == storage;
            if (editing) {
                ImGui::SetNextItemWidth(90.0f);
                if (m_register_edit_focus_requested) {
                    ImGui::SetKeyboardFocusHere();
                    m_register_edit_focus_requested = false;
                }
                const bool enter = ImGui::InputText(
                    "##temp_edit", m_register_edit_text,
                    sizeof(m_register_edit_text),
                    ImGuiInputTextFlags_CharsHexadecimal |
                        ImGuiInputTextFlags_EnterReturnsTrue |
                        ImGuiInputTextFlags_AutoSelectAll);
                if (enter) {
                    CommitRegisterEdit();
                } else if (ImGui::IsItemActive() &&
                           ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    m_register_edit_active = false;
                    m_register_edit_focus_requested = false;
                    m_register_edit_is_temp = false;
                    m_register_edit_temp_address = 0;
                    m_debug_status = "F0 temp register edit cancelled";
                }
            } else {
                char value_text[16];
                std::snprintf(value_text, sizeof(value_text), "%08X", value);
                if (ImGui::Selectable(value_text, false,
                                      ImGuiSelectableFlags_AllowDoubleClick,
                                      ImVec2(82.0f, 0.0f)) &&
                    can_edit &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    BeginTempRegisterEdit(name, value, storage);
                }
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Edit Temp Value", nullptr, false,
                                        can_edit)) {
                        BeginTempRegisterEdit(name, value, storage);
                    }
                    if (ImGui::BeginMenu("Copy")) {
                        if (ImGui::MenuItem("Register")) {
                            ImGui::SetClipboardText(name);
                        }
                        if (ImGui::MenuItem("Value")) {
                            ImGui::SetClipboardText(value_text);
                        }
                        ImGui::EndMenu();
                    }
                    if (ImGui::BeginMenu("View In")) {
                        if (ImGui::MenuItem("Memory (Value)")) {
                            JumpViewerTo(AddressSpace::Virtual, value);
                            SelectMemoryByte(AddressSpace::Virtual, value);
                            m_request_memory_tab = true;
                        }
                        if (ImGui::MenuItem("Memory (T Storage)")) {
                            JumpViewerTo(AddressSpace::Virtual, storage);
                            SelectMemoryByte(AddressSpace::Virtual, storage);
                            m_request_memory_tab = true;
                        }
                        if (ImGui::MenuItem("x86 Debugger")) {
                            NavigateDebuggerAddress(value);
                            m_request_debugger_tab = true;
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::PopID();
        }

        /* v0.1.65 private condition state. Offset +20h follows T7; the next
         * dword is an internal saved-game-EFLAGS scratch slot and is hidden. */
        {
            const uint32_t value = load_le(raw + 32u, 4u);
            const uint32_t storage = bank.temp_address + 32u;
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("TFLAGS");
            ImGui::TableNextColumn();
            ImGui::PushID("TFLAGS");

            char value_text[16];
            std::snprintf(value_text, sizeof(value_text), "%08X", value);
            ImGui::Selectable(value_text, false, 0, ImVec2(82.0f, 0.0f));
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::BeginMenu("Copy")) {
                    if (ImGui::MenuItem("Register")) {
                        ImGui::SetClipboardText("TFLAGS");
                    }
                    if (ImGui::MenuItem("Value")) {
                        ImGui::SetClipboardText(value_text);
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("View Storage In Memory")) {
                    JumpViewerTo(AddressSpace::Virtual, storage);
                    SelectMemoryByte(AddressSpace::Virtual, storage);
                    m_request_memory_tab = true;
                }
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Private F0 condition state. Read-only here so editing cannot "
                    "inject control bits such as TF/IF into transient POPFD use.");
            }
            ImGui::PopID();

            ImGui::TableNextColumn();
            ImGui::TextDisabled("CF%d ZF%d SF%d OF%d",
                                (value & (1u << 0)) != 0,
                                (value & (1u << 6)) != 0,
                                (value & (1u << 7)) != 0,
                                (value & (1u << 11)) != 0);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("PF%d AF%d",
                                (value & (1u << 2)) != 0,
                                (value & (1u << 4)) != 0);
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("T0-T7 and TFLAGS persist until this F0 is disabled or Type-F is reset.");
}

void MemoryToolsWindow::DrawBreakpoints()
{
    ImGui::TextUnformatted("Breakpoints");

    ImGui::SetNextItemWidth(120.0f);
    const bool address_entered = ImGui::InputText(
        "Address##breakpoint", m_breakpoint_address_text,
        sizeof(m_breakpoint_address_text),
        ImGuiInputTextFlags_CharsHexadecimal |
            ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    const char *breakpoint_types[] = {
        "Execute", "Read", "Write", "Read / Write"
    };
    ImGui::Combo("Type##breakpoint", &m_breakpoint_kind,
                 breakpoint_types, IM_ARRAYSIZE(breakpoint_types));

    ImGui::SameLine();
    ImGui::BeginDisabled(m_breakpoint_kind == 0);
    ImGui::SetNextItemWidth(65.0f);
    ImGui::InputInt("Len##breakpoint", &m_watchpoint_length, 0);
    ImGui::EndDisabled();
    if (m_watchpoint_length < 1) {
        m_watchpoint_length = 1;
    }

    if (ImGui::GetContentRegionAvail().x < 210.0f) {
        ImGui::NewLine();
    } else {
        ImGui::SameLine();
    }
    const bool add_clicked = ImGui::Button("Add Breakpoint");
    if (add_clicked || address_entered) {
        uint32_t address = 0;
        if (!ParseHexAddress(m_breakpoint_address_text, address)) {
            m_breakpoint_status = "Invalid breakpoint address";
        } else {
            AddBreakpointByKind(address, m_breakpoint_kind);
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear All")) {
        for (const ExecuteBreakpoint &bp : m_breakpoints) {
            if (bp.enabled) {
                xemu_cheat_breakpoint_remove(bp.address);
            }
        }
        for (const DataWatchpoint &wp : m_watchpoints) {
            if (wp.enabled) {
                xemu_cheat_watchpoint_remove(wp.address, wp.length,
                                             wp.access_flags);
            }
        }
        m_breakpoints.clear();
        m_watchpoints.clear();
        m_condition_editor_open = false;
        m_condition_target = BreakpointConditionTarget::None;
        m_breakpoint_status = "All breakpoints removed";
    }

    const int debug_backend = xemu_cheat_debug_backend();
    const bool watchpoints_supported =
        m_breakpoint_kind == 0 || xemu_cheat_watchpoint_supported() != 0;
    if (m_breakpoint_kind != 0 && !watchpoints_supported) {
        ImGui::TextDisabled(
            "Read/Write breakpoints are not available under the active debugger backend.");
    }

    /* Synchronize CR3/page-table state once, then translate each Virtual
     * breakpoint live for the Physical column. While the guest is stopped,
     * page-table state cannot advance between rows, so identical 4 KiB pages
     * may safely share one translation for this DrawBreakpoints() call only.
     * The cache is deliberately frame-local and is never used while running. */
    const bool guest_running = runstate_is_running();
    bool virtual_map_prepared = false;
    if ((!m_breakpoints.empty() || !m_watchpoints.empty()) && !guest_running) {
        virtual_map_prepared = xemu_cheat_prepare_virtual_map() != 0;
    }

    struct BreakpointPageTranslation {
        uint32_t virtual_page = 0;
        uint64_t physical_page = 0;
        bool physical_valid = false;
    };
    std::array<BreakpointPageTranslation, 16> page_translations = {};
    size_t page_translation_count = 0;

    auto translate_breakpoint_address = [&](uint32_t address,
                                            uint64_t &physical) -> bool {
        if (!virtual_map_prepared) {
            return xemu_cheat_virtual_to_physical(address, &physical) != 0;
        }

        const uint32_t virtual_page = address & 0xFFFFF000u;
        for (size_t i = 0; i < page_translation_count; ++i) {
            const BreakpointPageTranslation &cached = page_translations[i];
            if (cached.virtual_page == virtual_page) {
                if (cached.physical_valid) {
                    physical = cached.physical_page +
                               (uint64_t)(address - virtual_page);
                }
                return cached.physical_valid;
            }
        }

        uint64_t physical_page = 0;
        const bool physical_valid =
            xemu_cheat_virtual_to_physical(virtual_page, &physical_page) != 0;
        if (page_translation_count < page_translations.size()) {
            BreakpointPageTranslation &cached =
                page_translations[page_translation_count++];
            cached.virtual_page = virtual_page;
            cached.physical_page = physical_page;
            cached.physical_valid = physical_valid;
        }
        if (physical_valid) {
            physical = physical_page + (uint64_t)(address - virtual_page);
        }
        return physical_valid;
    };

    if (ImGui::BeginTable("breakpoint_list", 8,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_ScrollX |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(0, 260.0f))) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 34.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 78.0f);
        ImGui::TableSetupColumn("Virtual", ImGuiTableColumnFlags_WidthFixed, 88.0f);
        ImGui::TableSetupColumn("Physical", ImGuiTableColumnFlags_WidthFixed, 88.0f);
        ImGui::TableSetupColumn("Len", ImGuiTableColumnFlags_WidthFixed, 42.0f);
        ImGui::TableSetupColumn("Condition", ImGuiTableColumnFlags_WidthFixed, 68.0f);
        ImGui::TableSetupColumn("View", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("Remove", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableHeadersRow();

        size_t remove_execute = (size_t)-1;
        size_t remove_watch = (size_t)-1;

        for (size_t i = 0; i < m_breakpoints.size(); ++i) {
            ExecuteBreakpoint &bp = m_breakpoints[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            bool enabled = bp.enabled;
            if (ImGui::Checkbox("##execute_enabled", &enabled)) {
                if (enabled) {
                    if (xemu_cheat_breakpoint_insert(bp.address)) {
                        bp.enabled = true;
                    } else {
                        m_breakpoint_status = "Could not enable execute breakpoint";
                    }
                } else {
                    xemu_cheat_breakpoint_remove(bp.address);
                    bp.enabled = false;
                }
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted("Execute");
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%08X", bp.address);
            ImGui::TableSetColumnIndex(3);
            uint64_t physical = 0;
            if (translate_breakpoint_address(bp.address, physical)) {
                ImGui::Text("%08llX", (unsigned long long)physical);
            } else {
                ImGui::TextDisabled("unmapped");
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::TextDisabled("-");
            ImGui::TableSetColumnIndex(5);
            if (ImGui::SmallButton(bp.conditions.empty() ? "NO" : "YES")) {
                OpenBreakpointConditionEditor(bp);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Click to add, edit, or remove conditions for this breakpoint.");
            }
            ImGui::TableSetColumnIndex(6);
            if (ImGui::SmallButton("Go")) {
                FollowDebuggerAddress(bp.address, true);
            }
            ImGui::TableSetColumnIndex(7);
            if (ImGui::SmallButton("Delete")) {
                remove_execute = i;
            }
            ImGui::PopID();
        }

        for (size_t i = 0; i < m_watchpoints.size(); ++i) {
            DataWatchpoint &wp = m_watchpoints[i];
            ImGui::PushID((int)(10000 + i));
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            bool enabled = wp.enabled;
            if (ImGui::Checkbox("##watch_enabled", &enabled)) {
                if (enabled) {
                    if (xemu_cheat_watchpoint_insert(wp.address, wp.length,
                                                     wp.access_flags)) {
                        wp.enabled = true;
                    } else {
                        m_breakpoint_status =
                            debug_backend == XEMU_CHEAT_DEBUG_BACKEND_WHPX
                                ? "Could not enable data watchpoint (WHPX hardware slots exhausted or range unsupported)"
                                : "Could not enable data watchpoint";
                    }
                } else {
                    xemu_cheat_watchpoint_remove(wp.address, wp.length,
                                                 wp.access_flags);
                    wp.enabled = false;
                }
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(
                wp.access_flags == XEMU_CHEAT_WATCH_READ
                    ? "Read"
                    : (wp.access_flags == XEMU_CHEAT_WATCH_WRITE ? "Write"
                                                                  : "R/W"));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%08X", wp.address);
            ImGui::TableSetColumnIndex(3);
            uint64_t physical = 0;
            if (translate_breakpoint_address(wp.address, physical)) {
                ImGui::Text("%08llX", (unsigned long long)physical);
            } else {
                ImGui::TextDisabled("unmapped");
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%u", wp.length);
            ImGui::TableSetColumnIndex(5);
            if (ImGui::SmallButton(wp.conditions.empty() ? "NO" : "YES")) {
                OpenBreakpointConditionEditor(wp);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Click to add, edit, or remove conditions for this data breakpoint.");
            }
            ImGui::TableSetColumnIndex(6);
            if (ImGui::SmallButton("Go")) {
                FollowDebuggerAddress(wp.address, true);
            }
            ImGui::TableSetColumnIndex(7);
            if (ImGui::SmallButton("Delete")) {
                remove_watch = i;
            }
            ImGui::PopID();
        }

        ImGui::EndTable();

        if (remove_execute != (size_t)-1) {
            RemoveExecuteBreakpoint(remove_execute);
        }
        if (remove_watch != (size_t)-1) {
            RemoveDataWatchpoint(remove_watch);
        }
    }

    if (!m_breakpoint_status.empty()) {
        ImGui::TextWrapped("%s", m_breakpoint_status.c_str());
    }
    if (debug_backend == XEMU_CHEAT_DEBUG_BACKEND_WHPX) {
        ImGui::TextDisabled(
            "All breakpoint addresses are Virtual. Physical Memory/Search right-clicks are translated through the selected map alias. WHPX provides four x86 hardware data-breakpoint slots (DR0-DR3); Read-only can consume paired slots.");
    } else if (debug_backend == XEMU_CHEAT_DEBUG_BACKEND_KVM) {
        ImGui::TextDisabled(
            "All breakpoint addresses are Virtual. KVM uses QEMU guest-debug hardware execute/data breakpoints. Four x86 DR slots are shared by execute breakpoints and watchpoints. Native Read-only watchpoints are unavailable; use Read/Write or TCG.");
    } else {
        ImGui::TextDisabled(
            "All breakpoint addresses are Virtual. Physical Memory/Search right-clicks are translated through the selected map alias. Read/Write hits follow the x86 instruction that performed the access.");
    }
}

bool MemoryToolsWindow::DrawDisassemblyPane(bool physical)
{
    ImGui::TextUnformatted(physical ? "Physical x86 (backing RAM)"
                                   : "Virtual x86 (CPU / EIP)");

    const char *child_id = physical ? "physical_disassembly" : "virtual_disassembly";
    ImGui::BeginChild(child_id, ImVec2(0, m_disasm_pane_height), true,
                      ImGuiWindowFlags_HorizontalScrollbar);

    const bool hovered = ImGui::IsWindowHovered();
    const bool focused = ImGui::IsWindowFocused();
    if (focused) {
        m_disasm_last_keyboard_focus_physical = physical;
    }
    if (!hovered && !m_disasm_scroll_to_focus) {
        ImGui::SetScrollY(m_disasm_scroll_y);
    }

    if (!m_detached_rendering) {
        ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    }

    if (m_disassembly_rows.empty()) {
        ImGui::TextDisabled(physical
                                ? "Physical backing appears after Virtual disassembly."
                                : "Enter a Virtual address and press Disassemble.");
    } else {
        const float line_height = ImGui::GetTextLineHeightWithSpacing();
        const float branch_gutter = 30.0f;
        const float base_cursor_x = ImGui::GetCursorPosX();
        const float child_top = ImGui::GetWindowPos().y + 2.0f;
        const float child_bottom = ImGui::GetWindowPos().y +
                                   ImGui::GetWindowSize().y - 2.0f;
        ImDrawList *draw_list = ImGui::GetWindowDrawList();

        /* Find the row containing the requested focus address before clipping.
         * This lets a full 4 KiB page render efficiently while still opening
         * with several instructions before the breakpoint visible. */
        if (m_disasm_scroll_to_focus) {
            const size_t focus_index = find_disassembly_row(
                m_disassembly_rows.data(), m_disassembly_rows.size(),
                m_disasm_focus_virtual);
            if (focus_index != (size_t)-1) {
                const float desired = std::max(
                    0.0f,
                    (float)focus_index * line_height -
                        ImGui::GetWindowHeight() * 0.62f);
                ImGui::SetScrollY(desired);
                m_disasm_scroll_y = desired;
            }
        }

        const ImU32 backward_branch_color = ImGui::GetColorU32(
            ImVec4(0.45f, 0.78f, 1.00f, 0.95f));
        const ImU32 forward_branch_color = ImGui::GetColorU32(
            ImVec4(1.00f, 0.86f, 0.25f, 0.95f));

        ImGuiListClipper clipper;
        clipper.Begin((int)m_disassembly_rows.size(), line_height);
        while (clipper.Step()) {
            for (int row_index = clipper.DisplayStart;
                 row_index < clipper.DisplayEnd; ++row_index) {
                const size_t i = (size_t)row_index;
                const XemuCheatDisasmRow &row = m_disassembly_rows[i];
                const char *line = physical
                    ? m_disassembly_physical_text[i].c_str()
                    : m_disassembly_virtual_text[i].c_str();

                const bool current_eip =
                    m_have_registers && row.virtual_address == m_registers.pc;
                const bool break_stop =
                    m_have_break_highlight &&
                    row.virtual_address == m_last_break_highlight_pc;
                const bool selected = break_stop || current_eip ||
                    (m_have_disasm_selection &&
                     row.virtual_address == m_selected_disasm_virtual);

                /* Light blue marks the instruction that actually triggered the
                 * debugger: the execute-breakpoint instruction itself, or the
                 * memory-access instruction resolved from a post-access #DB.
                 * Hover is deliberately neutral grey for every row. Merely
                 * moving the mouse over an instruction must never look like
                 * selecting it or like the Current EIP/break marker. */
                int row_style_colors = 0;
                if (break_stop) {
                    ImGui::PushStyleColor(ImGuiCol_Header,
                                          ImVec4(0.30f, 0.68f, 0.95f, 0.58f));
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                          ImVec4(0.24f, 0.61f, 0.90f, 0.78f));
                    row_style_colors += 2;
                }
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                      ImVec4(0.38f, 0.38f, 0.38f, 0.68f));
                ++row_style_colors;

                ImGui::PushID(row_index);
                ImGui::SetCursorPosX(base_cursor_x + branch_gutter);
                const bool restore_keyboard_focus =
                    m_disasm_keyboard_focus_requested &&
                    m_disasm_keyboard_focus_physical == physical &&
                    row.virtual_address == m_selected_disasm_virtual;
                if (restore_keyboard_focus) {
                    ImGui::SetKeyboardFocusHere();
                }
                if (ImGui::Selectable(line, selected,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    m_have_disasm_selection = true;
                    m_selected_disasm_virtual = row.virtual_address;
                    m_selected_disasm_physical_valid = row.physical_valid != 0;
                    if (row.physical_valid) {
                        m_selected_disasm_physical = row.physical_address;
                    }

                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        /* Double-click toggles the execute breakpoint for this
                         * exact disassembly address. An enabled breakpoint is
                         * removed; otherwise the existing add path creates or
                         * re-enables it. */
                        auto breakpoint_it = std::find_if(
                            m_breakpoints.begin(), m_breakpoints.end(),
                            [&row](const ExecuteBreakpoint &bp) {
                                return bp.enabled &&
                                       bp.address == row.virtual_address;
                            });
                        if (breakpoint_it != m_breakpoints.end()) {
                            const size_t breakpoint_index =
                                (size_t)(breakpoint_it - m_breakpoints.begin());
                            RemoveExecuteBreakpoint(breakpoint_index);
                        } else {
                            AddBreakpointByKind(row.virtual_address, 0);
                        }
                    }
                }
                if (restore_keyboard_focus) {
                    m_disasm_keyboard_focus_requested = false;
                }

                const bool disasm_item_hovered = ImGui::IsItemHovered();
                const ImVec2 item_min = ImGui::GetItemRectMin();
                const ImVec2 item_max = ImGui::GetItemRectMax();

                /* Direct branch/call gutter. Backward/negative displacement is
                 * baby blue; forward/positive displacement is yellow. Active
                 * caves and branches are never relocated just to draw these. */
                const DisassemblyFlowCache *cached_flow =
                    i < m_disassembly_flow_cache.size()
                        ? &m_disassembly_flow_cache[i]
                        : nullptr;
                const DebugFlowInfo *flow = cached_flow ? &cached_flow->flow : nullptr;
                if (flow != nullptr && flow->target_valid &&
                    (flow->kind == DebugFlowKind::Jump ||
                     flow->kind == DebugFlowKind::ConditionalJump) &&
                    flow->target != row.virtual_address) {
                    const size_t target_index = cached_flow->target_index;
                    const bool target_in_rows = target_index != (size_t)-1;
                    const bool backward = flow->target < row.virtual_address;
                    const ImU32 color = backward ? backward_branch_color
                                                 : forward_branch_color;
                    const float source_y = (item_min.y + item_max.y) * 0.5f;
                    const float raw_target_y = target_in_rows
                        ? source_y + ((float)target_index - (float)i) * line_height
                        : (backward ? child_top : child_bottom);
                    const float target_y = std::clamp(raw_target_y,
                                                      child_top,
                                                      child_bottom);
                    const float text_x = item_min.x;
                    const float lane_x = text_x - 8.0f -
                        (float)(i % 4) * 4.5f;
                    const float arrow_x = text_x - 1.0f;

                    draw_list->AddLine(ImVec2(text_x - 2.0f, source_y),
                                       ImVec2(lane_x, source_y), color, 1.35f);
                    draw_list->AddLine(ImVec2(lane_x, source_y),
                                       ImVec2(lane_x, target_y), color, 1.35f);

                    const bool target_visible = target_in_rows &&
                        raw_target_y >= child_top && raw_target_y <= child_bottom;
                    if (target_visible) {
                        draw_list->AddLine(ImVec2(lane_x, target_y),
                                           ImVec2(arrow_x - 5.0f, target_y),
                                           color, 1.35f);
                        draw_list->AddTriangleFilled(
                            ImVec2(arrow_x, target_y),
                            ImVec2(arrow_x - 6.0f, target_y - 3.0f),
                            ImVec2(arrow_x - 6.0f, target_y + 3.0f), color);
                    } else {
                        const float tip_y = target_y;
                        const float direction = backward ? -1.0f : 1.0f;
                        draw_list->AddTriangleFilled(
                            ImVec2(lane_x, tip_y),
                            ImVec2(lane_x - 3.0f, tip_y - direction * 6.0f),
                            ImVec2(lane_x + 3.0f, tip_y - direction * 6.0f),
                            color);
                    }
                }

                const AddressSpace context_space =
                    physical ? AddressSpace::Physical : AddressSpace::Virtual;
                const bool context_address_valid =
                    !physical || row.physical_valid != 0;
                const uint32_t context_address =
                    physical ? (row.physical_valid
                                    ? (uint32_t)row.physical_address
                                    : 0u)
                             : row.virtual_address;
                DrawAddressContextMenu(context_space, context_address,
                                       ContextOrigin::Debugger, nullptr, &row,
                                       context_address_valid, true,
                                       row.virtual_address);

                if (break_stop && disasm_item_hovered) {
                    ImGui::SetTooltip(
                        m_last_break_highlight_is_access
                            ? "Actual watched memory access (light blue)"
                            : "Execute breakpoint instruction (light blue)");
                }
                ImGui::PopID();

                ImGui::PopStyleColor(row_style_colors);
            }
        }
    }

    if (!m_detached_rendering) {
        ImGui::PopFont();
    }

    if (hovered) {
        m_disasm_scroll_y = ImGui::GetScrollY();
    }
    ImGui::EndChild();
    return focused;
}

void MemoryToolsWindow::DrawDebugger()
{
    LoadDebuggerPreferences();
    UpdateBreakpointHitState();

    bool refresh_disassembly = false;
    if (m_inject_disasm_refresh_pending) {
        m_inject_disasm_refresh_pending = false;
        refresh_disassembly = true;
    }

    /* F0 hooks/caves can be installed by the Cheat Engine while this debugger
     * is already displaying the affected page. Track executable-code writes
     * globally so stale rows are refreshed without requiring Go To, another
     * Disassemble click, or a breakpoint operation to make the patch visible. */
    const uint64_t code_patch_generation = xemu_cheat_code_patch_generation();
    if (code_patch_generation != m_code_patch_generation) {
        m_code_patch_generation = code_patch_generation;
        if (!m_disassembly_rows.empty()) {
            refresh_disassembly = true;
        }
    }
    if (refresh_disassembly) {
        RefreshDisassembly();
    }

    m_debug_nav_key_consumed = false;

    // Keep the Current Registers pane alive while the debugger tab is open.
    // A modest refresh interval is enough to show live changes without doing
    // an accelerator state synchronization on every rendered frame.
    const double now = ImGui::GetTime();
    if (!m_have_registers || m_last_live_register_refresh < 0.0 ||
        now - m_last_live_register_refresh >= 0.10) {
        XemuCheatX86Registers live_regs = {};
        if (xemu_cheat_get_x86_registers(&live_regs)) {
            m_registers = live_regs;
            m_have_registers = true;
        }
        /* General registers drive EIP highlighting/navigation and therefore
         * stay on the established 100 ms cadence. x87/MMX/SSE are invisible
         * while General is selected, so do not synchronize that larger
         * backend state until one of those tabs is actually active.
         * DrawRegisters() performs an immediate same-frame fetch when the
         * user leaves General, preserving the visible behavior. */
        if (m_register_view != 0) {
            XemuCheatX86ExtraRegisters extra_regs = {};
            if (xemu_cheat_get_x86_extra_registers(&extra_regs)) {
                m_extra_registers = extra_regs;
                m_have_extra_registers = true;
            }
        }
        m_last_live_register_refresh = now;
    }

    const bool running = runstate_is_running();
    const int debug_backend = xemu_cheat_debug_backend();
    if (running) {
        if (ImGui::Button("Pause")) {
            vm_stop(RUN_STATE_PAUSED);
        }
    } else {
        if (ImGui::Button("Resume")) {
            XemuCheatX86Registers regs = {};
            bool at_execute_breakpoint = false;
            if (RefreshRegisters(regs)) {
                m_registers = regs;
                m_have_registers = true;
                at_execute_breakpoint = IsEnabledBreakpointAt(regs.pc);
            }

            if (at_execute_breakpoint &&
                debug_backend == XEMU_CHEAT_DEBUG_BACKEND_TCG) {
                /* TCG checks CPU breakpoints before executing the instruction
                 * at the current PC. Its documented single-step path overrides
                 * breakpoints for one instruction, so keep the invisible
                 * one-instruction Continue helper only for TCG. */
                if (StartDebugStep(DebugStepMode::ContinuePastBreakpoint)) {
                    m_breakpoint_status =
                        "Continuing past execute breakpoint (TCG step-over)...";
                }
            } else if (at_execute_breakpoint &&
                       debug_backend == XEMU_CHEAT_DEBUG_BACKEND_KVM) {
                /* KVM hardware execute breakpoints retrigger at the same EIP. Remove
                 * the current one for exactly one instruction, single-step,
                 * then UpdateBreakpointHitState() restores it before resuming. */
                if (!xemu_cheat_breakpoint_remove(regs.pc)) {
                    m_breakpoint_status =
                        "Could not temporarily remove KVM execute breakpoint";
                } else {
                    m_resume_breakpoint_restore_pending = true;
                    m_resume_breakpoint_restore_address = regs.pc;
                    if (StartDebugStep(DebugStepMode::ContinuePastBreakpoint)) {
                        m_breakpoint_status =
                            "Continuing past execute breakpoint (KVM step-over)...";
                    } else {
                        xemu_cheat_breakpoint_insert(regs.pc);
                        m_resume_breakpoint_restore_pending = false;
                    }
                }
            } else {
                /* WHPX already has native breakpoint resume handling in
                 * whpx_vcpu_run(): when PC is on an active breakpoint it
                 * temporarily restores the original instruction, performs an
                 * exclusive single-step, restores the breakpoint, and resumes.
                 * Do not layer our own single-step on top of that mechanism. */
                xemu_cheat_single_step(0);
                m_debug_step_mode = DebugStepMode::None;
                vm_start();
                m_was_debug_paused = false;
                if (at_execute_breakpoint &&
                    debug_backend == XEMU_CHEAT_DEBUG_BACKEND_WHPX) {
                    m_breakpoint_status =
                        "Continuing with WHPX native breakpoint step-over";
                }
            }
        }
        ImGui::SameLine();
        const bool guest_debug_supported =
            xemu_cheat_guest_debug_supported() != 0;
        ImGui::BeginDisabled(!guest_debug_supported);
        const bool step_into_clicked = ImGui::Button("Step Into");
        ImGui::EndDisabled();
        if (step_into_clicked) {
            XemuCheatX86Registers step_regs = {};
            const bool at_kvm_breakpoint =
                debug_backend == XEMU_CHEAT_DEBUG_BACKEND_KVM &&
                RefreshRegisters(step_regs) &&
                IsEnabledBreakpointAt(step_regs.pc);

            if (at_kvm_breakpoint) {
                if (!xemu_cheat_breakpoint_remove(step_regs.pc)) {
                    m_breakpoint_status =
                        "Could not temporarily remove KVM execute breakpoint for Step Into";
                } else {
                    m_resume_breakpoint_restore_pending = true;
                    m_resume_breakpoint_restore_address = step_regs.pc;
                    if (!StartDebugStep(DebugStepMode::UserStep)) {
                        xemu_cheat_breakpoint_insert(step_regs.pc);
                        m_resume_breakpoint_restore_pending = false;
                    }
                }
            } else {
                StartDebugStep(DebugStepMode::UserStep);
            }
        }
    }

    ImGui::SameLine();
    const RunState run_state = runstate_get();
    const char *run_state_name = run_state == RUN_STATE_DEBUG
                                     ? "Breakpoint / Debug"
                                     : (runstate_is_running() ? "Running" : "Paused");
    ImGui::Text("State: %s", run_state_name);
    ImGui::SameLine();
    if (debug_backend == XEMU_CHEAT_DEBUG_BACKEND_WHPX) {
        ImGui::TextDisabled("Backend: WHPX (native BP resume)");
    } else if (debug_backend == XEMU_CHEAT_DEBUG_BACKEND_TCG) {
        ImGui::TextDisabled("Backend: TCG");
    } else if (debug_backend == XEMU_CHEAT_DEBUG_BACKEND_KVM) {
        ImGui::TextDisabled("Backend: KVM (QEMU hardware guest debug)");
    } else if (debug_backend == XEMU_CHEAT_DEBUG_BACKEND_HVF) {
        ImGui::TextDisabled("Backend: HVF (breakpoints/watchpoints/step unavailable; use TCG for debugging)");
    } else {
        ImGui::TextDisabled("Backend: Other");
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh Registers")) {
        XemuCheatX86Registers regs = {};
        if (RefreshRegisters(regs)) {
            m_registers = regs;
            m_have_registers = true;
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow EIP", &m_follow_eip);
    ImGui::SameLine();
    ImGui::Checkbox("Labels", &m_labels_enabled);
    ImGui::SameLine();
    if (ImGui::Button("LABELS")) {
        m_label_browser_open = true;
        m_label_browser_focus_requested = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Browse/search XBE labels and jump the paired x86 disassembler");
    }
    ImGui::SameLine();
    if (ImGui::Button("DUMP LABELS")) {
        DumpLabels();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Write Virtual + current Physical XBE label addresses to text");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu", current_game_manager.Labels().labels.size());
    ImGui::SameLine();
    if (ImGui::Button("RESET UI")) {
        ResetDebuggerPreferences();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Reset debugger display preferences only\n"
            "Pane height, disassembly view/count, Follow EIP, Labels, register tab");
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(120.0f);
    const bool address_entered = ImGui::InputText(
        "Virtual Address##disasm", m_disasm_address_text,
        sizeof(m_disasm_address_text),
        ImGuiInputTextFlags_CharsHexadecimal |
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(95.0f);
    int disasm_view_mode = m_disasm_full_page ? 0 : 1;
    const char *disasm_view_modes[] = {"Full Page", "Count"};
    if (ImGui::Combo("View##disasm", &disasm_view_mode,
                     disasm_view_modes, IM_ARRAYSIZE(disasm_view_modes))) {
        m_disasm_full_page = disasm_view_mode == 0;
    }
    if (!m_disasm_full_page) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::InputInt("Count##disasm", &m_disasm_instruction_count, 0, 0);
        m_disasm_instruction_count = std::clamp(m_disasm_instruction_count, 1, 128);
    }
    ImGui::SameLine();
    if (ImGui::Button("Disassemble") || address_entered) {
        RefreshDisassembly();
    }
    ImGui::SameLine();
    if (ImGui::Button("Go to EIP")) {
        XemuCheatX86Registers regs = {};
        if (RefreshRegisters(regs)) {
            m_registers = regs;
            m_have_registers = true;
            FollowDebuggerAddress(regs.pc, true);
        }
    }

    if (!m_disassembly_rows.empty()) {
        const XemuCheatDisasmRow &first = m_disassembly_rows.front();
        ImGui::SameLine();
        if (m_disasm_full_page) {
            const uint32_t page_base = m_disasm_address & 0xFFFFF000u;
            ImGui::TextDisabled("Page V %08X-%08X", page_base,
                                page_base + 0xFFFu);
        } else if (first.physical_valid) {
            ImGui::TextDisabled("V %08X -> P %08llX",
                                first.virtual_address,
                                (unsigned long long)first.physical_address);
        } else {
            ImGui::TextDisabled("V %08X -> P unmapped", first.virtual_address);
        }
    }
    if (m_have_break_highlight) {
        ImGui::TextDisabled(
            m_last_break_highlight_is_access
                ? "Light blue = actual watched Read/Write instruction; Current EIP is the CPU stop after the access."
                : "Light blue = execute breakpoint instruction. Full Page auto-centers with earlier instructions visible.");
    }

    /* Both paired panes render the same immutable row/text cache. Check its
     * generation once per debugger frame rather than making both panes repeat
     * the size/generation/label-state test independently. RefreshDisassembly()
     * still rebuilds the cache immediately after byte changes; this frame check
     * exists only for label-generation and Labels-toggle invalidation. */
    const uint64_t disassembly_label_generation =
        current_game_manager.LabelGeneration();
    if (m_disassembly_virtual_text.size() != m_disassembly_rows.size() ||
        m_disassembly_physical_text.size() != m_disassembly_rows.size() ||
        m_disassembly_label_generation != disassembly_label_generation ||
        m_disassembly_cached_labels_enabled != m_labels_enabled) {
        RebuildDisassemblyRenderCache();
    }

    bool disassembly_keyboard_focused = false;
    if (ImGui::BeginTable("debugger_disassembly_pair", 2,
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Virtual x86", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableSetupColumn("Physical x86", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        disassembly_keyboard_focused |= DrawDisassemblyPane(false);
        ImGui::TableSetColumnIndex(1);
        disassembly_keyboard_focused |= DrawDisassemblyPane(true);
        ImGui::EndTable();
    }

    /* Any focus request that existed when this frame began has now been
     * consumed by both panes. Navigation is intentionally processed only
     * after that reset, so a Right/Follow generated from either pane leaves a
     * fresh scroll-to-target request for the next frame. This removes the old
     * Physical-pane/top-of-page behavior. */
    m_disasm_scroll_to_focus = false;
    if (m_debug_nav_pending_action != 0) {
        const int pending_action = m_debug_nav_pending_action;
        const uint32_t pending_address = m_debug_nav_pending_address;
        std::string pending_status = std::move(m_debug_nav_pending_status);
        m_debug_nav_pending_action = 0;

        bool navigated = false;
        if (pending_action == 1) {
            NavigateDebuggerAddress(pending_address);
            navigated = true;
        } else if (pending_action == 2) {
            navigated = NavigateDebuggerBack();
        } else if (pending_action == 3) {
            navigated = NavigateDebuggerForward();
        }
        if (navigated && !pending_status.empty()) {
            m_debug_status = pending_status;
        }
    } else if (disassembly_keyboard_focused) {
        HandleDebuggerNavigationKeys();
    }

    /* Full-width horizontal disassembly splitter.  The old 1.65 layout had
     * only the table's vertical Virtual/Physical divider; the disassembly
     * children themselves were hard-coded to 320 px high.  Keep this resize
     * handle in its own row before the explanatory text so the hit target can
     * never be covered by that text. */
    {
        const float splitter_height = 10.0f;
        const ImVec2 splitter_pos = ImGui::GetCursorScreenPos();
        const float splitter_width = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##debugger_disassembly_horizontal_splitter",
                               ImVec2(splitter_width, splitter_height));
        const bool splitter_hovered = ImGui::IsItemHovered();
        const bool splitter_active = ImGui::IsItemActive();
        if (splitter_hovered || splitter_active) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
        if (splitter_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            m_disasm_pane_height = std::clamp(
                m_disasm_pane_height + ImGui::GetIO().MouseDelta.y,
                160.0f, 1200.0f);
        }
        if (splitter_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_disasm_pane_height = 320.0f;
        }

        ImDrawList *splitter_draw = ImGui::GetWindowDrawList();
        const float y = splitter_pos.y + splitter_height * 0.5f;
        const ImU32 splitter_color = ImGui::GetColorU32(
            splitter_active ? ImGuiCol_SeparatorActive
                            : (splitter_hovered ? ImGuiCol_SeparatorHovered
                                                : ImGuiCol_Separator));
        splitter_draw->AddLine(ImVec2(splitter_pos.x, y),
                               ImVec2(splitter_pos.x + splitter_width, y),
                               splitter_color, splitter_active ? 3.0f : 2.0f);
        const float center_x = splitter_pos.x + splitter_width * 0.5f;
        splitter_draw->AddLine(ImVec2(center_x - 18.0f, y - 2.0f),
                               ImVec2(center_x + 18.0f, y - 2.0f),
                               splitter_color, 1.0f);
        splitter_draw->AddLine(ImVec2(center_x - 18.0f, y + 2.0f),
                               ImVec2(center_x + 18.0f, y + 2.0f),
                               splitter_color, 1.0f);
        if (splitter_hovered) {
            ImGui::SetTooltip("Drag to resize both disassembly panes\nDouble-click to reset to 320 px");
        }
    }

    ImGui::TextDisabled(
        "The Physical pane is the current RAM backing for each Virtual instruction; "
        "rows are paired per instruction, so non-contiguous page mappings remain accurate.");
    ImGui::TextDisabled(
        "Flow: Right = follow target, Shift+Right = fall-through, Left = Back, Alt+Right = Forward. "
        "Branch gutter: baby blue = backward (-), yellow = forward (+). Double-click = Break on Exe.");

    ImGui::Separator();

    /* v0.1.65 compact debugger state workspace:
     *   left  = Current | Last BP, with the F-Type private state beneath both
     *   right = Breakpoints
     *
     * The register area is intentionally fixed/compact instead of stretching
     * across the entire debugger width. Widening the window therefore gives
     * the disassembly and breakpoint panes useful horizontal room. */
    if (ImGui::BeginTable("debugger_state_workspace", 2,
                          ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Registers / F-Type",
                                ImGuiTableColumnFlags_WidthFixed, 660.0f);
        ImGui::TableSetupColumn("Breakpoints",
                                ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::BeginTable("debugger_register_pair", 2,
                              ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Current Registers",
                                    ImGuiTableColumnFlags_WidthFixed, 320.0f);
            ImGui::TableSetupColumn("Last BP",
                                    ImGuiTableColumnFlags_WidthFixed, 320.0f);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            if (m_have_registers) {
                DrawRegisters(m_registers, false);
            } else {
                ImGui::TextUnformatted("Current Registers");
                ImGui::TextDisabled("Waiting for CPU register state...");
            }

            ImGui::TableSetColumnIndex(1);
            if (m_have_break_registers) {
                DrawRegisters(m_break_registers, true);
            } else {
                ImGui::TextUnformatted("Last BP");
                ImGui::Dummy(ImVec2(0.0f, ImGui::GetFrameHeight()));
                ImGui::TextDisabled("No breakpoint has been hit yet.");
            }
            ImGui::EndTable();
        }

        /* F0 private state spans only the compact register workspace instead
         * of stretching underneath the breakpoint pane. */
        DrawF0TempRegisters();

        if (!m_debug_status.empty()) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted("Info");
            ImGui::TextWrapped("%s", m_debug_status.c_str());
        }

        ImGui::TableSetColumnIndex(1);
        DrawBreakpoints();

        ImGui::EndTable();
    }

    StoreDebuggerPreferences();
}
