#pragma once

#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <future>
#include <cstddef>
#include "MainThreadDispatcher.h"

namespace Genesis {

// ============================================================================
// WorkerThread — persistent background thread with a serialised task queue.
//
// Fills the gap left by MainThreadDispatcher (background→main only).
// WorkerThread gives you the main→background direction: enqueue lambdas that
// run on a dedicated worker thread, with optional result callbacks that are
// automatically delivered back to the main thread via MainThreadDispatcher.
//
// Unlike the detach() pattern used in Database/Settings async methods,
// WorkerThread reuses one thread across many operations, giving you:
//   - Lower overhead (no thread-spawn cost per task)
//   - Natural FIFO ordering (tasks run in the order posted)
//   - Bounded concurrency (one background thread, not N)
//
// For parallel CPU-bound work across N threads, see ThreadPool (not yet
// implemented; WorkerThread covers the common serial-I/O use case).
//
// Quick reference:
//   WorkerThread worker;
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
class WorkerThread {
public:
    WorkerThread() {
        m_thread = std::thread([this]{ _loop(); });
    }

    // Drains all remaining tasks then joins the thread.
    ~WorkerThread() { stop(); }

    WorkerThread(const WorkerThread&)            = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;

    // ---- Posting tasks -----------------------------------------------------

    // Queue a fire-and-forget task. Returns immediately.
    void post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_stopping) return;
            m_queue.push_back(std::move(task));
        }
        m_cv.notify_one();
    }

    // Run task() on the worker thread; cb(result) fires on the main thread.
    // T must be movable. Use postWithCompletion() for void tasks.
    template<typename T>
    void postWithResult(std::function<T()> task, std::function<void(T)> cb) {
        post([task = std::move(task), cb = std::move(cb)]() mutable {
            T result = task();
            MainThreadDispatcher::instance().post(
                [cb = std::move(cb), r = std::move(result)]() mutable {
                    cb(std::move(r));
                });
        });
    }

    // Run task() on the worker thread; cb() fires on the main thread when done.
    // cb may be null if no notification is needed.
    void postWithCompletion(std::function<void()> task, std::function<void()> cb = nullptr) {
        post([task = std::move(task), cb = std::move(cb)]() mutable {
            task();
            if (cb) MainThreadDispatcher::instance().post(std::move(cb));
        });
    }

    // ---- Control -----------------------------------------------------------

    // Block the calling thread until all currently queued tasks complete.
    // Inserts a sentinel task and waits for it — straightforward and deadlock-free.
    // WARNING: do not call from the main render thread; use postWithCompletion instead.
    void waitForIdle() {
        std::promise<void> p;
        auto f = p.get_future();
        post([&p]{ p.set_value(); });
        f.wait();
    }

    // Finish all pending tasks then stop and join. Blocks the calling thread.
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

    // Number of tasks currently waiting in the queue (not counting the running one).
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
                // Wait until there's work, or we're stopping.
                m_cv.wait(lk, [this]{ return !m_queue.empty() || m_stopping; });
                if (m_queue.empty()) break; // stopping with nothing left to do
                task = std::move(m_queue.front());
                m_queue.pop_front();
            }
            // Execute outside the lock — post()/stopNow() remain responsive.
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
