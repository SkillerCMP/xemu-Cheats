//
// xemu Memory Viewer / Search
//
// Copyright (C) 2026 xemu contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#include "memory-tools.hh"
#include "current-game.hh"
#include "cheat-engine-memory.h"
#include "cheat-engine.hh"
#include "../font-manager.hh"

#include <algorithm>
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

MemoryToolsWindow memory_tools_window;

namespace {
constexpr size_t kRowBytes = 16;
constexpr size_t kPageSize = 0x1000;
constexpr uint64_t kMaxScanBytes = 256ull * 1024ull * 1024ull;
constexpr size_t kMaxDisplayedResults = 100000;
constexpr size_t kMaxSearchResults = 5000000;

static uint32_t load_le(const uint8_t *p, size_t size)
{
    uint32_t v = 0;
    for (size_t i = 0; i < size; ++i) {
        v |= (uint32_t)p[i] << (i * 8);
    }
    return v;
}

static void store_le(uint8_t *p, size_t size, uint32_t value)
{
    for (size_t i = 0; i < size; ++i) {
        p[i] = (uint8_t)((value >> (i * 8)) & 0xFFu);
    }
}

static float raw_to_float(uint32_t raw)
{
    float value;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

static unsigned char ascii_lower(unsigned char ch)
{
    return ch >= 'A' && ch <= 'Z' ? (unsigned char)(ch + ('a' - 'A')) : ch;
}

static void format_hex_byte(uint8_t value, char out[3])
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    out[0] = kHex[(value >> 4) & 0x0Fu];
    out[1] = kHex[value & 0x0Fu];
    out[2] = '\0';
}

static void format_hex_u32(uint32_t value, char out[9])
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < 8; ++i) {
        const unsigned shift = (7u - i) * 4u;
        out[i] = kHex[(value >> shift) & 0x0Fu];
    }
    out[8] = '\0';
}

static constexpr const char *kMemoryColumnLabels[16] = {
    "0", "1", "2", "3", "4", "5", "6", "7",
    "8", "9", "A", "B", "C", "D", "E", "F",
};

static bool ascii_equal_case_insensitive(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static bool ascii_contains_case_insensitive(const std::string &text,
                                            const char *needle)
{
    if (needle == nullptr || *needle == '\0') {
        return true;
    }
    const size_t needle_len = std::strlen(needle);
    if (needle_len > text.size()) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= text.size(); ++i) {
        size_t j = 0;
        while (j < needle_len &&
               ascii_lower((unsigned char)text[i + j]) ==
                   ascii_lower((unsigned char)needle[j])) {
            ++j;
        }
        if (j == needle_len) {
            return true;
        }
    }
    return false;
}

static bool ascii_starts_with_case_insensitive(const char *text,
                                                const char *prefix)
{
    while (*prefix != '\0') {
        if (*text == '\0' ||
            ascii_lower((unsigned char)*text) !=
                ascii_lower((unsigned char)*prefix)) {
            return false;
        }
        ++text;
        ++prefix;
    }
    return true;
}

static void format_disassembly_bytes(char *dst, size_t dst_size,
                                     const uint8_t *bytes, size_t size)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    if (dst_size == 0) {
        return;
    }

    const size_t count = std::min(size, (dst_size - 1) / 3);
    size_t out = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t value = bytes[i];
        dst[out++] = kHex[value >> 4];
        dst[out++] = kHex[value & 0x0Fu];
        dst[out++] = ' ';
    }
    dst[out] = '\0';
}

static std::string make_dump_timestamp()
{
    GDateTime *now = g_date_time_new_now_local();
    if (!now) {
        return "00000000-000000";
    }
    gchar *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
    std::string result = stamp ? stamp : "00000000-000000";
    g_free(stamp);
    g_date_time_unref(now);
    return result;
}


}

MemoryToolsWindow::MemoryToolsWindow()
{
    m_physical_viewer.address = 0x00000000u;
    m_virtual_viewer.address = 0x00010000u;
    SetHexText(m_physical_viewer.address_text,
               sizeof(m_physical_viewer.address_text),
               m_physical_viewer.address);
    SetHexText(m_virtual_viewer.address_text,
               sizeof(m_virtual_viewer.address_text),
               m_virtual_viewer.address);

    SetHexText(m_scan_start_text, sizeof(m_scan_start_text), m_scan_start);
    SetHexText(m_scan_end_text, sizeof(m_scan_end_text), m_scan_end);
    std::snprintf(m_search_value_text, sizeof(m_search_value_text), "0");

    SetHexText(m_disasm_address_text, sizeof(m_disasm_address_text),
               m_disasm_address);
    std::snprintf(m_breakpoint_address_text,
                  sizeof(m_breakpoint_address_text), "00010000");

    SetHexText(m_map_virtual_text, sizeof(m_map_virtual_text), 0x00010000u);
    SetHexText(m_map_physical_text, sizeof(m_map_physical_text), 0x00000000u);
}
bool MemoryToolsWindow::Read(AddressSpace space, uint32_t address,
                             void *buffer, size_t size) const
{
    return xemu_cheat_memory_read(space == AddressSpace::Virtual,
                                  address, buffer, size) != 0;
}

bool MemoryToolsWindow::Write(AddressSpace space, uint32_t address,
                              const void *buffer, size_t size) const
{
    return xemu_cheat_memory_write(space == AddressSpace::Virtual,
                                   address, buffer, size) != 0;
}

bool MemoryToolsWindow::ParseHexAddress(const char *text, uint32_t &value)
{
    if (!text || !*text) {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    unsigned long long v = std::strtoull(text, &end, 16);
    while (end && *end == ' ') {
        ++end;
    }
    if (errno != 0 || end == text || (end && *end != '\0') ||
        v > 0xFFFFFFFFull) {
        return false;
    }
    value = (uint32_t)v;
    return true;
}

void MemoryToolsWindow::SetHexText(char *dst, size_t dst_size, uint32_t value)
{
    std::snprintf(dst, dst_size, "%08X", value);
}

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

size_t MemoryToolsWindow::FindRegionForVirtual(uint32_t address) const
{
    const uint64_t a = address;
    const auto it = std::upper_bound(
        m_memory_map_regions.begin(), m_memory_map_regions.end(), a,
        [](uint64_t value, const MemoryMapRegion &region) {
            return value < region.virtual_start;
        });
    if (it == m_memory_map_regions.begin()) {
        return (size_t)-1;
    }

    const auto candidate = std::prev(it);
    if (a >= candidate->virtual_start && a < candidate->virtual_end_exclusive) {
        return (size_t)std::distance(m_memory_map_regions.begin(), candidate);
    }
    return (size_t)-1;
}

size_t MemoryToolsWindow::FindRegionForPhysical(uint32_t address) const
{
    const uint64_t a = address;

    // Preserve the alias explicitly selected in the center pane whenever that
    // mapping still covers the physical byte being viewed.
    if (m_active_map_region < m_memory_map_regions.size()) {
        const MemoryMapRegion &active = m_memory_map_regions[m_active_map_region];
        if (a >= active.physical_start && a < active.physical_end_exclusive) {
            return m_active_map_region;
        }
    }

    for (size_t i = 0; i < m_memory_map_regions.size(); ++i) {
        const MemoryMapRegion &region = m_memory_map_regions[i];
        if (a >= region.physical_start && a < region.physical_end_exclusive) {
            return i;
        }
    }
    return (size_t)-1;
}

void MemoryToolsWindow::SelectMemoryMapRegion(size_t index, bool jump_to_start)
{
    if (index >= m_memory_map_regions.size()) {
        return;
    }

    m_active_map_region = index;
    const MemoryMapRegion &region = m_memory_map_regions[index];

    if (jump_to_start) {
        m_physical_viewer.address = (uint32_t)region.physical_start;
        m_virtual_viewer.address = (uint32_t)region.virtual_start;
        m_physical_viewer.request_scroll = true;
        m_virtual_viewer.request_scroll = true;
        SetHexText(m_physical_viewer.address_text,
                   sizeof(m_physical_viewer.address_text),
                   m_physical_viewer.address);
        SetHexText(m_virtual_viewer.address_text,
                   sizeof(m_virtual_viewer.address_text),
                   m_virtual_viewer.address);

        m_have_memory_selection = true;
        m_have_selected_physical = true;
        m_have_selected_virtual = true;
        m_selected_physical_address = (uint32_t)region.physical_start;
        m_selected_virtual_address = (uint32_t)region.virtual_start;
        m_memory_edit_space = AddressSpace::Virtual;
        m_memory_edit_text[0] = '\0';
        m_memory_edit_focus_requested = false;
    }

    SetHexText(m_map_physical_text, sizeof(m_map_physical_text),
               (uint32_t)region.physical_start);
    SetHexText(m_map_virtual_text, sizeof(m_map_virtual_text),
               (uint32_t)region.virtual_start);
    LookupPhysicalAliases();
}

bool MemoryToolsWindow::SyncPhysicalFromVirtual(uint32_t virtual_address)
{
    if (!m_memory_map_valid) {
        return false;
    }

    const size_t index = FindRegionForVirtual(virtual_address);
    if (index == (size_t)-1) {
        return false;
    }

    const MemoryMapRegion &region = m_memory_map_regions[index];
    const uint64_t physical = region.physical_start +
                              ((uint64_t)virtual_address - region.virtual_start);
    if (physical > 0xFFFFFFFFull) {
        return false;
    }

    m_active_map_region = index;
    m_physical_viewer.address = (uint32_t)physical & ~0xFu;
    m_physical_viewer.request_scroll = true;
    SetHexText(m_physical_viewer.address_text,
               sizeof(m_physical_viewer.address_text),
               m_physical_viewer.address);
    SetHexText(m_map_virtual_text, sizeof(m_map_virtual_text), virtual_address);
    SetHexText(m_map_physical_text, sizeof(m_map_physical_text),
               (uint32_t)physical);
    LookupPhysicalAliases();
    return true;
}

bool MemoryToolsWindow::SyncVirtualFromPhysical(uint32_t physical_address)
{
    if (!m_memory_map_valid) {
        return false;
    }

    const size_t index = FindRegionForPhysical(physical_address);
    if (index == (size_t)-1) {
        return false;
    }

    const MemoryMapRegion &region = m_memory_map_regions[index];
    const uint64_t virtual_address = region.virtual_start +
                                     ((uint64_t)physical_address -
                                      region.physical_start);
    if (virtual_address > 0xFFFFFFFFull) {
        return false;
    }

    m_active_map_region = index;
    m_virtual_viewer.address = (uint32_t)virtual_address & ~0xFu;
    m_virtual_viewer.request_scroll = true;
    SetHexText(m_virtual_viewer.address_text,
               sizeof(m_virtual_viewer.address_text),
               m_virtual_viewer.address);
    SetHexText(m_map_physical_text, sizeof(m_map_physical_text),
               physical_address);
    SetHexText(m_map_virtual_text, sizeof(m_map_virtual_text),
               (uint32_t)virtual_address);
    LookupPhysicalAliases();
    return true;
}

bool MemoryToolsWindow::SelectedAddressForSpace(AddressSpace space,
                                                    uint32_t &address) const
{
    if (!m_have_memory_selection) {
        return false;
    }

    if (space == AddressSpace::Physical) {
        if (!m_have_selected_physical) {
            return false;
        }
        address = m_selected_physical_address;
        return true;
    }

    if (!m_have_selected_virtual) {
        return false;
    }
    address = m_selected_virtual_address;
    return true;
}

void MemoryToolsWindow::SelectMemoryByte(AddressSpace space, uint32_t address)
{
    m_have_memory_selection = true;
    m_memory_edit_space = space;
    m_memory_edit_text[0] = '\0';
    m_memory_edit_focus_requested = false;

    if (space == AddressSpace::Physical) {
        m_selected_physical_address = address;
        m_have_selected_physical = true;
        m_have_selected_virtual = false;

        if (m_memory_map_valid) {
            const size_t index = FindRegionForPhysical(address);
            if (index != (size_t)-1) {
                const MemoryMapRegion &region = m_memory_map_regions[index];
                const uint64_t virtual_address =
                    region.virtual_start + ((uint64_t)address - region.physical_start);
                if (virtual_address <= 0xFFFFFFFFull) {
                    const bool region_changed = m_active_map_region != index;
                    m_active_map_region = index;
                    m_selected_virtual_address = (uint32_t)virtual_address;
                    m_have_selected_virtual = true;

                    const bool already_visible =
                        !region_changed && m_virtual_viewer.visible_range_valid &&
                        virtual_address >= m_virtual_viewer.visible_start &&
                        virtual_address < m_virtual_viewer.visible_end_exclusive;
                    if (already_visible) {
                        m_virtual_viewer.request_scroll = false;
                    } else {
                        m_virtual_viewer.address =
                            (uint32_t)virtual_address & ~0xFu;
                        m_virtual_viewer.request_scroll = true;
                        SetHexText(m_virtual_viewer.address_text,
                                   sizeof(m_virtual_viewer.address_text),
                                   m_virtual_viewer.address);
                    }
                    SetHexText(m_map_virtual_text, sizeof(m_map_virtual_text),
                               (uint32_t)virtual_address);
                    SetHexText(m_map_physical_text, sizeof(m_map_physical_text),
                               address);
                    LookupPhysicalAliases();
                }
            }
        }
    } else {
        m_selected_virtual_address = address;
        m_have_selected_virtual = true;
        m_have_selected_physical = false;

        if (m_memory_map_valid) {
            const size_t index = FindRegionForVirtual(address);
            if (index != (size_t)-1) {
                const MemoryMapRegion &region = m_memory_map_regions[index];
                const uint64_t physical_address =
                    region.physical_start + ((uint64_t)address - region.virtual_start);
                if (physical_address <= 0xFFFFFFFFull) {
                    const bool region_changed = m_active_map_region != index;
                    m_active_map_region = index;
                    m_selected_physical_address = (uint32_t)physical_address;
                    m_have_selected_physical = true;

                    const bool already_visible =
                        !region_changed && m_physical_viewer.visible_range_valid &&
                        physical_address >= m_physical_viewer.visible_start &&
                        physical_address < m_physical_viewer.visible_end_exclusive;
                    if (already_visible) {
                        m_physical_viewer.request_scroll = false;
                    } else {
                        m_physical_viewer.address =
                            (uint32_t)physical_address & ~0xFu;
                        m_physical_viewer.request_scroll = true;
                        SetHexText(m_physical_viewer.address_text,
                                   sizeof(m_physical_viewer.address_text),
                                   m_physical_viewer.address);
                    }
                    SetHexText(m_map_virtual_text, sizeof(m_map_virtual_text),
                               address);
                    SetHexText(m_map_physical_text, sizeof(m_map_physical_text),
                               (uint32_t)physical_address);
                    LookupPhysicalAliases();
                }
            }
        }
    }
}

void MemoryToolsWindow::PrepareMemoryByteEdit(AddressSpace space,
                                                  uint32_t address)
{
    m_memory_edit_space = space;
    m_memory_edit_focus_requested = m_memory_writing_enabled;

    uint8_t current = 0;
    if (Read(space, address, &current, 1)) {
        std::snprintf(m_memory_edit_text, sizeof(m_memory_edit_text),
                      "%02X", current);
    } else {
        m_memory_edit_text[0] = '\0';
    }
}

bool MemoryToolsWindow::DrawScrollableMemoryPane(
    AddressSpace space, ViewerState &state, uint64_t range_start,
    uint64_t range_end_exclusive, const char *pane_id, float height)
{
    if (range_end_exclusive <= range_start) {
        ImGui::TextDisabled("No memory range selected.");
        return false;
    }

    const char *space_name = space == AddressSpace::Virtual ? "Virtual" : "Physical";
    bool user_changed_address = false;

    ImGui::PushID(pane_id);
    ImGui::Text("%s", space_name);
    ImGui::SameLine();
    ImGui::TextDisabled("%08llX-%08llX",
                        (unsigned long long)range_start,
                        (unsigned long long)(range_end_exclusive - 1));

    ImGui::SetNextItemWidth(92.0f);
    const bool enter = ImGui::InputText(
        "##address", state.address_text, sizeof(state.address_text),
        ImGuiInputTextFlags_CharsHexadecimal |
            ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool go = ImGui::Button("Go");
    if (enter || go) {
        uint32_t address = 0;
        if (!ParseHexAddress(state.address_text, address)) {
            state.status = "Invalid hexadecimal address";
        } else if (space == AddressSpace::Virtual && m_memory_map_valid) {
            // Virtual Go is a global lookup, not a lookup restricted to the
            // region currently selected in the center Memory Map pane.
            const size_t index = FindRegionForVirtual(address);
            if (index == (size_t)-1) {
                state.status = "Virtual address is unmapped";
            } else {
                m_active_map_region = index;
                const MemoryMapRegion &region = m_memory_map_regions[index];
                range_start = region.virtual_start;
                range_end_exclusive = region.virtual_end_exclusive;

                state.address = address & ~0xFu;
                SetHexText(state.address_text, sizeof(state.address_text),
                           state.address);
                state.request_scroll = true;
                state.status.clear();
                user_changed_address = true;
            }
        } else if ((uint64_t)address < range_start ||
                   (uint64_t)address >= range_end_exclusive) {
            state.status = "Address is outside this displayed memory range";
        } else {
            state.address = address & ~0xFu;
            SetHexText(state.address_text, sizeof(state.address_text), state.address);
            state.request_scroll = true;
            state.status.clear();
            user_changed_address = true;
        }
    }

    const uint64_t total_rows_u64 =
        (range_end_exclusive - range_start + kRowBytes - 1) / kRowBytes;
    const int total_rows = (int)std::min<uint64_t>(
        total_rows_u64, (uint64_t)std::numeric_limits<int>::max());
    const float row_height = ImGui::GetFrameHeightWithSpacing();

    // Keep the hex editor dense. The vertical table borders are the visual
    // separators, so an address/byte ends almost immediately at its '|'.
    const float address_width = ImGui::CalcTextSize("00000000").x + 5.0f;
    const float byte_width = ImGui::CalcTextSize("00").x + 5.0f;
    const float ascii_width = ImGui::CalcTextSize("................").x + 5.0f;

    const ImGuiTableFlags flags =
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit;

    state.visible_range_valid = false;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(1.0f, 1.0f));
    if (ImGui::BeginTable("##memory", 18, flags, ImVec2(0, height))) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed,
                                address_width);
        for (int i = 0; i < 16; ++i) {
            ImGui::TableSetupColumn(kMemoryColumnLabels[i],
                                    ImGuiTableColumnFlags_WidthFixed,
                                    byte_width);
        }
        ImGui::TableSetupColumn("ASCII", ImGuiTableColumnFlags_WidthFixed,
                                ascii_width);
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableHeadersRow();

        if (state.request_scroll) {
            uint64_t target = state.address;
            if (target < range_start) {
                target = range_start;
            }
            if (target >= range_end_exclusive) {
                target = range_end_exclusive - 1;
            }
            const uint64_t row = (target - range_start) / kRowBytes;
            ImGui::SetScrollY((float)row * row_height);
            state.request_scroll = false;
        }
        const float scroll_y_before = ImGui::GetScrollY();

        uint32_t selected_address = 0;
        const bool have_selected = SelectedAddressForSpace(space, selected_address);

        uint64_t visible_start = std::numeric_limits<uint64_t>::max();
        uint64_t visible_end_exclusive = 0;

        ImGuiListClipper clipper;
        clipper.Begin(total_rows, row_height);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const uint64_t row_address_u64 =
                    range_start + (uint64_t)row * kRowBytes;
                if (row_address_u64 >= range_end_exclusive ||
                    row_address_u64 > 0xFFFFFFFFull) {
                    continue;
                }
                const uint32_t row_address = (uint32_t)row_address_u64;
                const size_t bytes_to_read = (size_t)std::min<uint64_t>(
                    kRowBytes, range_end_exclusive - row_address_u64);
                visible_start = std::min<uint64_t>(visible_start, row_address_u64);
                visible_end_exclusive = std::max<uint64_t>(
                    visible_end_exclusive, row_address_u64 + bytes_to_read);
                uint8_t bytes[kRowBytes] = {};
                const bool ok = Read(space, row_address, bytes, bytes_to_read);
                const bool row_selected =
                    have_selected && selected_address >= row_address &&
                    (uint64_t)selected_address < row_address_u64 + bytes_to_read;

                ImGui::TableNextRow(0, row_height);
                ImGui::TableSetColumnIndex(0);
                char address_label[9];
                format_hex_u32(row_address, address_label);
                if (ImGui::Selectable(address_label, row_selected,
                                      ImGuiSelectableFlags_None,
                                      ImVec2(address_width - 2.0f, 0.0f))) {
                    SelectMemoryByte(space, row_address);
                    if (m_memory_writing_enabled) {
                        PrepareMemoryByteEdit(space, row_address);
                    }
                }
                DrawAddressContextMenu(space, row_address, ContextOrigin::Memory);

                for (size_t col = 0; col < kRowBytes; ++col) {
                    ImGui::TableSetColumnIndex((int)col + 1);
                    if (col >= bytes_to_read) {
                        ImGui::TextDisabled("  ");
                        continue;
                    }

                    const uint32_t byte_address = row_address + (uint32_t)col;
                    const bool byte_selected =
                        have_selected && selected_address == byte_address;

                    ImGui::PushID((int)(row_address ^ (uint32_t)col));
                    const bool editing_this_byte =
                        ok && byte_selected && m_memory_writing_enabled &&
                        m_memory_edit_space == space;
                    if (editing_this_byte) {
                        if (m_memory_edit_focus_requested) {
                            ImGui::SetKeyboardFocusHere();
                            m_memory_edit_focus_requested = false;
                        }

                        ImGui::SetNextItemWidth(byte_width - 2.0f);
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                            ImVec2(0.0f, 0.0f));
                        const bool edited = ImGui::InputText(
                            "##byte_edit", m_memory_edit_text,
                            sizeof(m_memory_edit_text),
                            ImGuiInputTextFlags_CharsHexadecimal |
                                ImGuiInputTextFlags_AutoSelectAll |
                                ImGuiInputTextFlags_NoHorizontalScroll);
                        ImGui::PopStyleVar();

                        if (edited && std::strlen(m_memory_edit_text) == 2) {
                            char *endptr = nullptr;
                            const unsigned long parsed =
                                std::strtoul(m_memory_edit_text, &endptr, 16);
                            if (endptr != m_memory_edit_text && endptr &&
                                *endptr == '\0' && parsed <= 0xFFul) {
                                const uint8_t value = (uint8_t)parsed;
                                if (Write(space, byte_address, &value, 1)) {
                                    char message[128];
                                    std::snprintf(
                                        message, sizeof(message),
                                        "Wrote %02X to %s %08X", value,
                                        space == AddressSpace::Physical
                                            ? "Physical" : "Virtual",
                                        byte_address);
                                    state.status = message;

                                    if (byte_address != 0xFFFFFFFFu) {
                                        const uint32_t next_address =
                                            byte_address + 1;
                                        SelectMemoryByte(space, next_address);
                                        PrepareMemoryByteEdit(space,
                                                              next_address);
                                    } else {
                                        m_memory_edit_focus_requested = false;
                                    }
                                } else {
                                    state.status = "Memory write failed";
                                    PrepareMemoryByteEdit(space, byte_address);
                                }
                            }
                        }
                    } else if (!ok) {
                        if (ImGui::Selectable("??", byte_selected,
                                              ImGuiSelectableFlags_None,
                                              ImVec2(byte_width - 2.0f, 0.0f))) {
                            SelectMemoryByte(space, byte_address);
                        }
                    } else {
                        char byte_label[3];
                        format_hex_byte(bytes[col], byte_label);
                        if (ImGui::Selectable(byte_label, byte_selected,
                                              ImGuiSelectableFlags_None,
                                              ImVec2(byte_width - 2.0f, 0.0f))) {
                            SelectMemoryByte(space, byte_address);
                            if (m_memory_writing_enabled) {
                                PrepareMemoryByteEdit(space, byte_address);
                            }
                        }
                    }
                    DrawAddressContextMenu(space, byte_address, ContextOrigin::Memory);
                    ImGui::PopID();
                }

                ImGui::TableSetColumnIndex(17);
                char ascii[17];
                for (size_t i = 0; i < kRowBytes; ++i) {
                    ascii[i] = ok && i < bytes_to_read && bytes[i] >= 0x20 &&
                                       bytes[i] <= 0x7E
                                   ? (char)bytes[i]
                                   : '.';
                }
                ascii[16] = '\0';
                ImGui::TextUnformatted(ascii);
            }
        }

        if (visible_start != std::numeric_limits<uint64_t>::max() &&
            visible_end_exclusive > visible_start) {
            state.visible_range_valid = true;
            state.visible_start = visible_start;
            state.visible_end_exclusive = visible_end_exclusive;
        }

        // Scroll the paired pane seamlessly, but don't interpret an ordinary
        // byte click as a scroll/navigation event.
        const float scroll_y_after = ImGui::GetScrollY();
        const bool scrolled =
            std::fabs(scroll_y_after - scroll_y_before) > 0.5f ||
            (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f);
        if (scrolled) {
            const double scroll_y = (double)scroll_y_after;
            uint64_t top_row = scroll_y > 0.0
                                   ? (uint64_t)(scroll_y / (double)row_height)
                                   : 0;
            if (top_row >= total_rows_u64) {
                top_row = total_rows_u64 - 1;
            }
            const uint64_t top_address = range_start + top_row * kRowBytes;
            if (top_address <= 0xFFFFFFFFull &&
                state.address != (uint32_t)top_address) {
                state.address = (uint32_t)top_address;
                SetHexText(state.address_text, sizeof(state.address_text),
                           state.address);
                user_changed_address = true;
            }
        }

        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    if (!state.status.empty()) {
        ImGui::TextWrapped("%s", state.status.c_str());
    }
    ImGui::PopID();
    return user_changed_address;
}

void MemoryToolsWindow::DrawMemoryMapPane(float height)
{
    if (ImGui::Button("Refresh Map")) {
        m_memory_map_status = "Scanning the 32-bit virtual page map...";
        RefreshMemoryMap();
    }

    if (!m_memory_map_valid) {
        ImGui::TextWrapped("Refresh the map to link Physical and Virtual RAM.");
        if (!m_memory_map_status.empty()) {
            ImGui::TextWrapped("%s", m_memory_map_status.c_str());
        }
        return;
    }

    const uint64_t mapped_pages = (uint64_t)m_virtual_page_map.size();
    ImGui::TextDisabled("%llu MB RAM",
                        (unsigned long long)(m_memory_map_ram_size /
                                             (1024ull * 1024ull)));
    ImGui::TextDisabled("%llu mapped pages",
                        (unsigned long long)mapped_pages);

    if (m_active_map_region < m_memory_map_regions.size()) {
        const MemoryMapRegion &active =
            m_memory_map_regions[m_active_map_region];
        ImGui::Separator();
        ImGui::TextUnformatted("Active mapping");
        ImGui::Text("V %08llX", (unsigned long long)active.virtual_start);
        ImGui::Text("P %08llX", (unsigned long long)active.physical_start);
        const uint64_t size = active.virtual_end_exclusive - active.virtual_start;
        if (size >= 1024ull * 1024ull) {
            ImGui::TextDisabled("%.2f MB", (double)size / (1024.0 * 1024.0));
        } else {
            ImGui::TextDisabled("%.2f KB", (double)size / 1024.0);
        }
    }

    ImGui::Separator();
    const float list_height = std::max(120.0f, height - 155.0f);
    ImGui::BeginChild("##map_regions", ImVec2(0, list_height), true);
    for (size_t i = 0; i < m_memory_map_regions.size(); ++i) {
        const MemoryMapRegion &region = m_memory_map_regions[i];
        ImGui::PushID((int)i);
        char label[192];
        const uint64_t size = region.virtual_end_exclusive - region.virtual_start;
        std::snprintf(label, sizeof(label),
                      "V %08llX-%08llX\nP %08llX-%08llX\n%llu KB",
                      (unsigned long long)region.virtual_start,
                      (unsigned long long)(region.virtual_end_exclusive - 1),
                      (unsigned long long)region.physical_start,
                      (unsigned long long)(region.physical_end_exclusive - 1),
                      (unsigned long long)(size / 1024ull));
        const bool selected = i == m_active_map_region;
        if (ImGui::Selectable(label, selected,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            SelectMemoryMapRegion(i, true);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Click to make this the active Physical <-> Virtual mapping");
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (!m_memory_map_status.empty()) {
        ImGui::TextWrapped("%s", m_memory_map_status.c_str());
    }
}

void MemoryToolsWindow::DrawMemoryWorkspace()
{
    const uint64_t generation = current_game_manager.Generation();
    if (m_memory_map_valid && generation != m_memory_map_generation) {
        m_memory_map_valid = false;
        m_virtual_page_map.clear();
        m_physical_alias_page_index.clear();
        m_memory_map_regions.clear();
        m_physical_aliases.clear();
        m_active_map_region = (size_t)-1;
        m_have_memory_selection = false;
        m_have_selected_physical = false;
        m_have_selected_virtual = false;
        m_memory_edit_text[0] = '\0';
        m_memory_edit_focus_requested = false;
        m_memory_map_status = "Current game changed. Refresh the Memory Map.";
    }

    const uint64_t ram_size = xemu_cheat_ram_size();
    if (ram_size == 0) {
        ImGui::TextUnformatted("Xbox RAM is not available yet.");
        return;
    }

    // Build one map automatically for each detected game generation. The
    // center pane still exposes Refresh Map for page-table changes at runtime.
    if (!m_memory_map_valid && xemu_cheat_cpu_available() &&
        m_memory_map_attempt_generation != generation) {
        m_memory_map_attempt_generation = generation;
        RefreshMemoryMap();
    }

    const char *write_button = m_memory_writing_enabled
                                   ? "Memory Writing: Enabled"
                                   : "Memory Writing: Disabled";
    if (ImGui::Button(write_button)) {
        m_memory_writing_enabled = !m_memory_writing_enabled;
        m_memory_edit_text[0] = '\0';
        m_memory_edit_focus_requested = false;
        m_physical_viewer.status = m_memory_writing_enabled
                                       ? "Direct byte editing enabled - click a byte and type two hex digits"
                                       : "Memory writing disabled";
        m_virtual_viewer.status = m_physical_viewer.status;
        if (m_memory_writing_enabled && m_have_memory_selection) {
            uint32_t edit_address = 0;
            if (SelectedAddressForSpace(m_memory_edit_space, edit_address)) {
                PrepareMemoryByteEdit(m_memory_edit_space, edit_address);
            }
        }
    }
    ImGui::SameLine();
    if (m_memory_writing_enabled) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.20f, 1.0f),
                           "Click a byte and type two hex digits - writes immediately");
    } else {
        ImGui::TextDisabled("Enable writing to edit byte cells directly");
    }

    if (m_memory_map_valid &&
        m_active_map_region >= m_memory_map_regions.size()) {
        size_t region = FindRegionForVirtual(m_virtual_viewer.address);
        if (region == (size_t)-1) {
            region = FindRegionForPhysical(m_physical_viewer.address);
        }
        if (region != (size_t)-1) {
            SelectMemoryMapRegion(region, false);
        }
    }

    const float available_height = std::max(360.0f, ImGui::GetContentRegionAvail().y - 4.0f);
    const ImGuiTableFlags layout_flags =
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("memory_workspace", 3, layout_flags,
                          ImVec2(0, available_height))) {
        ImGui::TableSetupColumn("Physical", ImGuiTableColumnFlags_WidthStretch, 4.2f);
        ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthStretch, 1.7f);
        ImGui::TableSetupColumn("Virtual", ImGuiTableColumnFlags_WidthStretch, 4.2f);

        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        const bool physical_changed = DrawScrollableMemoryPane(
            AddressSpace::Physical, m_physical_viewer, 0, ram_size,
            "physical_pane", available_height - 8.0f);
        if (physical_changed) {
            SyncVirtualFromPhysical(m_physical_viewer.address);
        }

        ImGui::TableSetColumnIndex(1);
        DrawMemoryMapPane(available_height - 8.0f);

        ImGui::TableSetColumnIndex(2);
        if (m_memory_map_valid &&
            m_active_map_region < m_memory_map_regions.size()) {
            const MemoryMapRegion &region =
                m_memory_map_regions[m_active_map_region];
            const bool virtual_changed = DrawScrollableMemoryPane(
                AddressSpace::Virtual, m_virtual_viewer,
                region.virtual_start, region.virtual_end_exclusive,
                "virtual_pane", available_height - 8.0f);
            if (virtual_changed) {
                SyncPhysicalFromVirtual(m_virtual_viewer.address);
            }
        } else {
            ImGui::TextUnformatted("Virtual RAM");
            ImGui::TextWrapped(
                "Refresh the Memory Map, then click a mapped region in the middle pane.");
        }

        ImGui::EndTable();
    }

    // Only the selected byte becomes a compact InputText while editing.
    // This keeps the grid tight while letting Dear ImGui own text focus and
    // character input reliably. Two hex digits commit immediately.
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
                const SearchResult &r = m_results[(size_t)i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char addr[16];
                std::snprintf(addr, sizeof(addr), "%08X", r.address);
                ImGui::PushID(i);
                if (ImGui::Selectable(addr, false,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        JumpViewerTo(m_search_space, r.address);
                        SelectMemoryByte(m_search_space, r.address);
                        m_request_memory_tab = true;
                    }
                }
                char previous_value[64];
                char current_value[64];
                FormatValue(previous_value, sizeof(previous_value),
                            r.previous_raw, m_value_kind);
                FormatValue(current_value, sizeof(current_value),
                            r.current_raw, m_value_kind);
                DrawAddressContextMenu(m_search_space, r.address,
                                       ContextOrigin::Search, current_value);
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(previous_value);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(current_value);
            }
        }
        ImGui::EndTable();
    }
}

std::string MemoryToolsWindow::DumpDirectory() const
{
    char executable_dir[4096] = {};
    if (!xemu_cheat_get_executable_dir(executable_dir, sizeof(executable_dir))) {
        return {};
    }

    gchar *path = g_build_filename(executable_dir, "Ram-Dumps", nullptr);
    std::string result = path ? path : std::string();
    g_free(path);
    return result;
}

std::string MemoryToolsWindow::DumpStem() const
{
    const auto &game = current_game_manager.Get();
    std::string stem;
    if (game.valid) {
        stem = CurrentGameManager::FormatDatabaseGameId(game.title_id);
        if (!game.header_sha256.empty()) {
            stem += "-";
            stem += game.header_sha256.substr(0, std::min<size_t>(16, game.header_sha256.size()));
        }
    } else {
        stem = "NO-XBE";
    }
    stem += "-";
    stem += make_dump_timestamp();
    return stem;
}


void MemoryToolsWindow::JumpViewerTo(AddressSpace space, uint32_t address)
{
    ViewerState &viewer = space == AddressSpace::Virtual
                              ? m_virtual_viewer
                              : m_physical_viewer;
    viewer.address = address & ~0xFu;
    viewer.request_scroll = true;
    SetHexText(viewer.address_text, sizeof(viewer.address_text), viewer.address);

    if (space == AddressSpace::Virtual) {
        SyncPhysicalFromVirtual(address);
    } else {
        SyncVirtualFromPhysical(address);
    }

    char message[96];
    std::snprintf(message, sizeof(message),
                  "Memory jump to %08X", address);
    viewer.status = message;
}

bool MemoryToolsWindow::RefreshMemoryMap()
{
    constexpr uint64_t kVirtualSize = 0x100000000ull;
    constexpr uint64_t kVirtualPage = 0x1000ull;

    m_virtual_page_map.clear();
    m_physical_alias_page_index.clear();
    m_memory_map_regions.clear();
    m_physical_aliases.clear();
    m_memory_map_valid = false;
    m_active_map_region = (size_t)-1;
    m_memory_map_unique_physical_pages = 0;

    const uint64_t ram_size = xemu_cheat_ram_size();
    if (ram_size == 0 || ram_size > 0x40000000ull) {
        m_memory_map_status = "Could not determine a valid Xbox RAM size.";
        return false;
    }
    if (!xemu_cheat_cpu_available()) {
        m_memory_map_status = "Xbox CPU is not available.";
        return false;
    }
    if (!xemu_cheat_prepare_virtual_map()) {
        m_memory_map_status = "Could not synchronize the Xbox CPU/page tables.";
        return false;
    }

    const size_t physical_page_count =
        (size_t)((ram_size + kVirtualPage - 1) / kVirtualPage);
    std::vector<uint8_t> seen_physical_pages(physical_page_count, 0);

    // Most Xbox mappings are a small number of aliases of installed RAM.
    // Reserve a useful starting size without assuming an alias count.
    m_virtual_page_map.reserve(physical_page_count * 2);
    m_physical_alias_page_index.reserve(physical_page_count * 2);

    bool in_region = false;
    MemoryMapRegion current = {};

    for (uint64_t virtual_address = 0;
         virtual_address < kVirtualSize;
         virtual_address += kVirtualPage) {
        uint64_t physical_address = 0;
        if (!xemu_cheat_virtual_to_physical((uint32_t)virtual_address,
                                            &physical_address) ||
            physical_address >= ram_size) {
            if (in_region) {
                m_memory_map_regions.push_back(current);
                in_region = false;
            }
            continue;
        }

        const uint64_t physical_page = physical_address & ~(kVirtualPage - 1);
        m_virtual_page_map.push_back(
            {(uint32_t)virtual_address, physical_page});
        m_physical_alias_page_index.push_back(
            {(uint32_t)physical_page, (uint32_t)virtual_address});

        const size_t physical_index = (size_t)(physical_page / kVirtualPage);
        if (physical_index < seen_physical_pages.size()) {
            seen_physical_pages[physical_index] = 1;
        }

        // A displayed region is only merged when BOTH sides are contiguous.
        // This means a row describes a true linear virtual->physical mapping,
        // not merely adjacent virtual pages that jump around in physical RAM.
        if (!in_region ||
            current.virtual_end_exclusive != virtual_address ||
            current.physical_end_exclusive != physical_page) {
            if (in_region) {
                m_memory_map_regions.push_back(current);
            }
            current.virtual_start = virtual_address;
            current.virtual_end_exclusive = virtual_address + kVirtualPage;
            current.physical_start = physical_page;
            current.physical_end_exclusive = physical_page + kVirtualPage;
            in_region = true;
        } else {
            current.virtual_end_exclusive += kVirtualPage;
            current.physical_end_exclusive += kVirtualPage;
        }
    }

    if (in_region) {
        m_memory_map_regions.push_back(current);
    }

    std::sort(m_physical_alias_page_index.begin(),
              m_physical_alias_page_index.end(),
              [](const PhysicalAliasPage &a, const PhysicalAliasPage &b) {
                  if (a.physical_page != b.physical_page) {
                      return a.physical_page < b.physical_page;
                  }
                  return a.virtual_page < b.virtual_page;
              });

    m_memory_map_unique_physical_pages =
        (uint64_t)std::count(seen_physical_pages.begin(),
                             seen_physical_pages.end(), (uint8_t)1);
    m_memory_map_ram_size = ram_size;
    m_memory_map_generation = current_game_manager.Generation();
    m_memory_map_valid = true;

    size_t initial_region = FindRegionForVirtual(m_virtual_viewer.address);
    if (initial_region == (size_t)-1) {
        initial_region = FindRegionForPhysical(m_physical_viewer.address);
    }
    if (initial_region == (size_t)-1 && !m_memory_map_regions.empty()) {
        initial_region = 0;
    }
    if (initial_region != (size_t)-1) {
        SelectMemoryMapRegion(initial_region, false);
        SyncVirtualFromPhysical(m_physical_viewer.address);
    }

    const uint64_t mapped_pages = (uint64_t)m_virtual_page_map.size();
    const uint64_t alias_pages = mapped_pages >= m_memory_map_unique_physical_pages
                                     ? mapped_pages - m_memory_map_unique_physical_pages
                                     : 0;
    char status[320];
    std::snprintf(status, sizeof(status),
                  "Map refreshed: %llu RAM-backed virtual page(s), %llu unique physical page(s), "
                  "%llu alias page(s), %zu linear region(s).",
                  (unsigned long long)mapped_pages,
                  (unsigned long long)m_memory_map_unique_physical_pages,
                  (unsigned long long)alias_pages,
                  m_memory_map_regions.size());
    m_memory_map_status = status;

    return true;
}

void MemoryToolsWindow::LookupPhysicalAliases()
{
    m_physical_aliases.clear();

    uint32_t physical_address = 0;
    if (!ParseHexAddress(m_map_physical_text, physical_address)) {
        m_memory_map_status = "Invalid physical hexadecimal address.";
        return;
    }
    if (!m_memory_map_valid) {
        m_memory_map_status = "Build the Memory Map first.";
        return;
    }
    if ((uint64_t)physical_address >= m_memory_map_ram_size) {
        m_memory_map_status = "Physical address is outside installed Xbox RAM.";
        return;
    }

    const uint32_t physical_page = physical_address & ~0xFFFu;
    const uint32_t page_offset = physical_address & 0xFFFu;

    const auto first = std::lower_bound(
        m_physical_alias_page_index.begin(), m_physical_alias_page_index.end(),
        physical_page,
        [](const PhysicalAliasPage &mapping, uint32_t page) {
            return mapping.physical_page < page;
        });
    const auto last = std::upper_bound(
        first, m_physical_alias_page_index.end(), physical_page,
        [](uint32_t page, const PhysicalAliasPage &mapping) {
            return page < mapping.physical_page;
        });
    m_physical_aliases.reserve((size_t)std::distance(first, last));
    for (auto it = first; it != last; ++it) {
        m_physical_aliases.push_back(it->virtual_page + page_offset);
    }

    char status[256];
    if (m_physical_aliases.empty()) {
        std::snprintf(status, sizeof(status),
                      "Physical %08X has no RAM-backed virtual alias in this map snapshot.",
                      physical_address);
    } else {
        std::snprintf(status, sizeof(status),
                      "Physical %08X is visible at %zu virtual address(es).",
                      physical_address, m_physical_aliases.size());
    }
    m_memory_map_status = status;
}

void MemoryToolsWindow::DumpCurrentPage(AddressSpace space, uint32_t address)
{
    const bool was_running = runstate_is_running();
    if (was_running) {
        vm_stop(RUN_STATE_PAUSED);
    }

    const std::string directory = DumpDirectory();
    if (directory.empty()) {
        m_dump_status = "Could not determine the xemu RAM dump directory.";
        if (was_running) {
            vm_start();
        }
        return;
    }
    if (g_mkdir_with_parents(directory.c_str(), 0755) != 0) {
        m_dump_status = "Could not create RAM dump directory: " + directory;
        if (was_running) {
            vm_start();
        }
        return;
    }

    const uint32_t page_base = address & 0xFFFFF000u;
    char filename[192];
    std::snprintf(filename, sizeof(filename), "%s-%s-%08X-PAGE.bin",
                  DumpStem().c_str(),
                  space == AddressSpace::Virtual ? "VIRTUAL" : "PHYSICAL",
                  page_base);
    gchar *path_c = g_build_filename(directory.c_str(), filename, nullptr);
    const std::string path = path_c ? path_c : filename;
    g_free(path_c);

    size_t failed_pages = 0;
    const bool ok = DumpRange(space, page_base, kPageSize, path, failed_pages);

    char message[512];
    if (ok && failed_pages == 0) {
        std::snprintf(message, sizeof(message),
                      "Current %s page %08X-%08X dumped to: %s",
                      space == AddressSpace::Virtual ? "Virtual" : "Physical",
                      page_base, page_base + 0xFFFu, path.c_str());
    } else if (ok) {
        std::snprintf(message, sizeof(message),
                      "Current %s page %08X-%08X dumped with unreadable data zero-filled: %s",
                      space == AddressSpace::Virtual ? "Virtual" : "Physical",
                      page_base, page_base + 0xFFFu, path.c_str());
    } else {
        std::snprintf(message, sizeof(message),
                      "Failed to dump current %s page %08X-%08X.",
                      space == AddressSpace::Virtual ? "Virtual" : "Physical",
                      page_base, page_base + 0xFFFu);
    }
    m_dump_status = message;

    if (was_running) {
        vm_start();
    }
}

bool MemoryToolsWindow::DumpRange(AddressSpace space, uint32_t base,
                                  uint64_t size, const std::string &path,
                                  size_t &failed_pages) const
{
    failed_pages = 0;
    FILE *fp = g_fopen(path.c_str(), "wb");
    if (!fp) {
        return false;
    }

    constexpr size_t kDumpPage = 0x1000;
    uint8_t buffer[kDumpPage];
    bool file_ok = true;

    for (uint64_t offset = 0; offset < size; offset += kDumpPage) {
        const size_t amount = (size_t)std::min<uint64_t>(kDumpPage, size - offset);
        const uint32_t address = base + (uint32_t)offset;
        if (!Read(space, address, buffer, amount)) {
            std::memset(buffer, 0, amount);
            ++failed_pages;
        }
        if (std::fwrite(buffer, 1, amount, fp) != amount) {
            file_ok = false;
            break;
        }
    }

    if (std::fclose(fp) != 0) {
        file_ok = false;
    }
    if (!file_ok) {
        g_remove(path.c_str());
    }
    return file_ok;
}

bool MemoryToolsWindow::ScanMappedVirtualRam(
    uint64_t ram_size, std::vector<VirtualDumpRegion> &regions,
    uint64_t &mapped_pages) const
{
    constexpr uint64_t kVirtualSize = 0x100000000ull;
    constexpr uint64_t kVirtualPage = 0x1000ull;

    regions.clear();
    mapped_pages = 0;

    if (!xemu_cheat_prepare_virtual_map()) {
        return false;
    }

    bool in_region = false;
    VirtualDumpRegion current;

    for (uint64_t virtual_address = 0;
         virtual_address < kVirtualSize;
         virtual_address += kVirtualPage) {
        uint64_t physical_address = 0;
        const bool mapped =
            xemu_cheat_virtual_to_physical((uint32_t)virtual_address,
                                           &physical_address) != 0 &&
            physical_address < ram_size;

        if (mapped) {
            ++mapped_pages;
            if (!in_region) {
                current.start = virtual_address;
                current.end_exclusive = virtual_address + kVirtualPage;
                in_region = true;
            } else {
                current.end_exclusive = virtual_address + kVirtualPage;
            }
        } else if (in_region) {
            regions.push_back(current);
            in_region = false;
        }
    }

    if (in_region) {
        regions.push_back(current);
    }

    return true;
}

bool MemoryToolsWindow::WriteVirtualMapIndex(
    const std::string &path, uint64_t ram_size,
    const std::vector<VirtualDumpRegion> &regions,
    uint64_t mapped_pages,
    const std::vector<std::string> &region_files) const
{
    FILE *fp = g_fopen(path.c_str(), "wb");
    if (!fp) {
        return false;
    }

    const auto &game = current_game_manager.Get();
    bool ok = true;
    auto put = [&](const char *format, auto... args) {
        if (ok && std::fprintf(fp, format, args...) < 0) {
            ok = false;
        }
    };

    put("xemu Mapped Virtual RAM Dump\n");
    put("============================\n\n");
    if (game.valid) {
        const std::string game_id = CurrentGameManager::FormatDatabaseGameId(game.title_id);
        put("Game: %s\n", game.title_name.c_str());
        put("GameID: %s\n", game_id.c_str());
        put("TitleID: %08X\n", game.title_id);
        put("XBE Header SHA-256: %s\n", game.header_sha256.c_str());
    } else {
        put("Game: <no valid XBE detected>\n");
    }
    put("Installed physical RAM: %llu bytes (%llu MB)\n",
        (unsigned long long)ram_size,
        (unsigned long long)(ram_size / (1024ull * 1024ull)));
    put("Virtual scan range: 00000000-FFFFFFFF\n");
    put("Page size: 00001000\n");
    put("RAM-backed mapped pages: %llu\n",
        (unsigned long long)mapped_pages);
    put("RAM-backed mapped bytes (aliases included): %llu\n",
        (unsigned long long)(mapped_pages * 0x1000ull));
    put("Contiguous virtual regions: %zu\n\n", regions.size());
    put("Each .bin starts at file offset 00000000. Add the region's virtual\n");
    put("start address to a file offset to recover the guest virtual address.\n");
    put("Only mappings backed by installed Xbox RAM are included; MMIO/device\n");
    put("mappings are intentionally excluded. Physical pages inside a virtual\n");
    put("region are not required to be physically contiguous.\n\n");

    put("#   Virtual Start-End       Size (bytes)       File\n");
    put("--  ---------------------   ----------------   ----\n");
    for (size_t i = 0; i < regions.size(); ++i) {
        const uint64_t size = regions[i].end_exclusive - regions[i].start;
        const uint64_t end_inclusive = regions[i].end_exclusive - 1;
        const char *file = i < region_files.size() ? region_files[i].c_str() : "<missing>";
        put("%02zu  %08llX-%08llX   %016llX   %s\n",
            i + 1,
            (unsigned long long)regions[i].start,
            (unsigned long long)end_inclusive,
            (unsigned long long)size,
            file);
    }

    if (std::fclose(fp) != 0) {
        ok = false;
    }
    if (!ok) {
        g_remove(path.c_str());
    }
    return ok;
}

void MemoryToolsWindow::DumpPhysicalRam()
{
    DumpRam(true, false);
}

void MemoryToolsWindow::DumpMappedVirtualRam()
{
    DumpRam(false, true);
}

void MemoryToolsWindow::DumpFullRam()
{
    DumpRam(true, true);
}

void MemoryToolsWindow::DumpRam(bool dump_physical, bool dump_virtual)
{
    if (!dump_physical && !dump_virtual) {
        m_dump_status = "No RAM dump mode was selected.";
        return;
    }

    // Full-range dumps are intentionally taken from a stopped guest. This
    // keeps a combined physical + virtual dump internally consistent and also
    // makes the individual modes deterministic for debugger inspection. Leave
    // the VM paused afterward, matching the existing full-dump behavior.
    if (runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
    }

    const uint64_t ram_size = xemu_cheat_ram_size();
    if (ram_size == 0 || ram_size > 0x40000000ull) {
        m_dump_status = "Could not determine a valid Xbox RAM size.";
        return;
    }
    if (dump_virtual && !xemu_cheat_cpu_available()) {
        m_dump_status = "Xbox CPU is not available; mapped virtual RAM cannot be scanned. VM remains paused.";
        return;
    }

    const std::string directory = DumpDirectory();
    if (directory.empty()) {
        m_dump_status = "Could not determine the xemu executable directory.";
        return;
    }
    if (g_mkdir_with_parents(directory.c_str(), 0755) != 0) {
        m_dump_status = "Could not create RAM dump directory: " + directory;
        return;
    }

    std::vector<VirtualDumpRegion> regions;
    uint64_t mapped_pages = 0;
    if (dump_virtual && !ScanMappedVirtualRam(ram_size, regions, mapped_pages)) {
        m_dump_status = "Could not synchronize the Xbox CPU/page tables for virtual RAM mapping. VM remains paused.";
        return;
    }

    const std::string stem = DumpStem();

    bool physical_ok = true;
    size_t physical_failed = 0;
    if (dump_physical) {
        gchar *physical_c = g_build_filename(
            directory.c_str(),
            (stem + "-PHYSICAL-00000000.bin").c_str(), nullptr);
        const std::string physical_path = physical_c ? physical_c : "physical.bin";
        g_free(physical_c);

        physical_ok = DumpRange(AddressSpace::Physical, 0x00000000u,
                                ram_size, physical_path, physical_failed);
    }

    bool virtual_ok = true;
    bool map_ok = true;
    size_t virtual_failed = 0;
    std::vector<std::string> region_files;
    if (dump_virtual) {
        region_files.reserve(regions.size());
        for (const VirtualDumpRegion &region : regions) {
            const uint64_t end_inclusive = region.end_exclusive - 1;
            char filename[160];
            std::snprintf(filename, sizeof(filename),
                          "%s-VIRTUAL-%08llX-%08llX.bin",
                          stem.c_str(),
                          (unsigned long long)region.start,
                          (unsigned long long)end_inclusive);
            gchar *path_c = g_build_filename(directory.c_str(), filename, nullptr);
            const std::string path = path_c ? path_c : filename;
            g_free(path_c);

            size_t failed_pages = 0;
            const uint64_t region_size = region.end_exclusive - region.start;
            const bool region_ok = DumpRange(AddressSpace::Virtual,
                                             (uint32_t)region.start,
                                             region_size, path, failed_pages);
            virtual_failed += failed_pages;
            virtual_ok = virtual_ok && region_ok;
            region_files.emplace_back(filename);
        }

        gchar *map_c = g_build_filename(
            directory.c_str(),
            (stem + "-VIRTUAL-MAP.txt").c_str(), nullptr);
        const std::string map_path = map_c ? map_c : "virtual-map.txt";
        g_free(map_c);
        map_ok = WriteVirtualMapIndex(map_path, ram_size, regions,
                                      mapped_pages, region_files);
    }

    const bool ok = physical_ok && virtual_ok && map_ok;
    if (!ok) {
        if (dump_physical && dump_virtual) {
            m_dump_status = "Physical + mapped virtual RAM dump was only partially completed. VM remains paused. Check the output folder: " + directory;
        } else if (dump_physical) {
            m_dump_status = "Physical RAM dump failed or was only partially completed. VM remains paused. Check the output folder: " + directory;
        } else {
            m_dump_status = "Mapped virtual RAM dump failed or was only partially completed. VM remains paused. Check the output folder: " + directory;
        }
        return;
    }

    char message[640];
    if (dump_physical && dump_virtual) {
        std::snprintf(message, sizeof(message),
                      "Physical + mapped virtual RAM dump complete. VM remains paused. "
                      "Physical RAM: %llu MB. Mapped virtual RAM: %llu pages in %zu region(s), "
                      "%llu MB including aliases. Physical unreadable pages: %zu; "
                      "Virtual unreadable pages: %zu. Saved to: %s",
                      (unsigned long long)(ram_size / (1024ull * 1024ull)),
                      (unsigned long long)mapped_pages,
                      regions.size(),
                      (unsigned long long)((mapped_pages * 0x1000ull) / (1024ull * 1024ull)),
                      physical_failed, virtual_failed, directory.c_str());
    } else if (dump_physical) {
        std::snprintf(message, sizeof(message),
                      "Physical RAM dump complete. VM remains paused. Physical RAM: %llu MB. "
                      "Unreadable pages zero-filled: %zu. Saved to: %s",
                      (unsigned long long)(ram_size / (1024ull * 1024ull)),
                      physical_failed, directory.c_str());
    } else {
        std::snprintf(message, sizeof(message),
                      "Mapped virtual RAM dump complete. VM remains paused. %llu pages in %zu region(s), "
                      "%llu MB including aliases. Unreadable pages zero-filled: %zu. Saved to: %s",
                      (unsigned long long)mapped_pages,
                      regions.size(),
                      (unsigned long long)((mapped_pages * 0x1000ull) / (1024ull * 1024ull)),
                      virtual_failed, directory.c_str());
    }
    m_dump_status = message;
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
    for (size_t i = 0; i < page_row_count; ++i) {
        const XemuCheatDisasmRow &row = page_rows[i];
        const uint64_t row_end =
            (uint64_t)row.virtual_address + std::max<uint8_t>(row.size, 1);
        if ((uint64_t)requested_address >= row.virtual_address &&
            (uint64_t)requested_address < row_end) {
            resolved_address = row.virtual_address;
            break;
        }
    }

    if (m_disasm_full_page) {
        m_disassembly_rows.resize(page_row_count);
    } else {
        const int count = std::clamp(m_disasm_instruction_count, 1, 128);
        size_t row_count = 0;
        m_disassembly_rows.assign((size_t)count, {});
        result = xemu_cheat_disassemble_paired(
            resolved_address, count, m_disassembly_rows.data(),
            m_disassembly_rows.size(), &row_count);
        if (result != XEMU_CHEAT_DISAS_OK) {
            m_disassembly_rows.clear();
            m_disassembly_flow_cache.clear();
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

    RebuildDisassemblyFlowCache();

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
    uint64_t physical = 0;
    xemu_cheat_prepare_virtual_map();
    if (xemu_cheat_virtual_to_physical(resolved_address, &physical)) {
        m_selected_disasm_physical = physical;
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

void MemoryToolsWindow::UpdateBreakpointHitState()
{
    const bool debug_paused = runstate_get() == RUN_STATE_DEBUG;
    if (!debug_paused) {
        if (runstate_is_running()) {
            m_was_debug_paused = false;
        }
        return;
    }

    XemuCheatX86Registers regs = {};
    if (!RefreshRegisters(regs)) {
        return;
    }
    m_registers = regs;
    m_have_registers = true;

    if (m_was_debug_paused) {
        return;
    }

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
         * but Last BP reserves the same row height. This keeps General EAX,
         * x87 ST0, MM0 and XMM0 horizontally aligned with the live side. */
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFrameHeight()));
        if (m_register_view == 0) {
            DrawGeneralRegisterTable(r, true);
        } else {
            DrawExtraRegisterTable(m_break_extra_registers,
                                   m_have_break_extra_registers,
                                   m_register_view, true);
        }
        return;
    }

    if (ImGui::BeginTabBar("current_register_tabs")) {
        if (ImGui::BeginTabItem("General")) {
            m_register_view = 0;
            DrawGeneralRegisterTable(r, false);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("x87 / FPU")) {
            m_register_view = 1;
            DrawExtraRegisterTable(m_extra_registers, m_have_extra_registers,
                                   m_register_view, false);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("MMX")) {
            m_register_view = 2;
            DrawExtraRegisterTable(m_extra_registers, m_have_extra_registers,
                                   m_register_view, false);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("SSE")) {
            m_register_view = 3;
            DrawExtraRegisterTable(m_extra_registers, m_have_extra_registers,
                                   m_register_view, false);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void MemoryToolsWindow::DrawF0TempRegisters()
{
    /* One debugger window is rendered on the UI thread. Keep the active-bank
     * scratch list alive across frames so its vector storage and repeated
     * cheat-name string capacities can be reused instead of rebuilt every
     * draw. GetActiveF0TempBanks() fully overwrites the logical contents. */
    static std::vector<CheatEngineWindow::FTempBankInfo> banks;
    cheat_engine_window.GetActiveF0TempBanks(banks);
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
        char preview[160];
        std::snprintf(preview, sizeof(preview), "%s  [hook %08X]",
                      banks[selected].cheat_name.c_str(),
                      banks[selected].hook_address);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##f0_temp_bank", preview)) {
            for (size_t i = 0; i < banks.size(); ++i) {
                char item[160];
                std::snprintf(item, sizeof(item), "%s  [hook %08X]",
                              banks[i].cheat_name.c_str(), banks[i].hook_address);
                const bool is_selected = i == selected;
                if (ImGui::Selectable(item, is_selected)) {
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

    const bool watchpoints_supported = xemu_cheat_watchpoint_supported() != 0;
    if (m_breakpoint_kind != 0 && !watchpoints_supported) {
        ImGui::TextDisabled(
            "Read/Write breakpoints are not available under the active debugger backend.");
    }

    /* Synchronize CR3/page-table state once, then translate each Virtual
     * breakpoint live for the Physical column. */
    if ((!m_breakpoints.empty() || !m_watchpoints.empty()) &&
        !runstate_is_running()) {
        xemu_cheat_prepare_virtual_map();
    }

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
            if (xemu_cheat_virtual_to_physical(bp.address, &physical)) {
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
                            xemu_cheat_debug_backend() == XEMU_CHEAT_DEBUG_BACKEND_WHPX
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
            if (xemu_cheat_virtual_to_physical(wp.address, &physical)) {
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
    if (xemu_cheat_debug_backend() == XEMU_CHEAT_DEBUG_BACKEND_WHPX) {
        ImGui::TextDisabled(
            "All breakpoint addresses are Virtual. Physical Memory/Search right-clicks are translated through the selected map alias. WHPX provides four x86 hardware data-breakpoint slots (DR0-DR3); Read-only can consume paired slots.");
    } else if (xemu_cheat_debug_backend() == XEMU_CHEAT_DEBUG_BACKEND_KVM) {
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
            size_t focus_index = (size_t)-1;
            for (size_t i = 0; i < m_disassembly_rows.size(); ++i) {
                const XemuCheatDisasmRow &row = m_disassembly_rows[i];
                const uint64_t row_end =
                    (uint64_t)row.virtual_address + std::max<uint8_t>(row.size, 1);
                if ((uint64_t)m_disasm_focus_virtual >= row.virtual_address &&
                    (uint64_t)m_disasm_focus_virtual < row_end) {
                    focus_index = i;
                    break;
                }
            }
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
                char bytes[64];
                format_disassembly_bytes(bytes, sizeof(bytes), row.bytes,
                                         std::min<size_t>(row.size,
                                                          sizeof(row.bytes)));

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

                if (m_labels_enabled) {
                    const XemuXbeLabels::Label *label =
                        current_game_manager.PrimaryLabelAt(row.virtual_address);
                    if (label != nullptr) {
                        const size_t used = std::strlen(line);
                        if (used + label->name.size() + 6 < sizeof(line)) {
                            std::snprintf(line + used, sizeof(line) - used,
                                          "  ; %s", label->name.c_str());
                        }
                    }
                }

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
    UpdateBreakpointHitState();
    if (m_inject_disasm_refresh_pending) {
        m_inject_disasm_refresh_pending = false;
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
        XemuCheatX86ExtraRegisters extra_regs = {};
        if (xemu_cheat_get_x86_extra_registers(&extra_regs)) {
            m_extra_registers = extra_regs;
            m_have_extra_registers = true;
        }
        m_last_live_register_refresh = now;
    }

    const bool running = runstate_is_running();
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

            const int backend = xemu_cheat_debug_backend();
            if (at_execute_breakpoint &&
                backend == XEMU_CHEAT_DEBUG_BACKEND_TCG) {
                /* TCG checks CPU breakpoints before executing the instruction
                 * at the current PC. Its documented single-step path overrides
                 * breakpoints for one instruction, so keep the invisible
                 * one-instruction Continue helper only for TCG. */
                if (StartDebugStep(DebugStepMode::ContinuePastBreakpoint)) {
                    m_breakpoint_status =
                        "Continuing past execute breakpoint (TCG step-over)...";
                }
            } else if (at_execute_breakpoint &&
                       backend == XEMU_CHEAT_DEBUG_BACKEND_KVM) {
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
                    xemu_cheat_debug_backend() == XEMU_CHEAT_DEBUG_BACKEND_WHPX) {
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
            const int backend = xemu_cheat_debug_backend();
            const bool at_kvm_breakpoint =
                backend == XEMU_CHEAT_DEBUG_BACKEND_KVM &&
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
    const int debug_backend = xemu_cheat_debug_backend();
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
}

void MemoryToolsWindow::DumpLabels()
{
    const auto &database = current_game_manager.Labels();
    if (database.labels.empty()) {
        m_label_status = "No XBE labels are available for the current game.";
        m_debug_status = m_label_status;
        return;
    }

    const bool was_running = runstate_is_running();
    if (was_running) {
        vm_stop(RUN_STATE_PAUSED);
    }

    const std::string directory = DumpDirectory();
    if (directory.empty()) {
        m_label_status = "Could not determine the xemu label dump directory.";
        if (was_running) {
            vm_start();
        }
        m_debug_status = m_label_status;
        return;
    }
    if (g_mkdir_with_parents(directory.c_str(), 0755) != 0) {
        m_label_status = "Could not create label dump directory: " + directory;
        if (was_running) {
            vm_start();
        }
        m_debug_status = m_label_status;
        return;
    }

    const std::string filename = DumpStem() + "-LABELS.txt";
    gchar *path_c = g_build_filename(directory.c_str(), filename.c_str(), nullptr);
    const std::string path = path_c ? path_c : filename;
    g_free(path_c);

    FILE *fp = g_fopen(path.c_str(), "wb");
    if (fp == nullptr) {
        m_label_status = "Could not create label dump file: " + path;
        if (was_running) {
            vm_start();
        }
        m_debug_status = m_label_status;
        return;
    }

    const auto &game = current_game_manager.Get();
    std::fprintf(fp, "Xemu XBE Labels\n");
    std::fprintf(fp, "Title: %s\n",
                 game.title_name.empty() ? "<unknown>" : game.title_name.c_str());
    std::fprintf(fp, "Title ID: %s\n",
                 CurrentGameManager::FormatTitleId(game.title_id).c_str());
    std::fprintf(fp, "Header SHA-256: %s\n", game.header_sha256.c_str());
    std::fprintf(fp, "default.xbe SHA-256: %s\n\n",
                 game.disc_xbe_sha256.c_str());
    std::fprintf(fp, "VIRTUAL    PHYSICAL   TYPE       LABEL\n");
    std::fprintf(fp, "---------------------------------------------------------------\n");

    const bool can_translate = xemu_cheat_prepare_virtual_map() != 0;
    size_t mapped = 0;
    for (const XemuXbeLabels::Label &label : database.labels) {
        uint64_t physical = 0;
        const bool physical_valid = can_translate &&
            xemu_cheat_virtual_to_physical(label.virtual_address, &physical) != 0;
        if (physical_valid) {
            ++mapped;
            std::fprintf(fp, "%08X   %08llX   %-10s %s\n",
                         label.virtual_address,
                         (unsigned long long)physical,
                         XemuXbeLabels::TypeName(label.type),
                         label.name.c_str());
        } else {
            std::fprintf(fp, "%08X   UNMAPPED   %-10s %s\n",
                         label.virtual_address,
                         XemuXbeLabels::TypeName(label.type),
                         label.name.c_str());
        }
    }
    const bool write_ok = std::fclose(fp) == 0;

    if (was_running) {
        vm_start();
    }

    char status[640];
    if (write_ok) {
        std::snprintf(status, sizeof(status),
                      "Dumped %zu XBE labels (%zu currently mapped) to: %s",
                      database.labels.size(), mapped, path.c_str());
    } else {
        std::snprintf(status, sizeof(status),
                      "Label dump write failed while closing: %s", path.c_str());
    }
    m_label_status = status;
    m_debug_status = m_label_status;
}

void MemoryToolsWindow::DrawLabelBrowser()
{
    if (!m_label_browser_open) {
        return;
    }
    if (m_label_browser_focus_requested) {
        ImGui::SetNextWindowFocus();
        ImGui::SetNextWindowSize(ImVec2(820.0f, 520.0f), ImGuiCond_Appearing);
        m_label_browser_focus_requested = false;
    }

    if (!ImGui::Begin("x86 Current Labels", &m_label_browser_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    const auto &database = current_game_manager.Labels();
    ImGui::Text("Current default.xbe labels: %zu", database.labels.size());
    ImGui::SameLine();
    ImGui::TextDisabled("Virtual address is the stable master; Physical is resolved live.");

    ImGui::SetNextItemWidth(340.0f);
    ImGui::InputTextWithHint("##label_search", "Search labels...",
                             m_label_search, sizeof(m_label_search));
    ImGui::SameLine();
    const char *filters[] = {
        "All", "Entry", "Section", "Kernel", "String", "XRef", "RTTI", "Inferred"
    };
    ImGui::SetNextItemWidth(130.0f);
    ImGui::Combo("Type##label_filter", &m_label_filter,
                 filters, IM_ARRAYSIZE(filters));
    ImGui::SameLine();
    if (ImGui::Button("DUMP LABELS")) {
        DumpLabels();
    }

    ImGui::TextDisabled("Inferred labels begin with '~' and are heuristics from function-like XBE strings/xrefs, not original PDB symbols.");
    ImGui::Separator();

    std::vector<size_t> visible_labels;
    visible_labels.reserve(database.labels.size());
    for (size_t i = 0; i < database.labels.size(); ++i) {
        const XemuXbeLabels::Label &label = database.labels[i];
        if (m_label_filter != 0 &&
            (int)label.type != m_label_filter - 1) {
            continue;
        }
        if (!ascii_contains_case_insensitive(label.name, m_label_search) &&
            !ascii_contains_case_insensitive(
                XemuXbeLabels::TypeName(label.type), m_label_search)) {
            continue;
        }
        visible_labels.push_back(i);
    }

    const bool can_translate = !database.labels.empty() &&
                               xemu_cheat_prepare_virtual_map() != 0;
    if (ImGui::BeginTable("current_label_table", 4,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0.0f, 360.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 85.0f);
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Virtual", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Physical", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin((int)visible_labels.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const size_t label_index = visible_labels[(size_t)row];
                const XemuXbeLabels::Label &label = database.labels[label_index];
                uint64_t physical = 0;
                const bool physical_valid = can_translate &&
                    xemu_cheat_virtual_to_physical(label.virtual_address, &physical) != 0;

                ImGui::TableNextRow();
                ImGui::PushID((int)label_index);
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(XemuXbeLabels::TypeName(label.type));
                if (label.type == XemuXbeLabels::Type::Inferred &&
                    ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Auto-inferred from a function-like XBE string/xref; not a confirmed PDB symbol.");
                }

                ImGui::TableSetColumnIndex(1);
                const bool selected = m_selected_label_index == (int)label_index;
                if (ImGui::Selectable(label.name.c_str(), selected,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    m_selected_label_index = (int)label_index;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        FollowDebuggerAddress(label.virtual_address, true);
                        m_debug_status = "Jumped to label " + label.name;
                    }
                }
                if (ImGui::BeginPopupContextItem("label_context")) {
                    if (ImGui::MenuItem("Jump Virtual")) {
                        FollowDebuggerAddress(label.virtual_address, true);
                    }
                    if (!physical_valid) {
                        ImGui::BeginDisabled();
                    }
                    if (ImGui::MenuItem("Jump Physical")) {
                        FollowDebuggerAddress(label.virtual_address, true);
                        char message[160];
                        std::snprintf(message, sizeof(message),
                                      "Jumped to %s at current Physical %08llX",
                                      label.name.c_str(),
                                      (unsigned long long)physical);
                        m_debug_status = message;
                    }
                    if (!physical_valid) {
                        ImGui::EndDisabled();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Copy Label")) {
                        ImGui::SetClipboardText(label.name.c_str());
                    }
                    char address_text[32];
                    std::snprintf(address_text, sizeof(address_text), "%08X",
                                  label.virtual_address);
                    if (ImGui::MenuItem("Copy Virtual Address")) {
                        ImGui::SetClipboardText(address_text);
                    }
                    if (!physical_valid) {
                        ImGui::BeginDisabled();
                    }
                    std::snprintf(address_text, sizeof(address_text), "%08llX",
                                  (unsigned long long)physical);
                    if (ImGui::MenuItem("Copy Physical Address")) {
                        ImGui::SetClipboardText(address_text);
                    }
                    if (!physical_valid) {
                        ImGui::EndDisabled();
                    }
                    ImGui::EndPopup();
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%08X", label.virtual_address);
                ImGui::TableSetColumnIndex(3);
                if (physical_valid) {
                    ImGui::Text("%08llX", (unsigned long long)physical);
                } else {
                    ImGui::TextDisabled("UNMAPPED");
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    const XemuXbeLabels::Label *selected_label = nullptr;
    uint64_t selected_physical = 0;
    bool selected_physical_valid = false;
    if (m_selected_label_index >= 0 &&
        (size_t)m_selected_label_index < database.labels.size()) {
        selected_label = &database.labels[(size_t)m_selected_label_index];
        selected_physical_valid = can_translate &&
            xemu_cheat_virtual_to_physical(selected_label->virtual_address,
                                           &selected_physical) != 0;
    } else {
        m_selected_label_index = -1;
    }

    if (selected_label == nullptr) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("JUMP VIRTUAL", ImVec2(125.0f, 0.0f)) && selected_label) {
        FollowDebuggerAddress(selected_label->virtual_address, true);
        m_debug_status = "Jumped to label " + selected_label->name;
    }
    if (selected_label == nullptr) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (selected_label == nullptr || !selected_physical_valid) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("JUMP PHYSICAL", ImVec2(125.0f, 0.0f)) && selected_label) {
        // The paired debugger is Virtual-mastered. Navigating the label's
        // stable Virtual address positions the Physical pane at its current
        // backing address without baking a launch-specific mapping into the DB.
        FollowDebuggerAddress(selected_label->virtual_address, true);
        char message[192];
        std::snprintf(message, sizeof(message),
                      "Jumped to %s at current Physical %08llX",
                      selected_label->name.c_str(),
                      (unsigned long long)selected_physical);
        m_debug_status = message;
    }
    if (selected_label == nullptr || !selected_physical_valid) {
        ImGui::EndDisabled();
    }
    if (selected_label != nullptr) {
        ImGui::SameLine();
        if (selected_physical_valid) {
            ImGui::TextDisabled("Selected V %08X -> P %08llX",
                                selected_label->virtual_address,
                                (unsigned long long)selected_physical);
        } else {
            ImGui::TextDisabled("Selected V %08X -> P UNMAPPED",
                                selected_label->virtual_address);
        }
    }

    if (!m_label_status.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_label_status.c_str());
    }
    ImGui::End();
}

void MemoryToolsWindow::DrawDumpRam()
{
    const uint64_t ram_size = xemu_cheat_ram_size();
    const uint64_t ram_mb = ram_size / (1024ull * 1024ull);

    ImGui::TextWrapped("Choose which RAM view to dump. Full-range dump actions pause the guest before reading and leave it paused afterward for debugger inspection.");
    ImGui::Spacing();
    if (ram_size != 0) {
        ImGui::Text("Installed Xbox RAM: %llu MB", (unsigned long long)ram_mb);
        const uint32_t physical_end = (uint32_t)(ram_size - 1);
        ImGui::Text("Physical: 00000000-%08X -> one complete .bin", physical_end);
        ImGui::Text("Virtual:  00000000-FFFFFFFF -> mapped RAM-backed regions only");
        ImGui::TextDisabled("Mapped virtual regions are discovered from the running Xbox page tables; MMIO/device mappings are excluded and aliases are preserved.");
    } else {
        ImGui::TextDisabled("Xbox RAM size is not available yet.");
    }

    const std::string dump_directory = DumpDirectory();
    if (!dump_directory.empty()) {
        ImGui::TextDisabled("Output folder: %s", dump_directory.c_str());
    } else {
        ImGui::TextDisabled("Output folder: <xemu.exe directory>\\Ram-Dumps");
    }

    ImGui::Spacing();
    if (ImGui::Button("DUMP PHYSICAL", ImVec2(320.0f, 0.0f))) {
        DumpPhysicalRam();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("One complete PHYSICAL .bin");

    if (ImGui::Button("DUMP MAPPED VIRTUAL RAM", ImVec2(320.0f, 0.0f))) {
        DumpMappedVirtualRam();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("VIRTUAL region .bin files + VIRTUAL-MAP.txt");

    if (ImGui::Button("DUMP PHYSICAL + DUMP MAPPED VIRTUAL RAM", ImVec2(320.0f, 0.0f))) {
        DumpFullRam();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Creates both dump sets from the same paused guest state");

    if (!m_dump_status.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_dump_status.c_str());
    }
}

void MemoryToolsWindow::Draw(bool detached)
{
    if (!is_open) {
        return;
    }

    m_detached_rendering = detached;

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_None;
    const char *window_name = "Memory Viewer / Search";
    bool *window_open = &is_open;
    if (detached) {
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
        window_name = "##DetachedMemoryViewerSearch";
        window_open = nullptr;
    } else {
        ImGui::SetNextWindowSize(ImVec2(1180, 720), ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin(window_name, window_open, window_flags)) {
        ImGui::End();
        m_detached_rendering = false;
        return;
    }

    current_game_manager.DrawInlineSummary("memory-tools-current-game");
    ImGui::Separator();

    if (!xemu_cheat_cpu_available()) {
        ImGui::TextUnformatted("Xbox CPU is not available yet. Start a game to use virtual memory tools.");
    }

    if (ImGui::BeginTabBar("memory_tools_tabs")) {
        const bool select_memory_tab = m_request_memory_tab;
        const bool select_debugger_tab = m_request_debugger_tab;
        if (select_memory_tab) {
            m_request_memory_tab = false;
        }
        if (select_debugger_tab) {
            m_request_debugger_tab = false;
        }
        if (ImGui::BeginTabItem(
                "Memory", nullptr,
                select_memory_tab ? ImGuiTabItemFlags_SetSelected
                                  : ImGuiTabItemFlags_None)) {
            DrawMemoryWorkspace();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Search / Compare")) {
            DrawSearch();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(
                "x86 Debugger", nullptr,
                select_debugger_tab ? ImGuiTabItemFlags_SetSelected
                                    : ImGuiTabItemFlags_None)) {
            DrawDebugger();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Dump RAM")) {
            DrawDumpRam();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();

    /* Keep debugger Inject editors independent of the active tab. Closing an
     * editor never silently changes guest memory; RESTORE remains explicit. */
    DrawInstructionChanger();
    DrawCodeCaveBuilder();
    DrawLabelBrowser();
    DrawBreakpointConditionEditor();
    m_detached_rendering = false;
}
