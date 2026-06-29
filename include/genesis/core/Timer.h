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
// onTick fires on the main thread via JMainThreadDispatcher — safe for widget updates.
//
// Usage:
//   JTimer t(std::chrono::milliseconds(100));          // repeating
//   t.onTick.connect([](){ updateDisplay(); });
//
//   JTimer::singleShot(std::chrono::seconds(2), [](){ showToast(); });
class JTimer {
public:
    enum class JMode { SingleShot, Repeating };

    JTimer() = default;

    explicit JTimer(std::chrono::milliseconds interval, JMode mode = JMode::Repeating) {
        start(interval, mode);
    }

    ~JTimer() {
        // Invalidate queued-but-not-drained lambdas before destruction so they
        // don't touch onTick after this object is gone.
        if (m_alive) m_alive->store(false, std::memory_order_release);
        m_running = false;
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    JTimer(const JTimer&)            = delete;
    JTimer& operator=(const JTimer&) = delete;

    Core::JSignal<> onTick;

    void start(std::chrono::milliseconds interval, JMode mode = JMode::Repeating) {
        // Clear sentinel before stopping — the old thread must not fire after restart.
        if (m_alive) m_alive->store(false, std::memory_order_release);
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

                // Capture sentinel by value — if the JTimer is destroyed before
                // this lambda runs on the main thread, the check prevents
                // touching the dead onTick.
                JMainThreadDispatcher::instance().post([this, alive]{
                    if (alive->load(std::memory_order_relaxed))
                        onTick.emit();
                });

                if (mode == JMode::SingleShot) break;
            }
            m_running = false;
        });
    }

    // Stops the timer — no new ticks will be queued, but lambdas already posted
    // to JMainThreadDispatcher are still valid to fire (this object is still alive).
    void stop() {
        if (!m_running.exchange(false)) return;
        // Do NOT clear m_alive here — queued lambdas can safely call onTick.emit().
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    bool isRunning() const { return m_running.load(std::memory_order_relaxed); }

    // Fire cb once on the main thread after delay, no JTimer object needed.
    static void singleShot(std::chrono::milliseconds delay, std::function<void()> cb) {
        std::thread([delay, cb = std::move(cb)]() {
            std::this_thread::sleep_for(delay);
            JMainThreadDispatcher::instance().post(std::move(cb));
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
