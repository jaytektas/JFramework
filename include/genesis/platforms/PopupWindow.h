#pragma once

#include <genesis/core/BaseWidgets.h>
#include <genesis/core/AiBusHook.h>
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
        if (AiBusHook::emit) AiBusHook::emit(0, "popup.open", "");
    }

    ~PopupWindow() {
        if (AiBusHook::emit) AiBusHook::emit(0, "popup.close", "");
#if defined(_WIN32)
        ReleaseCapture();
#else
        if (m_hasPointerGrab) {
            xcb_connection_t* conn = m_window->nativeConnection();
            xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
            xcb_flush(conn);
        }
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

        auto& l = m_graph.getLayout(m_root);
        l.boundingBox.width = static_cast<float>(m_winW);
        l.boundingBox.height = static_cast<float>(m_winH);
        m_graph.invalidateNode(m_root, DirtySelf);
        m_graph.computeLayout(m_root, { static_cast<float>(m_winW), static_cast<float>(m_winW), 0.0f, 100000.f });
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
            m_hasPointerGrab = true;
#else
            xcb_connection_t* conn = m_window->nativeConnection();
            xcb_window_t      wid  = m_window->nativeWindow();
            xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, wid, XCB_CURRENT_TIME);
            auto grabCookie = xcb_grab_pointer(conn, 0, wid,
                XCB_EVENT_MASK_BUTTON_PRESS   |
                XCB_EVENT_MASK_BUTTON_RELEASE |
                XCB_EVENT_MASK_POINTER_MOTION,
                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
            xcb_flush(conn);
            auto* grabReply = xcb_grab_pointer_reply(conn, grabCookie, nullptr);
            m_hasPointerGrab = grabReply && grabReply->status == XCB_GRAB_STATUS_SUCCESS;
            free(grabReply);
#endif
            m_focusSet = true;
        }

        float mx = m_window->mouseX();
        float my = m_window->mouseY();
        bool  pressed  = m_window->consumePress();
        bool  released = m_window->consumeRelease();

        // Mouse movement cancels keyboard selection
        if (mx != m_lastPollMx || my != m_lastPollMy) {
            m_keyNavIdx = -1;
            m_lastPollMx = mx;
            m_lastPollMy = my;
        }

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

        // Keyboard navigation — arrow keys, Enter, Escape
        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            using K = KeyEvent::Key;
            if (ke.key == K::Escape) {
                out.type = PollResult::Type::Dismissed;
                return out;
            }
            if (ke.key == K::Up)   { _navStep(-1); }
            if (ke.key == K::Down) { _navStep(1); }
            if (ke.key == K::Return || ke.key == K::Space) {
                if (m_keyNavIdx >= 0 && m_keyNavIdx < static_cast<int>(m_widgets.size())) {
                    auto* w = m_widgets[m_keyNavIdx].get();
                    if (w->isVisible() && w->isEnabled()) {
                        const auto& bb = m_graph.getLayoutConst(w->getNodeId()).boundingBox;
                        w->handleMousePress(bb.x + 1.f, bb.y + 1.f);
                        out.activatedNodeId = w->getNodeId();
                    }
                }
            }
        }

        return out;
    }

    // Keyboard-navigate the popup: call from outside if the platform routes keys here.
    // Returns true if the key was consumed.
    bool handleKeyNav(const KeyEvent& ke) {
        if (!ke.pressed) return false;
        using K = KeyEvent::Key;
        if (ke.key == K::Up)   { _navStep(-1); return true; }
        if (ke.key == K::Down) { _navStep(1);  return true; }
        return false;
    }

    int keyNavIdx() const { return m_keyNavIdx; }

    // Poll in floating mode: no grab, no dismiss-on-outside, TearOffHandle
    // area (top kFloatHandleH pixels) drags the window using xcb_query_pointer
    // so no pointer grab is required. The top-right 16×16 corner is a close btn.
    enum class FloatPollResult { Alive, Close };
    FloatPollResult pollFloating() {
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) return FloatPollResult::Close;

        float mx = m_window->mouseX();
        float my = m_window->mouseY();
        bool pressed  = m_window->consumePress();
        bool released = m_window->consumeRelease();

        // On the very first poll after tearoff the press event has already been
        // consumed by pollEvents().  If the button is still held, start dragging
        // immediately so the window follows the cursor with no extra click needed.
        if (!m_floatFirstPollDone) {
            m_floatFirstPollDone = true;
#if !defined(_WIN32)
            if (m_window->isLeftButtonDown()) {
                auto [gx, gy] = m_window->globalCursorPos();
                m_floatDragging   = true;
                m_floatDragStartX = gx;
                m_floatDragStartY = gy;
                m_floatWinStartX  = m_window->screenX();
                m_floatWinStartY  = m_window->screenY();
            }
#endif
        }

        bool inHandle = (my >= 0.f && my <= kFloatHandleH);
        float closeX  = static_cast<float>(m_winW) - kFloatHandleH - 1.f;
        bool inClose  = inHandle && (mx >= closeX && mx < closeX + kFloatHandleH);

        // Close button — must be checked before drag so the click isn't consumed as a drag.
        if (pressed && inClose) return FloatPollResult::Close;

        // Drag-to-move: press in the handle strip (but not close button) starts a move.
        // Consuming the press prevents it reaching TearOffHandle (would fire onTornOff again).
        if (pressed && inHandle) {
#if !defined(_WIN32)
            auto [gx, gy] = m_window->globalCursorPos();
            m_floatDragging   = true;
            m_floatDragStartX = gx;
            m_floatDragStartY = gy;
            m_floatWinStartX  = m_window->screenX();
            m_floatWinStartY  = m_window->screenY();
#endif
            pressed = false;  // consumed — don't pass to widgets
        }

        if (m_floatDragging) {
#if !defined(_WIN32)
            if (m_window->isLeftButtonDown()) {
                auto [gx, gy] = m_window->globalCursorPos();
                int nx = m_floatWinStartX + (gx - m_floatDragStartX);
                int ny = m_floatWinStartY + (gy - m_floatDragStartY);
                m_window->setPosition(nx, ny);
            } else {
                m_floatDragging = false;
            }
#endif
        }

        // Route input to all widgets normally.
        for (auto& w : m_widgets) {
            if (!w->isVisible()) continue;
            w->handleMouseMove(mx, my);
            if (pressed)  w->handleMousePress(mx, my);
            if (released) w->handleMouseRelease(mx, my);
        }
        return FloatPollResult::Alive;
    }

    void render(GpuHal& hal, PrimitiveBuffer& buf) {
        if (m_surfaceW != m_winW || m_surfaceH != m_winH) {
            hal.resizeSurface(m_surface, m_winW, m_winH);
            m_surfaceW = m_winW;
            m_surfaceH = m_winH;
        }

        buf.clear();  // each popup renders to its own surface — start with a clean buffer

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

        // Keyboard-nav selection highlight (drawn on top of normal hover)
        if (m_keyNavIdx >= 0 && m_keyNavIdx < static_cast<int>(m_widgets.size())) {
            auto* w = m_widgets[m_keyNavIdx].get();
            if (w->isVisible()) {
                const auto& bb = m_graph.getLayoutConst(w->getNodeId()).boundingBox;
                uint8_t sel[4]    = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 40};
                uint8_t border[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 160};
                buf.pushRectangle(bb.x, bb.y, bb.width, bb.height, sel, 4.f, 1.f, border);
            }
        }

        Widget::renderTooltips(buf, m_window->mouseX(), m_window->mouseY());

        // Close button — same style as DockWidget title-bar close button.
        if (m_showCloseButton) {
            float cw  = static_cast<float>(m_winW);
            float cx  = cw - kFloatHandleH - 1.f;
            float cy  = (kFloatHandleH - kFloatHandleH) * 0.5f;  // centred in strip
            float hmx = m_window->mouseX(), hmy = m_window->mouseY();
            bool hov  = (hmx >= cx && hmx < cx + kFloatHandleH &&
                         hmy >= 0.f  && hmy < kFloatHandleH);
            const auto& src = hov ? Colors::CloseBtnHover : Colors::CloseBtn;
            uint8_t cbg[4] = {src[0], src[1], src[2], src[3]};
            buf.pushRectangle(cx, cy, kFloatHandleH, kFloatHandleH, cbg, 3.f);
            uint8_t xc[4] = {Colors::CloseBtnMark[0], Colors::CloseBtnMark[1],
                              Colors::CloseBtnMark[2], Colors::CloseBtnMark[3]};
            buf.pushRectangle(cx + 3.f,                    cy + kFloatHandleH * 0.42f,
                              kFloatHandleH - 6.f, 2.5f,   xc, 1.f);
            buf.pushRectangle(cx + kFloatHandleH * 0.42f, cy + 3.f,
                              2.5f, kFloatHandleH - 6.f,   xc, 1.f);
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

    void releasePointerGrab() {
#if defined(_WIN32)
        ReleaseCapture();
#else
        if (m_hasPointerGrab) {
            xcb_ungrab_pointer(m_window->nativeConnection(), XCB_CURRENT_TIME);
            xcb_flush(m_window->nativeConnection());
            m_hasPointerGrab = false;
        }
#endif
    }

    void enableCloseButton() { m_showCloseButton = true; }

    PlatformWinType&       window()       { return *m_window; }
    const PlatformWinType& window() const { return *m_window; }
    bool                   isViewable() const { return m_focusSet; }
    const std::vector<std::unique_ptr<Widget>>& widgets() const { return m_widgets; }

private:
    static constexpr float m_kBorderPad  = 4.f;
    static constexpr float kFloatHandleH = 16.f;

    Style     m_style;
    uint32_t  m_winW, m_winH;

    SceneGraph m_graph;
    NodeId     m_root{0};
    std::vector<std::unique_ptr<Widget>> m_widgets;

    std::unique_ptr<PlatformWinType> m_window;
    GpuSurfaceId m_surface{kPrimarySurface};
    bool m_focusSet{false};
    bool m_hasPointerGrab{false};
    uint32_t m_surfaceW{0}, m_surfaceH{0};
    bool  m_showCloseButton{false};
    bool  m_floatFirstPollDone{false};
    bool  m_floatDragging{false};
    int   m_floatDragStartX{0}, m_floatDragStartY{0};
    int   m_floatWinStartX{0},  m_floatWinStartY{0};
    int   m_keyNavIdx{-1};
    float m_lastPollMx{-1.f}, m_lastPollMy{-1.f};

    // Move the keyboard selection by dir (+1 = down, -1 = up), skipping
    // non-focusable items (separators etc.) and wrapping around.
    void _navStep(int dir) {
        int n = static_cast<int>(m_widgets.size());
        if (n == 0) return;
        int next = m_keyNavIdx < 0 ? (dir > 0 ? -1 : n) : m_keyNavIdx;
        for (int i = 0; i < n; ++i) {
            next = ((next + dir) % n + n) % n;
            if (m_widgets[next]->isVisible() && m_widgets[next]->isFocusable()) {
                m_keyNavIdx = next;
                return;
            }
        }
    }
};

} // namespace Genesis
