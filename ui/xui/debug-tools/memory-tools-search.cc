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

size_t MemoryToolsWindow::ValueSize(ValueKind kind) const
{
    switch (kind) {
    case ValueKind::U8:
    case ValueKind::S8:
        return 1;
    case ValueKind::U16:
    case ValueKind::S16:
        return 2;
    case ValueKind::U32:
    case ValueKind::S32:
    case ValueKind::Float32:
        return 4;
    }
    return 4;
}

bool MemoryToolsWindow::ReadRaw(AddressSpace space, uint32_t address,
                                ValueKind kind, uint32_t &raw) const
{
    const size_t size = ValueSize(kind);
    uint8_t bytes[4] = {};
    if (!Read(space, address, bytes, size)) {
        return false;
    }
    raw = load_le(bytes, size);
    return true;
}

void MemoryToolsWindow::FormatValue(char *dst, size_t dst_size,
                                    uint32_t raw, ValueKind kind) const
{
    switch (kind) {
    case ValueKind::U8:
        std::snprintf(dst, dst_size, "%u (0x%02X)", raw & 0xFFu, raw & 0xFFu);
        break;
    case ValueKind::U16:
        std::snprintf(dst, dst_size, "%u (0x%04X)", raw & 0xFFFFu, raw & 0xFFFFu);
        break;
    case ValueKind::U32:
        std::snprintf(dst, dst_size, "%u (0x%08X)", raw, raw);
        break;
    case ValueKind::S8:
        std::snprintf(dst, dst_size, "%d (0x%02X)", (int)(int8_t)raw, raw & 0xFFu);
        break;
    case ValueKind::S16:
        std::snprintf(dst, dst_size, "%d (0x%04X)", (int)(int16_t)raw, raw & 0xFFFFu);
        break;
    case ValueKind::S32:
        std::snprintf(dst, dst_size, "%d (0x%08X)", (int32_t)raw, raw);
        break;
    case ValueKind::Float32:
        std::snprintf(dst, dst_size, "%.9g (0x%08X)", raw_to_float(raw), raw);
        break;
    }
}

void MemoryToolsWindow::ResetSearch()
{
    m_have_first_scan = false;
    m_snapshot_mode = false;
    m_results.clear();
    m_snapshot.clear();
    m_snapshot_valid_pages.clear();
    m_search_status = "Search reset";
}

bool MemoryToolsWindow::ParseTarget(uint32_t &raw) const
{
    errno = 0;
    char *end = nullptr;
    if (m_value_kind == ValueKind::Float32) {
        float v = std::strtof(m_search_value_text, &end);
        if (errno != 0 || end == m_search_value_text || (end && *end != '\0')) {
            return false;
        }
        std::memcpy(&raw, &v, sizeof(raw));
        return true;
    }

    const int base = m_value_hex ? 16 : 10;
    if (m_value_kind == ValueKind::S8 ||
        m_value_kind == ValueKind::S16 ||
        m_value_kind == ValueKind::S32) {
        long long v = std::strtoll(m_search_value_text, &end, base);
        if (errno != 0 || end == m_search_value_text || (end && *end != '\0')) {
            return false;
        }
        raw = (uint32_t)v;
        return true;
    }

    unsigned long long v = std::strtoull(m_search_value_text, &end, base);
    if (errno != 0 || end == m_search_value_text || (end && *end != '\0') ||
        v > 0xFFFFFFFFull) {
        return false;
    }
    raw = (uint32_t)v;
    return true;
}

bool MemoryToolsWindow::MatchTarget(uint32_t raw, uint32_t target,
                                    NextScanMode mode) const
{
    switch (m_value_kind) {
    case ValueKind::Float32: {
        float a = raw_to_float(raw);
        float b = raw_to_float(target);
        if (std::isnan(a) || std::isnan(b)) {
            return false;
        }
        switch (mode) {
        case NextScanMode::Exact: return a == b;
        case NextScanMode::NotEqual: return a != b;
        case NextScanMode::GreaterThan: return a > b;
        case NextScanMode::LessThan: return a < b;
        default: return false;
        }
    }
    case ValueKind::S8: {
        int8_t a = (int8_t)raw, b = (int8_t)target;
        if (mode == NextScanMode::Exact) return a == b;
        if (mode == NextScanMode::NotEqual) return a != b;
        if (mode == NextScanMode::GreaterThan) return a > b;
        if (mode == NextScanMode::LessThan) return a < b;
        return false;
    }
    case ValueKind::S16: {
        int16_t a = (int16_t)raw, b = (int16_t)target;
        if (mode == NextScanMode::Exact) return a == b;
        if (mode == NextScanMode::NotEqual) return a != b;
        if (mode == NextScanMode::GreaterThan) return a > b;
        if (mode == NextScanMode::LessThan) return a < b;
        return false;
    }
    case ValueKind::S32: {
        int32_t a = (int32_t)raw, b = (int32_t)target;
        if (mode == NextScanMode::Exact) return a == b;
        if (mode == NextScanMode::NotEqual) return a != b;
        if (mode == NextScanMode::GreaterThan) return a > b;
        if (mode == NextScanMode::LessThan) return a < b;
        return false;
    }
    default: {
        const size_t size = ValueSize(m_value_kind);
        uint32_t mask = size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
        uint32_t a = raw & mask;
        uint32_t b = target & mask;
        if (mode == NextScanMode::Exact) return a == b;
        if (mode == NextScanMode::NotEqual) return a != b;
        if (mode == NextScanMode::GreaterThan) return a > b;
        if (mode == NextScanMode::LessThan) return a < b;
        return false;
    }
    }
}

bool MemoryToolsWindow::MatchPrevious(uint32_t current, uint32_t previous,
                                      NextScanMode mode) const
{
    if (m_value_kind == ValueKind::Float32) {
        float a = raw_to_float(current);
        float b = raw_to_float(previous);
        if (std::isnan(a) || std::isnan(b)) {
            return false;
        }
        if (mode == NextScanMode::Changed) return a != b;
        if (mode == NextScanMode::Unchanged) return a == b;
        if (mode == NextScanMode::Increased) return a > b;
        if (mode == NextScanMode::Decreased) return a < b;
        return false;
    }

    if (m_value_kind == ValueKind::S8) {
        int8_t a = (int8_t)current, b = (int8_t)previous;
        if (mode == NextScanMode::Changed) return a != b;
        if (mode == NextScanMode::Unchanged) return a == b;
        if (mode == NextScanMode::Increased) return a > b;
        if (mode == NextScanMode::Decreased) return a < b;
        return false;
    }
    if (m_value_kind == ValueKind::S16) {
        int16_t a = (int16_t)current, b = (int16_t)previous;
        if (mode == NextScanMode::Changed) return a != b;
        if (mode == NextScanMode::Unchanged) return a == b;
        if (mode == NextScanMode::Increased) return a > b;
        if (mode == NextScanMode::Decreased) return a < b;
        return false;
    }
    if (m_value_kind == ValueKind::S32) {
        int32_t a = (int32_t)current, b = (int32_t)previous;
        if (mode == NextScanMode::Changed) return a != b;
        if (mode == NextScanMode::Unchanged) return a == b;
        if (mode == NextScanMode::Increased) return a > b;
        if (mode == NextScanMode::Decreased) return a < b;
        return false;
    }

    const size_t size = ValueSize(m_value_kind);
    uint32_t mask = size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    uint32_t a = current & mask;
    uint32_t b = previous & mask;
    if (mode == NextScanMode::Changed) return a != b;
    if (mode == NextScanMode::Unchanged) return a == b;
    if (mode == NextScanMode::Increased) return a > b;
    if (mode == NextScanMode::Decreased) return a < b;
    return false;
}

bool MemoryToolsWindow::CaptureSnapshot()
{
    uint64_t length = (uint64_t)m_scan_end - (uint64_t)m_scan_start + 1ull;
    if (m_scan_end < m_scan_start || length == 0 || length > kMaxScanBytes) {
        m_search_status = "Scan range must be 1 byte to 256 MB";
        return false;
    }

    try {
        m_snapshot.assign((size_t)length, 0);
        size_t page_count = ((size_t)length + kPageSize - 1) / kPageSize;
        m_snapshot_valid_pages.assign(page_count, 0);
    } catch (...) {
        m_search_status = "Could not allocate scan snapshot";
        return false;
    }

    for (size_t page = 0; page < m_snapshot_valid_pages.size(); ++page) {
        size_t offset = page * kPageSize;
        size_t amount = std::min(kPageSize, m_snapshot.size() - offset);
        uint32_t address = m_scan_start + (uint32_t)offset;
        if (Read(m_search_space, address, m_snapshot.data() + offset, amount)) {
            m_snapshot_valid_pages[page] = 1;
        }
    }

    m_snapshot_start = m_scan_start;
    m_snapshot_end = m_scan_end;
    m_snapshot_kind = m_value_kind;
    m_snapshot_aligned = m_aligned;
    return true;
}

void MemoryToolsWindow::FirstScan()
{
    uint32_t start, end;
    if (!ParseHexAddress(m_scan_start_text, start) ||
        !ParseHexAddress(m_scan_end_text, end) || end < start) {
        m_search_status = "Invalid scan range";
        return;
    }
    m_scan_start = start;
    m_scan_end = end;

    uint64_t length = (uint64_t)end - (uint64_t)start + 1ull;
    if (length > kMaxScanBytes) {
        m_search_status = "Scan range is larger than 256 MB";
        return;
    }

    m_results.clear();
    m_snapshot.clear();
    m_snapshot_valid_pages.clear();
    m_have_first_scan = false;
    m_snapshot_mode = false;

    if (m_first_mode == FirstScanMode::UnknownInitial) {
        if (!CaptureSnapshot()) {
            return;
        }
        m_have_first_scan = true;
        m_snapshot_mode = true;
        m_search_status = "Unknown-value snapshot captured. Change the game value, then use Next Scan.";
        return;
    }

    uint32_t target;
    if (!ParseTarget(target)) {
        m_search_status = "Invalid search value";
        return;
    }

    const size_t size = ValueSize(m_value_kind);
    const uint32_t stride = m_aligned ? (uint32_t)size : 1u;
    uint8_t pagebuf[kPageSize];

    for (uint64_t page_base = start; page_base <= end; ) {
        uint64_t remaining = (uint64_t)end - page_base + 1ull;
        size_t amount = (size_t)std::min<uint64_t>(kPageSize, remaining);
        bool page_ok = Read(m_search_space, (uint32_t)page_base, pagebuf, amount);
        if (page_ok) {
            for (size_t off = 0; off + size <= amount; off += stride) {
                uint32_t raw = load_le(pagebuf + off, size);
                if (MatchTarget(raw, target, NextScanMode::Exact)) {
                    SearchResult result;
                    result.address = (uint32_t)page_base + (uint32_t)off;
                    result.previous_raw = raw;
                    result.current_raw = raw;
                    m_results.push_back(result);
                    if (m_results.size() >= kMaxSearchResults) {
                        break;
                    }
                }
            }
        }
        if (m_results.size() >= kMaxSearchResults || remaining <= kPageSize) {
            break;
        }
        page_base += kPageSize;
    }

    m_have_first_scan = true;
    char msg[192];
    if (m_results.size() >= kMaxSearchResults) {
        std::snprintf(msg, sizeof(msg),
                      "First scan reached the safety cap of %zu results. Narrow the range or search value before refining.",
                      kMaxSearchResults);
    } else {
        std::snprintf(msg, sizeof(msg), "First scan complete: %zu result(s)", m_results.size());
    }
    m_search_status = msg;
}

void MemoryToolsWindow::NextScan()
{
    if (!m_have_first_scan) {
        m_search_status = "Run First Scan first";
        return;
    }

    uint32_t target = 0;
    const bool target_mode = m_next_mode == NextScanMode::Exact ||
                             m_next_mode == NextScanMode::NotEqual ||
                             m_next_mode == NextScanMode::GreaterThan ||
                             m_next_mode == NextScanMode::LessThan;
    if (target_mode && !ParseTarget(target)) {
        m_search_status = "Invalid search value";
        return;
    }

    const size_t size = ValueSize(m_value_kind);

    if (m_snapshot_mode) {
        if (m_value_kind != m_snapshot_kind || m_aligned != m_snapshot_aligned ||
            m_scan_start != m_snapshot_start || m_scan_end != m_snapshot_end) {
            m_search_status = "Value type/alignment/range changed; start a new scan";
            return;
        }

        const uint32_t stride = m_aligned ? (uint32_t)size : 1u;

        // Unknown-initial scans intentionally have no result list until this
        // refinement. Fill the retained vector directly so any capacity from
        // an earlier scan can be reused instead of allocating a temporary
        // result vector and swapping it in afterward.
        m_results.clear();
        for (size_t page = 0; page < m_snapshot_valid_pages.size(); ++page) {
            if (!m_snapshot_valid_pages[page]) {
                continue;
            }
            size_t offset = page * kPageSize;
            size_t amount = std::min(kPageSize, m_snapshot.size() - offset);
            uint8_t current_page[kPageSize];
            uint32_t page_address = m_snapshot_start + (uint32_t)offset;
            if (!Read(m_search_space, page_address, current_page, amount)) {
                continue;
            }
            for (size_t off = 0; off + size <= amount; off += stride) {
                uint32_t previous = load_le(m_snapshot.data() + offset + off, size);
                uint32_t current = load_le(current_page + off, size);
                bool match = target_mode
                                 ? MatchTarget(current, target, m_next_mode)
                                 : MatchPrevious(current, previous, m_next_mode);
                if (match) {
                    SearchResult r;
                    r.address = page_address + (uint32_t)off;
                    r.previous_raw = previous;
                    r.current_raw = current;
                    m_results.push_back(r);
                    if (m_results.size() >= kMaxSearchResults) {
                        break;
                    }
                }
            }
            if (m_results.size() >= kMaxSearchResults) {
                break;
            }
        }
        m_snapshot.clear();
        m_snapshot_valid_pages.clear();
        m_snapshot_mode = false;
    } else {
        // Stable in-place compaction preserves exactly the same result order
        // while avoiding a second potentially multi-million-entry vector on
        // every refinement. Read from each original slot before writing the
        // next retained slot so source values are never overwritten early.
        const size_t original_count = m_results.size();
        size_t write_index = 0;
        for (size_t read_index = 0; read_index < original_count; ++read_index) {
            const SearchResult old = m_results[read_index];
            uint32_t current;
            if (!ReadRaw(m_search_space, old.address, m_value_kind, current)) {
                continue;
            }
            bool match = target_mode
                             ? MatchTarget(current, target, m_next_mode)
                             : MatchPrevious(current, old.current_raw, m_next_mode);
            if (match) {
                SearchResult &r = m_results[write_index++];
                r = old;
                r.previous_raw = old.current_raw;
                r.current_raw = current;
                if (write_index >= kMaxSearchResults) {
                    break;
                }
            }
        }
        m_results.resize(write_index);
    }

    char msg[192];
    if (m_results.size() >= kMaxSearchResults) {
        std::snprintf(msg, sizeof(msg),
                      "Next scan reached the safety cap of %zu results. Refine with a stricter comparison or smaller range.",
                      kMaxSearchResults);
    } else {
        std::snprintf(msg, sizeof(msg), "Next scan complete: %zu result(s)", m_results.size());
    }
    m_search_status = msg;
}

void MemoryToolsWindow::DrawSearch()
{
    int space = (int)m_search_space;
    if (ImGui::Combo("Address Space", &space, "Physical\0Virtual\0")) {
        m_search_space = (AddressSpace)space;
        ResetSearch();
    }

    ImGui::SameLine();
    if (ImGui::Button("Physical 64 MB")) {
        m_search_space = AddressSpace::Physical;
        m_scan_start = 0x00000000u;
        m_scan_end = 0x03FFFFFFu;
        SetHexText(m_scan_start_text, sizeof(m_scan_start_text), m_scan_start);
        SetHexText(m_scan_end_text, sizeof(m_scan_end_text), m_scan_end);
        ResetSearch();
    }
    ImGui::SameLine();
    if (ImGui::Button("Physical 128 MB")) {
        m_search_space = AddressSpace::Physical;
        m_scan_start = 0x00000000u;
        m_scan_end = 0x07FFFFFFu;
        SetHexText(m_scan_start_text, sizeof(m_scan_start_text), m_scan_start);
        SetHexText(m_scan_end_text, sizeof(m_scan_end_text), m_scan_end);
        ResetSearch();
    }
    ImGui::SameLine();
    if (ImGui::Button("VA 80000000-83FFFFFF")) {
        m_search_space = AddressSpace::Virtual;
        m_scan_start = 0x80000000u;
        m_scan_end = 0x83FFFFFFu;
        SetHexText(m_scan_start_text, sizeof(m_scan_start_text), m_scan_start);
        SetHexText(m_scan_end_text, sizeof(m_scan_end_text), m_scan_end);
        ResetSearch();
    }

    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputText("Start", m_scan_start_text, sizeof(m_scan_start_text),
                     ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputText("End", m_scan_end_text, sizeof(m_scan_end_text),
                     ImGuiInputTextFlags_CharsHexadecimal);

    int kind = (int)m_value_kind;
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::Combo("Value Type", &kind,
                     "UInt8\0UInt16\0UInt32\0Int8\0Int16\0Int32\0Float32\0")) {
        m_value_kind = (ValueKind)kind;
        ResetSearch();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Aligned", &m_aligned)) {
        ResetSearch();
    }
    if (m_value_kind != ValueKind::Float32) {
        ImGui::SameLine();
        ImGui::Checkbox("Hex Value", &m_value_hex);
    }

    if (!m_have_first_scan) {
        int mode = (int)m_first_mode;
        ImGui::SetNextItemWidth(150.0f);
        ImGui::Combo("Mode##first_scan_mode", &mode,
                     "Exact Value\0Unknown Initial\0");
        m_first_mode = (FirstScanMode)mode;
        if (m_first_mode == FirstScanMode::Exact) {
            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputText("Value", m_search_value_text,
                             sizeof(m_search_value_text));
        }
        if (ImGui::Button("First Scan##first_scan_button")) {
            uint32_t start, end;
            if (ParseHexAddress(m_scan_start_text, start) &&
                ParseHexAddress(m_scan_end_text, end)) {
                m_scan_start = start;
                m_scan_end = end;
            }
            FirstScan();
        }
    } else {
        int mode = (int)m_next_mode;
        ImGui::SetNextItemWidth(150.0f);
        ImGui::Combo("Compare", &mode,
                     "Exact Value\0Not Equal Value\0Changed\0Unchanged\0Increased\0Decreased\0Greater Than Value\0Less Than Value\0");
        m_next_mode = (NextScanMode)mode;
        if (m_next_mode == NextScanMode::Exact ||
            m_next_mode == NextScanMode::NotEqual ||
            m_next_mode == NextScanMode::GreaterThan ||
            m_next_mode == NextScanMode::LessThan) {
            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputText("Value", m_search_value_text,
                             sizeof(m_search_value_text));
        }
        if (ImGui::Button("Next Scan")) {
            NextScan();
        }
        ImGui::SameLine();
        if (ImGui::Button("New Scan")) {
            ResetSearch();
        }
    }

    if (!m_search_status.empty()) {
        ImGui::TextWrapped("%s", m_search_status.c_str());
    }

    ImGui::Separator();
    if (m_snapshot_mode) {
        ImGui::Text("Snapshot ready: %zu bytes", m_snapshot.size());
        ImGui::TextUnformatted("Change the value in-game, choose a comparison, then press Next Scan.");
        return;
    }

    ImGui::Text("Results: %zu", m_results.size());
    if (m_results.size() > kMaxDisplayedResults) {
        ImGui::SameLine();
        ImGui::Text("(showing first %zu)", kMaxDisplayedResults);
    }

    if (ImGui::BeginTable("search_results", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0, 300.0f))) {
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Previous");
        ImGui::TableSetupColumn("Current");
        ImGui::TableHeadersRow();

        const size_t count = std::min(m_results.size(), kMaxDisplayedResults);
        ImGuiListClipper clipper;
        clipper.Begin((int)count);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const size_t result_index = (size_t)i;
                const SearchResult &r = m_results[result_index];

                /* The clipper normally revisits the same small row window for
                 * many frames. Reuse formatting only when the complete source
                 * tuple is identical; scan mutations and value-kind changes
                 * therefore self-invalidate without a separate generation. */
                SearchDisplayCacheEntry &display =
                    m_search_display_cache[result_index %
                                           m_search_display_cache.size()];
                if (display.result_index != result_index ||
                    display.address != r.address ||
                    display.previous_raw != r.previous_raw ||
                    display.current_raw != r.current_raw ||
                    display.value_kind != m_value_kind) {
                    display.result_index = result_index;
                    display.address = r.address;
                    display.previous_raw = r.previous_raw;
                    display.current_raw = r.current_raw;
                    display.value_kind = m_value_kind;
                    std::snprintf(display.address_text,
                                  sizeof(display.address_text), "%08X",
                                  r.address);
                    FormatValue(display.previous_text,
                                sizeof(display.previous_text),
                                r.previous_raw, m_value_kind);
                    FormatValue(display.current_text,
                                sizeof(display.current_text),
                                r.current_raw, m_value_kind);
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(i);
                if (ImGui::Selectable(display.address_text, false,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        JumpViewerTo(m_search_space, r.address);
                        SelectMemoryByte(m_search_space, r.address);
                        m_request_memory_tab = true;
                    }
                }
                DrawAddressContextMenu(m_search_space, r.address,
                                       ContextOrigin::Search,
                                       display.current_text);
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(display.previous_text);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(display.current_text);
            }
        }
        ImGui::EndTable();
    }
}
