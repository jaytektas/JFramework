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
    jf::JSignal<>     onClicked;    // fired on RELEASE INSIDE (a completed click)
    jf::JSignal<>     onPressed;     // fired on mouse-down over the control
    jf::JSignal<>     onReleased;    // fired on mouse-up after we were pressed, inside OR outside
    // onFocusChanged now lives on the base JWidget (every widget emits it from setFocused) — inherited here.

    JControl(JSceneGraph& graph, const std::string& name) : JWidget(graph, name) {
        // Interactive controls are click- and tab-focusable by default (Qt StrongFocus).
        m_focusPolicy = JFocusPolicy::StrongFocus;
    }

    void handleMouseMove(float mx, float my) override {
        if (m_state == JWidgetState::Disabled) return;
        bool inside = isPointInside(mx, my);
        // While armed (pressed on us, button still held) track the pointer: show Pressed only while the
        // cursor is over us, springing back if you slide off — the visual half of click-cancel.
        if (m_armed) { setState(inside ? JWidgetState::Pressed : JWidgetState::Normal); return; }
        if (inside && m_state == JWidgetState::Normal)   { setState(JWidgetState::Hovered); onHoverEntered.emit(); }
        else if (!inside && m_state == JWidgetState::Hovered) { setState(JWidgetState::Normal);  onHoverExited.emit();  }
    }

    // Press ARMS the control; the click fires on RELEASE INSIDE. Press then slide off before releasing
    // CANCELS it — standard everywhere (Qt/GTK/Win32/macOS) and the only way a user can back out of a
    // click they didn't mean. (This used to emit onClicked from the press, so a button fired the instant
    // it was touched and could never be cancelled.)
    void handleMousePress(float mx, float my) override {
        if (m_state == JWidgetState::Disabled) return;
        if (isPointInside(mx, my)) {
            if (acceptsClickFocus()) requestFocus();   // clicking a control focuses it — every toolkit does this
            m_armed = true;
            setState(JWidgetState::Pressed);
            onPressed.emit();
        }
    }

    // ---- Keyboard: the standard control contract -------------------------------------------------
    // Space (and Return) performs the control's default action; arrow keys step it. Implemented ONCE
    // here so every control is keyboard-operable by default instead of opting in one at a time — a
    // subclass says WHAT its action is (activate/step), never how the keys reach it.
    //
    // A subclass that owns these keys for itself (JLineEdit types a space; JTextArea takes Return)
    // overrides handleKeyEvent and simply doesn't delegate here for those keys.
    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed || m_state == JWidgetState::Disabled) return false;
        using K = JKeyEvent::JKey;
        switch (ke.key) {
            case K::Space:                        activate(); return true;
            case K::Return:  if (!activatesOnReturn()) return false;  activate(); return true;
            case K::Up:                           return step(+1);
            case K::Down:                         return step(-1);
            case K::Left:                         return step(-1);
            case K::Right:                        return step(+1);
            default: break;
        }
        if (ke.utf8[0] == ' ') { activate(); return true; }   // Space arriving as a printable byte
        return false;
    }

protected:
    // The control's default action (Space / Return): a button clicks, a checkbox toggles, a combo
    // opens its list. Default: emit onClicked, which is what a plain control's click means.
    virtual void activate() { if (m_state != JWidgetState::Disabled) onClicked.emit(); }
    // Arrow-key stepping (+1 = up/right, -1 = down/left). Return true if the control consumed it.
    virtual bool step(int /*dir*/) { return false; }
    // Whether Return also activates. True for buttons; false for controls where Return means "commit"
    // or belongs to a dialog's default button.
    virtual bool activatesOnReturn() const { return false; }

public:

    void handleMouseRelease(float mx, float my) override {
        if (m_state == JWidgetState::Disabled) return;
        const bool inside = isPointInside(mx, my);
        const bool wasArmed = m_armed;
        const bool fire     = m_armed && inside;        // armed on us AND released on us
        m_armed = false;
        if (m_state == JWidgetState::Pressed || fire)
            setState(inside ? JWidgetState::Hovered : JWidgetState::Normal);
        if (wasArmed) onReleased.emit();                // released() fires even when the click is cancelled
        if (fire)     onClicked.emit();
    }


    // Interior text padding for input controls (JLineEdit/JSpinBox/JComboBox…). Falls back to the
    // scheme's JStyle::fieldPadding; set per-instance for granular control (a negative value restores
    // the scheme default).
    void  setTextPadding(float p) { m_textPad = p; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    float textPadding() const { return m_textPad >= 0.f ? m_textPad : _jStyleFieldPadding(); }

protected:
    bool  m_armed = false;    // press landed on us; a release inside fires onClicked
    float m_textPad = -1.f;   // -1 = inherit the scheme's fieldPadding
};

// (Legacy JAction stub removed — superseded by the full JAction in j/core/Action.h,
//  which carries a JKeySequence shortcut and integrates with the JShortcutRegistry.)

} // inline namespace jf
