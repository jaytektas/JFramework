// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// A border of width N must paint N solid rows.
//
// It did not. The border band is d in (-borderWidth, 0) and the antialiasing ramp used to span the WHOLE
// band, so the only pixel a 1px border has — the one half a pixel inside the edge, d = -0.5 — came out
// half fill and half border. It read as no border at all, and the first width that looked drawn was 2,
// which is one solid row plus a half. This asserts the fix on rasterised pixels rather than on the shader
// source, and it runs on SoftwareGpuHal because that path is an exact translation of rect.frag: if the two
// ever disagree, the software renderer has stopped being a reference for the GPU one.
#include "../graphics/SoftwareGpuHal.h"

#include <cstdio>
#include <cstdlib>

using namespace jf;

static int fails = 0;
static void check(const char* what, bool ok, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : " — ", detail.c_str());
    if (!ok) ++fails;
}

// Rows of border colour down the rect's left edge, counted at its vertical middle.
static int solidBorderRows(SoftwareGpuHal& hal, float bw) {
    uint8_t fill[4]   = { 0, 0, 0, 255 };          // black fill
    uint8_t border[4] = { 255, 255, 255, 255 };    // white border — unmistakable against it
    JPrimitiveBuffer buf;
    buf.pushRectangle(100.f, 100.f, 200.f, 120.f, fill, 0.f, bw, border);

    auto frame = hal.beginFrame();
    hal.drawPrimitives(buf);                        // rasterise only; presenting needs a window

    uint32_t w = 0, h = 0;
    const std::vector<uint32_t>* px = hal.surfacePixels(frame.surfaceId, w, h);
    if (!px) return -1;

    int rows = 0;
    const int y = 160;                              // mid-height, clear of the corners
    for (int x = 100; x < 140; ++x) {
        const uint32_t p = (*px)[y * w + x];
        const int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        if (r > 250 && g > 250 && b > 250) ++rows;   // fully the border colour, not a blend
        else break;                                  // the band is contiguous; stop at the first non-border
    }
    return rows;
}

int main() {
    std::printf("=== border width ===\n");
    JNativeWindowHandle handle{};                   // no window: initialize() only sizes the buffer
    SoftwareGpuHal hal(handle);
    if (!hal.initialize()) { std::fprintf(stderr, "hal init failed\n"); return 1; }

    for (int bw = 1; bw <= 4; ++bw) {
        const int rows = solidBorderRows(hal, static_cast<float>(bw));
        check(("width " + std::to_string(bw) + " paints " + std::to_string(bw) + " solid rows").c_str(),
              rows == bw, "got " + std::to_string(rows));
    }
    // 0 means none — "if (border) draw it, else DON'T", so no stray line at the edge.
    check("width 0 paints nothing", solidBorderRows(hal, 0.f) == 0);

    std::printf(fails ? "=== %d FAILED ===\n" : "=== border widths paint what they say ===\n", fails);
    return fails ? 1 : 0;
}
