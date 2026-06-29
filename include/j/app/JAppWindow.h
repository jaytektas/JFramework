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
#include <j/core/GenesisComponents.h>    // JGuiApplication
#include <j/core/Window.h>               // JFallbackWindowSkin (the frame) + JTextHelper via BaseWidgets
#include <j/core/StyleEngine.h>          // JStyleEngine
#include <j/platforms/PlatformWindow.h>  // createPlatformWindow
#include <j/graphics/GpuHal.h>
#include <j/graphics/FontEngine.h>
#include <j/graphics/RenderPrimitive.h>

#include <cstdint>
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

    int run() {
        if (!valid()) return -1;
        m_window->setVSync(true);
        while (!m_window->shouldClose()) {
            m_window->pollNativeEvents();

            if (m_window->consumeWasResized()) {
                m_w = m_window->width();
                m_h = m_window->height();
                if (m_w && m_h) {
                    m_hal->resizeSwapchain(m_w, m_h);
                    if (onResize) onResize(m_w, m_h);
                }
            }

            const float mx = m_window->mouseX(), my = m_window->mouseY();
            const bool pressed  = m_window->consumePress();
            const bool released = m_window->consumeRelease();
            const bool chromeAte = handleChrome(mx, my, pressed);
            if (onInput) onInput(mx, my, chromeAte ? false : pressed, released);

            if (auto* app = JGuiApplication::instance()) app->serviceFrame();

            JPrimitiveBuffer buffer;
            auto frame = m_hal->beginFrame();
            drawChrome(buffer);
            if (onRender) onRender(buffer);
            m_hal->drawPrimitives(buffer);
            m_hal->submitAndPresentFrame(frame);
        }
        m_hal->waitIdle();
        return 0;
    }

private:
    static constexpr float kBtnW = 44.0f;   // width of each window-control button

    // Hit-test the chrome on a fresh press; act on it. Returns true if it consumed the press.
    bool handleChrome(float mx, float my, bool pressed) {
        if (!pressed) return false;
        const float W = static_cast<float>(m_w), H = static_cast<float>(m_h), e = 6.0f;

        // Resize edges/corners first (dir: 0=TL,1=T,2=TR,3=R,4=BR,5=B,6=BL,7=L).
        const bool L = mx < e, R = mx >= W - e, T = my < e, B = my >= H - e;
        int dir = -1;
        if      (T && L) dir = 0; else if (T && R) dir = 2;
        else if (B && R) dir = 4; else if (B && L) dir = 6;
        else if (T)      dir = 1; else if (R)      dir = 3;
        else if (B)      dir = 5; else if (L)      dir = 7;
        if (dir >= 0) { m_window->startWindowResize(static_cast<uint32_t>(dir)); return true; }

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

    void drawChrome(JPrimitiveBuffer& buf) {
        JFallbackWindowSkin skin;
        JStyleEngine styles(JGuiApplication::instance()->sceneGraph());
        skin.drawFrame(buf, JRect{0.0f, 0.0f, static_cast<float>(m_w), static_cast<float>(m_h)},
                       styles, InvalidNodeId);

        if (!JTextHelper::hasAtlas()) return;
        uint8_t fg[4] = {220, 220, 228, 235};
        const float lh = JTextHelper::lineHeight();
        const float ty = (m_titleH - lh) * 0.5f;
        JTextHelper::pushText(buf, 12.0f, ty, m_title, fg);

        // Window-control glyphs: minimize, maximize, close (right-aligned).
        const float W = static_cast<float>(m_w);
        uint8_t closeBg[4] = {196, 64, 64, 255};
        buf.pushRectangle(W - kBtnW, 0.0f, kBtnW, m_titleH, closeBg, 0.0f);   // close highlight
        auto glyph = [&](float xCenter, const std::string& s) {
            JTextHelper::pushText(buf, xCenter - JTextHelper::measureWidth(s) * 0.5f, ty, s, fg);
        };
        glyph(W - kBtnW * 0.5f,     "x");   // close
        glyph(W - kBtnW * 1.5f,     "[]");  // maximize
        glyph(W - kBtnW * 2.5f,     "_");   // minimize
    }

    std::unique_ptr<JPlatformWindow> m_window;
    std::unique_ptr<JGpuHal>         m_hal;
    JFontEngine                      m_font;
    std::string                      m_title;
    uint32_t                         m_w{0}, m_h{0};
    float                            m_titleH{32.0f};
};

}  // namespace jf
