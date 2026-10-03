// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// Widgets in a dock's tab (JDockWidget::addTitleWidget): laid out at the end of the bar beside the close
// button while the dock is the front tab, drawn, given the mouse (a press on one is a click, not the start
// of a tab drag), and the tabs share what is left of the bar. A dock behind another shows none.
#include <j/core/DockManager.h>
#include <j/core/SceneGraph.h>
#include <j/core/JButton.h>
#include <cstdio>

using namespace jf;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    JSceneGraph graph;
    JDockWidget camera("Camera", 0, 0, 300, 200), other("Other", 0, 0, 300, 200);
    JButton snap(graph, "S", 20.f, 20.f), eye(graph, "E", 20.f, 20.f);
    int clicks = 0;
    snap.onClicked.connect([&clicks] { ++clicks; });
    camera.addTitleWidget(&eye, 20.f);
    camera.addTitleWidget(&snap, 20.f);

    JDockHost host;
    host.addDock(&camera);
    host.addDock(&other);   // tabbed with it
    host.insertDock(&camera, host.findDock(&camera));   // camera in front
    const JRect area{ 0.f, 0.f, 600.f, 400.f };
    host.computeLayout(area);
    JPrimitiveBuffer buf;
    host.populateRenderPrimitives(buf);

    std::puts("1. in the front tab's bar");
    const JRect e = eye.bounds();
    const JRect s = snap.bounds();
    check(s.x > e.x && s.x + s.width < 600.f, "in order, at the end of the bar");
    check(e.y >= 0.f && e.y + e.height <= JDockHost::TAB_BAR_SZ, "inside the bar");

    std::puts("2. the mouse");
    const float cx = s.x + s.width * 0.5f, cy = s.y + s.height * 0.5f;
    host.handleMouse(cx, cy, false, false);
    const auto ev = host.handleMouse(cx, cy, true, false);
    host.handleMouse(cx, cy, false, true);
    check(!ev && clicks == 1, "a press and release on one clicks it");
    host.handleMouse(cx - 80.f, cy, false, false);   // moving away after: no drag was started
    check(camera.placedIn() == &host, "and does not drag the tab out");

    std::puts("3. behind another tab");
    host.insertDock(&other, host.findDock(&other));
    host.computeLayout(area);
    host.handleMouse(cx, cy, true, false);
    host.handleMouse(cx, cy, false, true);
    check(clicks == 1, "a dock behind another has no widgets in the bar");

    std::printf(fails ? "=== %d FAILED ===\n" : "=== widgets in a dock's tab ===\n", fails);
    return fails ? 1 : 0;
}
