// The single-placement invariant, exercised through every route that used to break it.
#include <j/core/DockManager.h>
#include <cstdio>
static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}
int main() {
    jf::JDockWidget nav("Navigation", 0, 0, 200, 400);

    std::puts("1. re-home between two hosts that are NOT in the drop registry");
    {
        jf::JDockHost a, b;
        a.addDock(&nav);
        b.addDock(&nav);
        check(a.findDock(&nav) == jf::InvalidDockNodeId, "gone from the old host");
        check(b.findDock(&nav) != jf::InvalidDockNodeId, "present in the new host");
        check(nav.placedIn() == &b,                      "placedIn names the new host");
    }
    check(nav.placedIn() == nullptr, "destroyed host clears placedIn (no dangling owner)");

    std::puts("2. inserting into a host that ALREADY holds it (a redundant \"show\")");
    {
        jf::JDockHost h;
        const jf::JDockNodeId first = h.addDock(&nav);
        h.addDock(&nav);                       // again
        int seen = 0;
        h.forEachDockPanel([&](jf::JDockWidget* p, const jf::JRect&, bool, int) { if (p == &nav) ++seen; });
        check(seen == 1, "appears exactly once, not twice");
        check(h.findDock(&nav) == first, "stayed in its leaf");
    }

    std::puts("3. a second leaf in the SAME host");
    {
        jf::JDockHost h;
        h.addDock(&nav);
        const jf::JDockNodeId other = h.addLeaf(h.rootId(), "second", 0.5f);
        h.insertDock(&nav, other);
        int seen = 0;
        h.forEachDockPanel([&](jf::JDockWidget* p, const jf::JRect&, bool, int) { if (p == &nav) ++seen; });
        check(seen == 1, "moved, not duplicated across leaves");
        check(h.findDock(&nav) == other, "lives in the target leaf");
    }
    std::printf("\n%s\n", fails ? "FAILURES" : "all invariants hold");
    return fails ? 1 : 0;
}
