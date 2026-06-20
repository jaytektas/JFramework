#pragma once

// PopupWindow — a generic, focus-dismissing widget-container popup.
//
// Philosophy:
//   A popup is simply a temporary window that owns its focus.
//   The ONE rule: any FocusOut event, from any cause, closes it immediately.
//   This covers every scenario without special-casing:
//     • App loses focus to another application
//     • User clicks anywhere outside the popup
//     • User clicks back on the widget that opened it
//     • Window manager raises another window
//
// Content:
//   A PopupWindow is a real widget container backed by the same SceneGraph and
//   layout machinery used everywhere else in the toolkit. Any widget can be added
//   to it via add<W>(). Items are arranged in a vertical flow by default.
//
// Styles:
//   Borderless  — just the content; standard for combo-box lists and menus.
//   Bordered    — a visible card/shadow; for tree-pickers, config panels, etc.
//
// Usage (combo-box example):
//   auto popup = std::make_unique<PopupWindow>(x, y, w, hal, PopupWindow::Borderless);
//   for (auto& item : items) {
//       auto* pi = popup->add<PopupItem>(popup->graph(), item, popup->width());
//       pi->onActivated.connect([this, i](){ select(i); closePopup(); });
//   }
//   popup->computeNaturalHeight();   // auto-size to content
//   popup->open(parentXcbWindow);

#include <genesis/core/BaseWidgets.h>
#include <genesis/graphics/GpuHal.h>
#include <genesis/platforms/linux/LinuxPlatformWindow.h>
#include <memory>
#include <vector>
#include <functional>

namespace Genesis {

class PopupWindow {
public:
    enum class Style { Borderless, Bordered };

    // PollResult — returned by pollEvents() every frame.
    struct PollResult {
        enum class Type {
            None,        // nothing happened
            Dismissed,   // focus lost or outside click — destroy me
        } type{Type::None};
        uint32_t activatedNodeId{0};  // widget node id that was clicked/activated
        std::string activatedLabel;   // convenience copy of its label
    };

    // -----------------------------------------------------------------------
    // Construction
    //
    //   screenX/Y    — top-left of the popup in screen coordinates.
    //   width        — fixed width. Height must be set before open() either
    //                  explicitly or via computeNaturalHeight().
    //   hal          — shared Vulkan HAL.
    //   style        — Borderless or Bordered (adds a 1 px border + subtle bg).
    //   parentWindow — XCB parent for transient hint (optional).
    // -----------------------------------------------------------------------
    PopupWindow(int screenX, int screenY,
                uint32_t width, uint32_t height,
                GpuHal& hal,
                Style style = Style::Borderless,
                xcb_window_t parentWindow = 0,
                xcb_connection_t* sharedConnection = nullptr)
        : m_winW(width), m_winH(height), m_style(style)
        , m_window(std::make_unique<LinuxPlatformWindow>(
              "Popup", width, height, screenX, screenY,
              PlatformWindowStyle::Popup, parentWindow, sharedConnection))
        , m_surface(hal.createSurface(m_window->nativeHandle(), 0, 0))
    {
        // Root node — vertical flow container for all popup content.
        m_root = m_graph.createNode("PopupRoot");
        auto& l = m_graph.getLayout(m_root);
        l.boundingBox = { 0.f, 0.f, static_cast<float>(width), static_cast<float>(height) };
    }

    ~PopupWindow() {
        xcb_connection_t* conn = m_window->nativeConnection();
        xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
        xcb_flush(conn);
    }

    // -----------------------------------------------------------------------
    // Content API — add any widget to the popup's vertical layout.
    // -----------------------------------------------------------------------
    template<typename W, typename... Args>
    W* add(Args&&... args) {
        auto uptr = std::make_unique<W>(m_graph, std::forward<Args>(args)...);
        W* ptr = uptr.get();
        m_graph.addChild(m_root, ptr->getNodeId());
        m_widgets.push_back(std::move(uptr));
        return ptr;
    }

    // After adding all content, call this to auto-fit the popup height to the
    // stacked widget minimum heights (saves you pre-calculating it).
    void computeNaturalHeight() {
        m_graph.computeMinSize(m_root);
        float h = m_graph.getLayoutConst(m_root).minHeight;
        if (m_style == Style::Bordered) h += m_kBorderPad * 2.f;
        m_winH = static_cast<uint32_t>(std::max(1.f, h));
        // Resize the XCB window to match.
        uint32_t vals[] = { m_winH };
        xcb_configure_window(m_window->nativeConnection(), m_window->nativeWindow(),
                             XCB_CONFIG_WINDOW_HEIGHT, vals);
        xcb_flush(m_window->nativeConnection());
    }

    SceneGraph& graph()  { return m_graph; }
    NodeId      root()   const { return m_root; }
    uint32_t    width()  const { return m_winW; }
    uint32_t    height() const { return m_winH; }
    Style       style()  const { return m_style; }

    // -----------------------------------------------------------------------
    // Per-frame update — call in the render loop before render().
    // Returns Dismissed when the popup should be destroyed (focus lost, etc.).
    // -----------------------------------------------------------------------
    PollResult pollEvents(GpuHal& hal) {
        // One-time focus and pointer grab set once mapped and viewable
        if (!m_focusSet) {
            auto cookie = xcb_get_window_attributes(m_window->nativeConnection(), m_window->nativeWindow());
            xcb_flush(m_window->nativeConnection());
            xcb_generic_error_t* attrErr = nullptr;
            xcb_get_window_attributes_reply_t* attr = xcb_get_window_attributes_reply(m_window->nativeConnection(), cookie, &attrErr);
            if (attrErr) {
                free(attrErr);
            }
            if (attr) {
                if (attr->map_state == XCB_MAP_STATE_VIEWABLE) {

                    auto grabCookie = xcb_grab_pointer(m_window->nativeConnection(), 0 /*owner_events*/, m_window->nativeWindow(),
                        XCB_EVENT_MASK_BUTTON_PRESS   |
                        XCB_EVENT_MASK_BUTTON_RELEASE |
                        XCB_EVENT_MASK_POINTER_MOTION,
                        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                        XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
                    xcb_flush(m_window->nativeConnection());

                    xcb_generic_error_t* err = nullptr;
                    xcb_grab_pointer_reply_t* grabReply = xcb_grab_pointer_reply(m_window->nativeConnection(), grabCookie, &err);
                    if (err) {
                        free(err);
                    }
                    if (grabReply) {
                        if (grabReply->status == XCB_GRAB_STATUS_SUCCESS) {
                            xcb_set_input_focus(m_window->nativeConnection(), XCB_INPUT_FOCUS_POINTER_ROOT, m_window->nativeWindow(), XCB_CURRENT_TIME);
                            xcb_flush(m_window->nativeConnection());
                            hal.resizeSurface(m_surface, m_winW, m_winH);
                            m_focusSet = true;
                        }
                        free(grabReply);
                    }
                }
                free(attr);
            }
        }

        // 1. Let the platform layer handle motion / button / close.
        m_window->pollNativeEvents();

        // 2. Check if the window lost focus.
        if (m_window->consumeFocusLost()) {
            return {PollResult::Type::Dismissed};
        }

        // 3. Mouse state.
        float mx = m_window->mouseX();
        float my = m_window->mouseY();
        bool press   = m_window->consumePress();
        bool release = m_window->consumeRelease();

        if (press) {
            if (mx < 0.f || mx > static_cast<float>(m_winW) ||
                my < 0.f || my > static_cast<float>(m_winH)) {
                return {PollResult::Type::Dismissed};
            }
        }

        // 4. Layout (every frame — inexpensive since tree is tiny).
        {
            Constraints cc{ static_cast<float>(m_winW), static_cast<float>(m_winW),
                            0.f, 100000.f };
            float originY = (m_style == Style::Bordered) ? m_kBorderPad : 0.f;
            auto& l = m_graph.getLayout(m_root);
            l.boundingBox.x = (m_style == Style::Bordered) ? m_kBorderPad : 0.f;
            l.boundingBox.y = originY;
            m_graph.invalidateNode(m_root, DirtySelf);
            m_graph.computeLayout(m_root, cc);
        }

        // 5. Route mouse events to widgets.
        for (auto& w : m_widgets) {
            if (!w->isVisible()) continue;
            w->handleMouseMove(mx, my);
            if (press)   w->handleMousePress(mx, my);
            if (release) w->handleMouseRelease(mx, my);
        }

        if (m_window->shouldClose())
            return {PollResult::Type::Dismissed};

        return {PollResult::Type::None};
    }

    bool isViewable() const { return m_focusSet; }

    // -----------------------------------------------------------------------
    // Render — call after pollEvents() when Type == None.
    // -----------------------------------------------------------------------
    void render(GpuHal& hal, PrimitiveBuffer& buf) {
        buf.clear();

        // Bordered variant: card background + border.
        if (m_style == Style::Bordered) {
            uint8_t bg[4] = {22, 22, 26, 255};
            buf.pushRectangle(0.f, 0.f,
                              static_cast<float>(m_winW), static_cast<float>(m_winH),
                              bg, 6.0f, 1.0f, Colors::Border);
        } else {
            // Borderless: subtle very-dark background so content is readable.
            uint8_t bg[4] = {18, 18, 22, 250};
            buf.pushRectangle(0.f, 0.f,
                              static_cast<float>(m_winW), static_cast<float>(m_winH),
                              bg, 0.0f);
        }

        // Render each widget.
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

private:
    static constexpr float m_kBorderPad = 4.f;

    Style     m_style;
    uint32_t  m_winW, m_winH;

    SceneGraph m_graph;
    NodeId     m_root{0};
    std::vector<std::unique_ptr<Widget>> m_widgets;

    std::unique_ptr<LinuxPlatformWindow> m_window;
    GpuSurfaceId m_surface{kPrimarySurface};
    bool m_focusSet{false};
};

} // namespace Genesis
