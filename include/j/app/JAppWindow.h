#pragma once

// ============================================================================
// jf::JAppWindow — a top-level application window that owns the native window,
// the Vulkan GPU HAL, the font atlas, AND the framework's own window chrome
// (frame + title bar + min/max/close + drag + edge-resize), then runs the
// standard render/present/resize loop.
//
// The window is opened BORDERLESS so the OS chrome is gone and the framework
// draws its own frame (JWindowSkin) — consistent custom look across platforms.
// The application supplies only its content: onRender(buffer) draws into the
// content area (origin at contentTop()), and onInput(mx,my,pressed,released)
// receives clicks the chrome did not consume.
//
//   JGuiApplication app;
//   JAppWindow win("My App", 1280, 800);
//   win.onRender = [&](JPrimitiveBuffer& buf){ ...draw content below win.contentTop()... };
//   win.onInput  = [&](float x,float y,bool p,bool r){ ...dispatch... };
//   return win.run();
// ============================================================================

#include <j/core/ApplicationCore.h>      // JPlatformWindow
#include <j/core/PlatformCommon.h>       // JPlatformWindowStyle
#include <j/core/GenesisComponents.h>    // JGuiApplication, + JTextHelper/Colors via BaseWidgets
#include <j/platforms/PlatformWindow.h>  // createPlatformWindow
#include <j/graphics/GpuHal.h>
#include <j/graphics/FontEngine.h>
#include <j/graphics/RenderPrimitive.h>

#include <cstdint>
#include <chrono>
#include <thread>
#include <functional>
#include <memory>
#include <string>

inline namespace jf {

class JAppWindow {
public:
    JAppWindow(const std::string& title, uint32_t width, uint32_t height)
        : m_title(title), m_w(width), m_h(height) {
        // Borderless: the OS draws no chrome — the framework draws its own frame.
        m_window = createPlatformWindow(title, width, height, 100, 100,
                                        JPlatformWindowStyle::Borderless);
        if (!m_window) return;
        m_window->setMinSize(200, 150);   // declare a sane minimum -> WM treats it resizable
        m_hal = JGpuHal::create(JGpuApiType::Vulkan, m_window->nativeHandle());
        if (m_hal) m_hal->resizeSwapchain(width, height);
        if (m_font.loadSystemFont() && m_hal) {
            auto atlas = m_font.buildAtlas(14.0f * m_window->dpiScale());
            JTextHelper::setAtlas(atlas);
            m_hal->uploadFontAtlas(atlas.bitmap.data(), atlas.width, atlas.height);
        }
    }

    bool valid() const { return m_window && m_hal; }

    JPlatformWindow& window() { return *m_window; }
    JGpuHal&         hal()    { return *m_hal; }
    uint32_t width()  const { return m_w; }
    uint32_t height() const { return m_h; }
    float    contentTop() const { return m_titleH; }   // content y-origin (below the title bar)

    // App hooks.
    std::function<void(JPrimitiveBuffer&)>              onRender;  // draw content (below contentTop())
    std::function<void(float,float,bool,bool)>          onInput;   // mx,my,pressed,released (chrome-filtered)
    std::function<void(uint32_t,uint32_t)>              onResize;  // new client size (px)

    void requestRedraw() { m_needRedraw = true; }   // app asks for a frame (e.g. animation)

    int run() {
        if (!valid()) return -1;
        m_window->setVSync(true);
        int   redraw = 4;                  // frames left to render (armed by activity)
        float lastMx = -1.f, lastMy = -1.f;
        while (!m_window->shouldClose()) {
            m_window->pollNativeEvents();
            bool activity = false;

            // Sync the swapchain to the window's CURRENT size every iteration, by direct
            // comparison rather than the resize flag (which can lag/coalesce). If the
            // swapchain ever trails the window by a frame, the newly exposed edge flashes
            // — the main source of resize flicker. width()/height() keep them in lockstep.
            m_window->consumeWasResized();   // drain the flag; we resize by size-compare
            const uint32_t curW = m_window->width(), curH = m_window->height();
            if (curW > 0 && curH > 0 && (curW != m_w || curH != m_h)) {
                m_w = curW; m_h = curH;
                m_hal->resizeSwapchain(m_w, m_h);
                if (onResize) onResize(m_w, m_h);
                activity = true;
            }

            const float mx = m_window->mouseX(), my = m_window->mouseY();
            m_mx = mx; m_my = my;   // remembered for chrome hover in drawChrome()
            if (!m_window->isMaximized())   // resize-edge cursor feedback (makes edges discoverable)
                m_window->setCursor(cursorForDir(resizeDirAt(mx, my)));
            const bool pressed  = m_window->consumePress();
            const bool released = m_window->consumeRelease();
            if (pressed || released || mx != lastMx || my != lastMy) activity = true;
            // While a button is held (drag / WM resize), render continuously — never sleep
            // through resize events, or the swapchain lags the window and the edge flashes.
            if (m_window->isLeftButtonDown()) activity = true;
            lastMx = mx; lastMy = my;

            const bool chromeAte = handleChrome(mx, my, pressed);
            if (onInput) onInput(mx, my, chromeAte ? false : pressed, released);

            if (auto* app = JGuiApplication::instance()) app->serviceFrame();

            if (m_needRedraw) { activity = true; m_needRedraw = false; }
            if (activity) redraw = 4;

            // Event-driven: present only while armed, idle otherwise. Continuously
            // presenting static content flickered under the compositor — the toolkit's
            // own loop renders in bursts and stays solid, so we do the same.
            if (redraw > 0) {
                --redraw;
                JPrimitiveBuffer buffer;
                auto frame = m_hal->beginFrame();
                drawChrome(buffer);
                if (onRender) onRender(buffer);
                m_hal->drawPrimitives(buffer);
                m_hal->submitAndPresentFrame(frame);
                m_window->swapBuffers();   // frame presented -> echo _NET_WM_SYNC_REQUEST
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
        }
        m_hal->waitIdle();
        return 0;
    }

private:
    static constexpr float kBtnW = 28.0f;   // width of each window-control button (toolkit value)

    // Resize direction under (mx,my): _NET_WM_MOVERESIZE dirs 3=R,4=BR,5=B,6=BL,7=L, or
    // -1. Top edge is intentionally skipped — it conflicts with the title bar (catalog).
    int resizeDirAt(float mx, float my) const {
        const float W = static_cast<float>(m_w), H = static_cast<float>(m_h);
        constexpr float kEdge = 6.0f, kCorn = 14.0f;
        const bool onLeft   = mx < kEdge   && my >= m_titleH;
        const bool onRight  = mx >= W-kEdge && my >= m_titleH;
        const bool onBottom = my >= H-kEdge;
        const bool onBL     = mx < kCorn   && my >= H-kCorn;
        const bool onBR     = mx >= W-kCorn && my >= H-kCorn;
        if (onBL)     return 6;
        if (onBR)     return 4;
        if (onBottom) return 5;
        if (onLeft)   return 7;
        if (onRight)  return 3;
        return -1;
    }

    static JPlatformCursor cursorForDir(int d) {
        switch (d) {
            case 4: return JPlatformCursor::ResizeBottomRight;
            case 6: return JPlatformCursor::ResizeBottomLeft;
            case 5: return JPlatformCursor::ResizeUpDown;
            case 3: case 7: return JPlatformCursor::ResizeLeftRight;
            default: return JPlatformCursor::Default;
        }
    }

    // Hit-test the chrome on a fresh press; act on it. Returns true if it consumed the press.
    bool handleChrome(float mx, float my, bool pressed) {
        if (!pressed) return false;
        const float W = static_cast<float>(m_w);

        if (!m_window->isMaximized()) {
            const int dir = resizeDirAt(mx, my);
            if (dir >= 0) { m_window->startWindowResize(static_cast<uint32_t>(dir)); return true; }
        }

        // Title bar: window-control buttons (right-aligned), else drag.
        if (my < m_titleH) {
            if (mx >= W - kBtnW)     { m_window->requestClose(); return true; }
            if (mx >= W - 2 * kBtnW) { m_window->setMaximized(!m_window->isMaximized()); return true; }
            if (mx >= W - 3 * kBtnW) { m_window->minimize(); return true; }
            m_window->startWindowMove();
            return true;
        }
        return false;
    }

    // The toolkit's own title bar — matches controls_catalog exactly (flat 28px strip,
    // Colors palette, a 1px separator, and vector-drawn min/max/close with hover).
    void drawChrome(JPrimitiveBuffer& buf) {
        const float W  = static_cast<float>(m_w), H = static_cast<float>(m_h);
        const float lh = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() : 14.0f;

        // Opaque full-window background FIRST — every pixel is written each frame, so the
        // swapchain's images can't cycle stale content (that was the resize flicker).
        uint8_t bg[4] = {Colors::Surface0[0], Colors::Surface0[1], Colors::Surface0[2], 255};
        buf.pushRectangle(0.f, 0.f, W, H, bg, 0.f);

        uint8_t tbg[4] = {22, 22, 28, 255};
        buf.pushRectangle(0.f, 0.f, W, m_titleH, tbg, 0.f);

        if (JTextHelper::hasAtlas()) {
            uint8_t tc[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, tc);
            JTextHelper::pushText(buf, 10.f, (m_titleH - lh) * 0.5f, m_title, tc,
                                  W - kBtnW * 3.f - 20.f);
        }
        uint8_t sep[4] = {Colors::Border[0], Colors::Border[1], Colors::Border[2], Colors::Border[3]};
        buf.pushRectangle(0.f, m_titleH - 1.f, W, 1.f, sep, 0.f);

        const float closeX = W - kBtnW, maxX = W - kBtnW * 2.f, minX = W - kBtnW * 3.f;
        auto hov = [&](float bx) {
            return m_my >= 0.f && m_my < m_titleH && m_mx >= bx && m_mx < bx + kBtnW;
        };

        // Minimize  −
        {
            if (hov(minX)) { uint8_t hb[4]={60,60,70,200}; buf.pushRectangle(minX,0.f,kBtnW,m_titleH,hb,0.f); }
            float cx = minX + kBtnW*0.5f - 4.f, cy = m_titleH*0.5f - 1.f;
            uint8_t ic[4] = {190,190,200,220};
            buf.pushRectangle(cx, cy, 9.f, 2.f, ic, 0.f);
        }
        // Maximize □ / restore ⊡
        {
            if (hov(maxX)) { uint8_t hb[4]={60,60,70,200}; buf.pushRectangle(maxX,0.f,kBtnW,m_titleH,hb,0.f); }
            float cx = maxX + kBtnW*0.5f - 4.f, cy = m_titleH*0.5f - 4.f;
            uint8_t ic[4]={190,190,200,220}, nf[4]={0,0,0,0}, wbg[4]={22,22,28,255};
            if (!m_window->isMaximized()) {
                buf.pushRectangle(cx, cy, 9.f, 9.f, nf, 1.f, 1.5f, ic);
            } else {
                buf.pushRectangle(cx+2.f, cy,     7.f, 7.f, nf,  0.f, 1.f, ic);
                buf.pushRectangle(cx,     cy+2.f,  7.f, 7.f, wbg, 0.f, 1.f, ic);
            }
        }
        // Close  ×
        {
            if (hov(closeX)) { uint8_t hb[4]={180,40,40,220}; buf.pushRectangle(closeX,0.f,kBtnW,m_titleH,hb,0.f); }
            float cx = closeX + kBtnW*0.5f - 4.f, cy = m_titleH*0.5f - 1.f;
            uint8_t ic[4] = {210,210,220,230};
            buf.pushRectangle(cx,        cy,       9.f, 2.f, ic, 1.f);
            buf.pushRectangle(cx + 3.5f, cy - 3.5f, 2.f, 9.f, ic, 1.f);
        }
    }

    std::unique_ptr<JPlatformWindow> m_window;
    std::unique_ptr<JGpuHal>         m_hal;
    JFontEngine                      m_font;
    std::string                      m_title;
    uint32_t                         m_w{0}, m_h{0};
    float                            m_mx{-1.0f}, m_my{-1.0f};
    float                            m_titleH{28.0f};
    bool                             m_needRedraw{false};
};

}  // namespace jf
