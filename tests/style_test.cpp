// Headless test for the JPalette / JStyleOption / JStyle role-and-state style system.
// Compile:
//   g++ -std=c++20 -I<repo>/include -I<repo>/third_party tests/style_test.cpp -o /tmp/style_test
// Prints PASS/FAIL per case; exits non-zero on any failure.

#include "j/core/BaseWidgets.h"   // pulls in StyleEngine.h (JPalette/JStyleOption/JStyle) + JTheme
#include <cstdio>

using namespace jf;

static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}

int main() {
    // 1) Light and dark palettes must differ in the two most fundamental roles.
    JPalette dark = JTheme::dark().palette();
    JPalette light = JTheme::light().palette();
    check("dark/light Window differ",
          dark.color(JColorRole::Window) != light.color(JColorRole::Window));
    check("dark/light Text differ",
          dark.color(JColorRole::Text) != light.color(JColorRole::Text));

    // Same for the self-contained default palettes.
    check("JPalette::dark/light Window differ",
          JPalette::dark().color(JColorRole::Window) != JPalette::light().color(JColorRole::Window));

    // 2) Disabled text must read dimmer than active text.
    check("Text(Disabled) != Text(Active)",
          dark.color(JColorRole::Text, JColorGroup::Disabled)
              != dark.color(JColorRole::Text, JColorGroup::Active));

    // 3) No visual regression: the old named constants map onto roles with IDENTICAL rgba.
    JTheme t = JTheme::dark();
    JPalette p = t.palette();
    check("role Window == old Surface1",
          p.color(JColorRole::Window) == JColor::fromArray(t.Surface1));
    check("role Text == old TextPrimary",
          p.color(JColorRole::Text) == JColor::fromArray(t.TextPrimary));
    check("role Highlight == old Accent",
          p.color(JColorRole::Highlight) == JColor::fromArray(t.Accent));
    check("role Border == old Border",
          p.color(JColorRole::Border) == JColor::fromArray(t.Border));

    // 4) JStyle resolution returns the Highlight role when Selected is set.
    JStyleOption sel;
    sel.set(State_Selected, true);
    check("controlFill(Selected) == Highlight",
          JStyle::controlFill(sel, p) == p.color(JColorRole::Highlight));

    JStyleOption plain;   // enabled, not selected
    check("controlFill(unselected) == Base",
          JStyle::controlFill(plain, p) == p.color(JColorRole::Base));

    // 5) Proof the migrated JCheckBox paints exactly what it used to.
    //    checked -> Highlight(=Accent), unchecked -> Base(=Surface1);
    //    focused border -> Accent, else -> Border.
    JStyleOption checked;  checked.set(State_On | State_Selected, true);
    check("checkbox checked fill == old Accent",
          JStyle::controlFill(checked, p) == JColor::fromArray(t.Accent));
    check("checkbox unchecked fill == old Surface1",
          JStyle::controlFill(plain, p) == JColor::fromArray(t.Surface1));
    JStyleOption focused; focused.set(State_Focused, true);
    check("checkbox focused border == old Accent",
          JStyle::borderColor(focused, p) == JColor::fromArray(t.Accent));
    check("checkbox unfocused border == old Border",
          JStyle::borderColor(plain, p) == JColor::fromArray(t.Border));

    // 6) Style-hint table returns the theme's metrics (no magic numbers at call sites).
    check("hint FocusRingWidth == theme.focusRingWidth",
          t.hint(JStyleHint::FocusRingWidth) == t.focusRingWidth);
    check("hint ControlRadius == theme.cornerRadius",
          t.hint(JStyleHint::ControlRadius) == t.cornerRadius);

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
