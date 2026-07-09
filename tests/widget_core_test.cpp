// Headless test for JWidget core-API additions (geometry / constraints / focus
// policy / visibility / z-order). Compile:
//   g++ -std=c++20 -I<repo>/include tests/widget_core_test.cpp -o /tmp/widget_core_test
// Prints PASS/FAIL per case; exits non-zero on any failure.

#include "j/core/BaseWidgets.h"
#include <cstdio>
#include <cmath>

using namespace jf;

// Trivial concrete widgets for the test.
struct TestW : JControl {
    TestW(JSceneGraph& g) : JControl(g, "TestW") {}
    void populateRenderPrimitives(JPrimitiveBuffer&) override {}
};
struct PlainW : JWidget {
    PlainW(JSceneGraph& g) : JWidget(g, "PlainW") {}
    void populateRenderPrimitives(JPrimitiveBuffer&) override {}
};

static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}
static bool feq(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
    JSceneGraph graph;

    // 1) setGeometry / geometry round-trip.
    {
        TestW w(graph);
        w.setGeometry(JRect{10.f, 20.f, 120.f, 40.f});
        JRect g = w.geometry();
        check("setGeometry/geometry round-trips",
              feq(g.x, 10.f) && feq(g.y, 20.f) && feq(g.width, 120.f) && feq(g.height, 40.f));
    }

    // 2) setFixedSize clamps a larger setSize.
    {
        TestW w(graph);
        w.setFixedSize(50.f, 30.f);
        w.setSize(400.f, 300.f);          // should be clamped back to 50x30
        JRect g = w.geometry();
        check("setFixedSize clamps a larger setSize",
              feq(g.width, 50.f) && feq(g.height, 30.f));
    }

    // 2b) setMaximumSize clamps; setMinimumSize raises.
    {
        TestW w(graph);
        w.setMaximumSize(100.f, 80.f);
        w.setSize(999.f, 999.f);
        JRect g = w.geometry();
        bool maxOk = feq(g.width, 100.f) && feq(g.height, 80.f);
        w.setMinimumSize(60.f, 40.f);
        w.setSize(1.f, 1.f);
        g = w.geometry();
        bool minOk = feq(g.width, 60.f) && feq(g.height, 40.f);
        check("setMaximumSize/setMinimumSize clamp setSize", maxOk && minOk);
    }

    // 3) focusPolicy NoFocus -> not focusable; StrongFocus -> focusable.
    {
        TestW w(graph);
        // JControl defaults to StrongFocus.
        bool defaultFocusable = w.isFocusable();
        w.setFocusPolicy(JFocusPolicy::NoFocus);
        bool noFocus = !w.isFocusable() && !w.acceptsClickFocus();
        w.setFocusPolicy(JFocusPolicy::StrongFocus);
        bool strong = w.isFocusable() && w.acceptsClickFocus();
        check("focusPolicy NoFocus->false, StrongFocus->true",
              defaultFocusable && noFocus && strong);
    }

    // 3b) plain JWidget defaults to NoFocus (not focusable).
    {
        PlainW w(graph);
        check("plain JWidget defaults NoFocus (not focusable)", !w.isFocusable());
    }

    // 4) setHidden hides / isHidden reflects.
    {
        TestW w(graph);
        bool visInit = w.isVisible() && !w.isHidden();
        w.setHidden(true);
        bool hidden = !w.isVisible() && w.isHidden();
        w.setHidden(false);
        bool shown = w.isVisible() && !w.isHidden();
        check("setHidden hides / isHidden reflects", visInit && hidden && shown);
    }

    // 5) raise() puts a widget last in s_activeWidgets; lower() puts it first.
    {
        TestW a(graph), b(graph), c(graph);
        a.raise();
        bool raised = !JWidget::s_activeWidgets.empty() &&
                      JWidget::s_activeWidgets.back() == &a;
        a.lower();
        bool lowered = !JWidget::s_activeWidgets.empty() &&
                       JWidget::s_activeWidgets.front() == &a;
        check("raise()=last, lower()=first in s_activeWidgets", raised && lowered);
    }

    // extra) mapToGlobal honours s_screenOrigin hook.
    {
        TestW w(graph);
        JWidget::s_screenOrigin = []{ return std::pair<float,float>{100.f, 200.f}; };
        auto [gx, gy] = w.mapToGlobal(5.f, 6.f);
        auto [lx, ly] = w.mapFromGlobal(gx, gy);
        JWidget::s_screenOrigin = nullptr;
        check("mapToGlobal/mapFromGlobal honour s_screenOrigin",
              feq(gx, 105.f) && feq(gy, 206.f) && feq(lx, 5.f) && feq(ly, 6.f));
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILURES" : "ALL PASS",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
