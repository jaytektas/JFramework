#pragma once

// SpinRepeat — press-and-hold auto-repeat with acceleration for spin-box up/down buttons. Shared by JSpinBox
// and JDoubleSpinBox (which don't share a base). The owner holds one as a member, wires it in its constructor
//
//     m_repeat.onStep = [this](int units) { setValue(m_value + units); };   // apply an accelerated step
//     m_repeat.timer.onTick.connect([this] { m_repeat.tick(); });
//
// calls begin(+1 | -1) when a button goes down and end() when the mouse is released. One repeating JTimer runs
// at a fixed interval while held; the STEP grows the longer the button is down (acceleration) — so the timer
// thread is started once per press, not re-armed per tick. Everything (initial delay, repeat interval, when
// acceleration begins, how fast the step grows, the cap) is configurable.

#include "Timer.h"

#include <chrono>
#include <functional>

inline namespace jf {

struct SpinRepeat {
    JTimer timer;
    std::function<void(int units)> onStep;   // owner applies a signed, already-accelerated step

    // Tunables (all in ms except the step counts).
    int delayMs      = 400;   // hold this long before auto-repeat starts
    int intervalMs   = 55;    // repeat period once it starts
    int accelAfterMs = 900;   // steps stay 1 until held this long, then grow
    int accelEveryMs = 250;   // after accelAfterMs, the step grows by 1 every this-many ms
    int maxStep      = 25;    // cap on the accelerated step

    void configure(int delay, int interval, int accelAfter, int accelEvery, int maxSt) {
        delayMs = delay; intervalMs = interval; accelAfterMs = accelAfter; accelEveryMs = accelEvery; maxStep = maxSt;
    }

    void begin(int direction) {   // +1 (up) or -1 (down); the owner has already applied the first click's step
        m_dir   = direction;
        m_start = std::chrono::steady_clock::now();
        timer.start(std::chrono::milliseconds(intervalMs), JTimer::JMode::Repeating);
    }
    void end() { m_dir = 0; timer.stop(); }
    bool active() const { return m_dir != 0; }

    void tick() {   // wired to timer.onTick by the owner; runs on the UI thread
        if (m_dir == 0 || !onStep) return;
        const long held = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - m_start).count();
        if (held < delayMs) return;   // still inside the initial hold — no repeat yet
        const int step = held < accelAfterMs ? 1
                       : std::min(maxStep, 1 + static_cast<int>((held - accelAfterMs) / std::max(1, accelEveryMs)));
        onStep(m_dir * step);
    }

private:
    int m_dir = 0;
    std::chrono::steady_clock::time_point m_start{};
};

}  // inline namespace jf
