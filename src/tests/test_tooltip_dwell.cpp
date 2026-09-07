// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// A tooltip appears after a DWELL, and any real movement of the pointer takes it away again.
//
// The dwell used to reset only when the hovered WIDGET changed. Inside one widget the tooltip
// therefore came up once and stayed up, trailing the cursor for as long as the pointer remained over
// that widget — so it sat on top of whatever you moved the mouse to look at. This pins the rule the
// user expects and every other toolkit implements: move more than a few pixels and the tip goes away;
// stand still and it comes back.
//
//   cmake --build build --target test_tooltip_dwell && ./build/test_tooltip_dwell

#include <j/core/JWindowControls.h>
#include <j/core/JButton.h>
#include <j/core/SceneGraph.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace jf;

static int g_fail = 0;
static void check(const char* what, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// Did this frame draw a tooltip? The tip is the only thing renderTooltips can emit, so any primitive
// at all means it was shown — no need to know the shape of the box.
static bool drewTip(JWidget* w, JTooltipHover& hover, float mx, float my) {
    JPrimitiveBuffer buf;
    std::vector<JWidget*> roots{ w };
    JWidget::renderTooltips(buf, hover, roots, mx, my, 1280.f, 700.f);
    return !buf.getCommands().empty();
}

int main() {
    JSceneGraph graph;
    JButton b(graph, "Save");
    b.setBounds({ 100.f, 100.f, 200.f, 40.f });
    b.setTooltip("Write the tune to the ECU");

    const float delayMs = JStyle::current().tooltipDelayMs;
    const float reset   = JStyle::current().tooltipMoveResetPx;
    const auto dwell = [&] { std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<int>(delayMs) + 60)); };

    JTooltipHover hover;

    // 1. Arriving over the widget shows nothing until the dwell has been served.
    check("nothing on arrival", !drewTip(&b, hover, 150.f, 120.f));
    dwell();
    check("shown after the dwell", drewTip(&b, hover, 150.f, 120.f));

    // 2. A REAL MOVEMENT inside the same widget takes it away — this is the behaviour that was
    //    missing, and the reason a tooltip once shown never went away.
    check("hidden by movement", !drewTip(&b, hover, 150.f + reset + 2.f, 120.f));

    // 3. …and standing still at the new place brings it back.
    dwell();
    check("back after standing still", drewTip(&b, hover, 150.f + reset + 2.f, 120.f));

    // 4. JITTER IS NOT MOVEMENT. A mouse reports a pixel of noise while a hand rests on it, and a
    //    tooltip that flickers off for that is worse than one that never hides.
    check("survives sub-threshold jitter", drewTip(&b, hover, 150.f + reset + 2.f + 1.f, 120.f));

    // 5. Leaving the widget hides it, and it does not reappear without a fresh dwell.
    check("hidden off the widget", !drewTip(&b, hover, 900.f, 600.f));
    check("no tip on re-entry", !drewTip(&b, hover, 150.f, 120.f));

    std::printf("\n%s\n", g_fail ? "FAILURES" : "all passed");
    return g_fail ? 1 : 0;
}
