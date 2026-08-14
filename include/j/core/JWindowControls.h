#pragma once

// Window title/close primitives + the drag driver + JWidget's text-dependent out-of-line
// methods (renderTooltips/drawFocusRing). Kept together — they need JWidget AND JTextHelper.

#include "JWidget.h"
#include "JTextHelper.h"
#include "DragDrop.h"
#include "JStyle.h"
#include "../graphics/RenderPrimitive.h"

inline namespace jf {


// The topmost tooltip-bearing widget under (mx, my) within one subtree, or null.
//
// PAINT ORDER decides "topmost": a parent paints before its children, and earlier siblings before
// later ones, so the search tries the last child first and only falls back to the widget itself once
// nothing inside it answered. That is the tree equivalent of the reverse walk the flat registry scan
// used to do, and it stays right when a window reorders its children.
//
// Visibility prunes the whole subtree — a hidden container paints nothing, so nothing inside it can
// be hovered. isVisibleSelf() is enough here (not isVisible(), which re-walks every ancestor on each
// node): the descent only reaches a node whose ancestors were already checked on the way down.
//
// Bounds deliberately do NOT prune the descent. A child is not guaranteed to be inside its parent's
// box — an overflowing or hand-placed child would otherwise become untooltippable — so only the
// final hit decision consults the geometry.
inline JWidget* jTooltipHitTest(JWidget* w, float mx, float my) {
    if (!w || !w->isVisibleSelf()) return nullptr;
    std::vector<JWidget*> kids;
    w->collectChildren(kids);
    for (auto it = kids.rbegin(); it != kids.rend(); ++it)
        if (JWidget* hit = jTooltipHitTest(*it, mx, my)) return hit;
    return (!w->tooltip().empty() && w->hitTest(mx, my)) ? w : nullptr;
}

inline void JWidget::renderTooltips(JPrimitiveBuffer& buf, JTooltipHover& hover,
                                    const std::vector<JWidget*>& roots,
                                    float mouseX, float mouseY,
                                    float viewW, float viewH) {
    // Last root first: later roots are declared in paint order too (the central widget goes on after
    // the docks), so the same "later wins" rule that orders siblings orders these.
    JWidget* hovered = nullptr;
    for (auto it = roots.rbegin(); it != roots.rend() && !hovered; ++it)
        hovered = jTooltipHitTest(*it, mouseX, mouseY);

    if (hovered != hover.last) {
        hover.last  = hovered;
        hover.since = std::chrono::steady_clock::now();
    }

    if (hovered) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - hover.since).count();
        const JStyle& st = JStyle::current();
        if (static_cast<float>(elapsed) < st.tooltipDelayMs) {
            return; // hover dwell not yet served
        }

        const float padX = st.tooltipPaddingX, padY = st.tooltipPaddingY;
        const float lineH = JTextHelper::lineHeight();
        const std::string text = hovered->tooltip();

        // WRAP. A tooltip carrying real help — a sentence explaining what a field does — was drawn as one
        // unbroken line and simply ran off the screen, so the part that mattered was the part you could not
        // read. Break on spaces at a comfortable measure, then keep the whole box on screen.
        const float kMaxW = st.tooltipMaxWidth;
        std::vector<std::string> lines;
        {
            std::string line;
            size_t i = 0;
            while (i < text.size()) {
                if (text[i] == '\n') {           // an explicit break is honoured, not swallowed
                    lines.push_back(line);
                    line.clear();
                    ++i;
                    continue;
                }
                size_t sp = text.find_first_of(" \n", i);
                const std::string word = text.substr(i, sp == std::string::npos ? sp : sp - i);
                const std::string next = line.empty() ? word : line + " " + word;
                if (!line.empty() && JTextHelper::measureWidth(next) > kMaxW - padX * 2.0f) {
                    lines.push_back(line);
                    line = word;
                } else {
                    line = next;
                }
                if (sp == std::string::npos) break;
                i = (text[sp] == '\n') ? sp : sp + 1;   // leave a newline for the branch above
            }
            if (!line.empty()) lines.push_back(line);
        }
        float textW = 0.0f;
        for (const std::string& l : lines) textW = std::max(textW, JTextHelper::measureWidth(l));

        const float tooltipW = textW + padX * 2.0f;
        const float tooltipH = lineH * float(lines.size()) + padY * 2.0f;
        const float gap = st.tooltipCursorGap;
        float x = mouseX + gap;
        float y = mouseY + gap;
        // Keep it inside the window rather than letting it hang off the edge the cursor is near.
        if (viewW > 0.f && x + tooltipW > viewW) x = std::max(0.f, mouseX - gap - tooltipW);
        if (viewH > 0.f && y + tooltipH > viewH) y = std::max(0.f, mouseY - gap - tooltipH);

        const float sh = st.tooltipShadowOffset, r = st.tooltipRadius;
        buf.pushRectangle(x + sh, y + sh, tooltipW, tooltipH, Colors::ToolTipShadow, r);
        buf.pushRectangle(x, y, tooltipW, tooltipH, Colors::ToolTipFill, r, st.borderWidth, Colors::ToolTipBorder);
        for (size_t li = 0; li < lines.size(); ++li)
            JTextHelper::pushText(buf, x + padX, y + padY + lineH * float(li), lines[li], Colors::TextPrimary);
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
