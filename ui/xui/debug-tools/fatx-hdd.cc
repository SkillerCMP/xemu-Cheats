//
// xemu read-only FATX HDD snapshot parser
//
// The on-disk structure and retail partition layout follow the GPLv2 libfatx
// implementation by Matt Borgerson (mborgerson/fatx). This xemu parser is
// intentionally read-only and consumes the active QEMU BlockBackend through a
// callback instead of opening the host image file.
//
// Copyright (C) 2026 xemu contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#include "fatx-hdd.hh"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <unordered_set>
#include <utility>

namespace XemuFatxHdd {
namespace {

constexpr uint32_t kFatxSignature = 0x58544146u; // "FATX" on little endian disk
constexpr uint64_t kSuperblockSize = 4096;
constexpr uint64_t kFatOffset = 4096;
constexpr uint32_t kSectorSize = 512;
constexpr uint32_t kMaxFilename = 42;
constexpr uint8_t kDeleted = 0xE5;
constexpr uint8_t kEnd1 = 0xFF;
constexpr uint8_t kEnd2 = 0x00;
constexpr uint8_t kDirectoryAttribute = 0x10;
constexpr size_t kRawDirectoryEntrySize = 64;
constexpr size_t kMaxEntries = 200000;
constexpr unsigned kMaxDepth = 64;
constexpr size_t kMaxDirectoryClusters = 65536;

struct PartitionLayout {
    char letter;
    const char *label;
    uint64_t offset;
    uint64_t size;
};

// Standard Original Xbox retail layout. F is detected separately from the
// remaining bytes of larger homebrew/LBA48 images.
constexpr PartitionLayout kRetailPartitions[] = {
    {'C', "System", 0x8CA80000ull, 0x01F400000ull},
    {'E', "Data",   0xABE80000ull, 0x1312D6000ull},
    {'X', "Cache",  0x00080000ull, 0x02EE00000ull},
    {'Y', "Cache",  0x2EE80000ull, 0x02EE00000ull},
    {'Z', "Cache",  0x5DC80000ull, 0x02EE00000ull},
};
constexpr uint64_t kExtendedFOffset = 0x1DD156000ull;

static uint16_t ReadLe16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t ReadLe32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool RangeInside(uint64_t offset, uint64_t size, uint64_t limit)
{
    return offset <= limit && size <= limit - offset;
}

static uint64_t RoundUp4096(uint64_t value)
{
    return (value + 4095ull) & ~4095ull;
}

static bool ValidSectorsPerCluster(uint32_t value)
{
    return value != 0 && value <= 1024 && (value & (value - 1)) == 0;
}

static std::string SafeFilename(const uint8_t *data, size_t length)
{
    std::string out;
    out.reserve(length);
    char escaped[5];
    for (size_t i = 0; i < length; ++i) {
        const uint8_t c = data[i];
        if (c >= 0x20 && c <= 0x7E) {
            out.push_back((char)c);
        } else {
            std::snprintf(escaped, sizeof(escaped), "\\x%02X", c);
            out += escaped;
        }
    }
    return out;
}

class Parser {
public:
    Parser(ReadCallback read, void *opaque, uint64_t image_size)
        : m_read(read), m_opaque(opaque), m_image_size(image_size)
    {
    }

    bool ParsePartition(Partition &part)
    {
        part.entries.clear();
        part.available = false;

        if (!m_read || part.size < kSuperblockSize ||
            !RangeInside(part.offset, part.size, m_image_size)) {
            part.status = "Partition is outside the mounted HDD image.";
            return false;
        }

        std::array<uint8_t, kSuperblockSize> superblock{};
        if (!Read(part.offset, superblock.data(), superblock.size())) {
            part.status = "Unable to read FATX superblock.";
            return false;
        }
        if (ReadLe32(superblock.data()) != kFatxSignature) {
            part.status = "No FATX signature.";
            return false;
        }

        part.volume_id = ReadLe32(superblock.data() + 4);
        part.sectors_per_cluster = ReadLe32(superblock.data() + 8);
        part.root_cluster = ReadLe32(superblock.data() + 12);
        if (!ValidSectorsPerCluster(part.sectors_per_cluster)) {
            part.status = "Invalid FATX sectors-per-cluster value.";
            return false;
        }

        const uint64_t bytes_per_cluster64 =
            (uint64_t)part.sectors_per_cluster * kSectorSize;
        if (bytes_per_cluster64 > std::numeric_limits<uint32_t>::max()) {
            part.status = "FATX cluster size is too large.";
            return false;
        }
        part.bytes_per_cluster = (uint32_t)bytes_per_cluster64;

        uint64_t fat_entries = part.size / bytes_per_cluster64 + 1;
        part.fat_bits = fat_entries < 0xFFF0ull ? 16 : 32;
        uint64_t fat_size = RoundUp4096(fat_entries * (part.fat_bits / 8));
        if (fat_size + kFatOffset >= part.size) {
            part.status = "FATX allocation table exceeds partition size.";
            return false;
        }

        m_partition = &part;
        m_fat_offset = part.offset + kFatOffset;
        m_fat_size = fat_size;
        m_cluster_offset = m_fat_offset + fat_size;
        m_num_clusters =
            (part.size - fat_size - kFatOffset) / bytes_per_cluster64 + 1;
        m_total_entries = 0;
        m_active_directories.clear();

        if (!ValidCluster(part.root_cluster)) {
            part.status = "FATX root cluster is outside the partition.";
            return false;
        }

        std::string error;
        if (!ReadDirectory(part.root_cluster, 0, part.entries, error)) {
            part.status = error.empty() ? "Unable to read FATX root directory."
                                        : error;
            return false;
        }

        part.available = true;
        char text[128];
        std::snprintf(text, sizeof(text),
                      "FAT%u, %u KiB clusters, %zu entries",
                      part.fat_bits, part.bytes_per_cluster / 1024,
                      CountEntries(part.entries));
        part.status = text;
        return true;
    }

private:
    bool Read(uint64_t offset, void *buffer, size_t size) const
    {
        return RangeInside(offset, size, m_image_size) &&
               m_read(m_opaque, offset, buffer, size);
    }

    bool ValidCluster(uint32_t cluster) const
    {
        return cluster >= 1 && (uint64_t)cluster < m_num_clusters + 1;
    }

    bool ClusterOffset(uint32_t cluster, uint64_t &offset) const
    {
        if (!ValidCluster(cluster)) {
            return false;
        }
        const uint64_t rel = (uint64_t)(cluster - 1) *
                             m_partition->bytes_per_cluster;
        if (m_cluster_offset < m_partition->offset ||
            rel > std::numeric_limits<uint64_t>::max() - m_cluster_offset) {
            return false;
        }
        offset = m_cluster_offset + rel;
        return RangeInside(offset, m_partition->bytes_per_cluster,
                           m_partition->offset + m_partition->size);
    }

    enum class NextClusterResult { Next, End, Error };

    NextClusterResult NextCluster(uint32_t cluster, uint32_t &next) const
    {
        if (!ValidCluster(cluster)) {
            return NextClusterResult::Error;
        }
        const uint32_t entry_bytes = m_partition->fat_bits / 8;
        const uint64_t offset = m_fat_offset + (uint64_t)cluster * entry_bytes;
        if (!RangeInside(offset, entry_bytes, m_fat_offset + m_fat_size)) {
            return NextClusterResult::Error;
        }

        uint8_t raw[4] = {};
        if (!Read(offset, raw, entry_bytes)) {
            return NextClusterResult::Error;
        }
        uint32_t value = entry_bytes == 2 ? ReadLe16(raw) : ReadLe32(raw);
        const uint32_t reserved = entry_bytes == 2 ? 0xFFF0u : 0xFFFFFFF0u;
        const uint32_t end = entry_bytes == 2 ? 0xFFFFu : 0xFFFFFFFFu;
        if (value == end) {
            return NextClusterResult::End;
        }
        if (value == 0 || value >= reserved || !ValidCluster(value)) {
            return NextClusterResult::Error;
        }
        next = value;
        return NextClusterResult::Next;
    }

    bool ReadDirectory(uint32_t first_cluster, unsigned depth,
                       std::vector<Entry> &entries, std::string &error)
    {
        if (depth > kMaxDepth) {
            error = "FATX directory nesting exceeds safety limit.";
            return false;
        }
        if (!ValidCluster(first_cluster)) {
            error = "FATX directory points to an invalid cluster.";
            return false;
        }
        if (!m_active_directories.insert(first_cluster).second) {
            error = "FATX directory cycle detected.";
            return false;
        }

        struct ActiveGuard {
            std::unordered_set<uint32_t> &set;
            uint32_t cluster;
            ~ActiveGuard() { set.erase(cluster); }
        } guard{m_active_directories, first_cluster};

        std::unordered_set<uint32_t> chain_seen;
        uint32_t cluster = first_cluster;
        std::vector<uint8_t> buffer(m_partition->bytes_per_cluster);

        for (;;) {
            if (!chain_seen.insert(cluster).second) {
                error = "FATX directory cluster-chain cycle detected.";
                return false;
            }
            if (chain_seen.size() > kMaxDirectoryClusters) {
                error = "FATX directory cluster chain exceeds safety limit.";
                return false;
            }
            uint64_t cluster_offset = 0;
            if (!ClusterOffset(cluster, cluster_offset) ||
                !Read(cluster_offset, buffer.data(), buffer.size())) {
                error = "Unable to read FATX directory cluster.";
                return false;
            }

            const size_t count = buffer.size() / kRawDirectoryEntrySize;
            for (size_t i = 0; i < count; ++i) {
                const uint8_t *raw = buffer.data() + i * kRawDirectoryEntrySize;
                const uint8_t filename_len = raw[0];
                if (filename_len == kEnd1 || filename_len == kEnd2) {
                    return true;
                }
                if (filename_len == kDeleted) {
                    continue;
                }
                if (filename_len > kMaxFilename) {
                    error = "Invalid FATX directory filename length.";
                    return false;
                }
                if (++m_total_entries > kMaxEntries) {
                    error = "FATX directory entry count exceeds safety limit.";
                    return false;
                }

                Entry entry;
                entry.attributes = raw[1];
                entry.name = SafeFilename(raw + 2, filename_len);
                entry.first_cluster = ReadLe32(raw + 44);
                entry.file_size = ReadLe32(raw + 48);
                entry.modified_time = ReadLe16(raw + 52);
                entry.modified_date = ReadLe16(raw + 54);
                entry.directory = (entry.attributes & kDirectoryAttribute) != 0;

                if (entry.directory) {
                    if (!ReadDirectory(entry.first_cluster, depth + 1,
                                       entry.children, error)) {
                        return false;
                    }
                }
                entries.emplace_back(std::move(entry));
            }

            uint32_t next = 0;
            const NextClusterResult result = NextCluster(cluster, next);
            if (result == NextClusterResult::Next) {
                cluster = next;
                continue;
            }
            if (result == NextClusterResult::End) {
                // A well-formed FATX directory normally has an explicit 0x00
                // or 0xFF entry before chain end. Accept chain end read-only;
                // the snapshot already contains every complete entry.
                return true;
            }
            error = "Invalid FATX directory cluster chain.";
            return false;
        }
    }

    static size_t CountEntries(const std::vector<Entry> &entries)
    {
        size_t count = entries.size();
        for (const Entry &entry : entries) {
            count += CountEntries(entry.children);
        }
        return count;
    }

    ReadCallback m_read = nullptr;
    void *m_opaque = nullptr;
    uint64_t m_image_size = 0;
    Partition *m_partition = nullptr;
    uint64_t m_fat_offset = 0;
    uint64_t m_fat_size = 0;
    uint64_t m_cluster_offset = 0;
    uint64_t m_num_clusters = 0;
    size_t m_total_entries = 0;
    std::unordered_set<uint32_t> m_active_directories;
};

} // namespace

bool BuildSnapshot(ReadCallback read, void *opaque, uint64_t image_size,
                   Snapshot &snapshot)
{
    snapshot = {};
    snapshot.image_size = image_size;
    if (!read || image_size == 0) {
        snapshot.status = "Xbox HDD block device is not available.";
        return false;
    }

    snapshot.hdd_available = true;
    Parser parser(read, opaque, image_size);
    for (const PartitionLayout &layout : kRetailPartitions) {
        Partition part;
        part.letter = layout.letter;
        part.label = layout.label;
        part.offset = layout.offset;
        part.size = layout.size;
        parser.ParsePartition(part);
        snapshot.partitions.emplace_back(std::move(part));
    }

    if (image_size > kExtendedFOffset + kSuperblockSize) {
        Partition part;
        part.letter = 'F';
        part.label = "Extended";
        part.offset = kExtendedFOffset;
        part.size = image_size - kExtendedFOffset;
        parser.ParsePartition(part);
        if (part.available) {
            snapshot.partitions.emplace_back(std::move(part));
        }
    }

    size_t available = 0;
    for (const Partition &part : snapshot.partitions) {
        if (part.available) {
            ++available;
        }
    }
    snapshot.status = available != 0
        ? "Read-only FATX snapshot captured from ide0-hd0."
        : "HDD is present, but no readable FATX partitions were found.";
    return available != 0;
}


namespace {

static bool EqualsAsciiNoCase(const std::string &a, const char *b)
{
    size_t n = std::char_traits<char>::length(b);
    if (a.size() != n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb + ('a' - 'A'));
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

static Entry *FindChildNoCase(std::vector<Entry> &entries, const char *name)
{
    for (Entry &entry : entries) {
        if (EqualsAsciiNoCase(entry.name, name)) {
            return &entry;
        }
    }
    return nullptr;
}

static void AppendUtf8(std::string &out, uint32_t cp)
{
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

static std::string DecodeMetadataText(const std::vector<uint8_t> &bytes)
{
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        std::string out;
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
            uint32_t cp = static_cast<uint32_t>(bytes[i]) |
                          (static_cast<uint32_t>(bytes[i + 1]) << 8);
            if (cp == 0) {
                break;
            }
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < bytes.size()) {
                uint32_t low = static_cast<uint32_t>(bytes[i + 2]) |
                               (static_cast<uint32_t>(bytes[i + 3]) << 8);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                }
            }
            if (cp == '\r') {
                continue;
            }
            AppendUtf8(out, cp);
        }
        return out;
    }

    std::string out;
    out.reserve(bytes.size());
    for (uint8_t c : bytes) {
        if (c == 0) {
            break;
        }
        if (c != '\r') {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

static std::string MetadataValue(const std::vector<uint8_t> &bytes,
                                 const char *key)
{
    const std::string text = DecodeMetadataText(bytes);
    const std::string prefix = std::string(key) + "=";
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(pos, end - pos);
        size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos) {
            line.erase(0, first);
        }
        if (line.size() >= prefix.size()) {
            bool match = true;
            for (size_t i = 0; i < prefix.size(); ++i) {
                unsigned char a = static_cast<unsigned char>(line[i]);
                unsigned char b = static_cast<unsigned char>(prefix[i]);
                if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a + ('a' - 'A'));
                if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b + ('a' - 'A'));
                if (a != b) {
                    match = false;
                    break;
                }
            }
            if (match) {
                std::string value = line.substr(prefix.size());
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                    value.pop_back();
                }
                return value;
            }
        }
        if (end == text.size()) {
            break;
        }
        pos = end + 1;
    }
    return {};
}

struct VectorWriter {
    std::vector<uint8_t> *bytes = nullptr;
    size_t limit = 0;
};

static bool WriteVector(void *opaque, const void *buffer, size_t size)
{
    VectorWriter *writer = static_cast<VectorWriter *>(opaque);
    if (!writer || !writer->bytes || size > writer->limit - writer->bytes->size()) {
        return false;
    }
    const uint8_t *src = static_cast<const uint8_t *>(buffer);
    writer->bytes->insert(writer->bytes->end(), src, src + size);
    return true;
}

static bool ReadSmallFile(ReadCallback read, void *opaque, uint64_t image_size,
                          const Partition &partition, const Entry &entry,
                          std::vector<uint8_t> &bytes)
{
    constexpr size_t kMaxMetadataBytes = 64 * 1024;
    bytes.clear();
    if (entry.directory || entry.file_size > kMaxMetadataBytes) {
        return false;
    }
    bytes.reserve(entry.file_size);
    VectorWriter writer{&bytes, kMaxMetadataBytes};
    std::string error;
    return StreamFile(read, opaque, image_size, partition, entry,
                      WriteVector, &writer, error);
}

static void PopulateTitleAreaMetadata(ReadCallback read, void *opaque,
                                      uint64_t image_size,
                                      Partition &partition, Entry &area,
                                      bool save_names)
{
    for (Entry &title : area.children) {
        if (!title.directory) {
            continue;
        }
        if (const Entry *meta = FindChildNoCase(title.children, "TitleMeta.xbx")) {
            std::vector<uint8_t> bytes;
            if (ReadSmallFile(read, opaque, image_size, partition, *meta, bytes)) {
                title.friendly_name = MetadataValue(bytes, "TitleName");
            }
        }

        if (!save_names) {
            continue;
        }
        for (Entry &save : title.children) {
            if (!save.directory) {
                continue;
            }
            if (const Entry *meta = FindChildNoCase(save.children, "SaveMeta.xbx")) {
                std::vector<uint8_t> bytes;
                if (ReadSmallFile(read, opaque, image_size, partition, *meta, bytes)) {
                    save.friendly_name = MetadataValue(bytes, "Name");
                }
            }
        }
    }
}

} // namespace

const Partition *FindPartition(const Snapshot &snapshot, char letter)
{
    for (const Partition &partition : snapshot.partitions) {
        if (partition.letter == letter) {
            return &partition;
        }
    }
    return nullptr;
}

Partition *FindPartition(Snapshot &snapshot, char letter)
{
    for (Partition &partition : snapshot.partitions) {
        if (partition.letter == letter) {
            return &partition;
        }
    }
    return nullptr;
}

const Entry *FindEntry(const Partition &partition,
                       const std::vector<std::string> &path)
{
    const std::vector<Entry> *entries = &partition.entries;
    const Entry *current = nullptr;
    for (const std::string &part : path) {
        current = nullptr;
        for (const Entry &entry : *entries) {
            if (entry.name == part) {
                current = &entry;
                break;
            }
        }
        if (!current) {
            return nullptr;
        }
        entries = &current->children;
    }
    return current;
}

Entry *FindEntry(Partition &partition, const std::vector<std::string> &path)
{
    std::vector<Entry> *entries = &partition.entries;
    Entry *current = nullptr;
    for (const std::string &part : path) {
        current = nullptr;
        for (Entry &entry : *entries) {
            if (entry.name == part) {
                current = &entry;
                break;
            }
        }
        if (!current) {
            return nullptr;
        }
        entries = &current->children;
    }
    return current;
}

std::string DisplayName(const Entry &entry)
{
    if (entry.friendly_name.empty()) {
        return entry.name;
    }
    return entry.name + " - " + entry.friendly_name;
}

bool StreamFile(ReadCallback read, void *read_opaque, uint64_t image_size,
                const Partition &partition, const Entry &entry,
                WriteCallback write, void *write_opaque, std::string &error)
{
    error.clear();
    if (!read || !write || entry.directory || !partition.available ||
        partition.bytes_per_cluster == 0 ||
        (partition.fat_bits != 16 && partition.fat_bits != 32)) {
        error = "Invalid FATX file export parameters.";
        return false;
    }
    if (entry.file_size == 0) {
        return true;
    }

    const uint64_t cluster_size = partition.bytes_per_cluster;
    const uint64_t fat_entries = partition.size / cluster_size + 1;
    const uint64_t fat_size = RoundUp4096(fat_entries * (partition.fat_bits / 8));
    if (fat_size + kFatOffset >= partition.size) {
        error = "Invalid FATX allocation table.";
        return false;
    }
    const uint64_t fat_offset = partition.offset + kFatOffset;
    const uint64_t cluster_offset = fat_offset + fat_size;
    const uint64_t num_clusters =
        (partition.size - fat_size - kFatOffset) / cluster_size + 1;

    auto valid_cluster = [num_clusters](uint32_t cluster) {
        return cluster >= 1 && static_cast<uint64_t>(cluster) < num_clusters + 1;
    };
    auto cluster_disk_offset = [&](uint32_t cluster, uint64_t &offset) {
        if (!valid_cluster(cluster)) {
            return false;
        }
        const uint64_t rel = static_cast<uint64_t>(cluster - 1) * cluster_size;
        if (rel > std::numeric_limits<uint64_t>::max() - cluster_offset) {
            return false;
        }
        offset = cluster_offset + rel;
        return RangeInside(offset, cluster_size, partition.offset + partition.size) &&
               RangeInside(offset, cluster_size, image_size);
    };
    auto next_cluster = [&](uint32_t cluster, uint32_t &next) {
        const uint32_t entry_bytes = partition.fat_bits / 8;
        const uint64_t offset = fat_offset + static_cast<uint64_t>(cluster) * entry_bytes;
        if (!valid_cluster(cluster) ||
            !RangeInside(offset, entry_bytes, fat_offset + fat_size) ||
            !RangeInside(offset, entry_bytes, image_size)) {
            return false;
        }
        uint8_t raw[4] = {};
        if (!read(read_opaque, offset, raw, entry_bytes)) {
            return false;
        }
        const uint32_t value = entry_bytes == 2 ? ReadLe16(raw) : ReadLe32(raw);
        const uint32_t reserved = entry_bytes == 2 ? 0xFFF0u : 0xFFFFFFF0u;
        const uint32_t end = entry_bytes == 2 ? 0xFFFFu : 0xFFFFFFFFu;
        if (value == end || value == 0 || value >= reserved || !valid_cluster(value)) {
            return false;
        }
        next = value;
        return true;
    };

    if (!valid_cluster(entry.first_cluster)) {
        error = "FATX file points to an invalid first cluster.";
        return false;
    }

    uint64_t remaining = entry.file_size;
    uint32_t cluster = entry.first_cluster;
    std::unordered_set<uint32_t> seen;
    std::vector<uint8_t> buffer(static_cast<size_t>(cluster_size));
    const uint64_t needed_clusters = (remaining + cluster_size - 1) / cluster_size;

    while (remaining != 0) {
        if (!seen.insert(cluster).second || seen.size() > needed_clusters) {
            error = "FATX file cluster-chain cycle detected.";
            return false;
        }
        uint64_t offset = 0;
        if (!cluster_disk_offset(cluster, offset)) {
            error = "FATX file cluster is outside the partition.";
            return false;
        }
        const size_t amount = static_cast<size_t>(std::min<uint64_t>(remaining, cluster_size));
        if (!read(read_opaque, offset, buffer.data(), amount)) {
            error = "Unable to read FATX file data.";
            return false;
        }
        if (!write(write_opaque, buffer.data(), amount)) {
            error = "Unable to write exported host file.";
            return false;
        }
        remaining -= amount;
        if (remaining == 0) {
            break;
        }
        uint32_t next = 0;
        if (!next_cluster(cluster, next)) {
            error = "FATX file cluster chain ended before file size.";
            return false;
        }
        cluster = next;
    }
    return true;
}

bool PopulateXboxMetadata(ReadCallback read, void *opaque, uint64_t image_size,
                          Snapshot &snapshot, std::string &warning)
{
    warning.clear();
    Partition *data = FindPartition(snapshot, 'E');
    if (!data || !data->available) {
        return false;
    }

    if (Entry *udata = FindChildNoCase(data->entries, "UDATA")) {
        if (udata->directory) {
            PopulateTitleAreaMetadata(read, opaque, image_size, *data, *udata,
                                      true);
        }
    }
    if (Entry *tdata = FindChildNoCase(data->entries, "TDATA")) {
        if (tdata->directory) {
            PopulateTitleAreaMetadata(read, opaque, image_size, *data, *tdata,
                                      false);
        }
    }
    return true;
}

std::string FormatTimestamp(uint16_t date, uint16_t time)
{
    if (date == 0 && time == 0) {
        return {};
    }
    const unsigned year = ((date >> 9) & 0x7F) + 2000;
    const unsigned month = (date >> 5) & 0x0F;
    const unsigned day = date & 0x1F;
    const unsigned hour = (time >> 11) & 0x1F;
    const unsigned minute = (time >> 5) & 0x3F;
    const unsigned second = (time & 0x1F) * 2;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u %02u:%02u:%02u",
                  year, month, day, hour, minute, second);
    return buffer;
}

} // namespace XemuFatxHdd
