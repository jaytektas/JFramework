// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <vector>

inline namespace jf {

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
//
// ---------------------------------------------------------------------------
// BURSTS — when "once per frame" is the wrong rate
//
// Draining once per frame ties a subsystem's callback rate to the paint rate.
// That is right for a timer tick or a one-off async reply, and badly wrong for a
// TRANSFER: a request/reply protocol with one frame in flight advances by exactly
// one reply per drain, so moving 140 KB in 1 KB replies costs 140 rendered frames.
// On a GPU that is a fifth of a second; on a software rasteriser it is half a
// minute, and the frames are redundant — nothing on screen changes between them.
//
// A subsystem that is mid-transfer says so, and the run loop then services its
// callbacks at the transfer's own pace inside a bounded time slice rather than
// painting between each one. Registration is by SOURCE and idempotent, so a
// subsystem sets the flag from its own state and cannot leak a count:
//
//   dispatcher.setBurstSource(this, !m_queue.empty());   // wherever that changes
//
// The slice is bounded by the caller, so the UI keeps painting and stays
// responsive throughout the transfer.
// ============================================================================
class JMainThreadDispatcher {
public:
    static JMainThreadDispatcher& instance() {
        static JMainThreadDispatcher inst;
        return inst;
    }

    // Thread-safe. Queues callback for execution on the main thread.
    void post(std::function<void()> cb) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending.push_back(std::move(cb));
        }
        m_posted.notify_all();   // wake a drainFor() that is waiting on this very callback
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

    // Drain, and if there is nothing yet, WAIT up to maxMs for the first callback and
    // drain that. Returns the number drained — 0 means the wait expired with nothing
    // posted, which is the caller's signal that the burst has gone quiet.
    //
    // The wait is a condition variable rather than a sleep, so a reply that lands after
    // 300 us is serviced after 300 us instead of at the end of a fixed nap.
    int drainFor(int maxMs) {
        std::vector<std::function<void()>> batch;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_pending.empty() && maxMs > 0) {
                m_posted.wait_for(lock, std::chrono::milliseconds(maxMs),
                                  [this] { return !m_pending.empty(); });
            }
            batch.swap(m_pending);
        }
        for (auto& cb : batch) cb();
        return static_cast<int>(batch.size());
    }

    bool hasPending() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_pending.empty();
    }

    // Declare (or withdraw) `source` as mid-transfer. Idempotent in both directions:
    // a subsystem calls it from its own state rather than pairing begin/end calls, so
    // there is no count to leak and no way to leave the loop permanently busy.
    void setBurstSource(const void* source, bool active) {
        if (!source) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (active) m_bursting.insert(source);
        else        m_bursting.erase(source);
    }

    // Is any subsystem mid-transfer?
    bool bursting() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_bursting.empty();
    }

private:
    JMainThreadDispatcher() = default;
    mutable std::mutex                 m_mutex;
    std::condition_variable            m_posted;
    std::vector<std::function<void()>> m_pending;
    std::set<const void*>              m_bursting;   // subsystems currently mid-transfer
};

} // inline namespace jf
