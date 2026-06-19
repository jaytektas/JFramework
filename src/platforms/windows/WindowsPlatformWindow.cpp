#include <genesis/core/ApplicationCore.h>
#include <iostream>
#include <stdexcept>

// Minimum native Windows protocol headers required for absolute bare-metal bootstrapping
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

// --- Custom Logging Integration ---
#ifndef qCWarning
#define qCWarning(category) std::cerr << "[WARNING] "
#define qCInfo(category) std::cout << "[INFO] "
struct MockCategoryWin32 {};
inline MockCategoryWin32 LogWin32Backend;
#endif

namespace Genesis {

/**
 * @brief Concrete Windows implementation of the platform-agnostic PlatformWindow interface.
 * Implements low-overhead bare-metal protocol channeling using native Win32 handles.
 */
class WindowsPlatformWindow : public Core::PlatformWindow {
public:
    WindowsPlatformWindow(Core::Application& app, const std::string& title, uint32_t width, uint32_t height)
        : m_appContext(app), m_hwnd(nullptr), m_hInstance(GetModuleHandleW(nullptr)), m_closeRequested(false), m_dpiScaleFactor(1.0f) 
    {
        // 1. Enable modern High-DPI awareness (Windows 10 1607+)
        // Uses dynamic linking to avoid breaking on older Windows versions
        HMODULE user32 = LoadLibraryA("user32.dll");
        if (user32) {
            typedef BOOL(WINAPI *SetProcessDpiAwarenessContextProc)(DPI_AWARENESS_CONTEXT);
            auto setDpiAwareness = (SetProcessDpiAwarenessContextProc)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
            if (setDpiAwareness) {
                setDpiAwareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            }
            FreeLibrary(user32);
        }

        // 2. Register native Win32 Window Class
        std::wstring wTitle(title.begin(), title.end());
        LPCWSTR className = L"GenesisNativeWindowClass";

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc = WindowsPlatformWindow::StaticWindowProc;
        wc.hInstance = m_hInstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = className;

        if (!RegisterClassExW(&wc)) {
            throw std::runtime_error("Fatal: Genesis failed to register native Win32 window class.");
        }

        // 3. Create the Window Surface
        DWORD style = WS_OVERLAPPEDWINDOW;
        
        RECT rect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        AdjustWindowRect(&rect, style, FALSE);

        m_hwnd = CreateWindowExW(
            0,
            className,
            wTitle.c_str(),
            style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left,
            rect.bottom - rect.top,
            nullptr,
            nullptr,
            m_hInstance,
            this // Pass context to WM_NCCREATE
        );

        if (!m_hwnd) {
            throw std::runtime_error("Fatal: Failed to instantiate Win32 HWND surface.");
        }

        // 4. Calculate native High-DPI scaling factor for this specific monitor
        HDC hdc = GetDC(m_hwnd);
        if (hdc) {
            int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
            m_dpiScaleFactor = std::max(1.0f, static_cast<float>(dpi) / 96.0f);
            ReleaseDC(m_hwnd, hdc);
        }

        ShowWindow(m_hwnd, SW_SHOW);
        UpdateWindow(m_hwnd);

        qCInfo(LogWin32Backend) << "Bare-metal Windows HWND mapped safely. Native DPI Scale Factor: " << m_dpiScaleFactor << std::endl;
    }

    virtual ~WindowsPlatformWindow() {
        if (m_hwnd) {
            DestroyWindow(m_hwnd);
        }
    }

    /**
     * @brief Polls the native OS event queue, transforming raw events into core loop structures.
     */
    void pollNativeEvents() override {
        MSG msg;
        // Non-blocking pump to ensure we don't stall the Genesis Frame pacing loop
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                m_closeRequested = true;
            } else {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    void swapBuffers() override {
        // Managed natively inside Vulkan presentation queues via GpuHal
    }

    void setVSync(bool enabled) override {
        (void)enabled;
    }

    bool shouldClose() const override {
        return m_closeRequested;
    }

    // Expose raw native pointers for Vulkan GpuHal Binding
    HINSTANCE getNativeInstance() const { return m_hInstance; }
    HWND getNativeWindow() const { return m_hwnd; }

private:
    /**
     * @brief Standard native message router mapping back to our class instance.
     */
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
            return pThis->handleMessage(uMsg, wParam, lParam);
        }

        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    /**
     * @brief Instance-specific message handler transforming Win32 payloads into Genesis InputEvents.
     */
    LRESULT handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
        switch (uMsg) {
            case WM_MOUSEMOVE: {
                Core::InputEvent ie{};
                ie.type = Core::InputEvent::Type::MouseMove;
                ie.data.mouse.x = static_cast<double>(GET_X_LPARAM(lParam)) / m_dpiScaleFactor;
                ie.data.mouse.y = static_cast<double>(GET_Y_LPARAM(lParam)) / m_dpiScaleFactor;
                
                m_appContext.postToMainThread([ie]() {
                    // Hook context: Framework logical input routers capture 'ie' here
                });
                return 0;
            }
            case WM_LBUTTONDOWN: {
                Core::InputEvent ie{};
                ie.type = Core::InputEvent::Type::MouseButtonDown;
                ie.data.mouse.buttons = 1; // Left click map
                
                m_appContext.postToMainThread([ie]() {
                    // Trigger root framework hit testing sequence vectors
                });
                return 0;
            }
            case WM_LBUTTONUP: {
                Core::InputEvent ie{};
                ie.type = Core::InputEvent::Type::MouseButtonUp;
                ie.data.mouse.buttons = 1; 
                
                m_appContext.postToMainThread([ie]() {
                    // Trigger release handlers
                });
                return 0;
            }
            case WM_DESTROY: {
                PostQuitMessage(0);
                return 0;
            }
            case WM_CLOSE: {
                m_closeRequested = true;
                return 0;
            }
            default:
                break;
        }
        return DefWindowProcW(m_hwnd, uMsg, wParam, lParam);
    }

    Core::Application& m_appContext;
    HWND m_hwnd;
    HINSTANCE m_hInstance;
    bool m_closeRequested;
    float m_dpiScaleFactor;
};

} // namespace Genesis
