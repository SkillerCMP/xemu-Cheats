// v2.87 current regression ownership.
// v2.90: semantic coverage for the Keystone-backed F0 IA-32 assembler.
#include "../x86-cheat-assembler.hh"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

std::vector<XemuCheatAsmLine> source(std::initializer_list<const char *> text)
{
    std::vector<XemuCheatAsmLine> out;
    out.reserve(text.size());
    int line = 1;
    for (const char *item : text) {
        out.push_back({line++, item});
    }
    return out;
}

bool expect_success(const char *name, std::initializer_list<const char *> text,
                    uint32_t preserve_bytes = 0, uint32_t temp_bytes = 0,
                    size_t data_size = 0)
{
    XemuCheatAsmResult result;
    const bool ok = xemu_cheat_assemble_x86_32_at(
        source(text), 0x68010000u, 0x680F0000u, 0x680F1000u, result);
    if (!ok || !result.ok || result.bytes.empty() || result.error_line != 0 ||
        !result.error.empty() || result.preserve_bytes != preserve_bytes ||
        result.temp_bytes != temp_bytes || result.data.size() != data_size) {
        std::fprintf(stderr,
                     "FAIL %-22s ok=%d result.ok=%d line=%d bytes=%zu data=%zu "
                     "preserve=%u temp=%u error='%s'\n",
                     name, ok ? 1 : 0, result.ok ? 1 : 0, result.error_line,
                     result.bytes.size(), result.data.size(),
                     result.preserve_bytes, result.temp_bytes,
                     result.error.c_str());
        return false;
    }
    return true;
}

bool expect_error_contains(const char *name,
                           std::initializer_list<const char *> text,
                           int expected_line, const char *needle)
{
    XemuCheatAsmResult result;
    const bool ok = xemu_cheat_assemble_x86_32_at(
        source(text), 0x68010000u, 0x680F0000u, 0x680F1000u, result);
    if (ok || result.ok || result.error_line != expected_line ||
        result.error.find(needle) == std::string::npos) {
        std::fprintf(stderr,
                     "FAIL %-22s ok=%d result.ok=%d line=%d error='%s' "
                     "(wanted line=%d containing '%s')\n",
                     name, ok ? 1 : 0, result.ok ? 1 : 0, result.error_line,
                     result.error.c_str(), expected_line, needle);
        return false;
    }
    return true;
}

bool check_dd_layout()
{
    XemuCheatAsmResult result;
    if (!xemu_cheat_assemble_x86_32_at(
            source({"mov edx, CarList", "mov eax, [edx]", "ret",
                    "CarList:", "dd 01D28710, 8B80C5FC, E3BDE8CB"}),
            0x68010000u, 0, 0, result)) {
        std::fprintf(stderr, "FAIL dd_label: %s\n", result.error.c_str());
        return false;
    }
    const std::vector<uint8_t> expected = {
        0x10, 0x87, 0xD2, 0x01,
        0xFC, 0xC5, 0x80, 0x8B,
        0xCB, 0xE8, 0xBD, 0xE3,
    };
    if (result.data != expected || result.bytes.empty()) {
        std::fprintf(stderr, "FAIL dd_label data/layout\n");
        return false;
    }
    return true;
}

bool check_change_branches()
{
    XemuCheatAsmResult result;
    if (!xemu_cheat_assemble_x86_32_change_instruction(
            "jmp 0008C5A2", 0x0008C590u, 2u, result) ||
        result.bytes != std::vector<uint8_t>({0xEB, 0x10})) {
        std::fprintf(stderr, "FAIL change_short_jmp (%s)\n", result.error.c_str());
        return false;
    }
    if (!xemu_cheat_assemble_x86_32_change_instruction(
            "jne 0008C5A2", 0x0008C590u, 2u, result) ||
        result.bytes != std::vector<uint8_t>({0x75, 0x10})) {
        std::fprintf(stderr, "FAIL change_short_jcc (%s)\n", result.error.c_str());
        return false;
    }
    if (!xemu_cheat_assemble_x86_32_change_instruction(
            "jmp 00100000", 0x0008C590u, 6u, result) ||
        result.bytes.size() != 5u || result.bytes[0] != 0xE9) {
        std::fprintf(stderr, "FAIL change_near_jmp (%s)\n", result.error.c_str());
        return false;
    }
    if (xemu_cheat_assemble_x86_32_change_instruction(
            "mov eax, 12345678", 0x0008C590u, 4u, result) ||
        result.error.find("Keystone encoding needs") == std::string::npos) {
        std::fprintf(stderr, "FAIL change_size_guard (%s)\n", result.error.c_str());
        return false;
    }
    return true;
}


bool check_f0_label_branch_targets()
{
    XemuCheatAsmResult result;

    /* Keystone 0.9.2's symbol resolver encodes PC-relative x86 targets from
     * the wrong fixup origin. These exact cases are the minimal forms of the
     * +1 short-branch and +4 rel32 corruption seen in the CarList F0. */
    if (!xemu_cheat_assemble_x86_32_at(
            source({"Loop:", "jmp Loop"}),
            0x68010000u, 0, 0, result) ||
        result.bytes != std::vector<uint8_t>({0xEB, 0xFE})) {
        std::fprintf(stderr, "FAIL f0_backward_short_label (%s)\n",
                     result.error.c_str());
        return false;
    }

    if (!xemu_cheat_assemble_x86_32_at(
            source({"call Target", "Target:", "nop"}),
            0x68010000u, 0, 0, result) ||
        result.bytes.size() != 6u || result.bytes[0] != 0xE8 ||
        result.bytes[1] != 0x00 || result.bytes[2] != 0x00 ||
        result.bytes[3] != 0x00 || result.bytes[4] != 0x00 ||
        result.bytes[5] != 0x90) {
        std::fprintf(stderr, "FAIL f0_forward_rel32_label (%s)\n",
                     result.error.c_str());
        return false;
    }

    if (!xemu_cheat_assemble_x86_32_at(
            source({"Here:", "cmp eax, eax", "je Here"}),
            0x68010000u, 0, 0, result) ||
        result.bytes != std::vector<uint8_t>({0x39, 0xC0, 0x74, 0xFC})) {
        std::fprintf(stderr, "FAIL f0_backward_jcc_label (%s)\n",
                     result.error.c_str());
        return false;
    }

    return true;
}

bool check_legacy_absolute_memory_hex()
{
    XemuCheatAsmResult result;
    if (!xemu_cheat_assemble_x86_32_at(
            source({"mov cl, [0046D784]"}),
            0x68010000u, 0, 0, result) ||
        result.bytes != std::vector<uint8_t>({0x8A, 0x0D, 0x84, 0xD7, 0x46, 0x00})) {
        std::fprintf(stderr, "FAIL legacy_absolute_memory_hex (%s)\n",
                     result.error.c_str());
        return false;
    }
    return true;
}

bool check_existing_car_list_f0_control_flow()
{
    XemuCheatAsmResult result;
    if (!xemu_cheat_assemble_x86_32_at(
            source({"mov T0, CarList", "CheckCar:", "mov T1, [T0]",
                    "test T1, T1", "jz Original", "cmp eax, T1",
                    "je Success", "add T0, 4", "jmp CheckCar",
                    "Success:", "mov eax, 1", "mov ecx, [ebp+8]",
                    "mov [ecx], eax", "pop esi", "pop ebp", "ret 8",
                    "Original:", "mov cl, [0046D784]", "CarList:",
                    "dd 01D28710", "dd 8B80C5FC", "dd E3BDE8CB",
                    "dd 0001308E", "dd 60D6890E", "dd 0689441B",
                    "dd 00000000"}),
            0x68000000u, 0x680F0000u, 0, result)) {
        std::fprintf(stderr, "FAIL existing_car_list_control_flow: %s\n",
                     result.error.c_str());
        return false;
    }

    struct ByteCheck { size_t offset; uint8_t value; };
    const ByteCheck checks[] = {
        {0x45u, 0x74u}, {0x46u, 0x09u}, /* JE internal taken @ 0x50 */
        {0x4Eu, 0xEBu}, {0x4Fu, 0x09u}, /* JMP internal done @ 0x59 */
        {0x57u, 0xEBu}, {0x58u, 0x7Bu}, /* JMP Original @ 0xD4 */
        {0x89u, 0x74u}, {0x8Au, 0x09u}, /* JE internal taken @ 0x94 */
        {0x92u, 0xEBu}, {0x93u, 0x09u}, /* JMP internal done @ 0x9D */
        {0x9Bu, 0xEBu}, {0x9Cu, 0x28u}, /* JMP Success @ 0xC5 */
        {0xC0u, 0xE9u}, {0xC1u, 0x45u}, {0xC2u, 0xFFu},
        {0xC3u, 0xFFu}, {0xC4u, 0xFFu}, /* JMP CheckCar @ 0x0A */
    };
    for (const ByteCheck &check : checks) {
        if (check.offset >= result.bytes.size() ||
            result.bytes[check.offset] != check.value) {
            std::fprintf(stderr,
                         "FAIL existing_car_list_control_flow byte[%zu]=%02X wanted=%02X size=%zu\n",
                         check.offset,
                         check.offset < result.bytes.size() ? result.bytes[check.offset] : 0u,
                         check.value, result.bytes.size());
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    // Existing F0 syntax and directive behavior must remain source-compatible.
    if (!expect_success("basic/control-flow",
                        {"mov eax, 12345678", "add eax, 4", "cmp eax, 1234567C",
                         "jne Skip", "xor ecx, ecx", "Skip:", "ret"}) ||
        !expect_success("call/labels",
                        {"call Worker", "jmp Done", "Worker:", "mov eax, 1",
                         "ret", "Done:", "nop"}) ||
        !check_dd_layout() ||
        !expect_success("preserve",
                        {"PRESERVE EAX, ECX, EDX", "mov eax, 1234",
                         "add ecx, eax", "RESTORE", "ret"}, 784u) ||
        !expect_success("temp-registers",
                        {"mov T0, 81234567", "mov T1, [T0]", "add T1, 10",
                         "mov [T0], T1", "cmp T1, 20", "setne al", "ret"},
                        0u, 40u) ||
        // Real existing F0 code supplied for the v2.90 compatibility test.
        // The semantic regression below verifies the exact internal and user
        // label destinations, not merely that Keystone returned success.
        !check_existing_car_list_f0_control_flow()) {
        return 1;
    }

    // v2.90.5 regression: legacy F0 spellings stay accepted even though
    // Keystone itself is stricter about memory widths and A-F-leading hex.
    if (!expect_success("legacy-keystone-syntax",
                        {"mov cl, [0046D784]", "mov ecx, [ebp+8]",
                         "mov [ecx], eax", "mov eax, FFFFFFFF",
                         "inc [eax]", "setne [eax]", "ret"}) ||
        !check_legacy_absolute_memory_hex() ||
        !check_f0_label_branch_targets()) {
        return 1;
    }

    // v2.90 regression: forms the old hand-written encoder did not own are now
    // ordinary Keystone IA-32. These tests deliberately span x87/MMX/SSE and
    // less-common integer/addressing syntax instead of enumerating opcodes in xemu.
    if (!expect_success("x87",
                        {"fld dword ptr [eax+20]", "fadd dword ptr [ebx+4]",
                         "fstp dword ptr [ecx+8]", "ret"}) ||
        !expect_success("mmx",
                        {"movq mm0, mm1", "paddd mm0, mm2", "pxor mm3, mm3",
                         "emms", "ret"}) ||
        !expect_success("sse",
                        {"movaps xmm0, xmm1", "movups xmm2, [eax]",
                         "xorps xmm0, xmm0", "shufps xmm0, xmm1, 1B", "ret"}) ||
        !expect_success("extended-integer",
                        {"bswap eax", "xadd dword ptr [ebx+4], eax",
                         "cmpxchg dword ptr [esi], edx", "bt eax, ecx",
                         "bts dword ptr [edi], 3", "ret"}) ||
        !expect_success("segment-override",
                        {"mov eax, dword ptr fs:[30]", "ret"})) {
        return 1;
    }

    if (!expect_error_contains("invalid-opcode", {"frobnicate eax, 1"}, 1,
                               "Keystone x86 assembler rejected") ||
        !expect_error_contains("invalid-temp", {"mov T8, 1"}, 1,
                               "F0 temp registers are limited to T0-T7") ||
        !expect_error_contains("duplicate-label",
                               {"Again:", "nop", "Again:", "ret"}, 3,
                               "duplicate label")) {
        return 1;
    }

    if (!check_change_branches()) {
        return 1;
    }

    std::printf("PASS: Keystone-backed F0 assembler keeps existing syntax/current CarList F0 and accepts broad IA-32 x87/MMX/SSE/integer forms\n");
    return 0;
}
