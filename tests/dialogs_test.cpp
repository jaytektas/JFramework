// Headless test for the standard dialog DECISION logic (no window / no GPU).
// Compile:
//   g++ -std=c++20 -I<repo>/include -I<repo>/third_party tests/dialogs_test.cpp -o /tmp/dlg_test
// Prints PASS/FAIL per case; exits non-zero on any failure.

#include "j/core/StandardDialogs.h"
#include <cstdio>

using namespace jf;

static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}

int main() {
    // ---- JMessageBox: {Yes,No}, default Yes, escape No --------------------
    {
        JMessageBox box(JMessageIcon::Question, "Quit?", "Save before quitting?",
                        { JDialogButton::Yes, JDialogButton::No });
        box.setDefaultButton(JDialogButton::Yes).setEscapeButton(JDialogButton::No);

        check("msgbox button set is {Yes,No}",
              box.buttons().size() == 2 &&
              box.buttons()[0] == JDialogButton::Yes &&
              box.buttons()[1] == JDialogButton::No);
        check("msgbox default resolves to Yes", box.defaultButton() == JDialogButton::Yes);
        check("msgbox escape resolves to No",   box.escapeButton()  == JDialogButton::No);

        // Accept the default -> Yes (affirmative).
        JDialogResult r = box.activateDefault();
        check("msgbox activateDefault -> Yes", r.button == JDialogButton::Yes && r.accepted);
        check("msgbox is done after choice", box.isDone());
    }

    // Escape resolution on a fresh box -> No.
    {
        JMessageBox box(JMessageIcon::Question, "Quit?", "?",
                        { JDialogButton::Yes, JDialogButton::No });
        box.setDefaultButton(JDialogButton::Yes).setEscapeButton(JDialogButton::No);
        JDialogResult r = box.escape();
        check("msgbox escape -> No", r.button == JDialogButton::No && !r.accepted);
    }

    // ---- Escape auto-resolution (no explicit escape) ----------------------
    {
        JMessageBox box(JMessageIcon::Warning, "Delete", "Sure?",
                        { JDialogButton::Ok, JDialogButton::Cancel });
        check("auto escape prefers Cancel", box.escapeButton() == JDialogButton::Cancel);
        check("auto default is first (Ok)",  box.defaultButton() == JDialogButton::Ok);
    }
    {
        // Save / Discard / Cancel: escape -> Cancel; default -> first (Save).
        JMessageBox box(JMessageIcon::Warning, "Unsaved", "Save changes?",
                        { JDialogButton::Save, JDialogButton::Discard, JDialogButton::Cancel });
        check("3-button escape -> Cancel", box.escapeButton() == JDialogButton::Cancel);
        JDialogResult r = box.clickButton(JDialogButton::Discard);
        check("Discard is not affirmative", r.button == JDialogButton::Discard && !r.accepted);
    }

    // ---- Convenience constructors set icon + buttons ----------------------
    {
        JMessageBox info = JMessageBox::information("T", "hello");
        check("information() icon+buttons",
              info.icon() == JMessageIcon::Information &&
              info.buttons().size() == 1 && info.buttons()[0] == JDialogButton::Ok);

        JMessageBox warn = JMessageBox::warning("T", "careful");
        check("warning() icon", warn.icon() == JMessageIcon::Warning);

        JMessageBox crit = JMessageBox::critical("T", "boom");
        check("critical() icon", crit.icon() == JMessageIcon::Critical);

        JMessageBox q = JMessageBox::question("T", "yes?");
        check("question() icon+buttons+default+escape",
              q.icon() == JMessageIcon::Question &&
              q.hasButton(JDialogButton::Yes) && q.hasButton(JDialogButton::No) &&
              q.defaultButton() == JDialogButton::Yes &&
              q.escapeButton()  == JDialogButton::No);
    }

    // ---- Callback fires exactly once, carrying the result -----------------
    {
        JMessageBox box = JMessageBox::question("T", "?");
        JDialogButton got = JDialogButton::None; int calls = 0;
        box.onResult([&](JDialogResult r){ got = r.button; ++calls; });
        box.clickButton(JDialogButton::Yes);
        box.clickButton(JDialogButton::No);   // second click must be a no-op (already done)
        check("onResult fires once with Yes", calls == 1 && got == JDialogButton::Yes);
    }

    // ---- JMessageBox mouse hit-test maps to the right button --------------
    {
        JMessageBox box(JMessageIcon::Question, "T", "?",
                        { JDialogButton::Yes, JDialogButton::No });
        const float SW = 800.f, SH = 600.f;
        const float boxW = std::min(440.f, SW * 0.8f), boxH = 170.f;
        const float boxX = (SW - boxW) * 0.5f, boxY = (SH - boxH) * 0.5f;
        auto rects = box.buttonRects(boxX, boxY, boxW, boxH);
        check("two button rects laid out", rects.size() == 2);
        // Click the second rect (No).
        const JRect& r1 = rects[1];
        bool hit = box.handleMousePress(r1.x + r1.width * 0.5f, r1.y + r1.height * 0.5f, SW, SH);
        check("mouse press on 2nd rect -> No", hit && box.result().button == JDialogButton::No);
    }

    // ---- JInputDialog: text, seeded, accept -> value ----------------------
    {
        JInputDialog d = JInputDialog::getText("Name", "Enter name:", "Ada");
        check("input seeded text", d.buffer() == "Ada");
        JDialogResult r = d.accept();
        check("input accept -> (true, 'Ada')", r.accepted && r.text == "Ada");
    }
    // Cancel -> accepted=false.
    {
        JInputDialog d = JInputDialog::getText("Name", "Enter name:", "Ada");
        JDialogResult r = d.cancel();
        check("input cancel -> accepted=false", !r.accepted);
    }
    // Integer mode: seeded value, accept returns it; range clamps.
    {
        JInputDialog d = JInputDialog::getInt("Count", "How many?", 7, 0, 10);
        JDialogResult r = d.accept();
        check("input int seeded accept -> 7", r.accepted && r.intValue == 7);

        JInputDialog d2 = JInputDialog::getInt("Count", "?", 5, 0, 10);
        d2.setBuffer("999");            // out of range typed
        JDialogResult r2 = d2.accept();
        check("input int clamps to max", r2.intValue == 10);
    }
    // Double mode.
    {
        JInputDialog d = JInputDialog::getDouble("Gain", "Value:", 1.5, 0.0, 3.0);
        JDialogResult r = d.accept();
        check("input double seeded accept -> 1.5", r.accepted && r.doubleValue == 1.5);
    }
    // Choice mode.
    {
        JInputDialog d = JInputDialog::getItem("Mode", "Pick:", {"A", "B", "C"}, 1);
        check("input choice seeded index", d.choiceIndex() == 1);
        d.setChoiceIndex(2);
        JDialogResult r = d.accept();
        check("input choice accept -> (2,'C')", r.accepted && r.choiceIndex == 2 && r.text == "C");
    }

    // ---- Picker adaptor uniform result ------------------------------------
    {
        JDialogResult a = jResultFromPicker(true, "#ff8800");
        JDialogResult c = jResultFromPicker(false, "");
        check("picker accepted result", a.accepted && a.text == "#ff8800" && a.button == JDialogButton::Ok);
        check("picker cancelled result", !c.accepted && c.button == JDialogButton::Cancel);
    }

    std::printf(g_fail ? "\n%d FAIL\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
