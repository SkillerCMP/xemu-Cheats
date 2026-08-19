#include "fatx-hdd.hh"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kCOffset = 0x8CA80000ull;
constexpr uint64_t kCSize = 0x01F400000ull;
constexpr uint64_t kEOffset = 0xABE80000ull;
constexpr uint64_t kESize = 0x1312D6000ull;
constexpr uint64_t kRetailDiskSize = 0x1DD156000ull;
constexpr uint32_t kClusterSize = 16 * 1024;
constexpr uint64_t kCFatOffset = kCOffset + 4096;
constexpr uint64_t kCFatSize = 0x10000;
constexpr uint64_t kCDataOffset = kCFatOffset + kCFatSize;

struct SparseImage {
    std::map<uint64_t, std::vector<uint8_t>> chunks;
};

void PutLe16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void PutLe32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint64_t RoundUp4096(uint64_t value)
{
    return (value + 4095ull) & ~4095ull;
}

uint64_t DataOffset(uint64_t partition_offset, uint64_t partition_size)
{
    const uint64_t fat_entries = partition_size / kClusterSize + 1;
    const uint64_t fat_bits = fat_entries < 0xFFF0ull ? 16 : 32;
    const uint64_t fat_size = RoundUp4096(fat_entries * (fat_bits / 8));
    return partition_offset + 4096 + fat_size;
}

bool SparseRead(void *opaque, uint64_t offset, void *buffer, size_t size)
{
    auto &image = *static_cast<SparseImage *>(opaque);
    std::memset(buffer, 0, size);
    uint8_t *out = static_cast<uint8_t *>(buffer);
    for (const auto &[base, data] : image.chunks) {
        const uint64_t end = base + data.size();
        const uint64_t request_end = offset + size;
        if (end <= offset || base >= request_end) {
            continue;
        }
        const uint64_t begin = base > offset ? base : offset;
        const uint64_t finish = end < request_end ? end : request_end;
        std::memcpy(out + (begin - offset), data.data() + (begin - base),
                    (size_t)(finish - begin));
    }
    return true;
}

bool VectorWrite(void *opaque, const void *buffer, size_t size)
{
    auto &out = *static_cast<std::vector<uint8_t> *>(opaque);
    const auto *bytes = static_cast<const uint8_t *>(buffer);
    out.insert(out.end(), bytes, bytes + size);
    return true;
}

void AddEntry(std::vector<uint8_t> &cluster, size_t index, const char *name,
              uint8_t attrs, uint32_t first_cluster, uint32_t size,
              uint16_t date, uint16_t time)
{
    uint8_t *raw = cluster.data() + index * 64;
    const size_t len = std::strlen(name);
    assert(len <= 42);
    raw[0] = (uint8_t)len;
    raw[1] = attrs;
    std::memcpy(raw + 2, name, len);
    PutLe32(raw + 44, first_cluster);
    PutLe32(raw + 48, size);
    PutLe16(raw + 52, time);
    PutLe16(raw + 54, date);
}

std::vector<uint8_t> Utf16Le(const char *text)
{
    std::vector<uint8_t> out{0xFF, 0xFE};
    while (*text) {
        out.push_back((uint8_t)*text++);
        out.push_back(0);
    }
    return out;
}

void AddPartitionSuperblock(SparseImage &image, uint64_t offset,
                            uint32_t volume_id, uint32_t root_cluster)
{
    std::vector<uint8_t> superblock(4096, 0xFF);
    PutLe32(superblock.data() + 0, 0x58544146u);
    PutLe32(superblock.data() + 4, volume_id);
    PutLe32(superblock.data() + 8, 32u);
    PutLe32(superblock.data() + 12, root_cluster);
    image.chunks[offset] = std::move(superblock);
}

} // namespace

int main()
{
    SparseImage image;

    AddPartitionSuperblock(image, kCOffset, 0x12345678u, 1u);

    std::vector<uint8_t> root(kClusterSize, 0xFF);
    const uint16_t date = (uint16_t)((26u << 9) | (8u << 5) | 18u); // 2026-08-18
    const uint16_t time = (uint16_t)((17u << 11) | (45u << 5) | 15u); // 17:45:30
    AddEntry(root, 0, "foo.txt", 0, 2, 123, date, time);
    AddEntry(root, 1, "folder", 0x10, 3, 0, date, time);
    root[2 * 64] = 0xFF;
    image.chunks[kCDataOffset] = root;

    std::vector<uint8_t> child(kClusterSize, 0xFF);
    AddEntry(child, 0, "bar.bin", 0x01, 4, 456, date, time);
    child[1 * 64] = 0x00;
    image.chunks[kCDataOffset + 2ull * kClusterSize] = child; // cluster 3

    std::vector<uint8_t> foo_data(123);
    for (size_t i = 0; i < foo_data.size(); ++i) {
        foo_data[i] = (uint8_t)(i ^ 0x5A);
    }
    image.chunks[kCDataOffset + kClusterSize] = foo_data; // cluster 2

    // Build a minimal E:\UDATA\54510109 tree with TitleMeta/SaveMeta so the
    // read-only friendly-name pass is exercised against real FATX metadata.
    AddPartitionSuperblock(image, kEOffset, 0xCAFEBABEu, 1u);
    const uint64_t e_data = DataOffset(kEOffset, kESize);

    std::vector<uint8_t> e_root(kClusterSize, 0xFF);
    AddEntry(e_root, 0, "UDATA", 0x10, 2, 0, date, time);
    e_root[64] = 0xFF;
    image.chunks[e_data] = e_root;

    std::vector<uint8_t> udata(kClusterSize, 0xFF);
    AddEntry(udata, 0, "54510109", 0x10, 3, 0, date, time);
    udata[64] = 0xFF;
    image.chunks[e_data + kClusterSize] = udata;

    const std::vector<uint8_t> title_meta = Utf16Le("TitleName=Ratatouille\r\n");
    std::vector<uint8_t> title(kClusterSize, 0xFF);
    AddEntry(title, 0, "TitleMeta.xbx", 0, 4, (uint32_t)title_meta.size(), date, time);
    AddEntry(title, 1, "565190C2C935", 0x10, 5, 0, date, time);
    title[2 * 64] = 0xFF;
    image.chunks[e_data + 2ull * kClusterSize] = title;
    image.chunks[e_data + 3ull * kClusterSize] = title_meta;

    const std::vector<uint8_t> save_meta = Utf16Le("Name=Save Game 1\r\n");
    std::vector<uint8_t> save(kClusterSize, 0xFF);
    AddEntry(save, 0, "SaveMeta.xbx", 0, 6, (uint32_t)save_meta.size(), date, time);
    save[64] = 0xFF;
    image.chunks[e_data + 4ull * kClusterSize] = save;
    image.chunks[e_data + 5ull * kClusterSize] = save_meta;

    XemuFatxHdd::Snapshot snapshot;
    const bool ok = XemuFatxHdd::BuildSnapshot(
        SparseRead, &image, kRetailDiskSize, snapshot);
    assert(ok);
    assert(snapshot.hdd_available);
    assert(snapshot.image_size == kRetailDiskSize);
    assert(snapshot.partitions.size() == 5);

    const XemuFatxHdd::Partition *c = XemuFatxHdd::FindPartition(snapshot, 'C');
    assert(c != nullptr);
    assert(c->offset == kCOffset);
    assert(c->size == kCSize);
    assert(c->available);
    assert(c->volume_id == 0x12345678u);
    assert(c->sectors_per_cluster == 32u);
    assert(c->bytes_per_cluster == kClusterSize);
    assert(c->fat_bits == 16u);
    assert(c->entries.size() == 2);
    assert(c->entries[0].name == "foo.txt");
    assert(!c->entries[0].directory);
    assert(c->entries[0].file_size == 123u);
    assert(c->entries[1].name == "folder");
    assert(c->entries[1].directory);
    assert(c->entries[1].children.size() == 1);
    assert(c->entries[1].children[0].name == "bar.bin");
    assert(c->entries[1].children[0].file_size == 456u);
    assert(c->entries[1].children[0].attributes == 0x01u);
    assert(XemuFatxHdd::FormatTimestamp(date, time) == "2026-08-18 17:45:30");

    std::vector<uint8_t> exported;
    std::string stream_error;
    assert(XemuFatxHdd::StreamFile(SparseRead, &image, kRetailDiskSize,
                                   *c, c->entries[0], VectorWrite, &exported,
                                   stream_error));
    assert(stream_error.empty());
    assert(exported == foo_data);

    std::string metadata_warning;
    assert(XemuFatxHdd::PopulateXboxMetadata(
        SparseRead, &image, kRetailDiskSize, snapshot, metadata_warning));
    assert(metadata_warning.empty());
    const XemuFatxHdd::Partition *e = XemuFatxHdd::FindPartition(snapshot, 'E');
    assert(e && e->available);
    const XemuFatxHdd::Entry *title_id =
        XemuFatxHdd::FindEntry(*e, {"UDATA", "54510109"});
    assert(title_id && title_id->friendly_name == "Ratatouille");
    assert(XemuFatxHdd::DisplayName(*title_id) == "54510109 - Ratatouille");
    const XemuFatxHdd::Entry *save_dir =
        XemuFatxHdd::FindEntry(*e, {"UDATA", "54510109", "565190C2C935"});
    assert(save_dir && save_dir->friendly_name == "Save Game 1");
    assert(XemuFatxHdd::DisplayName(*save_dir) == "565190C2C935 - Save Game 1");

    for (const auto &part : snapshot.partitions) {
        if (part.letter != 'C' && part.letter != 'E') {
            assert(!part.available);
        }
    }

    std::cout << "PASS: FATX HDD parser metadata + stream export\n";
    return 0;
}
