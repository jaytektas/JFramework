#pragma once

// Window title/close primitives + the drag driver + JWidget's text-dependent out-of-line
// methods (renderTooltips/drawFocusRing). Kept together — they need JWidget AND JTextHelper.

#include "JWidget.h"
#include "JTextHelper.h"
#include "DragDrop.h"
#include "JStyle.h"
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
    const auto& th  = JStyle::current();
    float p = th.focusRingWidth * 0.5f + 1.f;
    uint8_t ring[4] = {th.Accent[0], th.Accent[1], th.Accent[2], 210};
    uint8_t none[4] = {0, 0, 0, 0};
    buf.pushRectangle(bb.x - p, bb.y - p, bb.width + p * 2.f, bb.height + p * 2.f,
                      none, th.cornerRadius + 1.f, th.focusRingWidth, ring);
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
