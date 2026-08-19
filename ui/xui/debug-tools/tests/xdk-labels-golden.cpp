// Local XDK importer golden tests using a fully synthetic COFF library.
#include "xbe-labels.hh"
#include "xdk-labels.hh"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

static void put16(std::vector<uint8_t> &b, size_t off, uint16_t v)
{
    b[off] = (uint8_t)v;
    b[off + 1] = (uint8_t)(v >> 8);
}

static void put32(std::vector<uint8_t> &b, size_t off, uint32_t v)
{
    b[off] = (uint8_t)v;
    b[off + 1] = (uint8_t)(v >> 8);
    b[off + 2] = (uint8_t)(v >> 16);
    b[off + 3] = (uint8_t)(v >> 24);
}

static void section(std::vector<uint8_t> &b, size_t off, uint32_t flags,
                    uint32_t va, uint32_t vsize, uint32_t raw,
                    uint32_t raw_size, uint32_t name_va)
{
    put32(b, off + 0, flags);
    put32(b, off + 4, va);
    put32(b, off + 8, vsize);
    put32(b, off + 12, raw);
    put32(b, off + 16, raw_size);
    put32(b, off + 20, name_va);
}

static std::vector<uint8_t> make_object()
{
    // One 20-byte .text function with a DIR32 relocation at +4.
    std::vector<uint8_t> obj(20 + 40 + 20 + 10 + 36 + 4, 0);
    put16(obj, 0, 0x014c); // i386
    put16(obj, 2, 1);      // sections
    const uint32_t raw = 60;
    const uint32_t rel = raw + 20;
    const uint32_t sym = rel + 10;
    put32(obj, 8, sym);
    put32(obj, 12, 2);     // function symbol + aux

    std::memcpy(obj.data() + 20, ".text", 5);
    put32(obj, 20 + 16, 20);
    put32(obj, 20 + 20, raw);
    put32(obj, 20 + 24, rel);
    put16(obj, 20 + 32, 1);
    put32(obj, 20 + 36, 0x60000020u); // code + execute/read

    const uint8_t code[20] = {
        0x55,0x8B,0xEC,0xA1, 0x00,0x00,0x00,0x00,
        0x83,0xC0,0x17,0x35, 0x6A,0xA5,0xC3,0x90,
        0x66,0x90,0x90,0x90
    };
    std::memcpy(obj.data() + raw, code, sizeof(code));

    put32(obj, rel + 0, 4);    // relocation offset inside function
    put32(obj, rel + 4, 0);    // symbol index is irrelevant to importer
    put16(obj, rel + 8, 0x0006); // IMAGE_REL_I386_DIR32

    std::memcpy(obj.data() + sym, "_XdkTest", 8);
    put32(obj, sym + 8, 0);
    put16(obj, sym + 12, 1);
    put16(obj, sym + 14, 0x20);
    obj[sym + 16] = 2; // external
    obj[sym + 17] = 1; // aux
    put32(obj, sym + 18 + 4, 20); // function total size

    put32(obj, sym + 36, 4); // string-table size
    return obj;
}

static std::vector<uint8_t> make_archive(const std::vector<uint8_t> &obj)
{
    std::vector<uint8_t> ar;
    const char magic[] = "!<arch>\n";
    ar.insert(ar.end(), magic, magic + 8);
    char header[60];
    std::memset(header, ' ', sizeof(header));
    std::memcpy(header, "test.obj/", 9);
    std::snprintf(header + 48, 11, "%-10zu", obj.size());
    header[58] = '`';
    header[59] = '\n';
    ar.insert(ar.end(), header, header + 60);
    ar.insert(ar.end(), obj.begin(), obj.end());
    if (obj.size() & 1u) ar.push_back('\n');
    return ar;
}

static std::vector<uint8_t> make_xbe()
{
    constexpr uint32_t base = 0x10000;
    constexpr uint32_t retail_entry_key = 0xA8FC57ABu;
    std::vector<uint8_t> xbe(0x2000, 0);
    std::memcpy(xbe.data(), "XBEH", 4);
    put32(xbe, 0x104, base);
    put32(xbe, 0x10c, 0x3000);
    put32(xbe, 0x11c, 1);
    put32(xbe, 0x120, base + 0x200);
    put32(xbe, 0x128, 0x11000u ^ retail_entry_key);
    put32(xbe, 0x160, 1);
    put32(xbe, 0x164, base + 0x340);
    std::memcpy(xbe.data() + 0x300, ".text", 6);
    section(xbe, 0x200, 0x4, 0x11000, 0x100, 0x1000, 0x100,
            base + 0x300);

    std::memcpy(xbe.data() + 0x340, "XAPILIB", 7);
    put16(xbe, 0x348, 1);
    put16(xbe, 0x34a, 0);
    put16(xbe, 0x34c, 5849);
    put16(xbe, 0x34e, 0x4001);

    const uint8_t linked[20] = {
        0x55,0x8B,0xEC,0xA1, 0x78,0x56,0x34,0x12,
        0x83,0xC0,0x17,0x35, 0x6A,0xA5,0xC3,0x90,
        0x66,0x90,0x90,0x90
    };
    std::memcpy(xbe.data() + 0x1000, linked, sizeof(linked));
    return xbe;
}

static bool write_file(const fs::path &path, const std::vector<uint8_t> &bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    return (bool)out;
}

static int fail(const char *message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    const fs::path root = fs::temp_directory_path() / "xemu-xdk-label-golden";
    const fs::path xdk = root / "XDK" / "5849" / "nested" / "xbox" / "lib";
    const fs::path cache = root / "Cache";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(xdk, ec);
    fs::create_directories(cache, ec);
    if (ec) return fail("temporary folder creation");

    const auto object = make_object();
    const auto archive = make_archive(object);
    if (!write_file(xdk / "xapilib.lib", archive)) {
        return fail("synthetic XDK library write");
    }

    const auto xbe = make_xbe();
    XemuXbeLabels::Database db;
    std::string error;
    if (!XemuXbeLabels::Build(xbe, db, error)) {
        std::fprintf(stderr, "FAIL: XBE parse: %s\n", error.c_str());
        return 1;
    }

    XemuXdkLabels::Status status;
    std::vector<XemuXbeLabels::Label> labels;
    if (!XemuXdkLabels::Process((root / "XDK").string(), cache.string(),
                                xbe, db, true, labels, status, error)) {
        std::fprintf(stderr, "FAIL: XDK import: %s\n", error.c_str());
        return 1;
    }
    if (status.build != 5849 || status.found_libraries != 1 ||
        status.required_libraries != 1 || status.signatures != 1 ||
        status.exact_matches != 1 || labels.size() != 1) {
        return fail("XDK import counts");
    }
    if (labels[0].virtual_address != 0x11000 ||
        labels[0].name != "XAPILIB::XdkTest" ||
        labels[0].type != XemuXbeLabels::Type::Function ||
        labels[0].source != XemuXbeLabels::Source::Xdk ||
        labels[0].confidence != XemuXbeLabels::Confidence::Exact) {
        return fail("exact XDK function label");
    }

    // The cache must be sufficient without the source .lib and must not retain
    // the original function-byte sequence.
    fs::remove(xdk / "xapilib.lib", ec);
    XemuXdkLabels::Status cached_status;
    std::vector<XemuXbeLabels::Label> cached_labels;
    if (!XemuXdkLabels::Process((root / "XDK").string(), cache.string(),
                                xbe, db, false, cached_labels,
                                cached_status, error) ||
        cached_labels.size() != 1 || !cached_status.cache_loaded) {
        return fail("cache-only reload");
    }

    std::ifstream cache_file(cached_status.cache_path, std::ios::binary);
    std::vector<uint8_t> cache_bytes((std::istreambuf_iterator<char>(cache_file)), {});
    const uint8_t code_prefix[12] = {
        0x55,0x8B,0xEC,0xA1,0x00,0x00,0x00,0x00,0x83,0xC0,0x17,0x35
    };
    if (std::search(cache_bytes.begin(), cache_bytes.end(),
                    std::begin(code_prefix), std::end(code_prefix)) !=
        cache_bytes.end()) {
        return fail("cache retained original executable bytes");
    }

    fs::remove_all(root, ec);
    std::puts("PASS: local XDK fingerprint/cache/match golden cases");
    return 0;
}
