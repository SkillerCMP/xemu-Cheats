#include "xdvdfs-disc.hh"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr uint64_t kSector = 0x800;

void put16(std::vector<uint8_t> &v, size_t off, uint16_t n)
{
    v[off + 0] = (uint8_t)n;
    v[off + 1] = (uint8_t)(n >> 8);
}

void put32(std::vector<uint8_t> &v, size_t off, uint32_t n)
{
    v[off + 0] = (uint8_t)n;
    v[off + 1] = (uint8_t)(n >> 8);
    v[off + 2] = (uint8_t)(n >> 16);
    v[off + 3] = (uint8_t)(n >> 24);
}

size_t write_entry(std::vector<uint8_t> &image, uint64_t table_base,
                   size_t entry_offset, uint16_t left_dwords,
                   uint16_t right_dwords, uint32_t sector, uint32_t size,
                   uint8_t attributes, const char *name)
{
    const size_t name_len = std::strlen(name);
    const uint64_t off = table_base + entry_offset;
    put16(image, off + 0, left_dwords);
    put16(image, off + 2, right_dwords);
    put32(image, off + 4, sector);
    put32(image, off + 8, size);
    image[off + 12] = attributes;
    image[off + 13] = (uint8_t)name_len;
    std::memcpy(image.data() + off + 14, name, name_len);
    return (14 + name_len + 3) & ~size_t(3);
}

bool require(bool value, const char *message)
{
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        return false;
    }
    return true;
}
} // namespace

int main()
{
    std::vector<uint8_t> image(0x30000, 0xFF);
    const char magic[] = "MICROSOFT*XBOX*MEDIA";
    std::memcpy(image.data() + 0x10000, magic, 20);
    std::memcpy(image.data() + 0x10000 + 0x7EC, magic, 20);

    const uint32_t root_sector = 0x30;
    const uint32_t root_size = 0x800;
    put32(image, 0x10000 + 0x14, root_sector);
    put32(image, 0x10000 + 0x18, root_size);

    const uint64_t root = (uint64_t)root_sector * kSector;
    const size_t default_size = (14 + std::strlen("default.xbe") + 3) & ~size_t(3);
    const size_t folder_offset = default_size;
    const uint16_t folder_dwords = (uint16_t)(folder_offset / 4);
    write_entry(image, root, 0, 0, folder_dwords, 0x50, 0x1234, 0x20,
                "default.xbe");
    write_entry(image, root, folder_offset, 0, 0, 0x40, 0x800, 0x10,
                "media");

    const uint64_t media = 0x40ull * kSector;
    write_entry(image, media, 0, 0, 0, 0x52, 0x88, 0x20, "intro.wmv");

    auto reader = [&image](uint64_t off, void *buffer, size_t size) {
        if (off > image.size() || size > image.size() - off) {
            return false;
        }
        std::memcpy(buffer, image.data() + off, size);
        return true;
    };

    XemuXdvdfs::Disc disc;
    std::string error;
    if (!require(XemuXdvdfs::Parse(reader, image.size(), disc, error),
                 error.c_str())) return 1;
    if (!require(disc.valid, "disc should be valid")) return 1;
    if (!require(disc.root_sector == root_sector, "root sector")) return 1;
    if (!require(disc.file_count == 2, "file count")) return 1;
    if (!require(disc.directory_count == 1, "directory count")) return 1;
    if (!require(disc.root_entries.size() == 2, "root entry count")) return 1;

    const XemuXdvdfs::Entry *xbe = XemuXdvdfs::FindRootFile(disc, "DEFAULT.XBE");
    if (!require(xbe != nullptr, "case-insensitive default.xbe lookup")) return 1;
    if (!require(xbe->size == 0x1234, "default.xbe size")) return 1;
    if (!require(xbe->disc_offset == 0x50ull * kSector,
                 "default.xbe disc offset")) return 1;

    const XemuXdvdfs::Entry &folder = disc.root_entries[1];
    if (!require(folder.IsDirectory(), "media is directory")) return 1;
    if (!require(folder.children.size() == 1, "media child count")) return 1;
    if (!require(folder.children[0].name == "intro.wmv", "media child name")) return 1;

    std::vector<uint8_t> invalid = image;
    invalid[0x10000] = 'X';
    auto invalid_reader = [&invalid](uint64_t off, void *buffer, size_t size) {
        if (off > invalid.size() || size > invalid.size() - off) return false;
        std::memcpy(buffer, invalid.data() + off, size);
        return true;
    };
    XemuXdvdfs::Disc bad;
    std::string bad_error;
    if (!require(!XemuXdvdfs::Parse(invalid_reader, invalid.size(), bad,
                                    bad_error),
                 "bad volume magic must fail")) return 1;

    std::puts("PASS: XDVDFS parser golden");
    return 0;
}
