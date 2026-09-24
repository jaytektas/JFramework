// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JVersion — a release version, major.minor.patch, and the one rule for which of two is newer.
//
// Read from a tag or a version string: "v1.2.3", "1.2.3" and "1.2" all parse, and anything after the
// numbers ("-rc1", "+build") is ignored, so a tag can carry a note without breaking the comparison.
// Written back WITHOUT the tag's "v" — "1.2.3" — which is how an application states its own version, so
// "1.2.3 is available. You have 1.2.2." reads as one scheme.

#include <string>

inline namespace jf {

struct JVersion {
    int  major = 0, minor = 0, patch = 0;
    bool valid = false;    // false: the text held no version at all

    static JVersion parse(const std::string& text) {
        JVersion v;
        size_t i = 0;
        if (i < text.size() && (text[i] == 'v' || text[i] == 'V')) ++i;
        int parts[3] = { 0, 0, 0 };
        int n = 0;
        while (n < 3) {
            if (i >= text.size() || text[i] < '0' || text[i] > '9') break;
            int x = 0;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') x = x * 10 + (text[i++] - '0');
            parts[n++] = x;
            if (i < text.size() && text[i] == '.') ++i; else break;
        }
        if (n == 0) return v;
        v.major = parts[0]; v.minor = parts[1]; v.patch = parts[2];
        v.valid = true;
        return v;
    }

    bool isNewerThan(const JVersion& o) const {
        if (major != o.major) return major > o.major;
        if (minor != o.minor) return minor > o.minor;
        return patch > o.patch;
    }

    std::string text() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }
};

} // inline namespace jf
