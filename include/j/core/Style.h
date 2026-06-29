#pragma once

// Style enums shared between the dock layer and the unified stylesheet (JTheme, in
// BaseWidgets.h). Kept in this leaf header (no framework deps) so both can use them without
// an include cycle. The stylesheet itself is JTheme; read it via style() (= JTheme::current()).

#include <cstdint>

inline namespace jf {

// Which edge of a leaf its tab bar sits on.
enum class JTabBarEdge : uint8_t { Top, Bottom, Left, Right };

// How tabs occupy the bar:
//   Fill     — equal share of the bar
//   Left     — natural width, left-justified (trailing space left empty)
//   Compress — shrink equally to fit, but never beyond their natural width
enum class JTabFill : uint8_t { Fill, Left, Compress };

} // inline namespace jf
