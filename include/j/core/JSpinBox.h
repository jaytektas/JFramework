#pragma once

// JSpinBox — integer spin box: a JLineEdit plus two small stepper buttons.
//
// The integer twin of JDoubleSpinBox (see that header for the reasoning). The value field is a real JLineEdit,
// adopted as a child, so caret placement, selection, drag-select, Home/End, Backspace/Delete and Ctrl+A/C/X/V
// all come from the one text implementation the toolkit already has. A JIntValidator carrying this box's range
// keeps the field to digits and a sign — no decimal point, because the value is an int.

#include "JControl.h"
#include "FocusManager.h"
#include "JLineEdit.h"
#include "JTextHelper.h"
#include "KeyEvent.h"
#include "SpinRepeat.h"
#include "Validator.h"

inline namespace jf {

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

        m_edit = adopt(std::make_unique<JLineEdit>(m_graph, "", w, l.boundingBox.height));
        m_edit->setFocusPolicy(JFocusPolicy::NoFocus);   // the SPIN BOX is the tab stop; this is its field
        m_validator.setRange(minVal, maxVal);
        m_edit->setValidator(&m_validator);              // digits, and a '-' only when the range allows it
        m_edit->setText(std::to_string(m_value));
        m_edit->onReturnPressed.connect([this] { _commitText(); });
        // Only text the USER typed may be committed. Without this, blurring a box whose value was changed by
        // its data source while focused would push the stale displayed text straight back over the new value.
        m_edit->onTextChanged.connect([this](const std::string&) { if (!m_pushing) m_dirty = true; });

        m_repeat.onStep = [this](int units) { _stepBy(units); };   // hold a stepper → accelerating repeat
        m_repeat.timer.onTick.connect([this] { m_repeat.tick(); });
    }

    // Auto-repeat tuning (press-and-hold on a stepper): initial delay, repeat interval, when the step starts
    // accelerating, how fast it grows, and the max step — all in ms except maxStep.
    void setRepeatConfig(int delayMs, int intervalMs, int accelAfterMs, int accelEveryMs, int maxStep) {
        m_repeat.configure(delayMs, intervalMs, accelAfterMs, accelEveryMs, maxStep);
    }

    void setValue(int v) {
        int c = std::clamp(v, m_min, m_max);
        if (m_value == c) return;
        m_value = c;
        // The text follows the value unless the user has TYPED something not yet committed (see JDoubleSpinBox):
        // holding focus alone must not stop a value changed underneath the box from being shown.
        if (!_editing() || !m_dirty) _setText(std::to_string(m_value));
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        onValueChanged.emit(c);
        notifyAccessibility();
    }
    int  value() const { return m_value; }

    JA11yNode a11yNode() const override {
        JA11yNode n; _a11yFillCommon(n, JA11yRole::SpinBox, m_debugName, std::to_string(m_value));
        n.hasRange = true; n.curValue = (float)m_value; n.minValue = (float)m_min; n.maxValue = (float)m_max;
        n.stateFlags |= JA11yEditable;
        return n;
    }

    void setRange(int minVal, int maxVal) {
        m_min = minVal; m_max = maxVal;
        m_validator.setRange(minVal, maxVal);
        setValue(m_value);                               // re-clamp
    }

    // The field, for a host that wants the text or the selection.
    JLineEdit*         lineEdit()           { return m_edit; }
    const std::string& text() const         { return m_edit->text(); }
    bool               hasSelection() const { return m_edit->hasSelection(); }
    std::string        selectedText() const { return m_edit->selectedText(); }
    void               selectAll()          { m_edit->selectAll(); invalidate(); }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) { endEdit(); return; }
        requestFocus();
        _layout();
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x + b.width - _btnW()) {             // a stepper — fires on press, then auto-repeats
            _commitText();
            const int dir = (my < b.y + b.height * 0.5f) ? 1 : -1;
            _stepBy(dir);
            m_repeat.begin(dir);
        } else {                                         // the field — the line edit places the caret
            setState(JWidgetState::Pressed);
            _enterEdit();
            m_edit->handleMousePress(mx, my);
            invalidate();
        }
    }

    void handleMouseMove(float mx, float my) override {
        JControl::handleMouseMove(mx, my);
        _layout();
        m_edit->handleMouseMove(mx, my);                 // drag-select inside the value
    }

    void handleMouseRelease(float mx, float my) override {
        m_repeat.end();                                  // releasing a stepper stops the auto-repeat
        _layout();
        m_edit->handleMouseRelease(mx, my);
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
        using K = JKeyEvent::JKey;

        if (ke.key == K::Up)   { _stepBy(1);  return true; }
        if (ke.key == K::Down) { _stepBy(-1); return true; }
        if (ke.key == K::Escape) {
            // Restore the value focus arrived with. If nothing changed, let Escape bubble (it can still close a
            // dialog).
            if (m_value != m_focusValue || m_edit->text() != std::to_string(m_value)) {
                setValue(m_focusValue);
                _setText(std::to_string(m_value));
                m_edit->selectAll();
                invalidate();
                return true;
            }
            return false;
        }

        // Give the field the keystroke: caret/selection movement, Backspace/Delete, Ctrl+A/C/X/V and any digit.
        // A printable the grammar cannot use is NOT offered, so a host's own shortcuts keep working.
        const char c = ke.utf8[0];
        const bool printable = !ke.ctrl && !ke.alt && (uint8_t)c >= 32;
        if (!printable || _isValueChar(c)) {
            if (m_edit->handleKeyEvent(ke)) {
                _enterEdit();                            // typing into an unfocused box still shows its caret
                notifyAccessibility();
                invalidate();
                return true;
            }
        }

        // App-defined value-nudge bindings (e.g. "." = increase) apply only when the field is NOT being edited.
        // An active edit owns the keyboard: Tab must leave the field, not step the value, and "." must type a
        // decimal point. Keying this on the EDIT rather than on focus matters for a host whose controls can
        // never hold framework focus (the studio canvas) -- there, gating on focus let a Tab bound to
        // IncreaseValue swallow every Tab press.
        if (!_editing()) {
            switch (valueKeyAction(ke)) {
                case JValueKeyAction::Increase:      _stepBy(1);   return true;
                case JValueKeyAction::Decrease:      _stepBy(-1);  return true;
                case JValueKeyAction::IncreaseLarge: _stepBy(10);  return true;
                case JValueKeyAction::DecreaseLarge: _stepBy(-10); return true;
                case JValueKeyAction::None:          break;
            }
        }
        return false;
    }

    // Focus arrives: the field becomes editable with the value in it, selected. Focus leaves: commit at once,
    // before any repaint or properties-panel rebuild can discard the text.
    void onFocusEvent(bool focused) override {
        if (focused) {
            beginEdit();
        } else if (JFocusManager::reachable(this)) {
            endEdit();          // a real blur: the user moved focus elsewhere
        }
        // Otherwise this is focus BOOKKEEPING, not a user action: a host that drives its own controls (the
        // studio canvas) keeps them out of the focus chain, so syncOrder() clears their focus a frame after
        // the click that set it. Tearing the edit down there is exactly what left a clicked spin box with no
        // caret. The host ends the edit explicitly instead, via endEdit().
        invalidate();
    }

    // Show the caret with the value selected, as tabbing into a field does. Called by the framework on
    // focus-in, and by a host that drives its own focus notion (the studio canvas) when its keyboard moves here.
    void beginEdit() override {
        m_focusValue = m_value;
        _enterEdit();
        _setText(std::to_string(m_value));
        m_edit->selectAll();
        invalidate();
    }

    // Commit and drop the caret. Called by the framework on a real blur, and by a host that drives its own
    // controls when its focused control moves on.
    void endEdit() override { _commitText(); _leaveEdit(); }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        _layout();
        m_edit->populateRenderPrimitives(buf);           // field surface, border, focus ring, text, caret

        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const float btnW = _btnW();
        JStyleOption o = jstyle::option(m_state, isFocused() || _editing());

        // The two steppers — Button role fill, Border-role outline, tiny arrow marks.
        const float halfH = b.height * 0.5f;
        const JColor btnFill = jstyle::role(JColorRole::Button, o);
        const JColor btnBd   = jstyle::role(JColorRole::Border, o);
        buf.pushRectangle(b.x + b.width - btnW, b.y,         btnW, halfH, btnFill.data(), 0.0f, 1.0f, btnBd.data());
        buf.pushRectangle(b.x + b.width - btnW, b.y + halfH, btnW, halfH, btnFill.data(), 0.0f, 1.0f, btnBd.data());
        const float ax = b.x + b.width - btnW + btnW * 0.3f, aw = btnW * 0.4f;
        uint8_t ac[4] = {Colors::MutedText[0], Colors::MutedText[1], Colors::MutedText[2], 200};
        buf.pushRectangle(ax, b.y + halfH * 0.35f,          aw, 2.0f, ac);   // up mark
        buf.pushRectangle(ax, b.y + halfH + halfH * 0.55f,  aw, 2.0f, ac);   // down mark
    }

private:
    SpinRepeat m_repeat;   // press-and-hold stepper auto-repeat with acceleration

    float _btnW() const { return m_graph.getLayoutConst(m_nodeId).boundingBox.height * 0.7f; }
    // The field fills everything left of the steppers. Run before any hit-test as well as every paint, so a
    // click maps to exactly the text the user can see even on the frame the box was moved.
    void _layout() {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        m_edit->setBounds({ b.x, b.y, std::max(8.0f, b.width - _btnW()), b.height });
    }

    // No decimal point: the value is an int. The validator has the final say.
    bool _isValueChar(char c) const {
        if (c >= '0' && c <= '9') return true;
        if (c == '-' || c == '+') return m_min < 0;
        return false;
    }

    // Push text into the field ourselves: not a user edit, so it clears the dirty flag rather than setting it.
    void _setText(const std::string& t) {
        m_pushing = true;
        m_edit->setText(t);
        m_pushing = false;
        m_dirty = false;
    }

    bool _editing() const { return m_editing; }
    void _enterEdit() { if (!m_editing) { m_editing = true; m_edit->setFocused(true); invalidate(); } }
    void _leaveEdit() { if (m_editing) { m_editing = false; m_edit->setFocused(false); invalidate(); } }

    // Parse the field into the value, then normalise the text. The caret goes to the END with no selection:
    // after Return you are still in the field, and re-selecting the value would let the next keystroke wipe it.
    void _commitText() {
        const std::string t = m_edit->text();
        if (m_dirty && !t.empty() && t != "-" && t != "+") {
            try { setValue(std::stoi(t)); } catch (...) {}      // out-of-range text was already refused as you typed
        }
        _setText(std::to_string(m_value));
        m_edit->clearSelection();
        invalidate();
    }

    void _stepBy(int units) {
        _commitText();
        setValue(m_value + units);
        _setText(std::to_string(m_value));
        m_edit->clearSelection();
        invalidate();
    }

    bool m_editing{false};   // the field is showing a caret and owns its text (OUR state, see _enterEdit)
    bool m_pushing{false};   // true while WE are setting the text (suppresses the dirty flag)
    bool m_dirty{false};     // the user has typed into the field since it was last seeded

    JLineEdit*    m_edit{nullptr};   // adopted: the value field. All text behaviour lives here.
    JIntValidator m_validator{0, 100};

    int m_min, m_max, m_value;
    int m_focusValue{0};   // value captured on focus-in; Escape restores it
};

} // inline namespace jf
