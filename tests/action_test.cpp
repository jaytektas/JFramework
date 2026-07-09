// action_test.cpp — headless (no window) tests for the JFramework action + shortcut system.
//
// Build:
//   g++ -std=c++20 -Iinclude -Ithird_party tests/action_test.cpp -o /tmp/act_test
// Runs entirely on the CPU; prints PASS/FAIL per case; exits non-zero on any failure.

#include <j/core/KeySequence.h>
#include <j/core/Action.h>
#include <j/core/ShortcutMap.h>

#include <cstdio>
#include <string>

using namespace jf;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

// Build a key event for a chord.
static JKeyEvent ev(JKeyEvent::JKey k, bool ctrl = false, bool shift = false, bool alt = false) {
    JKeyEvent e;
    e.key = k; e.ctrl = ctrl; e.shift = shift; e.alt = alt; e.pressed = true;
    return e;
}

int main() {
    // -----------------------------------------------------------------
    // 1. parse "Ctrl+S" -> matches ctrl+S, not plain S, not Ctrl+Shift+S.
    // -----------------------------------------------------------------
    {
        JKeySequence seq = JKeySequence::parse("Ctrl+S");
        check("parse Ctrl+S valid",              seq.valid());
        check("parse Ctrl+S key==S",             seq.key == JKeyEvent::JKey::S);
        check("parse Ctrl+S has ctrl only",      seq.hasCtrl() && !seq.hasShift() && !seq.hasAlt());
        check("Ctrl+S matches ctrl+S event",     seq.matches(ev(JKeyEvent::JKey::S, /*ctrl*/true)));
        check("Ctrl+S rejects plain S event",    !seq.matches(ev(JKeyEvent::JKey::S)));
        check("Ctrl+S rejects ctrl+shift+S",     !seq.matches(ev(JKeyEvent::JKey::S, true, true)));
        check("Ctrl+S rejects ctrl+D",           !seq.matches(ev(JKeyEvent::JKey::D, true)));
    }

    // -----------------------------------------------------------------
    // 2. JAction with Ctrl+S shortcut: register, dispatch -> triggered + consumed.
    // -----------------------------------------------------------------
    {
        jShortcuts().clear();
        JAction save("Save");
        save.setShortcut(JKeySequence::parse("Ctrl+S"));
        int fired = 0;
        save.triggered.connect([&fired]() { ++fired; });
        jShortcuts().registerAction(&save);

        const bool consumed = jShortcuts().dispatch(ev(JKeyEvent::JKey::S, true));
        check("dispatch(ctrl+S) consumed",       consumed);
        check("dispatch fired triggered once",   fired == 1);

        const bool miss = jShortcuts().dispatch(ev(JKeyEvent::JKey::S));   // plain S: no match
        check("dispatch(plain S) not consumed",  !miss);
        check("plain S did not fire",            fired == 1);
    }

    // -----------------------------------------------------------------
    // 3. Disabled action does NOT trigger / consume.
    // -----------------------------------------------------------------
    {
        jShortcuts().clear();
        JAction act("Disabled");
        act.setShortcut(JKeySequence::parse("Ctrl+D"));
        int fired = 0;
        act.triggered.connect([&fired]() { ++fired; });
        act.setEnabled(false);
        jShortcuts().registerAction(&act);

        const bool consumed = jShortcuts().dispatch(ev(JKeyEvent::JKey::D, true));
        check("disabled action not consumed",    !consumed);
        check("disabled action did not fire",    fired == 0);
    }

    // -----------------------------------------------------------------
    // 4. Checkable action toggles on trigger and emits toggled(bool).
    // -----------------------------------------------------------------
    {
        JAction toggle("Bold");
        toggle.setCheckable(true);
        int toggleCount = 0; bool lastState = false;
        toggle.toggled.connect([&](bool s) { ++toggleCount; lastState = s; });

        check("checkable starts unchecked",      !toggle.isChecked());
        toggle.trigger();
        check("trigger toggles to checked",      toggle.isChecked());
        check("toggled emitted true",            toggleCount == 1 && lastState == true);
        toggle.trigger();
        check("trigger toggles back off",        !toggle.isChecked());
        check("toggled emitted false",           toggleCount == 2 && lastState == false);
    }

    // -----------------------------------------------------------------
    // 5. Non-checkable trigger fires triggered but never toggled.
    // -----------------------------------------------------------------
    {
        JAction plain("Run");
        int trig = 0, tog = 0;
        plain.triggered.connect([&]() { ++trig; });
        plain.toggled.connect([&](bool) { ++tog; });
        plain.trigger();
        check("plain trigger fired triggered",   trig == 1);
        check("plain trigger no toggled",        tog == 0);
    }

    // -----------------------------------------------------------------
    // 6. Standalone shortcut + callback via dispatch.
    // -----------------------------------------------------------------
    {
        jShortcuts().clear();
        int hits = 0;
        jShortcuts().registerShortcut(JKeySequence::parse("Ctrl+Q"), [&]() { ++hits; });
        check("standalone Ctrl+Q consumed",      jShortcuts().dispatch(ev(JKeyEvent::JKey::Q, true)));
        check("standalone callback fired",       hits == 1);
        check("standalone wrong key ignored",    !jShortcuts().dispatch(ev(JKeyEvent::JKey::Q)));
    }

    // -----------------------------------------------------------------
    // 7. format(parse(x)) == x round-trip for several canonical sequences.
    // -----------------------------------------------------------------
    {
        const char* seqs[] = {
            "Ctrl+S", "Ctrl+Shift+S", "Alt+F4", "Ctrl+Alt+Del",
            "Esc", "Space", "F11", "Ctrl+Z", "Shift+Tab", "Ctrl+PageDown",
        };
        bool allOk = true;
        for (const char* s : seqs) {
            std::string round = JKeySequence::parse(s).toString();
            if (round != s) { allOk = false; std::printf("    round-trip mismatch: '%s' -> '%s'\n", s, round.c_str()); }
        }
        check("format(parse(x))==x round-trip",  allOk);
    }

    // -----------------------------------------------------------------
    // 8. Equality / hashing consistency.
    // -----------------------------------------------------------------
    {
        JKeySequence a = JKeySequence::parse("Ctrl+Shift+S");
        JKeySequence b = JKeySequence::parse("shift+ctrl+s");   // order/case independent
        check("parse is order/case independent", a == b);
        check("equal sequences hash equal",      std::hash<JKeySequence>{}(a) == std::hash<JKeySequence>{}(b));
        JKeySequence c = JKeySequence::parse("Ctrl+S");
        check("different sequences differ",      a != c);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILURES" : "ALL PASS",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
