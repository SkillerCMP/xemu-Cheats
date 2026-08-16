//
// xemu RAW Cheat Engine
//
// Copyright (C) 2026 xemu contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#include "cheat-engine.hh"
#include "current-game.hh"
#include "../font-manager.hh"
#include "../misc.hh"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include <glib.h>

#include "cheat-engine-memory.h"

CheatEngineWindow cheat_engine_window;

static std::string TypeFHex32(uint32_t value)
{
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08X", value);
    return std::string(buf);
}

/* Type-F hook/cave updates must be atomic from the guest CPU's point of
 * view.  A running vCPU must not execute a cave while its bytes are being
 * rewritten or observe a half-written E9 hook.  Preserve an existing paused
 * or debugger-stopped state; only resume when this guard paused a running VM. */
class TypeFGuestPauseGuard {
public:
    TypeFGuestPauseGuard()
        : m_resume(runstate_is_running())
    {
        if (m_resume) {
            vm_stop(RUN_STATE_PAUSED);
        }
    }

    ~TypeFGuestPauseGuard()
    {
        if (m_resume) {
            vm_start();
        }
    }

    TypeFGuestPauseGuard(const TypeFGuestPauseGuard &) = delete;
    TypeFGuestPauseGuard &operator=(const TypeFGuestPauseGuard &) = delete;

private:
    bool m_resume;
};

static const char *kInitialSource = R"CHEATS(; xemu RAW Cheat Engine / CMP-style code file
; Current-game files are loaded from the "Cheats" folder beside xemu.exe.
; The filename can be readable (for example EA009E-<hash>.txt), but the
; ^1/^2 header is authoritative when matching a running game.
;
; Accepted GameID forms are equivalent:
;   ^2 = GameID: EA009E
;   ^2 = GameID: 4541009E
;
; Groups use CMP-style nesting:
;   !Group Name:
;       +Cheat Name{Optional description}
;       %Credits: Author>Another Author
;       $XXXXXXXX YYYYYYYY
;   !!
;
; The cheat list starts Disabled. Press the Disabled/Enabled button to enable
; live selection; after that, clicking a cheat activates/deactivates it immediately.
;
; Supported RAW memory types: 0/1/2/3/4/5/6/7, custom stackable 9,
; custom A raw-byte fill/write, D/E conditions, and F0 assembly/F1 raw external x86 code caves. A leading '$' on RAW lines is accepted.
;
; Default address space is VIRTUAL RAM. Ordinary codes use the literal guest
; virtual address encoded in the RAW line. Type 9 is only needed to select an
; explicit base/context such as physical RAM or pointer-based addressing.

^1 = Hash: 0000000000000000
^2 = GameID: EA009E
^3 = NAME: Example Game

!Player Codes:
+Example - Disabled{This is only a format example.}
%Credits: Skiller
$20000120 000003E7
$10000130 00000064
$00000140 000000FF
!!
)CHEATS";

CheatEngineWindow::CheatEngineWindow()
    : m_source(kInitialSource)
{
    ParseSource(false);
}

void CheatEngineWindow::GetActiveF0TempBanks(
    std::vector<FTempBankInfo> &out) const
{
    if (out.capacity() < m_f_hooks.size()) {
        out.reserve(m_f_hooks.size());
    }

    size_t count = 0;
    for (const auto &entry : m_f_hooks) {
        const FHookState &state = entry.second;
        if (!state.installed || state.temp_entry == 0 || state.temp_size < 40u) {
            continue;
        }

        if (count == out.size()) {
            out.emplace_back();
        }
        FTempBankInfo &info = out[count++];
        info.cheat_name.clear();
        if (state.owner_block < m_blocks.size()) {
            info.cheat_name = m_blocks[state.owner_block].name;
        }
        if (info.cheat_name.empty()) {
            info.cheat_name = "F0 @ " + TypeFHex32(state.hook_address);
        }
        info.hook_address = state.hook_address;
        info.cave_address = state.external_entry;
        info.cave_size = state.allocation_size;
        info.temp_address = state.temp_entry;
    }
    out.resize(count);
    std::sort(out.begin(), out.end(),
              [](const FTempBankInfo &a, const FTempBankInfo &b) {
                  if (a.hook_address != b.hook_address) {
                      return a.hook_address < b.hook_address;
                  }
                  return a.cheat_name < b.cheat_name;
              });
}

bool CheatEngineWindow::ParseDebuggerF0Source(
    uint32_t hook_address, const std::string &source, RawCode &code,
    std::string &error) const
{
    code = {};
    error.clear();

    std::istringstream stream(source);
    std::string line;
    int line_number = 0;
    bool have_header = false;
    bool terminated = false;

    while (std::getline(stream, line)) {
        ++line_number;
        const std::string normalized = NormalizeTypeFLine(line);
        const std::string directive = TypeFDirective(normalized);
        if (directive.empty()) {
            continue;
        }

        if (!have_header) {
            RawCode header;
            if (!ParseCodeLine(line, header, line_number) ||
                header.command != 0xF0000000u) {
                error = "The first non-comment line must be $F0000000 AAAAAAAA.";
                return false;
            }
            if (header.value != hook_address) {
                error = "The F0 hook address must remain " + TypeFHex32(hook_address) +
                        " for this debugger selection.";
                return false;
            }
            code = std::move(header);
            have_header = true;
            continue;
        }

        if (!terminated) {
            std::istringstream tokens(directive);
            std::string name;
            std::string arg;
            std::string extra;
            tokens >> name >> arg >> extra;
            if (Upper(name) == "DEADCODE") {
                if (!arg.empty() || !extra.empty()) {
                    error = "Type-F0 DEADCODE must not have an operand (line " +
                            std::to_string(line_number) + ").";
                    return false;
                }
                terminated = true;
                continue;
            }

            code.f_body.push_back(XemuCheatAsmLine{line_number, normalized});
            continue;
        }

        /* Match the normal source parser: after DEADCODE only label-only and
         * DD declarations belong to this F0. They are physically attached
         * after the generated return JMP. */
        std::string after_label = directive;
        const size_t colon = after_label.find(':');
        bool label_only = false;
        if (colon != std::string::npos) {
            const std::string label_name = Trim(after_label.substr(0, colon));
            label_only = !label_name.empty() &&
                         Trim(after_label.substr(colon + 1)).empty();
            after_label = Trim(after_label.substr(colon + 1));
        }
        std::istringstream data_tokens(after_label);
        std::string data_name;
        data_tokens >> data_name;
        const bool is_dd = Upper(data_name) == "DD";
        if (!label_only && !is_dd) {
            error = "Only label/DD static data may follow DEADCODE (line " +
                    std::to_string(line_number) + ").";
            return false;
        }
        code.f_body.push_back(XemuCheatAsmLine{line_number, normalized});
    }

    if (!have_header) {
        error = "The Code Cave source is empty.";
        return false;
    }
    if (!terminated) {
        error = "Type-F0 source must end its executable section with $DEADCODE.";
        return false;
    }
    if (code.f_body.empty()) {
        error = "Type-F0 code cave contains no executable instructions.";
        return false;
    }

    code.f_terminated = true;
    return true;
}

void CheatEngineWindow::FillDebuggerF0HookInfo(
    const FHookState &state, DebuggerF0HookInfo &info) const
{
    info = {};
    info.installed = state.installed;
    info.hook_address = state.hook_address;
    info.overwrite_length = state.overwrite_length;
    info.cave_address = state.external_entry;
    info.code_size = state.code_size;
    info.return_address = state.hook_address + state.overwrite_length;
}

bool CheatEngineWindow::GetDebuggerF0HookInfo(DebuggerF0HookInfo &info) const
{
    info = {};
    const auto it = m_f_hooks.find(kDebuggerFHookKey);
    if (it == m_f_hooks.end()) {
        return false;
    }
    FillDebuggerF0HookInfo(it->second, info);
    return true;
}

bool CheatEngineWindow::ActiveFHookOwnsAddress(uint32_t address) const
{
    for (const auto &entry : m_f_hooks) {
        const FHookState &state = entry.second;
        if (!state.installed) {
            continue;
        }
        const uint64_t probe = address;
        const uint64_t hook_start = state.hook_address;
        const uint64_t hook_end = hook_start + state.overwrite_length;
        const uint64_t cave_start = state.external_entry;
        const uint64_t cave_end = cave_start + state.allocation_size;
        if ((probe >= hook_start && probe < hook_end) ||
            (state.external_entry != 0 && probe >= cave_start && probe < cave_end)) {
            return true;
        }
    }
    return false;
}

bool CheatEngineWindow::InstallDebuggerF0(
    uint32_t hook_address, const std::string &source,
    DebuggerF0HookInfo &info, std::string &status)
{
    info = {};
    status.clear();

    RawCode code;
    std::string parse_error;
    if (!ParseDebuggerF0Source(hook_address, source, code, parse_error)) {
        status = "Code Cave: " + parse_error;
        return false;
    }

    XemuCheatAsmResult assembled;
    if (!xemu_cheat_assemble_x86_32(code.f_body, assembled)) {
        status = "Code Cave assembler error";
        if (assembled.error_line > 0) {
            status += " on line " + std::to_string(assembled.error_line);
        }
        if (!assembled.error.empty()) {
            status += ": " + assembled.error;
        }
        return false;
    }

    std::string signature = "F0\n";
    for (const XemuCheatAsmLine &src : code.f_body) {
        signature += src.text;
        signature.push_back('\n');
    }

    m_last_runtime_message.clear();
    if (!InstallFHook(kDebuggerFHookOwner, kDebuggerFHookKey, hook_address,
                      assembled.bytes, assembled.data, &code.f_body,
                      assembled.uses_preserve, assembled.preserve_bytes,
                      assembled.uses_temp, assembled.temp_bytes, signature)) {
        status = m_last_runtime_message.empty()
                     ? "Code Cave hook installation failed."
                     : m_last_runtime_message;
        return false;
    }

    if (!GetDebuggerF0HookInfo(info) || !info.installed) {
        status = "Code Cave hook installed, but its debugger state could not be read.";
        return false;
    }

    status = "Code Cave RUNNING: " + TypeFHex32(info.hook_address) +
             " -> " + TypeFHex32(info.cave_address) +
             ", return " + TypeFHex32(info.return_address) + ".";
    return true;
}

bool CheatEngineWindow::RemoveDebuggerF0(std::string &status)
{
    status.clear();
    auto it = m_f_hooks.find(kDebuggerFHookKey);
    if (it == m_f_hooks.end() ||
        (!it->second.installed && !FHookHasTrackedEntries(it->second))) {
        status = "Code Cave is not running.";
        return true;
    }

    const uint32_t hook_address = it->second.hook_address;
    DeactivateFHook(kDebuggerFHookKey);
    it = m_f_hooks.find(kDebuggerFHookKey);
    if (it != m_f_hooks.end() && it->second.installed) {
        status = "Code Cave RESTORE failed; the hook is still active.";
        return false;
    }

    status = "Code Cave restored original bytes at " + TypeFHex32(hook_address) + ".";
    return true;
}

std::string CheatEngineWindow::Trim(const std::string &s)
{
    size_t first = 0;
    while (first < s.size() && std::isspace((unsigned char)s[first])) {
        ++first;
    }
    size_t last = s.size();
    while (last > first && std::isspace((unsigned char)s[last - 1])) {
        --last;
    }
    return s.substr(first, last - first);
}

bool CheatEngineWindow::ParseCodeLine(const std::string &input, RawCode &out,
                                      int source_line)
{
    std::string line = Trim(input);
    if (line.empty()) {
        return false;
    }

    if (line[0] == '$') {
        line.erase(0, 1);
        line = Trim(line);
    }

    // Remove trailing C/C++ or shell-style comments. Semicolon comments are
    // accepted only when they start the line so hex parsing stays unambiguous.
    size_t comment = line.find("//");
    if (comment != std::string::npos) {
        line.erase(comment);
    }
    comment = line.find('#');
    if (comment != std::string::npos) {
        line.erase(comment);
    }
    line = Trim(line);

    unsigned long long command = 0;
    unsigned long long value = 0;
    char extra = 0;
    int fields = std::sscanf(line.c_str(), "%8llx %8llx %c", &command, &value,
                             &extra);
    if (fields != 2 || command > 0xFFFFFFFFull || value > 0xFFFFFFFFull) {
        return false;
    }

    out.command = (uint32_t)command;
    out.value = (uint32_t)value;
    out.source_line = source_line;
    out.source = Trim(input);
    return true;
}

std::string CheatEngineWindow::Upper(const std::string &s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return (char)std::toupper(c);
    });
    return out;
}

std::string CheatEngineWindow::NormalizeTypeFLine(const std::string &line)
{
    std::string normalized = Trim(line);
    if (!normalized.empty() && normalized[0] == '$') {
        normalized = Trim(normalized.substr(1));
    }
    return normalized;
}

std::string CheatEngineWindow::TypeFDirective(const std::string &line)
{
    std::string directive = line;
    size_t cut = directive.size();
    size_t comment = directive.find(';');
    if (comment != std::string::npos) {
        cut = std::min(cut, comment);
    }
    comment = directive.find("//");
    if (comment != std::string::npos) {
        cut = std::min(cut, comment);
    }
    comment = directive.find('#');
    if (comment != std::string::npos) {
        cut = std::min(cut, comment);
    }
    directive.erase(cut);
    return Trim(directive);
}

bool CheatEngineWindow::ParseDeadcodeCount(const std::string &text,
                                           uint8_t &out)
{
    if (text.size() != 8) {
        return false;
    }
    uint32_t value = 0;
    for (char c : text) {
        unsigned char u = (unsigned char)c;
        int nibble = -1;
        if (u >= '0' && u <= '9') {
            nibble = u - '0';
        } else {
            u = (unsigned char)std::toupper(u);
            if (u >= 'A' && u <= 'F') {
                nibble = u - 'A' + 10;
            }
        }
        if (nibble < 0) {
            return false;
        }
        value = (value << 4) | (uint32_t)nibble;
    }
    if (value < 1u || value > 8u) {
        return false;
    }
    out = (uint8_t)value;
    return true;
}

bool CheatEngineWindow::ParseGameId(const std::string &text,
                                    uint32_t &title_id)
{
    std::string value = Upper(Trim(text));
    if (value.size() == 8) {
        char *end = nullptr;
        errno = 0;
        unsigned long parsed = std::strtoul(value.c_str(), &end, 16);
        if (errno == 0 && end && *end == '\0' && parsed <= 0xFFFFFFFFul) {
            title_id = (uint32_t)parsed;
            return true;
        }
        return false;
    }

    if (value.size() == 6 &&
        std::isxdigit((unsigned char)value[2]) &&
        std::isxdigit((unsigned char)value[3]) &&
        std::isxdigit((unsigned char)value[4]) &&
        std::isxdigit((unsigned char)value[5])) {
        char *end = nullptr;
        unsigned long game = std::strtoul(value.c_str() + 2, &end, 16);
        if (!end || *end != '\0' || game > 0xFFFFul) {
            return false;
        }
        title_id = ((uint32_t)(uint8_t)value[0] << 24) |
                   ((uint32_t)(uint8_t)value[1] << 16) |
                   (uint32_t)game;
        return true;
    }
    return false;
}

bool CheatEngineWindow::ParseHeader(const std::string &source, FileHeader &out)
{
    out = FileHeader{};
    std::istringstream stream(source);
    std::string line;
    int inspected = 0;
    while (std::getline(stream, line) && inspected < 64) {
        std::string t = Trim(line);
        if (t.empty() || t[0] == ';') {
            continue;
        }
        ++inspected;
        if (t.empty() || t[0] != '^') {
            // Header fields are expected at the head, but allow comments and
            // formatting around them. Once actual cheat/group data begins the
            // header scan is complete.
            if (t[0] == '+' || t[0] == '!' || t[0] == '$' ||
                std::isxdigit((unsigned char)t[0])) {
                break;
            }
            continue;
        }

        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string lhs = Upper(Trim(t.substr(0, eq)));
        std::string rhs = Trim(t.substr(eq + 1));
        size_t colon = rhs.find(':');
        std::string key = colon == std::string::npos
                              ? std::string()
                              : Upper(Trim(rhs.substr(0, colon)));
        std::string value = colon == std::string::npos
                                ? rhs
                                : Trim(rhs.substr(colon + 1));

        if (lhs == "^1" && (key.empty() || key == "HASH")) {
            out.hash = Upper(value);
        } else if (lhs == "^2" &&
                   (key.empty() || key == "GAMEID" || key == "GAME ID")) {
            out.game_id = Upper(value);
            out.title_id_valid = ParseGameId(value, out.title_id);
        } else if (lhs == "^3" && (key.empty() || key == "NAME")) {
            out.name = value;
        }
    }
    return !out.hash.empty() || !out.game_id.empty() || !out.name.empty();
}

bool CheatEngineWindow::HashMatches(const std::string &file_hash,
                                    const std::string &current_hash)
{
    std::string a = Upper(Trim(file_hash));
    std::string b = Upper(Trim(current_hash));
    if (a.rfind("0X", 0) == 0) {
        a.erase(0, 2);
    }
    if (a.empty() || b.empty() || a.size() > b.size()) {
        return false;
    }
    for (char c : a) {
        if (!std::isxdigit((unsigned char)c)) {
            return false;
        }
    }
    return b.compare(0, a.size(), a) == 0;
}

std::string CheatEngineWindow::CheatDirectory() const
{
    char executable_dir[4096] = {};
    if (!xemu_cheat_get_executable_dir(executable_dir, sizeof(executable_dir))) {
        // Do not fall back to the xemu settings/AppData path. A relative
        // Cheats path is only an emergency fallback if executable discovery
        // is unavailable during an unusual startup/error state.
        return "Cheats";
    }

    gchar *path = g_build_filename(executable_dir, "Cheats", nullptr);
    std::string result = path ? path : "Cheats";
    g_free(path);
    return result;
}

bool CheatEngineWindow::OpenPathExternally(const std::string &path)
{
    GError *error = nullptr;
    gchar *absolute = g_canonicalize_filename(path.c_str(), nullptr);
    gchar *uri = absolute ? g_filename_to_uri(absolute, nullptr, &error) : nullptr;
    if (!uri) {
        m_file_status = "Unable to open path: ";
        m_file_status += error ? error->message : path;
        if (error) {
            g_error_free(error);
        }
        g_free(absolute);
        return false;
    }

    const bool ok = SDL_OpenURL(uri);
    if (!ok) {
        m_file_status = "Unable to open path: ";
        m_file_status += SDL_GetError();
    }
    g_free(uri);
    g_free(absolute);
    return ok;
}

std::string CheatEngineWindow::SuggestedCurrentCheatPath() const
{
    const auto &game = current_game_manager.Get();
    if (!game.valid) {
        return {};
    }

    const std::string id =
        Upper(CurrentGameManager::FormatDatabaseGameId(game.title_id));
    const std::string hash = game.header_sha256.size() >= 16
                                 ? Upper(game.header_sha256.substr(0, 16))
                                 : Upper(game.header_sha256);
    const std::string filename = id + "-" + hash + ".txt";
    const std::string dir = CheatDirectory();
    gchar *path = g_build_filename(dir.c_str(), filename.c_str(), nullptr);
    std::string result = path ? path : filename;
    g_free(path);
    return result;
}

bool CheatEngineWindow::OpenCheatDirectory()
{
    const std::string dir = CheatDirectory();
    if (g_mkdir_with_parents(dir.c_str(), 0755) != 0) {
        m_file_status = "Could not create/open cheat folder: " + dir;
        return false;
    }
    return OpenPathExternally(dir);
}

bool CheatEngineWindow::ReloadCurrentCheatFile()
{
    if (!m_loaded_path.empty() && g_file_test(m_loaded_path.c_str(), G_FILE_TEST_IS_REGULAR)) {
        DisableAllCheats(true);
        return LoadSourceFile(m_loaded_path);
    }

    m_seen_game_generation = current_game_manager.Generation();
    DisableAllCheats(true);
    return LoadMatchingCurrentGameFile(true);
}

bool CheatEngineWindow::EditOrCreateCurrentCheatFile()
{
    std::string path = m_loaded_path;
    if (path.empty()) {
        path = SuggestedCurrentCheatPath();
    }
    if (path.empty()) {
        m_file_status = "No running XBE detected; cannot create a current-game cheat file.";
        return false;
    }

    const std::string dir = CheatDirectory();
    if (g_mkdir_with_parents(dir.c_str(), 0755) != 0) {
        m_file_status = "Could not create/open cheat folder: " + dir;
        return false;
    }

    if (!g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR)) {
        const auto &game = current_game_manager.Get();
        if (!game.valid) {
            m_file_status = "No running XBE detected; cannot create a current-game cheat file.";
            return false;
        }

        std::ostringstream source;
        source << "^1 = Hash: " << game.header_sha256 << "\n";
        source << "^2 = GameID: "
               << CurrentGameManager::FormatDatabaseGameId(game.title_id) << "\n";
        source << "^3 = NAME: "
               << (game.title_name.empty() ? "Unknown Game" : game.title_name)
               << "\n\n";
        source << "; Add CMP-style groups and +Cheat blocks below.\n";

        GError *error = nullptr;
        if (!g_file_set_contents(path.c_str(), source.str().c_str(), -1, &error)) {
            m_file_status = "Unable to create cheat file: ";
            m_file_status += error ? error->message : path;
            if (error) {
                g_error_free(error);
            }
            return false;
        }
        LoadSourceFile(path);
    }

    return OpenPathExternally(path);
}

bool CheatEngineWindow::LoadSourceFile(const std::string &path)
{
    gchar *contents = nullptr;
    gsize length = 0;
    GError *error = nullptr;
    if (!g_file_get_contents(path.c_str(), &contents, &length, &error)) {
        m_file_status = "Unable to read cheat file: ";
        m_file_status += error ? error->message : path;
        if (error) {
            g_error_free(error);
        }
        return false;
    }

    m_source.assign(contents, length);
    g_free(contents);
    m_loaded_path = path;
    ParseSource(false);
    m_file_status = "Loaded: " + path;
    return true;
}

bool CheatEngineWindow::LoadMatchingCurrentGameFile(bool force_reload)
{
    const auto &game = current_game_manager.Get();
    if (!game.valid) {
        DeactivateAllFHooks();
        for (auto &block : m_blocks) {
            block.enabled = false;
        }
        m_switches.clear();
        m_loaded_path.clear();
        m_blocks.clear();
        m_groups.clear();
        m_groups.push_back(CheatGroup{"", {}, {}});
        m_file_status = "No running XBE detected; no code file loaded.";
        return false;
    }

    const std::string dir = CheatDirectory();
    if (g_mkdir_with_parents(dir.c_str(), 0755) != 0) {
        m_file_status = "Could not create/open cheat folder: " + dir;
        return false;
    }

    GError *dir_error = nullptr;
    GDir *handle = g_dir_open(dir.c_str(), 0, &dir_error);
    if (!handle) {
        m_file_status = "Could not open cheat folder: ";
        m_file_status += dir_error ? dir_error->message : dir;
        if (dir_error) {
            g_error_free(dir_error);
        }
        return false;
    }

    const std::string short_id =
        Upper(CurrentGameManager::FormatDatabaseGameId(game.title_id));
    const std::string full_id = Upper(CurrentGameManager::FormatTitleId(game.title_id));
    std::string best_path;
    size_t best_score = 0;

    const gchar *name = nullptr;
    while ((name = g_dir_read_name(handle)) != nullptr) {
        std::string filename(name);
        if (filename.size() < 4 ||
            g_ascii_strcasecmp(filename.c_str() + filename.size() - 4,
                               ".txt") != 0) {
            continue;
        }

        gchar *full = g_build_filename(dir.c_str(), name, nullptr);
        if (!full) {
            continue;
        }
        gchar *contents = nullptr;
        gsize length = 0;
        if (!g_file_get_contents(full, &contents, &length, nullptr)) {
            g_free(full);
            continue;
        }
        std::string text(contents, length);
        g_free(contents);

        FileHeader header;
        ParseHeader(text, header);
        if (!header.title_id_valid || header.title_id != game.title_id ||
            !HashMatches(header.hash, game.header_sha256)) {
            g_free(full);
            continue;
        }

        size_t score = header.hash.size();
        std::string upper_name = Upper(filename);
        if (upper_name.rfind(short_id + "-", 0) == 0) {
            score += 256;
        } else if (upper_name.rfind(full_id + "-", 0) == 0) {
            score += 128;
        }
        if (best_path.empty() || score > best_score) {
            best_path = full;
            best_score = score;
        }
        g_free(full);
    }
    g_dir_close(handle);

    if (best_path.empty()) {
        DeactivateAllFHooks();
            for (auto &block : m_blocks) {
            block.enabled = false;
        }
        m_switches.clear();
        m_loaded_path.clear();
        m_blocks.clear();
        m_groups.clear();
        m_groups.push_back(CheatGroup{"", {}, {}});

        const std::string hash = game.header_sha256.size() >= 16
                                     ? game.header_sha256.substr(0, 16)
                                     : game.header_sha256;
        m_file_status = "No matching code file. Suggested filename: " +
                        short_id + "-" + Upper(hash) + ".txt";
        return false;
    }

    if (!force_reload && best_path == m_loaded_path) {
        return true;
    }
    return LoadSourceFile(best_path);
}

void CheatEngineWindow::MaybeAutoLoadCurrentGame()
{
    if (!m_auto_load_current_game) {
        return;
    }
    const uint64_t generation = current_game_manager.Generation();
    if (generation == m_seen_game_generation) {
        return;
    }

    /* CurrentGameManager has already observed a different XBE identity by the
     * time Tick() runs. Do not restore hook bytes captured from the previous
     * title into the new title's address space; simply forget those stale
     * snapshots and let the new file establish its own hooks. */
    m_f_hooks.clear();
    xemu_cheat_external_code_reset_allocations();
    m_seen_game_generation = generation;
    LoadMatchingCurrentGameFile(true);
}

void CheatEngineWindow::ParseSource(bool preserve_states)
{
    /* Keep deactivated Type-F hook metadata across a source reload so the
     * known hook/original-byte relationship remains associated with this XBE.
     * DeactivateAllFHooks() restores hooks and safely returns retired cave
     * blocks to the external arena whenever the CPU is no longer executing
     * them. Current-game changes still clear this table completely. */
    DeactivateAllFHooks();
    std::unordered_map<std::string, bool> selected_by_name;
    std::unordered_map<std::string, bool> enabled_by_name;
    if (preserve_states) {
        selected_by_name.reserve(m_blocks.size());
        enabled_by_name.reserve(m_blocks.size());
        for (const auto &block : m_blocks) {
            selected_by_name[block.name] = block.selected;
            enabled_by_name[block.name] = block.enabled;
        }
    }

    m_blocks.clear();
    m_groups.clear();
    m_groups.push_back(CheatGroup{"", {}, {}}); // invisible root
    m_parse_messages.clear();
    m_switches.clear();

    std::vector<int> group_stack{0};
    group_stack.reserve(8);
    CheatBlock *current = nullptr;
    std::istringstream stream(m_source);
    std::string line;
    int line_number = 0;

    /* Type-F executable and post-DEADCODE data lines intentionally share
     * the same normalization/comment rules through the helpers above. */

    auto new_block = [&](const std::string &spec) -> CheatBlock * {
        CheatBlock block;
        std::string label = Trim(spec);
        size_t open = label.find('{');
        size_t close = label.rfind('}');
        if (open != std::string::npos && close != std::string::npos &&
            close > open) {
            block.name = Trim(label.substr(0, open));
            block.description = Trim(label.substr(open + 1, close - open - 1));
        } else {
            block.name = label;
        }
        if (block.name.empty()) {
            block.name = "RAW Codes";
        }
        block.group_index = group_stack.back();
        if (preserve_states) {
            auto sit = selected_by_name.find(block.name);
            if (sit != selected_by_name.end()) {
                block.selected = sit->second;
            }
            auto eit = enabled_by_name.find(block.name);
            if (eit != enabled_by_name.end()) {
                block.enabled = eit->second;
            }
        }
        m_blocks.push_back(std::move(block));
        return &m_blocks.back();
    };

    while (std::getline(stream, line)) {
        ++line_number;
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == ';') {
            continue;
        }

        if (trimmed[0] == '^') {
            continue;
        }

        if (trimmed.rfind("!!", 0) == 0) {
            if (group_stack.size() > 1) {
                group_stack.pop_back();
            } else {
                m_parse_messages.emplace_back(
                    "Group close (!!) had no matching open group on line " +
                    std::to_string(line_number));
            }
            current = nullptr;
            continue;
        }

        if (trimmed[0] == '!') {
            std::string group_name = Trim(trimmed.substr(1));
            if (!group_name.empty() && group_name.back() == ':') {
                group_name.pop_back();
                group_name = Trim(group_name);
            }
            if (!group_name.empty()) {
                CheatGroup group;
                group.name = group_name;
                int index = (int)m_groups.size();
                m_groups.push_back(std::move(group));
                m_groups[(size_t)group_stack.back()].child_groups.push_back(index);
                group_stack.push_back(index);
            }
            current = nullptr;
            continue;
        }

        if (trimmed[0] == '+') {
            current = new_block(Trim(trimmed.substr(1)));
            continue;
        }

        if (trimmed[0] == '{') {
            if (current) {
                size_t close = trimmed.rfind('}');
                std::string desc = close != std::string::npos && close > 0
                                       ? trimmed.substr(1, close - 1)
                                       : trimmed.substr(1);
                if (!current->description.empty()) {
                    current->description += " ";
                }
                current->description += Trim(desc);
            }
            continue;
        }

        if (trimmed[0] == '%') {
            std::string upper = Upper(trimmed);
            const std::string prefix = "%CREDITS:";
            if (current && upper.rfind(prefix, 0) == 0) {
                current->credits = Trim(trimmed.substr(prefix.size()));
            }
            continue;
        }

        RawCode code;
        if (ParseCodeLine(line, code, line_number)) {
            if (!current) {
                current = new_block("RAW Codes");
            }

            /* Type-F source blocks are self-contained:
             *
             *   F0000000 AAAAAAAA   assembly source
             *   ...
             *   DEADCODE
             *
             *   F1000000 AAAAAAAA   aligned literal hex bytes
             *   XXXXXXXX YYYYYYYY
             *   ...
             *   DEADCODE 000000NN   NN=01..08 valid bytes in final line
             *
             * A leading '$' is optional on every Type-F body/directive line.
             * The body is captured as text instead of being fed through the
             * ordinary RAW-pair parser. DEADCODE is syntax, never literal
             * DE AD C0 DE bytes. */
            const uint32_t type = code.command >> 28;
            const uint32_t f_subtype = (code.command >> 24) & 0xFu;
            if (type == 0xF && (f_subtype == 0x0 || f_subtype == 0x1)) {
                bool terminated = false;
                std::string body_line;
                while (std::getline(stream, body_line)) {
                    ++line_number;
                    std::string normalized = NormalizeTypeFLine(body_line);
                    std::string directive = TypeFDirective(normalized);

                    std::istringstream directive_tokens(directive);
                    std::string directive_name;
                    directive_tokens >> directive_name;
                    if (Upper(directive_name) == "DEADCODE") {
                        terminated = true;
                        std::string arg;
                        std::string extra;
                        directive_tokens >> arg;
                        directive_tokens >> extra;

                        if (f_subtype == 0x0) {
                            if (!arg.empty() || !extra.empty()) {
                                m_parse_messages.emplace_back(
                                    "Type-F0 DEADCODE must not have an operand on line " +
                                    std::to_string(line_number) + ".");
                            }
                        } else {
                            if (!ParseDeadcodeCount(arg, code.f_final_valid_bytes) ||
                                !extra.empty()) {
                                code.f_final_valid_bytes = 0;
                                m_parse_messages.emplace_back(
                                    "Type-F1 must end with DEADCODE 000000NN where NN is 01-08 (line " +
                                    std::to_string(line_number) + ").");
                            }
                        }

                        /* F0 static DD data may be written after DEADCODE for
                         * readability. DEADCODE still marks the end of executable
                         * source; only label/DD declarations are absorbed here and
                         * the assembler physically places them after the generated
                         * return JMP. Rewind as soon as the next real cheat/source
                         * line is encountered so the outer parser sees it normally. */
                        if (f_subtype == 0x0) {
                            while (true) {
                                const std::streampos candidate_pos = stream.tellg();
                                const int before_candidate_line = line_number;
                                std::string data_line;
                                if (!std::getline(stream, data_line)) {
                                    break;
                                }
                                ++line_number;

                                std::string data_norm = NormalizeTypeFLine(data_line);
                                std::string data_directive =
                                    TypeFDirective(data_norm);

                                if (data_directive.empty()) {
                                    continue;
                                }

                                std::string after_label = data_directive;
                                const size_t data_colon = after_label.find(':');
                                bool label_only = false;
                                if (data_colon != std::string::npos) {
                                    std::string label_name =
                                        Trim(after_label.substr(0, data_colon));
                                    label_only = !label_name.empty() &&
                                        Trim(after_label.substr(data_colon + 1)).empty();
                                    after_label =
                                        Trim(after_label.substr(data_colon + 1));
                                }
                                std::istringstream data_tokens(after_label);
                                std::string data_name;
                                data_tokens >> data_name;
                                const bool is_dd = Upper(data_name) == "DD";

                                if (label_only || is_dd) {
                                    code.f_body.push_back(
                                        XemuCheatAsmLine{line_number, data_norm});
                                    continue;
                                }

                                if (candidate_pos != std::streampos(-1)) {
                                    stream.clear();
                                    stream.seekg(candidate_pos);
                                    line_number = before_candidate_line;
                                }
                                break;
                            }
                        }
                        break;
                    }

                    if (directive.empty()) {
                        continue;
                    }
                    code.f_body.push_back(XemuCheatAsmLine{line_number, normalized});
                }
                code.f_terminated = terminated;
                if (!terminated) {
                    m_parse_messages.emplace_back(
                        "Type-F block starting on line " +
                        std::to_string(code.source_line) +
                        " was not terminated with DEADCODE.");
                }
            }

            current->codes.push_back(std::move(code));
            continue;
        }

        bool looks_hex = !trimmed.empty() &&
                         std::isxdigit((unsigned char)trimmed[0]);
        if (looks_hex) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "Line %d was not a valid RAW pair: %s",
                          line_number, trimmed.c_str());
            m_parse_messages.emplace_back(msg);
        }
    }

    if (group_stack.size() > 1) {
        m_parse_messages.emplace_back(
            "One or more groups were not closed with !! before end of file.");
    }

    // Remove empty blocks, then rebuild each group's cheat index list.
    std::vector<CheatBlock> kept;
    kept.reserve(m_blocks.size());
    for (auto &block : m_blocks) {
        if (!block.codes.empty()) {
            kept.push_back(std::move(block));
        }
    }
    m_blocks = std::move(kept);
    for (auto &group : m_groups) {
        group.cheats.clear();
    }
    for (size_t i = 0; i < m_blocks.size(); ++i) {
        int group = m_blocks[i].group_index;
        if (group < 0 || (size_t)group >= m_groups.size()) {
            group = 0;
            m_blocks[i].group_index = 0;
        }
        m_groups[(size_t)group].cheats.push_back(i);
    }

    if (m_blocks.empty()) {
        m_parse_messages.emplace_back("No RAW code lines were found.");
    }
}

bool CheatEngineWindow::ReadGuest(GuestAddressSpace space, uint32_t address,
                                  void *buffer, size_t size)
{
    return xemu_cheat_memory_read(space == GuestAddressSpace::Virtual,
                                  address, buffer, size) != 0;
}

bool CheatEngineWindow::WriteGuest(GuestAddressSpace space, uint32_t address,
                                   const void *buffer, size_t size)
{
    return xemu_cheat_memory_write(space == GuestAddressSpace::Virtual,
                                   address, buffer, size) != 0;
}

bool CheatEngineWindow::ReadValue(GuestAddressSpace space, uint32_t address,
                                  size_t size, uint32_t &value)
{
    uint8_t bytes[4] = {};
    if (size == 0 || size > sizeof(bytes) ||
        !ReadGuest(space, address, bytes, size)) {
        return false;
    }

    value = bytes[0];
    if (size >= 2) {
        value |= (uint32_t)bytes[1] << 8;
    }
    if (size >= 3) {
        value |= (uint32_t)bytes[2] << 16;
    }
    if (size >= 4) {
        value |= (uint32_t)bytes[3] << 24;
    }
    return true;
}

bool CheatEngineWindow::WriteValue(GuestAddressSpace space, uint32_t address,
                                   size_t size, uint32_t value)
{
    uint8_t bytes[4] = {
        (uint8_t)(value & 0xFF),
        (uint8_t)((value >> 8) & 0xFF),
        (uint8_t)((value >> 16) & 0xFF),
        (uint8_t)((value >> 24) & 0xFF),
    };
    return size > 0 && size <= sizeof(bytes) &&
           WriteGuest(space, address, bytes, size);
}

bool CheatEngineWindow::DetermineFHookLength(uint32_t hook_address,
                                                 uint32_t &overwrite_length)
{
    XemuCheatDisasmRow rows[16] = {};
    size_t row_count = 0;
    overwrite_length = 0;

    if (!xemu_cheat_disassembler_available()) {
        m_last_runtime_message =
            "Type-F automatic hook sizing requires the Capstone x86 decoder.";
        return false;
    }

    const int rc = xemu_cheat_disassemble_paired(hook_address,
                                                  (int)(sizeof(rows) / sizeof(rows[0])),
                                                  rows,
                                                  sizeof(rows) / sizeof(rows[0]),
                                                  &row_count);
    if (rc != XEMU_CHEAT_DISAS_OK || row_count == 0) {
        m_last_runtime_message =
            "Type-F could not disassemble the virtual hook address 0x";
        char address[16];
        std::snprintf(address, sizeof(address), "%08X", hook_address);
        m_last_runtime_message += address;
        return false;
    }

    for (size_t n = 0; n < row_count && overwrite_length < 5u; ++n) {
        if (rows[n].size == 0 || rows[n].size > 15u) {
            break;
        }
        overwrite_length += rows[n].size;
        if (overwrite_length > 32u) {
            break;
        }
    }

    if (overwrite_length < 5u || overwrite_length > 32u) {
        m_last_runtime_message =
            "Type-F could not find a complete 5-32 byte instruction span for the hook.";
        return false;
    }
    return true;
}

bool CheatEngineWindow::ParseFRawHex(const RawCode &code,
                                     std::vector<uint8_t> &bytes,
                                     std::string &error,
                                     int &error_line)
{
    bytes.clear();
    error.clear();
    error_line = code.source_line;

    if (code.f_final_valid_bytes < 1u || code.f_final_valid_bytes > 8u) {
        error = "Type-F1 requires DEADCODE 000000NN with NN from 01 through 08";
        return false;
    }
    if (code.f_body.empty()) {
        error = "Type-F1 raw cave contains no 32-bit data pairs";
        return false;
    }
    bytes.reserve(std::min<size_t>(code.f_body.size() * 8u, 0x10000u));

    auto decode_word = [](const std::string &token, uint8_t out[4]) -> bool {
        if (token.size() != 8) {
            return false;
        }
        auto nibble = [](char c) -> int {
            unsigned char u = (unsigned char)c;
            if (u >= '0' && u <= '9') return u - '0';
            u = (unsigned char)std::toupper(u);
            if (u >= 'A' && u <= 'F') return u - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < 4; ++i) {
            const int hi = nibble(token[i * 2]);
            const int lo = nibble(token[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            out[i] = (uint8_t)((hi << 4) | lo);
        }
        return true;
    };

    for (size_t line_index = 0; line_index < code.f_body.size(); ++line_index) {
        const XemuCheatAsmLine &src = code.f_body[line_index];
        std::string line = Trim(src.text);
        if (!line.empty() && line[0] == '$') {
            line = Trim(line.substr(1));
        }

        size_t cut = line.size();
        size_t comment = line.find(';');
        if (comment != std::string::npos) cut = std::min(cut, comment);
        comment = line.find("//");
        if (comment != std::string::npos) cut = std::min(cut, comment);
        comment = line.find('#');
        if (comment != std::string::npos) cut = std::min(cut, comment);
        line.erase(cut);
        line = Trim(line);

        std::istringstream tokens(line);
        std::string left;
        std::string right;
        std::string extra;
        tokens >> left >> right >> extra;
        if (left.empty() || right.empty() || !extra.empty()) {
            error = "Type-F1 data lines must be exactly XXXXXXXX YYYYYYYY";
            error_line = src.source_line;
            return false;
        }

        uint8_t pair[8];
        if (!decode_word(left, &pair[0]) || !decode_word(right, &pair[4])) {
            error = "Type-F1 data lines require two 8-digit hexadecimal words";
            error_line = src.source_line;
            return false;
        }

        const bool final_line = line_index + 1u == code.f_body.size();
        const size_t valid = final_line ? code.f_final_valid_bytes : 8u;
        if (final_line && valid < 8u) {
            for (size_t i = valid; i < 8u; ++i) {
                if (pair[i] != 0) {
                    error = "Type-F1 bytes after the DEADCODE valid-byte count must be zero padding";
                    error_line = src.source_line;
                    return false;
                }
            }
        }

        bytes.insert(bytes.end(), pair, pair + valid);
        if (bytes.size() > 0x10000u) {
            error = "Type-F1 raw cave exceeds 64 KiB";
            error_line = src.source_line;
            return false;
        }
    }

    if (bytes.empty()) {
        error = "Type-F1 raw cave contains no executable bytes";
        return false;
    }
    return true;
}

bool CheatEngineWindow::InstallFHook(
    size_t owner_block, uint64_t key, uint32_t hook_address,
    const std::vector<uint8_t> &probe_code,
    const std::vector<uint8_t> &probe_data,
    const std::vector<XemuCheatAsmLine> *f0_source,
    bool f0_uses_preserve, uint32_t preserve_bytes,
    bool f0_uses_temp, uint32_t temp_bytes,
    const std::string &definition_signature)
{
    if (probe_code.empty() ||
        probe_code.size() + 5u + probe_data.size() > 0x10000u) {
        m_last_runtime_message =
            "Type-F hook install: assembled/raw cave is empty or exceeds 64 KiB.";
        return false;
    }
    if (f0_uses_preserve && (f0_source == nullptr || preserve_bytes == 0)) {
        m_last_runtime_message =
            "Type-F0 hook install: PRESERVE metadata was incomplete.";
        return false;
    }
    if (f0_uses_temp && (f0_source == nullptr || temp_bytes != 40u)) {
        m_last_runtime_message =
            "Type-F0 hook install: T0-T7/TFLAGS metadata was incomplete.";
        return false;
    }

    auto it = m_f_hooks.find(key);

    /* A source-position key can change when a user edits/reorders a cheat.
     * If another inactive state already owns the same guest hook address,
     * adopt its known-good hook/original-byte metadata. */
    if (it == m_f_hooks.end()) {
        for (auto other = m_f_hooks.begin(); other != m_f_hooks.end(); ++other) {
            if (other->first == key || other->second.hook_address != hook_address) {
                continue;
            }
            if (other->second.installed) {
                m_last_runtime_message =
                    "Type-F hook install: another active Type-F cheat already owns hook 0x" +
                    TypeFHex32(hook_address) + ". Disable it first.";
                return false;
            }

            FHookState adopted = std::move(other->second);
            m_f_hooks.erase(other);
            adopted.owner_block = owner_block;
            auto inserted = m_f_hooks.emplace(key, std::move(adopted));
            it = inserted.first;
            break;
        }
    }

    /* Normal 10 Hz ticks take this path. The source/RAW signature is stable
     * even when F0 contains absolute DD/preservation addresses, so we never
     * rewrite an already-active cave merely because its final bytes depend on
     * the allocator-selected address. */
    if (it != m_f_hooks.end() && it->second.installed &&
        it->second.hook_address == hook_address &&
        it->second.definition_signature == definition_signature) {
        it->second.owner_block = owner_block;
        return true;
    }

    TypeFGuestPauseGuard guest_pause;

    if (it != m_f_hooks.end() && it->second.hook_address != hook_address) {
        DeactivateFHook(key);
        if (it->second.installed) {
            m_last_runtime_message =
                "Type-F hook install: failed to restore the previous hook before moving it.";
            return false;
        }
        m_f_hooks.erase(it);
        it = m_f_hooks.end();
    }

    if (it == m_f_hooks.end()) {
        FHookState state;
        state.owner_block = owner_block;
        state.hook_address = hook_address;
        auto inserted = m_f_hooks.emplace(key, std::move(state));
        it = inserted.first;
    }

    FHookState &state = it->second;
    state.owner_block = owner_block;

    if (state.installed && state.definition_signature != definition_signature) {
        DeactivateFHook(key);
        if (state.installed) {
            m_last_runtime_message =
                "Type-F hook install: failed to restore the active hook before updating cave code.";
            return false;
        }
    }

    /* Never overwrite a retired cave or its preservation frames while EIP is
     * still inside the old cave or one of its CALLs has a pending return. */
    if (!state.installed &&
        (state.external_entry != 0 || state.preserve_entry != 0 ||
         state.temp_entry != 0) &&
        !ReleaseFHookCaveIfSafe(state)) {
        m_last_runtime_message =
            "Type-F hook install: previous cave is still executing or could not be safely reclaimed; retrying on the next tick.";
        return false;
    }

    uint32_t overwrite_length = 0;
    if (!DetermineFHookLength(hook_address, overwrite_length)) {
        if (m_last_runtime_message.empty()) {
            m_last_runtime_message =
                "Type-F hook install: failed while determining the hook instruction span.";
        }
        return false;
    }
    state.overwrite_length = overwrite_length;
    state.original_bytes.resize(overwrite_length);
    if (!ReadGuest(GuestAddressSpace::Virtual, hook_address,
                   state.original_bytes.data(), state.original_bytes.size())) {
        m_last_runtime_message =
            "Type-F hook install: failed to read/save original hook bytes at 0x" +
            TypeFHex32(hook_address) + ".";
        return false;
    }

    const uint32_t required_size =
        (uint32_t)probe_code.size() + 5u + (uint32_t)probe_data.size();
    uint32_t external_entry = 0;
    if (!xemu_cheat_external_code_allocate(required_size, &external_entry)) {
        const char *detail = xemu_cheat_external_code_last_error();
        m_last_runtime_message =
            "Type-F hook install: could not allocate external executable/DD memory";
        if (detail != nullptr && detail[0] != '\0') {
            m_last_runtime_message += ": ";
            m_last_runtime_message += detail;
        }
        m_last_runtime_message += ".";
        return false;
    }
    state.external_entry = external_entry;
    state.allocation_size = (required_size + 0x0Fu) & ~0x0Fu;
    state.retired_may_be_referenced = false;

    if (f0_uses_preserve) {
        uint32_t preserve_entry = 0;
        if (!xemu_cheat_external_preserve_allocate(preserve_bytes,
                                                   &preserve_entry)) {
            m_last_runtime_message =
                "Type-F0 hook install: could not allocate private preservation frames.";
            state.retired_may_be_referenced = false;
            ReleaseFHookCaveIfSafe(state);
            return false;
        }
        state.preserve_entry = preserve_entry;
        state.preserve_size = (preserve_bytes + 0x0Fu) & ~0x0Fu;
    }

    if (f0_uses_temp) {
        uint32_t temp_entry = 0;
        if (!xemu_cheat_external_preserve_allocate(temp_bytes, &temp_entry)) {
            m_last_runtime_message =
                "Type-F0 hook install: could not allocate private T0-T7/TFLAGS bank.";
            state.retired_may_be_referenced = false;
            ReleaseFHookCaveIfSafe(state);
            return false;
        }
        state.temp_entry = temp_entry;
        state.temp_size = (temp_bytes + 0x0Fu) & ~0x0Fu;
    }

    const std::vector<uint8_t> *final_code = &probe_code;
    const std::vector<uint8_t> *final_data = &probe_data;
    XemuCheatAsmResult final_asm;
    if (f0_source != nullptr) {
        if (!xemu_cheat_assemble_x86_32_at(*f0_source,
                                           state.external_entry,
                                           state.preserve_entry,
                                           state.temp_entry,
                                           final_asm)) {
            m_last_runtime_message = "Type-F0 final assembler error";
            if (final_asm.error_line > 0) {
                m_last_runtime_message += " on source line " +
                    std::to_string(final_asm.error_line);
            }
            m_last_runtime_message += ": " + final_asm.error;
            state.retired_may_be_referenced = false;
            ReleaseFHookCaveIfSafe(state);
            return false;
        }
        if (final_asm.bytes.size() != probe_code.size() ||
            final_asm.data.size() != probe_data.size() ||
            final_asm.uses_preserve != f0_uses_preserve ||
            final_asm.uses_temp != f0_uses_temp) {
            m_last_runtime_message =
                "Type-F0 internal error: final assembly changed the probed cave layout.";
            state.retired_may_be_referenced = false;
            ReleaseFHookCaveIfSafe(state);
            return false;
        }
        final_code = &final_asm.bytes;
        final_data = &final_asm.data;
    }

    state.code_size = (uint32_t)final_code->size();
    state.definition_signature = definition_signature;

    /* Build one contiguous cave payload while the guest is paused:
     *   executable code | generated DEADCODE JMP | attached DD data
     * This guarantees that a label such as CarList points to storage owned by
     * the same allocation and that no neighboring cave can overlap it. */
    uint8_t return_jump[5];
    return_jump[0] = 0xE9;
    const uint32_t return_from = state.external_entry + state.code_size;
    const uint32_t return_to = hook_address + state.overwrite_length;
    const uint32_t return_rel = return_to - (return_from + 5u);
    return_jump[1] = (uint8_t)(return_rel & 0xFFu);
    return_jump[2] = (uint8_t)((return_rel >> 8) & 0xFFu);
    return_jump[3] = (uint8_t)((return_rel >> 16) & 0xFFu);
    return_jump[4] = (uint8_t)((return_rel >> 24) & 0xFFu);

    std::vector<uint8_t> payload;
    payload.reserve(final_code->size() + sizeof(return_jump) + final_data->size());
    payload.insert(payload.end(), final_code->begin(), final_code->end());
    payload.insert(payload.end(), return_jump, return_jump + sizeof(return_jump));
    payload.insert(payload.end(), final_data->begin(), final_data->end());
    if (!xemu_cheat_external_code_write(state.external_entry, 0,
                                         payload.data(), payload.size())) {
        m_last_runtime_message =
            "Type-F hook install: failed to write code/DEADCODE/DD payload to external memory.";
        state.retired_may_be_referenced = false;
        ReleaseFHookCaveIfSafe(state);
        return false;
    }

    uint8_t hook[32];
    memset(hook, 0x90, state.overwrite_length);
    hook[0] = 0xE9;
    const uint32_t hook_rel = state.external_entry - (hook_address + 5u);
    hook[1] = (uint8_t)(hook_rel & 0xFFu);
    hook[2] = (uint8_t)((hook_rel >> 8) & 0xFFu);
    hook[3] = (uint8_t)((hook_rel >> 16) & 0xFFu);
    hook[4] = (uint8_t)((hook_rel >> 24) & 0xFFu);

    state.installed = true;
    if (!WriteGuest(GuestAddressSpace::Virtual, hook_address,
                    hook, state.overwrite_length)) {
        const std::string write_error =
            "Type-F hook install: failed to write the guest hook JMP at 0x" +
            TypeFHex32(hook_address) + ".";
        DeactivateFHook(key);
        if (state.installed) {
            m_last_runtime_message = write_error +
                " Automatic rollback also failed; the cave was retained.";
        } else {
            m_last_runtime_message = write_error +
                " Original hook bytes were restored.";
        }
        return false;
    }

    state.retired_may_be_referenced = false;
    return true;
}

bool CheatEngineWindow::FHookCaveMayStillBeReferenced(
    const FHookState &state, const XemuCheatX86Registers &regs)
{
    if (state.external_entry == 0 || state.allocation_size == 0) {
        return false;
    }

    const uint64_t cave_start = state.external_entry;
    const uint64_t cave_end = cave_start + state.allocation_size;
    if ((uint64_t)regs.pc >= cave_start && (uint64_t)regs.pc < cave_end) {
        return true;
    }

    /* If a cave executed CALL and the callee is still running, EIP can already
     * be outside the cave while a return address still points back into it.
     * Decode only this cave's user-code span and collect the exact return
     * addresses of real CALL instructions. This is much less prone to false
     * positives than treating any stack value in the 1 MiB arena as a return. */
    std::vector<uint32_t> call_returns;
    const uint64_t code_end = cave_start + state.code_size;
    uint32_t decode_pc = state.external_entry;
    while ((uint64_t)decode_pc < code_end) {
        XemuCheatDisasmRow rows[128] = {};
        size_t row_count = 0;
        const int rc = xemu_cheat_disassemble_paired(
            decode_pc, (int)(sizeof(rows) / sizeof(rows[0])), rows,
            sizeof(rows) / sizeof(rows[0]), &row_count);
        if (rc != XEMU_CHEAT_DISAS_OK || row_count == 0) {
            /* We cannot prove that no CALL return is pending, so leave this
             * cave retired rather than risk clearing live return code. */
            return true;
        }

        uint32_t next_pc = decode_pc;
        for (size_t i = 0; i < row_count; ++i) {
            const XemuCheatDisasmRow &row = rows[i];
            if ((uint64_t)row.virtual_address >= code_end) {
                break;
            }
            if (row.size == 0) {
                return true;
            }

            const uint64_t after =
                (uint64_t)row.virtual_address + (uint64_t)row.size;
            if (after > code_end) {
                /* The final instruction should never straddle DEADCODE. */
                return true;
            }

            if (g_ascii_strcasecmp(row.mnemonic, "call") == 0) {
                call_returns.push_back((uint32_t)after);
            }
            next_pc = (uint32_t)after;
        }

        if ((uint64_t)next_pc >= code_end) {
            break;
        }
        if (next_pc <= decode_pc) {
            return true;
        }
        decode_pc = next_pc;
    }

    if (call_returns.empty()) {
        return false;
    }
    std::sort(call_returns.begin(), call_returns.end());

    /* Only caves containing CALL need a stack scan. Search the next 64 KiB
     * from ESP for one of the exact CALL return addresses collected above.
     * Stop at the first unreadable range (normally the mapped stack boundary).
     * A false positive can only delay reclamation; active caves never move. */
    constexpr size_t kStackScanBytes = 0x10000;
    constexpr size_t kStackChunk = 0x1000;
    uint8_t buffer[kStackChunk];
    size_t scanned = 0;
    uint32_t address = regs.esp;

    while (scanned < kStackScanBytes) {
        const size_t page_remaining =
            0x1000u - (size_t)(address & 0x0FFFu);
        const size_t amount = std::min(
            std::min(kStackChunk, kStackScanBytes - scanned), page_remaining);
        if ((uint64_t)address + amount > 0x100000000ull) {
            break;
        }
        if (!ReadGuest(GuestAddressSpace::Virtual, address, buffer, amount)) {
            /* ESP itself should be readable. If the first stack range cannot
             * be inspected, be conservative and keep the cave retired. Once
             * at least one contiguous range was read, an unreadable next page
             * is treated as the mapped stack boundary. */
            if (scanned == 0) {
                return true;
            }
            break;
        }

        for (size_t off = 0; off + 4 <= amount; ++off) {
            const uint32_t candidate =
                (uint32_t)buffer[off] |
                ((uint32_t)buffer[off + 1] << 8) |
                ((uint32_t)buffer[off + 2] << 16) |
                ((uint32_t)buffer[off + 3] << 24);
            if (std::binary_search(call_returns.begin(), call_returns.end(),
                                   candidate)) {
                return true;
            }
        }

        const uint64_t next_address = (uint64_t)address + amount;
        scanned += amount;
        if (next_address >= 0x100000000ull) {
            break;
        }
        address = (uint32_t)next_address;
    }

    return false;
}

bool CheatEngineWindow::FHookHasTrackedEntries(const FHookState &state)
{
    return state.external_entry != 0 || state.preserve_entry != 0 ||
           state.temp_entry != 0;
}

bool CheatEngineWindow::FHookHasResources(const FHookState &state)
{
    return (state.external_entry != 0 && state.allocation_size != 0) ||
           (state.preserve_entry != 0 && state.preserve_size != 0) ||
           (state.temp_entry != 0 && state.temp_size != 0);
}

void CheatEngineWindow::ClearReleasedFHookState(FHookState &state)
{
    state.external_entry = 0;
    state.allocation_size = 0;
    state.code_size = 0;
    state.preserve_entry = 0;
    state.preserve_size = 0;
    state.temp_entry = 0;
    state.temp_size = 0;
    state.definition_signature.clear();
    state.retired_may_be_referenced = false;
}

bool CheatEngineWindow::ReleaseFHookCaveIfSafe(FHookState &state)
{
    const bool have_cave = state.external_entry != 0 && state.allocation_size != 0;
    const bool have_preserve = state.preserve_entry != 0 && state.preserve_size != 0;
    const bool have_temp = state.temp_entry != 0 && state.temp_size != 0;
    if (!have_cave && !have_preserve && !have_temp) {
        ClearReleasedFHookState(state);
        return true;
    }

    const bool was_running = runstate_is_running();
    if (was_running) {
        vm_stop(RUN_STATE_PAUSED);
    }

    bool safe_to_release = true;
    if (have_cave && state.retired_may_be_referenced) {
        XemuCheatX86Registers regs = {};
        const bool have_regs = xemu_cheat_get_x86_registers(&regs) != 0;
        safe_to_release = have_regs && !FHookCaveMayStillBeReferenced(state, regs);
    }

    bool released = false;
    if (safe_to_release) {
        bool code_ok = true;
        bool preserve_ok = true;
        bool temp_ok = true;
        if (have_cave) {
            code_ok = xemu_cheat_external_code_free(
                          state.external_entry, state.allocation_size) != 0;
            if (code_ok) {
                state.external_entry = 0;
                state.allocation_size = 0;
            }
        }
        /* The preservation block is reachable only from its owning cave. Once
         * that cave is unreachable/freed, zero and return the private frames. */
        if (code_ok && have_preserve) {
            preserve_ok = xemu_cheat_external_preserve_free(
                              state.preserve_entry, state.preserve_size) != 0;
            if (preserve_ok) {
                state.preserve_entry = 0;
                state.preserve_size = 0;
            }
        }
        /* T0-T7 persist while the F0 remains active. Once its cave is no
         * longer reachable, zero/free the one private T-register/TFLAGS bank. */
        if (code_ok && preserve_ok && have_temp) {
            temp_ok = xemu_cheat_external_preserve_free(
                          state.temp_entry, state.temp_size) != 0;
            if (temp_ok) {
                state.temp_entry = 0;
                state.temp_size = 0;
            }
        }
        released = code_ok && preserve_ok && temp_ok;
        if (released) {
            ClearReleasedFHookState(state);
        }
    }

    if (was_running) {
        vm_start();
    }
    return released;
}

void CheatEngineWindow::DeactivateFHook(uint64_t key)
{
    auto it = m_f_hooks.find(key);
    if (it == m_f_hooks.end()) {
        return;
    }

    FHookState &state = it->second;
    if (!state.installed && !FHookHasTrackedEntries(state)) {
        return;
    }

    const bool was_running = runstate_is_running();
    if (was_running) {
        vm_stop(RUN_STATE_PAUSED);
    }

    if (state.installed) {
        if (state.original_bytes.empty() ||
            !WriteGuest(GuestAddressSpace::Virtual, state.hook_address,
                        state.original_bytes.data(), state.original_bytes.size())) {
            if (was_running) {
                vm_start();
            }
            return;
        }
        state.installed = false;
        /* The hook really was reachable before restoration. EIP may still be
         * inside the cave, or a CALL may have a pending return into it. */
        state.retired_may_be_referenced = true;
    }

    /* Once the original hook is restored, reclaim only this cave. Active
     * neighboring caves are never moved. A genuinely retired cave is kept if
     * EIP is inside it or a live CALL return still points back into it. A cave
     * from a failed pre-hook install is known unreachable and skips that scan. */
    if (FHookHasResources(state)) {
        ReleaseFHookCaveIfSafe(state);
    }

    if (was_running) {
        vm_start();
    }
}

void CheatEngineWindow::DeactivateFHooksForBlock(size_t owner_block)
{
    bool have_block_cave = false;
    for (const auto &entry : m_f_hooks) {
        if (entry.second.owner_block == owner_block &&
            (entry.second.installed || FHookHasTrackedEntries(entry.second))) {
            have_block_cave = true;
            break;
        }
    }
    if (!have_block_cave) {
        return;
    }

    TypeFGuestPauseGuard guest_pause;
    for (auto &entry : m_f_hooks) {
        if (entry.second.owner_block == owner_block) {
            DeactivateFHook(entry.first);
        }
    }
}

void CheatEngineWindow::DeactivateAllFHooks()
{
    bool have_cave = false;
    for (const auto &entry : m_f_hooks) {
        if (entry.second.installed || FHookHasTrackedEntries(entry.second)) {
            have_cave = true;
            break;
        }
    }
    if (!have_cave) {
        return;
    }

    TypeFGuestPauseGuard guest_pause;
    for (auto &entry : m_f_hooks) {
        DeactivateFHook(entry.first);
    }
}

uint32_t CheatEngineWindow::Type9Count(const RawCode &code)
{
    return code.command & 0x00FFFFFFu;
}

size_t CheatEngineWindow::LogicalSpan(const std::vector<RawCode> &codes,
                                      size_t index) const
{
    if (index >= codes.size()) {
        return 0;
    }

    const RawCode &code = codes[index];
    uint32_t type = code.command >> 28;

    if (type == 0x9) {
        uint32_t count = Type9Count(code);
        if (count == 0) {
            return codes.size() - index;
        }
        size_t available = codes.size() - index - 1;
        return 1 + std::min<size_t>(count, available);
    }

    // D/E own their guarded raw lines for experimental logical skipping.
    if (type == 0xD || type == 0xE) {
        uint32_t count = (code.value >> 24) & 0xFF;
        size_t available = codes.size() - index - 1;
        return 1 + std::min<size_t>(count, available);
    }

    if (type == 0x3) {
        uint32_t subtype = (code.command >> 20) & 0xF;
        return (subtype == 0x4 || subtype == 0x5)
                   ? std::min<size_t>(2, codes.size() - index)
                   : 1;
    }

    if (type == 0x4 || type == 0x5) {
        // Types 4/5 own one continuation RAW line.
        return std::min<size_t>(2, codes.size() - index);
    }

    if (type == 0xA) {
        // xemu type A owns enough continuation RAW lines to supply Z bytes.
        // Each continuation contains exactly eight literal bytes.
        const uint32_t byte_count = code.value;
        if (byte_count == 0) {
            return 1;
        }
        const size_t data_lines = (size_t)(byte_count / 8u) +
                                  (byte_count % 8u != 0 ? 1u : 0u);
        const size_t span = 1u + data_lines;
        return std::min(span, codes.size() - index);
    }

    if (type == 0xF) {
        /* F0/F1 bodies are captured inside the header RawCode through their
         * DEADCODE directive, so the complete cave is one logical command. */
        return 1;
    }

    if (type == 0x6) {
        if (index + 1 >= codes.size()) {
            return 1;
        }
        uint32_t offsets = codes[index + 1].command & 0xFFFFu;
        if (offsets == 0) {
            offsets = 1;
        }
        // Header + descriptor/first-offset line + two offsets per extra line.
        size_t span = 2 + (size_t)(offsets / 2);
        return std::min(span, codes.size() - index);
    }

    // 0/1/2/7 are one-line commands.
    return 1;
}

size_t CheatEngineWindow::SkipLogicalCommands(const std::vector<RawCode> &codes,
                                              size_t start,
                                              uint32_t count) const
{
    size_t pos = start;
    for (uint32_t n = 0; n < count && pos < codes.size(); ++n) {
        size_t span = LogicalSpan(codes, pos);
        pos += std::max<size_t>(span, 1);
    }
    return std::min(pos, codes.size());
}

bool CheatEngineWindow::ExecuteBasicWrite(const RawCode &code,
                                         GuestAddressSpace active_space,
                                         uint32_t active_base)
{
    const uint32_t type = code.command >> 28;
    const size_t size = type == 0 ? 1 : (type == 1 ? 2 : 4);
    const uint32_t address = active_base + (code.command & 0x0FFFFFFFu);
    return WriteValue(active_space, address, size, code.value);
}

bool CheatEngineWindow::ExecuteArithmetic(const CheatBlock &block, size_t index,
                                          GuestAddressSpace active_space,
                                          uint32_t active_base,
                                          size_t &next_index)
{
    const RawCode &code = block.codes[index];
    // CodeBreaker-style arithmetic:
    //   30T0VVVV AAAAAAAA
    // T=0/1 byte +/-; T=2/3 halfword +/-; T=4/5 word +/-
    // with the 32-bit amount in word 1 of the following raw line.
    const uint32_t subtype = (code.command >> 20) & 0xF;
    const uint32_t address = active_base + (code.value & 0x0FFFFFFFu);
    size_t size = 0;
    bool subtract = false;
    uint32_t amount = 0;

    switch (subtype) {
    case 0x0:
        size = 1;
        amount = code.command & 0xFFu;
        break;
    case 0x1:
        size = 1;
        subtract = true;
        amount = code.command & 0xFFu;
        break;
    case 0x2:
        size = 2;
        amount = code.command & 0xFFFFu;
        break;
    case 0x3:
        size = 2;
        subtract = true;
        amount = code.command & 0xFFFFu;
        break;
    case 0x4:
    case 0x5:
        if (index + 1 >= block.codes.size()) {
            m_last_runtime_message =
                "Truncated 32-bit type-3 code on source line " +
                std::to_string(code.source_line);
            return false;
        }
        size = 4;
        subtract = subtype == 0x5;
        amount = block.codes[index + 1].command;
        next_index = index + 2;
        break;
    default:
        m_last_runtime_message =
            "Unsupported type-3 subtype on source line " +
            std::to_string(code.source_line);
        return false;
    }

    uint32_t current = 0;
    if (!ReadValue(active_space, address, size, current)) {
        return false;
    }
    const uint32_t mask = size == 1 ? 0xFFu :
                          (size == 2 ? 0xFFFFu : 0xFFFFFFFFu);
    const uint32_t result = subtract ? current - amount : current + amount;
    return WriteValue(active_space, address, size, result & mask);
}

bool CheatEngineWindow::ExecuteSerial(const CheatBlock &block, size_t index,
                                      GuestAddressSpace active_space,
                                      uint32_t active_base,
                                      size_t &next_index)
{
    const RawCode &code = block.codes[index];
    // 4AAAAAAA NNNNSSSS
    // VVVVVVVV IIIIIIII
    if (index + 1 >= block.codes.size()) {
        m_last_runtime_message =
            "Truncated type-4 serial code on source line " +
            std::to_string(code.source_line);
        return false;
    }

    const uint32_t count = (code.value >> 16) & 0xFFFFu;
    const uint32_t step_words = code.value & 0xFFFFu;
    uint32_t address = active_base + (code.command & 0x0FFFFFFFu);
    uint32_t serial_value = block.codes[index + 1].command;
    const uint32_t increment = block.codes[index + 1].value;
    next_index = index + 2;

    for (uint32_t n = 0; n < count; ++n) {
        if (!WriteValue(active_space, address, 4, serial_value)) {
            return false;
        }
        address += step_words * 4u;
        serial_value += increment;
    }
    return true;
}

bool CheatEngineWindow::ExecuteCopy(const CheatBlock &block, size_t index,
                                    GuestAddressSpace active_space,
                                    uint32_t active_base,
                                    size_t &next_index)
{
    const RawCode &code = block.codes[index];
    // 5AAAAAAA NNNNNNNN
    // DDDDDDDD ????????
    // Both source and destination use the active type-9 context.
    if (index + 1 >= block.codes.size()) {
        m_last_runtime_message =
            "Truncated type-5 copy code on source line " +
            std::to_string(code.source_line);
        return false;
    }

    if (code.value > 16u * 1024u * 1024u) {
        m_last_runtime_message =
            "Type-5 copy exceeds 16 MiB safety limit on source line " +
            std::to_string(code.source_line);
        next_index = index + 2;
        return false;
    }

    const uint32_t src = active_base + (code.command & 0x0FFFFFFFu);
    const uint32_t dst = active_base +
                         (block.codes[index + 1].command & 0x0FFFFFFFu);
    const size_t length = (size_t)code.value;
    std::vector<uint8_t> data(length);
    next_index = index + 2;
    if (length == 0) {
        return true;
    }
    return ReadGuest(active_space, src, data.data(), length) &&
           WriteGuest(active_space, dst, data.data(), length);
}

bool CheatEngineWindow::ExecutePointer(const CheatBlock &block, size_t index,
                                       GuestAddressSpace active_space,
                                       uint32_t active_base,
                                       size_t &next_index)
{
    const RawCode &code = block.codes[index];
    // 6AAAAAAA VVVVVVVV
    // 000TNNNN OOOOOOOO
    // OOOOOOOO OOOOOOOO ...
    // Type-9 selects physical/virtual addressing for the base and every
    // intermediate pointer. Full 32-bit Xbox pointers are kept.
    if (index + 1 >= block.codes.size()) {
        m_last_runtime_message =
            "Truncated type-6 pointer code on source line " +
            std::to_string(code.source_line);
        return false;
    }

    const RawCode &desc = block.codes[index + 1];
    const uint32_t write_type = (desc.command >> 16) & 0xFu;
    uint32_t offset_count = desc.command & 0xFFFFu;
    if (offset_count == 0) {
        offset_count = 1;
    }
    const size_t total_lines = 2 + (size_t)(offset_count / 2);
    next_index = std::min(block.codes.size(), index + total_lines);

    if (write_type > 2) {
        m_last_runtime_message =
            "Unsupported type-6 write size on source line " +
            std::to_string(code.source_line);
        return false;
    }
    if (offset_count > 256) {
        m_last_runtime_message =
            "Type-6 pointer has more than 256 offsets on source line " +
            std::to_string(code.source_line);
        return false;
    }
    if (index + total_lines > block.codes.size()) {
        m_last_runtime_message =
            "Truncated type-6 pointer offsets on source line " +
            std::to_string(code.source_line);
        return false;
    }

    std::vector<uint32_t> offsets;
    offsets.reserve(offset_count);
    offsets.push_back(desc.value);
    size_t p = index + 2;
    while (offsets.size() < offset_count) {
        offsets.push_back(block.codes[p].command);
        if (offsets.size() < offset_count) {
            offsets.push_back(block.codes[p].value);
        }
        ++p;
    }

    uint32_t ptr = 0;
    const uint32_t base_address = active_base +
                                  (code.command & 0x0FFFFFFFu);
    if (!ReadValue(active_space, base_address, 4, ptr)) {
        return false;
    }

    uint32_t target = 0;
    for (size_t n = 0; n < offsets.size(); ++n) {
        if (ptr == 0) {
            return false;
        }
        target = ptr + offsets[n];
        if (n + 1 < offsets.size() &&
            !ReadValue(active_space, target, 4, ptr)) {
            return false;
        }
    }

    const size_t size = write_type == 0 ? 1 :
                        (write_type == 1 ? 2 : 4);
    return WriteValue(active_space, target, size, code.value);
}

bool CheatEngineWindow::ExecuteBitwise(const RawCode &code,
                                       GuestAddressSpace active_space,
                                       uint32_t active_base)
{
    // 7AAAAAAA 00T0VVVV
    const uint32_t op = (code.value >> 20) & 0xFu;
    const uint32_t address = active_base + (code.command & 0x0FFFFFFFu);
    const size_t size = (op & 1u) ? 2 : 1;
    const uint32_t rhs = size == 1 ? (code.value & 0xFFu)
                                   : (code.value & 0xFFFFu);

    if (op > 5) {
        m_last_runtime_message =
            "Unsupported type-7 operation on source line " +
            std::to_string(code.source_line);
        return false;
    }

    uint32_t current = 0;
    if (!ReadValue(active_space, address, size, current)) {
        return false;
    }

    uint32_t result = 0;
    switch (op) {
    case 0:
    case 1:
        result = current | rhs;
        break;
    case 2:
    case 3:
        result = current & rhs;
        break;
    case 4:
    case 5:
        result = current ^ rhs;
        break;
    }
    return WriteValue(active_space, address, size, result);
}

bool CheatEngineWindow::PrepareAddressContext(
    const RawCode &code, const std::vector<AddressContext> &contexts,
    uint32_t active_base, AddressContext &next_context, bool &push_context)
{
    const uint32_t mode = (code.command >> 24) & 0xF;
    const uint32_t count = Type9Count(code);
    const uint32_t operand = code.value;
    const uint32_t source = active_base + operand;

    next_context.remaining = count;
    next_context.until_end = count == 0;

    switch (mode) {
    case 0x0: // virtual direct base
        next_context.space = GuestAddressSpace::Virtual;
        next_context.base = contexts.empty() ? operand : source;
        push_context = true;
        return true;
    case 0x1: // physical direct base
        next_context.space = GuestAddressSpace::Physical;
        next_context.base = contexts.empty() ? operand : source;
        push_context = true;
        return true;
    case 0x2: { // virtual pointer base
        uint32_t ptr = 0;
        next_context.space = GuestAddressSpace::Virtual;
        if (!ReadValue(GuestAddressSpace::Virtual,
                       contexts.empty() ? operand : source, 4, ptr)) {
            return false;
        }
        next_context.base = ptr;
        push_context = true;
        return true;
    }
    case 0x3: { // physical pointer base
        uint32_t ptr = 0;
        next_context.space = GuestAddressSpace::Physical;
        if (!ReadValue(GuestAddressSpace::Physical,
                       contexts.empty() ? operand : source, 4, ptr)) {
            return false;
        }
        next_context.base = ptr;
        push_context = true;
        return true;
    }
    default:
        m_last_runtime_message =
            "Unsupported type-9 mode on source line " +
            std::to_string(code.source_line);
        return false;
    }
}

bool CheatEngineWindow::ExecuteRawBytes(const CheatBlock &block, size_t index,
                                        GuestAddressSpace active_space,
                                        uint32_t active_base,
                                        size_t &next_index)
{
    const RawCode &code = block.codes[index];
    // xemu variable-length raw-byte fill/write:
    //   AXXXXXXX ZZZZZZZZ
    //   DDDDDDDD DDDDDDDD  <- 8 literal bytes
    const uint32_t byte_count = code.value;
    const size_t data_lines =
        byte_count == 0 ? 0 :
        (size_t)(byte_count / 8u) + (byte_count % 8u != 0 ? 1u : 0u);
    const size_t total_lines = 1u + data_lines;
    next_index = std::min(block.codes.size(), index + total_lines);

    if (byte_count == 0) {
        m_last_runtime_message =
            "Type-A byte count must be greater than zero on source line " +
            std::to_string(code.source_line);
        return false;
    }
    if (data_lines > block.codes.size() - index - 1u) {
        m_last_runtime_message =
            "Truncated type-A raw-byte write on source line " +
            std::to_string(code.source_line) + " (needs " +
            std::to_string(data_lines) + " continuation line" +
            (data_lines == 1 ? "" : "s") + ")";
        return false;
    }

    const uint32_t address = active_base + (code.command & 0x0FFFFFFFu);
    size_t bytes_remaining = (size_t)byte_count;
    uint32_t write_address = address;

    for (size_t line_index = 0; line_index < data_lines; ++line_index) {
        const RawCode &data = block.codes[index + 1u + line_index];
        const uint8_t bytes[8] = {
            (uint8_t)((data.command >> 24) & 0xFFu),
            (uint8_t)((data.command >> 16) & 0xFFu),
            (uint8_t)((data.command >> 8) & 0xFFu),
            (uint8_t)(data.command & 0xFFu),
            (uint8_t)((data.value >> 24) & 0xFFu),
            (uint8_t)((data.value >> 16) & 0xFFu),
            (uint8_t)((data.value >> 8) & 0xFFu),
            (uint8_t)(data.value & 0xFFu),
        };
        const size_t chunk = std::min<size_t>(bytes_remaining, 8u);
        if (!WriteGuest(active_space, write_address, bytes, chunk)) {
            return false;
        }
        write_address += (uint32_t)chunk;
        bytes_remaining -= chunk;
    }
    return true;
}

bool CheatEngineWindow::ExecuteTypeF(
    size_t block_index, size_t code_index, const RawCode &code,
    GuestAddressSpace active_space, uint32_t active_base,
    std::vector<uint64_t> &active_hooks)
{
    const uint32_t subtype = (code.command >> 24) & 0xFu;
    const uint64_t hook_key = ((uint64_t)block_index << 32) |
                              (uint32_t)code_index;

    const uint32_t flags = code.command & 0x00FFFFFFu;
    if (flags != 0) {
        m_last_runtime_message =
            "Type-F low 24 bits are reserved and must be 000000 on source line " +
            std::to_string(code.source_line);
        return false;
    }
    if (subtype != 0x0 && subtype != 0x1) {
        m_last_runtime_message =
            "Unsupported Type-F subtype on source line " +
            std::to_string(code.source_line);
        return false;
    }
    if (!code.f_terminated) {
        m_last_runtime_message =
            "Type-F block is missing DEADCODE (source line " +
            std::to_string(code.source_line) + ")";
        return false;
    }
    if (active_space != GuestAddressSpace::Virtual) {
        m_last_runtime_message =
            "Type-F hooks require a Virtual address context on source line " +
            std::to_string(code.source_line);
        return false;
    }

    const uint32_t hook_address = active_base + code.value;

    std::vector<uint8_t> bytes;
    std::vector<uint8_t> attached_data;
    const std::vector<XemuCheatAsmLine> *f0_source = nullptr;
    bool f0_uses_preserve = false;
    uint32_t preserve_bytes = 0;
    bool f0_uses_temp = false;
    uint32_t temp_bytes = 0;
    std::string definition_signature;

    if (subtype == 0x0) {
        /* This is the exact signature that InstallFHook() already uses. Build
         * it before the probe assembly so a steady-state installed F0 can be
         * recognized without reassembling identical source every engine tick. */
        size_t signature_size = 3u;
        for (const XemuCheatAsmLine &src : code.f_body) {
            signature_size += src.text.size() + 1u;
        }
        definition_signature.reserve(signature_size);
        definition_signature = "F0\n";
        for (const XemuCheatAsmLine &src : code.f_body) {
            definition_signature += src.text;
            definition_signature.push_back('\n');
        }

        auto installed = m_f_hooks.find(hook_key);
        if (installed != m_f_hooks.end() && installed->second.installed &&
            installed->second.hook_address == hook_address &&
            installed->second.definition_signature == definition_signature) {
            installed->second.owner_block = block_index;
            active_hooks.push_back(hook_key);
            return true;
        }

        XemuCheatAsmResult assembled;
        if (!xemu_cheat_assemble_x86_32(code.f_body, assembled)) {
            m_last_runtime_message = "Type-F0 assembler error";
            if (assembled.error_line > 0) {
                m_last_runtime_message += " on source line " +
                    std::to_string(assembled.error_line);
            }
            m_last_runtime_message += ": " + assembled.error;
            return false;
        }
        bytes = std::move(assembled.bytes);
        attached_data = std::move(assembled.data);
        f0_source = &code.f_body;
        f0_uses_preserve = assembled.uses_preserve;
        preserve_bytes = assembled.preserve_bytes;
        f0_uses_temp = assembled.uses_temp;
        temp_bytes = assembled.temp_bytes;
    } else {
        std::string error;
        int error_line = code.source_line;
        if (!ParseFRawHex(code, bytes, error, error_line)) {
            m_last_runtime_message =
                "Type-F1 raw-hex error on source line " +
                std::to_string(error_line) + ": " + error;
            return false;
        }
        definition_signature.assign("F1\n", 3);
        definition_signature.append(
            reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }

    if (!InstallFHook(block_index, hook_key, hook_address, bytes, attached_data,
                      f0_source, f0_uses_preserve, preserve_bytes,
                      f0_uses_temp, temp_bytes, definition_signature)) {
        if (m_last_runtime_message.empty()) {
            m_last_runtime_message =
                "Type-F hook installation failed on source line " +
                std::to_string(code.source_line);
        }
        return false;
    }

    active_hooks.push_back(hook_key);
    return true;
}

bool CheatEngineWindow::ExecuteConditional(
    size_t block_index, const CheatBlock &block, size_t index,
    const RawCode &code, const std::vector<AddressContext> &contexts,
    GuestAddressSpace active_space, uint32_t active_base,
    size_t &next_index)
{
    const uint32_t type = code.command >> 28;
    const uint32_t n = (code.value >> 24) & 0xFF;
    const uint32_t test = (code.value >> 20) & 0x7;
    const uint32_t field = (code.value >> 16) & 0xF;
    const bool is_8bit = (field & 0x2) != 0;
    const bool requested_virtual = (field & 0x1) != 0;
    const uint32_t compare_value = code.value & 0xFFFF;

    GuestAddressSpace compare_space;
    uint32_t compare_address;
    if (!contexts.empty()) {
        compare_space = active_space;
        compare_address = active_base + (code.command & 0x0FFFFFFFu);
    } else {
        compare_space = requested_virtual ? GuestAddressSpace::Virtual
                                          : GuestAddressSpace::Physical;
        compare_address = code.command & 0x0FFFFFFFu;
    }

    uint32_t memory_value = 0;
    const bool runtime_ok = ReadValue(compare_space, compare_address,
                                      is_8bit ? 1 : 2, memory_value);

    bool condition = false;
    if (runtime_ok) {
        const uint32_t rhs = is_8bit ? (compare_value & 0xFF) : compare_value;
        switch (test) {
        case 0: condition = memory_value == rhs; break;
        case 1: condition = memory_value != rhs; break;
        case 2: condition = memory_value < rhs; break;
        case 3: condition = memory_value > rhs; break;
        case 4: condition = (memory_value & rhs) == 0; break;
        case 5: condition = (memory_value & rhs) != 0; break;
        case 6: condition = (memory_value | rhs) == 0; break;
        case 7: condition = (memory_value | rhs) != 0; break;
        }
    }

    bool guard_active = condition;
    if (type == 0xE) {
        const uint64_t key = ((uint64_t)block_index << 32) | (uint32_t)index;
        SwitchState &state = m_switches[key];
        if (runtime_ok && condition && !state.previous_condition) {
            state.on = !state.on;
        }
        state.previous_condition = runtime_ok && condition;
        guard_active = state.on;
    }

    if (!runtime_ok) {
        guard_active = false;
    }

    if (!guard_active && n > 0) {
        if (m_code_aware_skip) {
            next_index = SkipLogicalCommands(block.codes, index + 1, n);
        } else {
            next_index = std::min(block.codes.size(),
                                  index + 1 + (size_t)n);
        }
    }
    return runtime_ok;
}

void CheatEngineWindow::ExecuteBlock(size_t block_index, CheatBlock &block)
{
    m_address_context_scratch.clear();
    std::vector<AddressContext> &contexts = m_address_context_scratch;
    m_active_f_hooks_scratch.clear();
    std::vector<uint64_t> &f_active_hooks = m_active_f_hooks_scratch;

    auto consume_one_physical_line = [&]() {
        for (auto &ctx : contexts) {
            if (!ctx.until_end && ctx.remaining > 0) {
                --ctx.remaining;
            }
        }
        contexts.erase(std::remove_if(contexts.begin(), contexts.end(),
                                      [](const AddressContext &ctx) {
                                          return !ctx.until_end &&
                                                 ctx.remaining == 0;
                                      }),
                       contexts.end());
    };

    auto consume_skipped_range = [&](size_t from, size_t to) {
        for (size_t p = from; p < to; ++p) {
            consume_one_physical_line();
        }
    };

    size_t i = 0;
    while (i < block.codes.size()) {
        const RawCode &code = block.codes[i];
        const uint32_t type = code.command >> 28;
        const size_t contexts_before = contexts.size();

        GuestAddressSpace active_space = GuestAddressSpace::Virtual;
        uint32_t active_base = 0;
        if (!contexts.empty()) {
            active_space = contexts.back().space;
            active_base = contexts.back().base;
        }

        bool runtime_ok = true;
        bool push_context = false;
        AddressContext next_context;
        size_t next_i = i + 1;

        if (type <= 0x2) {
            runtime_ok = ExecuteBasicWrite(code, active_space, active_base);
        } else if (type == 0x3) {
            runtime_ok = ExecuteArithmetic(block, i, active_space, active_base,
                                           next_i);
        } else if (type == 0x4) {
            runtime_ok = ExecuteSerial(block, i, active_space, active_base,
                                       next_i);
        } else if (type == 0x5) {
            runtime_ok = ExecuteCopy(block, i, active_space, active_base,
                                     next_i);
        } else if (type == 0x6) {
            runtime_ok = ExecutePointer(block, i, active_space, active_base,
                                        next_i);
        } else if (type == 0x7) {
            runtime_ok = ExecuteBitwise(code, active_space, active_base);
        } else if (type == 0x9) {
            runtime_ok = PrepareAddressContext(code, contexts, active_base,
                                               next_context, push_context);
        } else if (type == 0xA) {
            runtime_ok = ExecuteRawBytes(block, i, active_space, active_base,
                                         next_i);
        } else if (type == 0xF) {
            runtime_ok = ExecuteTypeF(block_index, i, code, active_space,
                                      active_base, f_active_hooks);
        } else if (type == 0xD || type == 0xE) {
            runtime_ok = ExecuteConditional(block_index, block, i, code,
                                            contexts, active_space, active_base,
                                            next_i);
        } else {
            // Types 8/B/C are CodeBreaker setup/master-engine families.
            // They are intentionally not mapped to Xbox memory operations yet.
            runtime_ok = false;
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                          "Unsupported RAW type %X on source line %d",
                          type, code.source_line);
            m_last_runtime_message = msg;
        }

        // The current physical line consumes any contexts that were active
        // before it. A type-9 context starts *after* its own header line.
        for (size_t c = 0; c < contexts_before && c < contexts.size(); ++c) {
            if (!contexts[c].until_end && contexts[c].remaining > 0) {
                --contexts[c].remaining;
            }
        }
        contexts.erase(std::remove_if(contexts.begin(), contexts.end(),
                                      [](const AddressContext &ctx) {
                                          return !ctx.until_end &&
                                                 ctx.remaining == 0;
                                      }),
                       contexts.end());

        if (push_context) {
            contexts.push_back(next_context);
        }

        if (!runtime_ok && m_last_runtime_message.empty()) {
            m_last_runtime_message = "Memory access failed on source line " +
                                     std::to_string(code.source_line);
        }

        if (next_i > i + 1) {
            // Continuation lines consumed by 3/4/5/6/A, or lines skipped by D/E,
            // still consume outer type-9 raw-line scopes. They are data/skip
            // lines here, so a type-9-looking continuation is never activated.
            consume_skipped_range(i + 1, next_i);
        }

        i = next_i;
    }

    /* F hooks are persistent machine-code patches, unlike ordinary periodic
     * writes. If an F0/F1 command was skipped by D/E this tick, restore its
     * original bytes so conditions control hooks predictably. */
    for (auto &entry : m_f_hooks) {
        if (entry.second.owner_block == block_index && entry.second.installed &&
            !std::binary_search(f_active_hooks.begin(), f_active_hooks.end(),
                                entry.first)) {
            DeactivateFHook(entry.first);
        }
    }
}

void CheatEngineWindow::SetGroupSelected(int group_index, bool selected)
{
    if (group_index < 0 || (size_t)group_index >= m_groups.size()) {
        return;
    }
    const CheatGroup &group = m_groups[(size_t)group_index];
    for (size_t cheat : group.cheats) {
        if (cheat < m_blocks.size()) {
            m_blocks[cheat].selected = selected;
            if (m_live_cheats_enabled) {
                m_blocks[cheat].enabled = selected;
                if (!selected) {
                    DeactivateFHooksForBlock(cheat);
                }
            }
        }
    }
    for (int child : group.child_groups) {
        SetGroupSelected(child, selected);
    }
    if (m_live_cheats_enabled) {
        m_switches.clear();
    }
}

void CheatEngineWindow::DisableAllCheats(bool clear_selection)
{
    DeactivateAllFHooks();
    for (auto &block : m_blocks) {
        block.enabled = false;
        if (clear_selection) {
            block.selected = false;
        }
    }
    m_switches.clear();
}

void CheatEngineWindow::SetLiveCheatsEnabled(bool enabled)
{
    if (m_live_cheats_enabled == enabled) {
        return;
    }
    m_live_cheats_enabled = enabled;
    if (!enabled) {
        DisableAllCheats(true);
    }
}

void CheatEngineWindow::CountGroupSelection(int group_index, size_t &selected,
                                            size_t &total) const
{
    if (group_index < 0 || (size_t)group_index >= m_groups.size()) {
        return;
    }
    const CheatGroup &group = m_groups[(size_t)group_index];
    for (size_t cheat : group.cheats) {
        if (cheat < m_blocks.size()) {
            ++total;
            if (m_blocks[cheat].selected) {
                ++selected;
            }
        }
    }
    for (int child : group.child_groups) {
        CountGroupSelection(child, selected, total);
    }
}

void CheatEngineWindow::DrawCheat(size_t block_index)
{
    if (block_index >= m_blocks.size()) {
        return;
    }
    CheatBlock &block = m_blocks[block_index];
    ImGui::PushID((int)block_index);
    if (ImGui::Checkbox("##selected", &block.selected)) {
        if (m_live_cheats_enabled) {
            block.enabled = block.selected;
            if (!block.enabled) {
                DeactivateFHooksForBlock(block_index);
            }
            m_switches.clear();
        }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(block.name.c_str());
    if (block.enabled) {
        ImGui::SameLine();
        ImGui::TextDisabled("[ACTIVE]");
    }
    if (ImGui::IsItemHovered() &&
        (!block.description.empty() || !block.credits.empty())) {
        std::string tip;
        if (!block.description.empty()) {
            tip += block.description;
        }
        if (!block.credits.empty()) {
            if (!tip.empty()) tip += "\n\n";
            tip += "Credits: " + block.credits;
        }
        ImGui::SetTooltip("%s", tip.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu RAW lines)", block.codes.size());
    ImGui::PopID();
}

void CheatEngineWindow::DrawGroup(int group_index)
{
    if (group_index < 0 || (size_t)group_index >= m_groups.size()) {
        return;
    }
    CheatGroup &group = m_groups[(size_t)group_index];

    if (group_index == 0) {
        for (size_t cheat : group.cheats) {
            DrawCheat(cheat);
        }
        for (int child : group.child_groups) {
            DrawGroup(child);
        }
        return;
    }

    ImGui::PushID(group_index);
    size_t selected = 0, total = 0;
    CountGroupSelection(group_index, selected, total);
    bool all_selected = total != 0 && selected == total;
    bool group_checkbox = all_selected;
    if (ImGui::Checkbox("##group-select", &group_checkbox)) {
        SetGroupSelected(group_index, group_checkbox);
    }
    if (selected != 0 && selected != total) {
        ImGui::SameLine();
        ImGui::TextDisabled("[-]");
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.65f, 1.00f, 1.00f));
    bool open = ImGui::TreeNodeEx("##group-tree",
                                  ImGuiTreeNodeFlags_DefaultOpen |
                                  ImGuiTreeNodeFlags_SpanAvailWidth,
                                  "%s (%zu/%zu)", group.name.c_str(),
                                  selected, total);
    ImGui::PopStyleColor();
    if (open) {
        for (size_t cheat : group.cheats) {
            DrawCheat(cheat);
        }
        for (int child : group.child_groups) {
            DrawGroup(child);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void CheatEngineWindow::Tick()
{
    MaybeAutoLoadCurrentGame();

    const bool cpu_available = xemu_cheat_cpu_available() != 0;
    for (size_t i = 0; i < m_blocks.size(); ++i) {
        if (!m_engine_enabled || !m_live_cheats_enabled ||
            !m_blocks[i].enabled) {
            if (cpu_available) {
                DeactivateFHooksForBlock(i);
            }
        }
    }

    if (!m_engine_enabled || !m_live_cheats_enabled || !cpu_available) {
        return;
    }

    m_last_runtime_message.clear();
    for (size_t i = 0; i < m_blocks.size(); ++i) {
        if (m_blocks[i].enabled) {
            ExecuteBlock(i, m_blocks[i]);
        }
    }
}

void CheatEngineWindow::DrawMenuBar()
{
    if (!ImGui::BeginMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open", nullptr)) {
            OpenCheatDirectory();
        }
        if (ImGui::MenuItem("Reload", nullptr)) {
            ReloadCurrentCheatFile();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Edit/Add Cheats", nullptr)) {
            EditOrCreateCurrentCheatFile();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Options")) {
        ImGui::MenuItem("Code-Type-Aware D/E", nullptr, &m_code_aware_skip);
        if (ImGui::MenuItem("Auto-load Current Game File", nullptr,
                            &m_auto_load_current_game) &&
            m_auto_load_current_game) {
            m_seen_game_generation = UINT64_MAX;
        }
        if (ImGui::MenuItem("Engine Enabled", nullptr, &m_engine_enabled) &&
            !m_engine_enabled) {
            DeactivateAllFHooks();
                }
        ImGui::EndMenu();
    }

    if (ImGui::MenuItem("Help", nullptr)) {
        m_show_help = true;
    }

    ImGui::EndMenuBar();
}

void CheatEngineWindow::DrawHelpPopup()
{
    if (m_show_help) {
        ImGui::OpenPopup("Code Types & Examples");
        m_show_help = false;
    }

    ImGui::SetNextWindowSize(ImVec2(680, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal("Code Types & Examples", nullptr,
                                ImGuiWindowFlags_NoCollapse)) {
        return;
    }

    ImGui::BeginChild("##CodeTypeHelp", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
    ImGui::TextWrapped(
        "Default address space: Virtual RAM. Addresses are used literally; "
        "no implicit 0x80000000 base is added. Type 9 can override the context.");
    ImGui::Separator();

    auto section = [](const char *title, const char *body) {
        ImGui::TextColored(ImVec4(0.30f, 0.65f, 1.00f, 1.00f), "%s", title);
        ImGui::TextUnformatted(body);
        ImGui::Spacing();
    };

    section("0 / 1 / 2 - Basic Writes",
            "0AAAAAAA  000000VV\n"
            "    0 = 8-bit Write\n"
            "    A = Virtual Address / Current Context Offset\n"
            "    V = Value\n\n"
            "1AAAAAAA  0000VVVV\n"
            "    1 = 16-bit Write\n"
            "    A = Virtual Address / Current Context Offset\n"
            "    V = Value\n\n"
            "2AAAAAAA  VVVVVVVV\n"
            "    2 = 32-bit Write\n"
            "    A = Virtual Address / Current Context Offset\n"
            "    V = Value\n\n"
            "Example\n"
            "    2006C32C  00AAE9C0");

    section("3 - Arithmetic",
            "30T0VVVV  0AAAAAAA\n"
            "    T = Type\n"
            "        0 = Byte Increment\n"
            "        1 = Byte Decrement\n"
            "        2 = Halfword Increment\n"
            "        3 = Halfword Decrement\n"
            "        4 = Word Increment (uses continuation line)\n"
            "        5 = Word Decrement (uses continuation line)\n"
            "    V = Amount for byte/halfword forms\n"
            "    A = Virtual Address / Current Context Offset");

    section("4 - 32-bit Serial Write",
            "4AAAAAAA  NNNNSSSS\n"
            "VVVVVVVV  IIIIIIII\n"
            "    A = Start Address / Current Context Offset\n"
            "    N = Number of Writes\n"
            "    S = Address Step (SSSS * 4)\n"
            "    V = Starting 32-bit Value\n"
            "    I = Value Increment per Write");

    section("5 - Byte Copy",
            "5AAAAAAA  NNNNNNNN\n"
            "DDDDDDDD  00000000\n"
            "    A = Source Address / Current Context Offset\n"
            "    N = Number of Bytes\n"
            "    D = Destination Address / Current Context Offset");

    section("6 - Pointer Write",
            "6AAAAAAA  VVVVVVVV\n"
            "000TNNNN  OOOOOOOO\n"
            "    A = Pointer Address / Current Context Offset\n"
            "    V = Value\n"
            "    T = Write Size\n"
            "        0 = 8-bit\n"
            "        1 = 16-bit\n"
            "        2 = 32-bit\n"
            "    N = Offset Count\n"
            "    O = Pointer Offset");

    section("7 - Bitwise",
            "7AAAAAAA  00T0VVVV\n"
            "    A = Virtual Address / Current Context Offset\n"
            "    T = Type\n"
            "        0 = Byte OR\n"
            "        1 = Halfword OR\n"
            "        2 = Byte AND\n"
            "        3 = Halfword AND\n"
            "        4 = Byte XOR\n"
            "        5 = Halfword XOR\n"
            "    V = Value");

    section("9 - Address Context",
            "9MCCCCCC  BBBBBBBB\n"
            "M = Type\n"
            "    0 = Virtual Base\n"
            "    1 = Physical Base\n"
            "    2 = Virtual Pointer Base\n"
            "    3 = Physical Pointer Base\n"
            "C = Count\n"
            "    000000 = Continue Until End of Cheat\n"
            "B = Offset / Base Address\n\n"
            "Normal codes already use Virtual RAM by default, so Type 90 is\n"
            "mainly useful when a relative Virtual Base is wanted.");

    section("A - Literal Raw-Byte Write",
            "AXXXXXXX  ZZZZZZZZ\n"
            "DDDDDDDD  DDDDDDDD\n"
            "    X = Virtual Address / Current Context Offset\n"
            "    Z = Total Number of Bytes to Write\n"
            "    D = Literal Bytes in the Exact Order Shown\n"
            "    Extra Lines = ceil(Z / 8)\n\n"
            "Example\n"
            "    A006C320  00000006\n"
            "    0F85A900  00000000\n"
            "Writes: 0F 85 A9 00 00 00");

    section("D / E - Conditions",
            "D = Level Conditional\n"
            "E = Edge-Toggle Conditional\n\n"
            "Code-Type-Aware D/E = ON\n"
            "    Skip counts operate on logical commands.\n\n"
            "Code-Type-Aware D/E = OFF\n"
            "    Skip counts operate on exact physical RAW lines.");

    section("F - Automatic External x86 Code Cave",
            "F0000000  AAAAAAAA   Recommended: x86 assembly\n"
            "<Intel-syntax instructions / labels>\n"
            "DEADCODE\n\n"
            "F1000000  AAAAAAAA   Advanced: aligned raw x86 bytes\n"
            "XXXXXXXX  YYYYYYYY\n"
            "...\n"
            "DEADCODE  000000NN\n\n"
            "    A = Virtual Hook Address / Virtual Type-9 Offset\n"
            "    NN = 01-08 valid bytes in the final F1 8-byte line\n"
            "    Low 24 F0/F1 bits are reserved and must be 000000.\n"
            "    A leading $ is optional on every Type-F line.\n\n"
            "xemu automatically allocates a 16-byte-aligned cave in its\n"
            "private 1 MiB mapped arena. 68000000-680EFFFF is code/DD space;\n"
            "680F0000-680FFFFF is reserved for private PRESERVE/T0-T7/TFLAGS state. The\n"
            "mapping is outside Xbox machine RAM. Capstone covers complete\n"
            "instructions until at least 5 hook bytes are available, then xemu\n"
            "writes JMP rel32 + NOP padding and generates the DEADCODE JMP back.\n"
            "Disabling safely restores/frees only that cave; other caves never\n"
            "move or shift.\n\n"
            "F0 supports labels, common integer/control-flow x86, DD static data,\n"
            "PRESERVEALL, PRESERVE <regs>, RESTORE, and RESTORE <regs>. DD data\n"
            "may follow DEADCODE and is physically placed after the generated\n"
            "return JMP inside the same allocation. Bare numbers are hexadecimal.\n"
            "PRESERVE supports EAX/EBX/ECX/EDX/ESI/EDI/EBP/EFLAGS; ESP/EIP are\n"
            "intentionally excluded. F0 also provides persistent virtual 32-bit\n"
            "scratch registers T0-T7 plus private TFLAGS. Common forms:\n"
            "mov T0,CarList; mov T1,[T0]; test T1,T1; cmp eax,T1; add T0,4.\n"
            "T-based flag writers capture their result into TFLAGS and restore\n"
            "the game's EFLAGS; following Jcc/SETcc/CMOVcc/LOOPcc consume TFLAGS.\n"
            "Each T-using F0 owns one private bank that persists until disable/reset.\n\n"
            "F1 always uses two 32-bit words per data line. DEADCODE's NN\n"
            "keeps only the first NN bytes of the final line; the remaining\n"
            "bytes must be 00 padding and are not written. The generated JMP\n"
            "back is placed immediately after the final valid byte.\n\n"
            "Example - money hook at 0009D411\n"
            "    $F0000000  0009D411\n"
            "    $mov dword ptr [eax+58],054C5638\n"
            "    $mov eax,ecx\n"
            "    $DEADCODE\n\n"
            "F0 data/preserve example\n"
            "    $PRESERVE ECX, EDX\n"
            "    $mov edx,CarList\n"
            "    ...\n"
            "    $RESTORE\n"
            "    $DEADCODE\n"
            "    $CarList:\n"
            "    $dd 01D28710, 8B80C5FC, E3BDE8CB\n\n"
            "Equivalent F1 raw form\n"
            "    $F1000000  0009D411\n"
            "    $C7405838  564C058B\n"
            "    $C1000000  00000000\n"
            "    $DEADCODE  00000001");

    section("CMP-style File Layout",
            "^1 = Hash: <XBE Header SHA-256>\n"
            "^2 = GameID: EA009E\n"
            "     4541009E is also accepted\n"
            "^3 = NAME: Game Name\n\n"
            "!Group Name:\n"
            "    +Cheat Name{Optional Description}\n"
            "    %Credits: Author>Author2\n"
            "    $XXXXXXXX YYYYYYYY\n"
            "!!");

    ImGui::EndChild();
    if (ImGui::Button("Close")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void CheatEngineWindow::Draw(bool detached)
{
    if (!is_open) {
        return;
    }

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_MenuBar;
    const char *window_name = "RAW Cheat Engine";
    bool *window_open = &is_open;
    if (detached) {
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
        window_name = "##DetachedRawCheatEngine";
        window_open = nullptr;
    } else {
        ImGui::SetNextWindowSize(ImVec2(940, 700), ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin(window_name, window_open, window_flags)) {
        ImGui::End();
        return;
    }

    DrawMenuBar();
    DrawHelpPopup();

    current_game_manager.DrawInlineSummary("cheat-engine-current-game");
    if (!m_file_status.empty()) {
        ImGui::TextDisabled("%s", m_file_status.c_str());
    }
    if (!m_engine_enabled) {
        ImGui::TextDisabled("Engine is disabled in Options.");
    }
    ImGui::Separator();

    if (ImGui::Button("Reload (Cheat File)")) {
        ReloadCurrentCheatFile();
    }
    ImGui::SameLine();

    if (m_live_cheats_enabled) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.22f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.55f, 0.27f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.38f, 0.19f, 1.00f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.20f, 0.20f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.24f, 0.24f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.38f, 0.16f, 0.16f, 1.00f));
    }
    if (ImGui::Button(m_live_cheats_enabled ? "Enabled" : "Disabled")) {
        SetLiveCheatsEnabled(!m_live_cheats_enabled);
    }
    ImGui::PopStyleColor(3);

    size_t active_count = 0;
    for (const auto &block : m_blocks) {
        if (block.enabled) {
            ++active_count;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Active: %zu", active_count);

    if (!m_parse_messages.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Parse messages: %zu", m_parse_messages.size());
        if (ImGui::IsItemHovered()) {
            std::string messages;
            for (const auto &msg : m_parse_messages) {
                if (!messages.empty()) messages += "\n";
                messages += msg;
            }
            ImGui::SetTooltip("%s", messages.c_str());
        }
    }
    if (!m_last_runtime_message.empty()) {
        ImGui::TextWrapped("Runtime: %s", m_last_runtime_message.c_str());
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Codes");

    const bool tree_disabled = !m_live_cheats_enabled || !m_engine_enabled;
    if (tree_disabled) {
        ImGui::BeginDisabled();
    }
    ImGui::BeginChild("##CheatTree", ImVec2(-1, -1), true);
    if (m_blocks.empty()) {
        ImGui::TextDisabled("No matching codes loaded for the current game.");
    } else {
        DrawGroup(0);
    }
    ImGui::EndChild();
    if (tree_disabled) {
        ImGui::EndDisabled();
    }

    ImGui::End();
}
