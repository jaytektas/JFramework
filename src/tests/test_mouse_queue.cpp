// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// test_mouse_queue.cpp — button events are a QUEUE, not a flag.
//
// A boolean latch loses information the moment two things happen between one frame and the next, which on
// a 60 Hz loop is often: the second of two clicks vanishes, a press and its release look simultaneous, and
// a press ends up reported at wherever the pointer travelled to afterwards rather than where it happened.
// Keys have always been queued; these are the three cases that says buttons must be too.
//
// The queue lives in the platform windows, which need a display to construct — so this exercises the same
// structure and rules directly, as the contract every backend implements.

#include <cassert>
#include <cstdio>
#include <deque>
#include <iostream>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

// The platform windows' queue, verbatim in shape and rules.
struct JButtonEvent { bool press; float x, y; bool ctrl, shift, alt; };
struct Window {
    std::deque<JButtonEvent> q;
    float mouseX{0.f}, mouseY{0.f};
    int   taken{0};        // events read since the last poll
    bool  had{false};      // …and whether there was anything to read at that point

    // The frame boundary, exactly as the platform windows implement it: an event that survived a whole
    // frame with nobody reading anything is an event nobody wants. The window's job, not the consumer's —
    // a dialog must not have to know a queue exists in order to keep taking clicks.
    void poll() {
        if (taken == 0 && had && !q.empty()) q.pop_front();
        taken = 0;
        had = !q.empty();
    }

    bool ctrlDown{false};   // what the window reports for the event being handled
    void onPress(float x, float y, bool ctrl = false)   { q.push_back({ true,  x, y, ctrl, false, false }); }
    void onRelease(float x, float y, bool ctrl = false) { q.push_back({ false, x, y, ctrl, false, false }); }
    void onKey()                     { ctrlDown = false; }   // a key event moves the live modifier state on
    void onMotion(float x, float y)  { mouseX = x; mouseY = y; }     // live position, as motion is

    bool take(bool wantPress) {
        if (q.empty() || q.front().press != wantPress) return false;
        ++taken;
        mouseX = q.front().x; mouseY = q.front().y; ctrlDown = q.front().ctrl;
        q.pop_front();
        return true;
    }
    bool consumePress()   { return take(true);  }
    bool consumeRelease() { return take(false); }
};

// A frame polls once: press, then release, in that order — what every caller in the toolkit does.
static void pollFrame(Window& w, int& presses, int& releases, float& lastPressX) {
    if (w.consumePress())   { ++presses;  lastPressX = w.mouseX; }
    if (w.consumeRelease()) { ++releases; }
}

static void test_two_clicks_in_one_frame_are_both_delivered() {
    Window w;
    w.onPress(10.f, 10.f); w.onRelease(10.f, 10.f);     // click one
    w.onPress(20.f, 20.f); w.onRelease(20.f, 20.f);     // click two, same frame
    int p = 0, r = 0; float px = -1.f;
    for (int frame = 0; frame < 4; ++frame) pollFrame(w, p, r, px);
    std::printf("  two clicks in one frame -> %d press(es), %d release(s)\n", p, r);
    CHECK(p == 2);                                      // the flag delivered one and dropped the other
    CHECK(r == 2);
}

static void test_press_position_survives_later_motion() {
    Window w;
    w.onPress(100.f, 50.f);                             // pressed HERE
    w.onMotion(400.f, 300.f);                           // pointer moves on in the same frame
    int p = 0, r = 0; float px = -1.f;
    pollFrame(w, p, r, px);
    std::printf("  press at 100 with motion to 400 -> reported at %.0f\n", px);
    CHECK(p == 1);
    CHECK(px == 100.f);                                 // the flag reported the press at 400: a fast drag's
                                                        // press landing wherever the pointer got to
}

static void test_order_is_preserved() {
    Window w;
    w.onRelease(5.f, 5.f);                              // release of an earlier press, still unread
    w.onPress(9.f, 9.f);                                // then a new press
    // A frame that asks "pressed?" first must NOT be handed the later press ahead of the pending release.
    CHECK(!w.consumePress());
    CHECK(w.consumeRelease());
    CHECK(w.consumePress());
    CHECK(w.mouseX == 9.f);
    std::printf("  a pending release is not jumped over by a later press\n");
}

static void test_empty_queue_reports_nothing() {
    Window w;
    CHECK(!w.consumePress());
    CHECK(!w.consumeRelease());
    w.onPress(1.f, 2.f);
    CHECK(!w.consumeRelease());                         // a press must not answer "released?"
    CHECK(w.consumePress());
    CHECK(!w.consumePress());                           // and it is consumed exactly once
    std::printf("  an empty queue reports nothing; an event is consumed once\n");
}

// THE RULE the queue imposes on its consumers, and the regression that proves why it matters: a window
// that takes presses and never takes releases works for exactly one click, then ignores the pointer for
// ever. It cost an afternoon in a file dialog — one click on "up a directory", then nothing, with the rest
// of the application still responding, which reads as a frozen dialog rather than an unread event.
static void test_a_press_only_consumer_goes_deaf() {
    Window w;
    int presses = 0;
    auto pressOnlyFrame = [&] { if (w.consumePress()) ++presses; };   // the bug: never consumes the release

    w.onPress(10.f, 10.f); w.onRelease(10.f, 10.f);     // click one
    pressOnlyFrame();
    CHECK(presses == 1);                                // the first click lands…
    w.onPress(20.f, 20.f); w.onRelease(20.f, 20.f);     // click two
    for (int i = 0; i < 10; ++i) pressOnlyFrame();
    std::printf("  press-only consumer: %d press(es) delivered from 2 clicks, %zu event(s) stuck\n",
                presses, w.q.size());
    CHECK(presses == 1);                                // …and every one after it is blocked

    // The fix is on the CONSUMER: take the release too, even when you do nothing with it.
    Window v;
    int got = 0;
    auto properFrame = [&] { if (v.consumePress()) ++got; (void)v.consumeRelease(); };
    v.onPress(10.f, 10.f); v.onRelease(10.f, 10.f);
    v.onPress(20.f, 20.f); v.onRelease(20.f, 20.f);
    for (int i = 0; i < 6; ++i) properFrame();
    std::printf("  drain-both consumer: %d press(es) delivered from 2 clicks\n", got);
    CHECK(got == 2);
}

// A consumer that reads presses and never releases — the app window's right button, a file dialog that
// tracks the button with isLeftButtonDown() — must still get every click. It used to get exactly one: the
// first release it ignored blocked every press behind it, so a context menu opened once and never again.
static void test_a_press_only_consumer_still_gets_every_click() {
    Window w;
    int presses = 0;
    auto pressOnlyFrame = [&] { w.poll(); if (w.consumePress()) ++presses; };
    w.onPress(10.f, 10.f); w.onRelease(10.f, 10.f);
    w.onPress(20.f, 20.f); w.onRelease(20.f, 20.f);
    for (int frame = 0; frame < 6; ++frame) pressOnlyFrame();
    std::printf("  press-only consumer: %d press(es) from 2 clicks, %zu left queued\n", presses, w.q.size());
    CHECK(presses == 2);                                // the bug: 1, for ever after
}

// …and a consumer that reads BOTH loses nothing and keeps its ordering: it always takes something while
// the queue is non-empty, so the expiry can never reach one of its events.
static void test_expiry_never_touches_a_well_behaved_consumer() {
    Window w;
    int p = 0, r = 0; float px = -1.f;
    w.onPress(10.f, 10.f); w.onRelease(10.f, 10.f);
    w.onPress(20.f, 20.f); w.onRelease(20.f, 20.f);
    for (int frame = 0; frame < 4; ++frame) { w.poll(); pollFrame(w, p, r, px); }
    std::printf("  drain-both consumer: %d press(es), %d release(s), nothing dropped\n", p, r);
    CHECK(p == 2);
    CHECK(r == 2);
}

// A press carries the modifiers it HAPPENED under. The queue can hold it into a later frame, and any key
// event in between rewrites the window's live modifier state — a modifier key's own press reports the state
// BEFORE itself, so pressing Ctrl writes ctrl=false. Read the live state at consume time and a Ctrl-click
// becomes a plain click: no multi-select, no multi-entry drag.
static void test_modifiers_travel_with_the_event() {
    Window w;
    w.onPress(10.f, 10.f, /*ctrl=*/true);      // clicked WITH ctrl held
    w.onKey();                                 // …then a key event moves the live state on
    CHECK(w.consumePress());
    std::printf("  press made with ctrl, consumed after a key event -> ctrl=%d\n", (int)w.ctrlDown);
    CHECK(w.ctrlDown);                         // the bug: false, so the click lost its modifier
}

int main() {
    std::cout << "platform mouse queue tests\n";
    test_two_clicks_in_one_frame_are_both_delivered();
    test_press_position_survives_later_motion();
    test_order_is_preserved();
    test_empty_queue_reports_nothing();
    test_modifiers_travel_with_the_event();
    test_a_press_only_consumer_still_gets_every_click();
    test_expiry_never_touches_a_well_behaved_consumer();
    test_a_press_only_consumer_goes_deaf();
    std::printf(g_fails ? "mouse queue: FAILED (%d)\n" : "mouse queue: OK\n", g_fails);
    return g_fails ? 1 : 0;
}
