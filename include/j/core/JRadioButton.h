#pragma once

// JRadioButton.

#include "JControl.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
// JRadioButton
// ============================================================================

class JRadioButton : public JControl {
public:
    jf::JSignal<bool> onSelected;

    JRadioButton(JSceneGraph& graph, const std::string& label,
                float w = 200.0f, float h = 0.0f)
        : JControl(graph, "JRadioButton"), m_label(label)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().checkHeight;
        l.minWidth = JTextHelper::hasAtlas() ? (JTextHelper::measureWidth(m_label) + 28.f) : w;
        l.minHeight = h;
    }

    void setSelected(bool v) {
        if (m_selected != v) { m_selected = v; m_graph.invalidateNode(m_nodeId, DirtySelf); onSelected.emit(v); notifyAccessibility(); }
    }
    bool isSelected() const { return m_selected; }

    JA11yNode a11yNode() const override {
        JA11yNode n; _a11yFillCommon(n, JA11yRole::RadioButton, m_label, m_selected ? "selected" : "");
        if (m_selected) n.stateFlags |= (JA11yChecked | JA11ySelected);
        return n;
    }

    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) setSelected(true);
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float r = b.height;
        // Ring fill by role: selected=Highlight, else Base (old Accent / Surface1) — the
        // exact controlFill decision the checkbox uses.
        JStyleOption o = jstyle::option(m_state, isFocused(), m_selected, m_selected);
        const JColor ring = JStyle::controlFill(o, jstyle::pal());
        drawCircle(buf, b, r, ring.data(), isFocused());
        drawLabel(buf, b, r);
    }


protected:
    virtual void drawCircle(JPrimitiveBuffer& buf, const JRect& b, float r, const uint8_t* ring, bool focused) {
        float borderWidth = 1.5f;
        const JColor bd = jstyle::border(jstyle::option(m_state, focused));
        buf.pushRectangle(b.x, b.y, r, r, ring, r * 0.5f,
                          focused ? 2.0f : borderWidth,
                          bd.data());
        if (m_selected) {
            float dot = r * 0.42f, offset = (r - dot) * 0.5f;
            const JColor dc = jstyle::role(JColorRole::Text, jstyle::option(m_state, focused, true, true));
            buf.pushRectangle(b.x + offset, b.y + offset, dot, dot, dc.data(), dot * 0.5f);
        }
    }
    virtual void drawLabel(JPrimitiveBuffer& buf, const JRect& b, float r) {
        const float gap = JStyle::current().itemPadding;   // label offset from the ring
        if (JTextHelper::hasAtlas()) {
            uint8_t lc[4] = {Colors::LabelText[0], Colors::LabelText[1], Colors::LabelText[2], 200};
            JTextHelper::pushText(buf, b.x + r + gap,
                                 b.y + (b.height - JTextHelper::lineHeight()) * 0.5f,
                                 tr(m_label), lc, b.width - r - gap);
        } else {
            uint8_t lc[4] = {Colors::MutedText[0], Colors::MutedText[1], Colors::MutedText[2], 140};
            buf.pushRectangle(b.x + r + gap, b.y + (b.height - 6.0f)*0.5f,
                               b.width - r - gap, 6.0f, lc, 2.0f);
        }
    }

private:
    std::string m_label;
    bool m_selected{false};
};

} // inline namespace jf
