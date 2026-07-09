// dehard_dialogs_test.cpp — BYTE-EXACT no-regression gate for the dialog/misc
// colour de-hardcoding pass. Two kinds of check:
//   (A) every NEW JStyle role, under the default (dark) theme, equals the exact
//       literal it replaced;
//   (B) a real JMessageBox is rendered into a JPrimitiveBuffer headlessly and its
//       recorded background / title / button fill+border colours are asserted
//       equal to the pre-migration literals.
// Prints PASS/FAIL per case; non-zero exit on any mismatch.

#include <cstdio>
#include <cstdint>
#include <vector>

#include <j/core/JStyle.h>
#include <j/core/StandardDialogs.h>

using namespace jf;

static int g_fail = 0;

static bool eq4(const uint8_t* a, uint8_t r, uint8_t g, uint8_t b, uint8_t al) {
    return a[0] == r && a[1] == g && a[2] == b && a[3] == al;
}

static void check(const char* name, const uint8_t* got,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    bool ok = eq4(got, r, g, b, a);
    std::printf("%s %-24s got {%3u,%3u,%3u,%3u} want {%3u,%3u,%3u,%3u}\n",
                ok ? "PASS" : "FAIL", name,
                got[0], got[1], got[2], got[3], r, g, b, a);
    if (!ok) ++g_fail;
}

int main() {
    // Default theme is dark() — the singleton starts at JStyle{}.
    const JStyle& t = JStyle::current();

    std::printf("--- (A) new roles == replaced literals (default theme) ---\n");
    // Dialog / popup chrome
    check("DialogBg",         t.DialogBg,          26,  26,  32, 255);
    check("DialogTitleBg",    t.DialogTitleBg,     38,  38,  48, 255);
    check("OverlayScrim",     t.OverlayScrim,       0,   0,   0, 160);
    check("DialogShadow",     t.DialogShadow,       0,   0,   0,  60);
    check("InputFieldBg",     t.InputFieldBg,      18,  18,  28, 255);
    check("CancelBtnBg",      t.CancelBtnBg,       55,  55,  65, 255);
    check("CancelBtnBorder",  t.CancelBtnBorder,  100, 100, 110, 255);
    check("DialogCloseHover", t.DialogCloseHover, 180,  50,  50, 200);
    check("PopupBg",          t.PopupBg,           22,  22,  26, 255);
    check("PopupInnerBg",     t.PopupInnerBg,      18,  18,  22, 250);
    check("PopupItemText",    t.PopupItemText,    220, 220, 228, 255);
    check("PreviewBg",        t.PreviewBg,         20,  20,  26, 255);
    // Chart surface
    check("ChartBg",            t.ChartBg,           22,  24,  31, 255);
    check("ChartTitleText",     t.ChartTitleText,   220, 222, 230, 235);
    check("ChartAxisText",      t.ChartAxisText,    150, 154, 165, 220);
    check("ChartAxis2Text",     t.ChartAxis2Text,   160, 150, 140, 220);
    check("ChartLegendText",    t.ChartLegendText,  200, 204, 214, 230);
    check("ChartTooltipText",   t.ChartTooltipText, 210, 214, 224, 235);
    check("ChartTooltipBg",     t.ChartTooltipBg,    20,  22,  30, 235);
    check("ChartTooltipBorder", t.ChartTooltipBorder,90,  96, 110, 255);
    check("ChartCrosshair",     t.ChartCrosshair,   255, 255, 255,  70);

    // Reused existing roles must still hold their pre-migration bytes.
    check("Accent(reuse)",      t.Accent,            10, 132, 255, 255);
    check("Success(reuse)",     t.Success,           48, 209,  88, 255);
    check("CloseBtnMark(reuse)",t.CloseBtnMark,     255, 255, 255, 200);
    check("TitleBarText(reuse)",t.TitleBarText,     200, 200, 210, 230);

    std::printf("\n--- (B) rendered JMessageBox rect colours == old literals ---\n");
    // Two-button box so both the default (Accent) and secondary (CancelBtn*) fills render.
    JMessageBox box(JMessageIcon::Warning, "Title", "Body",
                    { JDialogButton::Ok, JDialogButton::Cancel });
    box.setDefaultButton(JDialogButton::Ok);

    JPrimitiveBuffer buf;
    const float screenW = 800.f, screenH = 600.f;
    // No hover (mx/my off-screen) → default button alpha 220, secondary alpha 220.
    box.render(buf, screenW, screenH, -1.f, -1.f);

    const auto& cmds = buf.getCommands();
    // Layout mirrors StandardDialogs::render: boxW=min(440,screenW*0.8)=440, boxH=170,
    // centred. First rect = full-screen backdrop; then body; then two title strips;
    // then the button fills in draw order.
    auto isRect = [](const JPrimitiveBuffer::JDrawCommand& c) {
        return c.kind == JPrimitiveBuffer::JDrawCommand::JKind::JRect;
    };

    // Backdrop: the one full-screen rect (width==screenW).
    const uint8_t* backdrop = nullptr;
    const uint8_t* body = nullptr; const uint8_t* bodyBorder = nullptr;
    const uint8_t* title = nullptr;
    std::vector<const uint8_t*> btnFills;
    std::vector<const uint8_t*> btnBorders;
    for (const auto& c : cmds) {
        if (!isRect(c)) continue;
        const float w = c.rect.rectBounds[2];
        const float h = c.rect.rectBounds[3];
        if (!backdrop && w == screenW && h == screenH) { backdrop = c.rect.color; continue; }
        if (!body && w == 440.f && h == 170.f) { body = c.rect.color; bodyBorder = c.rect.borderColor; continue; }
        if (!title && w == 440.f && h == 32.f) { title = c.rect.color; continue; }
        // Buttons: kBtnH = 30.f high; collect rects at button height.
        if (h == 30.f) { btnFills.push_back(c.rect.color); btnBorders.push_back(c.rect.borderColor); }
    }

    auto need = [](const char* n, const uint8_t* p) {
        if (!p) { std::printf("FAIL %-24s (rect not found in render output)\n", n); ++g_fail; return false; }
        return true;
    };

    if (need("mb.backdrop", backdrop))  check("mb.backdrop",      backdrop,  0,   0,   0, 160);
    if (need("mb.body",     body))      check("mb.body",          body,      26,  26,  32, 255);
    if (need("mb.bodyBorder", bodyBorder)) check("mb.bodyBorder", bodyBorder, 72, 72,  76, 255); // Border
    if (need("mb.title",    title))     check("mb.title",         title,     38,  38,  48, 255);

    // Default button (Accent, alpha 220 no-hover) and secondary (CancelBtnBg alpha 220 + border).
    bool sawAccent = false, sawCancel = false, sawCancelBorder = false;
    for (size_t i = 0; i < btnFills.size(); ++i) {
        const uint8_t* f = btnFills[i];
        if (eq4(f, 10, 132, 255, 220)) sawAccent = true;
        if (eq4(f, 55, 55, 65, 220))   { sawCancel = true;
            if (btnBorders[i] && eq4(btnBorders[i], 100, 100, 110, 255)) sawCancelBorder = true; }
    }
    std::printf("%s %-24s (Accent 220 default-button fill present)\n", sawAccent ? "PASS" : "FAIL", "mb.defaultBtnFill");
    if (!sawAccent) ++g_fail;
    std::printf("%s %-24s (CancelBtnBg 220 secondary fill present)\n", sawCancel ? "PASS" : "FAIL", "mb.cancelBtnFill");
    if (!sawCancel) ++g_fail;
    std::printf("%s %-24s (CancelBtnBorder present)\n", sawCancelBorder ? "PASS" : "FAIL", "mb.cancelBtnBorder");
    if (!sawCancelBorder) ++g_fail;

    std::printf("\n%s: %d failure(s)\n", g_fail ? "FAIL" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
