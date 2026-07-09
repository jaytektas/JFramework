#pragma once

// JControl — interactive widget base (hover/press/click). Extracted from BaseWidgets.h.

#include "JWidget.h"

inline namespace jf {

// ============================================================================
// JControl — interactive widget with hover/press/click signals
// ============================================================================

// The scheme's default interior text padding (defined after JStyle, below). Forward-declared here so
// JControl — which precedes JStyle in this header — can fall back to it in textPadding().
inline float _jStyleFieldPadding();

class JControl : public JWidget {
public:
    jf::JSignal<>     onHoverEntered;
    jf::JSignal<>     onHoverExited;
    jf::JSignal<>     onClicked;
    jf::JSignal<bool> onFocusChanged;

    JControl(JSceneGraph& graph, const std::string& name) : JWidget(graph, name) {
        // Interactive controls are click- and tab-focusable by default (Qt StrongFocus).
        m_focusPolicy = JFocusPolicy::StrongFocus;
    }

    void handleMouseMove(float mx, float my) override {
        if (m_state == JWidgetState::Disabled) return;
        bool inside = isPointInside(mx, my);
        if (inside && m_state == JWidgetState::Normal)   { setState(JWidgetState::Hovered); onHoverEntered.emit(); }
        else if (!inside && m_state == JWidgetState::Hovered) { setState(JWidgetState::Normal);  onHoverExited.emit();  }
    }

    void handleMousePress(float mx, float my) override {
        if (m_state == JWidgetState::Disabled) return;
        if (isPointInside(mx, my)) { setState(JWidgetState::Pressed); onClicked.emit(); }
    }

    void handleMouseRelease(float mx, float my) override {
        if (m_state == JWidgetState::Disabled) return;
        if (m_state == JWidgetState::Pressed)
            setState(isPointInside(mx, my) ? JWidgetState::Hovered : JWidgetState::Normal);
    }


    // Interior text padding for input controls (JLineEdit/JSpinBox/JComboBox…). Falls back to the
    // scheme's JStyle::fieldPadding; set per-instance for granular control (a negative value restores
    // the scheme default).
    void  setTextPadding(float p) { m_textPad = p; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    float textPadding() const { return m_textPad >= 0.f ? m_textPad : _jStyleFieldPadding(); }

protected:
    float m_textPad = -1.f;   // -1 = inherit the scheme's fieldPadding
};

// (Legacy JAction stub removed — superseded by the full JAction in j/core/Action.h,
//  which carries a JKeySequence shortcut and integrates with the JShortcutRegistry.)

} // inline namespace jf
