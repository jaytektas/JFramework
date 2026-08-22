#pragma once

// ============================================================================
// JDialogWindow — the window half of a modal dialog, written once.
//
// JAppWindow::openModal<T> is duck-typed: it needs kW/kH, NativeWinHandleType,
// pollAndRender and destroySurface, and nothing supplies them. So every dialog
// became a window and hand-rolled the same forty lines — the #if _WIN32 platform
// window, the GPU surface, pollNativeEvents, press/release/wheel, title-bar drag
// through globalCursorPos, its own close-button hit test, a focus manager, key
// routing, begin/draw/present. Seventeen of them in the studio at the last count,
// and the framework's own JColorPickerDialog and JFileDialogWindow do it too.
//
// Derive from this instead and write only the dialog:
//
//   class MyDialog : public jf::JDialogWindow {
//   public:
//       static constexpr uint32_t kW = 400, kH = 200;   // openModal reads these statically
//       MyDialog(Args..., JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
//           : JDialogWindow("My Dialog", kW, kH, hal, sx, sy, parent) {
//           m_field = std::make_unique<JLineEdit>(graph(), "", 150.f);
//           add(m_field.get());                          // routed, focusable, painted
//       }
//   protected:
//       void layout(float w, float h) override { m_field->setBounds({12.f, contentTop(), w - 24.f, rowH()}); }
//       void paint(JPrimitiveBuffer& buf, float w, float h) override { /* labels, frames */ }
//   };
//
// The base owns the chrome and the loop; `layout` places, `paint` draws behind
// the widgets, `onKey` sees keys the focused control did not take. accept() and
// close() end it — Escape and the [x] close by default.
//
// What it deliberately does NOT do: decide the dialog's shape. No implied button
// box, no fixed content margin, no title-bar variant — a dialog that wants those
// composes them, because the reason this class exists is the plumbing, not a
// house style nobody asked for.
// ============================================================================

#include <j/core/JWidget.h>
#include <j/core/JStyle.h>
#include <j/core/JTitleBar.h>
#include <j/core/JCloseButton.h>
#include <j/core/JTextHelper.h>
#include <j/core/FocusManager.h>
#include <j/core/SceneGraph.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

inline namespace jf {

class JDialogWindow {
public:
#if defined(_WIN32)
    using PlatformWinType     = JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    JDialogWindow(std::string title, uint32_t w, uint32_t h, JGpuHal& hal,
                  int sx, int sy, NativeWinHandleType parent)
        : m_title(std::move(title))
        , m_w(w), m_h(h)
        , m_window(std::make_unique<PlatformWinType>(m_title.c_str(), w, h, sx, sy,
                                                     JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), w, h)) {}

    virtual ~JDialogWindow() = default;
    JDialogWindow(const JDialogWindow&) = delete;
    JDialogWindow& operator=(const JDialogWindow&) = delete;

    // openModal's contract.
    void destroySurface(JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(JGpuHal& hal, JPrimitiveBuffer& buf) {
        if (m_done) return false;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) return false;

        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed  = m_window->consumePress();
        const bool released = m_window->consumeRelease();
        const bool held     = m_window->isLeftButtonDown();

        // A resizable dialog's surface follows its window; a fixed one never asks.
        if (m_window->width() != m_w || m_window->height() != m_h) {
            m_w = m_window->width(); m_h = m_window->height();
            hal.resizeSurface(m_surface, m_w, m_h);
        }
        const float W = static_cast<float>(m_w), H = static_cast<float>(m_h);

        // Close [x], then title-bar drag on whatever is left of the bar.
        const JRect cr = closeRect();
        if (pressed && _in(cr, mx, my)) return false;
        const bool inTitle = (my >= 0.f && my < headerH() && mx < cr.x);
        if (held && inTitle && !m_drag) { m_drag = true; m_ax = mx; m_ay = my; }
        if (m_drag) { auto [gx, gy] = m_window->globalCursorPos();
                      m_window->setPosition(gx - int(m_ax), gy - int(m_ay)); }
        if (!held) m_drag = false;

        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            if (onKey(ke)) { if (m_done) return false; continue; }   // the dialog's own keys win
            _refreshFocus();
            if (jRouteKey(ke, m_focus)) continue;                    // focused control, then Tab
            if (ke.key == JKeyEvent::JKey::Escape) return false;
        }

        // Declare this window as the parent for anything opened FROM the dialog (a combo drop-down,
        // a nested modal), so it stacks above rather than behind.
        m_graph.setHostWindow(m_window->screenX(), m_window->screenY(),
                              static_cast<std::uintptr_t>(m_window->rawWindowId()));

        layout(W, H);
        _refreshFocus();
        if (pressed) jRouteMouse(mx, my, m_focus);
        if (!m_focusSeeded) { m_focusSeeded = true; m_focus.focusFirst(); }
        for (JWidget* w : m_widgets) {
            if (!w) continue;
            w->handleMouseMove(mx, my);
            if (pressed)  w->handleMousePress(mx, my);
            if (released) w->handleMouseRelease(mx, my);
        }
        onMouse(mx, my, pressed, released, held);
        if (m_done) return false;

        buf.clear();
        buf.pushRectangle(0.f, 0.f, W, H, Colors::Surface1, JStyle::current().cornerRadius, 1.f, Colors::Border);
        JTitleBar::draw(buf, 0.f, 0.f, W, headerH(), m_title,
                        JStyle::current().cornerRadius, 1, 0.f, cr.width + 14.f);
        JCloseButton::draw(buf, cr, _in(cr, mx, my));
        paint(buf, W, H);                                  // the dialog's own drawing, behind its widgets
        for (JWidget* w : m_widgets) if (w) w->populateRenderPrimitives(buf);
        paintOver(buf, W, H);                              // …and anything that must sit on top

        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
        return true;
    }

protected:
    // ---- what a dialog implements -------------------------------------------------------------
    virtual void layout(float w, float h) = 0;                       // place the widgets, every frame
    virtual void paint(JPrimitiveBuffer&, float, float) {}           // labels/frames behind the widgets
    virtual void paintOver(JPrimitiveBuffer&, float, float) {}       // e.g. a control whose popup overdraws
    virtual bool onKey(const JKeyEvent&) { return false; }           // true = consumed
    virtual void onMouse(float, float, bool, bool, bool) {}          // for hit-testing the dialog draws itself

    // ---- what a dialog gets --------------------------------------------------------------------
    JSceneGraph& graph() { return m_graph; }
    // Register a widget: routed, focusable and painted, in call order. The dialog keeps ownership.
    void add(JWidget* w) { if (w) m_widgets.push_back(w); }
    void close() { m_done = true; }                                  // dismiss after this frame
    uint32_t width()  const { return m_w; }
    uint32_t height() const { return m_h; }
    PlatformWinType& window() { return *m_window; }

    void setResizable(bool on, uint32_t minW = 0, uint32_t minH = 0) {
        m_window->setResizable(on);
        m_window->setResizeTopInset(headerH());                      // the title bar drags, it does not resize
        if (minW && minH) m_window->setMinSize(minW, minH);
    }

    // Chrome metrics, from JStyle — never hardcoded per dialog.
    static float headerH() { return JStyle::current().titleBarHeight; }
    static float rowH()    { return JStyle::current().controlHeight; }
    static float btnH()    { return JStyle::current().buttonHeight; }
    float contentTop() const { return headerH() + 12.f; }
    JRect closeRect() const {
        return JCloseButton::rectFor({0.f, 0.f, static_cast<float>(m_w), headerH()});
    }
    static bool _in(const JRect& r, float mx, float my) {
        return mx >= r.x && mx < r.x + r.width && my >= r.y && my < r.y + r.height;
    }

private:
    void _refreshFocus() {
        std::vector<JWidget*> roots(m_widgets.begin(), m_widgets.end());
        m_focus.setFocusRoots(std::move(roots));
    }

    std::string m_title;
    uint32_t    m_w, m_h;
    std::unique_ptr<PlatformWinType> m_window;
    GpuSurfaceId m_surface{0};
    JSceneGraph  m_graph;                 // before the widgets so it outlives them
    std::vector<JWidget*> m_widgets;
    JFocusManager m_focus;
    bool  m_focusSeeded{false}, m_done{false}, m_drag{false};
    float m_ax{0}, m_ay{0};
};

} // namespace jf
