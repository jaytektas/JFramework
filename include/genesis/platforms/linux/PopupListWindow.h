#pragma once

#include <genesis/core/BaseWidgets.h>
#include <genesis/graphics/GpuHal.h>

#if defined(_WIN32)
#include <genesis/platforms/windows/WindowsPlatformWindow.h>
#else
#include <genesis/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <vector>
#include <string>
#include <functional>
#include <memory>

inline namespace jf {

class JPopupListWindow {
public:
    struct JPollResult {
        enum class JType { None, Selected, Dismissed } type{JType::None};
        int selectedIndex{-1};
    };

#if defined(_WIN32)
    using PlatformWinType = JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType = JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    JPopupListWindow(std::vector<std::string> items,
                    int screenX, int screenY,
                    uint32_t width, uint32_t height,
                    JGpuHal& hal,
                    NativeWinHandleType parentWindow = {})
        : m_window(std::make_unique<PlatformWinType>(
              "PopupList", width, height, screenX, screenY,
              JPlatformWindowStyle::Popup, parentWindow))
        , m_surface(hal.createSurface(m_window->nativeHandle(), width, height))
        , m_winW(width), m_winH(height)
        , m_items(std::move(items))
    {
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
    }

    ~JPopupListWindow() {
#if defined(_WIN32)
        ReleaseCapture();
#else
        xcb_connection_t* conn = m_window->nativeConnection();
        xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
        xcb_flush(conn);
#endif
    }

    JPollResult pollEvents(JGpuHal&) {
        m_window->pollNativeEvents();
        JPollResult out{};

        if (m_window->shouldClose() || m_window->consumeFocusLost()) {
            out.type = JPollResult::JType::Dismissed;
            return out;
        }

        float mx = m_window->mouseX();
        float my = m_window->mouseY();
        bool  pressed  = m_window->consumePress();
        bool  released = m_window->consumeRelease();
        (void)released;

        constexpr float itemH   = 28.0f;
        constexpr float padding = 4.0f;

        if (mx >= 0.f && mx < static_cast<float>(m_winW) &&
            my >= 0.f && my < static_cast<float>(m_winH))
        {
            int idx = static_cast<int>((my - padding) / itemH);
            if (idx >= 0 && idx < static_cast<int>(m_items.size())) {
                m_hoveredIndex = idx;
                if (pressed) {
                    out.type          = JPollResult::JType::Selected;
                    out.selectedIndex = idx;
                }
            } else {
                m_hoveredIndex = -1;
            }
        } else {
            m_hoveredIndex = -1;
            if (pressed) {
                out.type = JPollResult::JType::Dismissed;
            }
        }

        return out;
    }

    void render(JGpuHal& hal, JPrimitiveBuffer& buf) {
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

            if (JTextHelper::hasAtlas()) {
                uint8_t tc[4] = {220, 220, 228, 255};
                float ty = iy + (itemH - JTextHelper::lineHeight()) * 0.5f;
                JTextHelper::pushText(buf, padding + 8.0f, ty, m_items[i], tc,
                                     static_cast<float>(m_winW) - padding * 2.f - 16.f);
            }
        }

        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
    }

    void destroySurface(JGpuHal& hal) {
        if (m_surface != kPrimarySurface) {
            hal.destroySurface(m_surface);
            m_surface = kPrimarySurface;
        }
    }

    PlatformWinType&       window()       { return *m_window; }
    const PlatformWinType& window() const { return *m_window; }

private:
    std::unique_ptr<PlatformWinType> m_window;
    GpuSurfaceId  m_surface{kPrimarySurface};
    uint32_t      m_winW, m_winH;
    std::vector<std::string> m_items;
    int m_hoveredIndex{-1};
};

} // inline namespace jf
