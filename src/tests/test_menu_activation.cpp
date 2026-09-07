// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// test_menu_activation.cpp — a menu item fires when the button comes UP, on the item under the cursor.
//
// It used to fire on the press, and that is not a detail of timing: a dropdown opens UNDER the cursor
// on the opening press, so its first item sits exactly where the pointer already is. The button coming
// up then chose an entry the user had not yet seen — which is why the first item in a combo could not
// be picked, and why the whole thing behaved differently depending on how the grab and the frame
// boundary happened to line up. Firing on press also means a click can never be cancelled: press an
// item, slide off it, let go, and it has already run.
//
// Three things are pinned here:
//   1. a press alone does NOT activate — it only arms and shows pressed;
//   2. release inside activates exactly once;
//   3. release OUTSIDE the item does not activate at all (the cancel every toolkit offers).
// The fourth part of the fix — swallowing the release that belongs to the opening click — lives in
// JMenuManager's poll loop, which needs a live X connection and is not reachable from a unit test.

#include <j/core/MenuSystem.h>
#include <j/core/SceneGraph.h>

#include <cstdio>

using namespace jf;

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

int main() {
    std::printf("=== menu activation ===\n");

    JSceneGraph g;
    JMenuItem item(g, "Fuel Pump");
    auto& l = g.getLayout(item.getNodeId());
    l.boundingBox = { 0.f, 0.f, 180.f, 28.f };

    int fired = 0;
    item.onTriggered.connect([&fired] { ++fired; });

    // 1. The press arms it and nothing more.
    item.handleMousePress(90.f, 14.f);
    CHECK(fired == 0);
    CHECK(item.getState() == JWidgetState::Pressed);

    // 2. The release inside fires it, once.
    item.handleMouseRelease(90.f, 14.f);
    CHECK(fired == 1);

    // 3. Press, slide off, release: cancelled.
    item.handleMousePress(90.f, 14.f);
    item.handleMouseMove(90.f, 400.f);
    item.handleMouseRelease(90.f, 400.f);
    CHECK(fired == 1);

    // 4. Release with no press of its own still fires — a menu is also operated press-drag-release, and
    //    the item the button comes up over never saw the press that started the gesture.
    item.handleMouseRelease(90.f, 14.f);
    CHECK(fired == 2);

    // 5. A disabled item does neither.
    JMenuItem off(g, "Taken");
    auto& l2 = g.getLayout(off.getNodeId());
    l2.boundingBox = { 0.f, 0.f, 180.f, 28.f };
    int off_fired = 0;
    off.onTriggered.connect([&off_fired] { ++off_fired; });
    off.setEnabled(false);
    off.handleMousePress(90.f, 14.f);
    off.handleMouseRelease(90.f, 14.f);
    CHECK(off_fired == 0);

    std::printf(g_fails ? "  %d FAILED\n" : "  all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
