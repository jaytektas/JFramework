#pragma once

#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <memory>
#include "Signal.h"
#include "MainThreadDispatcher.h"

namespace Genesis {

// Repeating or single-shot timer backed by a private thread.
// onTick fires on the main thread via MainThreadDispatcher — safe for widget updates.
//
// Usage:
//   Timer t(std::chrono::milliseconds(100));          // repeating
//   t.onTick.connect([](){ updateDisplay(); });
//
//   Timer::singleShot(std::chrono::seconds(2), [](){ showToast(); });
class Timer {
public:
    enum class Mode { SingleShot, Repeating };

    Timer() = default;

    explicit Timer(std::chrono::milliseconds interval, Mode mode = Mode::Repeating) {
        start(interval, mode);
    }

    ~Timer() { stop(); }

    Timer(const Timer&)            = delete;
    Timer& operator=(const Timer&) = delete;

    Core::Signal<> onTick;

    void start(std::chrono::milliseconds interval, Mode mode = Mode::Repeating) {
        stop();
        // New sentinel each start() — invalidates any callbacks still queued
        // from a previous run that haven't fired yet.
        m_alive   = std::make_shared<std::atomic<bool>>(true);
        m_running = true;

        m_thread = std::thread([this, interval, mode, alive = m_alive]() {
            while (true) {
                {
                    std::unique_lock<std::mutex> lk(m_cvMutex);
                    m_cv.wait_for(lk, interval,
                                  [this]{ return !m_running.load(std::memory_order_relaxed); });
                }
                if (!m_running.load(std::memory_order_relaxed)) break;

                // Capture sentinel by value — if the Timer is destroyed before
                // this lambda runs on the main thread, the check prevents
                // touching the dead onTick.
                MainThreadDispatcher::instance().post([this, alive]{
                    if (alive->load(std::memory_order_relaxed))
                        onTick.emit();
                });

                if (mode == Mode::SingleShot) break;
            }
            m_running = false;
        });
    }

    // Stops the timer and returns immediately — does not block for the current interval.
    void stop() {
        if (!m_running.exchange(false)) return;
        if (m_alive) m_alive->store(false, std::memory_order_relaxed);
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    bool isRunning() const { return m_running.load(std::memory_order_relaxed); }

    // Fire cb once on the main thread after delay, no Timer object needed.
    static void singleShot(std::chrono::milliseconds delay, std::function<void()> cb) {
        std::thread([delay, cb = std::move(cb)]() {
            std::this_thread::sleep_for(delay);
            MainThreadDispatcher::instance().post(std::move(cb));
        }).detach();
    }

private:
    std::shared_ptr<std::atomic<bool>> m_alive;
    std::atomic<bool>                  m_running{false};
    std::thread                        m_thread;
    std::mutex                         m_cvMutex;
    std::condition_variable            m_cv;
};

} // namespace Genesis
