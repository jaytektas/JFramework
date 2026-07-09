#pragma once

// JButton.

#include "JControl.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
class JButton : public JControl {
public:
    JButton(JSceneGraph& graph, const std::string& label,
           float w = 160.0f, float h = 0.0f)
        : JControl(graph, "JButton"), m_label(label)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().buttonHeight;
        l.minWidth = JTextHelper::hasAtlas() ? (JTextHelper::measureWidth(m_label) + 24.f) : w;
        l.minHeight = h;
    }

    void setLabel(const std::string& label) { m_label = label; m_graph.invalidateNode(m_nodeId, DirtySelf); notifyAccessibility(); }
    const std::string& label() const { return m_label; }

    JA11yNode a11yNode() const override {
        JA11yNode n; _a11yFillCommon(n, JA11yRole::Button, m_label, ""); return n;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
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
                          JTheme::current().hint(JStyleHint::ControlRadius),
                          jstyle::borderW(focused), bd.data());
    }
    virtual void drawLabel(JPrimitiveBuffer& buf, const JRect& b) {
        if (JTextHelper::hasAtlas()) {
            std::string txt = tr(m_label);
            const float pad   = 6.f;
            const float avail = b.width - 2.f * pad;                 // interior width for the label
            const float tw    = JTextHelper::measureWidth(txt);
            // Centre when it fits; left-align and clip (maxWidth) when the label is too long, so a long
            // label is truncated inside the button instead of spilling past its edges.
            const float tx = (tw <= avail) ? b.x + (b.width - tw) * 0.5f : b.x + pad;
            const float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            uint8_t tc[4] = {220, 220, 228, 230};
            JTextHelper::pushText(buf, tx, ty, txt, tc, avail);
        } else {
            float tw = b.width * 0.5f;
            float tx = b.x + (b.width - tw) * 0.5f;
            uint8_t tc[4] = {220, 220, 228, 200};
            buf.pushRectangle(tx, b.y + (b.height - 6.0f) * 0.5f, tw, 6.0f, tc, 2.0f);
        }
    }

private:
    std::string m_label;
};

} // inline namespace jf
