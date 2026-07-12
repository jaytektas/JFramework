#pragma once

// JLabel.

#include "JWidget.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
// JLabel
// ============================================================================

class JLabel : public JWidget {
public:
    JLabel(JSceneGraph& graph, const std::string& text, float w = 240.0f, float h = 0.0f)
        : JWidget(graph, "JLabel: " + text), m_text(text)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().labelHeight;
        l.minWidth = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(m_text) : w;
        l.minHeight = h;
    }

    void setText(const std::string& t) { m_text = t; m_graph.invalidateNode(m_nodeId, DirtySelf); notifyAccessibility(); }

    JA11yNode a11yNode() const override {
        JA11yNode n; _a11yFillCommon(n, JA11yRole::Label, m_text, ""); return n;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        // Resolve the theme's TEXT role (so a custom global palette applies), not a raw label shade; the
        // muted label look comes from the alpha, not a separate hardcoded colour.
        const uint8_t* base = jstyle::role(JColorRole::Text, jstyle::option(m_state, false)).data();
        if (JTextHelper::hasAtlas()) {
            uint8_t c[4] = {base[0], base[1], base[2], 200};
            float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            JTextHelper::pushText(buf, b.x, ty, tr(m_text), c, b.width);
        } else {
            // Fallback placeholder bars
            float cy = b.y + b.height * 0.5f - 3.0f;
            uint8_t c[4] = {base[0], base[1], base[2], 140};
            buf.pushRectangle(b.x, cy, b.width * 0.55f, 6.0f, c, 2.0f);
        }
    }


private:
    std::string m_text;
};

} // inline namespace jf
