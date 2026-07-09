// dehard_widgets_test — BYTE-EXACT no-regression gate for the widget-group colour/size de-hardcoding.
//
// Each widget touched by the de-hardcode pass is constructed with a JSceneGraph, rendered into a
// JPrimitiveBuffer under the DEFAULT (dark) theme, and the emitted fill / border / text colours are
// asserted against the EXACT OLD literal bytes that were hardcoded before migration (the expected value
// in every assertion IS the recorded pre-migration literal). We also assert every NEW JStyle role added
// by the pass equals its old bytes under the default theme. PASS/FAIL per case; non-zero exit on any fail.

#include <j/core/JButton.h>
#include <j/core/JToolButton.h>
#include <j/core/JToggleButton.h>
#include <j/core/JCheckBox.h>
#include <j/core/JRadioButton.h>
#include <j/core/JLabel.h>
#include <j/core/JSeparator.h>
#include <j/core/JLineEdit.h>
#include <j/core/JKeySequenceEdit.h>
#include <j/core/JTextArea.h>
#include <j/core/JSpinBox.h>
#include <j/core/JDoubleSpinBox.h>
#include <j/core/JComboBox.h>
#include <j/core/JPopupItem.h>
#include <j/core/JFontButton.h>
#include <j/core/JSlider.h>
#include <j/core/JScrollBar.h>
#include <j/core/JProgressBar.h>
#include <j/core/JStyle.h>

#include <cstdio>
#include <cstdint>

using namespace jf;

static int g_pass = 0, g_fail = 0;

static void check(const char* name, bool cond) {
    if (cond) { ++g_pass; std::printf("PASS  %s\n", name); }
    else      { ++g_fail; std::printf("FAIL  %s\n", name); }
}

static bool eq4(const uint8_t c[4], uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return c[0]==r && c[1]==g && c[2]==b && c[3]==a;
}

// Scan a render buffer for an emitted TEXT-call colour == (r,g,b,a).
static bool hasText(const JPrimitiveBuffer& buf, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (const auto& cmd : buf.getCommands())
        if (cmd.kind == JPrimitiveBuffer::JDrawCommand::JKind::Text && eq4(cmd.text.color, r,g,b,a)) return true;
    return false;
}
// Scan for a rectangle FILL colour == (r,g,b,a).
static bool hasFill(const JPrimitiveBuffer& buf, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (const auto& cmd : buf.getCommands())
        if (cmd.kind == JPrimitiveBuffer::JDrawCommand::JKind::JRect && eq4(cmd.rect.color, r,g,b,a)) return true;
    return false;
}
// Scan for a rectangle BORDER colour == (r,g,b,a).
static bool hasBorder(const JPrimitiveBuffer& buf, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (const auto& cmd : buf.getCommands())
        if (cmd.kind == JPrimitiveBuffer::JDrawCommand::JKind::JRect && eq4(cmd.rect.borderColor, r,g,b,a)) return true;
    return false;
}

// Build a synthetic font atlas so text-emitting branches run (pushText requires a valid atlas + a sized glyph).
static void installTestAtlas() {
    JFontAtlas a;
    a.valid = true; a.width = 512; a.height = 256;
    a.pixelSize = 14.f; a.ascent = 11.f; a.descent = -3.f; a.lineHeight = 16.f;
    for (uint32_t cp = 32; cp <= 126; ++cp) {
        JGlyphInfo g{};
        g.u0 = 0; g.v0 = 0; g.u1 = 0.01f; g.v1 = 0.01f;
        g.pixelW = 5.f; g.pixelH = 8.f; g.bearingX = 0.f; g.bearingY = -8.f; g.advanceX = 6.f;
        a.glyphs[cp] = g;
    }
    a.glyphs[0x2022] = a.glyphs['o'];   // bullet (password echo)
    JTextHelper::setAtlas(std::move(a));
}

int main() {
    JStyle::apply(JStyle::dark());   // default theme = byte-exact baseline
    installTestAtlas();

    // ---- NEW roles: each default must equal the exact old literal bytes under the default theme ----
    {
        const JStyle& t = JStyle::current();
        check("role.ControlText      == {220,220,228,255}", eq4(t.ControlText,      220,220,228,255));
        check("role.FieldText        == {210,210,220,255}", eq4(t.FieldText,        210,210,220,255));
        check("role.LabelText        == {200,200,210,255}", eq4(t.LabelText,        200,200,210,255));
        check("role.MutedText        == {180,180,190,255}", eq4(t.MutedText,        180,180,190,255));
        check("role.FieldPlaceholder == {100,100,110,255}", eq4(t.FieldPlaceholder, 100,100,110,255));
        check("role.CaptureHint      == {150,190,255,255}", eq4(t.CaptureHint,      150,190,255,255));
        check("role.SelectionFill    == {65,105,225,255}",  eq4(t.SelectionFill,     65,105,225,255));
        check("role.HighlightedText  == {255,255,255,255}", eq4(t.HighlightedText,  255,255,255,255));
    }

    // ---- JButton: label text was {220,220,228,230} ----
    {
        JSceneGraph g; JButton w(g, "Ok");
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JButton label text {220,220,228,230}", hasText(buf, 220,220,228,230));
    }

    // ---- JToggleButton: label text was {220,220,228,230} ----
    {
        JSceneGraph g; JToggleButton w(g, "On");
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JToggleButton label text {220,220,228,230}", hasText(buf, 220,220,228,230));
    }

    // ---- JCheckBox (checked): mark was white {255,255,255,220}; label was {200,200,210,200} ----
    {
        JSceneGraph g; JCheckBox w(g, "Enable");
        w.setChecked(true);
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JCheckBox tick fill {255,255,255,220}", hasFill(buf, 255,255,255,220));
        check("JCheckBox label text {200,200,210,200}", hasText(buf, 200,200,210,200));
    }

    // ---- JCheckBox (partial): dash was white {255,255,255,200} ----
    {
        JSceneGraph g; JCheckBox w(g, "Part");
        w.setTristate(true); w.setCheckState(JCheckBox::PartiallyChecked);
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JCheckBox partial dash fill {255,255,255,200}", hasFill(buf, 255,255,255,200));
    }

    // ---- JRadioButton: label text was {200,200,210,200} ----
    {
        JSceneGraph g; JRadioButton w(g, "Choice");
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JRadioButton label text {200,200,210,200}", hasText(buf, 200,200,210,200));
    }

    // ---- JToolButton (menu arrow): chevron was {180,180,190,220} ----
    {
        JSceneGraph g; JToolButton w(g, "Tool");
        w.setMenuArrow(true);
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JToolButton chevron fill {180,180,190,220}", hasFill(buf, 180,180,190,220));
    }

    // ---- JLabel: text was {200,200,210,200} ----
    {
        JSceneGraph g; JLabel w(g, "Caption");
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JLabel text {200,200,210,200}", hasText(buf, 200,200,210,200));
    }

    // ---- JLineEdit (empty): placeholder text was {100,100,110,160} ----
    {
        JSceneGraph g; JLineEdit w(g, "type here");
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JLineEdit placeholder text {100,100,110,160}", hasText(buf, 100,100,110,160));
    }
    // ---- JLineEdit (with text): value text was {220,220,228,220} ----
    {
        JSceneGraph g; JLineEdit w(g, "");
        w.setText("hello");
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JLineEdit value text {220,220,228,220}", hasText(buf, 220,220,228,220));
    }

    // ---- JKeySequenceEdit (capturing): prompt was {150,190,255,210} ----
    {
        JSceneGraph g; JKeySequenceEdit w(g, "");
        w.setCapturing(true);
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JKeySequenceEdit prompt text {150,190,255,210}", hasText(buf, 150,190,255,210));
    }
    // ---- JKeySequenceEdit (idle, empty): "None" placeholder was {100,100,110,160} ----
    {
        JSceneGraph g; JKeySequenceEdit w(g, "");
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JKeySequenceEdit none placeholder {100,100,110,160}", hasText(buf, 100,100,110,160));
    }

    // ---- JTextArea: placeholder was {100,100,110,160}; selection band was {65,105,225,100} ----
    {
        JSceneGraph g; JTextArea w(g, "placeholder");
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JTextArea placeholder text {100,100,110,160}", hasText(buf, 100,100,110,160));
    }
    {
        JSceneGraph g; JTextArea w(g, "");
        w.setText("hello world");
        JKeyEvent ke; ke.key = JKeyEvent::JKey::A; ke.ctrl = true; ke.pressed = true;  // Ctrl+A → select all
        w.handleKeyEvent(ke);
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JTextArea selection fill {65,105,225,100}", hasFill(buf, 65,105,225,100));
        check("JTextArea value text {220,220,228,220}",    hasText(buf, 220,220,228,220));
    }

    // ---- JSpinBox: value text was {210,210,220,220}; arrow marks were {180,180,190,200} ----
    {
        JSceneGraph g; JSpinBox w(g, 0, 100);
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JSpinBox value text {210,210,220,220}", hasText(buf, 210,210,220,220));
        check("JSpinBox arrow mark fill {180,180,190,200}", hasFill(buf, 180,180,190,200));
    }

    // ---- JDoubleSpinBox: value text was {210,210,220,220}; arrow marks were {180,180,190,200} ----
    {
        JSceneGraph g; JDoubleSpinBox w(g, 0.0, 10.0);
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JDoubleSpinBox value text {210,210,220,220}", hasText(buf, 210,210,220,220));
        check("JDoubleSpinBox arrow mark fill {180,180,190,200}", hasFill(buf, 180,180,190,200));
    }

    // ---- JComboBox: chevron was {180,180,190,220}; text was {210,210,220,220} ----
    {
        JSceneGraph g; JComboBox w(g, {"aaa", "bbb"});
        w.setCurrentIndex(0);
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JComboBox chevron fill {180,180,190,220}", hasFill(buf, 180,180,190,220));
        check("JComboBox text {210,210,220,220}",          hasText(buf, 210,210,220,220));
    }

    // ---- JPopupItem (hovered): hover tint was white {255,255,255,18}; label was {220,220,228,230} ----
    {
        JSceneGraph g; JPopupItem w(g, "menu row");
        w.setState(JWidgetState::Hovered);
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JPopupItem hover tint fill {255,255,255,18}", hasFill(buf, 255,255,255,18));
        check("JPopupItem label text {220,220,228,230}",     hasText(buf, 220,220,228,230));
    }

    // ---- JFontButton (inheritable, set): inline clear ✕ was {200,200,210,200} ----
    {
        JSceneGraph g; JFontButton w(g);
        w.setInheritable(true); w.setFontSpec("Arial|12|0|0");
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JFontButton clear-x text {200,200,210,200}", hasText(buf, 200,200,210,200));
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
