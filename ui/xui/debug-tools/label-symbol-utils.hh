//
// xemu RAW Cheat Engine - shared Microsoft symbol display helpers
//
// Copyright (C) 2026 xemu contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#ifndef XEMU_UI_XUI_DEBUG_TOOLS_LABEL_SYMBOL_UTILS_HH
#define XEMU_UI_XUI_DEBUG_TOOLS_LABEL_SYMBOL_UTILS_HH

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace XemuLabelSymbolUtils {

inline bool all_digits(const std::string &value)
{
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isdigit(ch) != 0;
           });
}

inline std::string clean_c_symbol(std::string name)
{
    if (name.rfind("__imp_", 0) == 0) {
        name.erase(0, 6);
    }
    if (!name.empty() && (name.front() == '_' || name.front() == '@')) {
        name.erase(name.begin());
    }
    const size_t at = name.rfind('@');
    if (at != std::string::npos && at + 1 < name.size() &&
        all_digits(name.substr(at + 1))) {
        name.resize(at);
    }
    return name;
}

inline std::vector<std::string> split_at(const std::string &text)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t at = text.find('@', start);
        const size_t end = at == std::string::npos ? text.size() : at;
        parts.push_back(text.substr(start, end - start));
        if (at == std::string::npos) {
            break;
        }
        start = at + 1;
    }
    return parts;
}

inline std::string simple_msvc_name(const std::string &name)
{
    if (name.size() < 2 || name.front() != '?') {
        return {};
    }

    // Constructors/destructors are common and easy to make readable without
    // embedding a full Microsoft C++ ABI demangler in the debugger.
    if (name.rfind("??0", 0) == 0 || name.rfind("??1", 0) == 0) {
        const size_t end = name.find("@@", 3);
        if (end == std::string::npos) {
            return {};
        }
        const std::string klass = name.substr(3, end - 3);
        if (klass.empty() || klass.find('@') != std::string::npos ||
            klass.find("?$") != std::string::npos) {
            return {};
        }
        return klass + "::" + (name[2] == '1' ? "~" : "") + klass;
    }

    // Other ?? forms are operators/thunks with a more involved grammar.
    if (name[1] == '?') {
        return {};
    }

    const size_t end = name.find("@@", 1);
    if (end == std::string::npos) {
        return {};
    }
    const std::string body = name.substr(1, end - 1);
    if (body.find("?$") != std::string::npos) {
        return {};
    }
    std::vector<std::string> parts = split_at(body);
    if (parts.empty() || parts.front().empty()) {
        return {};
    }
    const std::string function = parts.front();
    std::string out;
    for (size_t i = parts.size(); i > 1; --i) {
        if (parts[i - 1].empty()) {
            continue;
        }
        if (!out.empty()) {
            out += "::";
        }
        out += parts[i - 1];
    }
    if (!out.empty()) {
        out += "::";
    }
    out += function;
    return out;
}

} // namespace XemuLabelSymbolUtils

#endif // XEMU_UI_XUI_DEBUG_TOOLS_LABEL_SYMBOL_UTILS_HH
