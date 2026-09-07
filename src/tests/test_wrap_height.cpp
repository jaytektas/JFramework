// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// A LIST THAT WRAPS INTO COLUMNS IS SHORTER, and the window has to be told.
//
// A popup too tall for the screen re-lays itself into columns (PopupWindow::wrapToHeight). The shape
// changes, so the height must too — two columns of thirty items are half as tall as one column of
// sixty, and the window is supposed to shrink onto them.
//
// It did not. _computeMinSize accumulates with max(), which is what lets a widget declare a floor that
// a measurement can raise but never lower; the cost is that a container which has changed shape keeps
// the minimum of the shape it no longer has. The single column's height was still sitting in the root
// when the columns were measured, so the popup kept it — clamped to the work area, which on a 1080p
// screen is the whole screen with two-thirds of it empty.
//
// This drives the same sequence the popup does, on the graph alone: measure a column, switch to a
// column-major grid, measure again.
#include <j/core/SceneGraph.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace jf;

static int fails = 0;
static void check(const char* what, bool ok, const std::string& d = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, d.empty() ? "" : " — ", d.c_str());
    if (!ok) ++fails;
}

int main() {
    constexpr int kItems = 60, kItemH = 20, kCols = 2;

    JSceneGraph g;
    const NodeId root = g.createNode("popup-root");
    {
        auto& L = g.getLayout(root);
        L.mode = JLayoutMode::Flex;
        L.direction = JFlexDirection::Column;
        L.gap = 0.f;
    }
    for (int i = 0; i < kItems; ++i) {
        const NodeId k = g.createNode("item");
        auto& kl = g.getLayout(k);
        kl.minWidth = 100.f; kl.minHeight = float(kItemH);
        kl.boundingBox.width = 100.f; kl.boundingBox.height = float(kItemH);
        g.addChild(root, k);
    }

    // 1 — the single column, as computeNaturalHeight() measures it.
    g.computeMinSize(root);
    const float tall = g.getLayoutConst(root).minHeight;
    check("a column of 60 items measures 60 items tall", tall == float(kItems * kItemH),
          std::to_string(tall));

    // 2 — the same children, re-laid into columns, WITHOUT clearing the measured minimum. This is the
    //     bug, pinned: the grid measures 30 rows and the max() keeps the 60 from before.
    {
        auto& L = g.getLayout(root);
        L.mode = JLayoutMode::Grid;
        L.columns = kCols;
        L.gridColumnMajor = true;
    }
    g.computeMinSize(root);
    check("…and re-measuring alone does NOT shrink it (max() is a high-water mark)",
          g.getLayoutConst(root).minHeight == tall, std::to_string(g.getLayoutConst(root).minHeight));

    // 3 — with the measured minimum dropped first, the height is the tallest COLUMN.
    g.resetMinSize(root);
    g.computeMinSize(root);
    const float wrapped = g.getLayoutConst(root).minHeight;
    check("resetMinSize + re-measure gives the height of one column",
          wrapped == float((kItems / kCols) * kItemH),
          std::to_string(wrapped) + " (expected " + std::to_string((kItems / kCols) * kItemH) + ")");
    check("…which is genuinely shorter than the single column", wrapped < tall);

    // 4 — an ODD count still fits: 61 items in 2 columns is 31 rows, not 30. A popup sized to 30 would
    //     clip its last entry, which is the failure this arithmetic exists to avoid.
    {
        const NodeId k = g.createNode("item");
        auto& kl = g.getLayout(k);
        kl.minWidth = 100.f; kl.minHeight = float(kItemH);
        kl.boundingBox.width = 100.f; kl.boundingBox.height = float(kItemH);
        g.addChild(root, k);
    }
    g.resetMinSize(root);
    g.computeMinSize(root);
    check("61 items in 2 columns is 31 rows tall, not 30",
          g.getLayoutConst(root).minHeight == float(31 * kItemH),
          std::to_string(g.getLayoutConst(root).minHeight));

    std::printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
