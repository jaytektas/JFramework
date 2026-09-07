// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// test_scroll_track.cpp — the scrollbar of a scrolling list belongs to the scrollbar.
//
// A combo box's dropdown is a JScrollArea full of JPopupItems, and the row behind the scrollbar is NOT
// the row you are pointing at. Three things follow, and each of them was wrong at some point:
//
//   * a press on the bar must not reach the item behind it (that picked an entry),
//   * a press on the empty track must PAGE, as every scrollbar in every toolkit does (it used to arm a
//     drag that never moved, so the list just sat there),
//   * a host that highlights whatever the pointer is over must be able to ask whether the pointer is on
//     the bar — in a combo list that highlight is the selection Enter commits.

#include <j/core/JScrollArea.h>
#include <j/core/JPopupItem.h>
#include <j/core/SceneGraph.h>
#include <j/graphics/RenderPrimitive.h>

#include <cmath>
#include <cstdio>
#include <iostream>

using namespace jf;

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

static constexpr float kW = 200.f, kH = 100.f, kItemH = 28.f;

struct List {
    JSceneGraph graph;
    JScrollArea area{graph, kW, kH};
    int activations = 0;

    List() {
        area.setBounds({ 0.f, 0.f, kW, kH });
        area.setContentPadding(0.f, 0.f, 0.f);
        for (int i = 0; i < 20; ++i) {
            auto* pi = area.addChildWidget(std::make_unique<JPopupItem>(graph, "row " + std::to_string(i), kW, kItemH));
            pi->onActivated.connect([this] { ++activations; });
        }
        layout();
    }
    // A scroll area positions its children while PAINTING, so a test that clicks one has to paint first —
    // otherwise every row is still at the origin and a single click hits all of them.
    void layout() { JPrimitiveBuffer buf; area.populateRenderPrimitives(buf); }
    // A click = press then release at the same point, which is what a user does.
    void click(float x, float y) { area.handleMousePress(x, y); area.handleMouseRelease(x, y); }
};

static void test_the_bar_is_where_the_host_thinks_it_is() {
    List l;
    CHECK(l.area.pointInScrollbar(kW - 8.f, kH * 0.5f));    // over the bar
    CHECK(l.area.pointInScrollbar(kW - 1.f, kH * 0.5f));    // right up to the edge
    CHECK(!l.area.pointInScrollbar(kW * 0.5f, kH * 0.5f));  // over the content
    CHECK(!l.area.pointInScrollbar(kW - 8.f, kH + 40.f));   // outside the area entirely
    CHECK(!l.area.pointInScrollbar(kW - 14.f, kH * 0.5f));  // just LEFT of the bar: content, not margin
    std::printf("  scrollbar column reported at x >= %.0f of %.0f\n", kW - 12.f, kW);
}

// THE THUMB YOU GRAB IS THE THUMB YOU SEE. The painter measured the content one way (its own padding and
// gap) and every hit test measured it another (the DEFAULT padding, written out as "12 + sum(h + 6)"), so
// on a list that sets its own — which a dropdown does, because a gap between rows swallows clicks — the
// two disagreed. The hit-tested thumb was shorter than the drawn one, and pressing the lower part of the
// visible thumb was read as a click on the empty track: the list PAGED instead of dragging.
static void test_the_painted_thumb_is_the_one_you_can_grab() {
    // Tall enough that the thumb is not clamped to its 20px minimum, or the two measurements agree by
    // accident and the test proves nothing.
    constexpr float kTallH = 300.f;
    JSceneGraph graph;
    JScrollArea area{graph, kW, kTallH};
    area.setBounds({ 0.f, 0.f, kW, kTallH });
    area.setContentPadding(0.f, 0.f, 0.f);
    for (int i = 0; i < 20; ++i)
        area.addChildWidget(std::make_unique<JPopupItem>(graph, "row " + std::to_string(i), kW, kItemH));
    { JPrimitiveBuffer buf; area.populateRenderPrimitives(buf); }

    // What the PAINTER draws: content = 20 * 28 (no padding, no gaps), thumb = track * (view / content).
    const float content = 20.f * kItemH;
    const float trackH  = kTallH - 4.f;
    const float thumbH  = trackH * (kTallH / content);
    const float thumbLo = 2.f, thumbHi = 2.f + thumbH;

    // The button is HELD for the length of a drag. JScrollArea reads that from JWidget::s_leftDown (the
    // app polls the server's real button mask each frame, which stays honest under a popup's pointer
    // grab); a test drives it by hand or the drag lets go on its first move.
    JWidget::s_leftDown = true;
    struct Restore { ~Restore() { JWidget::s_leftDown = false; } } restore;

    // Press near the BOTTOM of the drawn thumb — the part the old maths thought was empty track.
    area.handleMousePress(kW - 6.f, thumbHi - 4.f);
    std::printf("  press at %.0f of a thumb drawn %.0f..%.0f -> scrollY %.0f, dragging %d\n",
                thumbHi - 4.f, thumbLo, thumbHi, area.scrollY(), int(area.isDraggingScroll()));
    CHECK(area.scrollY() == 0.f);          // it grabbed the thumb; it did not page
    CHECK(area.isDraggingScroll());

    // …and the drag tracks the pointer: halfway down the thumb's travel is halfway down the content.
    const float maxScroll  = content - kTallH;
    const float thumbRange = trackH - thumbH;
    area.handleMouseMove(kW - 6.f, thumbHi - 4.f + thumbRange * 0.5f);
    std::printf("  thumb dragged to mid-track -> scrollY %.0f (want %.0f)\n", area.scrollY(), maxScroll * 0.5f);
    CHECK(std::abs(area.scrollY() - maxScroll * 0.5f) < 1.0f);
    area.handleMouseRelease(kW - 6.f, thumbHi - 4.f + thumbRange * 0.5f);
}

// The wheel stops where the rows do. It clamped against the believed content height, so on a list with its
// own padding it kept "scrolling" into 264px of nothing that the next paint silently took back.
static void test_the_wheel_stops_at_the_last_row() {
    List l;
    for (int i = 0; i < 100; ++i) l.area.handleScroll(kW * 0.5f, kH * 0.5f, -1.f);
    const float maxScroll = 20.f * kItemH - kH;             // content (no padding/gap) less one viewport
    std::printf("  wheeled to the end -> scrollY %.0f (want %.0f)\n", l.area.scrollY(), maxScroll);
    CHECK(l.area.scrollY() == maxScroll);
}

static void test_clicking_the_track_pages_and_picks_nothing() {
    List l;
    CHECK(l.area.scrollY() == 0.f);
    l.click(kW - 8.f, kH - 6.f);                            // empty track, below the thumb
    std::printf("  click below the thumb -> scrollY %.0f, %d activation(s)\n", l.area.scrollY(), l.activations);
    CHECK(l.area.scrollY() > 0.f);                          // it PAGED (was: nothing at all)
    CHECK(l.activations == 0);                              // …and picked nothing behind the bar

    const float paged = l.area.scrollY();
    l.click(kW - 8.f, 4.f);                                 // empty track, above the thumb
    CHECK(l.area.scrollY() < paged);                        // pages back up
    CHECK(l.activations == 0);
}

static void test_a_click_on_the_content_still_picks() {
    List l;
    l.click(kW * 0.5f, kItemH * 0.5f);                      // first row, well clear of the bar
    std::printf("  click on a row -> %d activation(s)\n", l.activations);
    CHECK(l.activations == 1);
}

// A list SHORT enough not to scroll has no bar, so its right-hand strip is ordinary content — not a dead
// margin where clicks vanish.
static void test_a_short_list_has_no_dead_strip() {
    JSceneGraph graph;
    JScrollArea area{graph, kW, kH};
    area.setBounds({ 0.f, 0.f, kW, kH });
    area.setContentPadding(0.f, 0.f, 0.f);
    int hits = 0;
    auto* pi = area.addChildWidget(std::make_unique<JPopupItem>(graph, "only row", kW, kItemH));
    pi->onActivated.connect([&hits] { ++hits; });
    { JPrimitiveBuffer buf; area.populateRenderPrimitives(buf); }

    CHECK(!area.pointInScrollbar(kW - 8.f, kItemH * 0.5f));  // nothing to scroll → no bar
    area.handleMousePress(kW - 8.f, kItemH * 0.5f);
    area.handleMouseRelease(kW - 8.f, kItemH * 0.5f);
    std::printf("  short list, click near the right edge -> %d activation(s)\n", hits);
    CHECK(hits == 1);
}

int main() {
    std::cout << "JScrollArea scrollbar-track tests\n";
    test_the_bar_is_where_the_host_thinks_it_is();
    test_clicking_the_track_pages_and_picks_nothing();
    test_a_click_on_the_content_still_picks();
    test_a_short_list_has_no_dead_strip();
    test_the_painted_thumb_is_the_one_you_can_grab();
    test_the_wheel_stops_at_the_last_row();
    std::printf(g_fails ? "scroll track: FAILED (%d)\n" : "scroll track: OK\n", g_fails);
    return g_fails ? 1 : 0;
}
