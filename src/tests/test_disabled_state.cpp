// test_disabled_state.cpp — a disabled control must LOOK disabled, and a focused field must still
// follow a value that changes underneath it.
//
// Two defects, found the same afternoon, both of them a control lying about its state:
//
//   1. jstyle::option() built its style option from a widget's state but never cleared State_Enabled, so
//      the palette was asked for the Active colour group no matter what. A disabled button painted
//      pixel-for-pixel like a live one while ignoring every click — the worst possible pairing.
//   2. A spin box refused to update its text while it had focus, on the reasoning that refreshing
//      mid-edit would wipe the caret. True of a half-typed number; not true of a box that is merely
//      focused, which then went on displaying a stale value after an undo, a device re-read, or any
//      other change made from outside it.

#include <j/core/JStyle.h>
#include <j/core/JButton.h>
#include <j/core/JDoubleSpinBox.h>
#include <j/core/SceneGraph.h>

#include <cstdio>
#include <iostream>

using namespace jf;

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

static void test_disabled_reaches_the_palette() {
    const JStyleOption normal   = jstyle::option(JWidgetState::Normal,   false);
    const JStyleOption disabled = jstyle::option(JWidgetState::Disabled, false);
    CHECK(normal.group()   == JColorGroup::Active);
    CHECK(disabled.group() == JColorGroup::Disabled);          // the bug: this was Active

    const JColor onFill  = jstyle::buttonFill(normal);
    const JColor offFill = jstyle::buttonFill(disabled);
    std::printf("  button fill: enabled rgb(%d,%d,%d) vs disabled rgb(%d,%d,%d)\n",
                onFill.r, onFill.g, onFill.b, offFill.r, offFill.g, offFill.b);
    CHECK(onFill != offFill);

    // The caption is the cue that actually reads at a glance — on a dark theme the fill's disabled shade
    // is a few levels apart and easy to miss, so the text colour must differ too.
    const JPalette p = jstyle::pal();
    CHECK(p.color(JColorRole::ButtonText, JColorGroup::Active) !=
          p.color(JColorRole::ButtonText, JColorGroup::Disabled));
}

static void test_disabled_button_still_ignores_input() {
    JSceneGraph graph;
    JButton btn(graph, "Burn");
    bool clicked = false;
    btn.onClicked.connect([&clicked] { clicked = true; });
    btn.setEnabled(false);
    btn.handleMousePress(50.f, 20.f);
    CHECK(!clicked);
    CHECK(btn.getState() == JWidgetState::Disabled);           // …and a press did not knock it out of it
    btn.setEnabled(true);
    btn.handleMousePress(50.f, 20.f);
    btn.handleMouseRelease(50.f, 20.f);                        // the click fires on RELEASE INSIDE
    CHECK(clicked);
    std::printf("  a disabled button ignores clicks; re-enabling restores them\n");
}

static void test_focused_but_untouched_field_follows_the_value() {
    JSceneGraph graph;
    JDoubleSpinBox box(graph, 0.0, 10000.0, 1.0, 0);   // roomy: a typed digit must not be refused as out of range
    box.setValue(300.0);
    CHECK(box.text() == "300");

    // Focused, nothing typed: an outside change (an undo, a re-read) must show.
    box.setFocused(true);
    box.beginEdit();
    box.setValue(299.0);
    std::printf("  focused + untouched, value 299 -> text \"%s\"\n", box.text().c_str());
    CHECK(box.text() == "299");                                // the bug: it still read "300"
    CHECK(box.value() == 299.0);

    // Half-typed, though, is the user's: leave it alone.
    JKeyEvent k{}; k.pressed = true; k.key = JKeyEvent::JKey::_5; k.utf8[0] = '5'; k.utf8[1] = '\0';
    const bool took = box.handleKeyEvent(k);
    CHECK(took);                                               // the field accepted the digit
    const std::string typed = box.text();
    CHECK(typed != "299");                                     // the digit really did land in the field
    box.setValue(123.0);
    std::printf("  half-typed \"%s\" survives an outside setValue -> \"%s\"\n", typed.c_str(), box.text().c_str());
    CHECK(box.text() == typed);
}

int main() {
    std::cout << "disabled-state + focused-field tests\n";
    test_disabled_reaches_the_palette();
    test_disabled_button_still_ignores_input();
    test_focused_but_untouched_field_follows_the_value();
    std::printf(g_fails ? "disabled state: FAILED (%d)\n" : "disabled state: OK\n", g_fails);
    return g_fails ? 1 : 0;
}
