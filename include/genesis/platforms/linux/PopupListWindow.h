#pragma once

#include <genesis/core/BaseWidgets.h>
#include <genesis/graphics/GpuHal.h>
#include <genesis/platforms/linux/LinuxPlatformWindow.h>
#include <vector>
#include <string>
#include <functional>
#include <memory>

namespace Genesis {

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
        // Grab the pointer immediately so any click outside dismisses us
        xcb_grab_pointer(m_window->nativeConnection(), 0, m_window->nativeWindow(),
            XCB_EVENT_MASK_BUTTON_PRESS   |
            XCB_EVENT_MASK_BUTTON_RELEASE |
            XCB_EVENT_MASK_POINTER_MOTION,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
            XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(m_window->nativeConnection());
    }

    ~PopupListWindow() {
        xcb_ungrab_pointer(m_window->nativeConnection(), XCB_CURRENT_TIME);
        xcb_flush(m_window->nativeConnection());
    }

    void destroySurface(GpuHal& hal) {
        if (m_surface != kPrimarySurface) {
            hal.destroySurface(m_surface);
            m_surface = kPrimarySurface;
        }
    }

    PollResult pollEvents() {
        m_window->pollNativeEvents();

        const bool btnDown = m_window->isLeftButtonDown();
        const bool press   = m_window->consumePress();
        const bool release = m_window->consumeRelease();

        // Determine if mouse is inside our window
        float mx = m_window->mouseX();
        float my = m_window->mouseY();

        // If a press occurs outside the window, dismiss
        if (press) {
            if (mx < 0.f || mx > static_cast<float>(m_winW) ||
                my < 0.f || my > static_cast<float>(m_winH)) {
                return PollResult{PollResult::Type::Dismissed};
            }
        }

        // Calculate hovered item index
        float itemH = 28.0f;
        float padding = 4.0f;
        int hoverIdx = -1;
        if (mx >= 0.f && mx <= static_cast<float>(m_winW) &&
            my >= padding && my <= static_cast<float>(m_winH) - padding) {
            hoverIdx = static_cast<int>((my - padding) / itemH);
            if (hoverIdx < 0 || hoverIdx >= static_cast<int>(m_items.size())) {
                hoverIdx = -1;
            }
        }
        m_hoveredIndex = hoverIdx;

        // If release happens on a hovered item, select it
        if (release && m_hoveredIndex != -1) {
            return PollResult{PollResult::Type::Selected, m_hoveredIndex};
        }

        if (m_window->shouldClose()) {
            return PollResult{PollResult::Type::Dismissed};
        }

        return PollResult{};
    }

    void render(GpuHal& hal, PrimitiveBuffer& buf) {
        buf.clear();

        // 1. Draw background card/box with border
        uint8_t bg[4] = {22, 22, 26, 255};
        buf.pushRectangle(0.f, 0.f, static_cast<float>(m_winW), static_cast<float>(m_winH),
                          bg, 6.0f, 1.0f, Colors::Border);

        // 2. Render each item
        float itemH = 28.0f;
        float padding = 4.0f;
        for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
            float iy = padding + static_cast<float>(i) * itemH;
            bool hovered = (i == m_hoveredIndex);

            if (hovered) {
                // Draw highlight background
                buf.pushRectangle(padding, iy, static_cast<float>(m_winW) - padding * 2.f, itemH,
                                  Colors::Surface3, 4.0f);
            }

            if (TextHelper::hasAtlas()) {
                uint8_t tc[4] = {220, 220, 228, 255};
                float ty = iy + (itemH - TextHelper::lineHeight()) * 0.5f;
                TextHelper::pushText(buf, padding + 8.0f, ty, m_items[i], tc, static_cast<float>(m_winW) - padding * 2.f - 16.f);
            }
        }

        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
    }

    LinuxPlatformWindow& window() { return *m_window; }
    const LinuxPlatformWindow& window() const { return *m_window; }

private:
    std::unique_ptr<LinuxPlatformWindow> m_window;
    GpuSurfaceId m_surface{kPrimarySurface};
    uint32_t m_winW, m_winH;
    std::vector<std::string> m_items;
    int m_hoveredIndex{-1};
};

} // namespace Genesis
