// test_dock_split.cpp — splitting a dock area must give BOTH sides room.
//
// A dock dropped onto an existing dock with a left/right bias splits that region in two. The dropped
// dock keeps its retained pixel size where the region can afford it; where it cannot, the retained size
// is abandoned and the space is divided evenly. What must never happen — and did — is the newcomer
// pinning itself to the full width while the dock already there flexes to zero.

#include <j/core/DockManager.h>
#include <j/core/DockWidget.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>

using namespace jf;

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

// The narrow-column case: a 215 px column (what the studio's left dock stack actually is) holding a dock
// whose minimum is the 120 px default, with a 215 px-wide dock dropped into it. 215 - 120 = 95, which is
// less than the dropped dock's own 120 px minimum — the region cannot honour the retained size at all.
static void test_narrow_column_split_gives_both_sides_width() {
    JDockHost host;
    JDockWidget resident("Resident", 0.f, 0.f, 215.f, 800.f);
    JDockWidget dragged ("Dictionary", 0.f, 0.f, 215.f, 800.f);   // retained size = the column it came from
    resident.setMinSize(120.f, 60.f);
    dragged.setMinSize(120.f, 60.f);

    const JDockNodeId leaf = host.addDock(&resident);
    host.computeLayout({ 0.f, 0.f, 215.f, 800.f });
    const JRect before = host.node(leaf)->rect;
    CHECK(before.width > 0.f);

    // Live preview OFF: updateDrag applies a preview split of its own, and this test is about what the
    // COMMITTED split does. With it on, the tree gains a preview leaf as well and the numbers measure
    // two splits rather than one.
    host.setLivePreviewEnabled(false);
    host.updateDrag(0.f, 0.f, 0.f, 0.f, 0, 0, &dragged);   // registers the dragged dock, as a drag does
    const JDockNodeId added = host.splitLeaf(leaf, JDropPos::Right);
    CHECK(added.valid());
    host.computeLayout({ 0.f, 0.f, 215.f, 800.f });

    const JRect a = host.node(leaf)->rect;
    const JRect b = host.node(added)->rect;
    std::printf("  215px column split: resident %.0fpx, dropped %.0fpx\n", a.width, b.width);
    CHECK(a.width > 1.f);                                 // the bug: this was exactly 0
    CHECK(b.width > 1.f);                                 // and this was the whole 215
    CHECK(std::abs(a.width - b.width) < 2.f);             // an even carve-up is what this region can offer
    CHECK(a.width > 215.f * 0.4f);
}

// The roomy case is unchanged: a dropped dock narrower than the space keeps its own width and the
// resident absorbs the remainder, rather than being forced into a 50/50 it never asked for.
static void test_wide_column_keeps_the_dropped_dock_size() {
    JDockHost host;
    JDockWidget resident("Resident", 0.f, 0.f, 800.f, 800.f);
    JDockWidget dragged ("Narrow",   0.f, 0.f, 160.f, 400.f);
    resident.setMinSize(120.f, 60.f);
    dragged.setMinSize(120.f, 60.f);

    const JDockNodeId leaf = host.addDock(&resident);
    host.computeLayout({ 0.f, 0.f, 800.f, 800.f });
    host.setLivePreviewEnabled(false);
    host.updateDrag(0.f, 0.f, 0.f, 0.f, 0, 0, &dragged);
    const JDockNodeId added = host.splitLeaf(leaf, JDropPos::Right);
    host.computeLayout({ 0.f, 0.f, 800.f, 800.f });

    const JRect a = host.node(leaf)->rect;
    const JRect b = host.node(added)->rect;
    std::printf("  800px column split: resident %.0fpx, dropped %.0fpx\n", a.width, b.width);
    CHECK(a.width > 1.f && b.width > 1.f);
    CHECK(b.width < a.width);                             // the newcomer kept its (smaller) retained size
}

// A programmatic split with no drag in progress has no retained size to honour: even halves.
static void test_split_without_a_drag_is_even() {
    JDockHost host;
    JDockWidget resident("Resident", 0.f, 0.f, 400.f, 400.f);
    const JDockNodeId leaf = host.addDock(&resident);
    host.computeLayout({ 0.f, 0.f, 400.f, 400.f });
    const JDockNodeId added = host.splitLeaf(leaf, JDropPos::Right);
    host.computeLayout({ 0.f, 0.f, 400.f, 400.f });
    const float a = host.node(leaf)->rect.width, b = host.node(added)->rect.width;
    std::printf("  400px programmatic split: %.0fpx / %.0fpx\n", a, b);
    CHECK(a > 1.f && b > 1.f);
    CHECK(std::abs(a - b) < 2.f);
}

int main() {
    std::cout << "JDockHost split tests\n";
    test_narrow_column_split_gives_both_sides_width();
    test_wide_column_keeps_the_dropped_dock_size();
    test_split_without_a_drag_is_even();
    std::printf(g_fails ? "dock split: FAILED (%d)\n" : "dock split: OK\n", g_fails);
    return g_fails ? 1 : 0;
}
