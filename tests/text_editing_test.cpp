// Headless unit test for JLineEdit commercial-grade text editing.
//
// Drives a real JLineEdit (backed by a JSceneGraph) through handleKeyEvent /
// handleMousePress and asserts selection, word-navigation, clipboard and
// double-click behaviour. No GPU / windowing — a synthetic monospace font atlas
// makes glyph metrics deterministic so mouse hit-testing is exact.
//
// Build:
//   g++ -std=c++20 -I<repo>/include tests/text_editing_test.cpp -o /tmp/text_editing_test
// Exit code: 0 = all PASS, non-zero = a failure.

#include "j/core/BaseWidgets.h"
#include <cstdio>
#include <string>

using namespace jf;

// ---- test harness ----------------------------------------------------------
static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("%-45s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++g_fail;
}

// ---- synthetic monospace atlas (8 px advance / glyph) ----------------------
static void installMonoAtlas() {
    JFontAtlas atl;
    atl.valid = true;
    atl.pixelSize = 12.f;
    atl.ascent = 12.f; atl.descent = 3.f; atl.lineHeight = 16.f;
    for (uint32_t cp = 32; cp < 127; ++cp) {
        JGlyphInfo g{};
        g.advanceX = 8.f;
        g.pixelW = 6.f; g.pixelH = 10.f;
        g.bearingX = 1.f; g.bearingY = -10.f;
        atl.glyphs[cp] = g;
    }
    JTextHelper::setAtlas(std::move(atl));
}

// ---- key-event builders ----------------------------------------------------
static JKeyEvent keyC(JKeyEvent::JKey k, bool shift=false, bool ctrl=false) {
    JKeyEvent e; e.key = k; e.shift = shift; e.ctrl = ctrl; e.pressed = true; return e;
}
static JKeyEvent typeCh(char c) {
    JKeyEvent e; e.key = JKeyEvent::JKey::Unknown; e.utf8[0] = c; e.utf8[1] = '\0'; e.pressed = true; return e;
}

int main() {
    installMonoAtlas();
    using K = JKeyEvent::JKey;

    // A field at origin so screen-x maps cleanly to glyph columns. Padding = 8 (scheme default),
    // so char i occupies x in [8 + 8*i, 8 + 8*(i+1)).
    JSceneGraph graph;

    // 1) Ctrl+A then type replaces the whole content.
    {
        JLineEdit le(graph, "");
        le.setBounds({0, 0, 280, 30});
        le.setText("hello");
        le.handleKeyEvent(keyC(K::A, false, true));   // Ctrl+A
        le.handleKeyEvent(typeCh('X'));
        check("select-all + type replaces", le.text() == "X");
    }

    // 2) Word navigation lands on word boundaries.
    {
        JLineEdit le(graph, "");
        le.setBounds({0, 0, 280, 30});
        le.setText("hello world");                    // caret at end (11)
        le.handleKeyEvent(keyC(K::Left, false, true));  // Ctrl+Left -> start of "world" (6)
        bool at6 = (le.caret() == 6);
        le.handleKeyEvent(keyC(K::Left, false, true));  // Ctrl+Left -> start of "hello" (0)
        bool at0 = (le.caret() == 0);
        le.handleKeyEvent(keyC(K::Right, false, true)); // Ctrl+Right -> start of "world" (6)
        bool back6 = (le.caret() == 6);
        check("word-left/right land on boundaries", at6 && at0 && back6);
    }

    // 3) Copy then paste duplicates the text.
    {
        JLineEdit le(graph, "");
        le.setBounds({0, 0, 280, 30});
        le.setText("abc");
        le.handleKeyEvent(keyC(K::A, false, true));    // select all
        le.handleKeyEvent(keyC(K::C, false, true));    // copy
        le.handleKeyEvent(keyC(K::End));               // collapse selection, caret to end
        le.handleKeyEvent(keyC(K::V, false, true));    // paste
        check("copy then paste duplicates", le.text() == "abcabc");
    }

    // 4) Cut removes the selection and places it on the clipboard.
    {
        JLineEdit le(graph, "");
        le.setBounds({0, 0, 280, 30});
        le.setText("abcdef");
        le.handleKeyEvent(keyC(K::Home));                       // caret 0
        le.handleKeyEvent(keyC(K::Right, /*shift*/true, false)); // select a
        le.handleKeyEvent(keyC(K::Right, true, false));          // ab
        le.handleKeyEvent(keyC(K::Right, true, false));          // abc
        bool sel = (le.selectedText() == "abc");
        le.handleKeyEvent(keyC(K::X, false, true));              // cut
        check("cut removes selection", sel && le.text() == "def" && JWidget::clipboardGet() == "abc");
    }

    // 5) Double-click selects the word under the cursor.
    {
        JLineEdit le(graph, "");
        le.setBounds({0, 0, 280, 30});
        le.setText("hello world");
        // x=20 -> column 2 (inside "hello"); two quick presses = double-click.
        le.handleMousePress(20.f, 15.f);
        le.handleMousePress(20.f, 15.f);
        check("double-click selects a word", le.selectedText() == "hello");
    }

    // 6) Bonus: triple-click selects all; shift-arrow extends selection.
    {
        JLineEdit le(graph, "");
        le.setBounds({0, 0, 280, 30});
        le.setText("one two");
        le.handleMousePress(20.f, 15.f);
        le.handleMousePress(20.f, 15.f);
        le.handleMousePress(20.f, 15.f);   // triple
        check("triple-click selects all", le.selectedText() == "one two");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL PASS" : "SOME FAILED");
    return g_fail == 0 ? 0 : 1;
}
