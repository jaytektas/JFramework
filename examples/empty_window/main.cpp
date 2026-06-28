// Minimal empty Genesis window — for testing window-management behavior
// (drag, snap/tile, maximize/restore, minimize, edge-resize) in isolation,
// without any dock content inflating the minimum size.
#include <genesis/core/ApplicationCore.h>
#include <genesis/graphics/GpuHal.h>
#include <genesis/graphics/RenderPrimitive.h>
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

using namespace Genesis;

int main() {
    std::cout << "[GENESIS] Empty Window test starting...\n";

    constexpr uint32_t W = 600, H = 400;
    uint32_t curW = W, curH = H;

    constexpr float kTitleH = 28.f;   // custom title bar height
    constexpr float kBtnW   = 28.f;   // width of each window-control button

    auto window = std::make_unique<PlatformWindowImpl>(
        "Genesis Empty Window", W, H, 100, 100,
        Genesis::PlatformWindowStyle::Borderless);

    NativeWindowHandle handle = window->nativeHandle();
    auto hal = GpuHal::create(GpuApiType::Vulkan, handle);
    if (!hal) { std::cerr << "[GENESIS] Failed to create Vulkan HAL\n"; return -1; }
    hal->resizeSwapchain(W, H);

    Genesis::FontEngine fontEngine;
    if (fontEngine.loadSystemFont()) {
        auto atlas = fontEngine.buildAtlas(14.0f * window->dpiScale());
        Genesis::TextHelper::setAtlas(atlas);
        hal->uploadFontAtlas(atlas.bitmap.data(), atlas.width, atlas.height);
    }

    // Tiny minimum size so the WM can freely tile this window to any half/quarter.
    window->setMinSize(200, 150);

    // ---- Window-management state (mirrors controls_catalog) ----
    bool     titleMoveInitiated  = false;
    float    titlePressLocalXFrac = 0.f;
    float    titlePressLocalY     = 0.f;
    uint32_t titlePressWinW       = 0;
    uint32_t titlePressWinH       = 0;
    bool     closePendingRelease  = false;

    using Clock = std::chrono::steady_clock;
    Clock::time_point lastTitleClick{};

    PrimitiveBuffer buffer;

    while (!window->shouldClose()) {
        window->pollNativeEvents();

        // CSD un-maximize tear-out: restart MOVERESIZE with corrected offset.
        if (titleMoveInitiated) {
            bool resized = window->consumeWasResized();
            bool held    = window->isLeftButtonDown();
            if (resized) {
                uint32_t newW = window->width();
                uint32_t newH = window->height();
                bool widthShrunk   = (newW < titlePressWinW);
                bool fromCSDUnsnap = window->consumeWasUnsnapped();
                if (held && widthShrunk && fromCSDUnsnap) {
                    titleMoveInitiated  = false;
                    closePendingRelease = false;
                    float restoredW = static_cast<float>(newW);
                    auto [cx, cy] = window->globalCursorPos();
                    titlePressLocalXFrac = (restoredW > 0.f)
                        ? static_cast<float>(cx - window->screenX()) / restoredW : 0.f;
                    titlePressLocalY  = static_cast<float>(cy - window->screenY());
                    titlePressWinW    = newW;
                    titlePressWinH    = newH;
                    titleMoveInitiated = true;
                    window->consumeWasResized();
                    window->startWindowMove();
                } else {
                    titleMoveInitiated = false;
                }
            }
        }
        if (!window->isLeftButtonDown()) {
            titleMoveInitiated = false;
        }

        // Mouse position (refresh from the live cursor when the window moved).
        float mouseX_val = window->mouseX();
        float mouseY_val = window->mouseY();
        static int prevWinX = 0, prevWinY = 0;
        int curWinX = window->screenX(), curWinY = window->screenY();
        bool windowMovedThisFrame = (curWinX != prevWinX || curWinY != prevWinY);
        prevWinX = curWinX; prevWinY = curWinY;
        if (windowMovedThisFrame) {
            auto [gx, gy] = window->globalCursorPos();
            mouseX_val = static_cast<float>(gx - window->screenX());
            mouseY_val = static_cast<float>(gy - window->screenY());
        }

        // Track window size → swapchain.
        {
            uint32_t newW = window->width();
            uint32_t newH = window->height();
            if (newW > 0 && newH > 0 && (newW != curW || newH != curH)) {
                curW = newW; curH = newH;
                hal->resizeSwapchain(curW, curH);
            }
        }

        bool pressed  = window->consumePress();
        bool released = window->consumeRelease();

        // ---- Custom title bar — WM-delegated drag/resize ----
        uint32_t wmResizeDir = UINT32_MAX;
        {
            float Wf     = static_cast<float>(curW);
            float Hf     = static_cast<float>(curH);
            float closeX = Wf - kBtnW;
            float maxX   = Wf - kBtnW * 2.f;
            float minX   = Wf - kBtnW * 3.f;

            float tbMx = mouseX_val;
            float tbMy = mouseY_val;
            if (windowMovedThisFrame) {
                auto [tbGx, tbGy] = window->globalCursorPos();
                tbMx = static_cast<float>(tbGx - window->screenX());
                tbMy = static_cast<float>(tbGy - window->screenY());
            }
            bool inBar   = tbMy >= 0.f && tbMy < kTitleH;
            bool inClose = inBar && tbMx >= closeX;
            bool inMax   = inBar && tbMx >= maxX && tbMx < closeX;
            bool inMin   = inBar && tbMx >= minX && tbMx < maxX;
            bool inDrag  = inBar && tbMx < minX;

            if (released) {
                if (closePendingRelease && inClose) window->requestClose();
                closePendingRelease = false;
            }

            if (pressed) {
                if (inClose) {
                    closePendingRelease = true;
                    if (window->isMaximized()) {
                        titleMoveInitiated   = true;
                        titlePressLocalXFrac = (Wf > 0.f) ? mouseX_val / Wf : 0.f;
                        titlePressLocalY     = mouseY_val;
                        titlePressWinW       = window->width();
                        titlePressWinH       = window->height();
                        window->consumeWasResized();
                        window->startWindowMove();
                    }
                } else if (inMax) {
                    window->setMaximized(!window->isMaximized());
                } else if (inMin) {
                    window->minimize();
                } else if (inDrag) {
                    auto clickNow = Clock::now();
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  clickNow - lastTitleClick).count();
                    lastTitleClick = clickNow;
                    if (ms < 300) {
                        titleMoveInitiated = false;
                        window->setMaximized(!window->isMaximized());
                    } else {
                        titleMoveInitiated   = true;
                        titlePressLocalXFrac = (Wf > 0.f) ? mouseX_val / Wf : 0.f;
                        titlePressLocalY     = mouseY_val;
                        titlePressWinW       = window->width();
                        titlePressWinH       = window->height();
                        window->consumeWasResized();
                        window->startWindowMove();
                    }
                } else if (!window->isMaximized()) {
                    constexpr float kEdge = 6.f;
                    constexpr float kCorn = 14.f;
                    bool onLeft   = mouseX_val < kEdge    && mouseY_val >= kTitleH;
                    bool onRight  = mouseX_val >= Wf-kEdge && mouseY_val >= kTitleH;
                    bool onBottom = mouseY_val >= Hf-kEdge;
                    bool onBL     = mouseX_val < kCorn    && mouseY_val >= Hf-kCorn;
                    bool onBR     = mouseX_val >= Wf-kCorn && mouseY_val >= Hf-kCorn;
                    if      (onBL)     wmResizeDir = 6;
                    else if (onBR)     wmResizeDir = 4;
                    else if (onBottom) wmResizeDir = 5;
                    else if (onLeft)   wmResizeDir = 7;
                    else if (onRight)  wmResizeDir = 3;
                    if (wmResizeDir != UINT32_MAX) window->startWindowResize(wmResizeDir);
                }
            }
        }

        // ---- Render: solid body + custom title bar ----
        auto frame = hal->beginFrame();
        buffer.clear();

        float Wf = static_cast<float>(curW);
        float Hf = static_cast<float>(curH);

        // Body background
        uint8_t body[4] = {40, 42, 48, 255};
        buffer.pushRectangle(0.f, kTitleH, Wf, Hf - kTitleH, body, 0.f);

        // Title bar
        uint8_t tbg[4] = {22, 22, 28, 255};
        buffer.pushRectangle(0.f, 0.f, Wf, kTitleH, tbg, 0.f);
        float lh = Genesis::TextHelper::lineHeight();
        uint8_t tc[4] = {190, 190, 200, 220};
        Genesis::TextHelper::pushText(buffer, 10.f, (kTitleH - lh) * 0.5f,
                                      "Genesis Empty Window", tc, Wf - kBtnW * 3.f - 20.f);
        uint8_t sep[4] = {60, 60, 70, 255};
        buffer.pushRectangle(0.f, kTitleH - 1.f, Wf, 1.f, sep, 0.f);

        float closeX = Wf - kBtnW;
        float maxX   = Wf - kBtnW * 2.f;
        float minX   = Wf - kBtnW * 3.f;
        auto hovBtn = [&](float bx) {
            return mouseY_val >= 0.f && mouseY_val < kTitleH &&
                   mouseX_val >= bx  && mouseX_val < bx + kBtnW;
        };
        // Minimize −
        {
            if (hovBtn(minX)) { uint8_t hbg[4]={60,60,70,200}; buffer.pushRectangle(minX, 0.f, kBtnW, kTitleH, hbg, 0.f); }
            float cx = minX + kBtnW * 0.5f - 4.f, cy = kTitleH * 0.5f - 1.f;
            uint8_t ic[4] = {190,190,200,220};
            buffer.pushRectangle(cx, cy, 9.f, 2.f, ic, 0.f);
        }
        // Maximize / restore
        {
            if (hovBtn(maxX)) { uint8_t hbg[4]={60,60,70,200}; buffer.pushRectangle(maxX, 0.f, kBtnW, kTitleH, hbg, 0.f); }
            float cx = maxX + kBtnW * 0.5f - 4.f, cy = kTitleH * 0.5f - 4.f;
            uint8_t ic[4] = {190,190,200,220};
            uint8_t nf[4] = {0,0,0,0};
            uint8_t wbg[4] = {22,22,28,255};
            if (!window->isMaximized()) {
                buffer.pushRectangle(cx, cy, 9.f, 9.f, nf, 1.f, 1.5f, ic);
            } else {
                buffer.pushRectangle(cx + 2.f, cy,       7.f, 7.f, nf,  0.f, 1.f, ic);
                buffer.pushRectangle(cx,        cy + 2.f, 7.f, 7.f, wbg, 0.f, 1.f, ic);
            }
        }
        // Close ×
        {
            if (hovBtn(closeX)) { uint8_t hbg[4]={180,40,40,220}; buffer.pushRectangle(closeX, 0.f, kBtnW, kTitleH, hbg, 0.f); }
            float cx = closeX + kBtnW * 0.5f - 4.f, cy = kTitleH * 0.5f - 1.f;
            uint8_t ic[4] = {210,210,220,230};
            buffer.pushRectangle(cx, cy, 9.f, 2.f, ic, 1.f);
            buffer.pushRectangle(cx + 3.5f, cy - 3.5f, 2.f, 9.f, ic, 1.f);
        }

        hal->drawPrimitives(buffer);
        hal->submitAndPresentFrame(frame);
    }

    std::cout << "[GENESIS] Empty Window test exiting.\n";
    return 0;
}
