#pragma once

#include <functional>
#include <mutex>
#include <vector>

namespace Genesis {

// ============================================================================
// JMainThreadDispatcher — thread-safe callback queue (Step 4: thread-safe signals)
//
// Worker threads post callbacks here; the main loop drains them each frame.
// This makes JTimer::onTick, JSerialPort::onData, and JDatabase async queries safe
// to emit from any thread without races on the widget tree.
//
// Main loop integration (call once per frame, before rendering):
//   JMainThreadDispatcher::instance().drain();
//
// Usage from any thread:
//   JMainThreadDispatcher::instance().post([this]{ myWidget->setValue(x); });
// ============================================================================
class JMainThreadDispatcher {
public:
    static JMainThreadDispatcher& instance() {
        static JMainThreadDispatcher inst;
        return inst;
    }

    // Thread-safe. Queues callback for execution on the main thread.
    void post(std::function<void()> cb) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.push_back(std::move(cb));
    }

    // Call from the main thread each frame. Returns number of callbacks drained.
    int drain() {
        std::vector<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            batch.swap(m_pending);
        }
        for (auto& cb : batch) cb();
        return static_cast<int>(batch.size());
    }

    bool hasPending() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_pending.empty();
    }

private:
    JMainThreadDispatcher() = default;
    mutable std::mutex               m_mutex;
    std::vector<std::function<void()>> m_pending;
};

} // namespace Genesis
