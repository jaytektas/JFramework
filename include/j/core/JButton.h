#pragma once

#include <cmath>

// JButton.

#include "JControl.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
class JButton : public JControl {
public:
    // A button is activated by Space AND Return (the base handles Space for every control).
protected:
    bool activatesOnReturn() const override { return true; }
public:
    JButton(JSceneGraph& graph, const std::string& label,
           float w = 160.0f, float h = 0.0f)
        : JControl(graph, "JButton"), m_label(label)
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
    void setLabel(const std::string& label) { m_label = label; m_graph.invalidateNode(m_nodeId, DirtySelf); notifyAccessibility(); }
    const std::string& label() const { return m_label; }

    JA11yNode a11yNode() const override {
        JA11yNode n; _a11yFillCommon(n, JA11yRole::Button, m_label, ""); return n;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        _refreshMinWidth();   // the label decides the floor, not the caller's design width
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        // Fill by ROLE/state: normal=Button, hover=ToolTipBase (old Surface3), pressed=Highlight.
        const JColor fill = jstyle::buttonFill(jstyle::option(m_state, isFocused()));
        drawBackground(buf, b, fill.data(), isFocused());
        drawLabel(buf, b);
    }


protected:
    virtual void drawBackground(JPrimitiveBuffer& buf, const JRect& b, const uint8_t* fill, bool focused) {
        // Outline by role (Accent ring when focused, else Border) + themed widths.
        const JColor bd = jstyle::border(jstyle::option(m_state, focused));
        buf.pushRectangle(b.x, b.y, b.width, b.height, fill,
                          JStyle::current().hint(JStyleHint::ControlRadius),
                          jstyle::borderW(focused), bd.data());
    }
    virtual void drawLabel(JPrimitiveBuffer& buf, const JRect& b) {
        if (JTextHelper::hasAtlas()) {
            // Centre when it fits; left-align and clip when the label is too long, so a long label is
            // truncated inside the button instead of spilling past its edges — see pushTextAligned.
            uint8_t tc[4] = {Colors::ControlText[0], Colors::ControlText[1], Colors::ControlText[2], 230};
            // A DISABLED button says so with its CAPTION. The fill's disabled shade differs by a handful
            // of levels on a dark theme — true to the palette and all but invisible — so a greyed-out
            // button read as a live one, which is exactly the state a button must never be caught in.
            if (m_state == JWidgetState::Disabled) {
                const JColor t = jstyle::pal().color(JColorRole::ButtonText, JColorGroup::Disabled);
                tc[0] = t.r; tc[1] = t.g; tc[2] = t.b; tc[3] = 230;
            }
            JTextHelper::pushTextAligned(buf, b.x, b.y, b.width, b.height, tr(m_label), tc,
                                         JTextHelper::Align::Center, 6.f);
        } else {
            float tw = b.width * 0.5f;
            float tx = b.x + (b.width - tw) * 0.5f;
            uint8_t tc[4] = {Colors::ControlText[0], Colors::ControlText[1], Colors::ControlText[2], 200};
            buf.pushRectangle(tx, b.y + (b.height - 6.0f) * 0.5f, tw, 6.0f, tc, 2.0f);
        }
    }

private:
    std::string m_label;
};

} // inline namespace jf
