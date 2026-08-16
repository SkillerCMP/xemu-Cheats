//
// xemu RAW Cheat Engine - XBE-derived disassembler labels
//
// Copyright (C) 2026 xemu contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace XemuXbeLabels {

enum class Type {
    Entry = 0,
    Section,
    Kernel,
    String,
    Xref,
    Rtti,
    Inferred,
};

struct Label {
    uint32_t virtual_address = 0;
    Type type = Type::String;
    std::string name;
};

struct Database {
    std::vector<Label> labels;
};

bool Build(const std::vector<uint8_t> &xbe_file, Database &database,
           std::string &error);
const Label *PrimaryAt(const Database &database, uint32_t virtual_address);
const char *TypeName(Type type);

} // namespace XemuXbeLabels
