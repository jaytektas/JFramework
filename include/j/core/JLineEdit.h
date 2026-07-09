#pragma once

// JLineEdit.

#include "JControl.h"
#include "JTextHelper.h"
#include "KeyEvent.h"
#include "Validator.h"

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
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().controlHeight;
        l.minWidth = 60.0f;
        l.minHeight = h;
    }

    void setText(const std::string& t) {
        if (m_text != t) {
            m_text = t;
            m_caret = m_anchor = m_text.size();               // caret to end + collapse selection on set
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onTextChanged.emit(t);
            notifyAccessibility();
        }
    }
    const std::string& text()        const { return m_text; }
    const std::string& placeholder() const { return m_placeholder; }
    void setPlaceholderText(const std::string& p) { m_placeholder = p; m_graph.invalidateNode(m_nodeId, DirtySelf); }

    // ---- Echo mode -------------------------------------------------------------------------------------
    // Normal              — show the text verbatim.
    // Password            — every character rendered as a bullet ('•').
    // NoEcho              — render nothing (blank field), text still held.
    // PasswordEchoOnEdit  — plain while focused (being edited), bullets otherwise.
    enum EchoMode { Normal, Password, NoEcho, PasswordEchoOnEdit };
    void     setEchoMode(EchoMode m) { m_echo = m; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    EchoMode echoMode() const { return m_echo; }
    // The string as it is drawn (post-echo). Exposed so callers/tests can see the masked form
    // without scraping the render buffer.
    std::string displayText() const { return _echo(m_text); }

    JA11yNode a11yNode() const override {
        // A masked field (Password/NoEcho) must never leak its text through the
        // accessibility value: report it Protected+Editable with an empty value.
        const bool masked = _masked();
        const std::string name = !m_placeholder.empty() ? m_placeholder : m_debugName;
        JA11yNode n; _a11yFillCommon(n, JA11yRole::TextField, name, masked ? "" : m_text);
        n.stateFlags |= JA11yEditable;
        if (masked)     n.stateFlags |= JA11yProtected;
        if (m_readOnly) n.stateFlags |= JA11yReadOnly;
        return n;
    }

    // ---- Length limit / read-only / validator ----------------------------------------------------------
    // 0 == unlimited. Truncates the current text and gates future inserts/pastes.
    void   setMaxLength(int n) { m_maxLength = n < 0 ? 0 : (size_t)n;
                                 if (m_maxLength && m_text.size() > m_maxLength) { m_text.resize(m_maxLength); m_caret = m_anchor = m_text.size(); m_graph.invalidateNode(m_nodeId, DirtySelf); } }
    int    maxLength() const { return (int)m_maxLength; }
    void   setReadOnly(bool ro) { m_readOnly = ro; }
    bool   isReadOnly() const { return m_readOnly; }
    // Non-owning. A keystroke that would make the text Invalid is rejected; on commit/focus-out a
    // non-Acceptable value is fixup()'d (clamped) and, failing that, reverted to the last acceptable text.
    void         setValidator(JValidator* v) { m_validator = v; m_committed = m_text; }
    JValidator*  validator() const { return m_validator; }
    // Force a commit-time validation now (also invoked on Return and focus-out).
    void commit() { _enforceValidatorOnCommit(); }

    // ---- Selection model -------------------------------------------------------------------------------
    // A selection is the byte range [anchor, caret) (either end may be the larger). anchor==caret ⇒ none.
    bool        hasSelection() const { return m_anchor != m_caret; }
    size_t      selectionStart() const { return std::min(m_anchor, m_caret); }
    size_t      selectionEnd()   const { return std::max(m_anchor, m_caret); }
    std::string selectedText()   const {
        return hasSelection() ? m_text.substr(selectionStart(), selectionEnd() - selectionStart()) : std::string();
    }
    void selectAll() { m_anchor = 0; m_caret = m_text.size(); m_graph.invalidateNode(m_nodeId, DirtySelf); }
    void clearSelection() { m_anchor = m_caret; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    size_t caret() const { return m_caret; }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) return;
        requestFocus();   // clicking the field focuses it, so typed characters route here (framework focus)
        onClicked.emit();
        // Multi-click detection (single → caret + drag-select, double → word, triple → all). Two presses count
        // as a double-click when they land close together in space and within 400 ms — the same test path the
        // platform's real click stream drives.
        const auto now  = std::chrono::steady_clock::now();
        const bool near = std::abs(mx - m_lastClickX) < 4.0f;
        const bool quick = m_clickCount > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastClick).count() < 400;
        m_clickCount = (quick && near) ? m_clickCount + 1 : 1;
        m_lastClick  = now;
        m_lastClickX = mx;

        const size_t idx = _caretFromX(mx);
        if (m_clickCount >= 3) {            // triple-click → select the whole field
            m_anchor = 0; m_caret = m_text.size(); m_selecting = false;
        } else if (m_clickCount == 2) {     // double-click → select the word (or whitespace run) under the cursor
            _selectWordAt(idx); m_selecting = false;
        } else {                            // single-click → place caret, begin a drag-select
            m_caret = idx; m_anchor = idx; m_selecting = true;
        }
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    void handleMouseMove(float mx, float my) override {
        JControl::handleMouseMove(mx, my);
        if (m_selecting) { m_caret = _caretFromX(mx); m_graph.invalidateNode(m_nodeId, DirtySelf); }   // extend, keep anchor
    }
    void handleMouseRelease(float mx, float my) override {
        m_selecting = false;
        JControl::handleMouseRelease(mx, my);
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed) return false;
        using K = JKeyEvent::JKey;
        if (m_caret  > m_text.size()) m_caret  = m_text.size();
        if (m_anchor > m_text.size()) m_anchor = m_text.size();
        auto changed = [&]{
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onTextChanged.emit(m_text);
        };
        auto moved = [&]{ m_graph.invalidateNode(m_nodeId, DirtySelf); };

        // ---- Clipboard + select-all (Ctrl). Match on both the JKey and the folded printable, since a
        //      Ctrl-chord may arrive as a named key or as a control-code utf8 byte depending on platform. ----
        if (ke.ctrl && !ke.alt) {
            const char lc = static_cast<char>(ke.utf8[0] | 0x20);   // case-fold the printable, if any
            if (ke.key == K::A || lc == 'a') { selectAll(); return true; }
            // Copy is disabled while masked so a password can't be lifted verbatim.
            if (ke.key == K::C || lc == 'c') { if (hasSelection() && !_masked()) clipboardSet(selectedText()); return true; }
            if (ke.key == K::X || lc == 'x') {
                if (m_readOnly) return true;
                if (hasSelection()) { if (!_masked()) clipboardSet(selectedText()); _deleteSelection(); changed(); }
                return true;
            }
            if (ke.key == K::V || lc == 'v') {
                if (m_readOnly) return true;
                std::string clip = clipboardGet();
                clip.erase(std::remove(clip.begin(), clip.end(), '\n'), clip.end());   // single-line field: drop newlines
                clip.erase(std::remove(clip.begin(), clip.end(), '\r'), clip.end());
                if (!clip.empty()) {
                    // Build the candidate (selection replaced by clip), enforce max length + validator.
                    std::string cand = m_text;
                    const size_t lo = selectionStart(), hi = selectionEnd();
                    cand.replace(lo, hi - lo, clip);
                    if (m_maxLength && cand.size() > m_maxLength) {          // trim the paste to fit
                        const size_t room = m_maxLength > (m_text.size() - (hi - lo)) ? m_maxLength - (m_text.size() - (hi - lo)) : 0;
                        clip.resize(std::min(clip.size(), room));
                        cand = m_text; cand.replace(lo, hi - lo, clip);
                    }
                    if (!clip.empty() && _acceptsText(cand)) {
                        _deleteSelection();                              // paste replaces the active selection
                        m_text.insert(m_caret, clip);
                        m_caret += clip.size();
                        m_anchor = m_caret;
                        changed();
                    }
                }
                return true;
            }
        }

        switch (ke.key) {
            case K::Backspace:
                if (m_readOnly) return true;
                if (hasSelection())      { _deleteSelection(); changed(); }
                else if (ke.ctrl)        { const size_t p = _prevWord(m_caret); if (p < m_caret) { m_text.erase(p, m_caret - p); m_caret = m_anchor = p; changed(); } }
                else if (m_caret > 0)    { const size_t p = _prevCharStart(m_caret); m_text.erase(p, m_caret - p); m_caret = m_anchor = p; changed(); }
                return true;
            case K::Delete:
                if (m_readOnly) return true;
                if (hasSelection())              { _deleteSelection(); changed(); }
                else if (ke.ctrl)                { const size_t e = _nextWord(m_caret); if (e > m_caret) { m_text.erase(m_caret, e - m_caret); m_anchor = m_caret; changed(); } }
                else if (m_caret < m_text.size()){ m_text.erase(m_caret, _nextCharStart(m_caret) - m_caret); m_anchor = m_caret; changed(); }
                return true;
            case K::Left: {
                size_t pos = ke.ctrl ? _prevWord(m_caret) : _prevCharStart(m_caret);
                if (!ke.shift && !ke.ctrl && hasSelection()) pos = selectionStart();   // plain arrow collapses to edge
                m_caret = pos; if (!ke.shift) m_anchor = pos; moved(); return true;
            }
            case K::Right: {
                size_t pos = ke.ctrl ? _nextWord(m_caret) : _nextCharStart(m_caret);
                if (!ke.shift && !ke.ctrl && hasSelection()) pos = selectionEnd();
                m_caret = pos; if (!ke.shift) m_anchor = pos; moved(); return true;
            }
            case K::Home: m_caret = 0;             if (!ke.shift) m_anchor = m_caret; moved(); return true;
            case K::End:  m_caret = m_text.size(); if (!ke.shift) m_anchor = m_caret; moved(); return true;
            case K::Return: _enforceValidatorOnCommit(); onReturnPressed.emit(); return true;
            default: break;
        }
        if (!ke.ctrl && !ke.alt && ke.utf8[0] != '\0' && static_cast<uint8_t>(ke.utf8[0]) >= 32) {   // printable → replace selection + insert
            if (m_readOnly) return true;
            const std::string ch = ke.utf8;
            // Build the resulting text and gate it on max length + validator (reject → consume, no change).
            std::string cand = m_text;
            const size_t lo = selectionStart(), hi = selectionEnd();
            cand.replace(lo, hi - lo, ch);
            if (m_maxLength && cand.size() > m_maxLength) return true;
            if (!_acceptsText(cand)) return true;
            _deleteSelection();
            m_text.insert(m_caret, ch);
            m_caret += ch.size();
            m_anchor = m_caret;
            changed();
            return true;
        }
        return false;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        bool focused = isFocused();
        // Losing focus commits: clamp/revert a non-Acceptable value via the validator.
        if (m_wasFocused && !focused) _enforceValidatorOnCommit();
        m_wasFocused = focused;
        JStyleOption o = jstyle::option(m_state, focused);

        // Background = Base role, border = Accent ring when focused else Border (by role).
        buf.pushRectangle(b.x, b.y, b.width, b.height, jstyle::fieldFill(o).data(),
                          JTheme::current().hint(JStyleHint::ControlRadius),
                          jstyle::borderW(focused), jstyle::border(o).data());

        const float pad = textPadding();
        float innerX = b.x + pad;
        float innerW = b.width - 2.0f * pad;
        float midY   = b.y + (b.height - 7.0f) * 0.5f;

        // Echo-aware display string (verbatim / bullets / blank). Selection + caret measure against it.
        const std::string disp = _echo(m_text);

        // Selection highlight — a translucent rectangle behind the selected glyph run (drawn before the text).
        if (hasSelection() && JTextHelper::hasAtlas() && !disp.empty()) {
            float xLo = innerX + JTextHelper::measureWidth(_echo(m_text.substr(0, selectionStart())));
            float xHi = innerX + JTextHelper::measureWidth(_echo(m_text.substr(0, selectionEnd())));
            if (xHi > innerX + innerW) xHi = innerX + innerW;
            if (xLo < innerX) xLo = innerX;
            const JColor sc = withAlpha(jstyle::role(JColorRole::Highlight, o), 90);   // Accent @ 90
            buf.pushRectangle(xLo, b.y + 4.0f, std::max(1.0f, xHi - xLo), b.height - 8.0f, sc.data(), 2.0f);
        }

        if (JTextHelper::hasAtlas()) {
            float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            if (m_text.empty()) {
                uint8_t pc[4] = {Colors::FieldPlaceholder[0], Colors::FieldPlaceholder[1], Colors::FieldPlaceholder[2], 160};
                JTextHelper::pushText(buf, innerX, ty, m_placeholder, pc, innerW);
            } else {
                uint8_t tc[4] = {Colors::ControlText[0], Colors::ControlText[1], Colors::ControlText[2], 220};
                JTextHelper::pushText(buf, innerX, ty, disp, tc, innerW);
            }
        } else {
            if (m_text.empty()) {
                uint8_t pc[4] = {Colors::FieldPlaceholder[0], Colors::FieldPlaceholder[1], Colors::FieldPlaceholder[2], 120};
                buf.pushRectangle(innerX, midY, innerW * 0.55f, 7.0f, pc, 2.0f);
            } else {
                uint8_t tc[4] = {Colors::ControlText[0], Colors::ControlText[1], Colors::ControlText[2], 200};
                buf.pushRectangle(innerX, midY, innerW * 0.65f, 7.0f, tc, 2.0f);
            }
        }

        // Caret at the actual insertion point (measured up to the caret index), not a fixed fraction.
        if (focused) {
            const size_t caret = m_caret > m_text.size() ? m_text.size() : m_caret;
            float cx = innerX;
            if (caret > 0)
                cx = innerX + (JTextHelper::hasAtlas() ? JTextHelper::measureWidth(_echo(m_text.substr(0, caret)))
                                                       : innerW * 0.65f * (float)caret / (float)std::max<size_t>(1, m_text.size()));
            if (cx > innerX + innerW) cx = innerX + innerW;   // clamp inside the field
            buf.pushRectangle(cx, b.y + 6.0f, 1.5f, b.height - 12.0f,
                              jstyle::role(JColorRole::Accent, o).data());
        }
    }


private:
    // UTF-8 char-boundary navigation: skip continuation bytes (0b10xxxxxx) so the caret lands on whole chars.
    size_t _nextCharStart(size_t i) const {
        if (i >= m_text.size()) return m_text.size();
        ++i;
        while (i < m_text.size() && (static_cast<uint8_t>(m_text[i]) & 0xC0) == 0x80) ++i;
        return i;
    }
    size_t _prevCharStart(size_t i) const {
        if (i == 0) return 0;
        --i;
        while (i > 0 && (static_cast<uint8_t>(m_text[i]) & 0xC0) == 0x80) --i;
        return i;
    }

    // A "word" character for navigation: ASCII alphanumerics, '_' and any UTF-8 lead/continuation byte
    // (so multibyte glyphs count as word content). Everything else (space, punctuation) is a separator.
    static bool _isWordChar(unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80;
    }
    // Start of the next word: skip the current word run, then the following separators (Qt/GTK semantics).
    size_t _nextWord(size_t i) const {
        const size_t n = m_text.size();
        while (i < n &&  _isWordChar(static_cast<unsigned char>(m_text[i]))) ++i;
        while (i < n && !_isWordChar(static_cast<unsigned char>(m_text[i]))) ++i;
        return i;
    }
    // Start of the current/previous word: skip separators to the left, then the word run.
    size_t _prevWord(size_t i) const {
        while (i > 0 && !_isWordChar(static_cast<unsigned char>(m_text[i - 1]))) --i;
        while (i > 0 &&  _isWordChar(static_cast<unsigned char>(m_text[i - 1]))) --i;
        return i;
    }
    // Select the contiguous run (word OR whitespace/punct) of the same class as the char at/under `i`.
    void _selectWordAt(size_t i) {
        const size_t n = m_text.size();
        if (n == 0) { m_anchor = m_caret = 0; return; }
        if (i >= n) i = n - 1;
        const bool w = _isWordChar(static_cast<unsigned char>(m_text[i]));
        size_t s = i, e = i;
        while (s > 0 && _isWordChar(static_cast<unsigned char>(m_text[s - 1])) == w) --s;
        while (e < n && _isWordChar(static_cast<unsigned char>(m_text[e]))     == w) ++e;
        m_anchor = s; m_caret = e;
    }
    // Nearest character boundary to a screen x — used by click and drag-select.
    size_t _caretFromX(float mx) const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const float innerX = b.x + textPadding();
        if (!JTextHelper::hasAtlas() || m_text.empty()) return m_text.size();
        const float target = mx - innerX;
        if (target <= 0.f) return 0;
        float prevW = 0.f;
        for (size_t i = 0; i < m_text.size(); ) {
            const size_t nxt = _nextCharStart(i);
            const float w = JTextHelper::measureWidth(m_text.substr(0, nxt));
            if (target < (prevW + w) * 0.5f) return i;
            prevW = w; i = nxt;
        }
        return m_text.size();
    }
    void _deleteSelection() {
        if (!hasSelection()) return;
        const size_t lo = selectionStart(), hi = selectionEnd();
        m_text.erase(lo, hi - lo);
        m_caret = m_anchor = lo;
    }

    // ---- Echo / validator plumbing --------------------------------------------------------------------
    // True when characters are currently masked (so copy is suppressed / caret metrics use bullets).
    bool _masked() const {
        return m_echo == Password || m_echo == NoEcho ||
               (m_echo == PasswordEchoOnEdit && !isFocused());
    }
    // Number of UTF-8 codepoints (glyph cells) in s.
    static size_t _cpCount(const std::string& s) {
        size_t n = 0;
        for (size_t i = 0; i < s.size(); ++i)
            if ((static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) ++n;
        return n;
    }
    // Map a plaintext run to its rendered form per the echo mode (one bullet per codepoint).
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
    // A candidate string is acceptable to type when there is no validator, or it is not Invalid.
    bool _acceptsText(const std::string& cand) const {
        if (!m_validator) return true;
        int pos = (int)m_caret;
        return m_validator->validate(cand, pos) != JValidator::Invalid;
    }
    // Commit-time gate: keep an Acceptable value, else fixup() (clamp) toward Acceptable, else revert.
    void _enforceValidatorOnCommit() {
        if (!m_validator) { m_committed = m_text; return; }
        int pos = (int)m_caret;
        if (m_validator->validate(m_text, pos) == JValidator::Acceptable) { m_committed = m_text; return; }
        std::string t = m_text; m_validator->fixup(t); pos = (int)t.size();
        if (m_validator->validate(t, pos) == JValidator::Acceptable) { setText(t); m_committed = t; }
        else { setText(m_committed); }   // unrecoverable → restore the last good value
    }

    std::string m_text;
    std::string m_placeholder;
    size_t      m_caret  = 0;   // insertion index into m_text (bytes; on a UTF-8 char boundary)
    size_t      m_anchor = 0;   // selection anchor (== m_caret ⇒ no selection)
    bool        m_selecting = false;                                 // mouse drag-select in progress
    int         m_clickCount = 0;                                    // 1/2/3 = single/double/triple within the window
    std::chrono::steady_clock::time_point m_lastClick{};
    float       m_lastClickX = 0.f;
    EchoMode    m_echo{Normal};
    size_t      m_maxLength{0};        // 0 = unlimited
    bool        m_readOnly{false};
    bool        m_wasFocused{false};   // focus-out edge → commit
    JValidator* m_validator{nullptr};  // non-owning
    std::string m_committed;           // last Acceptable text (revert target)
};

} // inline namespace jf
