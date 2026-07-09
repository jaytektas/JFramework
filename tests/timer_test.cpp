// timer_test.cpp — headless (no window, no sleeping) tests for the frame-ticked
// timer system (FrameTimer.h). JFrameTimerRegistry::tick() is driven manually with
// fake dt, exactly as the runner would at real frame rate.
//
// Build:
//   g++ -std=c++20 -Iinclude -Ithird_party tests/timer_test.cpp -o /tmp/tmr_test
// Prints PASS/FAIL per case; exits non-zero on any failure.

#include <j/core/FrameTimer.h>

#include <cstdio>
#include <string>

using namespace jf;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

int main() {
    auto& reg = jTimers();
    reg.clear();

    // -----------------------------------------------------------------
    // 1. Repeating 100ms timer: fires at cumulative 100ms, again at 200ms,
    //    never early. Drive in 10x10ms frames per interval.
    // -----------------------------------------------------------------
    {
        reg.clear();
        JFrameTimer t;
        t.setInterval(100.0f);
        int fires = 0;
        t.timeout.connect([&]{ ++fires; });
        t.start();

        check("repeat: active after start", t.isActive());

        for (int i = 0; i < 9; ++i) reg.tick(10.0f);   // 90ms total
        check("repeat: no fire before 100ms", fires == 0);
        reg.tick(10.0f);                                // 100ms
        check("repeat: fires once at 100ms", fires == 1);

        for (int i = 0; i < 9; ++i) reg.tick(10.0f);   // 190ms
        check("repeat: still one fire at 190ms", fires == 1);
        reg.tick(10.0f);                                // 200ms
        check("repeat: fires again at 200ms (2 total)", fires == 2);
        check("repeat: still active", t.isActive());
        t.stop();
        check("repeat: inactive after stop", !t.isActive());
    }

    // -----------------------------------------------------------------
    // 2. Single-shot: fires exactly once, then isActive()==false; no more.
    // -----------------------------------------------------------------
    {
        reg.clear();
        JFrameTimer t;
        t.setInterval(50.0f);
        t.setSingleShot(true);
        int fires = 0;
        t.timeout.connect([&]{ ++fires; });
        t.start();

        reg.tick(20.0f);
        check("single-shot: no fire at 20ms", fires == 0 && t.isActive());
        reg.tick(40.0f);                                // 60ms >= 50ms
        check("single-shot: fired once at 60ms", fires == 1);
        check("single-shot: inactive after fire", !t.isActive());
        reg.tick(1000.0f);
        check("single-shot: never fires again", fires == 1);
    }

    // -----------------------------------------------------------------
    // 3. JFrameTimer::singleShot(50, cb): fires cb once after 50ms, self-reaped.
    // -----------------------------------------------------------------
    {
        reg.clear();
        int fires = 0;
        JFrameTimer::singleShot(50.0f, [&]{ ++fires; });
        check("static singleShot: registered active", reg.activeCount() == 1);

        reg.tick(40.0f);
        check("static singleShot: no fire at 40ms", fires == 0);
        reg.tick(20.0f);                                // 60ms
        check("static singleShot: fired once", fires == 1);
        check("static singleShot: self-reaped", reg.activeCount() == 0);
        reg.tick(1000.0f);
        check("static singleShot: never again", fires == 1);
    }

    // -----------------------------------------------------------------
    // 4. Overshoot / no drift: one 250ms tick on a 100ms repeating timer
    //    fires twice (100, 200) and re-arms with 50ms left — not 100.
    // -----------------------------------------------------------------
    {
        reg.clear();
        JFrameTimer t;
        t.setInterval(100.0f);
        int fires = 0;
        t.timeout.connect([&]{ ++fires; });
        t.start();

        reg.tick(250.0f);
        check("overshoot: 250ms tick fires twice", fires == 2);
        check("overshoot: re-armed carrying overshoot (50ms left)",
              t.remaining() > 49.9f && t.remaining() < 50.1f);
        // Next 50ms completes the third interval exactly (300ms cumulative).
        reg.tick(50.0f);
        check("overshoot: third fire at 300ms (no drift)", fires == 3);
    }

    // -----------------------------------------------------------------
    // 5. jPostToNextFrame: runs on the next tick, exactly once, not before/after.
    // -----------------------------------------------------------------
    {
        reg.clear();
        int runs = 0;
        jPostToNextFrame([&]{ ++runs; });
        check("post: not run before a tick", runs == 0);
        reg.tick(16.0f);
        check("post: runs on the next tick", runs == 1);
        reg.tick(16.0f);
        check("post: not run again on later ticks", runs == 1);
    }

    // -----------------------------------------------------------------
    // 6. A callback posted DURING a tick lands on the FOLLOWING tick.
    // -----------------------------------------------------------------
    {
        reg.clear();
        int outer = 0, inner = 0;
        jPostToNextFrame([&]{ ++outer; jPostToNextFrame([&]{ ++inner; }); });
        reg.tick(16.0f);
        check("post: re-post deferred to next frame (inner=0 this tick)", outer == 1 && inner == 0);
        reg.tick(16.0f);
        check("post: re-posted callback runs next tick (inner=1)", inner == 1);
    }

    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
