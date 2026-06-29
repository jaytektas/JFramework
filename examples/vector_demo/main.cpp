// Vector drawing showcase — exercises the Genesis 2D vector layer
// (gradients, gauges, charts, AA strokes/fills) on the GPU vector pipeline.
#include <j/core/ApplicationCore.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>
#include <j/graphics/VectorGraphics.h>
#include <j/graphics/FontEngine.h>
#if defined(_WIN32)
#include <j/platforms/windows/WindowsPlatformWindow.h>
using PlatformWindowImpl = jf::JWindowsPlatformWindow;
#else
#include <j/platforms/linux/LinuxPlatformWindow.h>
using PlatformWindowImpl = jf::JLinuxPlatformWindow;
#endif

#include <iostream>
#include <memory>
#include <chrono>
#include <cmath>

using namespace jf;

static void buildScene(JVectorCanvas& vg, float W, float H, float t) {
    constexpr float kTitleH = 28.f;
    // Background: vertical gradient panel.
    vg.fillRect(0, kTitleH, W, H - kTitleH,
                JPaint::linear(0, kTitleH, rgb(28, 30, 38), 0, H, rgb(14, 15, 20)));

    // ---- Circular gauge (left) ----
    float gx = 150, gy = 200, gr = 100;
    // track ring
    vg.fillRing(gx, gy, gr - 14, gr, 2.356f, 7.069f, JPaint::solid(rgb(45, 48, 58)));
    // value sweep (animated), gradient green→amber
    float sweep = 2.356f + (7.069f - 2.356f) * (0.5f + 0.4f * std::sin(t));
    vg.strokeArc(gx, gy, gr - 7, 2.356f, sweep, 12.f,
                 JPaint::linear(gx - gr, gy, rgb(0, 210, 120), gx + gr, gy, rgb(250, 180, 0)),
                 JLineCap::Round);
    // hub with radial gradient
    vg.fillCircle(gx, gy, 26, JPaint::radial(gx, gy - 8, 0, rgb(70, 74, 90), 30, rgb(20, 22, 30)));
    // needle
    float na = sweep;
    vg.drawLine(gx, gy, gx + std::cos(na) * (gr - 22), gy + std::sin(na) * (gr - 22),
                4.f, rgb(240, 80, 80), JLineCap::Round);
    vg.fillCircle(gx, gy, 6, JPaint::solid(rgb(240, 80, 80)));

    // ---- Live chart panel (right) ----
    float px = 290, py = 90, pw = W - px - 30, ph = 200;
    vg.fillRoundedRect(px, py, pw, ph, 10, JPaint::solid(rgb(22, 24, 31)));
    vg.strokeRoundedRect(px, py, pw, ph, 10, 1.5f, JPaint::solid(rgb(60, 64, 78)));
    // gridlines
    for (int i = 1; i < 4; ++i)
        vg.drawLine(px + 8, py + ph * i / 4.f, px + pw - 8, py + ph * i / 4.f,
                    1.f, rgb(40, 43, 52));
    // animated waveform polyline with round joins + gradient stroke
    vg.beginPath();
    int N = 80;
    for (int i = 0; i <= N; ++i) {
        float fx = px + 8 + (pw - 16) * (float)i / N;
        float ph2 = ph - 24;
        float fy = py + 12 + ph2 * (0.5f - 0.42f * std::sin(i * 0.30f + t * 1.7f)
                                              * std::cos(i * 0.07f + t));
        if (i == 0) vg.moveTo(fx, fy); else vg.lineTo(fx, fy);
    }
    vg.stroke(2.5f, JPaint::linear(px, 0, rgb(80, 200, 255), px + pw, 0, rgb(180, 120, 255)),
              JLineCap::Round, JLineJoin::Round);

    // ---- Shape strip (bottom) ----
    float by = 330;
    vg.fillCircle(80, by + 30, 34, JPaint::radial(70, by + 18, 0, rgb(255, 90, 90), 40, rgb(120, 0, 40)));
    vg.fillRoundedRect(150, by, 90, 60, 14,
                       JPaint::linear(150, by, rgb(0, 180, 200), 240, by + 60, rgb(0, 80, 160)));
    // filled star (concave-ish via pie petals → use a polygon)
    vg.beginPath();
    float sx = 320, sy = by + 30, R = 36, r = 15;
    for (int i = 0; i < 10; ++i) {
        float a = -1.5708f + i * 3.14159f / 5.f;
        float rr = (i % 2 == 0) ? R : r;
        float vx = sx + std::cos(a) * rr, vy = sy + std::sin(a) * rr;
        if (i == 0) vg.moveTo(vx, vy); else vg.lineTo(vx, vy);
    }
    vg.close();
    vg.stroke(2.5f, rgb(255, 210, 60), JLineCap::Butt, JLineJoin::Round);
    // gradient bar
    vg.fillRect(400, by + 10, W - 430, 40,
                JPaint::linear(400, 0, rgb(255, 0, 128), W, 0, rgb(60, 0, 255)));
}

int main() {
    std::cout << "[GENESIS] Vector demo starting...\n";
    constexpr uint32_t W = 760, H = 420;
    uint32_t curW = W, curH = H;
    constexpr float kTitleH = 28.f, kBtnW = 28.f;

    auto window = std::make_unique<PlatformWindowImpl>(
        "Genesis Vector Demo", W, H, 120, 120, jf::JPlatformWindowStyle::Borderless);
    auto hal = JGpuHal::create(JGpuApiType::Vulkan, window->nativeHandle());
    if (!hal) { std::cerr << "[GENESIS] no HAL\n"; return -1; }
    hal->resizeSwapchain(W, H);
    window->setMinSize(300, 200);

    jf::JFontEngine fontEngine;
    if (fontEngine.loadSystemFont()) {
        auto atlas = fontEngine.buildAtlas(14.0f * window->dpiScale());
        jf::JTextHelper::setAtlas(atlas);
        hal->uploadFontAtlas(atlas.bitmap.data(), atlas.width, atlas.height);
    }

    JPrimitiveBuffer buffer;
    JVectorCanvas vg;
    auto t0 = std::chrono::steady_clock::now();
    bool closePending = false;

    while (!window->shouldClose()) {
        window->pollNativeEvents();
        float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();

        uint32_t nw = window->width(), nh = window->height();
        if (nw && nh && (nw != curW || nh != curH)) { curW = nw; curH = nh; hal->resizeSwapchain(curW, curH); }

        float mx = window->mouseX(), my = window->mouseY();
        bool pressed = window->consumePress(), released = window->consumeRelease();
        float Wf = (float)curW, Hf = (float)curH;
        float closeX = Wf - kBtnW;
        bool inClose = my >= 0 && my < kTitleH && mx >= closeX;
        bool inDrag  = my >= 0 && my < kTitleH && mx < closeX;
        if (pressed) {
            if (inClose) closePending = true;
            else if (inDrag) window->startWindowMove();
        }
        if (released) { if (closePending && inClose) window->requestClose(); closePending = false; }

        auto frame = hal->beginFrame();
        buffer.clear();
        vg.clear();
        buildScene(vg, Wf, Hf, t);
        vg.flush(buffer);

        // Title bar over the top.
        uint8_t tbg[4] = {18, 18, 24, 255};
        buffer.pushRectangle(0, 0, Wf, kTitleH, tbg, 0.f);
        float lh = jf::JTextHelper::lineHeight();
        uint8_t tc[4] = {200, 200, 210, 230};
        jf::JTextHelper::pushText(buffer, 10.f, (kTitleH - lh) * 0.5f,
                                      "Genesis Vector Demo", tc, Wf - kBtnW - 20.f);
        uint8_t hb[4] = {180, 40, 40, 220};
        if (my >= 0 && my < kTitleH && mx >= closeX)
            buffer.pushRectangle(closeX, 0, kBtnW, kTitleH, hb, 0.f);
        uint8_t xc[4] = {220, 220, 230, 235};
        float cx = closeX + kBtnW * 0.5f - 4.f, cy = kTitleH * 0.5f - 1.f;
        buffer.pushRectangle(cx, cy, 9.f, 2.f, xc, 1.f);
        buffer.pushRectangle(cx + 3.5f, cy - 3.5f, 2.f, 9.f, xc, 1.f);

        hal->drawPrimitives(buffer);
        hal->submitAndPresentFrame(frame);
    }
    std::cout << "[GENESIS] Vector demo exiting.\n";
    return 0;
}
