#pragma once

#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <future>
#include <cstddef>
#include <genesis/core/MainThreadDispatcher.h>

namespace Genesis {

// ============================================================================
// JWorkerThread — persistent background thread with a serialised task queue.
//
// Fills the gap left by JMainThreadDispatcher (background→main only).
// JWorkerThread gives you the main→background direction: enqueue lambdas that
// run on a dedicated worker thread, with optional result callbacks that are
// automatically delivered back to the main thread via JMainThreadDispatcher.
//
// Unlike the detach() pattern used in JDatabase/JSettings async methods,
// JWorkerThread reuses one thread across many operations, giving you:
//   - Lower overhead (no thread-spawn cost per task)
//   - Natural FIFO ordering (tasks run in the order posted)
//   - Bounded concurrency (one background thread, not N)
//
// Quick reference:
//   JWorkerThread worker;
//
//   // Fire and forget
//   worker.post([]{ expensiveIO(); });
//
//   // Typed result delivered to the main thread
//   worker.postWithResult<std::vector<Run>>(
//       []{ return db.loadRuns(); },
//       [this](std::vector<Run> runs){ listView->setItems(runs); });
//
//   // Void task with main-thread completion notification
//   worker.postWithCompletion(
//       [&]{ db.save(); },
//       []{ showToast("Saved"); });
//
//   // Block the calling thread until the queue is drained
//   worker.waitForIdle();   // do NOT call from the main render loop
//
//   // Destructor calls stop() — drains remaining tasks before joining.
//   // Call stopNow() to discard pending work and exit immediately.
// ============================================================================
class JWorkerThread {
public:
    JWorkerThread() {
        m_thread = std::thread([this]{ _loop(); });
    }

    ~JWorkerThread() { stop(); }

    JWorkerThread(const JWorkerThread&)            = delete;
    JWorkerThread& operator=(const JWorkerThread&) = delete;

    // ---- Posting tasks -----------------------------------------------------

    void post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_stopping) return;
            m_queue.push_back(std::move(task));
        }
        m_cv.notify_one();
    }

    // Run task() on the worker thread; cb(result) fires on the main thread.
    template<typename T>
    void postWithResult(std::function<T()> task, std::function<void(T)> cb) {
        post([task = std::move(task), cb = std::move(cb)]() mutable {
            T result = task();
            JMainThreadDispatcher::instance().post(
                [cb = std::move(cb), r = std::move(result)]() mutable {
                    cb(std::move(r));
                });
        });
    }

    // Run task() on the worker thread; cb() fires on the main thread when done.
    void postWithCompletion(std::function<void()> task, std::function<void()> cb = nullptr) {
        post([task = std::move(task), cb = std::move(cb)]() mutable {
            task();
            if (cb) JMainThreadDispatcher::instance().post(std::move(cb));
        });
    }

    // ---- JControl -----------------------------------------------------------

    // Block until all currently queued tasks complete.
    // WARNING: do not call from the main render thread; use postWithCompletion instead.
    void waitForIdle() {
        std::promise<void> p;
        auto f = p.get_future();
        post([&p]{ p.set_value(); });
        f.wait();
    }

    // Drain all pending tasks then stop and join.
    void stop() {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_stopping) return;
            m_stopping = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    // Discard all pending tasks and stop immediately.
    void stopNow() {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_stopping = true;
            m_queue.clear();
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    size_t queueSize() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_queue.size();
    }

    bool isRunning() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return !m_stopping;
    }

private:
    void _loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(m_mutex);
                m_cv.wait(lk, [this]{ return !m_queue.empty() || m_stopping; });
                if (m_queue.empty()) break;
                task = std::move(m_queue.front());
                m_queue.pop_front();
            }
            task();
        }
    }

    mutable std::mutex                m_mutex;
    std::condition_variable           m_cv;
    bool                              m_stopping{false};
    std::deque<std::function<void()>> m_queue;
    std::thread                       m_thread;
};

} // namespace Genesis
