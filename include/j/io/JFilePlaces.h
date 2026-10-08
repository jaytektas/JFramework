// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JFilePlaces — the places a file picker offers to jump to, as a desktop's file chooser does: the home
// folder, its Desktop, Documents and Downloads where they exist, the whole file system, and the drives
// mounted (a USB stick, a network share) — on Linux those under /media/<user>, /run/media/<user> and
// /mnt; on Windows each drive letter in use.
//
// Separate from the picker so it can be listed (and tested) without a window.

#include <j/io/DirectoryListing.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

inline namespace jf {

class JFilePlaces {
public:
    struct Place {
        std::string           name;   // what the list shows
        std::filesystem::path path;
    };

    static std::vector<Place> list() {
        namespace fs = std::filesystem;
        std::vector<Place> out;
        std::error_code ec;
        const fs::path home = JDirectoryListing::homeFolder();
        out.push_back({ "Home", home });
        for (const char* sub : { "Desktop", "Documents", "Downloads" })
            if (fs::is_directory(home / sub, ec)) out.push_back({ sub, home / sub });
#if defined(_WIN32)
        for (char d = 'A'; d <= 'Z'; ++d) {
            const fs::path root = std::string(1, d) + ":\\";
            if (fs::exists(root, ec)) out.push_back({ std::string(1, d) + ":", root });
        }
#else
        out.push_back({ "File System", fs::path("/") });
        const char* user = std::getenv("USER");
        std::vector<fs::path> mountRoots;
        if (user && *user) {
            mountRoots.push_back(fs::path("/media") / user);
            mountRoots.push_back(fs::path("/run/media") / user);
        }
        mountRoots.push_back("/mnt");
        for (const fs::path& r : mountRoots) {
            std::vector<Place> found;
            for (fs::directory_iterator it(r, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                std::error_code dec;
                if (it->is_directory(dec)) found.push_back({ it->path().filename().string(), it->path() });
            }
            std::sort(found.begin(), found.end(), [](const Place& a, const Place& b) { return a.name < b.name; });
            out.insert(out.end(), found.begin(), found.end());
            ec.clear();
        }
#endif
        return out;
    }
};

} // inline namespace jf
