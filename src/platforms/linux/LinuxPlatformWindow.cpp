#include <genesis/core/ApplicationCore.h>
#include <iostream>
#include <stdexcept>
#include <xcb/xcb.h>
#include <algorithm>

namespace Genesis {

/**
 * @brief Concrete Linux implementation of the platform-agnostic PlatformWindow interface.
 */
class LinuxPlatformWindow : public Core::PlatformWindow {
public:
    LinuxPlatformWindow(Core::Application& app, const std::string& title, uint32_t width, uint32_t height)
        : m_appContext(app), m_connection(nullptr), m_windowId(0), m_closeRequested(false), m_dpiScaleFactor(1.0f) 
    {
        m_connection = xcb_connect(nullptr, nullptr);
        if (xcb_connection_has_error(m_connection)) {
            throw std::runtime_error("Fatal: Genesis failed to open native XCB platform protocol connection.");
        }

        const xcb_setup_t* setup = xcb_get_setup(m_connection);
        xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
        xcb_screen_t* screen = iter.data;

        if (!screen) {
            throw std::runtime_error("Fatal: Failed to parse primary XCB screen graphics context.");
        }

        float dpi = (static_cast<float>(screen->width_in_pixels) * 25.4f) / static_cast<float>(screen->width_in_millimeters);
        if (dpi > 0.0f) {
            m_dpiScaleFactor = std::max(1.0f, dpi / 96.0f);
        }

        m_windowId = xcb_generate_id(m_connection);

        uint32_t valueMask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
        uint32_t valueList[2] = {
            screen->black_pixel,
            XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS | 
            XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS | 
            XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION |
            XCB_EVENT_MASK_STRUCTURE_NOTIFY
        };

        xcb_create_window(
            m_connection,
            XCB_COPY_FROM_PARENT,
            m_windowId,
            screen->root,
            0, 0,
            static_cast<uint16_t>(width * m_dpiScaleFactor),
            static_cast<uint16_t>(height * m_dpiScaleFactor),
            0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT,
            screen->root_visual,
            valueMask,
            valueList
        );

        xcb_map_window(m_connection, m_windowId);
        xcb_flush(m_connection);
    }

    virtual ~LinuxPlatformWindow() {
        if (m_connection) {
            xcb_destroy_window(m_connection, m_windowId);
            xcb_disconnect(m_connection);
        }
    }

    void pollNativeEvents() override {
        if (!m_connection) return;

        xcb_generic_event_t* event = nullptr;
        while ((event = xcb_poll_for_event(m_connection))) {
            uint8_t responseType = event->response_type & ~0x80;

            switch (responseType) {
                case XCB_MOTION_NOTIFY: {
                    auto* motion = reinterpret_cast<xcb_motion_notify_event_t*>(event);
                    Core::InputEvent ie{};
                    ie.type = Core::InputEvent::Type::MouseMove;
                    ie.data.mouse.x = static_cast<double>(motion->event_x) / m_dpiScaleFactor;
                    ie.data.mouse.y = static_cast<double>(motion->event_y) / m_dpiScaleFactor;
                    
                    m_appContext.postToMainThread([ie]() {
                        // Framework logical input routing
                    });
                    break;
                }
                case XCB_BUTTON_PRESS: {
                    auto* btn = reinterpret_cast<xcb_button_press_event_t*>(event);
                    Core::InputEvent ie{};
                    ie.type = Core::InputEvent::Type::MouseButtonDown;
                    ie.data.mouse.buttons = btn->detail;
                    
                    m_appContext.postToMainThread([ie]() {
                        // Root framework hit testing
                    });
                    break;
                }
                case XCB_DESTROY_NOTIFY: {
                    m_closeRequested = true;
                    break;
                }
                default:
                    break;
            }
            free(event);
        }
    }

    void swapBuffers() override {}
    void setVSync(bool enabled) override { (void)enabled; }
    bool shouldClose() const override { return m_closeRequested; }

private:
    Core::Application& m_appContext;
    xcb_connection_t* m_connection;
    xcb_window_t m_windowId;
    bool m_closeRequested;
    float m_dpiScaleFactor;
};

} // namespace Genesis
