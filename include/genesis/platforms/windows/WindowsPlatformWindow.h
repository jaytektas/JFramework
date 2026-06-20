#pragma once

#include <genesis/core/ApplicationCore.h>
#include <genesis/core/FocusManager.h>
#include <genesis/graphics/GpuHal.h>
#include <windows.h>
#include <windowsx.h>
#include <stdexcept>
#include <algorithm>
#include <vector>
#include <deque>
#include <utility>

// Custom Logging Integration
inline constexpr Genesis::Log::Category LogWin32Backend{"Win32Backend"};

namespace Genesis {

class WindowsPlatformWindow : public Core::PlatformWindow {
public:
    using KeyEvent = Genesis::KeyEvent;
    WindowsPlatformWindow(const std::string& title, uint32_t width, uint32_t height,
                          int screenX = 100, int screenY = 100,
                          PlatformWindowStyle style = PlatformWindowStyle::Normal,
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
        wc.lpfnWndProc = WindowsPlatformWindow::StaticWindowProc;
        wc.hInstance = m_hInstance;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = className;

        RegisterClassExW(&wc);

        DWORD winStyle = WS_OVERLAPPEDWINDOW;
        if (style == PlatformWindowStyle::Borderless) {
            winStyle = WS_POPUP | WS_SYSMENU;
        } else if (style == PlatformWindowStyle::Popup) {
            winStyle = WS_POPUP;
        }

        RECT rect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        AdjustWindowRect(&rect, winStyle, FALSE);

        m_hwnd = CreateWindowExW(
            0,
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

        ShowWindow(m_hwnd, SW_SHOW);
        UpdateWindow(m_hwnd);
    }

    ~WindowsPlatformWindow() override {
        if (m_hwnd) {
            DestroyWindow(m_hwnd);
        }
    }

    void pollNativeEvents() override {
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
    bool  consumePress() override { bool v = m_pendingPress; m_pendingPress = false; return v; }
    bool  consumeRelease() override { bool v = m_pendingRelease; m_pendingRelease = false; return v; }
    float consumeWheel() override { float v = m_wheelY; m_wheelY = 0.0f; return v; }

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

    void setCursor(Genesis::PlatformCursor shape) override { (void)shape; }
    Genesis::PlatformWindowStyle windowStyle() const override { return m_style; }

    Genesis::NativeWindowHandle nativeHandle() const override {
        Genesis::NativeWindowHandle h{};
        h.apiTarget         = Genesis::GpuApiType::Vulkan;
        h.connectionPointer = m_hInstance;
        h.windowPointer     = m_hwnd;
        return h;
    }

    HWND nativeWindow() const { return m_hwnd; }
    HINSTANCE nativeInstance() const { return m_hInstance; }

    bool consumeFocusLost() override { bool v = m_focusLost; m_focusLost = false; return v; }
    bool isAltDown() const override { return m_altDown; }

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

    void setFullscreen(bool on) override {
        if (on) {
            SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(m_hwnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_FRAMECHANGED);
        } else {
            DWORD winStyle = WS_OVERLAPPEDWINDOW;
            if (m_style == PlatformWindowStyle::Borderless) {
                winStyle = WS_POPUP | WS_SYSMENU;
            } else if (m_style == PlatformWindowStyle::Popup) {
                winStyle = WS_POPUP;
            }
            SetWindowLongPtrW(m_hwnd, GWL_STYLE, winStyle | WS_VISIBLE);
            SetWindowPos(m_hwnd, nullptr, m_screenX, m_screenY, m_width, m_height, SWP_FRAMECHANGED | SWP_NOZORDER);
        }
    }

private:
    static LRESULT CALLBACK StaticWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        WindowsPlatformWindow* pThis = nullptr;
        if (uMsg == WM_NCCREATE) {
            CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
            pThis = reinterpret_cast<WindowsPlatformWindow*>(pCreate->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        } else {
            pThis = reinterpret_cast<WindowsPlatformWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (pThis) {
            return pThis->handleMessage(hwnd, uMsg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    LRESULT handleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        switch (uMsg) {
            case WM_MOUSEMOVE: {
                m_mouseX = static_cast<float>(GET_X_LPARAM(lParam));
                m_mouseY = static_cast<float>(GET_Y_LPARAM(lParam));
                qCDebug(Genesis::Log::Platform) << "WM_MOUSEMOVE: " << m_mouseX << ", " << m_mouseY << "\n";
                return 0;
            }
            case WM_LBUTTONDOWN: {
                m_mouseX = static_cast<float>(GET_X_LPARAM(lParam));
                m_mouseY = static_cast<float>(GET_Y_LPARAM(lParam));
                m_pendingPress = true;
                m_altDown = (GetKeyState(VK_MENU) & 0x8000) != 0;
                SetCapture(hwnd);
                qCDebug(Genesis::Log::Platform) << "WM_LBUTTONDOWN: " << m_mouseX << ", " << m_mouseY << "\n";
                return 0;
            }
            case WM_LBUTTONUP: {
                m_pendingRelease = true;
                ReleaseCapture();
                qCDebug(Genesis::Log::Platform) << "WM_LBUTTONUP: " << m_mouseX << ", " << m_mouseY << "\n";
                return 0;
            }
            case WM_MOUSEWHEEL: {
                m_wheelY += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA);
                qCDebug(Genesis::Log::Platform) << "WM_MOUSEWHEEL: " << m_wheelY << "\n";
                return 0;
            }
            case WM_SIZE: {
                m_width = LOWORD(lParam);
                m_height = HIWORD(lParam);
                qCInfo(Genesis::Log::Platform) << "WM_SIZE: " << m_width << "x" << m_height << "\n";
                return 0;
            }
            case WM_WINDOWPOSCHANGED: {
                WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(lParam);
                if (wp && !(wp->flags & SWP_NOMOVE)) {
                    m_screenX = wp->x;
                    m_screenY = wp->y;
                    qCDebug(Genesis::Log::Platform) << "WM_WINDOWPOSCHANGED position: " << m_screenX << ", " << m_screenY << "\n";
                }
                break;
            }
            case WM_KILLFOCUS: {
                m_focusLost = true;
                qCInfo(Genesis::Log::Platform) << "WM_KILLFOCUS\n";
                return 0;
            }
            case WM_DESTROY: {
                if (m_style == PlatformWindowStyle::Normal) {
                    PostQuitMessage(0);
                }
                m_closeRequested = true;
                qCInfo(Genesis::Log::Platform) << "WM_DESTROY\n";
                return 0;
            }
            case WM_CLOSE: {
                m_closeRequested = true;
                qCInfo(Genesis::Log::Platform) << "WM_CLOSE\n";
                return 0;
            }
        }
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    HWND m_hwnd{nullptr};
    HINSTANCE m_hInstance{nullptr};
    Genesis::PlatformWindowStyle m_style;
    bool m_closeRequested;
    float m_dpiScaleFactor;

    int      m_screenX{0};
    int      m_screenY{0};
    uint32_t m_width{0};
    uint32_t m_height{0};
    float m_mouseX{0.0f};
    float m_mouseY{0.0f};
    float m_wheelY{0.0f};
    bool  m_pendingPress{false};
    bool  m_pendingRelease{false};
    bool  m_focusLost{false};
    bool  m_altDown{false};

    std::deque<Genesis::KeyEvent> m_keyQueue;
};

} // namespace Genesis
