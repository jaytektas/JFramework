#pragma once

// JToolButton — compact icon/text button over JButton.

#include "JButton.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
// JToolButton — a compact icon/text button. Two opt-in traits over JButton:
//   autoRaise      — flat (no fill/border) at rest; only draws the button chrome
//                    on hover/press/focus. The toolbar look.
//   menu arrow     — an attached down-chevron on the right; clicking it fires
//                    onMenuRequested (for an attached pop-up menu) instead of
//                    onClicked, so a split action/menu button is one control.
// Everything else (label, sizing, styling) is inherited unchanged.
// ============================================================================

class JToolButton : public JButton {
public:
    jf::JSignal<> onMenuRequested;   // fires when the menu-arrow zone is clicked

    JToolButton(JSceneGraph& graph, const std::string& label,
                float w = 96.0f, float h = 0.0f)
        : JButton(graph, label, w, h) {}

    void setAutoRaise(bool a) { m_autoRaise = a; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    bool autoRaise() const { return m_autoRaise; }
    // Show the attached pop-up-menu arrow on the right edge.
    void setMenuArrow(bool on) { m_menuArrow = on; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    bool hasMenuArrow() const { return m_menuArrow; }

    void handleMousePress(float mx, float my) override {
        if (m_state == JWidgetState::Disabled || !isPointInside(mx, my)) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (m_menuArrow && mx >= b.x + b.width - _arrowZone(b.height)) {
            setState(JWidgetState::Pressed);
            onMenuRequested.emit();      // arrow zone → request the menu, not the primary action
            return;
        }
        JButton::handleMousePress(mx, my);   // primary action (emits onClicked)
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        JButton::populateRenderPrimitives(buf);   // background(our override) + label
        if (m_menuArrow) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            float cx = b.x + b.width - _arrowZone(b.height) * 0.5f;
            float cy = b.y + b.height * 0.5f;
            uint8_t ac[4] = {180, 180, 190, 220};    // down-chevron (two rects forming a V)
            buf.pushRectangle(cx - 4.0f, cy - 1.0f, 5.0f, 2.0f, ac, 1.0f);
            buf.pushRectangle(cx + 1.0f, cy - 1.0f, 5.0f, 2.0f, ac, 1.0f);
        }
    }

protected:
    // Flat at rest when auto-raised: skip the fill+border entirely unless hovered/pressed/focused.
    void drawBackground(JPrimitiveBuffer& buf, const JRect& b, const uint8_t* fill, bool focused) override {
        if (m_autoRaise && m_state == JWidgetState::Normal && !focused) return;   // flat
        JButton::drawBackground(buf, b, fill, focused);
    }

private:
    static float _arrowZone(float h) { return h * 0.7f; }
    bool m_autoRaise{true};
    bool m_menuArrow{false};
};

} // inline namespace jf
