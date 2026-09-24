// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JSelfInstaller — put a downloaded, checked release of this application in place and start it.
//
// Two steps, because the second can only happen once the application has closed:
//   stage()             — while running: write the download where the install will want it.
//   installAndRestart() — after the main loop has returned: swap it in and start the new copy.
//
// WINDOWS: the download is the application's installer. It is run with /SILENT, so the person sees its
// progress bar but is asked nothing; it installs over this copy (an Inno Setup installer with the same
// AppId does), and its [Run] entry starts the new version when it finishes.
//
// LINUX: the download is the new AppImage. It is staged beside the running one ("<AppImage>.new"), so the
// swap is a rename on one filesystem: atomic, and safe while the old file is still running — the running
// process keeps its own copy of the old file until it exits. Only an AppImage can update itself this way;
// a copy run from a build directory or installed by a package manager is told so instead.

#include <cstdint>
#include <string>
#include <vector>

inline namespace jf {

class JSelfInstaller {
public:
    // Can this copy replace itself? False with the reason when it cannot.
    static bool canInstallHere(std::string& why);

    // Write the (already checked) download to its staging place. `stagedPath` is what
    // installAndRestart takes.
    static bool stage(const std::vector<uint8_t>& data, const std::string& assetName,
                      std::string& stagedPath, std::string& error);

    // Install the staged file and start the new copy. Call only as the application is exiting.
    static bool installAndRestart(const std::string& stagedPath, std::string& error);
};

} // inline namespace jf
