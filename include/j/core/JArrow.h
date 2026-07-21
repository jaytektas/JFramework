#pragma once

// JArrow — the toolkit's triangular direction mark.
//
// One place draws a solid triangle: tree disclosure indicators, data-grid sort direction, and anything
// else that needs to point. Before this, JTreeView drew its own and JDataGrid was about to draw a second,
// differently — which is exactly the fork the composition rule exists to prevent. Extend this if a caller
// needs it to flex; do not re-inline a triangle.
//
// Geometry is theme-driven: the size defaults to JStyle::arrowSize (the half-extent, so a mark spans
// 2*size across its base). Callers pass a centre point, not a rect, because every existing call site
// already knows the centre of the cell it is marking.

#include "JStyle.h"
#include "../graphics/VectorGraphics.h"

inline namespace jf {

class JArrow {
public:
    enum class Direction : uint8_t { Up, Down, Left, Right };

    // Draw a filled triangle centred on (cx, cy). `size` <= 0 takes the scheme's arrowSize.
    static void draw(JPrimitiveBuffer& buf, float cx, float cy, Direction dir,
                     const uint8_t color[4], float size = 0.f)
    {
        const float s = (size > 0.f) ? size : JStyle::current().arrowSize;
        if (s <= 0.f) return;
        const JColor col = jf::rgba(color[0], color[1], color[2], color[3]);
        // The base sits at 0.55*s from centre and the tip at 0.85*s, so the mark reads as a triangle
        // pointing somewhere rather than an equilateral blob with no direction.
        const float back = s * 0.55f, tip = s * 0.85f;
        JVectorCanvas vg;
        vg.setAntiAlias(1.2f);
        switch (dir) {
            case Direction::Down:
                vg.fillConvex({ {cx - s, cy - back}, {cx + s, cy - back}, {cx, cy + tip} }, JPaint::solid(col));
                break;
            case Direction::Up:
                vg.fillConvex({ {cx - s, cy + back}, {cx + s, cy + back}, {cx, cy - tip} }, JPaint::solid(col));
                break;
            case Direction::Right:
                vg.fillConvex({ {cx - back, cy - s}, {cx + tip, cy}, {cx - back, cy + s} }, JPaint::solid(col));
                break;
            case Direction::Left:
                vg.fillConvex({ {cx + back, cy - s}, {cx - tip, cy}, {cx + back, cy + s} }, JPaint::solid(col));
                break;
        }
        vg.flush(buf);
    }
};

} // inline namespace jf
