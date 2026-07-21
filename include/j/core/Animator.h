#pragma once

#include <cmath>
#include <vector>
#include <cstdint>
#include <algorithm>

#include "Easing.h"   // the toolkit's ONE easing enum + curve set

// JAnimatedFloat / JAnimatedColor — self-contained tweens a widget can own directly.
//
// The slot-array JAnimator that used to live here was a SECOND class of that name: Animation.h:235
// already defines JAnimator over JAnimatable/JAnimation/JAnimationGroup, and two classes with one name
// is an ODR clash, not a convenience. Callers that just want a tween hold a JAnimatedFloat; callers
// composing timelines use Animation.h's JAnimator. There is one of each.

inline namespace jf {

class JAnimatedFloat {
public:
    JAnimatedFloat() = default;

    JAnimatedFloat(float initial)
        : m_current(initial), m_from(initial), m_to(initial) {}

    void animateTo(float target, float durationMs, JEasing e = JEasing::OutQuad) {
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
        m_current     = m_from + (m_to - m_from) * ease(m_easing, t);
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
    JEasing m_easing{JEasing::OutQuad};
};

struct JAnimatedColor {
    JAnimatedFloat r, g, b, a;

    JAnimatedColor() = default;

    JAnimatedColor(uint8_t R, uint8_t G, uint8_t B, uint8_t A)
        : r(R / 255.0f), g(G / 255.0f), b(B / 255.0f), a(A / 255.0f) {}

    void animateTo(const uint8_t col[4], float durationMs, JEasing e = JEasing::OutQuad) {
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

} // inline namespace jf
