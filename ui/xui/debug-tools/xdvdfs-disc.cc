//
// xemu RAW Cheat Engine - read-only XDVDFS disc browser helpers
//
// Copyright (C) 2026 xemu contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#include "xdvdfs-disc.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace XemuXdvdfs {
namespace {

constexpr uint64_t kSectorSize = 0x800;
constexpr uint64_t kVolumeDescriptorOffset = 0x10000;
constexpr size_t kMagicLength = 20;
constexpr char kMagic[kMagicLength + 1] = "MICROSOFT*XBOX*MEDIA";
constexpr uint8_t kDirectoryAttribute = 0x10;
constexpr uint64_t kMaxDirectoryTableSize = 64ull * 1024ull * 1024ull;
constexpr size_t kMaxEntries = 100000;
constexpr unsigned kMaxDirectoryDepth = 64;

// extract-xiso supports these common image layouts. Normal xemu XISOs use
// base zero; the additional bases let the browser remain useful with images
// whose XDVDFS game partition begins later in the container image.
constexpr std::array<uint64_t, 4> kFilesystemBases = {
    0x00000000ull,
    0x0FD90000ull,
    0x02080000ull,
    0x18300000ull,
};

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool range_inside(uint64_t offset, uint64_t size, uint64_t limit)
{
    return offset <= limit && size <= limit - offset;
}

static std::string safe_name(const uint8_t *p, size_t len)
{
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = p[i];
        if (c >= 0x20 && c <= 0x7e && c != '/' && c != '\\') {
            out.push_back((char)c);
        } else {
            out.push_back('?');
        }
    }
    return out;
}

static bool ascii_equal_case_insensitive(const std::string &a, const char *b)
{
    if (b == nullptr || a.size() != std::strlen(b)) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const unsigned char ac = (unsigned char)a[i];
        const unsigned char bc = (unsigned char)b[i];
        if (std::tolower(ac) != std::tolower(bc)) {
            return false;
        }
    }
    return true;
}

struct Parser {
    const Reader &reader;
    uint64_t image_size;
    uint64_t base;
    Disc &disc;
    std::string &error;
    size_t entries_seen = 0;

    bool ReadDirectory(uint32_t start_sector, uint32_t table_size,
                       std::vector<Entry> &out, unsigned depth)
    {
        if (table_size == 0) {
            return true;
        }
        if (depth > kMaxDirectoryDepth) {
            error = "XDVDFS directory nesting exceeds the safety limit.";
            return false;
        }
        if (table_size > kMaxDirectoryTableSize) {
            error = "XDVDFS directory table is unreasonably large.";
            return false;
        }

        const uint64_t directory_offset = base + (uint64_t)start_sector * kSectorSize;
        if (!range_inside(directory_offset, table_size, image_size)) {
            error = "XDVDFS directory table points outside the mounted disc image.";
            return false;
        }

        std::vector<uint8_t> table(table_size);
        if (!reader(directory_offset, table.data(), table.size())) {
            error = "Unable to read an XDVDFS directory table from the mounted disc.";
            return false;
        }

        std::unordered_set<uint32_t> visited;
        return WalkEntry(table, 0, out, depth, visited);
    }

    bool WalkEntry(const std::vector<uint8_t> &table, uint32_t offset,
                   std::vector<Entry> &out, unsigned depth,
                   std::unordered_set<uint32_t> &visited)
    {
        if (offset >= table.size()) {
            error = "XDVDFS directory entry offset is outside its directory table.";
            return false;
        }
        if (!visited.insert(offset).second) {
            error = "XDVDFS directory tree contains a cycle.";
            return false;
        }
        if (++entries_seen > kMaxEntries) {
            error = "XDVDFS disc contains more entries than the browser safety limit.";
            return false;
        }
        if (table.size() - offset < 14) {
            error = "XDVDFS directory entry is truncated.";
            return false;
        }

        const uint8_t *p = table.data() + offset;
        const uint16_t left_dwords = read_le16(p + 0);
        const uint16_t right_dwords = read_le16(p + 2);

        // 0xFFFF marks padding/empty directory data. At offset zero this means
        // the directory contains no entries.
        if (left_dwords == 0xFFFFu) {
            return true;
        }

        const uint32_t start_sector = read_le32(p + 4);
        const uint32_t file_size = read_le32(p + 8);
        const uint8_t attributes = p[12];
        const uint8_t filename_length = p[13];
        if (filename_length == 0 || table.size() - offset - 14 < filename_length) {
            error = "XDVDFS directory entry has an invalid filename length.";
            return false;
        }

        if (left_dwords != 0) {
            const uint32_t child = (uint32_t)left_dwords * 4u;
            if (!WalkEntry(table, child, out, depth, visited)) {
                return false;
            }
        }

        Entry entry;
        entry.name = safe_name(p + 14, filename_length);
        entry.attributes = attributes;
        entry.size = file_size;
        entry.start_sector = start_sector;
        entry.disc_offset = base + (uint64_t)start_sector * kSectorSize;

        if ((attributes & kDirectoryAttribute) != 0) {
            ++disc.directory_count;
            if (file_size != 0 && !ReadDirectory(start_sector, file_size,
                                                 entry.children, depth + 1)) {
                return false;
            }
        } else {
            ++disc.file_count;
        }

        out.push_back(std::move(entry));

        if (right_dwords != 0) {
            const uint32_t child = (uint32_t)right_dwords * 4u;
            if (!WalkEntry(table, child, out, depth, visited)) {
                return false;
            }
        }
        return true;
    }
};

static bool probe_volume_descriptor(const Reader &reader, uint64_t image_size,
                                    uint64_t base, uint32_t &root_sector,
                                    uint32_t &root_size)
{
    std::array<uint8_t, kSectorSize> vd{};
    const uint64_t offset = base + kVolumeDescriptorOffset;
    if (!range_inside(offset, vd.size(), image_size) ||
        !reader(offset, vd.data(), vd.size())) {
        return false;
    }
    if (std::memcmp(vd.data(), kMagic, kMagicLength) != 0 ||
        std::memcmp(vd.data() + 0x7EC, kMagic, kMagicLength) != 0) {
        return false;
    }

    root_sector = read_le32(vd.data() + 0x14);
    root_size = read_le32(vd.data() + 0x18);
    if (root_sector == 0 || root_size == 0 || root_size > kMaxDirectoryTableSize) {
        return false;
    }

    const uint64_t root_offset = base + (uint64_t)root_sector * kSectorSize;
    return range_inside(root_offset, root_size, image_size);
}

} // namespace

bool Parse(const Reader &reader, uint64_t image_size, Disc &disc,
           std::string &error)
{
    disc = {};
    error.clear();
    if (!reader || image_size < kVolumeDescriptorOffset + kSectorSize) {
        error = "No readable Xbox DVD media is mounted.";
        return false;
    }

    uint64_t base = 0;
    uint32_t root_sector = 0;
    uint32_t root_size = 0;
    bool found = false;
    for (uint64_t candidate : kFilesystemBases) {
        if (probe_volume_descriptor(reader, image_size, candidate,
                                    root_sector, root_size)) {
            base = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        error = "Mounted media does not contain a supported XDVDFS volume descriptor.";
        return false;
    }

    disc.filesystem_base = base;
    disc.root_sector = root_sector;
    disc.root_size = root_size;

    Parser parser{reader, image_size, base, disc, error};
    if (!parser.ReadDirectory(root_sector, root_size, disc.root_entries, 0)) {
        disc = {};
        return false;
    }

    disc.valid = true;
    return true;
}

const Entry *FindRootFile(const Disc &disc, const char *name)
{
    for (const Entry &entry : disc.root_entries) {
        if (!entry.IsDirectory() && ascii_equal_case_insensitive(entry.name, name)) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace XemuXdvdfs
