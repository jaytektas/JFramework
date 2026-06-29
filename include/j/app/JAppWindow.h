#pragma once

// ============================================================================
// jf::JAppWindow — a top-level application window that owns the native window,
// the Vulkan GPU HAL, and the font atlas, and runs the standard render/present/
// resize loop. The application supplies only what is genuinely app-specific —
// how to render its scene and how to dispatch input — via two hooks, and never
// touches the Vulkan / window / font / loop boilerplate.
//
//   JGuiApplication app;
//   JAppWindow win("My App", 1280, 800);
//   win.onRender = [&](JPrimitiveBuffer& buf){ rootWidget.populateRenderPrimitives(buf); };
//   win.onInput  = [&]{ ... dispatch from win.window() ... };
//   return win.run();
//
// This is the framework's answer to per-app Vulkan boilerplate: ~5 lines instead
// of the ~900-line hand-rolled loop a bare app would otherwise carry.
// ============================================================================

#include <j/core/ApplicationCore.h>      // JPlatformWindow
#include <j/core/PlatformCommon.h>       // JPlatformWindowStyle
#include <j/core/GenesisComponents.h>    // JGuiApplication (per-frame service)
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
    JAppWindow(const std::string& title, uint32_t width, uint32_t height,
               JPlatformWindowStyle style = JPlatformWindowStyle::Normal)
        : m_w(width), m_h(height) {
        m_window = createPlatformWindow(title, width, height, 100, 100, style);
        if (!m_window) return;
        m_hal = JGpuHal::create(JGpuApiType::Vulkan, m_window->nativeHandle());
        if (m_hal) m_hal->resizeSwapchain(width, height);

        // Bake + upload a font atlas once (text rendering needs it).
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

    // App hooks.
    std::function<void(JPrimitiveBuffer&)> onRender;            // build this frame's primitives
    std::function<void()>                  onInput;             // dispatch input from window()
    std::function<void(uint32_t,uint32_t)> onResize;            // new client size (px)

    // Standard render/present loop. Returns the process exit code.
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

            if (onInput) onInput();

            // Framework housekeeping: main-thread callbacks + AI semantic bus.
            if (auto* app = JGuiApplication::instance()) app->serviceFrame();

            JPrimitiveBuffer buffer;
            auto frame = m_hal->beginFrame();
            if (onRender) onRender(buffer);
            m_hal->drawPrimitives(buffer);
            m_hal->submitAndPresentFrame(frame);
        }
        m_hal->waitIdle();
        return 0;
    }

private:
    std::unique_ptr<JPlatformWindow> m_window;
    std::unique_ptr<JGpuHal>         m_hal;
    JFontEngine                      m_font;
    uint32_t                         m_w{0}, m_h{0};
};

}  // namespace jf
