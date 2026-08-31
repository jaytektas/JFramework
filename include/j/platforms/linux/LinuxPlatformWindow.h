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
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
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
        JLOGC("Platform", JLogLevel::Info)
            << "JLinuxPlatformWindow created: " << title
            << ", parentWindow: " << parentWindow << ", style: " << (int)style;
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

        // Declare the position we asked for as USER-specified (WM_NORMAL_HINTS / USPosition|PPosition).
        // The x,y passed to xcb_create_window is only a REQUEST: without these flags the ICCCM lets the
        // window manager place the window however it likes, and most place a new dialog under the pointer
        // or by "smart placement". That is why a dialog the framework had carefully centred on its parent
        // opened with its top-left at wherever the menu was clicked — the arithmetic was right and the WM
        // simply ignored it. Set before the window is mapped, since placement is decided at map time.
        _setPositionHint(screenX, screenY, static_cast<int32_t>(width), static_cast<int32_t>(height));

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

        // Clipboard (CLIPBOARD selection) atoms — used by set/getClipboardText and the
        // XCB_SELECTION_REQUEST handler. Interned once; native picker, no xclip shell-out.
        m_atomClipboard = _internAtom("CLIPBOARD");
        m_atomUtf8      = _internAtom("UTF8_STRING");
        m_atomTargets   = _internAtom("TARGETS");
        m_atomClipProp  = _internAtom("JF_CLIPBOARD_IN");

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

        s_lastCreatedRaw = static_cast<uintptr_t>(m_windowId);   // so a caller can parent the NEXT modal to this one

        qCInfo(jf::Log::Platform) << "XCB window created (" << width << "x" << height
                                       << " @ " << screenX << "," << screenY
                                       << ", style=" << static_cast<int>(style)
                                       << ", DPI scale " << m_dpiScale << ")\n";
    }

    ~JLinuxPlatformWindow() override {
        if (s_grabHolder == this) s_grabHolder = nullptr;   // never leave a dangling grab-holder pointer
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
        _expireUnwantedButtons();   // a frame nobody read is a frame nobody wanted
        xcb_generic_event_t* ev;
        while ((ev = _nextEvent())) {
            uint8_t type = ev->response_type & ~0x80;
            switch (type) {
                case XCB_SELECTION_REQUEST: {
                    // Another app is pasting from us — serve the CLIPBOARD selection.
                    _serveSelectionRequest(reinterpret_cast<xcb_selection_request_event_t*>(ev));
                    break;
                }
                case XCB_MOTION_NOTIFY: {
                    auto* m = reinterpret_cast<xcb_motion_notify_event_t*>(ev);
                    // Input is in the same physical-pixel space as layout and the
                    // swapchain (computeLayout uses physical width/height, the
                    // swapchain is physical).  Do NOT divide by m_dpiScale here or
                    // hit-testing desyncs from rendering whenever scale != 1.0.
                    m_mouseX = static_cast<float>(m->event_x);
                    m_mouseY = static_cast<float>(m->event_y);
                    if (m_resizable) {   // framework-managed resize cursor feedback near edges/corners
                        const int rd = _resizeDirAt(m_mouseX, m_mouseY);
                        if (rd != m_lastResizeDir) { setCursor(_resizeCursor(rd)); m_lastResizeDir = rd; }
                    }
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
                        if (const int rd = _resizeDirAt(m_mouseX, m_mouseY); rd >= 0) {
                            startWindowResize(static_cast<uint32_t>(rd));   // framework-managed: WM drives the resize
                            break;                                          // don't record a press or grab the pointer
                        }
                        m_leftQueue.push_back({ true, static_cast<float>(b->event_x), static_cast<float>(b->event_y),
                                                (b->state & XCB_MOD_MASK_CONTROL) != 0,
                                                (b->state & XCB_MOD_MASK_SHIFT) != 0,
                                                (b->state & XCB_MOD_MASK_1) != 0 });
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
                            s_grabHolder = this;   // remember who holds the grab so a modal-stack push can drop it
                        }
                    } else if (b->detail == XCB_BUTTON_INDEX_3) {
                        m_rightQueue.push_back({ true, static_cast<float>(b->event_x), static_cast<float>(b->event_y),
                                                (b->state & XCB_MOD_MASK_CONTROL) != 0,
                                                (b->state & XCB_MOD_MASK_SHIFT) != 0,
                                                (b->state & XCB_MOD_MASK_1) != 0 });
                    }
                    break;
                }
                case XCB_BUTTON_RELEASE: {
                    auto* b = reinterpret_cast<xcb_button_release_event_t*>(ev);
                    if (b->detail == XCB_BUTTON_INDEX_1) {
                        m_leftQueue.push_back({ false, static_cast<float>(b->event_x), static_cast<float>(b->event_y),
                                                (b->state & XCB_MOD_MASK_CONTROL) != 0,
                                                (b->state & XCB_MOD_MASK_SHIFT) != 0,
                                                (b->state & XCB_MOD_MASK_1) != 0 });
                        if (m_style != JPlatformWindowStyle::Popup) _ungrabPointer();
                    } else if (b->detail == XCB_BUTTON_INDEX_3) {
                        m_rightQueue.push_back({ false, static_cast<float>(b->event_x), static_cast<float>(b->event_y),
                                                (b->state & XCB_MOD_MASK_CONTROL) != 0,
                                                (b->state & XCB_MOD_MASK_SHIFT) != 0,
                                                (b->state & XCB_MOD_MASK_1) != 0 });
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
                        // WHERE THE WM ACTUALLY PUT IT. WM_NORMAL_HINTS with USPosition is a request, and a
                        // window manager is free to ignore it: several centre a dialog on its transient
                        // parent when they can work out where that is, and fall back to placing it under the
                        // pointer when they cannot. Both happen, which is why the same dialog opened
                        // centred one time and at the mouse the next. Once mapped we can simply MOVE it —
                        // a client-initiated move after map is honoured — so the position the app asked for
                        // is the position it gets. Done once: a WM that insists must not be argued with.
                        if (!m_placementLogged) {
                            m_placementLogged = true;
                            const bool moved = std::abs(m_screenX - m_requestedX) > 4 ||
                                               std::abs(m_screenY - m_requestedY) > 4;
                            qCDebug(jf::Log::Platform) << "window placed at" << m_screenX << m_screenY
                                                       << "(requested" << m_requestedX << m_requestedY
                                                       << (moved ? "— correcting)" : ")");
                            if (moved && m_placementRequested) setPosition(m_requestedX, m_requestedY);
                        }
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
    void clearCloseRequest()  override { m_closeRequested = false; }
    bool consumeFocusLost()  override { bool v = m_focusLost;  m_focusLost  = false; return v; }

    // WHICH WINDOW HAS THE INPUT FOCUS RIGHT NOW, as an id comparable with rawWindowId(). Asked
    // when a focus loss needs interpreting rather than obeying: focus moving to another window of
    // ours is a window manager re-evaluating, and focus moving to a stranger is the user leaving.
    // Those want opposite responses and the FocusOut event alone cannot tell them apart.
    // A round trip, so call it on the transition and not per frame.
    uintptr_t currentInputFocus() const {
        if (!m_connection) return 0;
        xcb_get_input_focus_reply_t* r =
            xcb_get_input_focus_reply(m_connection, xcb_get_input_focus(m_connection), nullptr);
        if (!r) return 0;
        const uintptr_t w = static_cast<uintptr_t>(r->focus);
        free(r);
        return w;
    }
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
    // One queued event per call, and only from the FRONT, so press/release order is exactly as it happened.
    // The reported cursor position becomes that event's own, which is what a handler needs: where the click
    // was, not where the pointer has since travelled.
    bool  consumePress()   override { return _takeButton(m_leftQueue,  true);  }
    bool  consumeRelease() override { return _takeButton(m_leftQueue,  false); }
    bool  consumeRightPress() override { return _takeButton(m_rightQueue, true); }
    bool  consumeRightRelease() override { return _takeButton(m_rightQueue, false); }
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
    void     refreshScreenPosition() override { _updateRootPosition(); }
    int      screenX() const override { return m_screenX; }
    int      screenY() const override { return m_screenY; }
    uint32_t width()   const override { return m_width;   }
    uint32_t height()  const override { return m_height;  }

    // Root-screen pixel size — for keeping popups (menus/submenus) on-screen.
    std::pair<int,int> screenSize() const {
        if (const xcb_setup_t* setup = xcb_get_setup(m_connection)) {
            if (xcb_screen_t* s = xcb_setup_roots_iterator(setup).data)
                return { int(s->width_in_pixels), int(s->height_in_pixels) };
        }
        return { 1920, 1080 };
    }

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
        // This IS the app asking for a position, so it becomes the one the placement correction defends.
        //
        // m_requestedX/Y were set once, at construction, and never updated. The correction above (on the
        // first ConfigureNotify: "the WM put it somewhere else, move it back") therefore fought every
        // deliberate move made between creating a window and its first configure event, and put the window
        // back where it was CREATED. Menu placement is exactly that sequence — a popup is created at the
        // click, its items added, measured, and only then moved to fit the screen — so the move was undone
        // every time and a menu opened near the bottom of the screen always hung off it. Nothing said so:
        // the placement arithmetic was right, and it only ever checked its own answer.
        m_requestedX = x; m_requestedY = y; m_placementRequested = true;
        uint32_t vals[] = { static_cast<uint32_t>(x), static_cast<uint32_t>(y) };
        xcb_configure_window(m_connection, m_windowId,
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
        xcb_flush(m_connection);
    }

    // Runtime WM title (title-bar text). Mirrors the WM_NAME set at creation.
    void setTitle(const std::string& title) override {
        xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_windowId, XCB_ATOM_WM_NAME,
                            XCB_ATOM_STRING, 8, static_cast<uint32_t>(title.size()), title.c_str());
        xcb_flush(m_connection);
    }

    // Resize the window.
    void setSize(uint32_t w, uint32_t h) override {
        uint32_t vals[] = { w, h };
        xcb_configure_window(m_connection, m_windowId,
            XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
        xcb_flush(m_connection);
    }

    // Map/unmap without destroying: the window keeps its geometry, surface and contents while hidden, so
    // showing it again puts it back exactly where it was. This is what lets a mode change hide a floating
    // panel and give it back later — closing and respawning would lose its position and size.
    void setMapped(bool on) override {
        if (on == m_mapped) return;
        m_mapped = on;
        if (on) xcb_map_window(m_connection, m_windowId);
        else    xcb_unmap_window(m_connection, m_windowId);
        xcb_flush(m_connection);
    }
    bool isMapped() const override { return m_mapped; }

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

    // WM_NORMAL_HINTS, accumulated. ICCCM has ONE property for min-size, position and size hints, so they
    // must be written together — a second setter that rebuilt the struct from scratch would silently drop
    // whatever the first one declared.
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
    static constexpr uint32_t kUSPosition = 1u << 0;
    static constexpr uint32_t kUSSize     = 1u << 1;
    static constexpr uint32_t kPPosition  = 1u << 2;
    static constexpr uint32_t kPSize      = 1u << 3;
    static constexpr uint32_t kPMinSize   = 1u << 4;

    void _writeSizeHints() {
        xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_windowId,
                            XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS,
                            32, sizeof(m_sizeHints) / 4, &m_sizeHints);
        xcb_flush(m_connection);
    }

    // "This position is deliberate, not a default" — the only way to stop a WM placing the window itself.
    bool m_placementLogged = false, m_placementRequested = false;
    int  m_requestedX = 0, m_requestedY = 0;

    void _setPositionHint(int px, int py, int32_t pw, int32_t ph) {
        m_requestedX = px; m_requestedY = py; m_placementRequested = true;
        m_sizeHints.flags |= kUSPosition | kPPosition | kUSSize | kPSize;
        m_sizeHints.x = px; m_sizeHints.y = py;
        m_sizeHints.width = pw; m_sizeHints.height = ph;
        _writeSizeHints();
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
        m_sizeHints.flags     |= kPMinSize;
        m_sizeHints.min_width  = static_cast<int32_t>(minW);
        m_sizeHints.min_height = static_cast<int32_t>(minH);
        _writeSizeHints();
    }

    // Framework-managed edge resize (opt-in). When enabled, the window itself detects edge/corner grabs,
    // shows the matching resize cursor, and hands the interactive resize to the WM (_NET_WM_MOVERESIZE). The
    // application just flips this on + sets min size — identical behaviour for every window/dialog. `topInset`
    // reserves the title-bar strip for drag/buttons (side edges only resize below it).
    void setResizable(bool on)      { m_resizable = on; }
    void setResizeTopInset(float t) { m_resizeTopInset = t; }

    // Which resize direction (a _NET_WM_MOVERESIZE code) an edge/corner at (mx,my) grabs, or -1 for none.
    int _resizeDirAt(float mx, float my) const {
        if (!m_resizable || m_isMaximized) return -1;
        const float W = static_cast<float>(m_width), H = static_cast<float>(m_height);
        constexpr float kEdge = 6.f, kCorn = 14.f;
        const bool onLeft = mx < kEdge && my >= m_resizeTopInset, onRight = mx >= W - kEdge && my >= m_resizeTopInset;
        const bool onBottom = my >= H - kEdge, onBL = mx < kCorn && my >= H - kCorn, onBR = mx >= W - kCorn && my >= H - kCorn;
        if (onBL) return 6; if (onBR) return 4; if (onBottom) return 5; if (onLeft) return 7; if (onRight) return 3;
        return -1;
    }
    jf::JPlatformCursor _resizeCursor(int d) const {
        switch (d) {
            case 4:         return jf::JPlatformCursor::ResizeBottomRight;
            case 6:         return jf::JPlatformCursor::ResizeBottomLeft;
            case 5:         return jf::JPlatformCursor::ResizeUpDown;
            case 3: case 7: return jf::JPlatformCursor::ResizeLeftRight;
            default:        return jf::JPlatformCursor::Default;
        }
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
        _ungrabPointer();
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
        _ungrabPointer();
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

    // Which X window holds the input focus RIGHT NOW, as a raw id. A menu popup needs to know whether
    // focus is still somewhere in this application: FocusOut alone cannot say where it went, and a menu
    // that dismissed on any focus change would close itself the moment its own submenu opened.
    uintptr_t focusedWindowRaw() const {
        auto* r = xcb_get_input_focus_reply(m_connection, xcb_get_input_focus(m_connection), nullptr);
        const uintptr_t w = r ? static_cast<uintptr_t>(r->focus) : 0;
        if (r) free(r);
        return w;
    }

    // Usable rect for popup placement: _NET_WORKAREA (desktop minus panels/docks), else the root screen.
    // NOTE: X gives no per-monitor split without RandR (not linked here), so on a multi-head desktop this
    // is the whole work area, not the monitor under (px,py) — a menu can still be placed across a bezel.
    JScreenRect workAreaAt(int px, int py) const override {
        (void)px; (void)py;
        int wx = 0, wy = 0; uint32_t ww = 0, wh = 0;
        if (const_cast<JLinuxPlatformWindow*>(this)->_getWorkArea(wx, wy, ww, wh) && ww > 0 && wh > 0)
            return JScreenRect{ wx, wy, static_cast<int>(ww), static_cast<int>(wh) };
        const auto [sw, sh] = screenSize();
        return JScreenRect{ 0, 0, sw, sh };
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

    // The raw window id of the MOST RECENTLY created window. A modal dialog constructs its window in
    // its ctor, so reading this straight after constructing a dialog yields that dialog's window id —
    // which the modal stack stores so the NEXT nested modal can be parented (WM_TRANSIENT_FOR) to its
    // opener rather than the root window. That transient link is what makes the WM stack a child modal
    // above the modal that opened it (see the ctor's parentWindow branch). Single-threaded UI only.
    static uintptr_t lastCreatedRawWindowId() { return s_lastCreatedRaw; }

    // Release whatever window currently holds the pointer grab. A window grabs the pointer on
    // button-press (to keep motion/release during a drag-outside); it normally ungrabs on release.
    // But a modal that opens a CHILD modal freezes before it ever sees its own button-release, so
    // that grab would stick and steal every button event the child should get (keyboard is
    // unaffected — it routes by focus — which is why such a child takes keys but not the mouse).
    // The modal-stack push calls this so the superseded window's grab is dropped. No-op if none.
    static void releaseActivePointerGrab() { if (s_grabHolder) s_grabHolder->_ungrabPointer(); }

    // ---- Native text clipboard (CLIPBOARD selection) -----------------------
    // We own the selection and serve it from pollNativeEvents (XCB_SELECTION_REQUEST).
    // No xclip/xsel shell-out — this is the framework talking X11 directly.
    void setClipboardText(const std::string& text) override {
        m_clipboardText = text;
        xcb_set_selection_owner(m_connection, m_windowId, m_atomClipboard, XCB_CURRENT_TIME);
        xcb_flush(m_connection);
    }

    std::string getClipboardText() override {
        // Fast path: if we own the selection, return our own copy — no round-trip.
        xcb_get_selection_owner_reply_t* own = xcb_get_selection_owner_reply(
            m_connection, xcb_get_selection_owner(m_connection, m_atomClipboard), nullptr);
        bool weOwn = own && own->owner == m_windowId;
        if (own) free(own);
        if (weOwn) return m_clipboardText;

        // Otherwise ask the current owner to convert CLIPBOARD → UTF8_STRING into our property.
        xcb_delete_property(m_connection, m_windowId, m_atomClipProp);
        xcb_convert_selection(m_connection, m_windowId, m_atomClipboard,
                              m_atomUtf8, m_atomClipProp, XCB_CURRENT_TIME);
        xcb_flush(m_connection);

        // Wait (bounded ~200ms) for the SelectionNotify. Any non-selection events that
        // arrive meanwhile are deferred, not dropped — pollNativeEvents replays them.
        bool got = false;
        xcb_selection_notify_event_t notify{};
        for (int spins = 0; spins < 200 && !got; ++spins) {
            xcb_generic_event_t* e = xcb_poll_for_event(m_connection);
            if (!e) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            if ((e->response_type & ~0x80) == XCB_SELECTION_NOTIFY) {
                notify = *reinterpret_cast<xcb_selection_notify_event_t*>(e);
                got = true;
            } else {
                m_deferredEvents.push_back(e);   // replay through the normal switch next frame
                continue;
            }
            free(e);
        }
        if (!got || notify.property == XCB_ATOM_NONE) return {};
        return _readTextProperty(m_atomClipProp);
    }

private:
    // Single funnel for releasing the pointer grab: ungrab on this window's connection and clear the
    // shared grab-holder if it points at us. All ungrab sites (button-release, window move/resize,
    // releaseActivePointerGrab) go through here so the s_grabHolder tracker never goes stale.
    void _ungrabPointer() {
        xcb_ungrab_pointer(m_connection, XCB_CURRENT_TIME);
        xcb_flush(m_connection);
        if (s_grabHolder == this) s_grabHolder = nullptr;
    }
    // The window that currently holds an active pointer grab (nullptr if none). Windows here each own
    // their own XCB connection, so releasing must happen on the grabber's connection — hence a pointer
    // to the holder rather than a bare bool. Set at the grab, cleared at every ungrab + in the dtor.
    inline static JLinuxPlatformWindow* s_grabHolder = nullptr;
    // Raw window id of the most recently constructed window (see lastCreatedRawWindowId()).
    inline static uintptr_t s_lastCreatedRaw = 0;

    // Deferred-then-live event source, so a clipboard paste's blocking wait can stash
    // unrelated events for the next pollNativeEvents pass instead of discarding them.
    xcb_generic_event_t* _nextEvent() {
        if (!m_deferredEvents.empty()) {
            xcb_generic_event_t* e = m_deferredEvents.front();
            m_deferredEvents.erase(m_deferredEvents.begin());
            return e;
        }
        return xcb_poll_for_event(m_connection);
    }

    // Serve a paste request from another client against our owned CLIPBOARD text.
    void _serveSelectionRequest(xcb_selection_request_event_t* req) {
        xcb_selection_notify_event_t notify{};
        notify.response_type = XCB_SELECTION_NOTIFY;
        notify.requestor = req->requestor;
        notify.selection = req->selection;
        notify.target    = req->target;
        notify.time      = req->time;
        notify.property  = XCB_ATOM_NONE;   // default: refused

        if (req->target == m_atomTargets) {
            xcb_atom_t targets[] = { m_atomTargets, m_atomUtf8, XCB_ATOM_STRING };
            xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, req->requestor,
                                req->property, XCB_ATOM_ATOM, 32, 3, targets);
            notify.property = req->property;
        } else if (req->target == m_atomUtf8 || req->target == XCB_ATOM_STRING) {
            xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, req->requestor,
                                req->property, req->target, 8,
                                static_cast<uint32_t>(m_clipboardText.size()),
                                m_clipboardText.data());
            notify.property = req->property;
        }
        xcb_send_event(m_connection, 0, req->requestor,
                       XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&notify));
        xcb_flush(m_connection);
    }

    // Read (and delete) a text property delivered to our window.
    std::string _readTextProperty(xcb_atom_t prop) {
        std::string out;
        xcb_get_property_reply_t* r = xcb_get_property_reply(m_connection,
            xcb_get_property(m_connection, 1 /*delete after read*/, m_windowId,
                             prop, XCB_ATOM_ANY, 0, 0x3fffffff), nullptr);
        if (r) {
            int len = xcb_get_property_value_length(r);
            if (len > 0)
                out.assign(reinterpret_cast<const char*>(xcb_get_property_value(r)),
                           static_cast<size_t>(len));
            free(r);
        }
        return out;
    }

public:
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

    // Which modifier bit NumLock actually lives on. Conventionally Mod2, but that is a layout
    // convention and not a guarantee, so ask the server which modifier holds the Num_Lock keycode.
    uint16_t _numLockMask() {
        if (m_numLockMask) return m_numLockMask;
        m_numLockMask = XCB_MOD_MASK_2;                  // sane default if the query fails
        if (!m_syms) m_syms = xcb_key_symbols_alloc(m_connection);
        xcb_keycode_t* codes = xcb_key_symbols_get_keycode(m_syms, 0xFF7F /* XK_Num_Lock */);
        if (!codes) return m_numLockMask;
        if (auto* r = xcb_get_modifier_mapping_reply(m_connection,
                                                     xcb_get_modifier_mapping(m_connection), nullptr)) {
            const xcb_keycode_t* map = xcb_get_modifier_mapping_keycodes(r);
            const int per = r->keycodes_per_modifier;
            for (int mod = 0; mod < 8; ++mod)
                for (int i = 0; i < per; ++i)
                    for (const xcb_keycode_t* c = codes; *c != XCB_NO_SYMBOL; ++c)
                        if (map[mod * per + i] == *c) m_numLockMask = static_cast<uint16_t>(1u << mod);
            free(r);
        }
        free(codes);
        return m_numLockMask;
    }

    // A keypad key is not a second, unhandled keyboard: fold it onto the main-keyboard keysym it
    // means, so digits type and the navigation cluster navigates through the one path below.
    static xcb_keysym_t _foldKeypad(xcb_keysym_t ks) {
        if (ks >= 0xFFB0 && ks <= 0xFFB9) return '0' + (ks - 0xFFB0);   // XK_KP_0 … XK_KP_9
        switch (ks) {
            case 0xFFAE: return '.';        // XK_KP_Decimal
            case 0xFFAC: return ',';        // XK_KP_Separator
            case 0xFFAA: return '*';        // XK_KP_Multiply
            case 0xFFAB: return '+';        // XK_KP_Add
            case 0xFFAD: return '-';        // XK_KP_Subtract
            case 0xFFAF: return '/';        // XK_KP_Divide
            case 0xFFBD: return '=';        // XK_KP_Equal
            case 0xFF80: return 0x0020;     // XK_KP_Space
            case 0xFF89: return 0xFF09;     // XK_KP_Tab       -> Tab
            case 0xFF95: return 0xFF50;     // XK_KP_Home      -> Home
            case 0xFF96: return 0xFF51;     // XK_KP_Left      -> Left
            case 0xFF97: return 0xFF52;     // XK_KP_Up        -> Up
            case 0xFF98: return 0xFF53;     // XK_KP_Right     -> Right
            case 0xFF99: return 0xFF54;     // XK_KP_Down      -> Down
            case 0xFF9A: return 0xFF55;     // XK_KP_Page_Up   -> PageUp
            case 0xFF9B: return 0xFF56;     // XK_KP_Page_Down -> PageDown
            case 0xFF9C: return 0xFF57;     // XK_KP_End       -> End
            case 0xFF9F: return 0xFFFF;     // XK_KP_Delete    -> Delete
            default:     return ks;
        }
    }

    void _handleKey(xcb_key_press_event_t* k, bool pressed) {
        if (!m_syms) m_syms = xcb_key_symbols_alloc(m_connection);
        bool shift = (k->state & XCB_MOD_MASK_SHIFT) != 0;
        const xcb_keysym_t base = xcb_key_symbols_get_keysym(m_syms, k->detail, 0);
        // A keypad key holds its DIGIT at level 1 and its navigation meaning at level 0, and X picks
        // between them with NUMLOCK, not Shift (Shift inverts it). Reading level (shift?1:0) meant
        // every numpad digit arrived as KP_End/KP_Down/… and fell out of the switch with no character.
        const bool keypad  = (base >= 0xFF80 && base <= 0xFFBD);   // XK_KP_Space … XK_KP_9
        const bool numeric = ((k->state & _numLockMask()) != 0) != shift;
        xcb_keysym_t ks = xcb_key_symbols_get_keysym(m_syms, k->detail,
                                                     (keypad ? numeric : shift) ? 1 : 0);
        // Non-character keys (arrows, F-keys, Home/End…) have no shifted keysym level, so a Shift-held
        // lookup returns NoSymbol and the key would be dropped. Fall back to the base level so Shift+Arrow
        // etc. still identify as the same key (with ev.shift set) — e.g. Shift+Arrow range-select in tables.
        if (ks == 0) ks = base;
        if (keypad) ks = _foldKeypad(ks);

        JKeyEvent ev;
        ev.pressed = pressed;
        ev.shift   = shift;
        ev.ctrl    = (k->state & XCB_MOD_MASK_CONTROL) != 0;
        ev.alt     = (k->state & XCB_MOD_MASK_1)       != 0;
        ev.keysym  = static_cast<uint32_t>(ks);

        using K = JKeyEvent::JKey;
        switch (ks) {
            case 0xFE20: ev.key = K::BackTab; break;               // XK_ISO_Left_Tab — X11 sends this for Shift+Tab
            case 0xFF09: ev.key = ev.shift ? K::BackTab : K::Tab; break;   // XK_Tab (some servers keep it on Shift+Tab)
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
            case 0xFF55: ev.key = K::PageUp;    break;   // XK_Page_Up   (table Z-plane +1)
            case 0xFF56: ev.key = K::PageDown;  break;   // XK_Page_Down (table Z-plane -1)
            case 0xFFBE: ev.key = K::F1;  break;   // XK_F1..XK_F12 (consecutive)
            case 0xFFBF: ev.key = K::F2;  break;
            case 0xFFC0: ev.key = K::F3;  break;
            case 0xFFC1: ev.key = K::F4;  break;
            case 0xFFC2: ev.key = K::F5;  break;
            case 0xFFC3: ev.key = K::F6;  break;
            case 0xFFC4: ev.key = K::F7;  break;
            case 0xFFC5: ev.key = K::F8;  break;
            case 0xFFC6: ev.key = K::F9;  break;
            case 0xFFC7: ev.key = K::F10; break;
            case 0xFFC8: ev.key = K::F11; break;
            case 0xFFC9: ev.key = K::F12; break;
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
                        // ke.key is the canonical KEY IDENTITY, not the typed glyph: fold ASCII letters to
                        // uppercase so ke.key == K::Z holds for the 'z' key regardless of Shift/Caps (JKey has
                        // A=65..Z=90). The actual character stays in ke.utf8. Without this, every letter
                        // shortcut (Ctrl+Z/Y/C/V/G…) silently missed, since 'z' is 0x7a (122) != K::Z (90).
                        ev.key = static_cast<K>((cp >= 'a' && cp <= 'z') ? cp - 32 : cp);
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

    // Clipboard (CLIPBOARD selection) state.
    xcb_atom_t          m_atomClipboard{XCB_ATOM_NONE};
    xcb_atom_t          m_atomUtf8{XCB_ATOM_NONE};
    xcb_atom_t          m_atomTargets{XCB_ATOM_NONE};
    xcb_atom_t          m_atomClipProp{XCB_ATOM_NONE};
    std::string         m_clipboardText;                 // text we own on CLIPBOARD
    std::vector<xcb_generic_event_t*> m_deferredEvents;  // events stashed during a paste wait
    xcb_sync_counter_t  m_syncCounter{0};
    uint32_t            m_syncValueLo{0};
    uint32_t            m_syncValueHi{0};
    bool                m_syncPending{false};
    xcb_key_symbols_t*  m_syms{nullptr};
    uint16_t            m_numLockMask{0};   // 0 = not yet resolved; see _numLockMask()

    JPlatformWindowStyle m_style{JPlatformWindowStyle::Normal};
    bool m_isMaximized{false};
    bool m_mapped{true};             // mapped at construction (see setMapped)
    bool m_wasUnsnapped{false};
    bool m_selfMaximized{false};
    int      m_preMaxX{0}, m_preMaxY{0};
    uint32_t m_preMaxW{0}, m_preMaxH{0};
    float m_dpiScale{1.0f};
    int      m_screenX{0};
    int      m_screenY{0};
    uint32_t m_width{0};
    uint32_t m_height{0};
    bool  m_resizable{false};        // opt-in framework edge-resize
    float m_resizeTopInset{0.0f};    // title-bar strip kept free from side-edge resize
    int   m_lastResizeDir{-2};       // last resize cursor dir (avoid re-setting the cursor every motion)
    float m_mouseX{0.0f};
    float m_mouseY{0.0f};
    float m_wheelY{0.0f};
    // BUTTON EVENTS ARE QUEUED, one per button, in arrival order — not latched into a flag. A flag loses
    // the second of two clicks that land in the same frame, reports a press and its release as if they were
    // simultaneous, and (worst) lets a later motion overwrite the position a press happened at, so a fast
    // drag's press lands wherever the pointer got to. Each event carries the position it occurred at, and
    // one is handed over per poll, so a caller sees exactly the sequence the user performed. Keys have
    // always been queued this way; buttons had not.
    // A button event carries the state it HAPPENED under — position and modifiers both. The modifiers used
    // to be window-global scalars written as X events were processed, which was fine while a press was a
    // flag read in the same frame; with a queue, a press consumed a frame later reported whatever the last
    // KEY event had left behind (a key's own state field excludes the key being pressed, so Ctrl's press
    // writes ctrl=false). Ctrl-click and Shift-click then degraded to plain clicks: no multi-select, no
    // multi-entry drag out of the dictionary.
    struct JButtonEvent { bool press; float x, y; bool ctrl, shift, alt; };
    std::deque<JButtonEvent> m_leftQueue, m_rightQueue;
    int  m_leftTaken = 0, m_rightTaken = 0;      // events read since the last poll
    bool m_leftHad = false, m_rightHad = false;  // …and whether there was anything to read then
    // WHOSE JOB IS IT. Button events are an ordered queue so two clicks in one frame both arrive and a
    // press is reported where it happened. That ordering only matters to a consumer that reads BOTH kinds;
    // one that reads presses alone sits behind the first release for ever and ignores the pointer from then
    // on. Three consumers got that wrong in one afternoon — a file dialog, a message dialog, and the app
    // window's own right button, which is why a context menu opened once and never again. It is not a
    // mistake each consumer should have to avoid, so the WINDOW owns its queue: an event that survives a
    // whole frame with nobody reading anything is an event nobody wants, and it is dropped at the next poll
    // instead of blocking what is behind it. A consumer that reads both always takes something while the
    // queue is non-empty, so its ordering is never disturbed.
    void _expireUnwantedButtons() {
        _expireOne(m_leftQueue,  m_leftTaken,  m_leftHad);
        _expireOne(m_rightQueue, m_rightTaken, m_rightHad);
    }
    // The "had" half matters: without it an event would be dropped on the very frame it arrived, before
    // anybody had the chance to read it.
    static void _expireOne(std::deque<JButtonEvent>& q, int& taken, bool& had) {
        if (taken == 0 && had && !q.empty()) q.pop_front();
        taken = 0;
        had = !q.empty();
    }


    // Take the next event IF it is the kind asked for. Only ever from the front: a release still waiting to
    // be read must not be jumped over by a later press, or a caller sees them out of order.
    bool _takeButton(std::deque<JButtonEvent>& q, bool wantPress) {
        if (q.empty() || q.front().press != wantPress) return false;
        ++(&q == &m_leftQueue ? m_leftTaken : m_rightTaken);
        m_mouseX = q.front().x;
        m_mouseY = q.front().y;
        m_ctrlDown  = q.front().ctrl;    // the modifiers this event happened under, not the latest key's
        m_shiftDown = q.front().shift;
        m_altDown   = q.front().alt;
        q.pop_front();
        return true;
    }


    bool  m_closeRequested{false};
    bool  m_wasResized{false};
    bool  m_focusLost{false};
    JXSizeHints m_sizeHints{};   // accumulated WM_NORMAL_HINTS (position + min size)
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
