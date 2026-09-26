// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JLocalWebServer — serve an application's own HTML (a manual, generated help pages) to the user's
// browser, over http://127.0.0.1.
//
// Why not just open the file: a browser sandboxed as a snap (Ubuntu's default Firefox and Chromium)
// cannot read a file inside an AppImage's mount, under /tmp, or under a hidden folder such as
// ~/.local — it is handed that one file through a portal at best, and a multi-page site with its own
// stylesheets and links falls apart. Over HTTP on the loopback interface every browser can read it,
// sandboxed or not, on every platform.
//
// SCOPE, deliberately small: loopback only (never reachable from another machine), a port the system
// chooses, GET and HEAD only, files only from the folders mount() names, nothing outside them (a path
// that climbs out with ".." is refused), one connection at a time on its own thread. It is a way of
// showing local files to a local browser, not a web server.
//
//     JLocalWebServer web;
//     web.mount("manual", "/opt/app/manual");      // http://127.0.0.1:<port>/manual/…
//     std::string why;
//     if (web.start(why)) JDesktop::openUrl(web.url("manual/index.html"));

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

inline namespace jf {

class JLocalWebServer {
public:
    JLocalWebServer();
    ~JLocalWebServer();

    JLocalWebServer(const JLocalWebServer&)            = delete;
    JLocalWebServer& operator=(const JLocalWebServer&) = delete;

    // Serve `dir` under /<prefix>/. Mount before start(); a request for a folder serves its index.html.
    void mount(const std::string& prefix, const std::string& dir);

    // Listen on 127.0.0.1 on a port the system picks. False with the reason when it cannot. Starting a
    // server that is already running is a no-op that answers true.
    bool start(std::string& error);
    void stop();
    bool running() const { return m_running.load(); }
    int  port() const { return m_port; }

    // "http://127.0.0.1:<port>/<path>".
    std::string url(const std::string& path) const;

    // Which file a request path names, or "" when it names none (unmounted, or climbing out of the
    // mount). The whole security rule in one place, exposed so it can be tested without a socket.
    std::string resolve(const std::string& requestPath) const;

    // The Content-Type for a file name, by extension.
    static std::string mimeType(const std::string& path);

private:
    void serve();

    std::vector<std::pair<std::string, std::string>> m_mounts;   // prefix, canonical folder
    std::atomic<bool> m_running{false};
    long long         m_listen = -1;                              // the socket, as the platform's handle
    int               m_port = 0;
    std::thread       m_thread;
};

} // inline namespace jf
