#pragma once

// JSpinBox.

#include "JControl.h"
#include "JTextHelper.h"
#include "KeyEvent.h"
#include "SpinRepeat.h"

inline namespace jf {

// ============================================================================
// JSpinBox
// ============================================================================

class JSpinBox : public JControl {
public:
    jf::JSignal<int> onValueChanged;

    JSpinBox(JSceneGraph& graph, int minVal = 0, int maxVal = 100,
            float w = 140.0f, float h = 0.0f)
        : JControl(graph, "JSpinBox"), m_min(minVal), m_max(maxVal), m_value(minVal)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().controlHeight;
        l.minWidth = 60.0f;
        l.minHeight = h;
        m_repeat.onStep = [this](int units) { setValue(m_value + units); };   // hold up/down → accelerating repeat
        m_repeat.timer.onTick.connect([this] { m_repeat.tick(); });
    }

    // Auto-repeat tuning (press-and-hold on the up/down button): initial delay, repeat interval, when the step
    // starts accelerating, how fast it grows, and the max step — all in ms except maxStep.
    void setRepeatConfig(int delayMs, int intervalMs, int accelAfterMs, int accelEveryMs, int maxStep) {
        m_repeat.configure(delayMs, intervalMs, accelAfterMs, accelEveryMs, maxStep);
    }

    void setValue(int v) {
        int c = std::clamp(v, m_min, m_max);
        if (m_value != c) { m_value = c; m_graph.invalidateNode(m_nodeId, DirtySelf); onValueChanged.emit(c); notifyAccessibility(); }
    }
    int  value() const { return m_value; }

    JA11yNode a11yNode() const override {
        JA11yNode n; _a11yFillCommon(n, JA11yRole::SpinBox, m_debugName, std::to_string(m_value));
        n.hasRange = true; n.curValue = (float)m_value; n.minValue = (float)m_min; n.maxValue = (float)m_max;
        n.stateFlags |= JA11yEditable;
        return n;
    }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) { _commitEdit(); return; }
        requestFocus();   // clicking the box focuses it, so typed digits route here (framework focus)
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float btnW = b.height * 0.7f;
        if (mx >= b.x + b.width - btnW) {
            _commitEdit();
            const int dir = (my < b.y + b.height * 0.5f) ? 1 : -1;
            setValue(m_value + dir);   // first click steps once; holding then auto-repeats with acceleration
            m_repeat.begin(dir);
        } else {
            setState(JWidgetState::Pressed);
            _beginEdit();
        }
    }

    void handleMouseRelease(float mx, float my) override {
        m_repeat.end();   // releasing the button stops the auto-repeat (wherever the release lands)
        JControl::handleMouseRelease(mx, my);
    }

    bool handleScroll(float mx, float my, float wheel) override {
        if (!isPointInside(mx, my)) return false;
        _commitEdit();
        setValue(m_value + (wheel > 0.0f ? 1 : -1));
        return true;
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed) return false;
        using K = JKeyEvent::JKey;
        if (ke.key == K::Up)   { _commitEdit(); setValue(m_value + 1); return true; }
        if (ke.key == K::Down) { _commitEdit(); setValue(m_value - 1); return true; }
        if (ke.key == K::Return) { _commitEdit(); return true; }
        if (ke.key == K::Escape) {
            // Restore the value captured when focus arrived — reverts wheel/arrow/typed changes made while
            // focused. If nothing changed, let Escape bubble (so it can still close a dialog).
            if (m_editing || m_value != m_focusValue) { m_editing = false; m_selectAll = false; setValue(m_focusValue); invalidate(); return true; }
            return false;
        }
        if (ke.key == K::Backspace) {
            if (!m_editing) return false;
            if (m_selectAll) { m_editBuf.clear(); m_selectAll = false; }   // delete the selection
            else if (!m_editBuf.empty()) m_editBuf.pop_back();
            invalidate();
            return true;
        }
        const char c = ke.utf8[0];
        if (c != '\0') {
            if (m_editing && m_selectAll) {                 // click-then-type replaces the whole value
                const std::string prev = m_editBuf; m_editBuf.clear();
                if (_acceptChar(c)) { m_selectAll = false; m_editBuf += c; invalidate(); return true; }
                m_editBuf = prev; return true;              // reject but consume (selection stays)
            }
            if (_acceptChar(c)) {
                if (!m_editing) { m_editing = true; m_editBuf.clear(); }
                m_editBuf += c;
                invalidate();
                return true;
            }
        }
        return false;
    }

    // Commit a typed value the instant focus leaves (Tab / click-away), before any repaint or
    // properties-panel rebuild can discard the edit buffer.
    void onFocusEvent(bool focused) override { if (focused) m_focusValue = m_value; else _commitEdit(); }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float btnW = b.height * 0.7f;
        float fieldW = b.width - btnW;

        bool focused = isFocused();
        // Field surface + border by role. A field that is focused OR mid-edit takes the
        // Accent ring, so fold m_editing into the option's focus bit.
        JStyleOption o = jstyle::option(m_state, focused || m_editing);
        // Value field
        buf.pushRectangle(b.x, b.y, fieldW, b.height, jstyle::fieldFill(o).data(),
                          JStyle::current().hint(JStyleHint::ControlRadius),
                          jstyle::borderW(focused || m_editing), jstyle::border(o).data());
        // Value text
        const std::string txt = m_editing ? m_editBuf : std::to_string(m_value);
        if (JTextHelper::hasAtlas()) {
            uint8_t vc[4] = {Colors::FieldText[0], Colors::FieldText[1], Colors::FieldText[2], 220};
            float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            if (m_editing && m_selectAll && !txt.empty()) {   // selection highlight behind the value
                const JColor sel = withAlpha(jstyle::role(JColorRole::Highlight, o), 90);   // Accent @ 90
                buf.pushRectangle(b.x + textPadding() - 1.0f, b.y + 4.0f,
                                  std::min(fieldW - textPadding(), JTextHelper::measureWidth(txt) + 2.0f), b.height - 8.0f, sel.data(), 2.0f);
            }
            JTextHelper::pushText(buf, b.x + textPadding(), ty, txt, vc, fieldW - textPadding());
            if (m_editing && !m_selectAll) {
                float cx = b.x + textPadding() + JTextHelper::measureWidth(txt) + 1.0f;
                buf.pushRectangle(cx, b.y + 6.0f, 1.5f, b.height - 12.0f, jstyle::role(JColorRole::Accent, o).data());
            }
        } else {
            uint8_t vc[4] = {Colors::LabelText[0], Colors::LabelText[1], Colors::LabelText[2], 180};
            buf.pushRectangle(b.x + textPadding(), b.y + (b.height-7.0f)*0.5f, fieldW * 0.6f, 7.0f, vc, 2.0f);
        }

        // Up/down button area — Button role fill, Border-role outline.
        float halfH = b.height * 0.5f;
        const JColor btnFill = jstyle::role(JColorRole::Button, o);
        const JColor btnBd   = jstyle::role(JColorRole::Border, o);
        buf.pushRectangle(b.x + fieldW, b.y,          btnW, halfH, btnFill.data(), 0.0f, 1.0f, btnBd.data());
        buf.pushRectangle(b.x + fieldW, b.y + halfH,  btnW, halfH, btnFill.data(), 0.0f, 1.0f, btnBd.data());
        // Arrow marks (tiny rects)
        float ax = b.x + fieldW + btnW * 0.3f, aw = btnW * 0.4f;
        uint8_t ac[4] = {Colors::MutedText[0], Colors::MutedText[1], Colors::MutedText[2], 200};
        buf.pushRectangle(ax, b.y + halfH * 0.35f,        aw, 2.0f, ac);  // up mark
        buf.pushRectangle(ax, b.y + halfH + halfH * 0.55f, aw, 2.0f, ac); // down mark
    }


private:
    SpinRepeat m_repeat;   // press-and-hold up/down auto-repeat with acceleration

    // Click-to-edit pre-fills the current value AND selects it, so the first typed digit replaces it
    // (standard entry-field behaviour); backspace/edit keys drop into in-place editing instead.
    void _beginEdit() { m_editBuf = std::to_string(m_value); m_editing = true; m_selectAll = true; invalidate(); }
    void _commitEdit() {
        if (!m_editing) return;
        m_editing = false; m_selectAll = false;
        try { setValue(std::stoi(m_editBuf)); } catch (...) {}
        invalidate();
    }
    bool _acceptChar(char c) const {
        if (c >= '0' && c <= '9') return true;
        if (c == '-') return m_min < 0 && m_editBuf.find('-') == std::string::npos;
        return false;
    }

    int m_min, m_max, m_value;
    int m_focusValue{0};   // value captured on focus-in; Escape restores it
    bool m_editing{false};
    bool m_selectAll{false};
    std::string m_editBuf;
};

} // inline namespace jf
