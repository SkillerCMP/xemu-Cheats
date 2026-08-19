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

        // Snapshot guest 4 KiB pages only for this pane/frame. The viewer used
        // to issue one guest-memory read per visible 16-byte row, which meant
        // dozens of tiny reads for each Physical/Virtual pane every frame.
        // A frame-local cache keeps live-memory semantics unchanged while the
        // normal path becomes one read per visible page. A fixed four-entry
        // ring avoids adding a heap allocation to this hot draw path; even a
        // very tall viewport ordinarily touches only one or two pages.
        struct ViewerPageSnapshot {
            uint32_t base = 0;
            std::array<uint8_t, kPageSize> bytes;
            bool valid = false;
            bool readable = false;
        };
        std::array<ViewerPageSnapshot, 4> viewer_page_cache;
        size_t viewer_page_count = 0;
        size_t viewer_page_replacement = 0;

        auto get_viewer_page = [&](uint32_t page_base)
            -> ViewerPageSnapshot & {
            for (ViewerPageSnapshot &page : viewer_page_cache) {
                if (page.valid && page.base == page_base) {
                    return page;
                }
            }

            size_t slot = 0;
            if (viewer_page_count < viewer_page_cache.size()) {
                slot = viewer_page_count++;
            } else {
                slot = viewer_page_replacement++ % viewer_page_cache.size();
            }
            ViewerPageSnapshot &page = viewer_page_cache[slot];
            page.base = page_base;
            page.valid = true;
            const size_t page_bytes = (size_t)std::min<uint64_t>(
                kPageSize, 0x100000000ull - (uint64_t)page_base);
            page.readable = Read(space, page_base, page.bytes.data(), page_bytes);
            return page;
        };

        auto read_viewer_cached = [&](uint32_t address, uint8_t *out,
                                      size_t size) -> bool {
            const uint32_t original_address = address;
            const size_t original_size = size;
            uint8_t *dst = out;
            size_t remaining = size;
            while (remaining != 0) {
                const uint32_t page_base = address & ~(uint32_t)(kPageSize - 1);
                const size_t page_offset = (size_t)(address - page_base);
                const size_t chunk = std::min(remaining, kPageSize - page_offset);
                ViewerPageSnapshot &page = get_viewer_page(page_base);
                if (!page.readable) {
                    // Preserve the previous short-read behavior for unusual
                    // partial mappings/MMIO where a whole-page read can fail
                    // even though the requested row is readable.
                    return Read(space, original_address, out, original_size);
                }
                std::memcpy(dst, page.bytes.data() + page_offset, chunk);
                dst += chunk;
                remaining -= chunk;
                address += (uint32_t)chunk;
            }
            return true;
        };

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
                const bool ok = read_viewer_cached(row_address, bytes,
                                                   bytes_to_read);
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

bool MemoryToolsWindow::CollectRamVirtualPages(
    uint64_t ram_size, std::vector<VirtualPageMapping> &pages,
    bool &used_page_table_snapshot) const
{
    constexpr uint64_t kVirtualSize = 0x100000000ull;
    constexpr uint64_t kVirtualPage = 0x1000ull;

    pages.clear();
    used_page_table_snapshot = false;

    XemuCheatVirtualMapping *mappings = nullptr;
    size_t mapping_count = 0;
    if (xemu_cheat_collect_ram_virtual_mappings(ram_size, &mappings,
                                                &mapping_count)) {
        for (size_t i = 0; i < mapping_count; ++i) {
            const auto &mapping = mappings[i];
            for (uint64_t offset = 0; offset < mapping.length;
                 offset += kVirtualPage) {
                const uint64_t virtual_address =
                    (uint64_t)mapping.virtual_start + offset;
                const uint64_t physical_address =
                    mapping.physical_start + offset;
                if (virtual_address >= kVirtualSize || physical_address >= ram_size) {
                    break;
                }
                pages.push_back({(uint32_t)virtual_address,
                                 physical_address & ~(kVirtualPage - 1)});
            }
        }
        xemu_cheat_free_virtual_mappings(mappings);

        if (!pages.empty()) {
            std::sort(pages.begin(), pages.end(),
                      [](const VirtualPageMapping &a,
                         const VirtualPageMapping &b) {
                          if (a.virtual_page != b.virtual_page) {
                              return a.virtual_page < b.virtual_page;
                          }
                          return a.physical_page < b.physical_page;
                      });
            pages.erase(std::unique(
                            pages.begin(), pages.end(),
                            [](const VirtualPageMapping &a,
                               const VirtualPageMapping &b) {
                                return a.virtual_page == b.virtual_page &&
                                       a.physical_page == b.physical_page;
                            }),
                        pages.end());
            used_page_table_snapshot = true;
            return true;
        }
    } else {
        xemu_cheat_free_virtual_mappings(mappings);
    }

    /* Compatibility fallback for accelerators/targets where QEMU cannot expose
     * a page-table snapshot. This is the exact legacy 4 GiB page-probe path. */
    if (!xemu_cheat_prepare_virtual_map()) {
        return false;
    }
    const size_t physical_page_count =
        (size_t)((ram_size + kVirtualPage - 1) / kVirtualPage);
    pages.reserve(physical_page_count * 2);
    for (uint64_t virtual_address = 0;
         virtual_address < kVirtualSize;
         virtual_address += kVirtualPage) {
        uint64_t physical_address = 0;
        if (xemu_cheat_virtual_to_physical((uint32_t)virtual_address,
                                           &physical_address) &&
            physical_address < ram_size) {
            pages.push_back({(uint32_t)virtual_address,
                             physical_address & ~(kVirtualPage - 1)});
        }
    }
    return true;
}

bool MemoryToolsWindow::RefreshMemoryMap()
{
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

    bool used_page_table_snapshot = false;
    if (!CollectRamVirtualPages(ram_size, m_virtual_page_map,
                                used_page_table_snapshot)) {
        m_memory_map_status = "Could not synchronize/read the Xbox page tables.";
        return false;
    }

    const size_t physical_page_count =
        (size_t)((ram_size + kVirtualPage - 1) / kVirtualPage);
    std::vector<uint8_t> seen_physical_pages(physical_page_count, 0);
    m_physical_alias_page_index.reserve(m_virtual_page_map.size());
    m_memory_map_regions.reserve(m_virtual_page_map.size() / 16 + 1);

    bool in_region = false;
    MemoryMapRegion current = {};
    for (const auto &mapping : m_virtual_page_map) {
        const uint64_t virtual_address = mapping.virtual_page;
        const uint64_t physical_page = mapping.physical_page;

        m_physical_alias_page_index.push_back(
            {(uint32_t)physical_page, mapping.virtual_page});

        const size_t physical_index = (size_t)(physical_page / kVirtualPage);
        if (physical_index < seen_physical_pages.size()) {
            seen_physical_pages[physical_index] = 1;
        }

        /* A displayed region is only merged when BOTH sides are contiguous. */
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
    char status[384];
    std::snprintf(status, sizeof(status),
                  "Map refreshed via %s: %llu RAM-backed virtual page(s), %llu unique physical page(s), "
                  "%llu alias page(s), %zu linear region(s).",
                  used_page_table_snapshot ? "page-table snapshot" : "legacy page scan",
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
