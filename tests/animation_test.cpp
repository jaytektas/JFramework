// animation_test.cpp — headless (no window) tests for the JFramework animation system.
//
// Build:
//   g++ -std=c++20 -Iinclude -Ithird_party tests/animation_test.cpp -o /tmp/anim_test
// Runs entirely on the CPU; prints PASS/FAIL per case; exits non-zero on any failure.

#include <j/core/Easing.h>
#include <j/core/Animation.h>

#include <cmath>
#include <cstdio>
#include <string>

using namespace jf;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static bool approx(float a, float b, float eps = 0.001f) {
    return std::fabs(a - b) <= eps;
}

int main() {
    // -----------------------------------------------------------------
    // 1. JAnimation 0->100 over 200ms linear: advance 100ms -> ~50.
    // -----------------------------------------------------------------
    {
        JAnimation a(0.0f, 100.0f, 200.0f, JEasing::Linear);
        int valueChangedCount = 0;
        float lastValue = -1.0f;
        a.onValueChanged.connect([&](float v){ ++valueChangedCount; lastValue = v; });
        int finishedCount = 0;
        a.onFinished.connect([&](){ ++finishedCount; });

        float half = a.advance(100.0f);
        check("linear 0->100 @100ms is ~50", approx(half, 50.0f, 0.01f));
        check("not finished at half", !a.finished());
        check("onValueChanged fired at half", valueChangedCount >= 1 && approx(lastValue, 50.0f, 0.01f));

        float end = a.advance(100.0f);
        check("linear reaches 100 at 200ms", approx(end, 100.0f, 0.01f));
        check("finished() true at end", a.finished());
        check("onFinished fired exactly once", finishedCount == 1);

        // Further advances after completion must not re-fire onFinished / change value.
        a.advance(100.0f);
        check("onFinished not re-fired after completion", finishedCount == 1);
    }

    // -----------------------------------------------------------------
    // 2. Easing endpoints: ease(x,0)==0 and ease(x,1)==1 for several curves.
    // -----------------------------------------------------------------
    {
        JEasing curves[] = {
            JEasing::Linear, JEasing::InQuad, JEasing::OutQuad, JEasing::InOutQuad,
            JEasing::InCubic, JEasing::OutCubic, JEasing::InOutCubic, JEasing::InOutSine,
            JEasing::OutBack, JEasing::OutElastic, JEasing::OutBounce,
        };
        bool allZero = true, allOne = true;
        for (JEasing c : curves) {
            if (!approx(ease(c, 0.0f), 0.0f)) allZero = false;
            if (!approx(ease(c, 1.0f), 1.0f)) allOne  = false;
        }
        check("all curves ease(x,0)==0", allZero);
        check("all curves ease(x,1)==1", allOne);
        // t clamped: out-of-range inputs pin to endpoints.
        check("ease clamps t<0 to start", approx(ease(JEasing::InQuad, -0.5f), 0.0f));
        check("ease clamps t>1 to end",   approx(ease(JEasing::InQuad,  1.5f), 1.0f));
    }

    // -----------------------------------------------------------------
    // 3. Sequential group runs A then B.
    // -----------------------------------------------------------------
    {
        JAnimationGroup grp(JAnimationGroup::Mode::Sequential);
        JAnimation& A = grp.add(0.0f, 10.0f, 100.0f, JEasing::Linear);
        JAnimation& B = grp.add(0.0f, 20.0f, 100.0f, JEasing::Linear);

        grp.advance(50.0f);   // only A should move
        check("seq: A active first", approx(A.value(), 5.0f, 0.01f));
        check("seq: B untouched while A runs", approx(B.value(), 0.0f, 0.01f) && !B.finished());

        grp.advance(50.0f);   // A completes (100ms total)
        check("seq: A finished after its duration", A.finished() && approx(A.value(), 10.0f, 0.01f));

        grp.advance(50.0f);   // now B runs
        check("seq: B advances after A", B.value() > 0.0f);
        check("seq: group not finished mid-B", !grp.finished());

        grp.advance(100.0f);  // finish B
        check("seq: B finished", B.finished() && approx(B.value(), 20.0f, 0.01f));
        check("seq: group finished after A then B", grp.finished());
    }

    // -----------------------------------------------------------------
    // 4. Loop mode wraps.
    // -----------------------------------------------------------------
    {
        JAnimation a(0.0f, 100.0f, 200.0f, JEasing::Linear, JAnimation::Mode::Loop);
        a.advance(100.0f);
        check("loop: 50 at 100ms", approx(a.value(), 50.0f, 0.01f));
        a.advance(150.0f);   // elapsed 250ms -> wraps to 50ms -> value 25
        check("loop: wraps past duration (250ms -> 25)", approx(a.value(), 25.0f, 0.01f));
        check("loop: never finishes", !a.finished());
    }

    // -----------------------------------------------------------------
    // 5. Registry: JAnimator ticks and reaps finished animations.
    // -----------------------------------------------------------------
    {
        JAnimator reg;
        int fin = 0;
        JAnimation& a = reg.animate(0.0f, 1.0f, 100.0f, JEasing::Linear);
        a.onFinished.connect([&](){ ++fin; });
        check("registry: active before tick", reg.hasActive() && reg.activeCount() == 1);
        reg.tick(50.0f);
        check("registry: still active mid-run", reg.hasActive());
        reg.tick(60.0f);   // total 110ms > 100ms -> finishes and is reaped
        check("registry: reaped finished animation", !reg.hasActive() && reg.activeCount() == 0);
        check("registry: onFinished fired once", fin == 1);
    }

    // -----------------------------------------------------------------
    // 6. jAnimator() is a single shared instance.
    // -----------------------------------------------------------------
    {
        jAnimator().clear();
        jAnimator().animate(0.0f, 1.0f, 10.0f, JEasing::Linear);
        check("jAnimator() global holds the animation", jAnimator().activeCount() == 1);
        jAnimator().clear();
    }

    std::printf("\n%s (%d failure%s)\n",
                g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
