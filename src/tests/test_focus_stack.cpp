// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// Focus managers nest (main window, then one per open dialog) and s_active follows the top of that stack.
// They are NOT guaranteed to die in creation order: a dialog opened from another dialog's callback can
// outlive its opener. When that happened, s_active — or a survivor's m_prevActive — was left pointing at
// the freed manager, and the next widget-initiated focus request read m_focused out of freed memory and
// called setFocused() on it. A hard crash on the next click into any text field.
#include <j/core/FocusManager.h>
#include <cstdio>
#include <memory>

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}
static bool alive(jf::JFocusManager* m) {
    const auto& v = jf::JFocusManager::s_instances;
    return m && std::find(v.begin(), v.end(), m) != v.end();
}

int main() {
    std::puts("1. LIFO teardown (a dialog closing over the main window)");
    {
        jf::JFocusManager main;
        {
            jf::JFocusManager dialog;
            check(jf::JFocusManager::s_active == &dialog, "the newest manager is active");
        }
        check(jf::JFocusManager::s_active == &main, "closing it restores the one below");
    }

    std::puts("2. OUT-OF-ORDER teardown (a dialog opened from another dialog's callback)");
    {
        jf::JFocusManager main;
        auto first  = std::make_unique<jf::JFocusManager>();   // "Open an existing ECU"
        auto second = std::make_unique<jf::JFocusManager>();   // "Choose a tune", opened from its callback
        first.reset();                                         // the opener closes FIRST
        check(alive(jf::JFocusManager::s_active), "s_active is still a live manager");
        second.reset();
        check(jf::JFocusManager::s_active == &main, "and unwinding lands back on the main window");
        check(alive(jf::JFocusManager::s_active), "…which is alive, not a freed opener");
    }

    std::puts("3. the active manager is never a dead one, whatever the order");
    {
        jf::JFocusManager main;
        auto a = std::make_unique<jf::JFocusManager>();
        auto b = std::make_unique<jf::JFocusManager>();
        auto c = std::make_unique<jf::JFocusManager>();
        b.reset(); a.reset(); c.reset();
        check(jf::JFocusManager::s_active == &main, "s_active unwound to the survivor");
        check(alive(jf::JFocusManager::s_active), "s_active points at a live manager");
    }
    std::printf("\n%s\n", fails ? "FAILURES" : "focus stack holds under any teardown order");
    return fails ? 1 : 0;
}
