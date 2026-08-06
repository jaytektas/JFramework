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
struct JButtonEvent { bool press; float x, y; };
struct Window {
    std::deque<JButtonEvent> q;
    float mouseX{0.f}, mouseY{0.f};

    void onPress(float x, float y)   { q.push_back({ true,  x, y }); }
    void onRelease(float x, float y) { q.push_back({ false, x, y }); }
    void onMotion(float x, float y)  { mouseX = x; mouseY = y; }     // live position, as motion is

    bool take(bool wantPress) {
        if (q.empty() || q.front().press != wantPress) return false;
        mouseX = q.front().x; mouseY = q.front().y;
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

int main() {
    std::cout << "platform mouse queue tests\n";
    test_two_clicks_in_one_frame_are_both_delivered();
    test_press_position_survives_later_motion();
    test_order_is_preserved();
    test_empty_queue_reports_nothing();
    std::printf(g_fails ? "mouse queue: FAILED (%d)\n" : "mouse queue: OK\n", g_fails);
    return g_fails ? 1 : 0;
}
