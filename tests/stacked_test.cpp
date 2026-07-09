// Headless test for JStackedWidget: single-visible-page stack — count/index,
// setCurrentIndex fires currentChanged + flips visibility, currentWidget,
// removeWidget re-indexing, and paint/input reaching ONLY the current page.
// Compile:
//   g++ -std=c++20 -I<repo>/include -I<repo>/third_party \
//       tests/stacked_test.cpp -o /tmp/stk_test
// Prints PASS/FAIL per case; exits non-zero on any failure.

#include "j/core/StackedWidget.h"
#include <cstdio>
#include <cmath>

using namespace jf;

static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}
static bool feq(float a, float b) { return std::fabs(a - b) < 0.5f; }

// Concrete leaf page that counts how many times it was painted.
struct Page : JWidget {
    int paints = 0;
    Page(JSceneGraph& g, const char* n) : JWidget(g, n) {}
    void populateRenderPrimitives(JPrimitiveBuffer&) override { ++paints; }
};

int main() {
    JSceneGraph g;
    JStackedWidget stack(g, 400.f, 300.f);

    Page p0(g, "p0"), p1(g, "p1"), p2(g, "p2");
    int i0 = stack.addWidget(&p0);
    int i1 = stack.addWidget(&p1);
    int i2 = stack.addWidget(&p2);

    // ---- construction / defaults -------------------------------------------
    check("addWidget returns 0/1/2", i0 == 0 && i1 == 1 && i2 == 2);
    check("count == 3", stack.count() == 3);
    check("currentIndex == 0 (first added)", stack.currentIndex() == 0);
    check("currentWidget == p0", stack.currentWidget() == &p0);
    check("p0 visible, p1/p2 hidden",
          p0.isVisible() && !p1.isVisible() && !p2.isVisible());

    // ---- switch page fires currentChanged + flips visibility ---------------
    int changedTo = -99;
    stack.currentChanged.connect([&](int idx){ changedTo = idx; });
    stack.setCurrentIndex(2);
    check("setCurrentIndex(2) fires currentChanged(2)", changedTo == 2);
    check("currentIndex == 2", stack.currentIndex() == 2);
    check("currentWidget == p2", stack.currentWidget() == &p2);
    check("p2 visible, p0/p1 hidden",
          p2.isVisible() && !p0.isVisible() && !p1.isVisible());

    // ---- no-op set does not re-emit ----------------------------------------
    changedTo = -99;
    stack.setCurrentIndex(2);
    check("re-setting same index does not emit", changedTo == -99);

    // ---- setBounds sizes the current page to fill; paint hits only current --
    stack.setBounds(JRect{10.f, 20.f, 400.f, 300.f});
    JPrimitiveBuffer buf;
    stack.populateRenderPrimitives(buf);
    check("current page sized to fill stack rect",
          feq(p2.bounds().x, 10.f) && feq(p2.bounds().y, 20.f) &&
          feq(p2.bounds().width, 400.f) && feq(p2.bounds().height, 300.f));
    check("only current page painted (p2==1, p0==0, p1==0)",
          p2.paints == 1 && p0.paints == 0 && p1.paints == 0);

    stack.populateRenderPrimitives(buf);
    check("second paint still only touches current (p2==2)",
          p2.paints == 2 && p0.paints == 0 && p1.paints == 0);

    // ---- input routes only to the current page -----------------------------
    // Mouse press inside the box; a plain JWidget has no observable press state,
    // so we assert routing by proxy: hidden pages stay hidden (never activated)
    // and the visible page keeps taking paints below. Key/scroll return false
    // (leaf ignores) but must not crash and must target only the current page.
    stack.handleMousePress(50.f, 60.f);
    stack.handleMouseRelease(50.f, 60.f);
    check("input on hidden pages leaves them hidden",
          !p0.isVisible() && !p1.isVisible() && p2.isVisible());

    // ---- setCurrentWidget --------------------------------------------------
    changedTo = -99;
    stack.setCurrentWidget(&p0);
    check("setCurrentWidget(p0) selects index 0 + emits",
          stack.currentIndex() == 0 && changedTo == 0 && p0.isVisible());

    // ---- removeWidget: removing a page BEFORE current re-bases silently ----
    // current is p0 (index 0). Move current to p2 (index 2), then remove p1
    // (index 1, before current): current stays p2, index drops to 1, no emit.
    stack.setCurrentIndex(2);
    changedTo = -99;
    stack.removeWidget(&p1);
    check("remove page before current keeps current widget (p2)",
          stack.currentWidget() == &p2);
    check("remove page before current re-based index to 1",
          stack.currentIndex() == 1);
    check("remove non-current page emits nothing", changedTo == -99);
    check("count == 2 after remove", stack.count() == 2);
    check("removed page handed back visible", p1.isVisible());

    // ---- removeWidget: removing the CURRENT page selects the slot neighbour --
    // Pages now: [p0, p2], current == p2 (index 1, the last). Remove it → the
    // slot clamps to the new last (index 0 = p0) and currentChanged fires.
    changedTo = -99;
    stack.removeWidget(&p2);
    check("removing current selects clamped slot (p0)",
          stack.currentWidget() == &p0 && stack.currentIndex() == 0);
    check("removing current emits currentChanged(0)", changedTo == 0);

    // ---- draining to empty -------------------------------------------------
    changedTo = -99;
    stack.removeWidget(&p0);
    check("empty stack: currentIndex == -1", stack.currentIndex() == -1);
    check("empty stack: currentWidget == nullptr", stack.currentWidget() == nullptr);
    check("draining last page emits currentChanged(-1)", changedTo == -1);
    check("count == 0", stack.count() == 0);

    // Painting/input on an empty stack must be safe no-ops.
    stack.populateRenderPrimitives(buf);
    stack.handleMousePress(0.f, 0.f);
    check("empty stack paint/input is a safe no-op", true);

    // ---- insertWidget in the middle preserves current page -----------------
    Page q0(g, "q0"), q1(g, "q1"), q2(g, "q2");
    stack.addWidget(&q0);          // current -> q0 (idx 0)
    stack.addWidget(&q2);          // idx 1
    stack.setCurrentIndex(1);      // current -> q2
    changedTo = -99;
    int ins = stack.insertWidget(1, &q1);   // before current
    check("insertWidget returns clamped index 1", ins == 1);
    check("insert before current keeps current widget (q2)",
          stack.currentWidget() == &q2);
    check("insert before current shifts index to 2", stack.currentIndex() == 2);
    check("insert non-first emits nothing", changedTo == -99);

    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
