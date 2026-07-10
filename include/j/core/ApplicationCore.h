#pragma once

#include <iostream>
#include <vector>
#include <functional>
#include <memory>
#include <chrono>
#include <atomic>
#include <mutex>
#include <concepts>
#include <thread>

#include <j/core/muted_logging_mock.h>
namespace { inline constexpr auto& LogEngineCore = jf::Log::Core; }

#include <j/core/PlatformCommon.h>
#include <j/graphics/GpuHal.h>

#include <j/core/KeyEvent.h>
#include <j/core/MainThreadDispatcher.h>

inline namespace jf {

/**
 * @brief Normalized cross-platform raw input data package.
 */
struct JInputEvent {
    enum class JType { KeyDown, KeyUp, MouseMove, MouseButtonDown, MouseButtonUp, Scroll };
    JType type;
    int32_t deviceId = 0;
    double timestamp = 0.0;
    
    union {
        struct { uint32_t code; uint32_t modifiers; } key;
        struct { double x; double y; uint32_t buttons; } mouse;
    } data;
};

/**
 * @brief Base Platform JWindow Interface. 
 */
class JPlatformWindow {
public:
    virtual ~JPlatformWindow() = default;
    virtual void pollNativeEvents() = 0;
    virtual void swapBuffers() = 0;
    virtual void setVSync(bool enabled) = 0;
    virtual bool shouldClose() const = 0;

    // Common mouse and wheel getters/modifiers
    virtual float mouseX() const { return 0.0f; }
    virtual float mouseY() const { return 0.0f; }
    virtual bool  consumePress() { return false; }
    virtual bool  consumeRelease() { return false; }
    virtual bool  consumeRightPress() { return false; }
    virtual bool  consumeRightRelease() { return false; }
    virtual float consumeWheel() { return 0.0f; }
    
    // Keyboard events
    virtual bool hasKeyEvents() const { return false; }
    virtual jf::JKeyEvent consumeKey() { return {}; }
    virtual std::vector<jf::JKeyEvent> consumeAllKeys() { return {}; }

    // JWindow dimensions and state
    virtual int      screenX() const { return 0; }
    virtual int      screenY() const { return 0; }
    virtual uint32_t width()   const { return 0; }
    virtual uint32_t height()  const { return 0; }
    virtual void     setPosition(int x, int y) { (void)x; (void)y; }
    virtual void     setSize(uint32_t w, uint32_t h) { (void)w; (void)h; }
    virtual void     setCursor(jf::JPlatformCursor shape) { (void)shape; }
    virtual jf::JPlatformWindowStyle windowStyle() const { return jf::JPlatformWindowStyle::Normal; }
    virtual jf::JNativeWindowHandle nativeHandle() const { return {}; }

    // Text clipboard, backed by the OS selection/clipboard. Default: unsupported
    // (headless / other backends) — the framework then falls back to an in-process store.
    virtual void        setClipboardText(const std::string& text) { (void)text; }
    virtual std::string getClipboardText() { return {}; }

    // Focus & state checks
    virtual bool consumeFocusLost()  { return false; }
    virtual bool consumeMouseLeave() { return false; }
    virtual bool consumeWasResized() { return false; }
    virtual bool consumeWasUnsnapped() { return false; }
    virtual bool isAltDown() const { return false; }
    virtual bool isCtrlDown() const { return false; }
    virtual bool isShiftDown() const { return false; }

    virtual std::pair<int,int> globalCursorPos() const { return {0, 0}; }
    virtual bool isLeftButtonDown() const { return false; }

    virtual std::pair<int,int> virtualDesktopSize() const { return {1920, 1080}; }
    virtual void setFullscreen(bool on)  { (void)on; }
    virtual void minimize()              {}
    virtual void setMaximized(bool on)   { (void)on; }
    virtual bool isMaximized() const     { return false; }
    virtual void requestClose()          {}
    virtual void clearCloseRequest()     {}   // veto a pending close (app intercepted it, e.g. save prompt)
    virtual void startWindowMove()              {}
    // dir: 0=TL,1=T,2=TR,3=R,4=BR,5=B,6=BL,7=L  (_NET_WM_MOVERESIZE directions)
    virtual void startWindowResize(uint32_t dir) { (void)dir; }
    virtual void warpCursor(int gx, int gy)    { (void)gx; (void)gy; }
    virtual void grabKeyboardFocus()     {}
    virtual uintptr_t rawWindowId() const { return 0; }
    virtual float dpiScale() const { return 1.0f; }
    virtual void setResizeCallback(std::function<void(uint32_t, uint32_t)> cb) { (void)cb; }
    virtual void setMinSize(uint32_t minW, uint32_t minH) { (void)minW; (void)minH; }
    // Appended LAST so it adds a new vtable slot without shifting existing ones (no lib rebuild needed).
    virtual void setTitle(const std::string& title) { (void)title; }   // runtime WM title (no-op default)
};

/**
 * @brief Ultra-stable, high-performance cross-platform application runtime manager.
 */
class JApplication {
public:
    using Task = std::function<void()>;

    JApplication() : m_isRunning(false), m_targetFrameTime(std::chrono::microseconds(16666)) {} 
    virtual ~JApplication() { shutdown(); }   // base of the GUI application tier

    JApplication(const JApplication&) = delete;
    JApplication& operator=(const JApplication&) = delete;

    /**
     * @brief Post a task safely from any worker thread into the main UI loop.
     */
    void postToMainThread(Task task) {
        if (!task) return;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_incomingTasks.push_back(std::move(task));
        }
    }

    // Called once per frame (main thread, before swapBuffers).
    // Wire AI snapshot publishing, JMainThreadDispatcher::drain(), etc. here.
    std::function<void(double deltaTime)> onFrameUpdate;

    void setTargetFps(uint32_t fps) {
        if (fps == 0) {
            m_targetFrameTime = std::chrono::microseconds(0);
        } else {
            m_targetFrameTime = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::seconds(1) / fps
            );
        }
    }

    int run(std::unique_ptr<JPlatformWindow> window) {
        if (!window) {
            qCCritical(LogEngineCore) << "Failed to start runtime application loop: JWindow Abstraction layer is null." << std::endl;
            return -1;
        }

        m_window = std::move(window);
        m_isRunning = true;
        m_window->setVSync(true);

        qCInfo(LogEngineCore) << "Engine core heartbeat successfully initialized." << std::endl;

        auto lastFrameTime = std::chrono::high_resolution_clock::now();

        while (m_isRunning && !m_window->shouldClose()) {
            auto currentFrameStart = std::chrono::high_resolution_clock::now();
            
            m_window->pollNativeEvents();

            executeDispatchedTasks();

            double deltaTime = std::chrono::duration<double, std::ratio<1>>(currentFrameStart - lastFrameTime).count();
            lastFrameTime = currentFrameStart;
            
            updateEngineSystems(deltaTime);

            m_window->swapBuffers();

            if (m_targetFrameTime.count() > 0) {
                auto workDuration = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::high_resolution_clock::now() - currentFrameStart
                );
                if (workDuration < m_targetFrameTime) {
                    std::this_thread::sleep_for(m_targetFrameTime - workDuration);
                }
            }
        }

        shutdown();
        return 0;
    }

    void shutdown() {
        if (m_isRunning) {
            m_isRunning = false;
            qCInfo(LogEngineCore) << "Engine core heartbeat loop tearing down gracefully." << std::endl;
        }
    }

private:
    void executeDispatchedTasks() {
        std::vector<Task> processingList;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (!m_incomingTasks.empty()) {
                processingList.swap(m_incomingTasks);
            }
        }

        for (const auto& task : processingList) {
            if (task) {
                try {
                    task();
                } catch (const std::exception& e) {
                    qCCritical(LogEngineCore) << "Exception dropped inside background-dispatched main-thread task: " << e.what() << std::endl;
                }
            }
        }
    }

    void updateEngineSystems(double deltaTime) {
        // Always drain the main-thread dispatcher so JTimer/JSerialPort callbacks
        // reach the UI even when used without a GUI application.
        jf::JMainThreadDispatcher::instance().drain();
        onFrameTick(deltaTime);                 // GUI tier (subclass) injects per-frame work
        if (onFrameUpdate) onFrameUpdate(deltaTime);
    }

protected:
    // Per-frame hook for the derived GUI application tier (it publishes its semantic
    // snapshot here), keeping the public onFrameUpdate free for the user's own callback.
    virtual void onFrameTick(double /*deltaTime*/) {}

private:
    std::atomic<bool> m_isRunning;
    std::unique_ptr<JPlatformWindow> m_window;
    
    std::mutex m_queueMutex;
    std::vector<Task> m_incomingTasks; 
    std::chrono::microseconds m_targetFrameTime;
};

} // inline namespace jf
