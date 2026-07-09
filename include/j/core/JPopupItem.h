#pragma once

// JPopupItem — flat borderless full-width clickable text row.

#include "JControl.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
// JPopupItem — a flat, borderless, full-width clickable text row.
//
// The standard building block for popup list content (combo-boxes, menus,
// tree-pickers, etc.).  It has no border and no background of its own;
// the containing JPopupWindow supplies the backdrop.  On hover it shows a
// subtle tint so the user knows it is interactive.
//
// onActivated fires on mouse-release while the cursor is inside.
// ============================================================================

class JPopupItem : public JControl {
public:
    jf::JSignal<> onActivated;

    JPopupItem(JSceneGraph& graph, const std::string& label,
              float width = 200.f, float itemHeight = 28.f)
        : JControl(graph, "JPopupItem"), m_label(label)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width  = width;
        l.boundingBox.height = itemHeight;
        l.minWidth  = JTextHelper::hasAtlas()
                        ? (JTextHelper::measureWidth(label) + 24.f)
                        : width;
        l.minHeight = itemHeight;
    }

    void setLabel(const std::string& s) {
        m_label = s;
        auto& l = m_graph.getLayout(m_nodeId);
        l.minWidth = JTextHelper::hasAtlas()
                       ? (JTextHelper::measureWidth(s) + 24.f)
                       : l.boundingBox.width;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    const std::string& label() const { return m_label; }

    void handleMouseRelease(float mx, float my) override {
        if (m_state == JWidgetState::Pressed && isPointInside(mx, my)) {
            onClicked.emit();
            onActivated.emit();
        }
        JControl::handleMouseRelease(mx, my);
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;

        // Hover highlight — subtle tint only; no border, no solid fill at rest.
        if (m_state == JWidgetState::Hovered || m_state == JWidgetState::Pressed) {
            uint8_t hi[4] = {Colors::White[0], Colors::White[1], Colors::White[2], 18};
            buf.pushRectangle(b.x, b.y, b.width, b.height, hi, 3.0f);
        }

        // JLabel text
        if (JTextHelper::hasAtlas()) {
            uint8_t tc[4] = {Colors::ControlText[0], Colors::ControlText[1], Colors::ControlText[2], 230};
            float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            JTextHelper::pushText(buf, b.x + 10.f, ty, tr(m_label), tc,
                                 b.width - 16.f);
        } else {
            // Fallback: coloured bar representing the text
            uint8_t tc[4] = {Colors::LabelText[0], Colors::LabelText[1], Colors::LabelText[2], 180};
            float bw = JTextHelper::hasAtlas()
                       ? JTextHelper::measureWidth(m_label)
                       : b.width * 0.6f;
            buf.pushRectangle(b.x + 10.f, b.y + (b.height - 6.f) * 0.5f,
                              bw, 6.f, tc, 2.f);
        }
    }


private:
    std::string m_label;
};

} // inline namespace jf
