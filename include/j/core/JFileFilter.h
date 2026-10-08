// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JFileFilter — one choice in a file picker's file-type list: a name ("Job files") and the extensions it
// shows (without the dot, any case; empty = every file). Shown as "Job files (*.xml)".

#include <string>
#include <vector>

inline namespace jf {

struct JFileFilter {
    std::string              name;
    std::vector<std::string> extensions;

    // What the picker's list shows: the name, then the patterns ("All files (*)" for none); a filter
    // without a name is just its patterns.
    std::string label() const {
        std::string patterns;
        for (const std::string& e : extensions) patterns += (patterns.empty() ? "*." : " *.") + e;
        if (patterns.empty()) patterns = "*";
        return name.empty() ? patterns : name + " (" + patterns + ")";
    }
};

} // inline namespace jf
