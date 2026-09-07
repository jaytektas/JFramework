// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

#include <j/core/ApplicationCore.h>
#include <j/core/FocusManager.h>
#include <j/graphics/GpuHal.h>
#include <windows.h>
#include <windowsx.h>
#include <stdexcept>
#include <algorithm>
#include <vector>
#include <deque>
#include <utility>

// Custom Logging Integration
inline constexpr jf::Log::JCategory LogWin32Backend{"Win32Backend"};

inline namespace jf {

class JWindowsPlatformWindow : public jf::JPlatformWindow {
public:
    using JKeyEvent = jf::JKeyEvent;
    JWindowsPlatformWindow(const std::string& title, uint32_t width, uint32_t height,
                          int screenX = 100, int screenY = 100,
                          JPlatformWindowStyle style = JPlatformWindowStyle::Normal,
                          HWND parentWindow = nullptr,
                          HINSTANCE sharedInst = nullptr)
        : m_screenX(screenX), m_screenY(screenY)
        , m_width(width), m_height(height)
        , m_style(style)
        , m_closeRequested(false)
        , m_dpiScaleFactor(1.0f)
    {
        m_hInstance = sharedInst ? sharedInst : GetModuleHandleW(nullptr);

        HMODULE user32 = LoadLibraryA("user32.dll");
        if (user32) {
            typedef BOOL(WINAPI *SetProcessDpiAwarenessContextProc)(DPI_AWARENESS_CONTEXT);
            auto setDpiAwareness = (SetProcessDpiAwarenessContextProc)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
            if (setDpiAwareness) {
                setDpiAwareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            }
            FreeLibrary(user32);
        }

        std::wstring wTitle(title.begin(), title.end());
        LPCWSTR className = L"GenesisNativeWindowClass";

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc = JWindowsPlatformWindow::StaticWindowProc;
        wc.hInstance = m_hInstance;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = className;

        RegisterClassExW(&wc);

        DWORD winStyle = WS_OVERLAPPEDWINDOW;
        DWORD exStyle   = 0;
        if (style == JPlatformWindowStyle::Borderless) {
            winStyle = WS_POPUP | WS_SYSMENU;
        } else if (style == JPlatformWindowStyle::Popup) {
            winStyle = WS_POPUP;
            // A MENU IS NOT AN ORDINARY WINDOW, and saying so is not cosmetic.
            //
            // WS_EX_NOACTIVATE keeps the popup from taking activation: a menu that activates itself
            // makes the window behind it draw as inactive the moment it opens, and hands focus back
            // on close, which is a visible flicker on every single menu click.
            // WS_EX_TOOLWINDOW keeps it out of the taskbar and the alt-tab list -- a dropdown is not
            // a task -- and WS_EX_TOPMOST keeps it above the window that owns it.
            //
            // It also decides who chooses the popup's POSITION. An extended style of zero describes a
            // normal top-level window, so a window manager is entitled to place it wherever its own
            // policy says; under Wine that is exactly what happens, and menus open in the middle of
            // the screen instead of under the item that was clicked. These flags are what mark it as
            // the app's own furniture, to be put exactly where it was asked for.
            exStyle  = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
        }

        RECT rect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        AdjustWindowRectEx(&rect, winStyle, FALSE, exStyle);

        m_hwnd = CreateWindowExW(
            exStyle,
            className,
            wTitle.c_str(),
            winStyle,
            screenX, screenY,
            rect.right - rect.left,
            rect.bottom - rect.top,
            parentWindow,
            nullptr,
            m_hInstance,
            this
        );

        if (!m_hwnd) {
            DWORD err = GetLastError();
            throw std::runtime_error("Fatal: Failed to instantiate Win32 HWND surface. Error: " + std::to_string(err));
        }

        HDC hdc = GetDC(m_hwnd);
        if (hdc) {
            int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
            m_dpiScaleFactor = std::max(1.0f, static_cast<float>(dpi) / 96.0f);
            ReleaseDC(m_hwnd, hdc);
        }

        // SW_SHOW ACTIVATES, which would undo WS_EX_NOACTIVATE at the moment it matters. A menu has to
        // appear without taking focus from the window it belongs to.
        ShowWindow(m_hwnd, (style == JPlatformWindowStyle::Popup) ? SW_SHOWNOACTIVATE : SW_SHOW);
        UpdateWindow(m_hwnd);
        s_lastCreatedRaw = reinterpret_cast<uintptr_t>(m_hwnd);   // so a caller can parent the NEXT modal to this one
    }

    ~JWindowsPlatformWindow() override {
        if (m_hwnd) {
            DestroyWindow(m_hwnd);
        }
    }

    void pollNativeEvents() override {
        _expireUnwantedButtons();   // a frame nobody read is a frame nobody wanted
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                m_closeRequested = true;
            } else {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    void swapBuffers() override {}
    void setVSync(bool) override {}
    bool shouldClose() const override { return m_closeRequested; }

    float mouseX() const override { return m_mouseX; }
    float mouseY() const override { return m_mouseY; }
    // One QUEUED event per call, front only, adopting that event's own position — see the queue below.
    bool  consumePress() override        { return _takeButton(m_leftQueue,  true);  }
    bool  consumeRelease() override      { return _takeButton(m_leftQueue,  false); }
    bool  consumeRightPress() override   { return _takeButton(m_rightQueue, true);  }
    bool  consumeRightRelease() override { return _takeButton(m_rightQueue, false); }
    float consumeWheel() override { float v = m_wheelY; m_wheelY = 0.0f; return v; }

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

    void     refreshScreenPosition() override {
        RECT r{};
        if (m_hwnd && ::GetWindowRect(m_hwnd, &r)) { m_screenX = r.left; m_screenY = r.top; }
    }
    int      screenX() const override { return m_screenX; }
    int      screenY() const override { return m_screenY; }
    uint32_t width()   const override { return m_width; }
    uint32_t height()  const override { return m_height; }

    void setPosition(int x, int y) override {
        m_screenX = x;
        m_screenY = y;
        SetWindowPos(m_hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void setSize(uint32_t w, uint32_t h) override {
        m_width = w;
        m_height = h;
        SetWindowPos(m_hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // A floating dock is resized by grabbing its edge, and the only thing telling the user where
    // that edge is, is the cursor. Left as a no-op the grip is invisible and feels broken.
    void setCursor(jf::JPlatformCursor shape) override {
        // The IDC_* macros are MAKEINTRESOURCE, which is the ANSI spelling unless UNICODE is
        // defined -- and it is not for this build. Use the numeric ids with the W macro, the same
        // way the window class above already loads its arrow.
        constexpr WORD kArrow = 32512, kSizeNWSE = 32642, kSizeNESW = 32643,
                       kSizeWE = 32644, kSizeNS = 32645;
        WORD id = kArrow;
        switch (shape) {
            case jf::JPlatformCursor::ResizeLeftRight:   id = kSizeWE;   break;
            case jf::JPlatformCursor::ResizeUpDown:      id = kSizeNS;   break;
            case jf::JPlatformCursor::ResizeTopLeft:
            case jf::JPlatformCursor::ResizeBottomRight: id = kSizeNWSE; break;
            case jf::JPlatformCursor::ResizeTopRight:
            case jf::JPlatformCursor::ResizeBottomLeft:  id = kSizeNESW; break;
            default:                                     id = kArrow;    break;
        }
        m_cursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(id));
        // Windows re-asserts the class cursor on every WM_SETCURSOR, so setting it once is not
        // enough -- the handler below reapplies m_cursor, and this makes the change visible now.
        if (m_cursor) ::SetCursor(m_cursor);
    }

    // Show/hide without activating: a float is hidden while its dock is dragged back into a host,
    // and stealing focus on the way past would fight the drag.
    void setMapped(bool on) override {
        if (!m_hwnd || on == m_mapped) return;
        m_mapped = on;
        ShowWindow(m_hwnd, on ? SW_SHOWNA : SW_HIDE);
    }
    bool isMapped() const override { return m_mapped; }

    bool consumeMouseLeave() override { bool v = m_mouseLeft; m_mouseLeft = false; return v; }
    jf::JPlatformWindowStyle windowStyle() const override { return m_style; }

    jf::JNativeWindowHandle nativeHandle() const override {
        jf::JNativeWindowHandle h{};
        h.apiTarget         = jf::JGpuApiType::Vulkan;
        h.connectionPointer = m_hInstance;
        h.windowPointer     = m_hwnd;
        return h;
    }

    HWND nativeWindow() const { return m_hwnd; }
    HINSTANCE nativeInstance() const { return m_hInstance; }

    // ---- Parity with JLinuxPlatformWindow: the three things the menu runtime asks for ----

    // An HWND as a plain integer id, so menu code can compare windows without knowing the platform
    // type. Win32 hands out pointers where X hands out numeric ids; both fit a uintptr_t.
    uintptr_t rawWindowId() const override { return reinterpret_cast<uintptr_t>(m_hwnd); }

    // Which window holds the input focus RIGHT NOW. GetFocus() only answers for the calling thread's
    // own message queue, which is exactly wrong here: the question is whether focus went to another
    // application, so it must be asked globally.
    uintptr_t focusedWindowRaw() const { return reinterpret_cast<uintptr_t>(GetForegroundWindow()); }

    // Primary-monitor pixel size, for keeping popups on-screen.
    std::pair<int,int> screenSize() const {
        return { GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    }

    // ---- Window chrome ----------------------------------------------------------------
    //
    // The application DRAWS ITS OWN title bar, so nothing native is listening for a drag on
    // it, a double-click to maximise, or a click on a close button that Windows did not put
    // there. Every one of those arrives as an ordinary click in the client area and has to be
    // turned back into a window operation here.
    //
    // These are all declared on JPlatformWindow with an empty default body rather than as pure
    // virtuals, so a platform that implements none of them compiles perfectly and then does
    // nothing at all when the user drags the title bar. That is how they came to be missing.

    void requestClose()      override { m_closeRequested = true;  }
    void clearCloseRequest() override { m_closeRequested = false; }

    void minimize() override {
        if (m_style == jf::JPlatformWindowStyle::Popup || !m_hwnd) return;
        ShowWindow(m_hwnd, SW_MINIMIZE);
    }

    bool isMaximized() const override { return m_hwnd && IsZoomed(m_hwnd); }

    void setMaximized(bool on) override {
        if (m_style == jf::JPlatformWindowStyle::Popup || !m_hwnd) return;
        ShowWindow(m_hwnd, on ? SW_MAXIMIZE : SW_RESTORE);
    }

    // Hand the drag to Windows and let DefWindowProc run it. Tracking the move ourselves would
    // mean re-implementing snap, multi-monitor edges and the Escape-to-cancel that users expect,
    // all of which come free with the native modal loop.
    void startWindowMove() override {
        if (m_style == jf::JPlatformWindowStyle::Popup || !m_hwnd) return;
        ReleaseCapture();   // the click that began the drag still holds it; the loop needs it back
        SendMessageW(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }

    // `direction` is the _NET_WM_MOVERESIZE code, because that is what the framework's edge
    // hit-test produces and X11 was implemented first. Translated here rather than changed at
    // the source, so one platform's protocol constant does not leak into the other's.
    void startWindowResize(uint32_t direction) override {
        if (m_style == jf::JPlatformWindowStyle::Popup || !m_hwnd || !m_resizable) return;
        static constexpr WPARAM kHit[] = {
            HTTOPLEFT, HTTOP, HTTOPRIGHT, HTRIGHT, HTBOTTOMRIGHT, HTBOTTOM, HTBOTTOMLEFT, HTLEFT
        };
        if (direction >= sizeof(kHit) / sizeof(kHit[0])) return;
        ReleaseCapture();
        SendMessageW(m_hwnd, WM_NCLBUTTONDOWN, kHit[direction], 0);
    }

    // Release whatever window currently holds the mouse capture. A window captures the mouse on
    // button-down (to keep motion/up during a drag-outside) and releases on button-up. But a modal
    // that opens a CHILD modal freezes before it sees its own button-up, so the capture would stick
    // and steal every mouse event the child should get. The modal-stack push calls this so the
    // superseded window's capture is dropped. ReleaseCapture() no-ops if nothing is captured.
    static void releaseActivePointerGrab() { ReleaseCapture(); }

    // Raw HWND (as uintptr_t) of the most recently created window — the modal stack reads this straight
    // after constructing a dialog so the NEXT nested modal can be parented to its opener. See the Linux
    // counterpart for the full rationale.
    static uintptr_t lastCreatedRawWindowId() { return s_lastCreatedRaw; }

    bool consumeFocusLost() override { bool v = m_focusLost; m_focusLost = false; return v; }
    // Reported from the EVENT being handled, not the live keyboard: with a queue, a press consumed a frame
    // later must still say what it happened under. (Ctrl and Shift had no override here at all, so they
    // answered the base's false — ctrl-click and shift-click could never work on this backend.)
    bool isAltDown()   const override { return m_evAlt || m_altDown; }
    bool isCtrlDown()  const override { return m_evCtrl; }
    bool isShiftDown() const override { return m_evShift; }

    std::pair<int,int> globalCursorPos() const override {
        POINT pt;
        if (GetCursorPos(&pt)) {
            return { static_cast<int>(pt.x), static_cast<int>(pt.y) };
        }
        return { 0, 0 };
    }

    bool isLeftButtonDown() const override {
        return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    }

    std::pair<int,int> virtualDesktopSize() const override {
        return { GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN) };
    }

    // Work area of the monitor under (px,py) — taskbar/appbars excluded, per-monitor (unlike the X11 side,
    // which has no RandR here and can only report the whole desktop work area).
    JScreenRect workAreaAt(int px, int py) const override {
        POINT pt{ px, py };
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        if (mon && GetMonitorInfo(mon, &mi))
            return JScreenRect{ mi.rcWork.left, mi.rcWork.top,
                                mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top };
        RECT wa{};
        if (SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0))
            return JScreenRect{ wa.left, wa.top, wa.right - wa.left, wa.bottom - wa.top };
        return JScreenRect{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    }
    float dpiScale() const override { return m_dpiScaleFactor; }
    void setResizeCallback(std::function<void(uint32_t, uint32_t)> cb) override {
        m_resizeCallback = cb;
    }
    // Framework-managed resize affordances (parity with the Linux window; used by the dialog windows).
    void setResizable(bool on)      { m_resizable = on; }
    void setResizeTopInset(float t) { m_resizeTopInset = t; }
    void setOpacity(float a) {
        const BYTE alpha = static_cast<BYTE>((a < 0.f ? 0.f : a > 1.f ? 1.f : a) * 255.0f);
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(m_hwnd, 0, alpha, LWA_ALPHA);
    }

    void setFullscreen(bool on) override {
        if (on) {
            SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(m_hwnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_FRAMECHANGED);
        } else {
            DWORD winStyle = WS_OVERLAPPEDWINDOW;
            if (m_style == JPlatformWindowStyle::Borderless) {
                winStyle = WS_POPUP | WS_SYSMENU;
            } else if (m_style == JPlatformWindowStyle::Popup) {
                winStyle = WS_POPUP;
            }
            SetWindowLongPtrW(m_hwnd, GWL_STYLE, winStyle | WS_VISIBLE);
            SetWindowPos(m_hwnd, nullptr, m_screenX, m_screenY, m_width, m_height, SWP_FRAMECHANGED | SWP_NOZORDER);
        }
    }

private:
    static LRESULT CALLBACK StaticWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        JWindowsPlatformWindow* pThis = nullptr;
        if (uMsg == WM_NCCREATE) {
            CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
            pThis = reinterpret_cast<JWindowsPlatformWindow*>(pCreate->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        } else {
            pThis = reinterpret_cast<JWindowsPlatformWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (pThis) {
            return pThis->handleMessage(hwnd, uMsg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    LRESULT handleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        switch (uMsg) {
            case WM_SETCURSOR: {
                // Windows reasserts the window class's cursor constantly; without answering here,
                // setCursor() would be undone before the user saw it. Only over the client area --
                // the frame keeps its own arrows.
                if (LOWORD(lParam) == HTCLIENT && m_cursor) { ::SetCursor(m_cursor); return TRUE; }
                break;
            }
            case WM_MOUSELEAVE: {
                m_mouseLeft     = true;
                m_mouseTracking = false;   // the request is one-shot; re-arm on the next move
                return 0;
            }
            case WM_MOUSEMOVE: {
                // WM_MOUSELEAVE is not sent unless it is asked for, once, per entry.
                if (!m_mouseTracking) {
                    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                    if (TrackMouseEvent(&tme)) m_mouseTracking = true;
                }
                m_mouseX = static_cast<float>(GET_X_LPARAM(lParam));
                m_mouseY = static_cast<float>(GET_Y_LPARAM(lParam));
                qCDebug(LogWin32Backend) << "WM_MOUSEMOVE: " << m_mouseX << ", " << m_mouseY << "\n";
                return 0;
            }
            case WM_LBUTTONDOWN: {
                m_mouseX = static_cast<float>(GET_X_LPARAM(lParam));
                m_mouseY = static_cast<float>(GET_Y_LPARAM(lParam));
                m_leftQueue.push_back({ true, m_mouseX, m_mouseY, _modCtrl(), _modShift(), _modAlt() });
                m_altDown = (GetKeyState(VK_MENU) & 0x8000) != 0;
                SetCapture(hwnd);
                qCDebug(LogWin32Backend) << "WM_LBUTTONDOWN: " << m_mouseX << ", " << m_mouseY << "\n";
                return 0;
            }
            case WM_LBUTTONUP: {
                m_leftQueue.push_back({ false, static_cast<float>(GET_X_LPARAM(lParam)),
                                               static_cast<float>(GET_Y_LPARAM(lParam)), _modCtrl(), _modShift(), _modAlt() });
                ReleaseCapture();
                qCDebug(LogWin32Backend) << "WM_LBUTTONUP: " << m_mouseX << ", " << m_mouseY << "\n";
                return 0;
            }
            case WM_RBUTTONDOWN: {
                m_mouseX = static_cast<float>(GET_X_LPARAM(lParam));
                m_mouseY = static_cast<float>(GET_Y_LPARAM(lParam));
                m_rightQueue.push_back({ true, m_mouseX, m_mouseY, _modCtrl(), _modShift(), _modAlt() });
                SetCapture(hwnd);
                return 0;
            }
            case WM_RBUTTONUP: {
                m_rightQueue.push_back({ false, static_cast<float>(GET_X_LPARAM(lParam)),
                                                static_cast<float>(GET_Y_LPARAM(lParam)), _modCtrl(), _modShift(), _modAlt() });
                ReleaseCapture();
                return 0;
            }
            case WM_MOUSEWHEEL: {
                m_wheelY += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA);
                qCDebug(LogWin32Backend) << "WM_MOUSEWHEEL: " << m_wheelY << "\n";
                return 0;
            }
            case WM_GETMINMAXINFO: {
                // Windows clamps a MAXIMIZED window to the monitor work area only for ordinary
                // framed windows. This one is borderless, so without this it would maximise over
                // the taskbar and the user would lose the way back to everything else.
                if (m_style != JPlatformWindowStyle::Normal) {
                    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
                    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                    if (mon && GetMonitorInfo(mon, &mi)) {
                        auto* mm = reinterpret_cast<MINMAXINFO*>(lParam);
                        // Maximised position is expressed relative to the monitor, not the desktop.
                        mm->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                        mm->ptMaxPosition.y = mi.rcWork.top  - mi.rcMonitor.top;
                        mm->ptMaxSize.x     = mi.rcWork.right  - mi.rcWork.left;
                        mm->ptMaxSize.y     = mi.rcWork.bottom - mi.rcWork.top;
                        return 0;
                    }
                }
                break;
            }
            case WM_SIZE: {
                m_width = LOWORD(lParam);
                m_height = HIWORD(lParam);
                qCInfo(LogWin32Backend) << "WM_SIZE: " << m_width << "x" << m_height << "\n";
                if (m_resizeCallback && m_width && m_height) m_resizeCallback(m_width, m_height);
                return 0;
            }
            case WM_WINDOWPOSCHANGED: {
                WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(lParam);
                if (wp && !(wp->flags & SWP_NOMOVE)) {
                    m_screenX = wp->x;
                    m_screenY = wp->y;
                    qCDebug(LogWin32Backend) << "WM_WINDOWPOSCHANGED position: " << m_screenX << ", " << m_screenY << "\n";
                }
                break;
            }
            case WM_KILLFOCUS: {
                m_focusLost = true;
                qCInfo(LogWin32Backend) << "WM_KILLFOCUS\n";
                return 0;
            }
            case WM_DESTROY: {
                if (m_style == JPlatformWindowStyle::Normal) {
                    PostQuitMessage(0);
                }
                m_closeRequested = true;
                qCInfo(LogWin32Backend) << "WM_DESTROY\n";
                return 0;
            }
            case WM_CLOSE: {
                m_closeRequested = true;
                qCInfo(LogWin32Backend) << "WM_CLOSE\n";
                return 0;
            }
        }
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    HWND m_hwnd{nullptr};
    // Raw HWND of the most recently constructed window (see lastCreatedRawWindowId()).
    inline static uintptr_t s_lastCreatedRaw = 0;
    HINSTANCE m_hInstance{nullptr};
    jf::JPlatformWindowStyle m_style;
    bool m_closeRequested;
    float m_dpiScaleFactor;

    int      m_screenX{0};
    int      m_screenY{0};
    uint32_t m_width{0};
    uint32_t m_height{0};
    std::function<void(uint32_t, uint32_t)> m_resizeCallback;   // swapchain resize hook (fired on WM_SIZE)
    bool  m_resizable{true};
    float m_resizeTopInset{0.0f};
    // NO POINTER YET IS NOT A POINTER AT THE ORIGIN. These start OUTSIDE, where a mouse-leave puts
    // them, because a freshly created window has received no motion event and (0,0) is a
    // real, hittable place: the top-left of whatever it contains. A combo dropdown opened on its
    // current selection had that selection overwritten on its very first poll, by a "hover" over
    // the first row from a pointer that was still up in the combo box. Every consumer already
    // handles -1: the mouse-leave path has always produced it.
    float m_mouseX{-1.0f};
    float m_mouseY{-1.0f};
    float m_wheelY{0.0f};
    // Queued button events, per button, in arrival order — see LinuxPlatformWindow for why a flag is not
    // enough: it drops the second of two clicks in a frame, collapses a press and its release into one
    // moment, and lets later motion overwrite the position the press happened at.
    // See the note in the X11 window: an event carries the modifiers it HAPPENED under, because with a
    // queue it may be consumed a frame later, after a key event has moved the window-global state on.
    struct JButtonEvent { bool press; float x, y; bool ctrl, shift, alt; };
    bool m_evCtrl = false, m_evShift = false, m_evAlt = false;   // …as reported by isCtrlDown() etc.
    // The live keyboard modifiers, at the moment an event is recorded.
    static bool _modCtrl()  { return (GetKeyState(VK_CONTROL) & 0x8000) != 0; }
    static bool _modShift() { return (GetKeyState(VK_SHIFT)   & 0x8000) != 0; }
    static bool _modAlt()   { return (GetKeyState(VK_MENU)    & 0x8000) != 0; }
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


    bool _takeButton(std::deque<JButtonEvent>& q, bool wantPress) {
        if (q.empty() || q.front().press != wantPress) return false;
        ++(&q == &m_leftQueue ? m_leftTaken : m_rightTaken);
        m_mouseX = q.front().x;
        m_mouseY = q.front().y;
        m_evCtrl  = q.front().ctrl;      // the modifiers this event happened under, not the live keyboard
        m_evShift = q.front().shift;
        m_evAlt   = q.front().alt;
        q.pop_front();
        return true;
    }
    bool  m_focusLost{false};
    bool  m_mouseLeft{false};      // WM_MOUSELEAVE seen; consumed by consumeMouseLeave()
    bool  m_mouseTracking{false};  // a TrackMouseEvent request is outstanding
    bool  m_mapped{true};          // windows are created shown
    HCURSOR m_cursor{nullptr};     // what WM_SETCURSOR should reassert
    bool  m_altDown{false};

    std::deque<jf::JKeyEvent> m_keyQueue;
};

} // inline namespace jf
