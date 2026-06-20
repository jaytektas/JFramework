#pragma once

#include <genesis/core/ApplicationCore.h>
#include <genesis/core/muted_logging_mock.h>
#include <genesis/core/FocusManager.h>
#include <genesis/graphics/GpuHal.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <vector>
#include <deque>
#include <utility>  // std::pair
#include <unistd.h>

namespace Genesis {

// ---------------------------------------------------------------------------
// WindowStyle — controls decoration and WM interaction for a platform window.
//
//   Normal     : standard WM-decorated window (title bar, borders, WM-draggable)
//   Borderless : WM-managed but no decoration (_MOTIF_WM_HINTS); app draws its
//                own chrome.  Supports WM drag across monitors and fullscreen.
//   Popup      : override_redirect — bypasses WM entirely; app has absolute
//                control of position (great for overlays / secondary monitors).
//                Does NOT support setFullscreen; close detection is internal.
// ---------------------------------------------------------------------------

/**
 * @brief Concrete Linux platform window backed by XCB.
 *
 * Exposes native handles for Vulkan surface creation and polls mouse state
 * via consume* methods so the render loop stays free of raw event queues.
 */
class LinuxPlatformWindow : public Core::PlatformWindow {
public:
    LinuxPlatformWindow(const std::string& title, uint32_t width, uint32_t height,
                        int screenX = 100, int screenY = 100,
                        Genesis::PlatformWindowStyle style = Genesis::PlatformWindowStyle::Normal,
                        xcb_window_t parentWindow = 0,
                        xcb_connection_t* sharedConnection = nullptr)
        : m_screenX(screenX), m_screenY(screenY)
        , m_width(width), m_height(height)
        , m_style(style) {
        std::cout << "[INFO][Platform] LinuxPlatformWindow created: " << title
                  << ", parentWindow: " << parentWindow << ", style: " << (int)style << std::endl;
        if (sharedConnection) {
            m_connection = sharedConnection;
            m_ownsConnection = false;
        } else {
            m_connection = xcb_connect(nullptr, nullptr);
            if (xcb_connection_has_error(m_connection))
                throw std::runtime_error("Failed to open XCB connection.");
            m_ownsConnection = true;
        }

        const xcb_setup_t*     setup  = xcb_get_setup(m_connection);
        xcb_screen_iterator_t  iter   = xcb_setup_roots_iterator(setup);
        xcb_screen_t*          screen = iter.data;
        if (!screen)
            throw std::runtime_error("Failed to obtain XCB screen.");

        m_rootWindow = screen->root;

        // Compute HiDPI scale so logical px == physical px * scale
        float dpi = (static_cast<float>(screen->width_in_pixels) * 25.4f)
                  / static_cast<float>(screen->width_in_millimeters);
        m_dpiScale = (dpi > 0.0f) ? std::max(1.0f, dpi / 96.0f) : 1.0f;

        m_windowId = xcb_generate_id(m_connection);

        const uint32_t eventMask =
            XCB_EVENT_MASK_EXPOSURE        |
            XCB_EVENT_MASK_KEY_PRESS       | XCB_EVENT_MASK_KEY_RELEASE    |
            XCB_EVENT_MASK_BUTTON_PRESS    | XCB_EVENT_MASK_BUTTON_RELEASE |
            XCB_EVENT_MASK_POINTER_MOTION  | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
            XCB_EVENT_MASK_FOCUS_CHANGE;

        if (style == PlatformWindowStyle::Popup) {
            // override_redirect: bypass WM entirely.  Values must be ordered by
            // mask bit position: BACK_PIXEL(1) < OVERRIDE_REDIRECT(9) < EVENT_MASK(11).
            uint32_t mask   = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
            uint32_t vals[] = { screen->black_pixel, 1u, eventMask };
            xcb_create_window(m_connection, XCB_COPY_FROM_PARENT, m_windowId,
                              screen->root,
                              static_cast<int16_t>(screenX), static_cast<int16_t>(screenY),
                              static_cast<uint16_t>(width),  static_cast<uint16_t>(height),
                              0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                              screen->root_visual, mask, vals);
        } else {
            // No BACK_PIXEL → background_pixmap defaults to None, so the X server does
            // NOT erase newly-exposed area to a solid colour when the window grows
            // during a resize.  With a solid background the growing edge flashes black
            // for a frame before Vulkan repaints it; None keeps the prior pixels until
            // we draw, matching how Qt avoids the resize edge-flash.
            uint32_t mask   = XCB_CW_EVENT_MASK;
            uint32_t vals[] = { eventMask };
            xcb_create_window(m_connection, XCB_COPY_FROM_PARENT, m_windowId,
                              screen->root,
                              static_cast<int16_t>(screenX), static_cast<int16_t>(screenY),
                              static_cast<uint16_t>(width),  static_cast<uint16_t>(height),
                              0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                              screen->root_visual, mask, vals);
        }

        xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_windowId,
                            XCB_ATOM_WM_NAME, XCB_ATOM_STRING,
                            8, static_cast<uint32_t>(title.size()), title.c_str());

        // Set WM_CLASS so the WM (e.g. GNOME/Mutter) can correctly group our windows
        {
            const char classStr[] = "genesis-ui\0GenesisUi";
            xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_windowId,
                                XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                                sizeof(classStr), classStr);
        }

        // Set _NET_WM_PID to associate the window with our process
        {
            uint32_t pid = static_cast<uint32_t>(getpid());
            xcb_atom_t pidAtom = _internAtom("_NET_WM_PID");
            if (pidAtom != XCB_ATOM_NONE) {
                xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_windowId,
                                    pidAtom, XCB_ATOM_CARDINAL, 32, 1, &pid);
            }
        }

        // Set WM_CLIENT_LEADER to let the WM group this window under the parent application
        {
            xcb_window_t leaderWin = (parentWindow != 0) ? parentWindow : m_windowId;
            xcb_atom_t leaderAtom = _internAtom("WM_CLIENT_LEADER");
            if (leaderAtom != XCB_ATOM_NONE) {
                xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_windowId,
                                    leaderAtom, XCB_ATOM_WINDOW, 32, 1, &leaderWin);
            }
        }

        if (style == PlatformWindowStyle::Borderless) {
            // Remove WM decorations while staying WM-managed (draggable, fullscreen-capable).
            _applyMotifBorderless();
            if (parentWindow == 0) {
                // Hint the WM that this is a utility tool window (no taskbar, no focus steal).
                _applyWindowType("_NET_WM_WINDOW_TYPE_UTILITY");
            }
        }

        if (parentWindow != 0) {
            // Set transient property before mapping
            xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_windowId,
                                XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW,
                                32, 1, &parentWindow);
            
            // Set window type to NORMAL before mapping to allow proper stacking behavior (transient over parent only)
            _applyWindowType("_NET_WM_WINDOW_TYPE_NORMAL");
        }

        // Wire up WM_DELETE_WINDOW (irrelevant for Popup but harmless).
        xcb_intern_atom_reply_t* protocols_r = xcb_intern_atom_reply(
            m_connection,
            xcb_intern_atom(m_connection, 1, 12, "WM_PROTOCOLS"), nullptr);
        xcb_intern_atom_reply_t* delete_r = xcb_intern_atom_reply(
            m_connection,
            xcb_intern_atom(m_connection, 0, 16, "WM_DELETE_WINDOW"), nullptr);
        if (protocols_r && delete_r) {
            m_deleteWindowAtom = delete_r->atom;
            xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_windowId,
                                protocols_r->atom, XCB_ATOM_ATOM,
                                32, 1, &delete_r->atom);
        }
        free(protocols_r);
        free(delete_r);

        // Load standard cursors from cursor font
        m_cursorFont = xcb_generate_id(m_connection);
        xcb_open_font(m_connection, m_cursorFont, 6, "cursor");

        m_cursorDefault  = _createFontCursor(68);  // left_ptr
        m_cursorHoriz    = _createFontCursor(108); // sb_h_double_arrow
        m_cursorVert     = _createFontCursor(116); // sb_v_double_arrow
        m_cursorTopLeft  = _createFontCursor(134); // top_left_corner
        m_cursorTopRight = _createFontCursor(136); // top_right_corner
        m_cursorBotLeft  = _createFontCursor(12);  // bottom_left_corner
        m_cursorBotRight = _createFontCursor(14);  // bottom_right_corner

        xcb_map_window(m_connection, m_windowId);
        xcb_flush(m_connection);

        qCInfo(Genesis::Log::Platform) << "XCB window created (" << width << "x" << height
                                       << " @ " << screenX << "," << screenY
                                       << ", style=" << static_cast<int>(style)
                                       << ", DPI scale " << m_dpiScale << ")\n";
    }

    ~LinuxPlatformWindow() override {
        if (m_syms) { xcb_key_symbols_free(m_syms); m_syms = nullptr; }
        if (m_connection) {
            xcb_free_cursor(m_connection, m_cursorDefault);
            xcb_free_cursor(m_connection, m_cursorHoriz);
            xcb_free_cursor(m_connection, m_cursorVert);
            xcb_free_cursor(m_connection, m_cursorTopLeft);
            xcb_free_cursor(m_connection, m_cursorTopRight);
            xcb_free_cursor(m_connection, m_cursorBotLeft);
            xcb_free_cursor(m_connection, m_cursorBotRight);
            xcb_close_font(m_connection, m_cursorFont);
            xcb_destroy_window(m_connection, m_windowId);
            if (m_ownsConnection) {
                xcb_disconnect(m_connection);
            }
        }
    }

    // ---- PlatformWindow interface ----
    void pollNativeEvents() override {
        xcb_generic_event_t* ev;
        while ((ev = xcb_poll_for_event(m_connection))) {
            uint8_t type = ev->response_type & ~0x80;
            switch (type) {
                case XCB_MOTION_NOTIFY: {
                    auto* m = reinterpret_cast<xcb_motion_notify_event_t*>(ev);
                    // Input is in the same physical-pixel space as layout and the
                    // swapchain (computeLayout uses physical width/height, the
                    // swapchain is physical).  Do NOT divide by m_dpiScale here or
                    // hit-testing desyncs from rendering whenever scale != 1.0.
                    m_mouseX = static_cast<float>(m->event_x);
                    m_mouseY = static_cast<float>(m->event_y);
                    break;
                }
                case XCB_BUTTON_PRESS: {
                    auto* b = reinterpret_cast<xcb_button_press_event_t*>(ev);
                    // Buttons 4/5 are vertical wheel; 6/7 horizontal. Accumulate a delta.
                    if (b->detail == 4) { m_wheelY += 1.0f; break; }
                    if (b->detail == 5) { m_wheelY -= 1.0f; break; }
                    if (b->detail == XCB_BUTTON_INDEX_1) {
                        m_mouseX = static_cast<float>(b->event_x);
                        m_mouseY = static_cast<float>(b->event_y);
                        m_pendingPress = true;
                        m_altDown = (b->state & XCB_MOD_MASK_1) != 0;
                        if (m_style != PlatformWindowStyle::Popup) {
                            // Grab pointer so we keep receiving MotionNotify and
                            // ButtonRelease even when the cursor leaves the window.
                            // Required for drag-outside-window on X11 and XWayland.
                            xcb_grab_pointer(m_connection, 0, m_windowId,
                                XCB_EVENT_MASK_BUTTON_PRESS   |
                                XCB_EVENT_MASK_BUTTON_RELEASE |
                                XCB_EVENT_MASK_POINTER_MOTION,
                                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                                XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
                            xcb_flush(m_connection);
                        }
                    }
                    break;
                }
                case XCB_BUTTON_RELEASE: {
                    auto* b = reinterpret_cast<xcb_button_release_event_t*>(ev);
                    if (b->detail == XCB_BUTTON_INDEX_1) {
                        m_mouseX = static_cast<float>(b->event_x);
                        m_mouseY = static_cast<float>(b->event_y);
                        m_pendingRelease = true;
                        if (m_style != PlatformWindowStyle::Popup) {
                            xcb_ungrab_pointer(m_connection, XCB_CURRENT_TIME);
                            xcb_flush(m_connection);
                        }
                    }
                    break;
                }
                case XCB_KEY_PRESS:
                case XCB_KEY_RELEASE: {
                    auto* k = reinterpret_cast<xcb_key_press_event_t*>(ev);
                    m_altDown = (k->state & XCB_MOD_MASK_1) != 0;
                    _handleKey(k, type == XCB_KEY_PRESS);
                    break;
                }
                case XCB_CLIENT_MESSAGE: {
                    auto* cm = reinterpret_cast<xcb_client_message_event_t*>(ev);
                    if (cm->data.data32[0] == m_deleteWindowAtom)
                        m_closeRequested = true;
                    break;
                }
                case XCB_CONFIGURE_NOTIFY: {
                    auto* cfg = reinterpret_cast<xcb_configure_notify_event_t*>(ev);
                    if (cfg->event == cfg->window) {
                        // cfg->x/y are relative to the PARENT.  Under a WM the window
                        // is reparented into a decoration frame, so cfg->x/y are
                        // frame-relative, not screen-relative — using them directly
                        // corrupts the screen position after the first resize/move and
                        // offsets floated docks from the cursor.  Translate the window
                        // origin to root coordinates to get the true screen position.
                        _updateRootPosition();
                        if (cfg->width  > 0) m_width  = cfg->width;
                        if (cfg->height > 0) m_height = cfg->height;
                    }
                    break;
                }
                case XCB_DESTROY_NOTIFY:
                    m_closeRequested = true;
                    break;
                case XCB_FOCUS_OUT: {
                    auto* fe = reinterpret_cast<xcb_focus_out_event_t*>(ev);
                    if (fe->detail != XCB_NOTIFY_DETAIL_INFERIOR &&
                        fe->detail != XCB_NOTIFY_DETAIL_POINTER &&
                        fe->mode != XCB_NOTIFY_MODE_GRAB &&
                        fe->mode != XCB_NOTIFY_MODE_WHILE_GRABBED) {
                        m_focusLost = true;
                    }
                    break;
                }
                default: break;
            }
            free(ev);
        }
    }

    bool shouldClose()  const override { return m_closeRequested; }
    bool consumeFocusLost() override { bool v = m_focusLost; m_focusLost = false; return v; }
    void swapBuffers()        override {}
    void setVSync(bool)       override {}

    // ---- Mouse state accessors (consume-once for press/release) ----
    float mouseX() const override { return m_mouseX; }
    float mouseY() const override { return m_mouseY; }
    bool  consumePress()   override { bool v = m_pendingPress;   m_pendingPress   = false; return v; }
    bool  consumeRelease() override { bool v = m_pendingRelease; m_pendingRelease = false; return v; }
    float consumeWheel()   override { float v = m_wheelY; m_wheelY = 0.0f; return v; }  // +up / -down notches

    // ---- Keyboard events (consume-once queue) ----
    bool hasKeyEvents() const override { return !m_keyQueue.empty(); }
    Genesis::KeyEvent consumeKey() override {
        auto e = m_keyQueue.front();
        m_keyQueue.pop_front();
        return e;
    }
    std::vector<Genesis::KeyEvent> consumeAllKeys() override {
        std::vector<Genesis::KeyEvent> out(m_keyQueue.begin(), m_keyQueue.end());
        m_keyQueue.clear();
        return out;
    }

    // ---- Screen position and current size (updated from ConfigureNotify) ----
    int      screenX() const override { return m_screenX; }
    int      screenY() const override { return m_screenY; }
    uint32_t width()   const override { return m_width;   }
    uint32_t height()  const override { return m_height;  }

    Genesis::PlatformWindowStyle windowStyle() const override { return m_style; }
    bool        isAltDown()   const override { return m_altDown; }

    void setTransientParent(xcb_window_t parent) {
        if (parent != 0) {
            xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_windowId,
                                XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW,
                                32, 1, &parent);
            xcb_atom_t leaderAtom = _internAtom("WM_CLIENT_LEADER");
            if (leaderAtom != XCB_ATOM_NONE) {
                xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_windowId,
                                    leaderAtom, XCB_ATOM_WINDOW, 32, 1, &parent);
            }
            _applyWindowType("_NET_WM_WINDOW_TYPE_NORMAL");
            xcb_flush(m_connection);
        }
    }

    void setCursor(Genesis::PlatformCursor shape) override {
        if (m_currentCursor == shape) return;
        m_currentCursor = shape;
        xcb_cursor_t cursorId = 0;
        switch (shape) {
            case Genesis::PlatformCursor::Default:           cursorId = m_cursorDefault; break;
            case Genesis::PlatformCursor::ResizeLeftRight:   cursorId = m_cursorHoriz;   break;
            case Genesis::PlatformCursor::ResizeUpDown:      cursorId = m_cursorVert;    break;
            case Genesis::PlatformCursor::ResizeTopLeft:     cursorId = m_cursorTopLeft; break;
            case Genesis::PlatformCursor::ResizeTopRight:    cursorId = m_cursorTopRight;break;
            case Genesis::PlatformCursor::ResizeBottomLeft:  cursorId = m_cursorBotLeft; break;
            case Genesis::PlatformCursor::ResizeBottomRight: cursorId = m_cursorBotRight;break;
        }
        if (cursorId != 0) {
            uint32_t values[] = { cursorId };
            xcb_change_window_attributes(m_connection, m_windowId, XCB_CW_CURSOR, values);
            xcb_flush(m_connection);
        }
    }

    // Move the window to an absolute screen position.
    // Effective immediately for Popup (override_redirect) windows.
    // For Normal/Borderless, sends a ConfigureWindow request — the WM may
    // reposition it, but most WMs honour application position requests.
    void setPosition(int x, int y) override {
        m_screenX = x;
        m_screenY = y;
        uint32_t vals[] = { static_cast<uint32_t>(x), static_cast<uint32_t>(y) };
        xcb_configure_window(m_connection, m_windowId,
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
        xcb_flush(m_connection);
    }

    // Resize the window.
    void setSize(uint32_t w, uint32_t h) override {
        uint32_t vals[] = { w, h };
        xcb_configure_window(m_connection, m_windowId,
            XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
        xcb_flush(m_connection);
    }

    // Request fullscreen via _NET_WM_STATE_FULLSCREEN (WM-managed windows only).
    // The window fullscreens on whichever monitor it currently occupies — to
    // fullscreen on a secondary monitor, call setPosition() first to move it there.
    void setFullscreen(bool on) override {
        if (m_style == Genesis::PlatformWindowStyle::Popup) return;  // WM not involved
        xcb_atom_t stateAtom = _internAtom("_NET_WM_STATE");
        xcb_atom_t fullAtom  = _internAtom("_NET_WM_STATE_FULLSCREEN");
        if (stateAtom == XCB_ATOM_NONE || fullAtom == XCB_ATOM_NONE) return;

        xcb_client_message_event_t ev{};
        ev.response_type  = XCB_CLIENT_MESSAGE;
        ev.type           = stateAtom;
        ev.window         = m_windowId;
        ev.format         = 32;
        ev.data.data32[0] = on ? 1u : 0u;  // _NET_WM_STATE_ADD / REMOVE
        ev.data.data32[1] = fullAtom;
        ev.data.data32[2] = XCB_ATOM_NONE;
        ev.data.data32[3] = 0u;
        ev.data.data32[4] = 0u;

        xcb_send_event(m_connection, 0, m_rootWindow,
                       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char*>(&ev));
        xcb_flush(m_connection);
    }

    // Returns the total virtual desktop extent (union of all monitors).
    // On a multi-monitor setup (e.g. two 1920×1080 side by side) this gives
    // 3840×1080.  Use to determine coordinates for secondary monitors.
    std::pair<int,int> virtualDesktopSize() const override {
        auto cookie = xcb_get_geometry(m_connection, m_rootWindow);
        auto* geom  = xcb_get_geometry_reply(m_connection, cookie, nullptr);
        if (!geom) return {1920, 1080};
        std::pair<int,int> sz{geom->width, geom->height};
        free(geom);
        return sz;
    }

    // Recompute the window's true top-left in root (screen) coordinates by
    // translating its (0,0) through the X server.  Works regardless of WM
    // reparenting, unlike ConfigureNotify's parent-relative cfg->x/y.
    void _updateRootPosition() {
        auto cookie = xcb_translate_coordinates(m_connection, m_windowId,
                                                m_rootWindow, 0, 0);
        auto* reply = xcb_translate_coordinates_reply(m_connection, cookie, nullptr);
        if (!reply) return;
        m_screenX = static_cast<int>(reply->dst_x);
        m_screenY = static_cast<int>(reply->dst_y);
        free(reply);
    }

    // Returns the current global cursor position in screen coordinates by
    // querying the root window.  Reliable on Wayland/XWayland even when
    // individual window ButtonRelease events are swallowed by the compositor.
    std::pair<int,int> globalCursorPos() const {
        auto cookie = xcb_query_pointer(m_connection, m_rootWindow);
        auto* reply = xcb_query_pointer_reply(m_connection, cookie, nullptr);
        if (!reply) return {0, 0};
        int rx = reply->root_x, ry = reply->root_y;
        free(reply);
        return {rx, ry};
    }

    // Polls whether the left mouse button is currently held down (button mask
    // bit 8 = XCB_BUTTON_MASK_1).  Works on Wayland because it queries the
    // compositor pointer state rather than relying on event delivery.
    bool isLeftButtonDown() const {
        auto cookie = xcb_query_pointer(m_connection, m_rootWindow);
        auto* reply = xcb_query_pointer_reply(m_connection, cookie, nullptr);
        if (!reply) return false;
        bool down = (reply->mask & 0x100u) != 0;  // XCB_BUTTON_MASK_1
        free(reply);
        return down;
    }

    // ---- Native handles for Vulkan surface creation ----
    xcb_connection_t* nativeConnection() const { return m_connection; }
    xcb_window_t      nativeWindow()     const { return m_windowId; }

    Genesis::NativeWindowHandle nativeHandle() const override {
        Genesis::NativeWindowHandle h{};
        h.apiTarget         = Genesis::GpuApiType::Vulkan;
        h.connectionPointer = m_connection;
        h.windowPointer     = reinterpret_cast<void*>(static_cast<uintptr_t>(m_windowId));
        return h;
    }

private:
    // Intern an X atom by name.  Returns XCB_ATOM_NONE if not found and
    // only_if_exists is true.
    xcb_atom_t _internAtom(const char* name, bool only_if_exists = false) {
        auto cookie = xcb_intern_atom(m_connection, only_if_exists ? 1 : 0,
                                      static_cast<uint16_t>(strlen(name)), name);
        auto* r = xcb_intern_atom_reply(m_connection, cookie, nullptr);
        xcb_atom_t atom = r ? r->atom : XCB_ATOM_NONE;
        free(r);
        return atom;
    }

    // Remove WM title bar/borders via Motif hints (_MOTIF_WM_HINTS).
    // The window stays WM-managed (can be dragged across monitors, fullscreened).
    void _applyMotifBorderless() {
        xcb_atom_t atom = _internAtom("_MOTIF_WM_HINTS");
        if (atom == XCB_ATOM_NONE) return;
        struct MwmHints {
            uint32_t flags{2};        // MWM_HINTS_DECORATIONS
            uint32_t functions{0};
            uint32_t decorations{0};  // 0 = no decorations at all
            int32_t  input_mode{0};
            uint32_t status{0};
        } hints;
        xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_windowId,
                            atom, atom, 32, sizeof(hints) / 4, &hints);
    }

    // Set _NET_WM_WINDOW_TYPE to the given type atom name.
    void _applyWindowType(const char* typeName) {
        xcb_atom_t typeAtom = _internAtom("_NET_WM_WINDOW_TYPE");
        xcb_atom_t valAtom  = _internAtom(typeName);
        if (typeAtom == XCB_ATOM_NONE || valAtom == XCB_ATOM_NONE) return;
        xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_windowId,
                            typeAtom, XCB_ATOM_ATOM, 32, 1, &valAtom);
    }

    void _handleKey(xcb_key_press_event_t* k, bool pressed) {
        if (!m_syms) m_syms = xcb_key_symbols_alloc(m_connection);
        bool shift = (k->state & XCB_MOD_MASK_SHIFT) != 0;
        xcb_keysym_t ks = xcb_key_symbols_get_keysym(m_syms, k->detail, shift ? 1 : 0);

        KeyEvent ev;
        ev.pressed = pressed;
        ev.shift   = shift;
        ev.ctrl    = (k->state & XCB_MOD_MASK_CONTROL) != 0;
        ev.alt     = (k->state & XCB_MOD_MASK_1)       != 0;
        ev.keysym  = static_cast<uint32_t>(ks);

        using K = KeyEvent::Key;
        switch (ks) {
            case 0xFF09: ev.key = ev.shift ? K::BackTab : K::Tab; break;
            case 0xFF0D: ev.key = K::Return;    break;
            case 0x0020:
                ev.key = K::Space;
                ev.utf8[0] = ' ';
                ev.utf8[1] = '\0';
                break;
            case 0xFF1B: ev.key = K::Escape;    break;
            case 0xFF08: ev.key = K::Backspace; break;
            case 0xFFFF: ev.key = K::Delete;    break;
            case 0xFF51: ev.key = K::Left;      break;
            case 0xFF52: ev.key = K::Up;        break;
            case 0xFF53: ev.key = K::Right;     break;
            case 0xFF54: ev.key = K::Down;      break;
            case 0xFF50: ev.key = K::Home;      break;
            case 0xFF57: ev.key = K::End;       break;
            default: {
                uint32_t cp = 0;
                if ((ks >= 0x0020 && ks <= 0x007e) || (ks >= 0x00a0 && ks <= 0x00ff)) {
                    cp = ks;
                } else if (ks >= 0x01000100 && ks <= 0x0110ffff) {
                    cp = ks - 0x01000000;
                }
                if (cp != 0) {
                    if (cp < 0x80) {
                        ev.utf8[0] = static_cast<char>(cp);
                        ev.utf8[1] = '\0';
                        ev.key = static_cast<K>(cp);
                    } else if (cp < 0x800) {
                        ev.utf8[0] = static_cast<char>((cp >> 6) | 0xc0);
                        ev.utf8[1] = static_cast<char>((cp & 0x3f) | 0x80);
                        ev.utf8[2] = '\0';
                    } else if (cp < 0x10000) {
                        ev.utf8[0] = static_cast<char>((cp >> 12) | 0xe0);
                        ev.utf8[1] = static_cast<char>(((cp >> 6) & 0x3f) | 0x80);
                        ev.utf8[2] = static_cast<char>((cp & 0x3f) | 0x80);
                        ev.utf8[3] = '\0';
                    } else if (cp < 0x110000) {
                        ev.utf8[0] = static_cast<char>((cp >> 18) | 0xf0);
                        ev.utf8[1] = static_cast<char>(((cp >> 12) & 0x3f) | 0x80);
                        ev.utf8[2] = static_cast<char>(((cp >> 6) & 0x3f) | 0x80);
                        ev.utf8[3] = static_cast<char>((cp & 0x3f) | 0x80);
                        ev.utf8[4] = '\0';
                    }
                }
                break;
            }
        }

        m_keyQueue.push_back(ev);
    }

    xcb_connection_t*   m_connection{nullptr};
    bool                m_ownsConnection{true};
    xcb_window_t        m_windowId{0};
    xcb_window_t        m_rootWindow{0};
    xcb_atom_t          m_deleteWindowAtom{0};
    xcb_key_symbols_t*  m_syms{nullptr};

    PlatformWindowStyle m_style{PlatformWindowStyle::Normal};
    float m_dpiScale{1.0f};
    int      m_screenX{0};
    int      m_screenY{0};
    uint32_t m_width{0};
    uint32_t m_height{0};
    float m_mouseX{0.0f};
    float m_mouseY{0.0f};
    float m_wheelY{0.0f};
    bool  m_pendingPress{false};
    bool  m_pendingRelease{false};
    bool  m_closeRequested{false};
    bool  m_focusLost{false};
    bool  m_altDown{false};

    xcb_font_t   m_cursorFont{0};
    xcb_cursor_t m_cursorDefault{0};
    xcb_cursor_t m_cursorHoriz{0};
    xcb_cursor_t m_cursorVert{0};
    xcb_cursor_t m_cursorTopLeft{0};
    xcb_cursor_t m_cursorTopRight{0};
    xcb_cursor_t m_cursorBotLeft{0};
    xcb_cursor_t m_cursorBotRight{0};
    PlatformCursor m_currentCursor{PlatformCursor::Default};

    xcb_cursor_t _createFontCursor(uint16_t shape) {
        xcb_cursor_t cid = xcb_generate_id(m_connection);
        xcb_create_glyph_cursor(m_connection, cid, m_cursorFont, m_cursorFont,
                                shape, shape + 1,
                                0, 0, 0,
                                65535, 65535, 65535);
        return cid;
    }

    std::deque<KeyEvent> m_keyQueue;
};

} // namespace Genesis
