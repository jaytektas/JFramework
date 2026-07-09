// Headless regression + theming proof for the palette-adoption wave.
//
// Proves two things for the widgets migrated off hardcoded Colors:: onto the
// semantic palette + JStyle decision hooks:
//   (A) NO DEFAULT-THEME REGRESSION: under the default (dark) theme each
//       migrated widget emits the EXACT fill/border bytes it painted before
//       migration (checked for Normal AND Disabled — the pre-migration render
//       never dimmed on disable, so disabled must equal the base shade).
//   (B) THEMING NOW FLOWS: swap the palette (via JStyle::apply(light) AND via
//       the new JStyle::setPalette() custom-palette override) and the SAME
//       widget now emits DIFFERENT bytes — the palette actually drives paint.
//
// Compile:
//   g++ -std=c++20 -I<repo>/include -I<repo>/third_party \
//       tests/palette_adoption_test.cpp -o /tmp/pal_test
// Prints PASS/FAIL per case; exits non-zero on any failure.

#include "j/core/BaseWidgets.h"
#include <cstdio>
#include <cstring>

using namespace jf;

static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}

// Render a widget and return the FIRST emitted rectangle (its background/fill).
struct Rect { JColor fill, border; bool ok; };
static Rect firstRect(JWidget& w) {
    JPrimitiveBuffer buf;
    w.populateRenderPrimitives(buf);
    for (const auto& c : buf.getCommands()) {
        if (c.kind == JPrimitiveBuffer::JDrawCommand::JKind::JRect) {
            return { JColor::fromArray(c.rect.color),
                     JColor::fromArray(c.rect.borderColor), true };
        }
    }
    return { {}, {}, false };
}

static bool sameRGBA(JColor a, const uint8_t b[4]) { return a == JColor::fromArray(b); }

int main() {
    JSceneGraph graph;

    // ---- Capture the pre-migration named shades from the default (dark) theme.
    const JStyle dk = JStyle::dark();

    // ================================================================= (A) no regression
    JStyle::apply(JStyle::dark());
    {
        JButton btn(graph, "OK");
        Rect r = firstRect(btn);
        check("JButton Normal fill == old Surface2", r.ok && sameRGBA(r.fill, dk.Surface2));
        check("JButton Normal border == old Border", r.ok && sameRGBA(r.border, dk.Border));

        btn.setState(JWidgetState::Disabled);           // pre-migration: disabled == base shade
        Rect rd = firstRect(btn);
        check("JButton Disabled fill == old Surface2 (no regression)",
              rd.ok && sameRGBA(rd.fill, dk.Surface2));
        check("JButton Disabled border == old Border (no regression)",
              rd.ok && sameRGBA(rd.border, dk.Border));

        btn.setState(JWidgetState::Hovered);
        check("JButton Hovered fill == old Surface3 (state hook)",
              firstRect(btn).fill == JColor::fromArray(dk.Surface3));
        btn.setState(JWidgetState::Pressed);
        check("JButton Pressed fill == old Accent (state hook)",
              firstRect(btn).fill == JColor::fromArray(dk.Accent));
    }
    {
        JLineEdit le(graph);
        Rect r = firstRect(le);
        check("JLineEdit Normal fill == old Surface1", r.ok && sameRGBA(r.fill, dk.Surface1));
        check("JLineEdit Normal border == old Border", r.ok && sameRGBA(r.border, dk.Border));

        le.setState(JWidgetState::Disabled);
        Rect rd = firstRect(le);
        check("JLineEdit Disabled fill == old Surface1 (no regression)",
              rd.ok && sameRGBA(rd.fill, dk.Surface1));
    }
    {
        JComboBox cb(graph);
        Rect r = firstRect(cb);
        check("JComboBox Normal fill == old Surface2", r.ok && sameRGBA(r.fill, dk.Surface2));
        check("JComboBox Normal border == old Border", r.ok && sameRGBA(r.border, dk.Border));
    }
    {
        JListView lv(graph);
        Rect r = firstRect(lv);
        check("JListView Normal fill == old Surface1", r.ok && sameRGBA(r.fill, dk.Surface1));
        check("JListView Normal border == old Border", r.ok && sameRGBA(r.border, dk.Border));
    }
    {
        JProgressBar pb(graph);                          // trough = Button role (old Surface2)
        check("JProgressBar trough == old Surface2",
              firstRect(pb).fill == JColor::fromArray(dk.Surface2));
    }

    // Snapshot the default-theme button fill for the swap comparison below.
    JColor darkButtonFill;
    { JButton b(graph, "x"); darkButtonFill = firstRect(b).fill; }

    // ================================================================= (B1) theme swap flows
    JStyle::apply(JStyle::light());
    {
        const JStyle lt = JStyle::light();
        JButton btn(graph, "OK");
        JColor f = firstRect(btn).fill;
        check("JButton fill CHANGES after light-theme swap", f != darkButtonFill);
        check("JButton fill == light Surface2 (palette drives paint)",
              sameRGBA(f, lt.Surface2));

        JLineEdit le(graph);
        check("JLineEdit fill == light Surface1 after swap",
              sameRGBA(firstRect(le).fill, lt.Surface1));
    }

    // ================================================================= (B2) custom JPalette override flows
    // Install a fully custom palette straight onto the theme (no named-colour route):
    // paint a button GREEN via the Button role and confirm the widget follows it.
    {
        JPalette custom = JPalette::dark();
        const JColor green{0, 200, 0, 255};
        custom.setColor(JColorRole::Button, JColorGroup::Active, green);
        JStyle t = JStyle::light();
        t.setPalette(custom);
        JStyle::apply(t);

        JButton btn(graph, "OK");
        check("JButton follows a custom JPalette::setColor(Button) override",
              firstRect(btn).fill == green);
    }

    // Restore the default theme so we leave global state clean.
    JStyle::apply(JStyle::dark());

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
