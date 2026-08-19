//
// xemu Xbox HDD Directory Viewer
//
// Copyright (C) 2026 xemu contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#include "hdd-directory.hh"

#include "disc-block-io.h"
#include "guest-pause-guard.hh"
#include "../misc.hh"

#include <glib/gstdio.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

HddDirectoryWindow hdd_directory_window;

namespace {

bool ReadHddBlock(void *opaque, uint64_t offset, void *buffer, size_t size)
{
    return xemu_disc_block_pread((XemuDiscBlockHandle)opaque, offset, buffer,
                                 size);
}

bool WriteHostFile(void *opaque, const void *buffer, size_t size)
{
    FILE *fp = static_cast<FILE *>(opaque);
    return fp && (size == 0 || std::fwrite(buffer, 1, size, fp) == size);
}

bool EqualsNoCase(const std::string &a, const std::string &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

const XemuFatxHdd::Entry *FindChildNoCase(
    const std::vector<XemuFatxHdd::Entry> &entries, const std::string &name)
{
    for (const XemuFatxHdd::Entry &entry : entries) {
        if (EqualsNoCase(entry.name, name)) {
            return &entry;
        }
    }
    return nullptr;
}

std::string UniqueHostPath(const std::string &requested)
{
    namespace fs = std::filesystem;
    fs::path path(requested);
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return path.string();
    }

    const fs::path parent = path.parent_path();
    const std::string stem = path.stem().string();
    const std::string extension = path.extension().string();
    for (unsigned i = 1; i < 10000; ++i) {
        fs::path candidate = parent / (stem + " (" + std::to_string(i) + ")" + extension);
        ec.clear();
        if (!fs::exists(candidate, ec)) {
            return candidate.string();
        }
    }
    return requested;
}

} // namespace

std::string HddDirectoryWindow::FormatByteSize(uint64_t bytes)
{
    char buffer[64];
    if (bytes < 1024) {
        std::snprintf(buffer, sizeof(buffer), "%llu B",
                      (unsigned long long)bytes);
    } else if (bytes < 1024ull * 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KiB", bytes / 1024.0);
    } else if (bytes < 1024ull * 1024ull * 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%.2f MiB",
                      bytes / (1024.0 * 1024.0));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f GiB",
                      bytes / (1024.0 * 1024.0 * 1024.0));
    }
    return buffer;
}

std::string HddDirectoryWindow::FormatAttributes(uint8_t attributes)
{
    std::string out;
    if (attributes & 0x01) out += 'R';
    if (attributes & 0x02) out += 'S';
    if (attributes & 0x04) out += 'H';
    if (attributes & 0x08) out += 'V';
    if (attributes & 0x10) out += 'D';
    return out.empty() ? "-" : out;
}

std::string HddDirectoryWindow::HostSafeName(const std::string &name)
{
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            out.push_back('_');
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.back() = '_';
    }
    if (out.empty() || out == "." || out == "..") {
        out = "_";
    }
    return out;
}

void HddDirectoryWindow::Refresh()
{
    m_has_snapshot = true;
    m_snapshot = {};
    m_export_status.clear();

    XemuDiscBlockHandle hdd = xemu_disc_block_by_name("ide0-hd0");
    if (!xemu_disc_block_is_available(hdd)) {
        m_status = "Xbox HDD (ide0-hd0) is not available.";
        return;
    }

    const int64_t length = xemu_disc_block_get_length(hdd);
    if (length <= 0) {
        m_status = "Unable to determine Xbox HDD size.";
        return;
    }

    // FATX directory/FAT metadata can be modified by the running guest. Build
    // one coherent read-only snapshot while the VM is stopped, including the
    // small TitleMeta/SaveMeta files used only for display names. Browsing then
    // touches no live HDD state again until REFRESH or an explicit export.
    XemuDebugGuestPauseGuard pause;
    XemuFatxHdd::BuildSnapshot(ReadHddBlock, hdd, (uint64_t)length, m_snapshot);
    std::string metadata_warning;
    XemuFatxHdd::PopulateXboxMetadata(ReadHddBlock, hdd, (uint64_t)length,
                                      m_snapshot, metadata_warning);
    m_status = m_snapshot.status;
    if (!metadata_warning.empty()) {
        m_status += " " + metadata_warning;
    }
}

bool HddDirectoryWindow::ExportEntryRecursive(
    void *hdd, const XemuFatxHdd::Partition &partition,
    const XemuFatxHdd::Entry &entry, const std::string &host_path,
    size_t depth, size_t &file_count, uint64_t &byte_count, std::string &error)
{
    namespace fs = std::filesystem;
    if (depth > 64) {
        error = "Export directory depth exceeds safety limit.";
        return false;
    }

    if (entry.directory) {
        std::error_code ec;
        if (!fs::create_directories(fs::path(host_path), ec) && ec) {
            error = "Unable to create export folder: " + host_path;
            return false;
        }
        for (const XemuFatxHdd::Entry &child : entry.children) {
            const std::string child_path =
                (fs::path(host_path) / HostSafeName(child.name)).string();
            if (!ExportEntryRecursive(hdd, partition, child, child_path,
                                      depth + 1, file_count, byte_count, error)) {
                return false;
            }
        }
        return true;
    }

    FILE *fp = g_fopen(host_path.c_str(), "wb");
    if (!fp) {
        error = "Unable to create export file: " + host_path;
        return false;
    }

    const int64_t length = xemu_disc_block_get_length((XemuDiscBlockHandle)hdd);
    bool ok = length > 0 && XemuFatxHdd::StreamFile(
        ReadHddBlock, hdd, (uint64_t)length, partition, entry,
        WriteHostFile, fp, error);
    if (std::fclose(fp) != 0 && ok) {
        error = "Unable to finalize export file: " + host_path;
        ok = false;
    }
    if (!ok) {
        g_remove(host_path.c_str());
        return false;
    }

    ++file_count;
    byte_count += entry.file_size;
    return true;
}

bool HddDirectoryWindow::ExportToHost(const ExportTarget &target,
                                      const std::string &destination,
                                      std::string &error)
{
    error.clear();
    XemuDiscBlockHandle hdd = xemu_disc_block_by_name("ide0-hd0");
    if (!xemu_disc_block_is_available(hdd)) {
        error = "Xbox HDD (ide0-hd0) is not available.";
        return false;
    }
    const int64_t length = xemu_disc_block_get_length(hdd);
    if (length <= 0) {
        error = "Unable to determine Xbox HDD size.";
        return false;
    }

    // Rebuild a fresh metadata snapshot inside the same pause transaction used
    // for exporting. The user may browse an older snapshot for a while; export
    // must resolve the selected FATX path again before reading any file bytes.
    XemuDebugGuestPauseGuard pause;
    XemuFatxHdd::Snapshot fresh;
    if (!XemuFatxHdd::BuildSnapshot(ReadHddBlock, hdd, (uint64_t)length, fresh)) {
        error = fresh.status.empty() ? "Unable to refresh FATX metadata for export."
                                     : fresh.status;
        return false;
    }

    const XemuFatxHdd::Partition *partition =
        XemuFatxHdd::FindPartition(fresh, target.partition);
    if (!partition || !partition->available) {
        error = "The selected FATX partition is no longer available.";
        return false;
    }
    const XemuFatxHdd::Entry *entry = XemuFatxHdd::FindEntry(*partition, target.path);
    if (!entry || entry->directory != target.directory) {
        error = "The selected HDD item changed or no longer exists. Refresh and try again.";
        return false;
    }

    namespace fs = std::filesystem;
    const std::string leaf = target.path.empty() ? "Xbox HDD Export"
                                                 : HostSafeName(target.path.back());
    const std::string host_path =
        UniqueHostPath((fs::path(destination) / leaf).string());

    size_t file_count = 0;
    uint64_t byte_count = 0;
    if (!ExportEntryRecursive(hdd, *partition, *entry, host_path, 0,
                              file_count, byte_count, error)) {
        if (entry->directory) {
            std::error_code cleanup_error;
            fs::remove_all(fs::path(host_path), cleanup_error);
        }
        return false;
    }

    char status[320];
    if (entry->directory) {
        std::snprintf(status, sizeof(status),
                      "Exported %zu file(s), %s to %s", file_count,
                      FormatByteSize(byte_count).c_str(), host_path.c_str());
    } else {
        std::snprintf(status, sizeof(status), "Exported %s to %s",
                      FormatByteSize(entry->file_size).c_str(), host_path.c_str());
    }
    m_export_status = status;
    return true;
}

void HddDirectoryWindow::RequestExport(const ExportTarget &target)
{
    // Folder selection keeps the original FATX filename/folder name intact and
    // works the same for individual files, whole saves, and title-data trees.
    ShowOpenFolderDialog(nullptr, [this, target](const char *path) {
        std::string error;
        if (!ExportToHost(target, path, error)) {
            m_export_status = "Export failed: " + error;
        }
    });
}

void HddDirectoryWindow::DrawExportContext(
    const XemuFatxHdd::Partition &partition, const XemuFatxHdd::Entry &entry,
    const std::vector<std::string> &path, bool save_folder)
{
    if (!ImGui::BeginPopupContextItem()) {
        return;
    }

    ExportTarget target;
    target.partition = partition.letter;
    target.path = path;
    target.directory = entry.directory;

    const char *label = entry.directory
        ? (save_folder ? "EXPORT SAVE FOLDER..." : "EXPORT FOLDER...")
        : "EXPORT FILE...";
    if (ImGui::MenuItem(label)) {
        RequestExport(target);
    }
    ImGui::TextDisabled("Read-only: exports never modify the Xbox HDD.");
    ImGui::EndPopup();
}

void HddDirectoryWindow::DrawEntries(
    const XemuFatxHdd::Partition &partition,
    const std::vector<XemuFatxHdd::Entry> &entries,
    std::vector<std::string> &path, bool current_game_view)
{
    for (const XemuFatxHdd::Entry &entry : entries) {
        std::vector<std::string> entry_path = path;
        entry_path.push_back(entry.name);
        const bool save_folder = current_game_view && entry.directory &&
            entry_path.size() == 3 && EqualsNoCase(entry_path[0], "UDATA");
        const std::string display_name = XemuFatxHdd::DisplayName(entry);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);

        if (entry.directory) {
            const bool open = ImGui::TreeNodeEx(
                (const void *)&entry,
                ImGuiTreeNodeFlags_SpanFullWidth |
                    ImGuiTreeNodeFlags_OpenOnArrow |
                    ImGuiTreeNodeFlags_OpenOnDoubleClick,
                "%s", display_name.c_str());
            DrawExportContext(partition, entry, entry_path, save_folder);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(save_folder ? "Save" : "Directory");
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted("-");
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%08X", entry.first_cluster);
            ImGui::TableSetColumnIndex(4);
            const std::string modified = XemuFatxHdd::FormatTimestamp(
                entry.modified_date, entry.modified_time);
            ImGui::TextUnformatted(modified.empty() ? "-" : modified.c_str());
            ImGui::TableSetColumnIndex(5);
            const std::string attrs = FormatAttributes(entry.attributes);
            ImGui::TextUnformatted(attrs.c_str());

            if (open) {
                path.push_back(entry.name);
                DrawEntries(partition, entry.children, path, current_game_view);
                path.pop_back();
                ImGui::TableSetColumnIndex(0);
                ImGui::TreePop();
            }
        } else {
            ImGui::TreeNodeEx(
                (const void *)&entry,
                ImGuiTreeNodeFlags_Leaf |
                    ImGuiTreeNodeFlags_NoTreePushOnOpen |
                    ImGuiTreeNodeFlags_SpanFullWidth,
                "%s", display_name.c_str());
            DrawExportContext(partition, entry, entry_path, false);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted((entry.attributes & 0x08) ? "Volume" : "File");
            ImGui::TableSetColumnIndex(2);
            const std::string size = FormatByteSize(entry.file_size);
            ImGui::TextUnformatted(size.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%08X", entry.first_cluster);
            ImGui::TableSetColumnIndex(4);
            const std::string modified = XemuFatxHdd::FormatTimestamp(
                entry.modified_date, entry.modified_time);
            ImGui::TextUnformatted(modified.empty() ? "-" : modified.c_str());
            ImGui::TableSetColumnIndex(5);
            const std::string attrs = FormatAttributes(entry.attributes);
            ImGui::TextUnformatted(attrs.c_str());
        }
    }
}

void HddDirectoryWindow::DrawCurrentGameArea(
    const XemuFatxHdd::Partition &partition, const XemuFatxHdd::Entry *area,
    const std::string &title_id, const char *area_name, const char *description)
{
    ImGui::TextDisabled("%s", description);
    if (!area || !area->directory) {
        ImGui::TextDisabled("E:\\%s is not present on this HDD.", area_name);
        return;
    }

    const XemuFatxHdd::Entry *title = FindChildNoCase(area->children, title_id);
    if (!title || !title->directory) {
        ImGui::TextDisabled("No %s data found for Title ID %s.", area_name,
                            title_id.c_str());
        return;
    }

    const std::string title_display = XemuFatxHdd::DisplayName(*title);
    ImGui::Text("E:\\%s\\%s", area_name, title_display.c_str());
    ImGui::SameLine();
    ExportTarget whole;
    whole.partition = partition.letter;
    whole.path = {area->name, title->name};
    whole.directory = true;
    const char *button = EqualsNoCase(area_name, "UDATA")
        ? "EXPORT ALL SAVES..." : "EXPORT TITLE DATA...";
    if (ImGui::Button(button)) {
        RequestExport(whole);
    }
    ImGui::Separator();

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##current_game_hdd_entries", 6, flags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Cluster", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Attr", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableHeadersRow();
        std::vector<std::string> path{area->name, title->name};
        DrawEntries(partition, title->children, path, true);
        ImGui::EndTable();
    }
}

void HddDirectoryWindow::DrawCurrentGameHdd(uint32_t title_id)
{
    if (!m_has_snapshot) {
        Refresh();
    }

    if (title_id == 0) {
        ImGui::TextUnformatted("No running XBE detected.");
        ImGui::TextDisabled("Start a game to filter E:\\UDATA and E:\\TDATA by its Title ID.");
        return;
    }

    if (ImGui::Button("REFRESH HDD")) {
        Refresh();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Read-only FATX snapshot; export reads the selected item again while paused.");
    if (!m_status.empty()) {
        ImGui::TextWrapped("%s", m_status.c_str());
    }
    if (!m_export_status.empty()) {
        ImGui::TextWrapped("%s", m_export_status.c_str());
    }

    const XemuFatxHdd::Partition *data = XemuFatxHdd::FindPartition(m_snapshot, 'E');
    if (!data || !data->available) {
        ImGui::TextDisabled("The E: FATX partition is not available.");
        return;
    }

    char title_text[16];
    std::snprintf(title_text, sizeof(title_text), "%08X", title_id);
    const XemuFatxHdd::Entry *udata = FindChildNoCase(data->entries, "UDATA");
    const XemuFatxHdd::Entry *tdata = FindChildNoCase(data->entries, "TDATA");

    if (ImGui::BeginTabBar("##current_game_hdd_tabs")) {
        if (ImGui::BeginTabItem("Saves / UDATA")) {
            DrawCurrentGameArea(*data, udata, title_text, "UDATA",
                                "Xbox game saves for the running Title ID.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("DLC / TDATA")) {
            DrawCurrentGameArea(*data, tdata, title_text, "TDATA",
                                "Title-specific data / DLC for the running Title ID.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void HddDirectoryWindow::Draw(bool detached)
{
    if (!is_open) {
        return;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    const char *window_name = "Xbox HDD Directory";
    bool *window_open = &is_open;
    if (detached) {
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
        flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
        window_name = "##DetachedXboxHddDirectory";
        window_open = nullptr;
    } else {
        ImGui::SetNextWindowSize(ImVec2(1000, 680), ImGuiCond_FirstUseEver);
    }

    if (!ImGui::Begin(window_name, window_open, flags)) {
        ImGui::End();
        return;
    }

    if (!m_has_snapshot) {
        Refresh();
    }

    if (ImGui::Button("REFRESH")) {
        Refresh();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Read-only snapshot of the mounted Xbox HDD (ide0-hd0)");

    if (!m_status.empty()) {
        ImGui::TextWrapped("%s", m_status.c_str());
    }
    if (m_snapshot.hdd_available) {
        ImGui::SameLine();
        ImGui::TextDisabled("HDD size: %s",
                            FormatByteSize(m_snapshot.image_size).c_str());
    }
    if (!m_export_status.empty()) {
        ImGui::TextWrapped("%s", m_export_status.c_str());
    }
    ImGui::TextDisabled("Right-click a file/folder to export it. TitleMeta.xbx and SaveMeta.xbx names are display-only.");
    ImGui::Separator();

    if (m_snapshot.partitions.empty()) {
        ImGui::TextDisabled("No FATX partition snapshot is available.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##hdd_partitions")) {
        for (const XemuFatxHdd::Partition &part : m_snapshot.partitions) {
            char tab_name[48];
            std::snprintf(tab_name, sizeof(tab_name), "%c: %s", part.letter,
                          part.label.c_str());
            if (!ImGui::BeginTabItem(tab_name)) {
                continue;
            }

            ImGui::Text("Offset: %016llX    Size: %s",
                        (unsigned long long)part.offset,
                        FormatByteSize(part.size).c_str());
            if (part.available) {
                ImGui::SameLine();
                ImGui::TextDisabled("Volume %08X | FAT%u | Cluster %u KiB",
                                    part.volume_id, part.fat_bits,
                                    part.bytes_per_cluster / 1024);
            }
            ImGui::TextWrapped("%s", part.status.c_str());

            if (part.available) {
                const ImGuiTableFlags table_flags =
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                    ImGuiTableFlags_SizingStretchProp;
                if (ImGui::BeginTable("##fatx_directory", 6, table_flags,
                                      ImVec2(0, 0))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch,
                                            3.0f);
                    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed,
                                            90.0f);
                    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed,
                                            90.0f);
                    ImGui::TableSetupColumn("Cluster", ImGuiTableColumnFlags_WidthFixed,
                                            90.0f);
                    ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed,
                                            150.0f);
                    ImGui::TableSetupColumn("Attr", ImGuiTableColumnFlags_WidthFixed,
                                            55.0f);
                    ImGui::TableHeadersRow();
                    std::vector<std::string> path;
                    DrawEntries(part, part.entries, path, false);
                    ImGui::EndTable();
                }
            }

            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}
