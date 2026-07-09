#pragma once

// JCloseButton — the framework's ONE canonical window close control. A rounded square that highlights on
// hover (JStyle CloseBtn/CloseBtnHover roles) with the theme's cross mark (CloseBtnMark). Every window type
// draws its close button through THIS class — JDockWidget/JDockManager title bars, JPopupWindow, native/app
// dialogs, and app popups — so the control is defined in exactly one place (composition, not duplication).
//
// Drawing is a static method (the callers are immediate-mode window chrome that render into a buffer each
// frame, not a widget tree), plus rectFor()/hit() so a title bar computes the button's size + position and
// hit box from one source — callers never invent a magic size.

#include "JStyle.h"
#include "../graphics/RenderPrimitive.h"

inline namespace jf {

class JCloseButton {
public:
    // The standard close-button rect for a title bar occupying `bar`: a square inset from the bar's top and
    // bottom (so its size scales with the bar height) and nudged in from the right edge.
    static JRect rectFor(const JRect& bar) {
        const float sz = bar.height - 12.0f;
        return { bar.x + bar.width - sz - 6.0f, bar.y + (bar.height - sz) * 0.5f, sz, sz };
    }

    static bool hit(const JRect& r, float mx, float my) {
        return mx >= r.x && mx < r.x + r.width && my >= r.y && my < r.y + r.height;
    }

    // Draw the button filling `r`, highlighted when hovered.
    static void draw(JPrimitiveBuffer& buf, const JRect& r, bool hovered) {
        const uint8_t* s = hovered ? Colors::CloseBtnHover : Colors::CloseBtn;
        uint8_t fill[4] = {s[0], s[1], s[2], s[3]};
        buf.pushRectangle(r.x, r.y, r.width, r.height, fill, 3.0f);
        uint8_t m[4] = {Colors::CloseBtnMark[0], Colors::CloseBtnMark[1], Colors::CloseBtnMark[2], Colors::CloseBtnMark[3]};
        buf.pushRectangle(r.x + 3.0f,            r.y + r.height * 0.42f, r.width - 6.0f, 2.5f,           m, 1.0f);
        buf.pushRectangle(r.x + r.width * 0.42f, r.y + 3.0f,             2.5f,           r.height - 6.0f, m, 1.0f);
    }
};

} // inline namespace jf
