// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// The front tab stays the front tab while others come and go. A layout restored with a tab in front, then a
// panel put back (one the layout did not name) and another taken out (one kept closed), opened on the last
// tab instead: adding put the new one in front, and taking one from before the front left the index pointing
// at the tab after it.
#include <j/core/DockManager.h>
#include <cstdio>
#include <string>

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

static std::string front(const jf::JDockHost& h) {
    std::string out;
    h.forEachDockPanel([&](jf::JDockWidget* p, const jf::JRect&, bool active, int) { if (active) out = p->title(); });
    return out;
}

int main() {
    jf::JDockWidget a("A", 0, 0, 100, 100), b("B", 0, 0, 100, 100), c("C", 0, 0, 100, 100), d("D", 0, 0, 100, 100),
        e("E", 0, 0, 100, 100);
    jf::JDockHost h;
    const jf::JDockNodeId leaf = h.addDock(&a);
    for (jf::JDockWidget* w : { &b, &c, &d, &e }) h.insertDock(w, leaf);
    h.insertDock(&c, leaf);   // brought to the front where it is
    check(front(h) == "C", "a tab brought to the front where it is");

    h.removeDock(&a);          // before the front
    check(front(h) == "C", "taking one from before the front keeps the front");
    h.removeDock(&e);          // after it
    check(front(h) == "C", "taking one from after the front keeps the front");
    h.removeDock(&c);          // the front itself: the next takes its place
    check(front(h) == "D", "taking the front: the one after it comes forward");
    h.removeDock(&d);          // the last, in front: the one before
    check(front(h) == "B", "taking the last, in front: the one before it");

    if (fails) { std::printf("test_dock_front: %d failure(s)\n", fails); return 1; }
    std::printf("test_dock_front: OK\n");
    return 0;
}
