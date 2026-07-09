#pragma once
//
// FrameTimer.h — clean-room, thread-free timer / event-scheduling for JFramework.
//
//   JFrameTimer          — a countdown timer: setInterval(ms), setSingleShot(bool),
//                          start()/start(ms)/stop(), isActive(), remaining(), and a
//                          timeout() signal. Repeating by default; a single-shot
//                          timer auto-stops after one fire. Static convenience
//                          JFrameTimer::singleShot(ms, cb) fires once, self-owned.
//   JFrameTimerRegistry  — framework-owned registry of active timers; tick(dtMs)
//                          decrements each and fires timeouts. A repeating timer
//                          re-arms carrying any overshoot so it never drifts. Also
//                          services jPostToNextFrame() deferred callbacks.
//   jTimers()            — process-wide registry, ticked once per frame by the
//                          runner (JAppWindow), right beside jAnimator().tick().
//   jPostToNextFrame(fn) — run a callback exactly once on the next frame tick.
//
// This mirrors JAnimator (Animation.h): a global registry advanced by the real
// wall-clock frame delta the runner already computes, so timeouts fire on the UI
// thread in lockstep with rendering — no worker threads, no marshalling.
//
// Clean-room: the timer/registry shapes are original. QTimer / QElapsedTimer /
// QObject::startTimer were a conceptual reference only; no third-party source was
// copied.
//
// NOTE ON NAMING: jf::JTimer is already taken by a pre-existing thread-backed
// timer (Timer.h, used by StatusBar). This frame-ticked design intentionally lives
// under JFrameTimer to coexist without an ODR clash in inline namespace jf.
//
#include <j/core/Signal.h>

#include <memory>
#include <vector>
#include <functional>
#include <algorithm>
#include <unordered_set>
#include <cstddef>

inline namespace jf {

class JFrameTimerRegistry;
JFrameTimerRegistry& jTimers();

// ---------------------------------------------------------------------------
// JFrameTimer — one countdown, driven a frame at a time by the registry.
// ---------------------------------------------------------------------------
class JFrameTimer {
public:
    JFrameTimer() = default;
    explicit JFrameTimer(float intervalMs, bool singleShot = false)
        : m_intervalMs(intervalMs < 0.0f ? 0.0f : intervalMs)
        , m_singleShot(singleShot) {}

    // Unregister on destroy so the registry never ticks a dangling timer.
    ~JFrameTimer() { stop(); }

    JFrameTimer(const JFrameTimer&)            = delete;
    JFrameTimer& operator=(const JFrameTimer&) = delete;

    // Signal (jf::JSignal). Fired on the UI thread from the frame tick.
    JSignal<> timeout;

    // --- configuration (fluent) -------------------------------------------
    JFrameTimer& setInterval(float ms)  { m_intervalMs = ms < 0.0f ? 0.0f : ms; return *this; }
    JFrameTimer& setSingleShot(bool s)  { m_singleShot = s; return *this; }
    float interval()     const { return m_intervalMs; }
    bool  isSingleShot() const { return m_singleShot; }

    // Arm the timer: (re)load the full interval and register with jTimers().
    void start();
    void start(float ms) { setInterval(ms); start(); }
    void stop();

    bool  isActive()  const { return m_active; }
    // Time left before the next timeout (ms), 0 when stopped.
    float remaining() const { return m_active ? (m_remainingMs > 0.0f ? m_remainingMs : 0.0f) : 0.0f; }

    // Fire cb once after ms, no owned object needed — the registry reaps it.
    static void singleShot(float ms, std::function<void()> cb);

private:
    friend class JFrameTimerRegistry;

    // Advance one frame; fire due timeouts; return how many times it fired.
    // A repeating timer re-arms by ADDING its interval to the (negative) remainder,
    // so accumulated error stays out of the schedule — no drift across frames.
    int advance(float dtMs) {
        if (!m_active) return 0;
        if (dtMs < 0.0f) dtMs = 0.0f;
        m_remainingMs -= dtMs;

        int fires = 0;
        while (m_active && m_remainingMs <= 0.0f) {
            ++fires;
            if (m_singleShot) {
                m_active = false;           // inactive BEFORE the callback: isActive()==false inside it
                timeout.emit();
                break;                      // registry reap() drops it from the active set
            }
            m_remainingMs += m_intervalMs;  // carry the overshoot forward (drift-free)
            timeout.emit();                 // a callback may stop()/restart — the while re-reads state
            if (m_intervalMs <= 0.0f) break; // degenerate interval: fire once, don't spin
        }
        return fires;
    }

    float m_intervalMs{0.0f};
    float m_remainingMs{0.0f};
    bool  m_singleShot{false};
    bool  m_active{false};
};

// ---------------------------------------------------------------------------
// JFrameTimerRegistry — framework-owned set of active timers + deferred calls.
//
// The runner calls tick(dtMs) once per frame. tick() advances every active
// timer, fires timeouts, reaps finished ones, then runs any callback posted for
// this frame. It returns true when anything fired so the runner can arm a
// repaint (event-driven presenter).
// ---------------------------------------------------------------------------
class JFrameTimerRegistry {
public:
    // Register a borrowed timer (idempotent).
    void add(JFrameTimer* t) {
        if (!t) return;
        if (m_set.insert(t).second) m_active.push_back(t);
    }
    // Unregister a timer (safe if absent).
    void remove(JFrameTimer* t) {
        if (m_set.erase(t))
            m_active.erase(std::remove(m_active.begin(), m_active.end(), t), m_active.end());
    }
    // Take ownership of a timer (self-owned single-shots); reaped when it goes inactive.
    JFrameTimer* addOwned(std::unique_ptr<JFrameTimer> t) {
        JFrameTimer* p = t.get();
        m_owned.push_back(std::move(t));
        add(p);
        return p;
    }

    // Queue a callback to run exactly once on the NEXT tick.
    void postToNextFrame(std::function<void()> fn) {
        if (fn) m_pending.push_back(std::move(fn));
    }

    // Advance one frame. Returns true if any timer fired or a deferred call ran.
    bool tick(float dtMs) {
        bool fired = false;

        // Claim this frame's deferred callbacks up front, so anything posted DURING
        // this tick (by a timeout or by another deferred call) lands on the next frame.
        std::vector<std::function<void()>> deferred;
        deferred.swap(m_pending);

        // Snapshot the active set: a timeout callback may add, stop, or destroy timers
        // mid-tick. We re-check membership before advancing so a pointer removed (or a
        // timer destroyed) during this tick is skipped without a dereference.
        std::vector<JFrameTimer*> snapshot = m_active;
        for (JFrameTimer* t : snapshot) {
            if (m_set.find(t) == m_set.end()) continue;
            if (t->advance(dtMs) > 0) fired = true;
        }
        reap();

        for (auto& fn : deferred) if (fn) fn();
        if (!deferred.empty()) fired = true;

        return fired;
    }

    bool   hasActive()   const { return !m_active.empty() || !m_pending.empty(); }
    size_t activeCount() const { return m_active.size(); }
    void   clear() { m_active.clear(); m_set.clear(); m_owned.clear(); m_pending.clear(); }

private:
    // Drop timers that went inactive (single-shots that just fired); destroy the
    // self-owned ones. Called after the advance pass so we never mutate the active
    // vector while the snapshot loop is walking it.
    void reap() {
        for (std::size_t i = 0; i < m_active.size();) {
            JFrameTimer* t = m_active[i];
            if (t->isActive()) { ++i; continue; }
            m_set.erase(t);
            m_active.erase(m_active.begin() + i);
        }
        for (auto& up : m_owned)
            if (up && !up->isActive()) up.reset();  // ~JFrameTimer -> stop() -> remove() (already gone: no-op)
        m_owned.erase(std::remove(m_owned.begin(), m_owned.end(), nullptr), m_owned.end());
    }

    std::vector<JFrameTimer*>                  m_active;
    std::unordered_set<JFrameTimer*>           m_set;     // O(1) membership guard for mid-tick removal
    std::vector<std::unique_ptr<JFrameTimer>>  m_owned;   // self-owned single-shots
    std::vector<std::function<void()>>         m_pending; // deferred: run next tick
};

// Process-wide timer registry, ticked once per frame by the runner (JAppWindow).
// Function-local static => exactly one instance across all translation units.
inline JFrameTimerRegistry& jTimers() {
    static JFrameTimerRegistry instance;
    return instance;
}

// --- out-of-line JFrameTimer members (need the registry definition) --------
inline void JFrameTimer::start() {
    m_remainingMs = m_intervalMs;
    m_active = true;
    jTimers().add(this);
}

inline void JFrameTimer::stop() {
    m_active = false;
    jTimers().remove(this);
}

inline void JFrameTimer::singleShot(float ms, std::function<void()> cb) {
    auto t = std::make_unique<JFrameTimer>(ms, /*singleShot*/ true);
    if (cb) t->timeout.connect(std::move(cb));
    JFrameTimer* p = jTimers().addOwned(std::move(t));
    p->start();
}

// Run fn exactly once on the next frame tick (deferred/idle call).
inline void jPostToNextFrame(std::function<void()> fn) {
    jTimers().postToNextFrame(std::move(fn));
}

} // inline namespace jf
