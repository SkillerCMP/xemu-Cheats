//
// xemu RAW Cheat Engine - Keystone-backed 32-bit x86 assembler core
//
// Generic x86 text encoding belongs to Keystone. This file intentionally keeps
// only the small syntax/utility surface that is specific to xemu's F0 frontend:
// hex-first input, label resolution, and one-instruction assembly at a guest EIP.
//

#include "x86-cheat-assembler-internal.hh"

#include <keystone/keystone.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace xemu_cheat_assembler_internal {
namespace {

struct SymbolResolverContext {
    const std::unordered_map<std::string, size_t> *labels = nullptr;
    uint32_t base_address = 0;
    uint32_t fallback_address = 0;
    bool allow_unknown = false;
};

thread_local const SymbolResolverContext *g_symbol_resolver_context = nullptr;

static bool keystone_symbol_resolver(const char *symbol, uint64_t *value)
{
    if (symbol == nullptr || value == nullptr || g_symbol_resolver_context == nullptr) {
        return false;
    }

    const SymbolResolverContext &ctx = *g_symbol_resolver_context;
    if (ctx.labels != nullptr) {
        const std::string key = upper(symbol);
        const auto it = ctx.labels->find(key);
        if (it != ctx.labels->end()) {
            *value = (uint64_t)ctx.base_address + (uint64_t)it->second;
            return true;
        }
    }

    if (ctx.allow_unknown) {
        *value = ctx.fallback_address;
        return true;
    }
    return false;
}

class KeystoneHandle {
public:
    KeystoneHandle() = default;
    ~KeystoneHandle()
    {
        if (m_handle != nullptr) {
            ks_close(m_handle);
        }
    }

    KeystoneHandle(const KeystoneHandle &) = delete;
    KeystoneHandle &operator=(const KeystoneHandle &) = delete;

    bool Open(std::string &error)
    {
        ks_err rc = ks_open(KS_ARCH_X86, KS_MODE_32, &m_handle);
        if (rc != KS_ERR_OK || m_handle == nullptr) {
            error = "Keystone x86-32 initialization failed: ";
            error += ks_strerror(rc);
            return false;
        }

        /* Cheat files are historically hexadecimal-first. Keystone's RADIX16
         * mode preserves that behavior without maintaining a second numeric
         * parser inside the generic x86 encoder. */
        const size_t syntax = (size_t)KS_OPT_SYNTAX_INTEL |
                              (size_t)KS_OPT_SYNTAX_RADIX16;
        rc = ks_option(m_handle, KS_OPT_SYNTAX, syntax);
        if (rc != KS_ERR_OK) {
            error = "Keystone could not enable Intel/hex-first syntax: ";
            error += ks_strerror(rc);
            return false;
        }

        rc = ks_option(m_handle, KS_OPT_SYM_RESOLVER,
                       reinterpret_cast<size_t>(&keystone_symbol_resolver));
        if (rc != KS_ERR_OK) {
            error = "Keystone could not install the F0 label resolver: ";
            error += ks_strerror(rc);
            return false;
        }
        return true;
    }

    ks_engine *get() const { return m_handle; }

private:
    ks_engine *m_handle = nullptr;
};

class ResolverScope {
public:
    explicit ResolverScope(const SymbolResolverContext &context)
        : m_previous(g_symbol_resolver_context)
    {
        g_symbol_resolver_context = &context;
    }
    ~ResolverScope()
    {
        g_symbol_resolver_context = m_previous;
    }

    ResolverScope(const ResolverScope &) = delete;
    ResolverScope &operator=(const ResolverScope &) = delete;

private:
    const SymbolResolverContext *m_previous;
};

static std::string keystone_error(ks_engine *handle, const std::string &text)
{
    std::string error = "Keystone x86 assembler rejected '" + text + "': ";
    error += ks_strerror(ks_errno(handle));
    return error;
}

} // namespace

std::string trim(const std::string &s)
{
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) {
        ++a;
    }
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1])) {
        --b;
    }
    return s.substr(a, b - a);
}

std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char)std::toupper(c);
    });
    return s;
}

std::string strip_comment(const std::string &s)
{
    size_t cut = s.size();
    size_t pos = s.find(';');
    if (pos != std::string::npos) cut = std::min(cut, pos);
    pos = s.find("//");
    if (pos != std::string::npos) cut = std::min(cut, pos);
    pos = s.find('#');
    if (pos != std::string::npos) cut = std::min(cut, pos);
    return trim(s.substr(0, cut));
}

bool parse_register(const std::string &name, int &code, int &width)
{
    const std::string n = upper(trim(name));
    static const char *const regs32[] = {
        "EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI",
    };
    static const char *const regs16[] = {
        "AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI",
    };
    static const char *const regs8[] = {
        "AL", "CL", "DL", "BL", "AH", "CH", "DH", "BH",
    };
    for (int i = 0; i < 8; ++i) {
        if (n == regs32[i]) {
            code = i;
            width = 32;
            return true;
        }
        if (n == regs16[i]) {
            code = i;
            width = 16;
            return true;
        }
        if (n == regs8[i]) {
            code = i;
            width = 8;
            return true;
        }
    }
    return false;
}

int reg32(const std::string &name)
{
    int code = -1;
    int width = 0;
    return parse_register(name, code, width) && width == 32 ? code : -1;
}

bool valid_label(const std::string &name)
{
    if (name.empty()) {
        return false;
    }
    const unsigned char c0 = (unsigned char)name[0];
    if (!(std::isalpha(c0) || c0 == '_' || c0 == '.' || c0 == '$')) {
        return false;
    }
    for (const unsigned char c : name) {
        if (!(std::isalnum(c) || c == '_' || c == '.' || c == '$')) {
            return false;
        }
    }
    return true;
}

/* F0 source is hexadecimal-first. Keep this parser for F0-only directives such
 * as DD and for source-level validation; Keystone itself is configured with
 * KS_OPT_SYNTAX_RADIX16 for normal x86 operands. */
bool parse_number(const std::string &text, int64_t &value)
{
    std::string s = trim(text);
    if (s.empty()) {
        return false;
    }
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') {
        neg = s[0] == '-';
        s.erase(0, 1);
    }
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.erase(0, 2);
    }
    if (!s.empty() && (s.back() == 'h' || s.back() == 'H')) {
        s.pop_back();
    }
    if (s.empty()) {
        return false;
    }

    uint64_t v = 0;
    for (const unsigned char c : s) {
        if (!std::isxdigit(c)) {
            return false;
        }
        const unsigned digit = c >= '0' && c <= '9'
            ? c - '0'
            : (unsigned)(std::toupper(c) - 'A' + 10);
        if (v > (std::numeric_limits<uint64_t>::max() - digit) / 16u) {
            return false;
        }
        v = v * 16u + digit;
    }

    if (neg) {
        if (v > 0x80000000ull) {
            return false;
        }
        value = -(int64_t)v;
    } else {
        if (v > 0xffffffffull) {
            return false;
        }
        value = (int64_t)v;
    }
    return true;
}

std::vector<std::string> split_operands(const std::string &s)
{
    std::vector<std::string> out;
    out.reserve(3);
    int brackets = 0;
    int parens = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '[') ++brackets;
        else if (s[i] == ']') --brackets;
        else if (s[i] == '(') ++parens;
        else if (s[i] == ')') --parens;
        else if (s[i] == ',' && brackets == 0 && parens == 0) {
            out.push_back(trim(s.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (start < s.size() || !s.empty()) {
        out.push_back(trim(s.substr(start)));
    }
    return out;
}

void emit_u32(std::vector<uint8_t> &out, uint32_t v)
{
    out.push_back((uint8_t)(v & 0xffu));
    out.push_back((uint8_t)((v >> 8) & 0xffu));
    out.push_back((uint8_t)((v >> 16) & 0xffu));
    out.push_back((uint8_t)((v >> 24) & 0xffu));
}

bool condition_code(const std::string &mnemonic, uint8_t &cc)
{
    static const std::unordered_map<std::string, uint8_t> codes = {
        {"O",0x0},{"NO",0x1},{"B",0x2},{"NAE",0x2},{"C",0x2},
        {"AE",0x3},{"NB",0x3},{"NC",0x3},{"E",0x4},{"Z",0x4},
        {"NE",0x5},{"NZ",0x5},{"BE",0x6},{"NA",0x6},{"A",0x7},
        {"NBE",0x7},{"S",0x8},{"NS",0x9},{"P",0xA},{"PE",0xA},
        {"NP",0xB},{"PO",0xB},{"L",0xC},{"NGE",0xC},{"GE",0xD},
        {"NL",0xD},{"LE",0xE},{"NG",0xE},{"G",0xF},{"NLE",0xF},
    };
    const auto it = codes.find(mnemonic);
    if (it == codes.end()) {
        return false;
    }
    cc = it->second;
    return true;
}


static bool is_unsized_memory_operand(const std::string &operand)
{
    const std::string u = upper(trim(operand));
    if (u.find('[') == std::string::npos || u.find(']') == std::string::npos) {
        return false;
    }
    static const char *const size_prefixes[] = {
        "BYTE PTR ", "WORD PTR ", "DWORD PTR ", "FWORD PTR ",
        "QWORD PTR ", "TBYTE PTR ", "OWORD PTR ", "XMMWORD PTR ",
        "YMMWORD PTR ", "ZMMWORD PTR ",
    };
    for (const char *prefix : size_prefixes) {
        if (u.rfind(prefix, 0) == 0) {
            return false;
        }
    }
    return true;
}

static const char *memory_size_prefix(int width)
{
    switch (width) {
    case 8:  return "byte ptr ";
    case 16: return "word ptr ";
    case 32: return "dword ptr ";
    case 64: return "qword ptr ";
    case 128:return "xmmword ptr ";
    default: return nullptr;
    }
}

/* Keystone's Intel parser is intentionally stricter than the historical F0
 * encoder. Preserve the old source language at this adapter boundary instead
 * of making cheat authors rewrite existing codes.
 *
 * Old F0 rules that matter here:
 *  - a memory operand without BYTE/WORD/DWORD PTR inherited its width from the
 *    matching integer register when that width was unambiguous;
 *  - single-memory integer operations defaulted to dword where the old
 *    encoder did so;
 *  - hexadecimal-first operands such as FFFFFFFF were numbers even though
 *    Keystone tokenizes an A-F-leading token as a symbol.
 */
static std::string normalize_legacy_hex_tokens(std::string operand)
{
    std::string out;
    out.reserve(operand.size() + 8u);
    size_t i = 0;
    while (i < operand.size()) {
        const unsigned char c = (unsigned char)operand[i];
        if (!(std::isalnum(c) || c == '_' || c == '.' || c == '$')) {
            out.push_back(operand[i++]);
            continue;
        }

        size_t j = i + 1u;
        while (j < operand.size()) {
            const unsigned char d = (unsigned char)operand[j];
            if (!(std::isalnum(d) || d == '_' || d == '.' || d == '$')) {
                break;
            }
            ++j;
        }
        const std::string token = operand.substr(i, j - i);

        /* The F0 language has always been hexadecimal-first. Make every plain
         * hexadecimal operand token explicit before handing it to Keystone.
         * v2.90.5 covered A-F-leading values such as FFFFFFFF but missed the
         * much more common digit-leading absolute-address spelling, e.g.
         * [0046D784]. Labels are substituted before this normalization once
         * the F0 layout pass has a real symbol table. */
        bool plain_hex = !token.empty();
        for (const unsigned char h : token) {
            if (!std::isxdigit(h)) {
                plain_hex = false;
                break;
            }
        }
        if (plain_hex) {
            out += "0x";
        }
        out += token;
        i = j;
    }
    return out;
}

/* Keystone 0.9.2 has a known PC-relative fixup bug when KS_OPT_SYM_RESOLVER
 * supplies an x86 branch/call destination: a short branch lands +1 byte late
 * and a rel32 branch/call lands +4 bytes late. Xemu's iterative F0 layout was
 * already computing the correct final label offsets; the corruption happened
 * only when those correct offsets crossed Keystone's resolver boundary.
 *
 * Once a layout pass has the complete F0 label table, replace label tokens with
 * explicit absolute hexadecimal addresses before ks_asm(). Numeric direct
 * targets use Keystone's normal x86 PC-relative encoder and are not affected by
 * the 0.9.2 symbol-resolver bug. The resolver remains available only for the
 * first sizing probe, where unresolved labels are intentionally mapped to the
 * current EIP and no emitted bytes are retained. */
static std::string resolve_known_f0_labels(
    const std::string &instruction,
    const std::unordered_map<std::string, size_t> &labels,
    uint32_t base_address)
{
    std::string out;
    out.reserve(instruction.size() + 16u);

    size_t i = 0;
    while (i < instruction.size()) {
        const unsigned char c = (unsigned char)instruction[i];
        if (!(std::isalnum(c) || c == '_' || c == '.' || c == '$')) {
            out.push_back(instruction[i++]);
            continue;
        }

        size_t j = i + 1u;
        while (j < instruction.size()) {
            const unsigned char d = (unsigned char)instruction[j];
            if (!(std::isalnum(d) || d == '_' || d == '.' || d == '$')) {
                break;
            }
            ++j;
        }

        const std::string token = instruction.substr(i, j - i);
        const auto it = labels.find(upper(token));
        if (it != labels.end()) {
            const uint64_t absolute =
                (uint64_t)base_address + (uint64_t)it->second;
            char buf[24];
            std::snprintf(buf, sizeof(buf), "0x%08X", (uint32_t)absolute);
            out += buf;
        } else {
            out += token;
        }
        i = j;
    }

    return out;
}

static bool mnemonic_infers_matching_integer_width(const std::string &mnemonic)
{
    static const char *const names[] = {
        "MOV", "ADD", "ADC", "SUB", "SBB", "AND", "OR", "XOR",
        "CMP", "TEST", "XCHG", "XADD", "CMPXCHG", "BSF", "BSR",
        "CMOVO", "CMOVNO", "CMOVB", "CMOVNAE", "CMOVC", "CMOVAE",
        "CMOVNB", "CMOVNC", "CMOVE", "CMOVZ", "CMOVNE", "CMOVNZ",
        "CMOVBE", "CMOVNA", "CMOVA", "CMOVNBE", "CMOVS", "CMOVNS",
        "CMOVP", "CMOVPE", "CMOVNP", "CMOVPO", "CMOVL", "CMOVNGE",
        "CMOVGE", "CMOVNL", "CMOVLE", "CMOVNG", "CMOVG", "CMOVNLE",
    };
    for (const char *name : names) {
        if (mnemonic == name) {
            return true;
        }
    }
    return false;
}

static bool mnemonic_defaults_unsized_memory_to_dword(const std::string &mnemonic)
{
    static const char *const names[] = {
        "PUSH", "POP", "INC", "DEC", "NEG", "NOT", "MUL", "IMUL",
        "DIV", "IDIV", "SHL", "SAL", "SHR", "SAR", "ROL", "ROR",
        "BT", "BTS", "BTR", "BTC",
    };
    for (const char *name : names) {
        if (mnemonic == name) {
            return true;
        }
    }
    return false;
}

static std::string normalize_legacy_f0_instruction(const std::string &line)
{
    const size_t ws = line.find_first_of(" \t");
    if (ws == std::string::npos) {
        return line;
    }

    const std::string mnemonic = upper(trim(line.substr(0, ws)));
    const std::string rest = trim(line.substr(ws + 1u));
    if (rest.empty()) {
        return line;
    }

    std::vector<std::string> ops = split_operands(rest);
    for (std::string &op : ops) {
        op = normalize_legacy_hex_tokens(op);
    }

    if (mnemonic_infers_matching_integer_width(mnemonic) && ops.size() == 2u) {
        int code = -1;
        int width = 0;
        if (is_unsized_memory_operand(ops[1]) &&
            parse_register(ops[0], code, width)) {
            if (const char *prefix = memory_size_prefix(width)) {
                ops[1] = std::string(prefix) + ops[1];
            }
        } else if (is_unsized_memory_operand(ops[0]) &&
                   parse_register(ops[1], code, width)) {
            if (const char *prefix = memory_size_prefix(width)) {
                ops[0] = std::string(prefix) + ops[0];
            }
        }
    }

    /* The historical encoder used dword for these otherwise-unsized integer
     * memory forms. MOVZX/MOVSX deliberately are not here: the old frontend
     * required BYTE/WORD PTR for their memory source too. */
    if (mnemonic_defaults_unsized_memory_to_dword(mnemonic) && !ops.empty() &&
        is_unsized_memory_operand(ops[0])) {
        ops[0] = "dword ptr " + ops[0];
    }

    if (mnemonic.rfind("SET", 0) == 0 && ops.size() == 1u &&
        is_unsized_memory_operand(ops[0])) {
        ops[0] = "byte ptr " + ops[0];
    }

    std::string normalized = trim(line.substr(0, ws));
    normalized.push_back(' ');
    for (size_t i = 0; i < ops.size(); ++i) {
        if (i != 0u) {
            normalized += ", ";
        }
        normalized += ops[i];
    }
    return normalized;
}

bool encode_instruction(const std::string &text, size_t offset,
                        const std::unordered_map<std::string, size_t> *labels,
                        uint32_t base_address, std::vector<uint8_t> &out,
                        std::string &error)
{
    out.clear();
    error.clear();

    const std::string source_line = trim(text);
    if (source_line.empty()) {
        error = "empty x86 instruction";
        return false;
    }

    /* Do this before legacy hexadecimal token normalization so real F0 labels
     * remain symbols even when a legal label happens to look hexadecimal. */
    const std::string resolved_line =
        labels != nullptr
            ? resolve_known_f0_labels(source_line, *labels, base_address)
            : source_line;
    const std::string line = normalize_legacy_f0_instruction(resolved_line);

    KeystoneHandle engine;
    if (!engine.Open(error)) {
        return false;
    }

    SymbolResolverContext resolver;
    resolver.labels = labels;
    resolver.base_address = base_address;
    resolver.fallback_address = base_address + (uint32_t)offset;
    resolver.allow_unknown = labels == nullptr;
    ResolverScope scope(resolver);

    unsigned char *encoding = nullptr;
    size_t encoding_size = 0;
    size_t statement_count = 0;
    const uint64_t address = (uint64_t)base_address + (uint64_t)offset;
    const int rc = ks_asm(engine.get(), line.c_str(), address,
                          &encoding, &encoding_size, &statement_count);
    if (rc != 0 || encoding == nullptr || encoding_size == 0) {
        if (encoding != nullptr) {
            ks_free(encoding);
        }
        error = keystone_error(engine.get(), line);
        if (line != source_line) {
            error += " (from legacy F0 syntax '" + source_line + "')";
        }
        return false;
    }

    if (statement_count != 1u) {
        ks_free(encoding);
        error = "F0 expects exactly one x86 instruction per source line";
        return false;
    }
    if (encoding_size > 15u) {
        ks_free(encoding);
        error = "Keystone produced an x86 instruction longer than 15 bytes";
        return false;
    }

    out.assign(encoding, encoding + encoding_size);
    ks_free(encoding);
    return true;
}

} // namespace xemu_cheat_assembler_internal
