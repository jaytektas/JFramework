#pragma once

// JGroupBox.

#include "JWidget.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
// JGroupBox  (labelled container panel)
// ============================================================================

class JGroupBox : public JWidget {
public:
    JGroupBox(JSceneGraph& graph, const std::string& title,
             float w = 320.0f, float h = 120.0f)
        : JWidget(graph, "JGroupBox: " + title), m_title(title)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width  = w;
        l.boundingBox.height = h;
        l.padding = 12.0f;
        l.gap     = 8.0f;
        l.direction = JFlexDirection::Column;
    }

    JA11yNode a11yNode() const override {
        JA11yNode n; _a11yFillCommon(n, JA11yRole::Group, m_title, ""); return n;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        // Panel body. The translucent panel/title shades are bespoke (no palette role
        // resolves to them) so they stay literal to preserve pixels; the outline routes
        // through the Border role so it follows a theme/palette swap.
        uint8_t panelFill[4] = {22, 22, 25, 180};
        const JColor bd = jstyle::role(JColorRole::Border, jstyle::option(m_state, isFocused()));
        buf.pushRectangle(b.x, b.y, b.width, b.height, panelFill, 8.0f, 1.0f, bd.data());
        // Title bar strip at top
        uint8_t titleBg[4] = {36, 36, 40, 200};
        buf.pushRectangle(b.x + 1.0f, b.y + 1.0f, b.width - 2.0f, 24.0f, titleBg, 7.0f);
        // Title text
        if (JTextHelper::hasAtlas()) {
            uint8_t tc[4] = {200, 200, 210, 200};
            float ty = b.y + (24.0f - JTextHelper::lineHeight()) * 0.5f;
            JTextHelper::pushText(buf, b.x + 10.0f, ty, tr(m_title), tc, b.width - 20.0f);
        } else {
            uint8_t tc[4] = {200, 200, 210, 180};
            buf.pushRectangle(b.x + 10.0f, b.y + 9.0f, b.width * 0.4f, 7.0f, tc, 2.0f);
        }
    }


private:
    std::string m_title;
};

} // inline namespace jf
