// Chart showcase — a live scrolling telemetry chart + a static sweep chart,
// both built on Genesis::Chart over the VectorCanvas 2D layer.
#include <genesis/core/ApplicationCore.h>
#include <genesis/graphics/GpuHal.h>
#include <genesis/graphics/RenderPrimitive.h>
#include <genesis/graphics/Chart.h>
#include <genesis/graphics/FontEngine.h>
#if defined(_WIN32)
#include <genesis/platforms/windows/WindowsPlatformWindow.h>
using PlatformWindowImpl = Genesis::WindowsPlatformWindow;
#else
#include <genesis/platforms/linux/LinuxPlatformWindow.h>
using PlatformWindowImpl = Genesis::LinuxPlatformWindow;
#endif

#include <iostream>
#include <memory>
#include <chrono>
#include <cmath>

using namespace Genesis;

int main() {
    std::cout << "[GENESIS] Chart demo starting...\n";
    constexpr uint32_t W = 820, H = 520;
    uint32_t curW = W, curH = H;
    constexpr float kTitleH = 28.f, kBtnW = 28.f;

    auto window = std::make_unique<PlatformWindowImpl>(
        "Genesis Chart Demo", W, H, 120, 120, Genesis::PlatformWindowStyle::Borderless);
    auto hal = GpuHal::create(GpuApiType::Vulkan, window->nativeHandle());
    if (!hal) { std::cerr << "[GENESIS] no HAL\n"; return -1; }
    hal->resizeSwapchain(W, H);
    window->setMinSize(420, 320);

    Genesis::FontEngine fontEngine;
    if (fontEngine.loadSystemFont()) {
        auto atlas = fontEngine.buildAtlas(13.0f * window->dpiScale());
        Genesis::TextHelper::setAtlas(atlas);
        hal->uploadFontAtlas(atlas.bitmap.data(), atlas.width, atlas.height);
    }

    // Live scrolling telemetry chart (top).
    Chart live;
    live.setTitle("Live Telemetry");
    live.setXWindow(10.0);
    live.setAutoY(true);
    int sRpm   = live.addSeries("RPM",   rgb(80, 200, 255), 2.0f, /*area=*/true);
    int sBoost = live.addSeries("Boost", rgb(255, 170, 60), 2.0f, false);
    live.series(sBoost).secondary = true;            // Boost on the right axis
    live.setAxisTitles("t (s)", "RPM", "Boost (bar)");

    // Static power/torque sweep chart (bottom).
    Chart sweep;
    sweep.setTitle("Power / Torque Sweep");
    sweep.setXRange(1000, 7000);
    sweep.setAutoY(true);
    int sPow = sweep.addSeries("Power (kW)", rgb(120, 230, 120), 2.5f, true);
    int sTrq = sweep.addSeries("Torque (Nm)", rgb(230, 120, 200), 2.5f, false);
    sweep.series(sTrq).secondary = true;             // Torque on the right axis
    sweep.series(sPow).markers = true;               // dots on the power curve
    sweep.setAxisTitles("RPM", "kW", "Nm");
    for (int rpm = 1000; rpm <= 7000; rpm += 250) {
        double x = rpm;
        double tq = 300 + 120 * std::sin((rpm - 1000) / 6000.0 * 3.14159) - (rpm > 5500 ? (rpm - 5500) * 0.05 : 0);
        double kw = tq * rpm / 9549.0;
        sweep.addPoint(sTrq, x, tq);
        sweep.addPoint(sPow, x, kw);
    }

    PrimitiveBuffer buffer;
    auto t0 = std::chrono::steady_clock::now();
    auto last = t0;
    bool closePending = false;

    while (!window->shouldClose()) {
        window->pollNativeEvents();
        auto now = std::chrono::steady_clock::now();
        float t = std::chrono::duration<float>(now - t0).count();

        // Feed the live chart ~60Hz.
        if (std::chrono::duration<float>(now - last).count() > 0.016f) {
            last = now;
            live.addPoint(sRpm,   t, 3500 + 2500 * std::sin(t * 1.3) + 300 * std::sin(t * 7.0));
            live.addPoint(sBoost, t, 1.2 + 0.8 * std::sin(t * 1.3 + 0.6));
        }

        uint32_t nw = window->width(), nh = window->height();
        if (nw && nh && (nw != curW || nh != curH)) { curW = nw; curH = nh; hal->resizeSwapchain(curW, curH); }
        float Wf = (float)curW, Hf = (float)curH;

        float mx = window->mouseX(), my = window->mouseY();
        bool pressed = window->consumePress(), released = window->consumeRelease();
        float closeX = Wf - kBtnW;
        bool inClose = my >= 0 && my < kTitleH && mx >= closeX;
        bool inDrag  = my >= 0 && my < kTitleH && mx < closeX;
        if (pressed) { if (inClose) closePending = true; else if (inDrag) window->startWindowMove(); }
        if (released) { if (closePending && inClose) window->requestClose(); closePending = false; }

        auto frame = hal->beginFrame();
        buffer.clear();

        // Background.
        uint8_t bg[4]{16, 17, 22, 255};
        buffer.pushRectangle(0, kTitleH, Wf, Hf - kTitleH, bg, 0.f);

        float pad = 16.f, gap = 14.f;
        float chartW = Wf - pad * 2;
        float top = kTitleH + pad;
        float chartH = (Hf - top - pad - gap) * 0.5f;
        live.setRect(pad, top, chartW, chartH);
        sweep.setRect(pad, top + chartH + gap, chartW, chartH);

        // Interaction: hover crosshair/tooltip on the chart under the cursor,
        // mouse wheel zooms the sweep chart's X-axis about the cursor.
        bool overLive  = mx >= pad && mx <= pad + chartW && my >= top && my < top + chartH;
        bool overSweep = mx >= pad && mx <= pad + chartW && my >= top + chartH + gap && my <= Hf - pad;
        if (overLive)  { live.setHover(mx, my); } else live.clearHover();
        if (overSweep) { sweep.setHover(mx, my); } else sweep.clearHover();
        float wheel = window->consumeWheel();
        if (wheel != 0.f && overSweep)
            sweep.zoomX(wheel > 0 ? 0.85 : 1.18, sweep.pixelToDataX(mx));

        live.render(buffer);
        sweep.render(buffer);

        // Title bar.
        uint8_t tbg[4]{18, 18, 24, 255};
        buffer.pushRectangle(0, 0, Wf, kTitleH, tbg, 0.f);
        float lh = Genesis::TextHelper::lineHeight();
        uint8_t tc[4]{200, 200, 210, 230};
        Genesis::TextHelper::pushText(buffer, 10.f, (kTitleH - lh) * 0.5f,
                                      "Genesis Chart Demo", tc, Wf - kBtnW - 20.f);
        if (inClose) { uint8_t hb[4]{180, 40, 40, 220}; buffer.pushRectangle(closeX, 0, kBtnW, kTitleH, hb, 0.f); }
        uint8_t xc[4]{220, 220, 230, 235};
        float cx = closeX + kBtnW * 0.5f - 4.f, cy = kTitleH * 0.5f - 1.f;
        buffer.pushRectangle(cx, cy, 9.f, 2.f, xc, 1.f);
        buffer.pushRectangle(cx + 3.5f, cy - 3.5f, 2.f, 9.f, xc, 1.f);

        hal->drawPrimitives(buffer);
        hal->submitAndPresentFrame(frame);
    }
    std::cout << "[GENESIS] Chart demo exiting.\n";
    return 0;
}
