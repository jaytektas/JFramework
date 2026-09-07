// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// THE NUMBER WINS THE BOX.
//
// A spin box draws its unit inside itself and reserves that width out of the text field. Two things went
// wrong with that. The reserve was measured on the string the caller set — " RPM", leading space and all,
// plus a 6px pad — so three characters took about 38px, half of an 80px box. And when the box was too
// narrow for both, the unit was drawn in full and the VALUE was what got cut: 661 rendered as "1", which
// is not a clipped number but a different one, and one that reads as a real reading.
//
// A unit is a label you already know. The value is the thing you are there to read.
//
//   cmake --build build --target test_spinbox_suffix && ./build/test_spinbox_suffix

#include <j/core/JDoubleSpinBox.h>
#include <j/core/SceneGraph.h>
#include <j/core/JTextHelper.h>
#include <j/graphics/RenderPrimitive.h>
#include <j/graphics/FontEngine.h>

#include <cstdio>
#include <string>

using namespace jf;

static int fails = 0;
static void check(const char* what, bool ok, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : " — ", detail.c_str());
    if (!ok) ++fails;
}

int main() {
    JFontEngine fe;
    if (!fe.loadSystemFont()) { std::printf("skipped (no system font)\n"); return 0; }
    JTextHelper::setAtlas(fe.buildAtlas(14.0f));

    std::printf("=== spin box: number vs unit ===\n");
    JSceneGraph g;
    JDoubleSpinBox sb(g, 0.0, 2000.0, 1.0, 0);
    sb.setSuffix(" RPM");
    sb.setValue(661);

    // Wide: both fit. The unit is shown, and it costs about what three characters cost — not half the box.
    sb.setBounds({0, 0, 140, 24});
    JPrimitiveBuffer b1; sb.populateRenderPrimitives(b1);
    const float wideNum = sb.numberWidth();
    const float unitCost = 140.0f - 24.0f * 0.7f - wideNum;
    check("a wide box shows the unit", sb.unitShown());
    // "RPM" is 26px of glyphs at the 14px UI font, plus one 8px gap. The old reserve measured the
    // caller's leading space too and added a 6px pad on top, which is where the extra went.
    check("…and the unit costs what its glyphs cost", unitCost < 36.0f,
          "unit reserved " + std::to_string(unitCost) + "px for \"RPM\"");
    check("…leaving most of the box to the number", wideNum > 80.0f,
          "number gets " + std::to_string(wideNum) + "px");

    // Narrow: the unit would eat the digits, so the unit goes.
    sb.setBounds({0, 0, 80, 24});
    JPrimitiveBuffer b2; sb.populateRenderPrimitives(b2);
    check("a narrow box drops the unit, not the value", !sb.unitShown());
    check("…and the number then gets the whole box", sb.numberWidth() > 60.0f,
          "number gets " + std::to_string(sb.numberWidth()) + "px");

    // The decision follows the RANGE, not the value of the moment: a box must not lose its unit the
    // instant a reading ticks from 99 to 100, nor gain one when it falls back.
    sb.setBounds({0, 0, 140, 24});
    sb.setValue(7);
    JPrimitiveBuffer b3; sb.populateRenderPrimitives(b3);
    check("the unit does not flicker with the value", sb.unitShown() && sb.numberWidth() == wideNum);

    std::printf("%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
