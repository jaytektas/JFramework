#pragma once

#include <genesis/core/BaseWidgets.h>
#include <genesis/graphics/GpuHal.h>
#include <genesis/platforms/linux/LinuxPlatformWindow.h>
#include <vector>
#include <string>
#include <functional>
#include <memory>

namespace Genesis {

/**
 * PopupListWindow — a lightweight override_redirect Vulkan window used for
 * combo-box drop-downs and any other transient list picker.
 *
 * Dismissal policy (single rule):
 *   The popup claims input focus as soon as it opens.
 *   Any FocusOut event — whatever the cause — is treated as a dismissal:
 *     • another app is raised
 *     • the parent app is minimised / moved to background
 *     • the user clicks anywhere outside the popup
 *     • the user clicks back on the combo widget that opened it
 *   No special-casing needed for any of those scenarios.
 *
 * The pointer grab is kept only to receive MotionNotify for hover-highlighting
 * while the cursor is still inside our window.  It is NOT used for outside-click
 * detection (FocusOut handles that entirely).
 */
class PopupListWindow {
public:
    struct PollResult {
        enum class Type { None, Selected, Dismissed } type{Type::None};
        int selectedIndex{-1};
    };

    PopupListWindow(std::vector<std::string> items,
                    int screenX, int screenY,
                    uint32_t width, uint32_t height,
                    GpuHal& hal,
                    xcb_window_t parentWindow = 0)
        : m_window(std::make_unique<LinuxPlatformWindow>(
              "PopupList", width, height, screenX, screenY,
              PlatformWindowStyle::Popup, parentWindow))
        , m_surface(hal.createSurface(m_window->nativeHandle(), width, height))
        , m_winW(width), m_winH(height)
        , m_items(std::move(items))
    {
        xcb_connection_t* conn = m_window->nativeConnection();
        xcb_window_t      wid  = m_window->nativeWindow();

        // Claim keyboard input focus so we receive XCB_FOCUS_OUT when anything
        // else becomes active. XCB_INPUT_FOCUS_POINTER_ROOT means "if the pointer
        // leaves to the root, focus follows" — standard for menus.
        xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, wid, XCB_CURRENT_TIME);

        // Grab the pointer for hover-highlight only (we'll see MotionNotify while
        // inside, and clicks both inside and outside thanks to the grab).
        xcb_grab_pointer(conn, 0, wid,
            XCB_EVENT_MASK_BUTTON_PRESS   |
            XCB_EVENT_MASK_BUTTON_RELEASE |
            XCB_EVENT_MASK_POINTER_MOTION,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
            XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);

        xcb_flush(conn);
    }

    ~PopupListWindow() {
        xcb_connection_t* conn = m_window->nativeConnection();
        xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
        xcb_flush(conn);
    }

    void destroySurface(GpuHal& hal) {
        if (m_surface != kPrimarySurface) {
            hal.destroySurface(m_surface);
            m_surface = kPrimarySurface;
        }
    }

    PollResult pollEvents() {
        // Poll the underlying XCB connection directly so we can catch focus events
        // that LinuxPlatformWindow does not expose.  We call the platform window's
        // pollNativeEvents() for motion/button/close handling, then drain any
        // remaining events ourselves for focus.
        xcb_connection_t* conn = m_window->nativeConnection();

        // First let the platform layer process motion/button/close/resize.
        m_window->pollNativeEvents();

        // Now drain any remaining events for FocusIn/FocusOut.
        xcb_generic_event_t* ev;
        while ((ev = xcb_poll_for_event(conn))) {
            uint8_t type = ev->response_type & ~0x80;
            if (type == XCB_FOCUS_OUT) {
                auto* fe = reinterpret_cast<xcb_focus_out_event_t*>(ev);
                // XCB_NOTIFY_DETAIL_INFERIOR = focus moved to a child window (grab artefact).
                // Anything else means we genuinely lost focus → dismiss.
                if (fe->detail != XCB_NOTIFY_DETAIL_INFERIOR) {
                    free(ev);
                    return PollResult{PollResult::Type::Dismissed};
                }
            }
            // XCB_FOCUS_IN: we regained focus — nothing to do, stay open.
            free(ev);
        }

        // ---- Mouse state from the platform layer ----
        const bool press   = m_window->consumePress();
        const bool release = m_window->consumeRelease();
        float mx = m_window->mouseX();
        float my = m_window->mouseY();

        // If a click lands outside our window, the FocusOut will arrive before or
        // alongside this press; the check above already handles it.  But as a belt-
        // and-suspenders fallback (and for the initial frame before focus is confirmed)
        // also dismiss on an explicit outside-press.
        if (press) {
            if (mx < 0.f || mx > static_cast<float>(m_winW) ||
                my < 0.f || my > static_cast<float>(m_winH)) {
                return PollResult{PollResult::Type::Dismissed};
            }
        }

        // ---- Hover calculation ----
        constexpr float itemH   = 28.0f;
        constexpr float padding = 4.0f;
        int hoverIdx = -1;
        if (mx >= 0.f && mx <= static_cast<float>(m_winW) &&
            my >= padding && my <= static_cast<float>(m_winH) - padding) {
            hoverIdx = static_cast<int>((my - padding) / itemH);
            if (hoverIdx < 0 || hoverIdx >= static_cast<int>(m_items.size()))
                hoverIdx = -1;
        }
        m_hoveredIndex = hoverIdx;

        // ---- Selection on release inside an item ----
        if (release && m_hoveredIndex != -1)
            return PollResult{PollResult::Type::Selected, m_hoveredIndex};

        if (m_window->shouldClose())
            return PollResult{PollResult::Type::Dismissed};

        return PollResult{};
    }

    void render(GpuHal& hal, PrimitiveBuffer& buf) {
        buf.clear();

        // Background card with border
        uint8_t bg[4] = {22, 22, 26, 255};
        buf.pushRectangle(0.f, 0.f,
                          static_cast<float>(m_winW), static_cast<float>(m_winH),
                          bg, 6.0f, 1.0f, Colors::Border);

        constexpr float itemH   = 28.0f;
        constexpr float padding = 4.0f;
        for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
            float iy = padding + static_cast<float>(i) * itemH;
            bool  hovered = (i == m_hoveredIndex);

            if (hovered) {
                buf.pushRectangle(padding, iy,
                                  static_cast<float>(m_winW) - padding * 2.f, itemH,
                                  Colors::Surface3, 4.0f);
            }

            if (TextHelper::hasAtlas()) {
                uint8_t tc[4] = {220, 220, 228, 255};
                float ty = iy + (itemH - TextHelper::lineHeight()) * 0.5f;
                TextHelper::pushText(buf, padding + 8.0f, ty, m_items[i], tc,
                                     static_cast<float>(m_winW) - padding * 2.f - 16.f);
            }
        }

        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
    }

    LinuxPlatformWindow&       window()       { return *m_window; }
    const LinuxPlatformWindow& window() const { return *m_window; }

private:
    std::unique_ptr<LinuxPlatformWindow> m_window;
    GpuSurfaceId  m_surface{kPrimarySurface};
    uint32_t      m_winW, m_winH;
    std::vector<std::string> m_items;
    int m_hoveredIndex{-1};
};

} // namespace Genesis
