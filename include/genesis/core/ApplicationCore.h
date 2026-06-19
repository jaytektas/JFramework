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

#include <genesis/core/muted_logging_mock.h>
namespace { inline constexpr auto& LogEngineCore = Genesis::Log::Core; }

namespace Core {

/**
 * @brief Normalized cross-platform raw input data package.
 */
struct InputEvent {
    enum class Type { KeyDown, KeyUp, MouseMove, MouseButtonDown, MouseButtonUp, Scroll };
    Type type;
    int32_t deviceId = 0;
    double timestamp = 0.0;
    
    union {
        struct { uint32_t code; uint32_t modifiers; } key;
        struct { double x; double y; uint32_t buttons; } mouse;
    } data;
};

/**
 * @brief Base Platform Window Interface. 
 */
class PlatformWindow {
public:
    virtual ~PlatformWindow() = default;
    virtual void pollNativeEvents() = 0;
    virtual void swapBuffers() = 0;
    virtual void setVSync(bool enabled) = 0;
    virtual bool shouldClose() const = 0;
};

/**
 * @brief Ultra-stable, high-performance cross-platform application runtime manager.
 */
class Application {
public:
    using Task = std::function<void()>;

    Application() : m_isRunning(false), m_targetFrameTime(std::chrono::microseconds(16666)) {} 
    ~Application() { shutdown(); }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

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

    void setTargetFps(uint32_t fps) {
        if (fps == 0) {
            m_targetFrameTime = std::chrono::microseconds(0);
        } else {
            m_targetFrameTime = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::seconds(1) / fps
            );
        }
    }

    int run(std::unique_ptr<PlatformWindow> window) {
        if (!window) {
            qCCritical(LogEngineCore) << "Failed to start runtime application loop: Window Abstraction layer is null." << std::endl;
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
        (void)deltaTime; 
    }

    std::atomic<bool> m_isRunning;
    std::unique_ptr<PlatformWindow> m_window;
    
    std::mutex m_queueMutex;
    std::vector<Task> m_incomingTasks; 
    std::chrono::microseconds m_targetFrameTime;
};

} // namespace Core
