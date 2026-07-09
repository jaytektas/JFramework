#pragma once
//
// Animation.h — clean-room tween / transition system for JFramework.
//
//   JEasing            — easing-curve selector (Easing.h)
//   JAnimation         — one tweened float: from -> to over durationMs on a curve,
//                        with Once / Loop / PingPong modes and onValueChanged /
//                        onFinished signals.
//   JAnimatable        — abstract base advanced each frame (JAnimation & JAnimationGroup).
//   JAnimationGroup    — compose children in Parallel or Sequential mode.
//   JAnimator          — framework-owned registry of active animations; tick(dtMs)
//                        advances all and reaps finished ones. jAnimator() is the
//                        process-wide accessor, ticked once per frame by the runner.
//
// Clean-room: the tween/group/registry shapes are original; standard easing maths
// only. No third-party source was copied.
//
#include <j/core/Easing.h>
#include <j/core/Signal.h>

#include <memory>
#include <vector>
#include <utility>
#include <cmath>
#include <algorithm>

inline namespace jf {

// ---------------------------------------------------------------------------
// JAnimatable — anything the animator can drive one frame at a time.
// ---------------------------------------------------------------------------
class JAnimatable {
public:
    virtual ~JAnimatable() = default;
    virtual float advance(float dtMs) = 0;  // advance a frame delta (ms); returns current value
    virtual bool  finished() const = 0;     // true once terminal (never for Loop/PingPong)
    virtual void  reset() = 0;              // rewind to the start
    // Leftover frame-delta (ms) after this animatable finished within a frame, so a
    // Sequential group can hand the unused time to the next child. 0 unless it just
    // completed with time to spare.
    virtual float overshootMs() const { return 0.0f; }
};

// ---------------------------------------------------------------------------
// JAnimation — a single tweened value.
// ---------------------------------------------------------------------------
class JAnimation : public JAnimatable {
public:
    enum class Mode { Once, Loop, PingPong };

    JAnimation() = default;
    JAnimation(float from, float to, float durationMs,
               JEasing easing = JEasing::OutCubic, Mode mode = Mode::Once)
        : m_from(from), m_to(to)
        , m_durationMs(durationMs > 0.0f ? durationMs : 0.0f)
        , m_easing(easing), m_mode(mode) {
        m_current = from;
    }

    // Signals (jf::JSignal). Connect before the animation is reaped by the registry.
    JSignal<float> onValueChanged;   // fired with the new eased value whenever it changes
    JSignal<>      onFinished;       // fired once when a Once animation completes

    // --- configuration (fluent) -------------------------------------------
    JAnimation& setRange(float from, float to) { m_from = from; m_to = to; m_current = from; return *this; }
    JAnimation& setDuration(float ms)          { m_durationMs = ms > 0.0f ? ms : 0.0f; return *this; }
    JAnimation& setEasing(JEasing e)           { m_easing = e; return *this; }
    JAnimation& setMode(Mode m)                { m_mode = m; return *this; }

    // Advance by dtMs; updates the current value and returns the eased value.
    float advance(float dtMs) override {
        if (m_done) return m_current;
        if (dtMs < 0.0f) dtMs = 0.0f;
        m_elapsed += dtMs;

        const float prev = m_current;
        m_current = sample(m_elapsed);

        // Terminal handling by mode.
        if (m_mode == Mode::Once && m_elapsed >= m_durationMs) {
            m_current = m_to;
            m_done = true;
        }

        if (m_current != prev) onValueChanged.emit(m_current);
        if (m_done && !m_finishedFired) { m_finishedFired = true; onFinished.emit(); }
        return m_current;
    }

    bool  finished() const override { return m_done; }
    float overshootMs() const override {
        return m_done ? std::max(0.0f, m_elapsed - m_durationMs) : 0.0f;
    }
    float value()    const { return m_current; }
    float progress() const {                     // eased progress 0..1 of the current position
        if (m_durationMs <= 0.0f) return 1.0f;
        return ease(m_easing, phase(m_elapsed));
    }

    void reset() override {
        m_elapsed = 0.0f;
        m_current = m_from;
        m_done = false;
        m_finishedFired = false;
    }

private:
    // Linear phase in [0,1] for a given elapsed time, honouring the loop mode.
    float phase(float elapsed) const {
        if (m_durationMs <= 0.0f) return 1.0f;
        switch (m_mode) {
            case Mode::Once:
                return std::clamp(elapsed / m_durationMs, 0.0f, 1.0f);
            case Mode::Loop: {
                float t = std::fmod(elapsed, m_durationMs) / m_durationMs;
                return t;
            }
            case Mode::PingPong: {
                float span = std::fmod(elapsed, 2.0f * m_durationMs) / m_durationMs; // 0..2
                return span <= 1.0f ? span : 2.0f - span;
            }
        }
        return 1.0f;
    }

    // Eased value at a given elapsed time.
    float sample(float elapsed) const {
        if (m_durationMs <= 0.0f) return m_to;
        return m_from + (m_to - m_from) * ease(m_easing, phase(elapsed));
    }

    float   m_from{0.0f};
    float   m_to{0.0f};
    float   m_durationMs{0.0f};
    JEasing m_easing{JEasing::OutCubic};
    Mode    m_mode{Mode::Once};

    float   m_elapsed{0.0f};
    float   m_current{0.0f};
    bool    m_done{false};
    bool    m_finishedFired{false};
};

// ---------------------------------------------------------------------------
// JAnimationGroup — compose children in parallel or in sequence.
// ---------------------------------------------------------------------------
class JAnimationGroup : public JAnimatable {
public:
    enum class Mode { Parallel, Sequential };

    explicit JAnimationGroup(Mode mode = Mode::Parallel) : m_mode(mode) {}

    JSignal<> onFinished;   // fired once when the whole group completes

    // Take ownership of a child (JAnimation or nested JAnimationGroup).
    JAnimatable& add(std::unique_ptr<JAnimatable> child) {
        JAnimatable& ref = *child;
        m_children.push_back(std::move(child));
        return ref;
    }
    // Convenience: build+own a JAnimation in place.
    JAnimation& add(float from, float to, float durationMs,
                    JEasing easing = JEasing::OutCubic,
                    JAnimation::Mode mode = JAnimation::Mode::Once) {
        auto a = std::make_unique<JAnimation>(from, to, durationMs, easing, mode);
        JAnimation& ref = *a;
        m_children.push_back(std::move(a));
        return ref;
    }

    // Returns the value of the last child advanced this frame (0 if empty/done).
    float advance(float dtMs) override {
        if (m_done || m_children.empty()) { checkFinish(); return 0.0f; }

        float last = 0.0f;
        m_overshoot = 0.0f;
        if (m_mode == Mode::Parallel) {
            for (auto& c : m_children) last = c->advance(dtMs);
        } else {
            // Sequential: spend dt on the active child; only its *leftover* (overshoot)
            // carries to the next, so total elapsed across children stays exact.
            float remaining = dtMs;
            while (remaining > 0.0f && m_index < m_children.size()) {
                JAnimatable* c = m_children[m_index].get();
                last = c->advance(remaining);
                if (!c->finished()) { remaining = 0.0f; break; }
                remaining = c->overshootMs();   // unused time flows to the next child
                m_overshoot = remaining;        // (and out of the group, if it's the last)
                ++m_index;
            }
        }
        checkFinish();
        return last;
    }

    bool  finished() const override { return m_done; }
    float overshootMs() const override { return m_done ? m_overshoot : 0.0f; }

    void reset() override {
        for (auto& c : m_children) c->reset();
        m_index = 0;
        m_overshoot = 0.0f;
        m_done = false;
        m_finishedFired = false;
    }

    size_t size() const { return m_children.size(); }

private:
    void checkFinish() {
        if (m_done) return;
        bool all = true;
        for (auto& c : m_children) if (!c->finished()) { all = false; break; }
        if (all && !m_children.empty()) {
            m_done = true;
            if (!m_finishedFired) { m_finishedFired = true; onFinished.emit(); }
        }
    }

    Mode m_mode;
    std::vector<std::unique_ptr<JAnimatable>> m_children;
    size_t m_index{0};          // active child (Sequential)
    float  m_overshoot{0.0f};   // leftover dt after the last child finished this frame
    bool   m_done{false};
    bool   m_finishedFired{false};
};

// ---------------------------------------------------------------------------
// JAnimator — framework-owned registry of active animations.
//
// The runner calls tick(dtMs) once per frame; it advances every registered
// animation and reaps the finished ones. hasActive() lets the runner request
// a repaint only while something is animating.
// ---------------------------------------------------------------------------
class JAnimator {
public:
    // Own a pre-built animatable; returns a borrowed pointer valid until it finishes.
    JAnimatable* add(std::unique_ptr<JAnimatable> a) {
        JAnimatable* p = a.get();
        m_active.push_back(std::move(a));
        return p;
    }

    // Convenience: build+own a JAnimation and start it.
    JAnimation& animate(float from, float to, float durationMs,
                        JEasing easing = JEasing::OutCubic,
                        JAnimation::Mode mode = JAnimation::Mode::Once) {
        auto a = std::make_unique<JAnimation>(from, to, durationMs, easing, mode);
        JAnimation& ref = *a;
        m_active.push_back(std::move(a));
        return ref;
    }

    // Advance every animation one frame, then reap the finished ones.
    void tick(float dtMs) {
        for (auto& a : m_active) a->advance(dtMs);
        m_active.erase(
            std::remove_if(m_active.begin(), m_active.end(),
                           [](const std::unique_ptr<JAnimatable>& a) { return a->finished(); }),
            m_active.end());
    }

    bool   hasActive() const { return !m_active.empty(); }
    size_t activeCount() const { return m_active.size(); }
    void   clear() { m_active.clear(); }

private:
    std::vector<std::unique_ptr<JAnimatable>> m_active;
};

// Process-wide animator, ticked once per frame by the runner (JAppWindow).
// Function-local static => exactly one instance across all translation units.
inline JAnimator& jAnimator() {
    static JAnimator instance;
    return instance;
}

} // inline namespace jf
