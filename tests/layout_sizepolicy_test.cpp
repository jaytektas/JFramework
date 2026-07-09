// Headless test for the layout size-policy + stretch system: per-axis JSizePolicy modes,
// stretch-weighted distribution of leftover main-axis space, flexible spacers (addStretch/
// addSpacing), and min/max clamping during grow/shrink. Compile:
//   g++ -std=c++20 -I<repo>/include -I<repo>/third_party \
//       tests/layout_sizepolicy_test.cpp -o /tmp/layout_test
// Prints PASS/FAIL per case; exits non-zero on any failure.

#include "j/core/BaseWidgets.h"
#include <cstdio>
#include <cmath>

using namespace jf;

struct Leaf : JWidget {
    Leaf(JSceneGraph& g, const char* n) : JWidget(g, n) {}
    void populateRenderPrimitives(JPrimitiveBuffer&) override {}
};

static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}
static bool feq(float a, float b) { return std::fabs(a - b) < 0.5f; }

// Build a horizontal container of the given inner width; caller adds children first.
static void layoutRow(JSceneGraph& g, NodeId container, float width, float height) {
    g.getLayout(container).direction = JFlexDirection::JRow;
    g.getLayout(container).boundingBox = JRect{0.f, 0.f, width, height};
    // Force the container to exactly `width` x `height` so there is real slack to distribute.
    g.computeLayout(container, JConstraints{width, width, height, height});
}

int main() {
    // ------------------------------------------------------------------
    // 1) Fixed + two Expanding (stretch 1 and 2) split the remainder 1:2.
    // ------------------------------------------------------------------
    {
        JSceneGraph g;
        Leaf box(g, "box");
        Leaf fixed(g, "fixed"), exp1(g, "exp1"), exp2(g, "exp2");
        fixed.setFixedSize(60.f, 20.f);                       // Fixed 60
        exp1.setSize(0.f, 20.f);  exp1.setHSizePolicy(JSizePolicyMode::Expanding, 1);
        exp2.setSize(0.f, 20.f);  exp2.setHSizePolicy(JSizePolicyMode::Expanding, 2);
        g.addChild(box.getNodeId(), fixed.getNodeId());
        g.addChild(box.getNodeId(), exp1.getNodeId());
        g.addChild(box.getNodeId(), exp2.getNodeId());
        layoutRow(g, box.getNodeId(), 300.f, 20.f);

        float fw = fixed.geometry().width, w1 = exp1.geometry().width, w2 = exp2.geometry().width;
        // 300 - 60 = 240 leftover, split 1:2 -> 80 / 160.
        check("Fixed keeps 60px", feq(fw, 60.f));
        check("Expanding stretch 1 gets 80px", feq(w1, 80.f));
        check("Expanding stretch 2 gets 160px", feq(w2, 160.f));
        check("row fully consumed (60+80+160==300)", feq(fw + w1 + w2, 300.f));
    }

    // ------------------------------------------------------------------
    // 2) addStretch pushes a trailing widget to the right edge.
    // ------------------------------------------------------------------
    {
        JSceneGraph g;
        Leaf box(g, "box");
        Leaf left(g, "left"), right(g, "right");
        left.setFixedSize(50.f, 20.f);
        right.setFixedSize(40.f, 20.f);
        g.addChild(box.getNodeId(), left.getNodeId());
        g.addStretch(box.getNodeId());                        // flexible gap between them
        g.addChild(box.getNodeId(), right.getNodeId());
        layoutRow(g, box.getNodeId(), 300.f, 20.f);

        float lx = left.geometry().x;
        JRect rr = right.geometry();
        check("addStretch: left stays at origin", feq(lx, 0.f));
        check("addStretch: right hugs the right edge", feq(rr.x + rr.width, 300.f));
        check("addStretch: right keeps its fixed 40px", feq(rr.width, 40.f));
    }

    // ------------------------------------------------------------------
    // 2b) addSpacing inserts a rigid gap of the requested size.
    // ------------------------------------------------------------------
    {
        JSceneGraph g;
        Leaf box(g, "box");
        Leaf a(g, "a"), b(g, "b");
        a.setFixedSize(30.f, 20.f);
        b.setFixedSize(30.f, 20.f);
        g.addChild(box.getNodeId(), a.getNodeId());
        g.addSpacing(box.getNodeId(), 25.f);                  // rigid 25px gap
        g.addChild(box.getNodeId(), b.getNodeId());
        layoutRow(g, box.getNodeId(), 300.f, 20.f);
        // b should start at a.width(30) + spacing(25) = 55.
        check("addSpacing: rigid 25px gap positions next widget", feq(b.geometry().x, 55.f));
    }

    // ------------------------------------------------------------------
    // 3) Max clamp: an Expanding child with a maximum caps, surplus recirculates to its sibling.
    // ------------------------------------------------------------------
    {
        JSceneGraph g;
        Leaf box(g, "box");
        Leaf capped(g, "capped"), rest(g, "rest");
        capped.setSize(0.f, 20.f); capped.setHSizePolicy(JSizePolicyMode::Expanding, 1);
        capped.setMaximumSize(50.f, 1.0e6f);                  // ceiling 50
        rest.setSize(0.f, 20.f);   rest.setHSizePolicy(JSizePolicyMode::Expanding, 1);
        g.addChild(box.getNodeId(), capped.getNodeId());
        g.addChild(box.getNodeId(), rest.getNodeId());
        layoutRow(g, box.getNodeId(), 300.f, 20.f);
        check("max clamp: capped child stops at 50px", feq(capped.geometry().width, 50.f));
        check("max clamp: surplus flows to sibling (250px)", feq(rest.geometry().width, 250.f));
    }

    // ------------------------------------------------------------------
    // 4) Min clamp: under a deficit, a child never shrinks below its minimum.
    // ------------------------------------------------------------------
    {
        JSceneGraph g;
        Leaf box(g, "box");
        Leaf floored(g, "floored"), other(g, "other");
        floored.setSize(80.f, 20.f); floored.setHSizePolicy(JSizePolicyMode::Expanding, 1);
        floored.setMinimumSize(60.f, 0.f);                   // floor 60
        other.setSize(80.f, 20.f);   other.setHSizePolicy(JSizePolicyMode::Expanding, 1);
        g.addChild(box.getNodeId(), floored.getNodeId());
        g.addChild(box.getNodeId(), other.getNodeId());
        // Container 40px wide vs 160px of content -> heavy deficit forces shrink to the floors.
        layoutRow(g, box.getNodeId(), 40.f, 20.f);
        check("min clamp: floored child holds its 60px minimum", feq(floored.geometry().width, 60.f));
        check("min clamp: unfloored child shrinks toward 0", other.geometry().width <= 0.5f);
    }

    // ------------------------------------------------------------------
    // 5) Legacy flexGrow still distributes leftover for callers that never set a policy.
    // ------------------------------------------------------------------
    {
        JSceneGraph g;
        Leaf box(g, "box");
        Leaf a(g, "a"), b(g, "b");
        a.setSize(0.f, 20.f); b.setSize(0.f, 20.f);
        g.getLayout(a.getNodeId()).flexGrow = 1.0f;          // old API, no size policy
        g.getLayout(b.getNodeId()).flexGrow = 3.0f;
        g.addChild(box.getNodeId(), a.getNodeId());
        g.addChild(box.getNodeId(), b.getNodeId());
        layoutRow(g, box.getNodeId(), 400.f, 20.f);
        check("legacy flexGrow 1:3 split preserved", feq(a.geometry().width, 100.f) && feq(b.geometry().width, 300.f));
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "SOME TESTS FAILED" : "ALL TESTS PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
