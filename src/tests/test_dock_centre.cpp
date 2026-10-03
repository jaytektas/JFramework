// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// Docks in the centre (JDockSpace::setCentreDocks): two docks tabbed in the centre are laid out over the
// centre rect, found there for content input, take the mouse there, and the central widget steps aside.
// Without the opt-in the centre stays a plain widget's. A side area beside a central widget stays under
// half the window; beside centre docks it may take all but what the centre's docks need.
#include <j/core/DockSpace.h>
#include <j/core/SceneGraph.h>
#include <j/core/JContainer.h>
#include <cstdio>

using namespace jf;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    JSceneGraph graph;
    JContainer central(graph);
    JDockWidget top("Top camera", 0, 0, 300, 200), bottom("Bottom camera", 0, 0, 300, 200);
    const JRect content{ 0.f, 0.f, 1000.f, 600.f };

    std::puts("1. the centre as a plain widget (the default)");
    {
        JDockSpace space;
        space.setCentralWidget(&central);
        space.setRightWidth(250.f);
        space.computeLayout(content);
        check(space.centralWidget() == &central, "the central widget is the centre");
        check(space.contentDockAt(500.f, 300.f) == nullptr, "no dock is found in the centre");
    }

    std::puts("2. docks in the centre");
    {
        JDockSpace space;
        space.setCentralWidget(&central);
        space.setCentreDocks(true);
        space.host(JDockSpace::Center).addDock(&top);
        space.host(JDockSpace::Center).addDock(&bottom);   // tabbed with it
        space.computeLayout(content);
        check(space.centralWidget() == nullptr, "the central widget steps aside");
        const JRect& c = space.centerRect();
        check(c.width == 1000.f && c.height == 600.f, "the centre spans the content (no edge areas)");
        const auto st = space.host(JDockSpace::Center).tabStateOf(&bottom);
        check(st.held && st.tabCount == 2, "both docks share one leaf, as tabs");
        JDockWidget* found = space.contentDockAt(500.f, 300.f);
        check(found == &top || found == &bottom, "the front tab is found in the centre for input");
        check(space.isHost(JDockSpace::Center) && space.ownsHost(&space.host(JDockSpace::Center)),
              "the centre counts as one of the space's hosts");
        JPrimitiveBuffer buf;
        space.render(buf);
        check(!buf.getCommands().empty(), "it draws");
    }

    std::puts("3. how big a side may be");
    {
        JDockWidget side("Cameras", 0, 0, 300, 200), other("Setup", 0, 0, 300, 200), right("Right", 0, 0, 300, 200);
        JDockSpace plain;
        plain.setCentralWidget(&central);
        plain.left().addDock(&side);
        plain.setLeftWidth(700.f);
        plain.computeLayout(content);
        check(plain.centerRect().x == 450.f, "beside a central widget: under half (45 %)");

        JDockSpace docked;
        docked.setCentreDocks(true);
        docked.host(JDockSpace::Center).addDock(&other);
        docked.left().addDock(&side);
        docked.setLeftWidth(700.f);
        docked.computeLayout(content);
        check(docked.centerRect().x == 700.f, "beside centre docks: as wide as asked");
        docked.right().addDock(&right);
        docked.setRightWidth(400.f);
        docked.computeLayout(content);
        const float centreMin = docked.host(JDockSpace::Center).minWidthNeeded();
        check(docked.centerRect().width >= centreMin - 0.01f, "two sides never squeeze the centre under its docks' need");
    }

    std::printf(fails ? "=== %d FAILED ===\n" : "=== docks in the centre ===\n", fails);
    return fails ? 1 : 0;
}
