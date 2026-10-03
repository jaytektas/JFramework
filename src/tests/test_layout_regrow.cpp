// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// A control the layout cut short of room grows back when the room comes back. A leaf's box was both
// the size its widget asked for and the size the layout gave it, so a window made small and then big
// again kept its buttons and labels at the squeezed size (none at all, made small enough), and they
// were gone. The layout keeps a leaf's natural size apart from what it makes of it
// (JLayoutComponent::naturalWidth), and a widget that sets a new size is still heard.
#include <j/core/JButton.h>
#include <j/core/JContainer.h>
#include <j/core/JLabel.h>
#include <j/core/SceneGraph.h>
#include <cstdio>

using namespace jf;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

static void lay(JSceneGraph& graph, JContainer& c, float w, float h) {
    graph.invalidateNode(c.getNodeId());
    graph.computeLayout(c.getNodeId(), JConstraints{ w, w, h, h });
}

int main() {
    JSceneGraph graph;
    JContainer column(graph, 300.f, 300.f);
    column.setDirection(JFlexDirection::Column)->setAlignItems(JAlignItems::Stretch);
    column.setShrinkStretchyFirst(true);
    auto* row = column.add(std::make_unique<JContainer>(graph, 0.f, 0.f));
    row->setDirection(JFlexDirection::JRow);
    auto* button = row->add(std::make_unique<JButton>(graph, "Locate Board", 120.f, 0.f));
    auto* label  = column.add(std::make_unique<JLabel>(graph, "Side up", 80.f, 0.f));
    auto* list   = column.add(std::make_unique<JContainer>(graph, 300.f, 100.f));
    list->setVSizePolicy(JSizePolicyMode::Expanding, 1);

    lay(graph, column, 300.f, 300.f);
    const JRect b0 = button->bounds(), l0 = label->bounds();
    check(b0.height > 0.f && b0.width == 120.f && l0.width > 0.f, "laid out with room: at their own sizes");

    lay(graph, column, 20.f, 5.f);
    check(button->bounds().height < b0.height && label->bounds().width < l0.width, "made tiny: cut short");

    lay(graph, column, 300.f, 300.f);
    check(button->bounds().height == b0.height && button->bounds().width == b0.width, "room again: the button grows back");
    check(label->bounds().width == l0.width && label->bounds().height == l0.height, "and the label");

    // A size the widget sets itself is its new natural size, squeezed or not.
    lay(graph, column, 20.f, 5.f);
    button->setSize(150.f, b0.height);
    lay(graph, column, 300.f, 300.f);
    check(button->bounds().width == 150.f, "a size its widget sets is kept");

    std::printf(fails ? "=== %d FAILED ===\n" : "=== controls grow back ===\n", fails);
    return fails ? 1 : 0;
}
