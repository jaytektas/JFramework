#pragma once

#include <thread>
#include <atomic>
#include <chrono>
#include "Signal.h"
#include "MainThreadDispatcher.h"

namespace Genesis {

// Repeating or single-shot timer backed by a private thread.
// onTick is dispatched to the main thread via MainThreadDispatcher so callbacks
// can safely update widgets without locks.
class Timer {
public:
    enum class Mode { SingleShot, Repeating };

    Timer() = default;

    Timer(std::chrono::milliseconds interval, Mode mode = Mode::Repeating) {
        start(interval, mode);
    }

    ~Timer() { stop(); }

    Timer(const Timer&)            = delete;
    Timer& operator=(const Timer&) = delete;

    Core::Signal<> onTick;

    void start(std::chrono::milliseconds interval, Mode mode = Mode::Repeating) {
        stop();
        m_running = true;
        m_thread = std::thread([this, interval, mode]() {
            while (m_running.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(interval);
                if (!m_running.load(std::memory_order_relaxed)) break;
                MainThreadDispatcher::instance().post([this]{ onTick.emit(); });
                if (mode == Mode::SingleShot) break;
            }
            m_running = false;
        });
    }

    void stop() {
        m_running = false;
        if (m_thread.joinable())
            m_thread.join();
    }

    bool isRunning() const { return m_running.load(); }

private:
    std::atomic<bool> m_running{false};
    std::thread       m_thread;
};

} // namespace Genesis
