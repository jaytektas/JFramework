// Headless unit test for the input-completeness wave: validators, JLineEdit echo
// mode / read-only / max-length, JCheckBox tri-state, and editable JComboBox.
//
// No GPU / windowing — a synthetic monospace atlas makes glyph metrics
// deterministic. Drives real widgets through their public API + key events.
//
// Build:
//   g++ -std=c++20 -I<repo>/include -I<repo>/third_party tests/inputs_test.cpp -o /tmp/inp_test
// Exit code: 0 = all PASS, non-zero = a failure.

#include "j/core/BaseWidgets.h"
#include <cstdio>
#include <string>

using namespace jf;

// ---- test harness ----------------------------------------------------------
static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("%-52s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++g_fail;
}

// ---- synthetic monospace atlas (8 px advance / glyph, incl. U+2022) --------
static void installMonoAtlas() {
    JFontAtlas atl;
    atl.valid = true;
    atl.pixelSize = 12.f;
    atl.ascent = 12.f; atl.descent = 3.f; atl.lineHeight = 16.f;
    for (uint32_t cp = 32; cp < 127; ++cp) {
        JGlyphInfo g{}; g.advanceX = 8.f; g.pixelW = 6.f; g.pixelH = 10.f;
        g.bearingX = 1.f; g.bearingY = -10.f; atl.glyphs[cp] = g;
    }
    JGlyphInfo bullet{}; bullet.advanceX = 8.f; bullet.pixelW = 4.f; bullet.pixelH = 4.f;
    bullet.bearingX = 2.f; bullet.bearingY = -6.f; atl.glyphs[0x2022] = bullet;
    JTextHelper::setAtlas(std::move(atl));
}

static JKeyEvent keyC(JKeyEvent::JKey k) { JKeyEvent e; e.key = k; e.pressed = true; return e; }
static JKeyEvent typeCh(char c) { JKeyEvent e; e.utf8[0] = c; e.utf8[1] = '\0'; e.pressed = true; return e; }
static size_t cpCount(const std::string& s) {
    size_t n = 0; for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n; return n;
}

int main() {
    installMonoAtlas();
    using K = JKeyEvent::JKey;
    using V = JValidator;
    JSceneGraph graph;
    int pos = 0;

    // 1) JIntValidator classification.
    {
        JIntValidator iv(0, 100);
        check("IntValidator rejects 'abc'",       iv.validate("abc", pos) == V::Invalid);
        check("IntValidator accepts '42'",        iv.validate("42",  pos) == V::Acceptable);
        check("IntValidator '' is Intermediate",  iv.validate("",    pos) == V::Intermediate);
        check("IntValidator '150' out-of-range Invalid", iv.validate("150", pos) == V::Invalid);
        JIntValidator wide(10, 9999);
        check("IntValidator '5' below-range Intermediate", wide.validate("5", pos) == V::Intermediate);
        JIntValidator sg(-50, 50);
        check("IntValidator '-' Intermediate when min<0", sg.validate("-", pos) == V::Intermediate);
        std::string fx = "500"; iv.fixup(fx);
        check("IntValidator fixup clamps 500 -> 100", fx == "100");
    }

    // 2) JDoubleValidator decimals + range.
    {
        JDoubleValidator dv(0.0, 10.0, 2);
        check("DoubleValidator '1.23' Acceptable",       dv.validate("1.23",  pos) == V::Acceptable);
        check("DoubleValidator '1.234' too many decimals Invalid", dv.validate("1.234", pos) == V::Invalid);
        check("DoubleValidator '1.2' Acceptable",        dv.validate("1.2",   pos) == V::Acceptable);
        check("DoubleValidator '' Intermediate",         dv.validate("",      pos) == V::Intermediate);
        check("DoubleValidator '20' above-range Invalid",dv.validate("20",    pos) == V::Invalid);
        check("DoubleValidator '-5' Invalid (min>=0)",   dv.validate("-5",    pos) == V::Invalid);
        std::string fx = "99.9"; dv.fixup(fx);
        check("DoubleValidator fixup clamps -> 10.00",   fx == "10.00");
    }

    // 3) JRegexValidator full-match.
    {
        JRegexValidator rv(std::regex("[A-Z]{3}"));
        check("RegexValidator 'ABC' Acceptable", rv.validate("ABC", pos) == V::Acceptable);
        check("RegexValidator '' Intermediate",  rv.validate("",    pos) == V::Intermediate);
        check("RegexValidator 'ab' Invalid",     rv.validate("ab",  pos) == V::Invalid);
    }

    // 4) JLineEdit with an int validator: refuse a letter, accept digits.
    {
        JLineEdit le(graph, "");
        le.setBounds({0, 0, 280, 30});
        JIntValidator iv(0, 9999);
        le.setValidator(&iv);
        le.handleKeyEvent(typeCh('4'));
        le.handleKeyEvent(typeCh('a'));   // letter → Invalid → rejected
        le.handleKeyEvent(typeCh('2'));
        check("LineEdit int-validator refuses letter, keeps digits", le.text() == "42");
        // Commit an out-of-range value → fixup clamps into range.
        le.setText("50000");
        le.commit();
        check("LineEdit commit clamps out-of-range via fixup", le.text() == "9999");
    }

    // 5) JLineEdit echo mode masks the text.
    {
        JLineEdit le(graph, "");
        le.setBounds({0, 0, 280, 30});
        le.setText("secret");
        le.setEchoMode(JLineEdit::Password);
        const std::string disp = le.displayText();
        check("Password echo: display != raw text", disp != "secret");
        check("Password echo: one glyph per char",  cpCount(disp) == cpCount("secret"));
        check("Password echo: raw text preserved",  le.text() == "secret");
        le.setEchoMode(JLineEdit::NoEcho);
        check("NoEcho renders empty",               le.displayText().empty());
        le.setEchoMode(JLineEdit::Normal);
        check("Normal echo shows raw",              le.displayText() == "secret");
    }

    // 6) JLineEdit read-only + max length.
    {
        JLineEdit le(graph, "");
        le.setBounds({0, 0, 280, 30});
        le.setText("locked");
        le.setReadOnly(true);
        le.handleKeyEvent(typeCh('x'));
        le.handleKeyEvent(keyC(K::Backspace));
        check("ReadOnly rejects edits", le.text() == "locked");

        JLineEdit ml(graph, "");
        ml.setBounds({0, 0, 280, 30});
        ml.setMaxLength(3);
        ml.handleKeyEvent(typeCh('a')); ml.handleKeyEvent(typeCh('b'));
        ml.handleKeyEvent(typeCh('c')); ml.handleKeyEvent(typeCh('d'));   // 4th rejected
        check("MaxLength caps at 3", ml.text() == "abc");
    }

    // 7) JCheckBox tri-state cycle: Unchecked -> Checked -> Partial -> Unchecked.
    {
        JCheckBox cb(graph, "opt");
        cb.setBounds({0, 0, 200, 22});
        cb.setTristate(true);
        check("CheckBox starts Unchecked", cb.checkState() == JCheckBox::Unchecked);
        cb.handleMousePress(5, 11);
        check("CheckBox click 1 -> Checked",   cb.checkState() == JCheckBox::Checked);
        cb.handleMousePress(5, 11);
        check("CheckBox click 2 -> Partial",   cb.checkState() == JCheckBox::PartiallyChecked);
        cb.handleMousePress(5, 11);
        check("CheckBox click 3 -> Unchecked", cb.checkState() == JCheckBox::Unchecked);

        JCheckBox two(graph, "two");     // non-tristate: no partial
        two.setBounds({0, 0, 200, 22});
        two.handleMousePress(5, 11);
        two.handleMousePress(5, 11);
        check("Two-state never enters Partial", two.checkState() == JCheckBox::Unchecked);
    }

    // 8) Editable JComboBox accepts typed text and reports it.
    {
        JComboBox cb(graph, {"Red", "Green", "Blue"});
        cb.setBounds({0, 0, 200, 30});
        cb.setEditable(true);
        std::string live;
        cb.onEditTextChanged.connect([&](const std::string& s){ live = s; });
        cb.handleKeyEvent(typeCh('C')); cb.handleKeyEvent(typeCh('y'));
        cb.handleKeyEvent(typeCh('a')); cb.handleKeyEvent(typeCh('n'));
        check("Editable combo captures typed text", cb.editText() == "Cyan");
        check("Editable combo currentText == typed", cb.currentText() == "Cyan");
        check("Editable combo emits live edit text",  live == "Cyan");
        cb.handleKeyEvent(keyC(K::Backspace));
        check("Editable combo backspace edits",      cb.editText() == "Cya");
        // Typing an exact item name + Return syncs the index.
        cb.setEditText("Blue");
        cb.handleKeyEvent(keyC(K::Return));
        check("Editable combo Return syncs index",   cb.currentIndex() == 2);
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL PASS" : "SOME FAILED");
    return g_fail == 0 ? 0 : 1;
}
