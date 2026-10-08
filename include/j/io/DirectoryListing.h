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
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

inline namespace jf {

class JDirectoryListing {
public:
    struct Entry {
        std::string name;
        bool        isDir;
        std::uintmax_t                  size = 0;        // bytes (a file's)
        std::filesystem::file_time_type modified{};      // when last written
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
            std::error_code dec, sec, tec;
            const auto modified = it->last_write_time(tec);
            if (it->is_directory(dec)) dirs.push_back({ std::move(name), true, 0, tec ? fs::file_time_type{} : modified });
            else if (passesFilter(name, extensions)) {
                const std::uintmax_t size = it->file_size(sec);
                files.push_back({ std::move(name), false, sec ? 0 : size, tec ? fs::file_time_type{} : modified });
            }
        }
        auto byName = [](const Entry& a, const Entry& b) { return a.name < b.name; };
        std::sort(dirs.begin(), dirs.end(), byName);
        std::sort(files.begin(), files.end(), byName);
        dirs.insert(dirs.end(), files.begin(), files.end());
        return dirs;
    }

    // A path as typed, made whole: "~" (and "~/...") is the home folder, and anything not absolute is taken
    // from `cwd`; "." and ".." folded away.
    static std::filesystem::path resolve(const std::string& typed, const std::filesystem::path& cwd) {
        namespace fs = std::filesystem;
        std::string t = typed;
        if (!t.empty() && t[0] == '~' && (t.size() == 1 || t[1] == '/' || t[1] == '\\')) {
            const fs::path home = homeFolder();
            t = t.size() > 2 ? (home / t.substr(2)).string() : home.string();
        }
        fs::path p(t);
        if (!p.is_absolute()) p = cwd / p;
        return p.lexically_normal();
    }

    // The user's home folder ("/" when none is known).
    static std::filesystem::path homeFolder() {
#if defined(_WIN32)
        const char* h = std::getenv("USERPROFILE");
#else
        const char* h = std::getenv("HOME");
#endif
        return (h && *h) ? std::filesystem::path(h) : std::filesystem::path("/");
    }

    static bool isHidden(const std::string& name) { return !name.empty() && name[0] == '.'; }

    // Whether `name` ends in one of `extensions` (any case; one of several parts too: "job.xml" is
    // "board.job.xml"'s, not "board.xml"'s).
    static bool passesFilter(const std::string& name, const std::vector<std::string>& extensions) {
        if (extensions.empty()) return true;
        const std::string n = lower(name);
        return std::any_of(extensions.begin(), extensions.end(), [&](const std::string& e) {
            const std::string end = "." + lower(e);
            return n.size() > end.size() && n.compare(n.size() - end.size(), end.size(), end) == 0;
        });
    }

private:
    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    }
};

} // inline namespace jf
