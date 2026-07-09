#pragma once

// JScrollBar.

#include "JControl.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================================
// JScrollBar
// ============================================================================

class JScrollBar : public JControl {
public:
    jf::JSignal<float> onScrolled; // emits 0..1 position

    JScrollBar(JSceneGraph& graph, float w = 280.0f, float h = 14.0f,
              float thumbRatio = 0.3f)
        : JControl(graph, "JScrollBar"), m_thumbRatio(std::clamp(thumbRatio, 0.05f, 1.0f))
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().menuItemHeight;
        l.minWidth = 40.0f;
        l.minHeight = h;
    }

    void setScrollPosition(float p) {
        float c = std::clamp(p, 0.0f, 1.0f);
        if (m_position != c) { m_position = c; m_graph.invalidateNode(m_nodeId, DirtySelf); onScrolled.emit(c); notifyAccessibility(); }
    }
    float scrollPosition() const { return m_position; }

    JA11yNode a11yNode() const override {
        char v[24]; std::snprintf(v, sizeof(v), "%.2f", m_position);
        JA11yNode n; _a11yFillCommon(n, JA11yRole::ScrollBar, m_debugName, v);
        n.hasRange = true; n.curValue = m_position; n.minValue = 0.0f; n.maxValue = 1.0f;
        return n;
    }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) return;
        setState(JWidgetState::Pressed);
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        setScrollPosition((mx - b.x) / b.width - m_thumbRatio * 0.5f);
    }
    void handleMouseMove(float mx, float my) override {
        JControl::handleMouseMove(mx, my);
        if (m_state == JWidgetState::Pressed) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            setScrollPosition((mx - b.x) / b.width - m_thumbRatio * 0.5f);
        }
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        JStyleOption o = jstyle::option(m_state, isFocused());
        // Gutter = Base role (old Surface1).
        buf.pushRectangle(b.x, b.y, b.width, b.height, jstyle::fieldFill(o).data(), 7.0f);
        float tw = b.width * m_thumbRatio;
        float tx = b.x + m_position * (b.width - tw);
        // Thumb: pressed => themed AccentPress; hovered => ToolTipBase (old Surface3);
        // else Border role.
        const JColor tc = (m_state == JWidgetState::Pressed) ? JColor::fromArray(Colors::AccentPress)
                        : (m_state == JWidgetState::Hovered) ? jstyle::role(JColorRole::ToolTipBase, o)
                                                             : jstyle::role(JColorRole::Border, o);
        buf.pushRectangle(tx, b.y + 1.0f, tw, b.height - 2.0f, tc.data(), 6.0f);
    }


private:
    float m_position{0.0f};
    float m_thumbRatio;
};

} // inline namespace jf
