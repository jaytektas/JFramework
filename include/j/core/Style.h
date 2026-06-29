#pragma once

// JStyle — the global stylesheet. A single runtime-mutable struct of visual defaults read by
// the whole framework EACH FRAME (not cached), so mutating a field reflows the GUI live.
//
// Cascade (CSS-style): every component keeps SPARSE local overrides and falls back to the
// global for anything unset —
//   effective = localOverride.value_or(style().field)
// So one host can be styled one way and another differently, while both inherit everything
// they didn't override; change the global and every un-overridden component updates at once.
//
// This is a leaf header (no framework deps) so anything — widgets, dock, menu, chrome — can
// read style() without include cycles.

#include <cstdint>

inline namespace jf {

// Which edge of a leaf its tab bar sits on.
enum class JTabBarEdge : uint8_t { Top, Bottom, Left, Right };

// How tabs occupy the bar:
//   Fill     — equal share of the bar (current behaviour)
//   Left     — natural width, left-justified (trailing space left empty)
//   Compress — shrink equally to fit, but never beyond their natural width
enum class JTabFill : uint8_t { Fill, Left, Compress };

struct JStyle {
    // --- Dock tabs ---
    JTabBarEdge tabEdge{JTabBarEdge::Top};
    JTabFill    tabFill{JTabFill::Fill};
    float       tabBarSize{28.0f};
    // (grows as more of the GUI migrates off hardcoded constants)
};

// The one global stylesheet.
inline JStyle& style() { static JStyle s; return s; }

} // inline namespace jf
