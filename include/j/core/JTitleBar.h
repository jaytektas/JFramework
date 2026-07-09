#pragma once

// JTitleBar — the framework's ONE canonical window/dialog title strip. A header fill with rounded top
// corners (JStyle TitleBar role) plus the title caption (TitleBarText role). Every window type draws its
// title bar through THIS class — JAppWindow, JDockWidget/JDockManager, native/app dialogs, and app popups —
// so the title bar is defined in exactly one place (composition, not a fork per window).
//
// Drawing is a static method: the callers are immediate-mode window chrome rendering into a buffer each
// frame. Callers overlay controls (JCloseButton) and own the drag interaction. align: 0 = left (leftPad),
// 1 = centred. rightReserve leaves clearance for right-edge controls (the close button, etc.).

#include "JStyle.h"
#include "JTextHelper.h"
#include "../graphics/RenderPrimitive.h"

inline namespace jf {

class JTitleBar {
public:
    static void draw(JPrimitiveBuffer& buf, float x, float y, float w, float h,
                     const std::string& title, float cornerRadius = 8.0f,
                     int align = 0, float leftPad = 10.0f, float rightReserve = 0.0f) {
        uint8_t bg[4] = {Colors::TitleBar[0], Colors::TitleBar[1], Colors::TitleBar[2], 255};
        buf.pushRectangle(x, y, w, h, bg, cornerRadius);                                             // rounded top
        if (cornerRadius > 0.0f) buf.pushRectangle(x, y + cornerRadius, w, h - cornerRadius, bg, 0.0f);  // flatten bottom
        if (JTextHelper::hasAtlas() && !title.empty()) {
            uint8_t tc[4] = {Colors::TitleBarText[0], Colors::TitleBarText[1], Colors::TitleBarText[2], Colors::TitleBarText[3]};
            const float lh = JTextHelper::lineHeight(), ty = y + (h - lh) * 0.5f;
            const float maxW = std::max(0.0f, w - leftPad - rightReserve - 6.0f);
            float tx = x + leftPad;
            if (align == 1) tx = x + std::max(leftPad, (w - JTextHelper::measureWidth(title)) * 0.5f);
            JTextHelper::pushText(buf, tx, ty, title, tc, maxW);
        }
    }
};

} // inline namespace jf
