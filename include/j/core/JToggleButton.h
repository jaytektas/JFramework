// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

#include <cmath>

// JToggleButton.

#include "JControl.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
// JToggleButton
// ============================================================================

class JToggleButton : public JControl {
public:
    jf::JSignal<bool> onToggled;

    JToggleButton(JSceneGraph& graph, const std::string& label,
                 float w = 160.0f, float h = 0.0f)
        : JControl(graph, "JToggleButton"), m_label(label)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().buttonHeight;
        l.minWidth = JTextHelper::hasAtlas() ? (JTextHelper::measureWidth(m_label) + 24.f) : w;
        l.minHeight = h;
    }


    // WHAT THIS BUTTON NEEDS TO SHOW ITS LABEL, measured now rather than at construction. A caller's
    // width is a design size chosen at 100 %; the text is measured against the live font atlas, which
    // already carries the interface scale, so the two disagree the moment the scale is not 1 and the
    // label is what loses ("Locked" rendered as "Locke"). Asked at LAYOUT time, when the atlas exists —
    // the constructor runs before it does, which is why sizing there could never work.
    // KEEP THE FLOOR HONEST. The layout engine clamps a child to its minWidth, so that is where "this
    // button must be wide enough for its label" has to live — and it was computed ONCE, in the
    // constructor, where JTextHelper has no atlas yet and the answer fell back to the caller's design
    // width. It therefore never reflected the label, and never reflected the interface scale either.
    //
    // Re-measured whenever the button paints (the atlas certainly exists by then) and written only when
    // it actually moves, so a steady frame dirties nothing. One call, and every button in the app is
    // wide enough for its own text at whatever scale the screen asked for.
    void _refreshMinWidth() const {
        if (!JTextHelper::hasAtlas()) return;
        const float want = JTextHelper::measureWidth(m_label) + JStyle::current().fieldPadding * 3.f;
        auto& l = m_graph.getLayout(m_nodeId);
        if (std::abs(l.minWidth - want) > 0.5f) {
            l.minWidth = want;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        }
    }

    jf::JRect preferredSize() const override {
        const JStyle& s = JStyle::current();
        const float pad = s.fieldPadding * 3.f;   // 24 px at 100 %, as the old fixed minimum was
        const float tw  = JTextHelper::hasAtlas()
                        ? JTextHelper::measureWidth(m_label)
                        : static_cast<float>(m_label.size()) * 8.f * JStyle::uiScale();
        const jf::JRect b = bounds();
        return jf::JRect{ b.x, b.y, tw + pad, s.buttonHeight };
    }

    void setToggled(bool v) {
        if (m_toggled != v) {
            m_toggled = v;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onToggled.emit(v);
            notifyAccessibility();
        }
    }
    bool isToggled() const { return m_toggled; }

protected:
    void activate() override { setToggled(!isToggled()); }   // Space toggles
public:
    void setLabel(const std::string& l) { m_label = l; m_graph.invalidateNode(m_nodeId, DirtySelf); notifyAccessibility(); }
    const std::string& label() const { return m_label; }

    JA11yNode a11yNode() const override {
        JA11yNode n; _a11yFillCommon(n, JA11yRole::ToggleButton, m_label, m_toggled ? "on" : "off");
        if (m_toggled) n.stateFlags |= JA11yChecked;
        return n;
    }

    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) setState(JWidgetState::Pressed);
    }
    void handleMouseRelease(float mx, float my) override {
        if (m_state == JWidgetState::Pressed && isPointInside(mx, my)) setToggled(!m_toggled);
        JControl::handleMouseRelease(mx, my);
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        _refreshMinWidth();   // the label decides the floor, not the caller's design width
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        // Base fill by ROLE: toggled=Highlight, else Button (old Accent / Surface2).
        JStyleOption o = jstyle::option(m_state, isFocused(), m_toggled, m_toggled);
        JColor fill = m_toggled ? jstyle::role(JColorRole::Highlight, o)
                                : jstyle::role(JColorRole::Button, o);
        if (m_state == JWidgetState::Hovered) {   // hover brighten preserved (no discrete role)
            fill.r = static_cast<uint8_t>(std::min(255, fill.r + 20));
            fill.g = static_cast<uint8_t>(std::min(255, fill.g + 20));
            fill.b = static_cast<uint8_t>(std::min(255, fill.b + 20));
        }
        drawBackground(buf, b, fill.data(), isFocused());
        drawLabel(buf, b);
    }


protected:
    virtual void drawBackground(JPrimitiveBuffer& buf, const JRect& b, const uint8_t* fill, bool focused) {
        const JColor bd = jstyle::border(jstyle::option(m_state, focused));
        buf.pushRectangle(b.x, b.y, b.width, b.height, fill,
                          JStyle::current().hint(JStyleHint::ControlRadius),
                          jstyle::borderW(focused), bd.data());
    }
    virtual void drawLabel(JPrimitiveBuffer& buf, const JRect& b) {
        if (JTextHelper::hasAtlas()) {
            std::string txt = tr(m_label);
            const float pad   = 6.f;
            const float avail = b.width - 2.f * pad;
            const float tw    = JTextHelper::measureWidth(txt);
            const float tx    = (tw <= avail) ? b.x + (b.width - tw) * 0.5f : b.x + pad;   // centre or left-align+clip
            uint8_t tc[4] = {Colors::ControlText[0], Colors::ControlText[1], Colors::ControlText[2], 230};
            JTextHelper::pushText(buf, tx, b.y + (b.height-JTextHelper::lineHeight())*0.5f, txt, tc, avail);
        } else {
            float tw = b.width * 0.5f;
            uint8_t tc[4] = {Colors::ControlText[0], Colors::ControlText[1], Colors::ControlText[2], 200};
            buf.pushRectangle(b.x + (b.width-tw)*0.5f, b.y + (b.height-6.0f)*0.5f, tw, 6.0f, tc, 2.0f);
        }
    }

private:
    std::string m_label;
    bool m_toggled{false};
};

} // inline namespace jf
