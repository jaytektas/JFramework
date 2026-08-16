#pragma once

// JLineEdit — a single-line text field: chrome (box/border/focus ring), horizontal scroll, echo modes,
// a validator, and rendering. The EDITING itself (buffer, caret, selection, word/char nav, clipboard, the
// key map) lives in the shared JTextEditCore, so JLineEdit, the canvas caption editor, and the tree rename
// all behave identically and improve together.

#include "JControl.h"
#include "JTextHelper.h"
#include "JTextEditCore.h"
#include "KeyEvent.h"
#include "Validator.h"
#include "../graphics/VectorGraphics.h"   // JVectorCanvas — the clear button's ✕

inline namespace jf {

// ============================================================================
// JLineEdit
// ============================================================================

class JLineEdit : public JControl {
public:
    jf::JSignal<std::string> onTextChanged;
    jf::JSignal<>            onReturnPressed;

    JLineEdit(JSceneGraph& graph, const std::string& placeholder = "",
             float w = 280.0f, float h = 0.0f)
        : JControl(graph, "JLineEdit"), m_placeholder(placeholder)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().controlHeight;
        l.minWidth = 60.0f;
        l.minHeight = h;
        // The core consults this to reject a keystroke/paste that a validator would make Invalid.
        m_core.setAcceptFn([this](const std::string& cand) { return _acceptsText(cand); });
    }

    void setText(const std::string& t) {
        if (m_core.text() != t) {
            m_core.setText(t);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onTextChanged.emit(m_core.text());
            notifyAccessibility();
        }
    }
    const std::string& text()        const { return m_core.text(); }
    const std::string& placeholder() const { return m_placeholder; }

    // ---- Clear button ----------------------------------------------------------------------------------
    // An ✕ inside the right edge that empties the field in one click, shown only while there is text to
    // clear. Opt-in, because it only makes sense where the text is a QUERY you discard (a search/filter
    // box) rather than a value you are authoring. Clicking it emits onTextChanged like any other edit, so
    // a filter wired to that signal resets itself.
    void setClearButtonEnabled(bool on) { m_clearButton = on; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    bool isClearButtonEnabled() const { return m_clearButton; }
    // Visible only when it would DO something: enabled, non-empty, and the field is actually editable.
    bool clearButtonVisible() const { return m_clearButton && !m_core.text().empty() && !m_core.isReadOnly(); }
    void setPlaceholderText(const std::string& p) { m_placeholder = p; m_graph.invalidateNode(m_nodeId, DirtySelf); }

    // Which edge the text sits against when it is SHORTER than the field. A number belongs on the right —
    // a column of them lines up on the decimal point, and a field that shows a value left-aligned where
    // the same value is drawn right-aligned everywhere else makes the number appear to jump when you
    // click it. Overflowing text ignores this and scrolls with the caret, as it must.
    enum Alignment { AlignLeft, AlignRight };
    void      setAlignment(Alignment a) { if (m_align == a) return; m_align = a; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    Alignment alignment() const { return m_align; }

    // ---- Echo mode -------------------------------------------------------------------------------------
    // Normal / Password (bullets) / NoEcho (blank) / PasswordEchoOnEdit (plain while focused).
    enum EchoMode { Normal, Password, NoEcho, PasswordEchoOnEdit };
    void     setEchoMode(EchoMode m) { m_echo = m; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    EchoMode echoMode() const { return m_echo; }
    std::string displayText() const { return _echo(m_core.text()); }

    JA11yNode a11yNode() const override {
        const bool masked = _masked();
        const std::string name = !m_placeholder.empty() ? m_placeholder : m_debugName;
        JA11yNode n; _a11yFillCommon(n, JA11yRole::TextField, name, masked ? "" : m_core.text());
        n.stateFlags |= JA11yEditable;
        if (masked)                 n.stateFlags |= JA11yProtected;
        if (m_core.isReadOnly())    n.stateFlags |= JA11yReadOnly;
        return n;
    }

    // ---- Length limit / read-only / validator ----------------------------------------------------------
    void   setMaxLength(int n) { m_core.setMaxLength(n < 0 ? 0 : (size_t)n); m_graph.invalidateNode(m_nodeId, DirtySelf); }
    int    maxLength() const { return (int)m_core.maxLength(); }
    void   setReadOnly(bool ro) { m_core.setReadOnly(ro); }
    bool   isReadOnly() const { return m_core.isReadOnly(); }
    void         setValidator(JValidator* v) { m_validator = v; m_committed = m_core.text(); }
    JValidator*  validator() const { return m_validator; }
    void commit() { _enforceValidatorOnCommit(); }

    // ---- Selection model (delegated to the core) -------------------------------------------------------
    bool        hasSelection() const { return m_core.hasSelection(); }
    size_t      selectionStart() const { return m_core.selectionStart(); }
    size_t      selectionEnd()   const { return m_core.selectionEnd(); }
    std::string selectedText()   const { return m_core.selectedText(); }
    void selectAll()      { m_core.selectAll();      m_graph.invalidateNode(m_nodeId, DirtySelf); }
    void clearSelection() { m_core.clearSelection(); m_graph.invalidateNode(m_nodeId, DirtySelf); }
    size_t caret() const { return m_core.caret(); }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) return;
        requestFocus();
        // The ✕ is a button, not text: a press on it clears and consumes, so it never also places a caret
        // or starts a drag-selection in the text it just removed.
        if (clearButtonVisible() && _inClearButton(mx, my)) {
            setText("");
            m_scrollX = 0.0f;
            m_selecting = false;
            return;
        }
        onClicked.emit();
        // Multi-click detection (single → caret + drag-select, double → word, triple → all). Timing lives
        // here (widget), the selection ops in the core.
        const auto now  = std::chrono::steady_clock::now();
        const bool nearby = std::abs(mx - m_lastClickX) < 4.0f;
        const bool quick = m_clickCount > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastClick).count() < 400;
        m_clickCount = (quick && nearby) ? m_clickCount + 1 : 1;
        m_lastClick  = now;
        m_lastClickX = mx;

        const size_t idx = _caretFromX(mx);
        if (m_clickCount >= 3)      { m_core.selectAll();      m_selecting = false; }   // triple → whole field
        else if (m_clickCount == 2) { m_core.selectWordAt(idx); m_selecting = false; }  // double → word
        else                        { m_core.setCaret(idx, false); m_selecting = true; } // single → caret + drag
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    void handleMouseMove(float mx, float my) override {
        JControl::handleMouseMove(mx, my);
        const bool over = clearButtonVisible() && _inClearButton(mx, my);
        if (over != m_clearHover) { m_clearHover = over; m_graph.invalidateNode(m_nodeId, DirtySelf); }
        if (m_selecting) { m_core.setCaret(_caretFromX(mx), /*extend=*/true); m_graph.invalidateNode(m_nodeId, DirtySelf); }
    }
    void handleMouseRelease(float mx, float my) override {
        m_selecting = false;
        JControl::handleMouseRelease(mx, my);
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed) return false;
        m_core.setCopyEnabled(!_masked());   // masked field: don't lift the text to the clipboard
        const auto res = m_core.handleKey(ke);
        if (res.returnPressed) { _enforceValidatorOnCommit(); onReturnPressed.emit(); return true; }
        if (res.changed)       { m_graph.invalidateNode(m_nodeId, DirtySelf); onTextChanged.emit(m_core.text()); notifyAccessibility(); }
        else if (res.consumed) { m_graph.invalidateNode(m_nodeId, DirtySelf); }   // caret / selection moved
        return res.consumed;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        bool focused = isFocused();
        if (m_wasFocused && !focused) _enforceValidatorOnCommit();   // focus-out commits (clamp/revert)
        m_wasFocused = focused;
        JStyleOption o = jstyle::option(m_state, focused);

        buf.pushRectangle(b.x, b.y, b.width, b.height, jstyle::fieldFill(o).data(),
                          JStyle::current().hint(JStyleHint::ControlRadius),
                          jstyle::borderW(focused), jstyle::border(o).data());

        const float pad = textPadding();
        float innerX = b.x + pad;
        float innerW = b.width - 2.0f * pad;
        float midY   = b.y + (b.height - 7.0f) * 0.5f;
        // Give the ✕ its own strip: the text run is clipped to innerW, so without this the last characters
        // would slide under the glyph and be unreadable at exactly the moment you want to read them.
        if (clearButtonVisible()) innerW -= _clearBoxSize() + 4.0f;

        const std::string& raw = m_core.text();
        const std::string disp = _echo(raw);

        // Horizontal scroll so the caret stays visible (single-line behaviour).
        const size_t caret = m_core.caret() > raw.size() ? raw.size() : m_core.caret();
        const float caretW = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(_echo(raw.substr(0, caret))) : 0.0f;
        const float fullW  = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(disp) : 0.0f;
        if (caretW - m_scrollX > innerW) m_scrollX = caretW - innerW;
        if (caretW - m_scrollX < 0.0f)   m_scrollX = caretW;
        if (m_scrollX > fullW - innerW)  m_scrollX = fullW - innerW;
        if (m_scrollX < 0.0f)            m_scrollX = 0.0f;
        const float ox = _textOriginX();

        buf.pushClip(innerX, b.y, innerW, b.height);

        if (m_core.hasSelection() && JTextHelper::hasAtlas() && !disp.empty()) {
            const float xLo = ox + JTextHelper::measureWidth(_echo(raw.substr(0, m_core.selectionStart())));
            const float xHi = ox + JTextHelper::measureWidth(_echo(raw.substr(0, m_core.selectionEnd())));
            const JColor sc = withAlpha(jstyle::role(JColorRole::Highlight, o), 90);
            buf.pushRectangle(xLo, b.y + 4.0f, std::max(1.0f, xHi - xLo), b.height - 8.0f, sc.data(), 2.0f);
        }

        if (JTextHelper::hasAtlas()) {
            float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            if (raw.empty()) {
                uint8_t pc[4] = {Colors::FieldPlaceholder[0], Colors::FieldPlaceholder[1], Colors::FieldPlaceholder[2], 160};
                const float px = (m_align == AlignRight)
                               ? innerX + innerW - JTextHelper::measureWidth(m_placeholder) : innerX;
                JTextHelper::pushText(buf, px, ty, m_placeholder, pc, innerW);
            } else {
                uint8_t tc[4] = {Colors::ControlText[0], Colors::ControlText[1], Colors::ControlText[2], 220};
                JTextHelper::pushText(buf, ox, ty, disp, tc, 0.0f);
            }
        } else {
            if (raw.empty()) {
                uint8_t pc[4] = {Colors::FieldPlaceholder[0], Colors::FieldPlaceholder[1], Colors::FieldPlaceholder[2], 120};
                buf.pushRectangle(innerX, midY, innerW * 0.55f, 7.0f, pc, 2.0f);
            } else {
                uint8_t tc[4] = {Colors::ControlText[0], Colors::ControlText[1], Colors::ControlText[2], 200};
                buf.pushRectangle(innerX, midY, innerW * 0.65f, 7.0f, tc, 2.0f);
            }
        }

        if (focused) {
            const float cx = JTextHelper::hasAtlas() ? ox + caretW
                                                     : innerX + innerW * 0.65f * (float)caret / (float)std::max<size_t>(1, raw.size());
            buf.pushRectangle(cx, b.y + 6.0f, 1.5f, b.height - 12.0f, jstyle::role(JColorRole::Accent, o).data());
        }

        buf.popClip();

        // Drawn AFTER popClip: the ✕ sits in the strip reserved above, outside the text clip.
        if (clearButtonVisible()) _drawClearButton(buf, o);
    }


private:
    // ---- Clear button geometry / paint -----------------------------------------------------------------
    float _clearBoxSize() const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        return std::clamp(b.height - 8.0f, 8.0f, 14.0f);
    }
    void _clearBoxRect(float& x, float& y, float& s) const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        s = _clearBoxSize();
        x = b.x + b.width - textPadding() - s;
        y = b.y + (b.height - s) * 0.5f;
    }
    // Hit area is padded beyond the glyph: a 10px ✕ is a hard target, and missing it places a caret instead
    // of clearing — the one outcome a clear button must never produce.
    bool _inClearButton(float mx, float my) const {
        float x, y, s; _clearBoxRect(x, y, s);
        const float g = 3.0f;
        return mx >= x - g && mx <= x + s + g && my >= y - g && my <= y + s + g;
    }
    void _drawClearButton(JPrimitiveBuffer& buf, const JStyleOption& o) {
        float x, y, s; _clearBoxRect(x, y, s);
        const float cx = x + s * 0.5f, cy = y + s * 0.5f, r = s * 0.28f;
        // Hover gives the glyph a disc behind it, so it reads as a pressable target rather than decoration.
        const JColor tint = jstyle::role(JColorRole::Text, o);
        if (m_clearHover)
            buf.pushRectangle(x - 1.0f, y - 1.0f, s + 2.0f, s + 2.0f,
                              withAlpha(tint, 38).data(), (s + 2.0f) * 0.5f);
        JVectorCanvas vc;
        vc.setAntiAlias(1.0f);
        const JPaint p{withAlpha(tint, m_clearHover ? 235 : 165)};
        vc.drawLine(cx - r, cy - r, cx + r, cy + r, 1.4f, p);
        vc.drawLine(cx + r, cy - r, cx - r, cy + r, 1.4f, p);
        vc.flush(buf);
    }

    // Nearest character boundary to a screen x — click + drag-select. Delegates the scan to the core with a
    // measure that applies the echo, so bullets and plain text hit-test correctly.
    // The text box inside the frame: the padded rect, less the ✕'s strip when one is shown.
    void _innerBox(float& innerX, float& innerW) const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const float pad = textPadding();
        innerX = b.x + pad;
        innerW = b.width - 2.0f * pad;
        if (clearButtonVisible()) innerW -= _clearBoxSize() + 4.0f;
    }

    // WHERE THE RUN STARTS. The paint and the caret hit-test must agree to the pixel — a click lands on the
    // character it looks like it landed on only because both ask this. Right-aligned text that OVERFLOWS
    // falls back to scrolling with the caret: there is no right edge to sit against once it does not fit.
    float _textOriginX() const {
        float innerX = 0.f, innerW = 0.f;
        _innerBox(innerX, innerW);
        if (m_align == AlignRight && JTextHelper::hasAtlas()) {
            const float fullW = JTextHelper::measureWidth(_echo(m_core.text()));
            if (fullW <= innerW) return innerX + innerW - fullW;
        }
        return innerX - m_scrollX;
    }

    size_t _caretFromX(float mx) const {
        if (!JTextHelper::hasAtlas() || m_core.text().empty()) return m_core.text().size();
        const float target = mx - _textOriginX();
        return m_core.caretAtX(target, [this](size_t end) {
            return JTextHelper::measureWidth(_echo(m_core.text().substr(0, end)));
        });
    }

    // ---- Echo / validator plumbing --------------------------------------------------------------------
    bool _masked() const {
        return m_echo == Password || m_echo == NoEcho ||
               (m_echo == PasswordEchoOnEdit && !isFocused());
    }
    static size_t _cpCount(const std::string& s) {
        size_t n = 0;
        for (size_t i = 0; i < s.size(); ++i)
            if ((static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) ++n;
        return n;
    }
    std::string _echo(const std::string& s) const {
        switch (m_echo) {
            case Normal: return s;
            case NoEcho: return std::string();
            case PasswordEchoOnEdit: if (isFocused()) return s; [[fallthrough]];
            case Password: default: break;
        }
        std::string out; out.reserve(_cpCount(s) * 3);
        for (size_t i = 0, n = _cpCount(s); i < n; ++i) out += "\xE2\x80\xA2";   // U+2022 BULLET
        return out;
    }
    bool _acceptsText(const std::string& cand) const {
        if (!m_validator) return true;
        int pos = (int)m_core.caret();
        return m_validator->validate(cand, pos) != JValidator::Invalid;
    }
    void _enforceValidatorOnCommit() {
        if (!m_validator) { m_committed = m_core.text(); return; }
        int pos = (int)m_core.caret();
        if (m_validator->validate(m_core.text(), pos) == JValidator::Acceptable) { m_committed = m_core.text(); return; }
        std::string t = m_core.text(); m_validator->fixup(t); pos = (int)t.size();
        if (m_validator->validate(t, pos) == JValidator::Acceptable) { setText(t); m_committed = t; }
        else { setText(m_committed); }
    }

    JTextEditCore m_core;              // the shared text-editing model (buffer/caret/selection/keys)
    std::string m_placeholder;
    float       m_scrollX = 0.f;
    Alignment   m_align{AlignLeft};   // which edge short text sits against — see setAlignment
    bool        m_selecting = false;
    bool        m_clearButton = false;   // opt-in ✕ (see setClearButtonEnabled)
    bool        m_clearHover  = false;
    int         m_clickCount = 0;
    std::chrono::steady_clock::time_point m_lastClick{};
    float       m_lastClickX = 0.f;
    EchoMode    m_echo{Normal};
    bool        m_wasFocused{false};
    JValidator* m_validator{nullptr};
    std::string m_committed;
};

} // inline namespace jf
