// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// test_tab_reorder.cpp — dragging a tab along its own strip rearranges the tabs.
//
// The gesture is shared with tearing a dock out: press a tab and move. While the cursor stays inside the
// strip it is a reorder, and the tabs shift as it passes them so the arrangement on screen during the
// drag is the one you get on release. Leave the strip and it becomes the ordinary tear-out.
//
// Strips are configurable — top, bottom, left, right — so the ordering axis is whichever way the strip
// runs. A vertical strip orders top-to-bottom exactly as a horizontal one orders left-to-right.

#include <j/core/DockManager.h>
#include <j/core/DockWidget.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace jf;

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

static std::string order(const JDockHost& host, JDockNodeId leaf) {
    std::string s;
    for (const JDockWidget* d : host.node(leaf)->tabs) { if (!s.empty()) s += ','; s += d->title(); }
    return s;
}

// Centre of tab `i`, in the axis the strip runs.
static void tabCentre(JDockHost& host, JDockNodeId leaf, int i, bool vert, float& mx, float& my) {
    const std::vector<JRect> slots = host.tabSlots(leaf);
    mx = slots[i].x + slots[i].width  * 0.5f;
    my = slots[i].y + slots[i].height * 0.5f;
    (void)vert;
}

static JDockNodeId threeTabbedDocks(JDockHost& host, JDockWidget* a, JDockWidget* b, JDockWidget* c) {
    const JDockNodeId leaf = host.addDock(a);
    host.insertDock(b, leaf);      // tabify into the same leaf
    host.insertDock(c, leaf);
    return leaf;
}

static void test_reorder_along_a_horizontal_strip() {
    JDockHost host;
    host.setTabEdge(JTabBarEdge::Top);
    JDockWidget a("A", 0,0,300,300), b("B", 0,0,300,300), c("C", 0,0,300,300);
    const JDockNodeId leaf = threeTabbedDocks(host, &a, &b, &c);
    host.computeLayout({ 0.f, 0.f, 600.f, 400.f });
    CHECK(order(host, leaf) == "A,B,C");

    float mx, my;
    tabCentre(host, leaf, 0, false, mx, my);
    host.handleMouse(mx, my, true, false);                 // press tab A
    tabCentre(host, leaf, 2, false, mx, my);
    host.handleMouse(mx, my, false, false);                // drag it over C's slot
    std::printf("  horizontal: %s\n", order(host, leaf).c_str());
    CHECK(order(host, leaf) == "B,C,A");
    CHECK(host.node(leaf)->activeTab == 2);                // the dragged tab stays active, at its new home
    host.handleMouse(mx, my, false, true);                 // release: it stays where it was shown
    CHECK(order(host, leaf) == "B,C,A");
}

static void test_reorder_along_a_vertical_strip() {
    JDockHost host;
    host.setTabEdge(JTabBarEdge::Left);                    // the axis is now Y
    JDockWidget a("A", 0,0,300,300), b("B", 0,0,300,300), c("C", 0,0,300,300);
    const JDockNodeId leaf = threeTabbedDocks(host, &a, &b, &c);
    host.computeLayout({ 0.f, 0.f, 600.f, 400.f });
    CHECK(order(host, leaf) == "A,B,C");

    float mx, my;
    tabCentre(host, leaf, 2, true, mx, my);
    host.handleMouse(mx, my, true, false);                 // press tab C
    tabCentre(host, leaf, 0, true, mx, my);
    host.handleMouse(mx, my, false, false);                // drag it up to the first slot
    std::printf("  vertical:   %s\n", order(host, leaf).c_str());
    CHECK(order(host, leaf) == "C,A,B");
    CHECK(host.node(leaf)->activeTab == 0);
}

// Motion inside the strip must never tear the dock out, however far along it travels — the whole point
// of the gesture split.
static void test_in_strip_motion_does_not_float() {
    JDockHost host;
    host.setTabEdge(JTabBarEdge::Top);
    JDockWidget a("A", 0,0,300,300), b("B", 0,0,300,300), c("C", 0,0,300,300);
    const JDockNodeId leaf = threeTabbedDocks(host, &a, &b, &c);
    host.computeLayout({ 0.f, 0.f, 600.f, 400.f });

    float mx, my;
    tabCentre(host, leaf, 0, false, mx, my);
    host.handleMouse(mx, my, true, false);
    tabCentre(host, leaf, 2, false, mx, my);
    const auto ev = host.handleMouse(mx, my, false, false);
    CHECK(!ev.has_value());                                // no WantsFloat, despite crossing the threshold
    std::printf("  in-strip travel produced no float event\n");
}

// Out of the strip, the same press still tears out: the reorder must not have eaten the gesture.
static void test_leaving_the_strip_still_floats() {
    JDockHost host;
    host.setTabEdge(JTabBarEdge::Top);
    JDockWidget a("A", 0,0,300,300), b("B", 0,0,300,300), c("C", 0,0,300,300);
    const JDockNodeId leaf = threeTabbedDocks(host, &a, &b, &c);
    host.computeLayout({ 0.f, 0.f, 600.f, 400.f });

    float mx, my;
    tabCentre(host, leaf, 0, false, mx, my);
    host.handleMouse(mx, my, true, false);
    const auto ev = host.handleMouse(mx, my + 220.f, false, false);   // straight down, out of the strip
    CHECK(ev.has_value());
    if (ev) CHECK(ev->type == JDockHost::JDockEvent::JType::WantsFloat);
    std::printf("  leaving the strip still requests a float\n");
}

// The regression the first version of these tests walked straight past: a real drag moves in small
// steps, so it travels through the strip before leaving it. Tearing out has to survive that.
static void test_incremental_drag_out_still_floats() {
    JDockHost host;
    host.setTabEdge(JTabBarEdge::Top);
    JDockWidget a("A", 0,0,300,300), b("B", 0,0,300,300), c("C", 0,0,300,300);
    const JDockNodeId leaf = threeTabbedDocks(host, &a, &b, &c);
    host.computeLayout({ 0.f, 0.f, 600.f, 400.f });

    float mx, my;
    tabCentre(host, leaf, 0, false, mx, my);
    host.handleMouse(mx, my, true, false);                     // press
    bool floated = false;
    for (float step = 2.f; step <= 240.f && !floated; step += 6.f) {   // creep out, a few px at a time
        const auto ev = host.handleMouse(mx, my + step, false, false);
        floated = ev.has_value() && ev->type == JDockHost::JDockEvent::JType::WantsFloat;
    }
    CHECK(floated);
    std::printf("  incremental drag out of the strip still floats\n");
}

int main() {
    std::cout << "JDockHost tab reorder tests\n";
    test_reorder_along_a_horizontal_strip();
    test_reorder_along_a_vertical_strip();
    test_in_strip_motion_does_not_float();
    test_leaving_the_strip_still_floats();
    test_incremental_drag_out_still_floats();
    std::printf(g_fails ? "tab reorder: FAILED (%d)\n" : "tab reorder: OK\n", g_fails);
    return g_fails ? 1 : 0;
}
