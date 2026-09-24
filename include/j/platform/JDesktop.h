// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JDesktop — hand something to the desktop to open with whatever the user has chosen for it: a web
// page or an HTML help file in their browser, a folder in their file manager.
//
// Linux asks xdg-open; Windows asks the shell (ShellExecute "open"). Neither goes through a command
// line, so a path with spaces or quotes in it is passed as one argument, not re-parsed by a shell.
// The call returns once the request is made — it does not wait for the browser.

#include <string>

inline namespace jf {

class JDesktop {
public:
    // A URL ("https://…", "file:///…") or a local path. False when the desktop could not be asked.
    static bool openUrl(const std::string& urlOrPath);
};

} // inline namespace jf
