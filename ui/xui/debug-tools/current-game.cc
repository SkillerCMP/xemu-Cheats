//
// xemu Current Game Manager
//
// Copyright (C) 2026 xemu contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#include "current-game.hh"
#include "../font-manager.hh"

#include "xemu-xbe.h"
#include "disc-block-io.h"

#include <glib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

CurrentGameManager current_game_manager;

namespace {
constexpr uint64_t kRefreshIntervalMs = 500;
constexpr size_t kHashChunkSize = 1024 * 1024;

static std::string title_name_from_xbe(const struct xbe *xbe)
{
    if (!xbe || !xbe->cert) {
        return {};
    }

    glong items_written = 0;
    gchar *name = g_utf16_to_utf8(
        reinterpret_cast<const gunichar2 *>(xbe->cert->m_title_name),
        40, nullptr, &items_written, nullptr);
    if (!name) {
        return {};
    }

    std::string result(name);
    g_free(name);
    return result;
}

static std::string sha256_headers(const struct xbe *xbe)
{
    if (!xbe || !xbe->headers || xbe->headers_len == 0) {
        return {};
    }

    gchar *sum = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256,
        reinterpret_cast<const guchar *>(xbe->headers),
        xbe->headers_len);
    if (!sum) {
        return {};
    }

    std::string result(sum);
    g_free(sum);
    return result;
}

static bool range_inside(uint64_t offset, uint64_t size, uint64_t limit)
{
    return offset <= limit && size <= limit - offset;
}


static bool read_disc_file(XemuDiscBlockHandle blk, uint64_t image_size,
                           uint64_t offset, uint64_t size,
                           std::vector<uint8_t> &buffer, std::string &error)
{
    constexpr uint64_t kMaxXbeLabelBytes = 128ull * 1024ull * 1024ull;
    error.clear();
    buffer.clear();
    if (blk == nullptr || !range_inside(offset, size, image_size)) {
        error = "default.xbe points outside the mounted disc image.";
        return false;
    }
    if (size == 0 || size > kMaxXbeLabelBytes) {
        error = "default.xbe is too large for the label scanner safety limit.";
        return false;
    }
    buffer.resize((size_t)size);
    uint64_t position = 0;
    while (position < size) {
        const size_t amount = (size_t)std::min<uint64_t>(
            kHashChunkSize, size - position);
        if (!xemu_disc_block_pread(blk, offset + position,
                                   buffer.data() + position, amount)) {
            buffer.clear();
            error = "Unable to read default.xbe for label analysis.";
            return false;
        }
        position += amount;
    }
    return true;
}

static std::string sha256_disc_file(XemuDiscBlockHandle blk, uint64_t image_size,
                                    uint64_t offset, uint64_t size,
                                    std::string &error)
{
    error.clear();
    if (blk == nullptr || !range_inside(offset, size, image_size)) {
        error = "default.xbe points outside the mounted disc image.";
        return {};
    }

    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    if (checksum == nullptr) {
        error = "Unable to create SHA-256 checksum state.";
        return {};
    }

    std::vector<uint8_t> buffer(kHashChunkSize);
    uint64_t position = 0;
    while (position < size) {
        const size_t amount = (size_t)std::min<uint64_t>(buffer.size(),
                                                         size - position);
        if (!xemu_disc_block_pread(blk, offset + position, buffer.data(),
                                   amount)) {
            g_checksum_free(checksum);
            error = "Unable to read default.xbe from the mounted disc.";
            return {};
        }
        g_checksum_update(checksum, buffer.data(), amount);
        position += amount;
    }

    const gchar *sum = g_checksum_get_string(checksum);
    std::string result = sum != nullptr ? sum : "";
    g_checksum_free(checksum);
    if (result.empty()) {
        error = "Unable to finalize default.xbe SHA-256.";
    }
    return result;
}
}

bool CurrentGameManager::SameIdentity(const GameInfo &a, const GameInfo &b)
{
    if (a.valid != b.valid) {
        return false;
    }
    if (!a.valid) {
        return true;
    }
    return a.title_id == b.title_id &&
           a.header_sha256 == b.header_sha256 &&
           a.version == b.version &&
           a.disc_number == b.disc_number;
}

std::string CurrentGameManager::FormatTitleId(uint32_t title_id)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", title_id);
    return buf;
}

std::string CurrentGameManager::FormatDatabaseGameId(uint32_t title_id)
{
    const unsigned char a = (unsigned char)((title_id >> 24) & 0xFFu);
    const unsigned char b = (unsigned char)((title_id >> 16) & 0xFFu);
    if (a >= 0x21 && a <= 0x7E && b >= 0x21 && b <= 0x7E) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%c%c%04X", a, b,
                      title_id & 0xFFFFu);
        return buf;
    }
    return FormatTitleId(title_id);
}

std::string CurrentGameManager::FormatRegion(uint32_t region)
{
    std::string out;
    auto append = [&](const char *name) {
        if (!out.empty()) {
            out += " + ";
        }
        out += name;
    };

    // Xbox XBE certificate region flags.
    if (region & 0x00000001u) append("North America");
    if (region & 0x00000002u) append("Japan");
    if (region & 0x00000004u) append("Rest of World");
    if (region & 0x80000000u) append("Manufacturing");

    if (out.empty()) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "0x%08X", region);
        out = buf;
    }
    return out;
}

std::string CurrentGameManager::FormatByteSize(uint64_t bytes)
{
    char buf[64];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof(buf), "%llu B",
                      (unsigned long long)bytes);
    } else if (bytes < 1024ull * 1024ull) {
        std::snprintf(buf, sizeof(buf), "%.2f KiB", bytes / 1024.0);
    } else if (bytes < 1024ull * 1024ull * 1024ull) {
        std::snprintf(buf, sizeof(buf), "%.2f MiB",
                      bytes / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f GiB",
                      bytes / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

void CurrentGameManager::RefreshDisc(bool force)
{
    XemuDiscBlockHandle blk = xemu_disc_block_by_name("ide0-cd1");
    const uintptr_t backend_identity = xemu_disc_block_identity(blk);
    const bool available = xemu_disc_block_is_available(blk);
    const int64_t image_size = available ? xemu_disc_block_get_length(blk) : -1;

    const bool media_changed = backend_identity != m_disc_backend_identity ||
                               image_size != m_disc_image_size;
    if (!force && !media_changed) {
        return;
    }

    m_disc_backend_identity = backend_identity;
    m_disc_image_size = image_size;
    m_disc = {};
    m_disc_status.clear();
    m_info.disc_xbe_sha256.clear();
    m_info.disc_xbe_size = 0;
    m_info.disc_xbe_start_sector = 0;
    m_info.disc_xbe_offset = 0;
    m_labels = {};
    m_label_status.clear();

    if (blk == nullptr || backend_identity == 0 || image_size <= 0 ||
        !available) {
        m_disc_status = "No DVD media is currently mounted.";
        return;
    }

    auto reader = [blk, image_size](uint64_t offset, void *buffer,
                                    size_t size) -> bool {
        if (!range_inside(offset, size, (uint64_t)image_size)) {
            return false;
        }
        return xemu_disc_block_pread(blk, offset, buffer, size);
    };

    if (!XemuXdvdfs::Parse(reader, (uint64_t)image_size, m_disc,
                           m_disc_status)) {
        return;
    }

    const XemuXdvdfs::Entry *default_xbe =
        XemuXdvdfs::FindRootFile(m_disc, "default.xbe");
    if (default_xbe == nullptr) {
        m_disc_status = "XDVDFS loaded, but default.xbe was not found in the disc root.";
        return;
    }

    m_info.disc_xbe_size = default_xbe->size;
    m_info.disc_xbe_start_sector = default_xbe->start_sector;
    m_info.disc_xbe_offset = default_xbe->disc_offset;

    std::string hash_error;
    m_info.disc_xbe_sha256 = sha256_disc_file(
        blk, (uint64_t)image_size, default_xbe->disc_offset,
        default_xbe->size, hash_error);
    if (!hash_error.empty()) {
        m_disc_status = hash_error;
        return;
    }

    std::vector<uint8_t> xbe_file;
    std::string label_read_error;
    if (read_disc_file(blk, (uint64_t)image_size, default_xbe->disc_offset,
                       default_xbe->size, xbe_file, label_read_error)) {
        std::string label_error;
        if (XemuXbeLabels::Build(xbe_file, m_labels, label_error)) {
            char label_status[128];
            std::snprintf(label_status, sizeof(label_status),
                          "%zu XBE label(s) ready.", m_labels.labels.size());
            m_label_status = label_status;
        } else {
            m_label_status = label_error;
        }
    } else {
        m_label_status = label_read_error;
    }

    char status[160];
    std::snprintf(status, sizeof(status),
                  "XDVDFS loaded: %zu files, %zu folders.",
                  m_disc.file_count, m_disc.directory_count);
    m_disc_status = status;
}

void CurrentGameManager::Refresh(bool force)
{
    const uint64_t now = SDL_GetTicks();
    if (!force && m_last_refresh_ms != 0 &&
        now - m_last_refresh_ms < kRefreshIntervalMs) {
        return;
    }
    m_last_refresh_ms = now;

    GameInfo next;
    struct xbe *xbe = xemu_get_xbe_info();
    if (xbe && xbe->header && xbe->cert) {
        next.valid = true;
        next.title_id = xbe->cert->m_titleid;
        next.title_name = title_name_from_xbe(xbe);
        next.region = xbe->cert->m_game_region;
        next.disc_number = xbe->cert->m_disk_number;
        next.version = xbe->cert->m_version;
        next.xbe_base = xbe->header->m_base;
        next.xbe_image_size = xbe->header->m_sizeof_image;
        next.pe_checksum = xbe->header->m_pe_checksum;
        next.pe_timestamp = xbe->header->m_pe_timedate;
        next.header_sha256 = sha256_headers(xbe);

        const std::string title = FormatTitleId(next.title_id);
        const std::string short_hash = next.header_sha256.size() >= 16
                                           ? next.header_sha256.substr(0, 16)
                                           : next.header_sha256;
        next.revision_key = title + "-" + short_hash;
    }

    const bool game_changed = !SameIdentity(m_info, next);
    // Preserve disc-derived metadata until RefreshDisc decides whether the
    // mounted backend changed and needs to be rescanned.
    next.disc_xbe_sha256 = m_info.disc_xbe_sha256;
    next.disc_xbe_size = m_info.disc_xbe_size;
    next.disc_xbe_start_sector = m_info.disc_xbe_start_sector;
    next.disc_xbe_offset = m_info.disc_xbe_offset;
    m_info = std::move(next);

    if (game_changed) {
        ++m_generation;
    }

    RefreshDisc(force || game_changed);
}

void CurrentGameManager::DrawInlineSummary(const char *id) const
{
    ImGui::PushID(id);
    if (!m_info.valid) {
        ImGui::TextDisabled("Current Game: No XBE detected");
        ImGui::PopID();
        return;
    }

    const std::string title_id = FormatTitleId(m_info.title_id);
    const char *name = m_info.title_name.empty() ? "Unknown title" : m_info.title_name.c_str();
    ImGui::Text("Current Game: %s", name);
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]", title_id.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("Rev %08X / Disc %u", m_info.version,
                        m_info.disc_number);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("Region: %s", FormatRegion(m_info.region).c_str());
        ImGui::Text("Loaded Header SHA-256: %s", m_info.header_sha256.c_str());
        if (!m_info.disc_xbe_sha256.empty()) {
            ImGui::Text("Disc default.xbe SHA-256: %s",
                        m_info.disc_xbe_sha256.c_str());
        }
        ImGui::Text("Revision key: %s", m_info.revision_key.c_str());
        ImGui::EndTooltip();
    }
    ImGui::PopID();
}

void CurrentGameManager::DrawGameInfoTab()
{
    if (!m_info.valid) {
        ImGui::TextUnformatted("No running XBE detected.");
        ImGui::TextDisabled("Start a game or dashboard XBE and it will appear here automatically.");
        if (!m_info.disc_xbe_sha256.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("Disc default.xbe SHA-256   %s",
                               m_info.disc_xbe_sha256.c_str());
        }
        return;
    }

    ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    ImGui::Text("Title Name       %s", m_info.title_name.empty() ? "<unknown>" : m_info.title_name.c_str());
    ImGui::Text("Title ID         %s", FormatTitleId(m_info.title_id).c_str());
    ImGui::Text("Database GameID  %s", FormatDatabaseGameId(m_info.title_id).c_str());
    ImGui::Text("Region           %s", FormatRegion(m_info.region).c_str());
    ImGui::Text("Disc Number      %u", m_info.disc_number);
    ImGui::Text("Version          %08X", m_info.version);
    ImGui::Text("XBE Base         %08X", m_info.xbe_base);
    ImGui::Text("XBE Image Size   %08X", m_info.xbe_image_size);
    ImGui::Text("PE Checksum      %08X", m_info.pe_checksum);
    ImGui::Text("PE Timestamp     %08X", m_info.pe_timestamp);
    ImGui::Separator();
    ImGui::TextWrapped("Header SHA-256   %s", m_info.header_sha256.c_str());
    ImGui::TextWrapped("XBE SHA-256      %s",
                       m_info.disc_xbe_sha256.empty()
                           ? "<default.xbe unavailable>"
                           : m_info.disc_xbe_sha256.c_str());
    if (!m_info.disc_xbe_sha256.empty()) {
        ImGui::Text("XBE Disc Size    %08X", m_info.disc_xbe_size);
        ImGui::Text("XBE Start Sector %08X", m_info.disc_xbe_start_sector);
        ImGui::Text("XBE Disc Offset  %016llX",
                    (unsigned long long)m_info.disc_xbe_offset);
    }
    ImGui::Text("Revision Key     %s", m_info.revision_key.c_str());
    ImGui::Text("XBE Labels       %zu", m_labels.labels.size());
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::TextDisabled("Header SHA-256 hashes the loaded in-memory XBE header block.");
    ImGui::TextDisabled("XBE SHA-256 hashes the complete default.xbe file from the currently mounted DVD.");
    if (!m_disc_status.empty()) {
        ImGui::TextDisabled("Disc: %s", m_disc_status.c_str());
    }
    if (!m_label_status.empty()) {
        ImGui::TextDisabled("Labels: %s", m_label_status.c_str());
    }
}

void CurrentGameManager::DrawDiscEntry(const XemuXdvdfs::Entry &entry)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!entry.IsDirectory()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }

    const bool open = ImGui::TreeNodeEx((const void *)&entry, flags,
                                        "%s", entry.name.c_str());

    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(entry.IsDirectory() ? "Folder" : "File");

    ImGui::TableSetColumnIndex(2);
    const std::string size = FormatByteSize(entry.size);
    ImGui::TextUnformatted(entry.IsDirectory() ? "-" : size.c_str());

    ImGui::TableSetColumnIndex(3);
    ImGui::Text("%08X", entry.start_sector);

    ImGui::TableSetColumnIndex(4);
    ImGui::Text("%016llX", (unsigned long long)entry.disc_offset);

    if (entry.IsDirectory() && open) {
        for (const XemuXdvdfs::Entry &child : entry.children) {
            DrawDiscEntry(child);
        }
        ImGui::TreePop();
    }
}

void CurrentGameManager::DrawDiscContentsTab()
{
    if (!m_disc.valid) {
        ImGui::TextUnformatted("Disc contents are not available.");
        if (!m_disc_status.empty()) {
            ImGui::TextWrapped("%s", m_disc_status.c_str());
        }
        ImGui::Spacing();
        ImGui::TextDisabled("This viewer reads the XDVDFS filesystem directly from the currently mounted ide0-cd1 DVD backend.");
        return;
    }

    ImGui::Text("Xbox DVD (D:\\)   %zu files / %zu folders",
                m_disc.file_count, m_disc.directory_count);
    ImGui::SameLine();
    ImGui::TextDisabled("XDVDFS base %016llX",
                        (unsigned long long)m_disc.filesystem_base);
    if (!m_info.disc_xbe_sha256.empty()) {
        ImGui::TextDisabled("default.xbe SHA-256: %s",
                            m_info.disc_xbe_sha256.c_str());
    }
    ImGui::Separator();

    const ImGuiTableFlags table_flags = ImGuiTableFlags_BordersInnerV |
                                        ImGuiTableFlags_BordersOuter |
                                        ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_Resizable |
                                        ImGuiTableFlags_ScrollY |
                                        ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##xdvdfs_contents", 5, table_flags,
                          ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 95.0f);
        ImGui::TableSetupColumn("Sector", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Disc Offset", ImGuiTableColumnFlags_WidthFixed, 145.0f);
        ImGui::TableHeadersRow();

        for (const XemuXdvdfs::Entry &entry : m_disc.root_entries) {
            DrawDiscEntry(entry);
        }
        ImGui::EndTable();
    }
}

void CurrentGameManager::Draw()
{
    if (!is_open) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(860, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Current Game", &is_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Refresh Now")) {
        Refresh(true);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Reads the running XBE and currently mounted Xbox DVD");
    ImGui::Separator();

    if (ImGui::BeginTabBar("##current_game_tabs")) {
        if (ImGui::BeginTabItem("Game Info")) {
            DrawGameInfoTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Disc Contents")) {
            DrawDiscContentsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}
