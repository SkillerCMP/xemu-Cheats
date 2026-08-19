// XBE label parser golden tests.
#include "xbe-labels.hh"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

static void put32(std::vector<uint8_t> &b, size_t off, uint32_t v)
{
    b[off + 0] = (uint8_t)(v & 0xff);
    b[off + 1] = (uint8_t)((v >> 8) & 0xff);
    b[off + 2] = (uint8_t)((v >> 16) & 0xff);
    b[off + 3] = (uint8_t)((v >> 24) & 0xff);
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

static bool have(const XemuXbeLabels::Database &db, uint32_t address,
                 XemuXbeLabels::Type type, const char *name)
{
    for (const auto &label : db.labels) {
        if (label.virtual_address == address && label.type == type &&
            label.name == name) {
            return true;
        }
    }
    return false;
}

static int fail(const char *what)
{
    std::fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

} // namespace

int main()
{
    constexpr uint32_t base = 0x00010000u;
    constexpr uint32_t entry = 0x00011000u;
    constexpr uint32_t string_va = 0x00012000u;
    constexpr uint32_t thunk_va = 0x00012020u;
    constexpr uint32_t retail_entry_key = 0xA8FC57ABu;
    constexpr uint32_t retail_thunk_key = 0x5B6D40B6u;

    std::vector<uint8_t> file(0x3000, 0);
    std::memcpy(file.data(), "XBEH", 4);
    put32(file, 0x104, base);
    put32(file, 0x108, 0x1000);
    put32(file, 0x10c, 0x4000);
    put32(file, 0x11c, 2);
    put32(file, 0x120, base + 0x200);
    put32(file, 0x128, entry ^ retail_entry_key);
    put32(file, 0x158, thunk_va ^ retail_thunk_key);
    put32(file, 0x160, 1);
    put32(file, 0x164, base + 0x340);
    std::memcpy(file.data() + 0x340, "XAPILIB", 7);
    file[0x348] = 1; file[0x349] = 0;
    file[0x34a] = 0; file[0x34b] = 0;
    file[0x34c] = 0xD9; file[0x34d] = 0x16; // 5849
    file[0x34e] = 1; file[0x34f] = 0x40;

    std::memcpy(file.data() + 0x300, ".text", 6);
    std::memcpy(file.data() + 0x308, ".rdata", 7);
    section(file, 0x200, 0x4, 0x11000, 0x100, 0x1000, 0x100,
            base + 0x300);
    section(file, 0x200 + 56, 0x0, 0x12000, 0x100, 0x2000, 0x100,
            base + 0x308);

    // Standard frame prologue followed by push offset Foo::Bar().
    file[0x1000] = 0x55;
    file[0x1001] = 0x8b;
    file[0x1002] = 0xec;
    file[0x1003] = 0x68;
    put32(file, 0x1004, string_va);
    file[0x1008] = 0xc3;

    std::memcpy(file.data() + 0x2000, "Foo::Bar()", 10);
    file[0x200a] = 0;
    put32(file, 0x2020, 0x80000008u); // DbgPrint
    put32(file, 0x2024, 0);

    XemuXbeLabels::Database db;
    std::string error;
    if (!XemuXbeLabels::Build(file, db, error)) {
        std::fprintf(stderr, "FAIL: parser rejected synthetic XBE: %s\n",
                     error.c_str());
        return 1;
    }

    if (!have(db, entry, XemuXbeLabels::Type::Entry, "XBE_EntryPoint")) {
        return fail("entry label");
    }
    if (!have(db, 0x11000, XemuXbeLabels::Type::Section, "section_text")) {
        return fail("text section label");
    }
    if (!have(db, string_va, XemuXbeLabels::Type::String, "str_Foo_Bar")) {
        return fail("string label");
    }
    if (!have(db, 0x11003, XemuXbeLabels::Type::Xref, "xref_Foo_Bar")) {
        return fail("string xref label");
    }
    if (!have(db, entry, XemuXbeLabels::Type::Inferred, "~Foo_Bar")) {
        return fail("inferred function label");
    }
    if (!have(db, thunk_va, XemuXbeLabels::Type::Kernel,
              "kernel_DbgPrint")) {
        return fail("kernel ordinal resolution");
    }
    if (db.libraries.size() != 1 || db.libraries[0].name != "XAPILIB" ||
        db.libraries[0].build != 5849) {
        return fail("XBE library-version metadata");
    }
    const auto *primary = XemuXbeLabels::PrimaryAt(db, entry);
    if (primary == nullptr || primary->name != "XBE_EntryPoint") {
        return fail("primary label priority");
    }

    std::puts("PASS: XBE label parser/generator golden cases");
    return 0;
}
