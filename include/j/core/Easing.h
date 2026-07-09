#pragma once
//
// Easing.h — clean-room easing functions for the JFramework animation system.
//
// A single free function `float ease(JEasing, float t)` maps a normalized time
// t in [0,1] to an eased progress value. All curves satisfy the invariants
// ease(*, 0) == 0 and ease(*, 1) == 1 (endpoints pinned) so a tween always
// starts exactly at `from` and lands exactly on `to`. t is clamped to [0,1]
// on entry; overshooting curves (OutBack / OutElastic) may return values
// slightly outside [0,1] in the interior, which is intentional.
//
// Clean-room: shapes are the standard Penner/Robert-Penner easing formulas
// (public-domain maths). No third-party source was copied.
//
#include <cmath>
#include <algorithm>

inline namespace jf {

enum class JEasing {
    Linear,
    InQuad,   OutQuad,   InOutQuad,
    InCubic,  OutCubic,  InOutCubic,
    InOutSine,
    OutBack,
    OutElastic,
    OutBounce,
};

namespace detail {
inline constexpr float kPi = 3.14159265358979323846f;

// OutBounce is the shared kernel for the bounce family.
inline float outBounce(float t) {
    constexpr float n1 = 7.5625f;
    constexpr float d1 = 2.75f;
    if (t < 1.0f / d1)        { return n1 * t * t; }
    else if (t < 2.0f / d1)   { t -= 1.5f  / d1; return n1 * t * t + 0.75f; }
    else if (t < 2.5f / d1)   { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
    else                      { t -= 2.625f/ d1; return n1 * t * t + 0.984375f; }
}
} // namespace detail

inline float ease(JEasing e, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    using detail::kPi;

    switch (e) {
        case JEasing::Linear:
            return t;

        case JEasing::InQuad:
            return t * t;

        case JEasing::OutQuad:
            return t * (2.0f - t);

        case JEasing::InOutQuad:
            return t < 0.5f ? 2.0f * t * t
                            : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) / 2.0f;

        case JEasing::InCubic:
            return t * t * t;

        case JEasing::OutCubic: {
            float s = 1.0f - t;
            return 1.0f - s * s * s;
        }

        case JEasing::InOutCubic:
            return t < 0.5f ? 4.0f * t * t * t
                            : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;

        case JEasing::InOutSine:
            return -(std::cos(kPi * t) - 1.0f) / 2.0f;

        case JEasing::OutBack: {
            constexpr float c1 = 1.70158f;
            constexpr float c3 = c1 + 1.0f;
            float s = t - 1.0f;
            return 1.0f + c3 * s * s * s + c1 * s * s;
        }

        case JEasing::OutElastic: {
            if (t == 0.0f) return 0.0f;
            if (t == 1.0f) return 1.0f;
            constexpr float c4 = (2.0f * kPi) / 3.0f;
            return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
        }

        case JEasing::OutBounce:
            return detail::outBounce(t);
    }
    return t; // unreachable; keeps the compiler happy for future enum growth
}

} // inline namespace jf
