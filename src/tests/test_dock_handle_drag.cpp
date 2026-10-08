// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// test_dock_handle_drag.cpp — dragging the handle between two docks moves that edge and nothing else.
//
// The dock on each side of the handle grows or shrinks by what the other gives; every other dock in the
// split keeps its size. It did not when one side had a fixed (preferred) size and the other was flexible:
// the fixed side took or gave its pixels and every flexible sibling shared the difference, so a dock past
// the handle moved too — or, with a fixed split beside the handle, it kept its size and slid along whole.

#include <j/core/DockManager.h>

#include <cmath>
#include <cstdio>

using namespace jf;

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 0.5f; }

// Three docks stacked; the middle one fixed at `middlePx` (0: flexible); the handle `handle` dragged by `by`.
static void drag(float middlePx, int handle, float by) {
    JDockHost host;
    host.setRootSplit(JSplitDir::Vertical);
    JDockConstraints fixed;
    fixed.preferredH = middlePx;
    const JDockNodeId a = host.addLeaf(host.rootId(), "A", 1.f);
    const JDockNodeId b = host.addLeaf(host.rootId(), "B", 1.f, {}, fixed);
    const JDockNodeId c = host.addLeaf(host.rootId(), "C", 1.f);
    const JRect area { 0.f, 0.f, 300.f, 900.f };
    host.computeLayout(area);
    const float ha = host.node(a)->rect.height, hb = host.node(b)->rect.height, hc = host.node(c)->rect.height;
    const JRect h = host.node(host.rootId())->handleRects[size_t(handle)];
    const float x = h.x + h.width / 2, y = h.y + h.height / 2;
    host.handleMouse(x, y, true, false);
    host.handleMouse(x, y + by, false, false);
    host.handleMouse(x, y + by, false, true);
    const float ha2 = host.node(a)->rect.height, hb2 = host.node(b)->rect.height, hc2 = host.node(c)->rect.height;
    std::printf("  middle %s, handle %d by %+.0f: A %.0f->%.0f  B %.0f->%.0f  C %.0f->%.0f\n",
                middlePx > 0.f ? "fixed" : "flexible", handle, by, ha, ha2, hb, hb2, hc, hc2);
    if (handle == 0) {
        CHECK(near(ha2, ha + by));
        CHECK(near(hb2, hb - by));
        CHECK(near(hc2, hc));
    } else {
        CHECK(near(ha2, ha));
        CHECK(near(hb2, hb + by));
        CHECK(near(hc2, hc - by));
    }
}

int main() {
    std::printf("test_dock_handle_drag\n");
    for (const float middle : { 0.f, 200.f })
        for (const int handle : { 0, 1 })
            for (const float by : { 50.f, -40.f }) drag(middle, handle, by);
    if (g_fails) {
        std::printf("FAILED: %d\n", g_fails);
        return 1;
    }
    std::printf("All dock handle drag tests passed.\n");
    return 0;
}
