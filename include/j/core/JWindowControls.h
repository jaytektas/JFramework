#pragma once

// Window title/close primitives + the drag driver + JWidget's text-dependent out-of-line
// methods (renderTooltips/drawFocusRing). Kept together — they need JWidget AND JTextHelper.

#include "JWidget.h"
#include "JTextHelper.h"
#include "DragDrop.h"
#include "JTheme.h"
#include "../graphics/RenderPrimitive.h"

inline namespace jf {


inline void JWidget::renderTooltips(JPrimitiveBuffer& buf, float mouseX, float mouseY) {
    static JWidget* lastHovered = nullptr;
    static auto hoverStart = std::chrono::steady_clock::now();

    JWidget* hovered = nullptr;
    for (auto it = s_activeWidgets.rbegin(); it != s_activeWidgets.rend(); ++it) {
        JWidget* w = *it;
        if (w && w->isVisible() && !w->tooltip().empty() && w->hitTest(mouseX, mouseY)) {
            hovered = w;
            break;
        }
    }

    if (hovered != lastHovered) {
        lastHovered = hovered;
        hoverStart = std::chrono::steady_clock::now();
    }

    if (hovered) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - hoverStart).count();
        if (elapsed < 500) {
            return; // 500ms delay
        }

        float padX = 8.0f;
        float padY = 6.0f;
        std::string text = hovered->tooltip();
        float textW = JTextHelper::measureWidth(text);
        float textH = JTextHelper::lineHeight();
        float tooltipW = textW + padX * 2.0f;
        float tooltipH = textH + padY * 2.0f;
        float x = mouseX + 12.0f;
        float y = mouseY + 12.0f;

        uint8_t shadow[4] = {0, 0, 0, 80};
        buf.pushRectangle(x + 2.f, y + 2.f, tooltipW, tooltipH, shadow, 4.0f);

        uint8_t fill[4] = {30, 30, 34, 250};
        uint8_t border[4] = {80, 80, 85, 255};
        buf.pushRectangle(x, y, tooltipW, tooltipH, fill, 4.0f, 1.0f, border);

        uint8_t textColor[4] = {240, 240, 245, 255};
        JTextHelper::pushText(buf, x + padX, y + padY, text, textColor);
    }
}

inline void JWidget::drawFocusRing(JPrimitiveBuffer& buf) const {
    if (m_state != JWidgetState::Focused) return;
    const auto& bb  = m_graph.getLayoutConst(m_nodeId).boundingBox;
    const auto& th  = JTheme::current();
    float p = th.focusRingWidth * 0.5f + 1.f;
    uint8_t ring[4] = {th.Accent[0], th.Accent[1], th.Accent[2], 210};
    uint8_t none[4] = {0, 0, 0, 0};
    buf.pushRectangle(bb.x - p, bb.y - p, bb.width + p * 2.f, bb.height + p * 2.f,
                      none, th.cornerRadius + 1.f, th.focusRingWidth, ring);
}

// ---- Window close button — the framework's ONE canonical close control ------
// A rounded square in `sz`×`sz` at (x,y) that highlights on hover (Colors::CloseBtn / CloseBtnHover)
// with the theme's cross mark (Colors::CloseBtnMark). Every window type shares it — JDockWidget title
// bars, JPopupWindow, native/app dialogs, and app popups — so the close button is styled in exactly
// one place. Pair with jCloseButtonHit() for the matching hit box.
inline void jDrawCloseButton(JPrimitiveBuffer& buf, float x, float y, float sz, bool hovered) {
    const uint8_t* s = hovered ? Colors::CloseBtnHover : Colors::CloseBtn;
    uint8_t fill[4] = {s[0], s[1], s[2], s[3]};
    buf.pushRectangle(x, y, sz, sz, fill, 3.0f);
    uint8_t m[4] = {Colors::CloseBtnMark[0], Colors::CloseBtnMark[1], Colors::CloseBtnMark[2], Colors::CloseBtnMark[3]};
    buf.pushRectangle(x + 3.0f,       y + sz * 0.42f, sz - 6.0f, 2.5f,      m, 1.0f);   // horizontal bar
    buf.pushRectangle(x + sz * 0.42f, y + 3.0f,       2.5f,      sz - 6.0f, m, 1.0f);   // vertical bar
}
inline bool jCloseButtonHit(float x, float y, float sz, float mx, float my) {
    return mx >= x && mx < x + sz && my >= y && my < y + sz;
}

// The standard close-button rect for a title bar occupying (x,y,w,h): a square inset from the bar's top
// and bottom (so its size scales with the bar height) and nudged in from the right edge. ONE place decides
// the close button's size + position — callers pass the bar, never a magic number.
inline JRect jTitleCloseRect(float x, float y, float w, float h) {
    const float sz = h - 12.0f;
    return { x + w - sz - 6.0f, y + (h - sz) * 0.5f, sz, sz };
}

// ---- Window title bar — the framework's ONE canonical title strip -----------
// A header fill across (x,y,w,h) with rounded TOP corners (Colors::Surface2, the same fill JDockWidget
// uses) plus the title text (Colors::TextPrimary). Every window/dialog/popup shares it so the title bar
// is styled in one place. Callers overlay controls (jDrawCloseButton) and own the drag interaction.
// align: 0 = left (leftPad), 1 = centred. rightReserve leaves clearance for right-edge controls.
inline void jDrawTitleBar(JPrimitiveBuffer& buf, float x, float y, float w, float h,
                          const std::string& title, float cornerRadius = 8.0f,
                          int align = 0, float leftPad = 10.0f, float rightReserve = 0.0f) {
    uint8_t bg[4] = {Colors::TitleBar[0], Colors::TitleBar[1], Colors::TitleBar[2], 255};
    buf.pushRectangle(x, y, w, h, bg, cornerRadius);                                  // rounded top
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

// ---- Drag & drop driver (declared in DragDrop.h; defined here where JWidget is
// complete and its hooks + s_activeWidgets are visible). Routes the active drag
// session to the top-most visible widget that canDrop() the payload. ----------
inline bool jDragTick(float mx, float my, bool pressed, bool released) {
    JDragSession& s = jCurrentDrag();
    if (!s.active) return false;
    s.x = mx; s.y = my;
    (void)pressed;   // cursor already tracked; kept for host-runner symmetry

    // Top-most accepting target under the cursor. Paint order in s_activeWidgets
    // is back-to-front, so scan in reverse for the front-most hit. The drag
    // source itself is skipped (a widget cannot drop onto its own drag).
    JWidget* target = nullptr;
    for (auto it = JWidget::s_activeWidgets.rbegin();
         it != JWidget::s_activeWidgets.rend(); ++it) {
        JWidget* w = *it;
        if (!w || w == s.source || !w->isVisible()) continue;
        if (w->hitTest(mx, my) && w->canDrop(s.mime)) { target = w; break; }
    }

    // Enter/leave transitions.
    if (target != s.over) {
        if (s.over) s.over->onDragLeave(s);
        s.over = target;
        if (target) { s.proposed = s.supported; target->onDragEnter(s); }
        else          s.proposed = JDropAction::Ignore;
    } else if (target) {
        target->onDragMove(s);
    }

    if (released) {
        bool dropped = false;
        if (target && target->onDrop(s)) dropped = true;
        if (!dropped) s.proposed = JDropAction::Ignore;
        s.active = false;
        s.over   = nullptr;
        return dropped;
    }
    return false;
}

} // inline namespace jf
