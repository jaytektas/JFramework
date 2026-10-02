// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JDirectoryListing — what a file picker shows for one folder: its sub-folders, then the files that
// pass the picker's filter, each group by name.
//
// HIDDEN ENTRIES (names starting with '.', the Unix convention) are left out unless asked for. They are
// usually clutter, but not always: an application's configuration lives in one (~/.openpnp2,
// ~/.config), and a picker that can never show them cannot open the file a user came for.
//
// Separate from JFileDialogWindow so the rules are testable without a window, and so any other view
// of a folder lists it the same way.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

inline namespace jf {

class JDirectoryListing {
public:
    struct Entry {
        std::string name;
        bool        isDir;
    };

    // `dir`'s entries: folders first, then files whose extension is in `extensions` (without the dot,
    // any case; empty = every file). Entries the process may not read are skipped, not an error.
    static std::vector<Entry> list(const std::filesystem::path& dir,
                                   const std::vector<std::string>& extensions, bool showHidden) {
        namespace fs = std::filesystem;
        std::vector<Entry> dirs, files;
        std::error_code ec;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::string name = it->path().filename().string();
            if (name.empty() || (!showHidden && isHidden(name))) continue;
            std::error_code dec;
            if (it->is_directory(dec)) dirs.push_back({ std::move(name), true });
            else if (passesFilter(name, extensions)) files.push_back({ std::move(name), false });
        }
        auto byName = [](const Entry& a, const Entry& b) { return a.name < b.name; };
        std::sort(dirs.begin(), dirs.end(), byName);
        std::sort(files.begin(), files.end(), byName);
        dirs.insert(dirs.end(), files.begin(), files.end());
        return dirs;
    }

    static bool isHidden(const std::string& name) { return !name.empty() && name[0] == '.'; }

    static bool passesFilter(const std::string& name, const std::vector<std::string>& extensions) {
        if (extensions.empty()) return true;
        const auto dot = name.rfind('.');
        if (dot == std::string::npos) return false;
        const std::string ext = lower(name.substr(dot + 1));
        return std::any_of(extensions.begin(), extensions.end(),
                           [&](const std::string& e) { return lower(e) == ext; });
    }

private:
    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    }
};

} // inline namespace jf
