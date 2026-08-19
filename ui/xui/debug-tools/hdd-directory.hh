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
#pragma once

#include "fatx-hdd.hh"

#include <cstdint>
#include <string>
#include <vector>

class HddDirectoryWindow
{
public:
    bool is_open = false;

    void Draw(bool detached = false);
    void DrawCurrentGameHdd(uint32_t title_id);
    void Refresh();

    const XemuFatxHdd::Snapshot &Snapshot() const { return m_snapshot; }
    bool HasSnapshot() const { return m_has_snapshot; }
    const std::string &Status() const { return m_status; }

private:
    struct ExportTarget {
        char partition = '?';
        std::vector<std::string> path;
        bool directory = false;
    };

    XemuFatxHdd::Snapshot m_snapshot;
    bool m_has_snapshot = false;
    std::string m_status;
    std::string m_export_status;

    void DrawEntries(const XemuFatxHdd::Partition &partition,
                     const std::vector<XemuFatxHdd::Entry> &entries,
                     std::vector<std::string> &path,
                     bool current_game_view = false);
    void DrawCurrentGameArea(const XemuFatxHdd::Partition &partition,
                             const XemuFatxHdd::Entry *area,
                             const std::string &title_id,
                             const char *area_name,
                             const char *description);
    void DrawExportContext(const XemuFatxHdd::Partition &partition,
                           const XemuFatxHdd::Entry &entry,
                           const std::vector<std::string> &path,
                           bool save_folder);
    void RequestExport(const ExportTarget &target);
    bool ExportToHost(const ExportTarget &target,
                      const std::string &destination,
                      std::string &error);
    bool ExportEntryRecursive(void *hdd,
                              const XemuFatxHdd::Partition &partition,
                              const XemuFatxHdd::Entry &entry,
                              const std::string &host_path,
                              size_t depth,
                              size_t &file_count,
                              uint64_t &byte_count,
                              std::string &error);

    static std::string HostSafeName(const std::string &name);
    static std::string FormatByteSize(uint64_t bytes);
    static std::string FormatAttributes(uint8_t attributes);
};

extern HddDirectoryWindow hdd_directory_window;
