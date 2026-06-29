#pragma once

#include <cmath>
#include <vector>
#include <cstdint>
#include <algorithm>

namespace Genesis::Core {

enum class JEasing {
    Linear,
    EaseIn,
    EaseOut,
    EaseInOut,
    EaseInCubic,
    EaseOutCubic,
    EaseInOutCubic,
    EaseOutElastic,
    EaseInBounce,
    EaseOutBounce
};

inline float applyEasing(float t, JEasing e) {
    t = std::clamp(t, 0.0f, 1.0f);

    switch (e) {
        case JEasing::Linear:
            return t;

        case JEasing::EaseIn:
            return t * t;

        case JEasing::EaseOut:
            return t * (2.0f - t);

        case JEasing::EaseInOut:
            return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;

        case JEasing::EaseInCubic:
            return t * t * t;

        case JEasing::EaseOutCubic: {
            float s = 1.0f - t;
            return 1.0f - s * s * s;
        }

        case JEasing::EaseInOutCubic:
            return t < 0.5f
                ? 4.0f * t * t * t
                : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;

        case JEasing::EaseOutElastic: {
            if (t == 0.0f) return 0.0f;
            if (t == 1.0f) return 1.0f;
            constexpr float pi = 3.14159265358979323846f;
            return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * (2.0f * pi / 3.0f)) + 1.0f;
        }

        case JEasing::EaseInBounce:
            return 1.0f - applyEasing(1.0f - t, JEasing::EaseOutBounce);

        case JEasing::EaseOutBounce: {
            constexpr float n1 = 7.5625f;
            constexpr float d1 = 2.75f;
            if (t < 1.0f / d1) {
                return n1 * t * t;
            } else if (t < 2.0f / d1) {
                t -= 1.5f / d1;
                return n1 * t * t + 0.75f;
            } else if (t < 2.5f / d1) {
                t -= 2.25f / d1;
                return n1 * t * t + 0.9375f;
            } else {
                t -= 2.625f / d1;
                return n1 * t * t + 0.984375f;
            }
        }

        default:
            return t;
    }
}

class JAnimatedFloat {
public:
    JAnimatedFloat() = default;

    JAnimatedFloat(float initial)
        : m_current(initial), m_from(initial), m_to(initial) {}

    void animateTo(float target, float durationMs, JEasing e = JEasing::EaseOut) {
        if (durationMs <= 0.0f) {
            m_current = target;
            m_from    = target;
            m_to      = target;
            m_elapsed  = 0.0f;
            m_duration = 0.0f;
            m_done     = true;
            return;
        }
        m_from     = m_current;
        m_to       = target;
        m_duration = durationMs / 1000.0f;
        m_elapsed  = 0.0f;
        m_easing   = e;
        m_done     = false;
    }

    void set(float v) {
        m_current  = v;
        m_from     = v;
        m_to       = v;
        m_elapsed  = 0.0f;
        m_duration = 0.0f;
        m_done     = true;
    }

    bool advance(float dt) {
        if (m_done) return false;

        m_elapsed += dt;
        float t = (m_duration > 0.0f) ? (m_elapsed / m_duration) : 1.0f;
        if (t >= 1.0f) {
            t       = 1.0f;
            m_done  = true;
        }

        float prev    = m_current;
        m_current     = m_from + (m_to - m_from) * applyEasing(t, m_easing);
        return m_current != prev;
    }

    float current() const { return m_current; }
    float target()  const { return m_to; }
    bool  isDone()  const { return m_done; }

private:
    float  m_from{0.0f};
    float  m_to{0.0f};
    float  m_current{0.0f};
    float  m_elapsed{0.0f};
    float  m_duration{0.0f};
    bool   m_done{true};
    JEasing m_easing{JEasing::EaseOut};
};

struct JAnimatedColor {
    JAnimatedFloat r, g, b, a;

    JAnimatedColor() = default;

    JAnimatedColor(uint8_t R, uint8_t G, uint8_t B, uint8_t A)
        : r(R / 255.0f), g(G / 255.0f), b(B / 255.0f), a(A / 255.0f) {}

    void animateTo(const uint8_t col[4], float durationMs, JEasing e = JEasing::EaseOut) {
        r.animateTo(col[0] / 255.0f, durationMs, e);
        g.animateTo(col[1] / 255.0f, durationMs, e);
        b.animateTo(col[2] / 255.0f, durationMs, e);
        a.animateTo(col[3] / 255.0f, durationMs, e);
    }

    bool advance(float dt) {
        bool changed = false;
        changed |= r.advance(dt);
        changed |= g.advance(dt);
        changed |= b.advance(dt);
        changed |= a.advance(dt);
        return changed;
    }

    void fill(uint8_t out[4]) const {
        auto toU8 = [](float v) -> uint8_t {
            return static_cast<uint8_t>(std::clamp(static_cast<int>(v * 255.0f + 0.5f), 0, 255));
        };
        out[0] = toU8(r.current());
        out[1] = toU8(g.current());
        out[2] = toU8(b.current());
        out[3] = toU8(a.current());
    }

    bool isDone() const {
        return r.isDone() && g.isDone() && b.isDone() && a.isDone();
    }
};

class JAnimator {
public:
    size_t add(float initial = 0.0f) {
        m_values.emplace_back(initial);
        return m_values.size() - 1;
    }

    void animateTo(size_t idx, float target, float durationMs, JEasing e = JEasing::EaseOut) {
        if (idx < m_values.size()) {
            m_values[idx].animateTo(target, durationMs, e);
        }
    }

    void set(size_t idx, float v) {
        if (idx < m_values.size()) {
            m_values[idx].set(v);
        }
    }

    float value(size_t idx) const {
        if (idx >= m_values.size()) return 0.0f;
        return m_values[idx].current();
    }

    bool advance(float dt) {
        bool anyActive = false;
        for (auto& af : m_values) {
            af.advance(dt);
            if (!af.isDone()) anyActive = true;
        }
        return anyActive;
    }

    bool isDone() const {
        for (const auto& af : m_values) {
            if (!af.isDone()) return false;
        }
        return true;
    }

private:
    std::vector<JAnimatedFloat> m_values;
};

} // namespace Genesis::Core
