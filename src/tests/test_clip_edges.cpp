// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// Clip rectangles on whole pixels: OUTWARD, so a clip edge between pixels keeps that pixel (truncating it
// cut the last row and column off widgets all over every app), and an EMPTY clip draws nothing. The second
// went wrong once the first was fixed: a widget scrolled wholly out of a scroll area is given a zero-height
// clip at its own edge, and rounding that outward made it one pixel tall, so a one-pixel slice of the
// widget showed below the scroll area. Runs on SoftwareGpuHal, which rounds as the Vulkan HAL does.
#include "../graphics/SoftwareGpuHal.h"

#include <cstdio>
#include <string>

using namespace jf;

static int fails = 0;
static void check(const char* what, bool ok, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : " — ", detail.c_str());
    if (!ok) ++fails;
}

// White pixels painted when a white rect at (100,100) 100x50 is drawn under the clip given.
static int painted(SoftwareGpuHal& hal, float cx, float cy, float cw, float ch) {
    uint8_t white[4] = { 255, 255, 255, 255 };
    uint8_t black[4] = { 0, 0, 0, 255 };
    JPrimitiveBuffer buf;
    buf.pushRectangle(0.f, 0.f, 4000.f, 4000.f, black, 0.f);   // the last frame's pixels gone
    buf.pushClip(cx, cy, cw, ch);
    buf.pushRectangle(100.f, 100.f, 100.f, 50.f, white, 0.f);
    buf.popClip();
    auto frame = hal.beginFrame();
    hal.drawPrimitives(buf);
    uint32_t w = 0, h = 0;
    const std::vector<uint32_t>* px = hal.surfacePixels(frame.surfaceId, w, h);
    if (!px) return -1;
    int n = 0;
    for (uint32_t p : *px)
        if (((p >> 16) & 0xFF) > 250 && ((p >> 8) & 0xFF) > 250 && (p & 0xFF) > 250) ++n;
    return n;
}

int main() {
    std::printf("=== clip edges ===\n");
    JNativeWindowHandle handle{};
    SoftwareGpuHal hal(handle);
    if (!hal.initialize()) { std::fprintf(stderr, "hal init failed\n"); return 1; }

    // Rows 100..119.5: the half-covered row 119 is kept, so 20 rows of 100.
    const int partial = painted(hal, 0.f, 0.f, 1000.f, 119.5f);
    check("a clip edge between pixels keeps that pixel", partial == 20 * 100, "got " + std::to_string(partial));
    // A zero-height clip at a fractional y, inside the rect: nothing at all.
    const int empty = painted(hal, 120.f, 120.4f, 50.f, 0.f);
    check("an empty clip paints nothing", empty == 0, "got " + std::to_string(empty));
    const int narrow = painted(hal, 120.6f, 110.f, 0.f, 20.f);
    check("a zero-width clip paints nothing", narrow == 0, "got " + std::to_string(narrow));

    std::printf(fails ? "=== %d FAILED ===\n" : "=== clips cover what they say ===\n", fails);
    return fails ? 1 : 0;
}
