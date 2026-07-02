#pragma once

#include <j/core/ApplicationCore.h>
#include <j/core/muted_logging_mock.h>
#include <j/core/FocusManager.h>
#include <j/graphics/GpuHal.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/sync.h>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <vector>
#include <deque>
#include <utility>  // std::pair
#include <unistd.h>

inline namespace jf {

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
class JLinuxPlatformWindow : public jf::JPlatformWindow {
public:
    JLinuxPlatformWindow(const std::string& title, uint32_t width, uint32_t height,
                        int screenX = 100, int screenY = 100,
                        jf::JPlatformWindowStyle style = jf::JPlatformWindowStyle::Normal,
                        xcb_window_t parentWindow = 0,
                        xcb_connection_t* sharedConnection = nullptr)
        : m_screenX(screenX), m_screenY(screenY)
        , m_width(width), m_height(height)
        , m_style(style) {
        std::cout << "[INFO][Platform] JLinuxPlatformWindow created: " << title
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
            XCB_EVENT_MASK_FOCUS_CHANGE    | XCB_EVENT_MASK_LEAVE_WINDOW   |
            XCB_EVENT_MASK_PROPERTY_CHANGE;

        if (style == JPlatformWindowStyle::Popup) {
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

        if (style == JPlatformWindowStyle::Borderless) {
            // Remove WM decorations while staying WM-managed (draggable, fullscreen-capable).
            _applyMotifBorderless();
            // Keep NORMAL window type so the WM still handles _NET_WM_MOVERESIZE,
            // maximize, and snap — UTILITY windows are excluded from those by many WMs.
        }

        if (parentWindow != 0) {
            // Transient + DIALOG type: WM stacks above parent, grants keyboard focus.
            xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_windowId,
                                XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW,
                                32, 1, &parentWindow);
            _applyWindowType("_NET_WM_WINDOW_TYPE_DIALOG");
        }

        // Wire up WM_DELETE_WINDOW (irrelevant for Popup but harmless).
        xcb_intern_atom_reply_t* protocols_r = xcb_intern_atom_reply(
            m_connection,
            xcb_intern_atom(m_connection, 1, 12, "WM_PROTOCOLS"), nullptr);
        xcb_intern_atom_reply_t* delete_r = xcb_intern_atom_reply(
            m_connection,
            xcb_intern_atom(m_connection, 0, 16, "WM_DELETE_WINDOW"), nullptr);
        m_syncRequestAtom = _internAtom("_NET_WM_SYNC_REQUEST");
        if (protocols_r && delete_r) {
            m_deleteWindowAtom = delete_r->atom;
            // Advertise BOTH WM_DELETE_WINDOW and _NET_WM_SYNC_REQUEST.
            xcb_atom_t protos[2] = { delete_r->atom, m_syncRequestAtom };
            xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_windowId,
                                protocols_r->atom, XCB_ATOM_ATOM,
                                32, 2, protos);
        }
        free(protocols_r);
        free(delete_r);

        // _NET_WM_SYNC_REQUEST counter (basic protocol): after we render a frame for a
        // requested resize, we set this counter to the value the WM sent. The compositor
        // waits for that before showing the new size, so the resized window is never
        // displayed before we've drawn it — eliminates the resize edge-flash.
        m_syncCounter = xcb_generate_id(m_connection);
        xcb_sync_int64_t zero{0, 0};
        xcb_sync_create_counter(m_connection, m_syncCounter, zero);
        xcb_atom_t syncCounterAtom = _internAtom("_NET_WM_SYNC_REQUEST_COUNTER");
        xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_windowId,
                            syncCounterAtom, XCB_ATOM_CARDINAL, 32, 1, &m_syncCounter);

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

        qCInfo(jf::Log::Platform) << "XCB window created (" << width << "x" << height
                                       << " @ " << screenX << "," << screenY
                                       << ", style=" << static_cast<int>(style)
                                       << ", DPI scale " << m_dpiScale << ")\n";
    }

    ~JLinuxPlatformWindow() override {
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

    // ---- JPlatformWindow interface ----
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
                        m_altDown   = (b->state & XCB_MOD_MASK_1) != 0;
                        m_ctrlDown  = (b->state & XCB_MOD_MASK_CONTROL) != 0;
                        m_shiftDown = (b->state & XCB_MOD_MASK_SHIFT) != 0;
                        if (m_style != JPlatformWindowStyle::Popup) {
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
                    } else if (b->detail == XCB_BUTTON_INDEX_3) {
                        m_mouseX = static_cast<float>(b->event_x);
                        m_mouseY = static_cast<float>(b->event_y);
                        m_pendingRightPress = true;
                    }
                    break;
                }
                case XCB_BUTTON_RELEASE: {
                    auto* b = reinterpret_cast<xcb_button_release_event_t*>(ev);
                    if (b->detail == XCB_BUTTON_INDEX_1) {
                        m_mouseX = static_cast<float>(b->event_x);
                        m_mouseY = static_cast<float>(b->event_y);
                        m_pendingRelease = true;
                        if (m_style != JPlatformWindowStyle::Popup) {
                            xcb_ungrab_pointer(m_connection, XCB_CURRENT_TIME);
                            xcb_flush(m_connection);
                        }
                    } else if (b->detail == XCB_BUTTON_INDEX_3) {
                        m_mouseX = static_cast<float>(b->event_x);
                        m_mouseY = static_cast<float>(b->event_y);
                        m_pendingRightRelease = true;
                    }
                    break;
                }
                case XCB_KEY_PRESS:
                case XCB_KEY_RELEASE: {
                    auto* k = reinterpret_cast<xcb_key_press_event_t*>(ev);
                    m_altDown   = (k->state & XCB_MOD_MASK_1) != 0;
                    m_ctrlDown  = (k->state & XCB_MOD_MASK_CONTROL) != 0;
                    m_shiftDown = (k->state & XCB_MOD_MASK_SHIFT) != 0;
                    _handleKey(k, type == XCB_KEY_PRESS);
                    break;
                }
                case XCB_CLIENT_MESSAGE: {
                    auto* cm = reinterpret_cast<xcb_client_message_event_t*>(ev);
                    if (cm->data.data32[0] == m_deleteWindowAtom) {
                        m_closeRequested = true;
                    } else if (cm->data.data32[0] == m_syncRequestAtom) {
                        // WM asks us to echo this counter value once we've drawn the
                        // (about-to-be-resized) frame. data32[2]=lo, data32[3]=hi.
                        m_syncValueLo = cm->data.data32[2];
                        m_syncValueHi = cm->data.data32[3];
                        m_syncPending = true;
                    }
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
                        bool wChanged = cfg->width  > 0 && cfg->width  != m_width;
                        bool hChanged = cfg->height > 0 && cfg->height != m_height;
                        if (wChanged || hChanged) {
                            m_wasResized = true;
                        }
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
                case XCB_LEAVE_NOTIFY: {
                    auto* le = reinterpret_cast<xcb_leave_notify_event_t*>(ev);
                    // Only reset for genuine cursor-exit, not grab-induced leaves.
                    if (le->mode == XCB_NOTIFY_MODE_NORMAL) {
                        m_mouseX = -1.f;
                        m_mouseY = -1.f;
                    }
                    m_mouseLeft = true;
                    break;
                }
                case XCB_PROPERTY_NOTIFY: {
                    auto* pn = reinterpret_cast<xcb_property_notify_event_t*>(ev);
                    xcb_atom_t netState = _internAtom("_NET_WM_STATE");
                    // When WE own the maximize state (button/double-click), Mutter does
                    // not track it — its MAXIMIZED atoms stay false, so a _NET_WM_STATE
                    // PropertyNotify (e.g. from minimize/un-minimize) would otherwise look
                    // like a phantom un-maximize and wrongly restore the window. Ignore
                    // _NET_WM_STATE in self-managed mode; only react to genuine
                    // WM-initiated snaps (drag-to-edge), where m_selfMaximized is false.
                    if (pn->atom == netState && !m_selfMaximized) {
                        xcb_atom_t maxV = _internAtom("_NET_WM_STATE_MAXIMIZED_VERT");
                        xcb_atom_t maxH = _internAtom("_NET_WM_STATE_MAXIMIZED_HORZ");
                        auto cookie = xcb_get_property(m_connection, 0, m_windowId,
                                                       netState, XCB_ATOM_ATOM, 0, 32);
                        auto* reply = xcb_get_property_reply(m_connection, cookie, nullptr);
                        if (reply) {
                            bool hasV = false, hasH = false;
                            auto* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply));
                            int n = xcb_get_property_value_length(reply) / sizeof(xcb_atom_t);
                            for (int i = 0; i < n; ++i) {
                                if (atoms[i] == maxV) hasV = true;
                                if (atoms[i] == maxH) hasH = true;
                            }
                            bool wasMax = m_isMaximized;
                            m_isMaximized = hasV && hasH;
                            free(reply);

                            // CSD protocol: the WM (Mutter/GNOME) flips the MAXIMIZED
                            // atoms but does NOT resize borderless windows — we resize
                            // ourselves. This MUST happen here (after the WM has updated
                            // its maximized state) rather than synchronously in
                            // setMaximized(): the WM ignores configure requests on a
                            // window it still considers maximized, so a synchronous
                            // restore would be dropped and the window would stay full size.
                            // Driving it from the state transition handles button toggle,
                            // double-click, AND drag-to-edge snapping uniformly.
                            if (m_isMaximized && !wasMax) {
                                // Became maximized: save pre-max geometry, fill work area.
                                m_preMaxX = m_screenX; m_preMaxY = m_screenY;
                                m_preMaxW = m_width;   m_preMaxH = m_height;
                                _applyWorkArea();
                            } else if (!m_isMaximized && wasMax && m_preMaxW > 0) {
                                // Became un-maximized: restore pre-max geometry. The
                                // resulting ConfigureNotify (width-shrink) drives the
                                // MOVERESIZE-restart fallback in the application loop when
                                // this happened mid-drag (drag-out from a snap).
                                _restorePreMax();
                            }
                        }
                    }
                    break;
                }
                default: break;
            }
            free(ev);
        }
    }

    bool shouldClose()  const override { return m_closeRequested; }
    void requestClose()       override { m_closeRequested = true; }
    bool consumeFocusLost()  override { bool v = m_focusLost;  m_focusLost  = false; return v; }
    bool consumeMouseLeave() override { bool v = m_mouseLeft; m_mouseLeft = false; return v; }
    bool consumeWasResized() override { bool v = m_wasResized; m_wasResized = false; return v; }
    void swapBuffers()        override {
        // Per-frame: if the WM requested a sync (resize handshake), echo the value now
        // that this frame has been presented, releasing the compositor to show it.
        if (m_syncPending) {
            xcb_sync_int64_t v{ static_cast<int32_t>(m_syncValueHi), m_syncValueLo };
            xcb_sync_set_counter(m_connection, m_syncCounter, v);
            xcb_flush(m_connection);
            m_syncPending = false;
        }
    }
    void setVSync(bool)       override {}

    // ---- Mouse state accessors (consume-once for press/release) ----
    float mouseX() const override { return m_mouseX; }
    float mouseY() const override { return m_mouseY; }
    bool  consumePress()   override { bool v = m_pendingPress;   m_pendingPress   = false; return v; }
    bool  consumeRelease() override { bool v = m_pendingRelease; m_pendingRelease = false; return v; }
    bool  consumeRightPress() override { bool v = m_pendingRightPress; m_pendingRightPress = false; return v; }
    bool  consumeRightRelease() override { bool v = m_pendingRightRelease; m_pendingRightRelease = false; return v; }
    float consumeWheel()   override { float v = m_wheelY; m_wheelY = 0.0f; return v; }  // +up / -down notches

    // ---- Keyboard events (consume-once queue) ----
    bool hasKeyEvents() const override { return !m_keyQueue.empty(); }
    jf::JKeyEvent consumeKey() override {
        auto e = m_keyQueue.front();
        m_keyQueue.pop_front();
        return e;
    }
    std::vector<jf::JKeyEvent> consumeAllKeys() override {
        std::vector<jf::JKeyEvent> out(m_keyQueue.begin(), m_keyQueue.end());
        m_keyQueue.clear();
        return out;
    }

    // ---- Screen position and current size (updated from ConfigureNotify) ----
    int      screenX() const override { return m_screenX; }
    int      screenY() const override { return m_screenY; }
    uint32_t width()   const override { return m_width;   }
    uint32_t height()  const override { return m_height;  }

    jf::JPlatformWindowStyle windowStyle() const override { return m_style; }
    bool        isAltDown()   const override { return m_altDown; }
    bool        isCtrlDown()  const override { return m_ctrlDown; }
    bool        isShiftDown() const override { return m_shiftDown; }
    float       dpiScale()    const override { return m_dpiScale; }

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

    void setCursor(jf::JPlatformCursor shape) override {
        if (m_currentCursor == shape) return;
        m_currentCursor = shape;
        xcb_cursor_t cursorId = 0;
        switch (shape) {
            case jf::JPlatformCursor::Default:           cursorId = m_cursorDefault; break;
            case jf::JPlatformCursor::ResizeLeftRight:   cursorId = m_cursorHoriz;   break;
            case jf::JPlatformCursor::ResizeUpDown:      cursorId = m_cursorVert;    break;
            case jf::JPlatformCursor::ResizeTopLeft:     cursorId = m_cursorTopLeft; break;
            case jf::JPlatformCursor::ResizeTopRight:    cursorId = m_cursorTopRight;break;
            case jf::JPlatformCursor::ResizeBottomLeft:  cursorId = m_cursorBotLeft; break;
            case jf::JPlatformCursor::ResizeBottomRight: cursorId = m_cursorBotRight;break;
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

    // Window translucency via _NET_WM_WINDOW_OPACITY (compositor-applied). 1 = opaque.
    void setOpacity(float a) {
        a = a < 0.f ? 0.f : (a > 1.f ? 1.f : a);
        // Use a DOUBLE scale and special-case fully-opaque. 4294967295.0f (float) rounds up to
        // 2^32, and static_cast<uint32_t>(2^32) overflows to 0 — so a=1.0 would set
        // _NET_WM_WINDOW_OPACITY=0, i.e. FULLY TRANSPARENT (invisible under a compositor).
        uint32_t v = (a >= 1.0f) ? 0xFFFFFFFFu
                                 : static_cast<uint32_t>(a * 4294967295.0);
        xcb_atom_t atom = _internAtom("_NET_WM_WINDOW_OPACITY");
        xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_windowId,
                            atom, XCB_ATOM_CARDINAL, 32, 1, &v);
        xcb_flush(m_connection);
    }

    // Inform the WM of our minimum window size via WM_NORMAL_HINTS so the
    // WM enforces it during interactive resize and never sends us a
    // ConfigureNotify below this threshold.
    //
    // Under XWayland + Mutter, WM_NORMAL_HINTS is translated to
    // xdg_toplevel.set_min_size, which Mutter treats as the *total* window
    // height including its own SSD title bar — not just the client area.  We
    // add the top frame extent (_NET_FRAME_EXTENTS[2]) so that the enforced
    // client area minimum equals the caller's layout minimum.  On native X11
    // the same property is set by the WM but is interpreted as a client-area
    // minimum; the small extra margin is harmless.
    void setMinSize(uint32_t minW, uint32_t minH) override {
        minH += _frameTop();

        struct JXSizeHints {
            uint32_t flags{0};
            int32_t  x{0}, y{0}, width{0}, height{0};
            int32_t  min_width{0}, min_height{0};
            int32_t  max_width{0}, max_height{0};
            int32_t  width_inc{0}, height_inc{0};
            int32_t  min_aspect_num{0}, min_aspect_den{0};
            int32_t  max_aspect_num{0}, max_aspect_den{0};
            int32_t  base_width{0}, base_height{0};
            uint32_t win_gravity{0};
        };
        static constexpr uint32_t PMinSize = 1u << 4;
        JXSizeHints hints{};
        hints.flags      = PMinSize;
        hints.min_width  = static_cast<int32_t>(minW);
        hints.min_height = static_cast<int32_t>(minH);
        xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_windowId,
                            XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS,
                            32, sizeof(hints) / 4, &hints);
        xcb_flush(m_connection);
    }

    // Request fullscreen via _NET_WM_STATE_FULLSCREEN (WM-managed windows only).
    // The window fullscreens on whichever monitor it currently occupies — to
    // fullscreen on a secondary monitor, call setPosition() first to move it there.
    void setFullscreen(bool on) override {
        if (m_style == jf::JPlatformWindowStyle::Popup) return;  // WM not involved
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

    // Iconify (minimise) the window via WM_CHANGE_STATE (works on WM-managed windows).
    void minimize() override {
        if (m_style == jf::JPlatformWindowStyle::Popup) return;
        xcb_atom_t changeState = _internAtom("WM_CHANGE_STATE");
        if (changeState == XCB_ATOM_NONE) return;
        xcb_client_message_event_t ev{};
        ev.response_type  = XCB_CLIENT_MESSAGE;
        ev.type           = changeState;
        ev.window         = m_windowId;
        ev.format         = 32;
        ev.data.data32[0] = 3;  // IconicState
        xcb_send_event(m_connection, 0, m_rootWindow,
                       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char*>(&ev));
        xcb_flush(m_connection);
    }

    // Toggle maximize via _NET_WM_STATE_MAXIMIZED_VERT + _HORZ.
    //
    // Mutter/GNOME does NOT honor a client-message maximize for our borderless
    // (CSD) window — it sends no PropertyNotify and does not resize. So we drive
    // the geometry and state ourselves here. The atom is still sent to keep the
    // WM's notion of the window state in sync (taskbar, alt-tab, etc.). The
    // PropertyNotify path remains for WM-INITIATED snaps (drag-to-edge), which
    // see no transition here because we set m_isMaximized synchronously.
    void setMaximized(bool on) override {
        if (m_style == jf::JPlatformWindowStyle::Popup) return;
        if (on == m_isMaximized) return;  // no-op (also tames repeated button fires)
        xcb_atom_t stateAtom = _internAtom("_NET_WM_STATE");
        xcb_atom_t maxVAtom  = _internAtom("_NET_WM_STATE_MAXIMIZED_VERT");
        xcb_atom_t maxHAtom  = _internAtom("_NET_WM_STATE_MAXIMIZED_HORZ");
        if (stateAtom == XCB_ATOM_NONE || maxVAtom == XCB_ATOM_NONE || maxHAtom == XCB_ATOM_NONE) return;
        xcb_client_message_event_t ev{};
        ev.response_type  = XCB_CLIENT_MESSAGE;
        ev.type           = stateAtom;
        ev.window         = m_windowId;
        ev.format         = 32;
        ev.data.data32[0] = on ? 1u : 0u;  // _NET_WM_STATE_ADD / REMOVE
        ev.data.data32[1] = maxVAtom;
        ev.data.data32[2] = maxHAtom;       // two atoms in one message
        ev.data.data32[3] = 1u;             // source indication: normal application
        ev.data.data32[4] = 0u;
        xcb_send_event(m_connection, 0, m_rootWindow,
                       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char*>(&ev));
        xcb_flush(m_connection);

        // Apply the geometry ourselves (the WM won't for a CSD window) and mark the
        // maximize as self-managed so the PropertyNotify path ignores Mutter's atoms.
        if (on) {
            m_preMaxX = m_screenX; m_preMaxY = m_screenY;
            m_preMaxW = m_width;   m_preMaxH = m_height;
            m_isMaximized   = true;
            m_selfMaximized = true;
            _applyWorkArea();
        } else {
            m_isMaximized   = false;
            m_selfMaximized = false;
            _restorePreMax();
            m_wasUnsnapped = false;  // a button restore is not a drag
        }
    }

    bool isMaximized() const override { return m_isMaximized; }

    // Hand a title-bar drag to the WM via _NET_WM_MOVERESIZE (action 8 = MOVE).
    // The WM grabs the pointer and handles the drag natively, which restores all
    // WM features: edge snapping, drag-to-top maximise, multi-monitor crossing.
    // Call this on ButtonPress in the drag zone — do NOT do your own setPosition().
    void grabKeyboardFocus() override {
        xcb_set_input_focus(m_connection, XCB_INPUT_FOCUS_POINTER_ROOT,
                            m_windowId, XCB_CURRENT_TIME);
        xcb_flush(m_connection);
    }

    uintptr_t rawWindowId() const override {
        return static_cast<uintptr_t>(m_windowId);
    }

    void warpCursor(int gx, int gy) override {
        xcb_warp_pointer(m_connection, XCB_NONE, m_rootWindow,
                         0, 0, 0, 0,
                         static_cast<int16_t>(gx),
                         static_cast<int16_t>(gy));
        xcb_flush(m_connection);
    }

    void startWindowMove() override {
        if (m_style == jf::JPlatformWindowStyle::Popup) return;
        xcb_atom_t atom = _internAtom("_NET_WM_MOVERESIZE");
        if (atom == XCB_ATOM_NONE) return;
        // Start each drag with a clean unsnap flag; only an un-maximize that
        // happens DURING this drag should drive the MOVERESIZE-restart fallback.
        m_wasUnsnapped = false;
        xcb_ungrab_pointer(m_connection, XCB_CURRENT_TIME);
        xcb_flush(m_connection);
        auto [gx, gy] = globalCursorPos();
        xcb_client_message_event_t ev{};
        ev.response_type  = XCB_CLIENT_MESSAGE;
        ev.type           = atom;
        ev.window         = m_windowId;
        ev.format         = 32;
        ev.data.data32[0] = static_cast<uint32_t>(gx);
        ev.data.data32[1] = static_cast<uint32_t>(gy);
        ev.data.data32[2] = 8u;  // _NET_WM_MOVERESIZE_MOVE
        ev.data.data32[3] = 1u;  // left button
        ev.data.data32[4] = 1u;  // source = normal application
        xcb_send_event(m_connection, 0, m_rootWindow,
                       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char*>(&ev));
        xcb_flush(m_connection);
    }

    void startWindowResize(uint32_t direction) override {
        if (m_style == jf::JPlatformWindowStyle::Popup) return;
        xcb_atom_t atom = _internAtom("_NET_WM_MOVERESIZE");
        if (atom == XCB_ATOM_NONE) return;
        xcb_ungrab_pointer(m_connection, XCB_CURRENT_TIME);
        xcb_flush(m_connection);
        auto [gx, gy] = globalCursorPos();
        xcb_client_message_event_t ev{};
        ev.response_type  = XCB_CLIENT_MESSAGE;
        ev.type           = atom;
        ev.window         = m_windowId;
        ev.format         = 32;
        ev.data.data32[0] = static_cast<uint32_t>(gx);
        ev.data.data32[1] = static_cast<uint32_t>(gy);
        ev.data.data32[2] = direction;
        ev.data.data32[3] = 1u;
        ev.data.data32[4] = 1u;
        xcb_send_event(m_connection, 0, m_rootWindow,
                       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char*>(&ev));
        xcb_flush(m_connection);
    }

    // Read desktop 0's work area (screen minus panels) into x,y,w,h.
    bool _getWorkArea(int& wx, int& wy, uint32_t& ww, uint32_t& wh) {
        xcb_atom_t waAtom = _internAtom("_NET_WORKAREA");
        auto cookie = xcb_get_property(m_connection, 0, m_rootWindow,
                                       waAtom, XCB_ATOM_CARDINAL, 0, 16);
        auto* reply = xcb_get_property_reply(m_connection, cookie, nullptr);
        bool ok = false;
        if (reply && xcb_get_property_value_length(reply) >= static_cast<int>(4 * sizeof(uint32_t))) {
            auto* wa = static_cast<uint32_t*>(xcb_get_property_value(reply));
            wx = static_cast<int>(wa[0]); wy = static_cast<int>(wa[1]);
            ww = wa[2]; wh = wa[3];
            ok = true;
        }
        if (reply) free(reply);
        return ok;
    }

    // CSD snap protocol: the WM (Mutter/GNOME) sets the MAXIMIZED atoms but doesn't
    // resize borderless windows. Resize ourselves to fill the work area.
    void _applyWorkArea() {
        int wx, wy; uint32_t ww, wh;
        if (!_getWorkArea(wx, wy, ww, wh)) return;
        uint32_t vals[4] = { static_cast<uint32_t>(wx), static_cast<uint32_t>(wy), ww, wh };
        uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
        xcb_configure_window(m_connection, m_windowId, mask, vals);
        xcb_flush(m_connection);
        m_screenX = wx; m_screenY = wy;
        m_width   = ww; m_height  = wh;
        m_wasResized = true;
    }

    // Restore the geometry saved before the last WM-initiated snap.
    void _restorePreMax() {
        if (m_preMaxW == 0) return;
        uint32_t vals[4] = {
            static_cast<uint32_t>(m_preMaxX),
            static_cast<uint32_t>(m_preMaxY),
            m_preMaxW, m_preMaxH
        };
        uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
        xcb_configure_window(m_connection, m_windowId, mask, vals);
        xcb_flush(m_connection);
        m_wasUnsnapped = true;
    }

    bool consumeWasUnsnapped() override {
        bool v = m_wasUnsnapped; m_wasUnsnapped = false; return v;
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

    jf::JNativeWindowHandle nativeHandle() const override {
        jf::JNativeWindowHandle h{};
        h.apiTarget         = jf::JGpuApiType::Vulkan;
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
        struct JMwmHints {
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

        JKeyEvent ev;
        ev.pressed = pressed;
        ev.shift   = shift;
        ev.ctrl    = (k->state & XCB_MOD_MASK_CONTROL) != 0;
        ev.alt     = (k->state & XCB_MOD_MASK_1)       != 0;
        ev.keysym  = static_cast<uint32_t>(ks);

        using K = JKeyEvent::JKey;
        switch (ks) {
            case 0xFF09: ev.key = ev.shift ? K::BackTab : K::Tab; break;
            case 0xFF0D:                         // XK_Return (main Enter)
            case 0xFF8D: ev.key = K::Return;    break;  // XK_KP_Enter (numpad Enter)
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
    xcb_atom_t          m_syncRequestAtom{0};
    xcb_sync_counter_t  m_syncCounter{0};
    uint32_t            m_syncValueLo{0};
    uint32_t            m_syncValueHi{0};
    bool                m_syncPending{false};
    xcb_key_symbols_t*  m_syms{nullptr};

    JPlatformWindowStyle m_style{JPlatformWindowStyle::Normal};
    bool m_isMaximized{false};
    bool m_wasUnsnapped{false};
    bool m_selfMaximized{false};
    int      m_preMaxX{0}, m_preMaxY{0};
    uint32_t m_preMaxW{0}, m_preMaxH{0};
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
    bool  m_pendingRightPress{false};
    bool  m_pendingRightRelease{false};
    bool  m_closeRequested{false};
    bool  m_wasResized{false};
    bool  m_focusLost{false};
    bool  m_mouseLeft{false};
    bool  m_altDown{false};
    bool  m_ctrlDown{false};
    bool  m_shiftDown{false};

    xcb_font_t   m_cursorFont{0};
    xcb_cursor_t m_cursorDefault{0};
    xcb_cursor_t m_cursorHoriz{0};
    xcb_cursor_t m_cursorVert{0};
    xcb_cursor_t m_cursorTopLeft{0};
    xcb_cursor_t m_cursorTopRight{0};
    xcb_cursor_t m_cursorBotLeft{0};
    xcb_cursor_t m_cursorBotRight{0};
    JPlatformCursor m_currentCursor{JPlatformCursor::Default};

    // Return the height of the WM title bar decoration above our client window.
    //
    // Strategy: our client window is reparented into the WM's decoration frame.
    // We translate both the frame's (0,0) and our own (0,0) to root coordinates;
    // the difference is the top decoration height.  Works on both native X11 and
    // XWayland/Mutter (which does not set _NET_FRAME_EXTENTS on XWayland clients).
    // Returns 0 if the window has no WM parent (Popup / not yet reparented).
    uint32_t _frameTop() const {
        // Find our immediate parent window (the WM decoration frame).
        auto qtcook = xcb_query_tree(m_connection, m_windowId);
        auto* qtree = xcb_query_tree_reply(m_connection, qtcook, nullptr);
        if (!qtree) return 0;
        xcb_window_t parent = qtree->parent;
        free(qtree);

        if (parent == m_rootWindow) return 0;  // no WM reparenting

        // Translate the parent frame's origin to root coordinates.
        auto pcook = xcb_translate_coordinates(m_connection, parent, m_rootWindow, 0, 0);
        auto* preply = xcb_translate_coordinates_reply(m_connection, pcook, nullptr);
        if (!preply) return 0;
        int parentRootY = static_cast<int>(preply->dst_y);
        free(preply);

        // m_screenY is our client area's root-Y (updated by _updateRootPosition).
        int top = static_cast<int>(m_screenY) - parentRootY;
        return top > 0 ? static_cast<uint32_t>(top) : 0;
    }

    xcb_cursor_t _createFontCursor(uint16_t shape) {
        xcb_cursor_t cid = xcb_generate_id(m_connection);
        xcb_create_glyph_cursor(m_connection, cid, m_cursorFont, m_cursorFont,
                                shape, shape + 1,
                                0, 0, 0,
                                65535, 65535, 65535);
        return cid;
    }

    std::deque<JKeyEvent> m_keyQueue;
};

} // inline namespace jf
