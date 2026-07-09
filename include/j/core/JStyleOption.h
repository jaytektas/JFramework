#pragma once

#include "JPalette.h"      // JColorGroup (JStyleOption::group())
#include "SceneGraph.h"    // JRect
#include <cstdint>

inline namespace jf {

// The vocabulary a widget uses to talk to the styler: its live state (JStyleState),
// the packet it hands over (JStyleOption), and the metric keys it queries (JStyleHint).

// ============================================================================
// JStyleState — widget state bit flags carried by a JStyleOption.
// ============================================================================
enum JStyleState : uint32_t {
    State_None     = 0,
    State_Enabled  = 1u << 0,
    State_Hovered  = 1u << 1,
    State_Pressed  = 1u << 2,
    State_Focused  = 1u << 3,
    State_Selected = 1u << 4,
    State_On       = 1u << 5,   // checked / toggled on
    State_Off      = 1u << 6,   // explicitly off
    State_Inactive = 1u << 7,   // owning top-level is not active
};

// ============================================================================
// JStyleOption — the packet a widget hands the styler: its state + geometry.
// ============================================================================
struct JStyleOption {
    uint32_t state = State_Enabled;
    JRect    rect{};

    bool has(uint32_t flag) const { return (state & flag) != 0; }
    void set(uint32_t flag, bool on) { on ? (state |= flag) : (state &= ~flag); }

    JColorGroup group() const {
        if (!(state & State_Enabled)) return JColorGroup::Disabled;
        if (state & State_Inactive)   return JColorGroup::Inactive;
        return JColorGroup::Active;
    }
};

// ============================================================================
// JStyleHint — keyed metrics a widget queries instead of embedding magic
// numbers. Resolved by JStyle::hint() against the live theme.
// ============================================================================
enum class JStyleHint : uint16_t {
    FocusRingWidth,   // outline width of the focus ring
    ControlRadius,    // default corner radius of controls
    BorderWidth,      // default outline width
    ControlHeight,    // default interactive-field height
    ItemPadding,      // interior padding
    Spacing,          // gap between items
};

} // inline namespace jf
