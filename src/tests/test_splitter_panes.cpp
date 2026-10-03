// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// A splitter's panes are laid out at their shares, drawn, and given the mouse; a press on the divider is
// the splitter's and drags it. And a column short of room takes it from its expanding children first: a
// row of buttons keeps its height while the list beside it gives.
#include <j/core/Splitter.h>
#include <j/core/JContainer.h>
#include <j/core/JButton.h>
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

    std::puts("1. a splitter's panes");
    {
        JSplitter split(graph, JSplitter::JOrientation::Vertical, 300.f, 405.f);
        JButton top(graph, "Top", 100.f, 20.f), bottom(graph, "Bottom", 100.f, 20.f);
        int clicks = 0;
        bottom.onClicked.connect([&clicks] { ++clicks; });
        split.addPane(&top, 0.5f);
        split.addPane(&bottom, 0.5f);
        JPrimitiveBuffer buf;
        split.populateRenderPrimitives(buf);
        const JRect b = bottom.bounds();
        check(top.bounds().height == 200.f && b.y == 205.f && b.height == 200.f, "laid out at their shares");
        check(!buf.getCommands().empty(), "drawn");
        JWidget::s_leftDown = true;
        split.handleMouseMove(b.x + 10.f, b.y + 10.f);
        split.handleMousePress(b.x + 10.f, b.y + 10.f);
        JWidget::s_leftDown = false;
        split.handleMouseRelease(b.x + 10.f, b.y + 10.f);
        check(clicks == 1, "a pane takes its click");
        // The divider, at 200..205: dragged down 50.
        JWidget::s_leftDown = true;
        split.handleMousePress(150.f, 202.f);
        split.handleMouseMove(150.f, 252.f);
        JWidget::s_leftDown = false;
        split.handleMouseRelease(150.f, 252.f);
        split.layout();
        check(top.bounds().height > 245.f && clicks == 1, "the divider drags, and is not a pane's click");
        check(split.fractions().size() == 2 && split.fractions()[0] > 0.6f, "its fractions say where it is");
    }

    std::puts("2. short of room, the stretchy give first");
    {
        JContainer column(graph, 300.f, 100.f);
        column.setDirection(JFlexDirection::Column);
        auto* list = column.add(std::make_unique<JContainer>(graph, 300.f, 120.f));
        list->setVSizePolicy(JSizePolicyMode::Expanding, 1);
        auto* row = column.add(std::make_unique<JContainer>(graph, 300.f, 30.f));
        row->setVSizePolicy(JSizePolicyMode::Preferred);
        graph.computeLayout(column.getNodeId(), JConstraints{ 300.f, 300.f, 100.f, 100.f });
        check(row->bounds().height == 30.f, "the row keeps its height");
        check(list->bounds().height <= 70.5f, "the list gives the room");
    }

    std::printf(fails ? "=== %d FAILED ===\n" : "=== splitter panes, shrink order ===\n", fails);
    return fails ? 1 : 0;
}
