#pragma once

// JDoubleSpinBox.

#include "JControl.h"
#include "JTextHelper.h"
#include "JNumericField.h"
#include "KeyEvent.h"
#include "SpinRepeat.h"

inline namespace jf {

// ============================================================================
// JDoubleSpinBox — floating-point spin box.
//
// Mirrors JSpinBox exactly, but stores a `double` with configurable step,
// decimal places and an optional textual suffix.  Full precision is kept
// internally; only the rendered text is rounded to `decimals`.
//
// The value field is a REAL text field (JNumericField over the shared JTextEditCore): focusing the box puts a
// caret in the value, so you can click into the middle of it, select part of it, copy it, and edit a digit
// without retyping the number. Return / focus-out commits, Escape restores the value focus arrived with.
// ============================================================================

class JDoubleSpinBox : public JControl {
public:
    jf::JSignal<double> onValueChanged;

    JDoubleSpinBox(JSceneGraph& graph, double min, double max,
                   double step = 1.0, int decimals = 2,
                   float w = 120.0f, float h = 0.0f)
        : JControl(graph, "JDoubleSpinBox"),
          m_value(min), m_min(min), m_max(max), m_step(step), m_decimals(decimals)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().controlHeight;
        l.minWidth = 60.0f;
        l.minHeight = h;
        m_field.setGrammar(decimals > 0, min < 0.0);
        m_repeat.onStep = [this](int units) { _stepBy(units); };   // hold up/down → accelerating repeat
        m_repeat.timer.onTick.connect([this] { m_repeat.tick(); });
    }

    // Press-and-hold auto-repeat tuning (delay, interval, accel-onset, accel-rate, max step) — all ms but maxStep.
    void setRepeatConfig(int delayMs, int intervalMs, int accelAfterMs, int accelEveryMs, int maxStep) {
        m_repeat.configure(delayMs, intervalMs, accelAfterMs, accelEveryMs, maxStep);
    }

    void setValue(double v) {
        double c = std::clamp(v, m_min, m_max);
        if (m_value == c) return;
        m_value = c;
        // A value pushed from elsewhere (a data source, a binding) refreshes the text ONLY while the user has
        // not typed into it — an in-progress edit is never overwritten.
        if (m_field.active() && !m_field.dirty()) m_field.reseed(_numberText());   // fresh value, selected
        m_graph.invalidateNode(m_nodeId, DirtySelf); onValueChanged.emit(c); notifyAccessibility();
    }
    double value() const { return m_value; }

    JA11yNode a11yNode() const override {
        JA11yNode n; _a11yFillCommon(n, JA11yRole::SpinBox, m_debugName, _formatValue());
        n.hasRange = true; n.curValue = (float)m_value; n.minValue = (float)m_min; n.maxValue = (float)m_max;
        n.stateFlags |= JA11yEditable;
        return n;
    }

    void setRange(double min, double max) { m_min = min; m_max = max; m_field.setGrammar(m_decimals > 0, m_min < 0.0); setValue(m_value); }
    void setStep(double step)             { m_step = step; }
    void setDecimals(int decimals)        { m_decimals = decimals; m_field.setGrammar(decimals > 0, m_min < 0.0); m_graph.invalidateNode(m_nodeId, DirtySelf); }
    void setSuffix(const std::string& s)  { m_suffix = s; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    const std::string& suffix() const     { return m_suffix; }

    // The field's selection model, exposed like JLineEdit's so a host (properties panel, canvas) can act on it.
    bool        hasSelection() const { return m_field.core().hasSelection(); }
    std::string selectedText() const { return m_field.core().selectedText(); }
    const std::string& text() const  { return m_field.text(); }   // what the field is showing/editing
    void        selectAll()          { m_field.core().selectAll(); invalidate(); }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) { _commitEdit(); return; }
        requestFocus();   // clicking the box focuses it, so typed digits route here (framework focus)
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float btnW = b.height * 0.7f;
        if (mx >= b.x + b.width - btnW) {   // an arrow — commit any edit, then step
            const int dir = (my < b.y + b.height * 0.5f) ? 1 : -1;
            _stepBy(dir);                       // first click steps once; holding then auto-repeats with acceleration
            m_repeat.begin(dir);
        } else {                            // the value field — put a caret where the click landed
            setState(JWidgetState::Pressed);
            m_field.begin(_numberText(), /*selectAll=*/false);
            m_field.press(mx, _textX());
            invalidate();
        }
    }

    void handleMouseMove(float mx, float my) override {
        JControl::handleMouseMove(mx, my);
        if (m_field.drag(mx, _textX())) invalidate();   // drag-select inside the value
    }

    void handleMouseRelease(float mx, float my) override {
        m_repeat.end();   // releasing the button stops the auto-repeat
        m_field.release();
        JControl::handleMouseRelease(mx, my);
    }

    bool handleScroll(float mx, float my, float wheel) override {
        // Only the FOCUSED spin box takes the wheel — so a box embedded in a scroll area (e.g. the properties
        // panel) adjusts its value when focused, and lets the scroll area scroll when it isn't. Hover alone
        // never steals the wheel from the surrounding list.
        (void)mx; (void)my;
        if (!isFocused()) return false;
        _stepBy(wheel > 0.0f ? 1 : -1);
        return true;
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed) return false;
        // The text field gets first refusal: caret/selection movement, Backspace/Delete, Ctrl+A/C/X/V and any
        // character that can build a number are EDITS, not commands. Up/Down/Return/Escape come back as an
        // Outcome for this box to apply, since only it knows its step, range and revert value.
        if (!m_field.active() && !ke.ctrl && !ke.alt && m_field.wouldAccept(ke.utf8[0]))
            m_field.begin(_numberText(), /*selectAll=*/true);   // type-to-edit replaces the value
        const auto out = m_field.handleKey(ke);
        if (out.step != 0) { _stepBy(out.step); return true; }
        // Return interprets the text and keeps the field live with the result selected (Qt's behaviour), so the
        // box stays editable while it still holds focus. A canvas-hosted control is NoFocus and nothing will
        // blur it, so there Return closes the edit instead of leaving an orphaned caret.
        if (out.commit)    { _commitEdit(/*keepField=*/isFocused()); return true; }
        if (out.revert) {
            // Restore the value focus arrived with — reverts wheel/arrow/typed changes. If nothing changed, let
            // Escape bubble so it can still close a dialog.
            if (m_field.dirty() || m_value != m_focusValue) {
                setValue(m_focusValue);
                m_field.reseed(_numberText());
                invalidate();
                return true;
            }
            return false;
        }
        if (out.consumed) { if (out.changed) notifyAccessibility(); invalidate(); return true; }

        // App-defined value-nudge bindings (e.g. "." = increase) apply only to keys the field did not take, so
        // a bound "." still serves as this box's decimal point while editing.
        switch (valueKeyAction(ke)) {
            case JValueKeyAction::Increase:      _stepBy(1);   return true;
            case JValueKeyAction::Decrease:      _stepBy(-1);  return true;
            case JValueKeyAction::IncreaseLarge: _stepBy(10);  return true;
            case JValueKeyAction::DecreaseLarge: _stepBy(-10); return true;
            case JValueKeyAction::None:          break;
        }
        return false;
    }

    // Commit a typed value the instant focus leaves (Tab / click-away), before any repaint or
    // properties-panel rebuild can discard the edit buffer.
    void onFocusEvent(bool focused) override {
        if (focused) {
            m_focusValue = m_value;
            // Focus makes the field editable with the value already in it and selected — Tab in, type, Tab out
            // replaces the value; click in, and the press places the caret instead.
            m_field.begin(_numberText(), /*selectAll=*/true);
        } else {
            _commitEdit();
        }
        invalidate();
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float btnW = b.height * 0.7f;
        float fieldW = b.width - btnW;

        bool focused = isFocused();
        // Field surface + border by role; focus takes the Accent ring.
        // Focused OR editing takes the Accent ring: a canvas-hosted control is NoFocus by design, so the
        // ring is what tells you the field is taking your keystrokes.
        const bool ring = focused || m_field.active();
        JStyleOption o = jstyle::option(m_state, ring);
        // Value field
        buf.pushRectangle(b.x, b.y, fieldW, b.height, jstyle::fieldFill(o).data(),
                          JStyle::current().hint(JStyleHint::ControlRadius),
                          jstyle::borderW(ring), jstyle::border(o).data());
        // Value text, caret and selection — the field owns all of it.
        if (!m_field.active()) m_field.syncDisplay(_numberText());
        m_field.draw(buf, {b.x, b.y, fieldW, b.height}, o, textPadding(), m_suffix);

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

    // Where the value text starts on screen — the field hit-tests against exactly what draw() rendered.
    float _textX() const { return m_graph.getLayoutConst(m_nodeId).boundingBox.x + textPadding(); }

    // The value as text: _numberText() is what the field edits (no suffix); _formatValue() adds the suffix for
    // accessibility and any caller that wants the whole label.
    std::string _numberText() const {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", m_decimals, m_value);
        return std::string(buf);
    }
    std::string _formatValue() const { return _numberText() + m_suffix; }

    // Step the value: commit whatever is typed first (so stepping from an edited number starts from it), then
    // re-seed the field with the result, selected, ready to be replaced.
    void _stepBy(int units) {
        _commitEdit(/*keepField=*/true);
        setValue(m_value + units * m_step);
        if (m_field.active()) m_field.reseed(_numberText());
        invalidate();
    }

    // Parse the typed text into the value. An empty or sign-only field means "no number" and keeps the current
    // value rather than collapsing it to zero.
    void _commitEdit(bool keepField = false) {
        if (!m_field.active()) return;
        if (m_field.dirty()) {
            try { setValue(std::stod(m_field.text())); } catch (...) {}
        }
        if (keepField) m_field.reseed(_numberText());
        else           m_field.end();
        invalidate();
    }

    double      m_value, m_min, m_max, m_step;
    double      m_focusValue{0.0};   // value captured on focus-in; Escape restores it
    int         m_decimals;
    std::string m_suffix;
    JNumericField m_field;   // the real text field: caret, selection, clipboard, numeric grammar
};

} // inline namespace jf
