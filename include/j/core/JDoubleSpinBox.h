#pragma once

// JDoubleSpinBox — floating-point spin box: a JLineEdit plus two small stepper buttons.
//
// That IS the widget. The value field is a real JLineEdit, adopted as a child, so every text behaviour comes
// from the one implementation the rest of the toolkit uses: click places the caret, drag selects, double-click
// takes a word, triple-click the value, Home/End/arrows move, Backspace/Delete edit either side of the caret,
// Ctrl+A/C/X/V work. Nothing here re-implements editing, a caret, a selection or text rendering.
//
// What this class adds on top:
//   * the value — a double clamped to [min,max], stepped by `step`, formatted to `decimals` (+ optional
//     suffix). The text is the editing surface; m_value is the truth;
//   * the grammar — a JDoubleValidator on the line edit carrying the same range and decimals, so the field
//     takes digits, a decimal point only when the box has decimals, and a sign only when the range goes below
//     zero;
//   * the steppers — two half-height buttons that fire on PRESS and auto-repeat while held (a stepper is not a
//     JButton: a button fires on release-inside and never repeats);
//   * commit points — Return and focus-out parse the text into the value; Escape restores the value focus
//     arrived with. Typing does NOT emit onValueChanged per keystroke, so a bound data source sees one write
//     per committed value rather than one per digit.
//
// The line edit is NoFocus: the spin box is the single tab stop and mirrors its own focus onto the field, the
// way Qt gives QAbstractSpinBox a focus proxy. Because of that, a HOST that routes keys to an unfocused control
// (the studio canvas types into whatever was last clicked) still gets a caret and an editable field — see
// _enterEdit.

#include "JControl.h"
#include "FocusManager.h"
#include "JLineEdit.h"
#include "JTextHelper.h"
#include "KeyEvent.h"
#include "SpinRepeat.h"
#include "Validator.h"

inline namespace jf {

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

        m_edit = adopt(std::make_unique<JLineEdit>(m_graph, "", w, l.boundingBox.height));
        m_edit->setFocusPolicy(JFocusPolicy::NoFocus);   // the SPIN BOX is the tab stop; this is its field
        m_validator.setRange(min, max);
        m_validator.setDecimals(decimals);
        m_edit->setValidator(&m_validator);              // digits, one '.', a '-' only when the range allows it
        m_edit->setText(_numberText());
        m_edit->onReturnPressed.connect([this] { _commitText(); });
        // Only text the USER typed may be committed. Without this, blurring a box whose value was changed by
        // its data source while focused would push the stale displayed text straight back over the new value.
        m_edit->onTextChanged.connect([this](const std::string&) { if (!m_pushing) m_dirty = true; });

        m_repeat.onStep = [this](int units) { _stepBy(units); };   // hold a stepper → accelerating repeat
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
        // The text follows the value unless the user has TYPED something not yet committed — refreshing then
        // would wipe the caret and the half-typed number under their hands. Merely holding focus is not that:
        // a focused, untouched box that refuses to show a value changed underneath it (an undo, a re-read from
        // the device, a value the controller itself moved) is simply displaying something untrue.
        if (!_editing() || !m_dirty) _setText(_numberText());
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        onValueChanged.emit(c);
        notifyAccessibility();
    }
    double value() const { return m_value; }

    JA11yNode a11yNode() const override {
        JA11yNode n; _a11yFillCommon(n, JA11yRole::SpinBox, m_debugName, _formatValue());
        n.hasRange = true; n.curValue = (float)m_value; n.minValue = (float)m_min; n.maxValue = (float)m_max;
        n.stateFlags |= JA11yEditable;
        return n;
    }

    void setRange(double min, double max) {
        m_min = min; m_max = max;
        m_validator.setRange(min, max);
        setValue(m_value);                               // re-clamp
    }
    void setStep(double step) { m_step = step; }
    void setDecimals(int decimals) {
        if (m_decimals == decimals) return;
        m_decimals = decimals;
        m_validator.setDecimals(decimals);
        if (!_editing()) _setText(_numberText());
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    void setSuffix(const std::string& s) { m_suffix = s; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    const std::string& suffix() const    { return m_suffix; }

    // The field, for a host that wants the text or the selection (copying a value out of a properties panel).
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
            // Restore the value focus arrived with — reverts stepped, scrolled or typed changes. If nothing
            // changed, let Escape bubble so it can still close a dialog.
            if (m_value != m_focusValue || m_edit->text() != _numberText()) {
                setValue(m_focusValue);
                _setText(_numberText());
                m_edit->selectAll();
                invalidate();
                return true;
            }
            return false;
        }

        // Give the field the keystroke: caret/selection movement, Backspace/Delete, Ctrl+A/C/X/V and any
        // character the grammar can use. A printable it cannot use is NOT offered, so a host's own shortcuts
        // (letters over a canvas) keep working over a spin box.
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

    // Focus arrives: the field becomes editable with the value in it, selected, so Tab-in-and-type replaces it.
    // Focus leaves: commit immediately, before any repaint or properties-panel rebuild can discard the text.
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
        _setText(_numberText());
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

        if (!m_suffix.empty() && JTextHelper::hasAtlas()) {
            uint8_t sc[4] = {Colors::MutedText[0], Colors::MutedText[1], Colors::MutedText[2], 200};
            const float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            JTextHelper::pushText(buf, b.x + b.width - btnW - _suffixW() + 3.0f, ty, m_suffix, sc, _suffixW());
        }

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
    float _suffixW() const {
        if (m_suffix.empty() || !JTextHelper::hasAtlas()) return 0.0f;
        return JTextHelper::measureWidth(m_suffix) + 6.0f;
    }
    // The field fills everything left of the suffix and the steppers. Run before any hit-test as well as every
    // paint, so a click maps to exactly the text the user can see even on the frame the box was moved.
    // THE FRAME ENCLOSES THE NUMBER AND THE UNIT. The field used to stop short of the suffix, so a box read
    // "[400.0] RPM" with the unit stranded outside its own border — the number looked like the widget and the
    // unit like something next to it. The field now runs to the steppers and RESERVES the suffix's width, so
    // the text still cannot slide under the unit.
    void _layout() {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        m_edit->setRightInset(_suffixW());
        m_edit->setBounds({ b.x, b.y, std::max(8.0f, b.width - _btnW()), b.height });
    }

    // Only characters the numeric grammar can use are offered to the field; the validator has the final say.
    bool _isValueChar(char c) const {
        if (c >= '0' && c <= '9') return true;
        if (c == '.') return m_decimals > 0;
        if (c == '-' || c == '+') return m_min < 0.0;
        return false;
    }

    // "Editing" means the field shows a caret and owns its text. It follows our focus, but a host that routes
    // keys to an unfocused control (the studio canvas) turns it on by typing or clicking.
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

    std::string _numberText() const {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", m_decimals, m_value);
        return std::string(buf);
    }
    std::string _formatValue() const { return _numberText() + m_suffix; }

    // Parse the field into the value, then normalise the text to what was committed. The caret goes to the END
    // with no selection: after Return you are still in the field, and re-selecting the whole value would mean
    // the next keystroke silently wiped it.
    void _commitText() {
        const std::string t = m_edit->text();
        if (m_dirty && !t.empty() && t != "-" && t != "+" && t != ".") {
            try { setValue(std::stod(t)); } catch (...) {}      // out-of-range text was already refused as you typed
        }
        _setText(_numberText());
        m_edit->clearSelection();
        invalidate();
    }

    // Step the value: commit whatever is typed first (so stepping continues from an edited number), then show
    // the result, unselected, so a following keystroke edits it rather than replacing it.
    void _stepBy(int units) {
        _commitText();
        setValue(m_value + units * m_step);
        _setText(_numberText());
        m_edit->clearSelection();
        invalidate();
    }

    bool m_editing{false};   // the field is showing a caret and owns its text (OUR state, see _enterEdit)
    bool m_pushing{false};   // true while WE are setting the text (suppresses the dirty flag)
    bool m_dirty{false};     // the user has typed into the field since it was last seeded

    JLineEdit*       m_edit{nullptr};   // adopted: the value field. All text behaviour lives here.
    JDoubleValidator m_validator{-1e300, 1e300, 2};

    double      m_value, m_min, m_max, m_step;
    double      m_focusValue{0.0};   // value captured on focus-in; Escape restores it
    int         m_decimals;
    std::string m_suffix;
};

} // inline namespace jf
