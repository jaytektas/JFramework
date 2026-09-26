// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JVersion — a release version, major.minor.patch, and the one rule for which of two is newer.
//
// Read from a tag or a version string: "v1.2.3", "1.2.3" and "1.2" all parse. A PRE-RELEASE after a
// dash ("1.2.3-beta.1", "-rc2") is kept and ordered the semver way: it comes BEFORE the release it
// leads up to (1.2.3-beta.1 < 1.2.3), and pre-releases order among themselves by their dot-separated
// parts, numbers numerically and before words (beta.2 < beta.10 < rc.1). Anything after "+" is build
// metadata and ignored. Written back WITHOUT the tag's "v" — "1.2.3", "1.2.3-beta.1" — which is how an
// application states its own version, so "1.2.3 is available. You have 1.2.2." reads as one scheme.

#include <string>

inline namespace jf {

struct JVersion {
    int  major = 0, minor = 0, patch = 0;
    std::string pre;       // the pre-release after the dash ("beta.1"), "" for a release
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
        // After the numbers: "-<pre-release>" up to any "+<build>". A trailing '.' left by "1.2." or
        // anything else is not a pre-release.
        if (i < text.size() && text[i] == '-') {
            const size_t end = text.find('+', i + 1);
            v.pre = text.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1);
        }
        v.valid = true;
        return v;
    }

    bool isNewerThan(const JVersion& o) const {
        if (major != o.major) return major > o.major;
        if (minor != o.minor) return minor > o.minor;
        if (patch != o.patch) return patch > o.patch;
        if (pre.empty() || o.pre.empty()) return pre.empty() && !o.pre.empty();   // the release is newer
        return comparePre(pre, o.pre) > 0;
    }
    bool isPreRelease() const { return !pre.empty(); }

    std::string text() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch) +
               (pre.empty() ? std::string() : "-" + pre);
    }

private:
    // Semver precedence between two pre-releases: part by part, numbers compared as numbers and ranked
    // below words, words compared as text; when one runs out first it is the lower.
    static int comparePre(const std::string& a, const std::string& b) {
        size_t i = 0, j = 0;
        while (i <= a.size() && j <= b.size()) {
            if (i == a.size() && j == b.size()) return 0;
            if (i == a.size()) return -1;
            if (j == b.size()) return 1;
            const size_t ea = a.find('.', i), eb = b.find('.', j);
            const std::string pa = a.substr(i, ea == std::string::npos ? std::string::npos : ea - i);
            const std::string pb = b.substr(j, eb == std::string::npos ? std::string::npos : eb - j);
            const bool na = !pa.empty() && pa.find_first_not_of("0123456789") == std::string::npos;
            const bool nb = !pb.empty() && pb.find_first_not_of("0123456789") == std::string::npos;
            if (na && nb) {
                if (pa.size() != pb.size()) return pa.size() < pb.size() ? -1 : 1;   // no leading zeros in semver
                if (pa != pb) return pa < pb ? -1 : 1;
            } else if (na != nb) {
                return na ? -1 : 1;
            } else if (pa != pb) {
                return pa < pb ? -1 : 1;
            }
            i = ea == std::string::npos ? a.size() : ea + 1;
            j = eb == std::string::npos ? b.size() : eb + 1;
        }
        return 0;
    }
};

} // inline namespace jf
