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
#pragma once

#include "../common.hh"

#include <cstdint>
#include <string>

#include "xdvdfs-disc.hh"
#include "xbe-labels.hh"

class CurrentGameManager
{
public:
    struct GameInfo {
        bool valid = false;
        uint32_t title_id = 0;
        std::string title_name;
        uint32_t region = 0;
        uint32_t disc_number = 0;
        uint32_t version = 0;
        uint32_t xbe_base = 0;
        uint32_t xbe_image_size = 0;
        uint32_t pe_checksum = 0;
        uint32_t pe_timestamp = 0;
        std::string header_sha256;
        std::string disc_xbe_sha256;
        uint32_t disc_xbe_size = 0;
        uint32_t disc_xbe_start_sector = 0;
        uint64_t disc_xbe_offset = 0;
        std::string revision_key;
    };

    bool is_open = false;

    void Refresh(bool force = false);
    void Draw();
    void DrawInlineSummary(const char *id) const;

    const GameInfo &Get() const { return m_info; }
    uint64_t Generation() const { return m_generation; }
    bool HasGame() const { return m_info.valid; }
    const XemuXbeLabels::Database &Labels() const { return m_labels; }
    const XemuXbeLabels::Label *PrimaryLabelAt(uint32_t virtual_address) const
    {
        return XemuXbeLabels::PrimaryAt(m_labels, virtual_address);
    }
    const std::string &LabelStatus() const { return m_label_status; }

    static std::string FormatTitleId(uint32_t title_id);
    static std::string FormatDatabaseGameId(uint32_t title_id);
    static std::string FormatRegion(uint32_t region);

private:
    GameInfo m_info;
    uint64_t m_generation = 0;
    uint64_t m_last_refresh_ms = 0;
    XemuXdvdfs::Disc m_disc;
    std::string m_disc_status;
    XemuXbeLabels::Database m_labels;
    std::string m_label_status;
    uintptr_t m_disc_backend_identity = 0;
    int64_t m_disc_image_size = 0;

    void RefreshDisc(bool force);
    void DrawGameInfoTab();
    void DrawDiscContentsTab();
    static void DrawDiscEntry(const XemuXdvdfs::Entry &entry);
    static std::string FormatByteSize(uint64_t bytes);

    static bool SameIdentity(const GameInfo &a, const GameInfo &b);
};

extern CurrentGameManager current_game_manager;
