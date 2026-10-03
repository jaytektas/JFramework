// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// A tab widget too narrow for its tabs: with setTabWrap the tabs that do not
// fit go onto a second row (the page moves down a row's thickness, and a tab
// on the second row is clicked like any other); without it, as it always was,
// one row.
#include <j/core/JTabWidget.h>
#include <j/core/JContainer.h>
#include <j/core/SceneGraph.h>
#include <cstdio>

using namespace jf;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    JSceneGraph graph;
    const char* labels[] = { "General Configuration", "Camera Settling", "Device Settings", "White Balance",
                             "Position", "Advanced Calibration" };
    for (bool wrap : { false, true }) {
        JTabWidget tabs(graph, 300.f, 400.f);
        tabs.setTabWrap(wrap);
        tabs.setBounds({ 0.f, 0.f, 300.f, 400.f });
        std::vector<std::unique_ptr<JContainer>> pages;
        for (const char* l : labels) {
            pages.push_back(std::make_unique<JContainer>(graph, 10.f, 10.f));
            tabs.addTab(l, pages.back().get());
        }
        JPrimitiveBuffer buf;
        tabs.populateRenderPrimitives(buf);
        const float pageTop = pages[0]->bounds().y;
        if (!wrap) {
            check(pageTop == tabs.stripThickness(), "not wrapping: one row, as always");
            continue;
        }
        check(pageTop >= 2 * tabs.stripThickness(), "wrapping: the page below two or more rows");
        // The last tab, on a later row: clicked, it is the one in front.
        tabs.handleMousePress(20.f, pageTop - tabs.stripThickness() * 0.5f);
        tabs.handleMouseRelease(20.f, pageTop - tabs.stripThickness() * 0.5f);
        check(tabs.activeTab() > 0, "a tab on the last row is clicked like any other");
    }
    std::printf(fails ? "=== %d FAILED ===\n" : "=== tabs wrap ===\n", fails);
    return fails ? 1 : 0;
}
