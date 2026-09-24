// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JUdevRule — may this person talk to a USB device? On Linux a vendor-specific device belongs to root
// until a udev rule says otherwise. A package installs its rule; an AppImage has no install step, so the
// application carries the rule and installs it itself, once, through pkexec — the system's own password
// prompt, so the password is typed into the system and never into the application.
//
// "Installed" means a rule naming every one of `markers` (the VID/PID strings the rule must cover) in a
// file ordered before 73-seat-late.rules — the rule that turns `uaccess` into an ACL. A copy under a later
// number looks installed and grants nothing on a real plug-in, so it does not count. Both /etc (a rule the
// application or a person put there) and /usr/lib (a package's) are looked in.
//
// Windows has no udev; binding a driver there is the installer's job, so this always answers "installed".

#include <functional>
#include <string>
#include <vector>

inline namespace jf {

class JUdevRule {
public:
    // fileName: what it is installed as, "70-jayecu.rules". contents: the rule text. markers: strings
    // every qualifying rule file contains. retire: older file names this one replaces, removed on install.
    JUdevRule(std::string fileName, std::string contents, std::vector<std::string> markers,
              std::vector<std::string> retire = {});

    bool installed() const;

    // Install it: pkexec asks for the password. Runs OFF the main thread; `done` runs on the main thread.
    void install(std::function<void(bool ok, const std::string& error)> done) const;

private:
    std::string              m_fileName, m_contents;
    std::vector<std::string> m_markers, m_retire;
};

} // inline namespace jf
