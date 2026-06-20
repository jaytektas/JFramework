#pragma once

#include <genesis/core/BaseWidgets.h>
#include <genesis/graphics/GpuHal.h>

#if defined(_WIN32)
#include <genesis/platforms/windows/WindowsPlatformWindow.h>
#else
#include <genesis/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <memory>
#include <vector>
#include <functional>

namespace Genesis {

class PopupWindow {
public:
    enum class Style { Borderless, Bordered };

    struct PollResult {
        enum class Type {
            None,
            Dismissed,
        } type{Type::None};
        uint32_t activatedNodeId{0};
        std::string activatedLabel;
    };

#if defined(_WIN32)
    using PlatformWinType = WindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType = LinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    PopupWindow(int screenX, int screenY,
                uint32_t width, uint32_t height,
                GpuHal& hal,
                Style style = Style::Borderless,
                NativeWinHandleType parentWindow = {},
                void* sharedContext = nullptr)
        : m_winW(width), m_winH(height), m_style(style)
        , m_window(std::make_unique<PlatformWinType>(
              "Popup", width, height, screenX, screenY,
              PlatformWindowStyle::Popup, parentWindow,
#if defined(_WIN32)
              static_cast<HINSTANCE>(sharedContext)
#else
              static_cast<xcb_connection_t*>(sharedContext)
#endif
          ))
        , m_surface(hal.createSurface(m_window->nativeHandle(), 0, 0))
    {
        m_root = m_graph.createNode("PopupRoot");
        auto& l = m_graph.getLayout(m_root);
        l.boundingBox = { 0.f, 0.f, static_cast<float>(width), static_cast<float>(height) };
    }

    ~PopupWindow() {
#if defined(_WIN32)
        ReleaseCapture();
#else
        xcb_connection_t* conn = m_window->nativeConnection();
        xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
        xcb_flush(conn);
#endif
    }

    template<typename W, typename... Args>
    W* add(Args&&... args) {
        auto uptr = std::make_unique<W>(m_graph, std::forward<Args>(args)...);
        W* ptr = uptr.get();
        m_graph.addChild(m_root, ptr->getNodeId());
        m_widgets.push_back(std::move(uptr));
        return ptr;
    }

    void computeNaturalHeight() {
        m_graph.computeMinSize(m_root);
        float h = m_graph.getLayoutConst(m_root).minHeight;
        if (m_style == Style::Bordered) h += m_kBorderPad * 2.f;
        m_winH = static_cast<uint32_t>(std::max(1.f, h));
        m_window->setSize(m_winW, m_winH);
    }

    SceneGraph& graph()  { return m_graph; }
    NodeId      root()   const { return m_root; }
    uint32_t    width()  const { return m_winW; }
    uint32_t    height() const { return m_winH; }
    Style       style()  const { return m_style; }

    PollResult pollEvents(GpuHal& hal) {
        m_window->pollNativeEvents();
        PollResult out{};

        if (m_window->shouldClose() || m_window->consumeFocusLost()) {
            out.type = PollResult::Type::Dismissed;
            return out;
        }

        if (!m_focusSet) {
#if defined(_WIN32)
            SetFocus(m_window->nativeWindow());
            SetCapture(m_window->nativeWindow());
#else
            xcb_connection_t* conn = m_window->nativeConnection();
            xcb_window_t      wid  = m_window->nativeWindow();
            xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, wid, XCB_CURRENT_TIME);
            xcb_grab_pointer(conn, 0, wid,
                XCB_EVENT_MASK_BUTTON_PRESS   |
                XCB_EVENT_MASK_BUTTON_RELEASE |
                XCB_EVENT_MASK_POINTER_MOTION,
                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
            xcb_flush(conn);
#endif
            m_focusSet = true;
        }

        float mx = m_window->mouseX();
        float my = m_window->mouseY();
        bool  pressed  = m_window->consumePress();
        bool  released = m_window->consumeRelease();

        bool inside = (mx >= 0.f && mx < static_cast<float>(m_winW) &&
                       my >= 0.f && my < static_cast<float>(m_winH));

        if (!inside && pressed) {
            out.type = PollResult::Type::Dismissed;
            return out;
        }

        uint32_t clickedId = 0;
        std::string clickedLabel;

        for (auto& w : m_widgets) {
            if (!w->isVisible()) continue;
            w->handleMouseMove(mx, my);
            if (pressed) {
                w->handleMousePress(mx, my);
                if (w->hitTest(mx, my)) {
                    clickedId = w->getNodeId();
                    if (auto* pi = dynamic_cast<PopupItem*>(w.get())) {
                        clickedLabel = pi->label();
                    }
                }
            }
            if (released) {
                w->handleMouseRelease(mx, my);
            }
        }

        if (clickedId != 0) {
            out.activatedNodeId = clickedId;
            out.activatedLabel  = clickedLabel;
        }

        return out;
    }

    void render(GpuHal& hal, PrimitiveBuffer& buf) {
        if (m_style == Style::Bordered) {
            uint8_t bg[4] = {22, 22, 26, 255};
            buf.pushRectangle(0.f, 0.f,
                              static_cast<float>(m_winW), static_cast<float>(m_winH),
                              bg, 6.0f, 1.0f, Colors::Border);
        } else {
            uint8_t bg[4] = {18, 18, 22, 250};
            buf.pushRectangle(0.f, 0.f,
                              static_cast<float>(m_winW), static_cast<float>(m_winH),
                              bg, 0.0f);
        }

        for (auto& w : m_widgets) {
            if (w->isVisible())
                w->populateRenderPrimitives(buf);
        }

        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
    }

    void destroySurface(GpuHal& hal) {
        if (m_surface != kPrimarySurface) {
            hal.destroySurface(m_surface);
            m_surface = kPrimarySurface;
        }
    }

    PlatformWinType&       window()       { return *m_window; }
    const PlatformWinType& window() const { return *m_window; }
    bool                   isViewable() const { return m_focusSet; }

private:
    static constexpr float m_kBorderPad = 4.f;

    Style     m_style;
    uint32_t  m_winW, m_winH;

    SceneGraph m_graph;
    NodeId     m_root{0};
    std::vector<std::unique_ptr<Widget>> m_widgets;

    std::unique_ptr<PlatformWinType> m_window;
    GpuSurfaceId m_surface{kPrimarySurface};
    bool m_focusSet{false};
};

} // namespace Genesis
