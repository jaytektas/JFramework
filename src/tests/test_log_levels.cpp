// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// The log's levels and listeners: a global level reaches categories already
// used; a category's own level stays its own, and clearLevel puts it back to
// the default; a listener hears every line written, and nothing once removed.
#include <j/core/Log.h>
#include <cstdio>

using namespace jf;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    JLog& log = JLog::instance();
    log.setLogFile("/dev/null");
    log.setGlobalLevel(JLogLevel::Info);
    check(!log.enabled("t.used", JLogLevel::Debug), "Info: a Debug line is quiet");
    log.setGlobalLevel(JLogLevel::Debug);
    check(log.enabled("t.used", JLogLevel::Debug), "Debug: a category already used takes it");
    log.setLevel("t.own", JLogLevel::Error);
    log.setGlobalLevel(JLogLevel::Trace);
    JLogLevel own;
    check(!log.enabled("t.own", JLogLevel::Warn) && log.ownLevel("t.own", own) && own == JLogLevel::Error,
          "a category's own level stays its own");
    log.clearLevel("t.own");
    check(log.enabled("t.own", JLogLevel::Trace) && !log.ownLevel("t.own", own), "cleared: the default again");

    int heard = 0;
    std::string last;
    const int id = log.addListener([&](JLogLevel, const std::string& cat, const std::string& msg) {
        ++heard;
        last = cat + ": " + msg;
    });
    JLOGC("t.used", JLogLevel::Info) << "hello " << 3;
    check(heard == 1 && last == "t.used: hello 3", "a listener hears a line written");
    log.removeListener(id);
    JLOGC("t.used", JLogLevel::Info) << "again";
    check(heard == 1, "removed: it hears no more");
    std::printf(fails ? "=== %d FAILED ===\n" : "=== log levels and listeners ===\n", fails);
    return fails ? 1 : 0;
}
