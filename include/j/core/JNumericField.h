#pragma once

// JNumericField — the editable text field inside a spin box.
//
// A spin box IS a text field with steppers (Qt builds QAbstractSpinBox on a real QLineEdit), so it must edit
// like one: click to place the caret, drag to select, double-click a word, triple-click all, Home/End, arrows
// within the text, Backspace/Delete either side of the caret, Ctrl+A/C/X/V. The spin boxes previously carried
// a private std::string plus a single `selectAll` flag, which gave a caret pinned to the end of the text and
// no way to edit PART of a value — typing replaced the lot.
//
// The editing model itself is JTextEditCore, the same one behind JLineEdit, the canvas caption editor and the
// tree rename. This class adds only what is spin-box specific:
//
//   * the numeric grammar — the accept predicate rejects any keystroke or paste that could not build a number,
//     while still allowing the PARTIAL states a person types through ("", "-", "-.", "12.");
//   * an active/inactive split — inactive it just shows the host's formatted value (with suffix); active it
//     owns the text, so a repaint or a value pushed from elsewhere cannot swallow what is being typed;
//   * Outcome, which reports what a keystroke MEANT (commit / revert / step) and leaves the host to apply it,
//     because clamping, rounding and change signals are the host's business.
//
// Usage from a spin box:
//     m_field.setGrammar(/*decimal=*/m_decimals > 0, /*negative=*/m_min < 0.0);
//     press:  m_field.begin(numberText(), false); m_field.press(mx, textX)   // caret lands where clicked
//     key:    switch on m_field.handleKey(ke) -> commit / revert / step / redraw
//     paint:  if (!m_field.active()) m_field.syncDisplay(formatted()); m_field.draw(...)

#include "JWidget.h"
#include "JTextHelper.h"
#include "JTextEditCore.h"
#include "KeyEvent.h"

#include <chrono>
#include <string>

inline namespace jf {

class JNumericField {
public:
    JNumericField() {
        m_core.setAcceptFn([this](const std::string& cand) { return _acceptable(cand); });
    }

    // Which characters can build this box's numbers: a decimal point only when it has decimals, a sign only
    // when its range reaches below zero.
    void setGrammar(bool allowDecimal, bool allowNegative) { m_decimal = allowDecimal; m_negative = allowNegative; }

    // ---- active state ----------------------------------------------------------------------------------
    // Active means "the field owns the text": the caret shows, the buffer is authoritative, and nothing else
    // may overwrite it. A spin box activates the field the moment it takes focus, so the value is there to be
    // edited in place instead of vanishing on the first keystroke.
    bool active() const { return m_active; }
    bool dirty()  const { return m_dirty; }          // has the user changed the text since begin()?

    void begin(const std::string& seed, bool selectAll) {
        if (!m_active) { m_active = true; m_core.setText(seed); }
        else if (!m_dirty && m_core.text() != seed) m_core.setText(seed);   // reseed only if untouched
        if (selectAll) m_core.selectAll(); else m_core.setCaret(m_core.text().size(), false);
        m_dirty = false;
        m_scrollX = 0.f;
    }
    // Re-seed an ACTIVE field after the host changed the value itself (stepper, wheel, commit) and select it,
    // so the next keystroke replaces the stepped value the way every toolkit does.
    void reseed(const std::string& text) { m_core.setText(text); m_core.selectAll(); m_dirty = false; }
    void end() { m_active = false; m_dirty = false; m_selecting = false; m_core.clearSelection(); }

    // While inactive the field mirrors the host's formatted value — one text path for both states.
    void syncDisplay(const std::string& s) { if (!m_active && m_core.text() != s) { m_core.setText(s); m_scrollX = 0.f; } }

    const std::string& text() const { return m_core.text(); }
    // Could this character start a number in this box? Hosts use it to decide whether a keystroke means
    // "begin editing" — so keys the grammar would reject keep bubbling to the host's own shortcuts.
    bool wouldAccept(char c) const { return _acceptable(std::string(1, c)); }
    JTextEditCore&       core()       { return m_core; }
    const JTextEditCore& core() const { return m_core; }

    // ---- keys ------------------------------------------------------------------------------------------
    // What the keystroke meant. The host applies it: `step` is in step units (±1), `commit` parses the text,
    // `revert` restores the value focus arrived with, `changed`/`consumed` drive repaint and event bubbling.
    struct Outcome {
        bool consumed = false;
        bool changed  = false;   // the text changed -> repaint
        bool commit   = false;   // Return
        bool revert   = false;   // Escape
        int  step     = 0;       // Up/Down, in step units
    };

    Outcome handleKey(const JKeyEvent& ke) {
        Outcome out;
        if (!ke.pressed) return out;
        using K = JKeyEvent::JKey;
        if (ke.key == K::Up)     { out.consumed = true; out.step =  1; return out; }
        if (ke.key == K::Down)   { out.consumed = true; out.step = -1; return out; }
        if (ke.key == K::Escape) { out.consumed = m_active; out.revert = m_active; return out; }
        if (!m_active) return out;

        const auto r = m_core.handleKey(ke);
        out.consumed = r.consumed;
        out.changed  = r.changed;
        out.commit   = r.returnPressed;
        if (r.changed) m_dirty = true;
        return out;
    }

    // ---- mouse -----------------------------------------------------------------------------------------
    // textX is the screen x where the text run starts (the host's left padding), so hit-testing matches
    // exactly what draw() rendered. Click counts: 1 caret + drag, 2 word, 3 whole value.
    void press(float mx, float textX) {
        const auto now = std::chrono::steady_clock::now();
        const bool nearby = std::abs(mx - m_lastClickX) < 4.f;
        const bool quick  = m_clicks > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastClick).count() < 400;
        m_clicks = (quick && nearby) ? m_clicks + 1 : 1;
        m_lastClick = now; m_lastClickX = mx;

        const size_t idx = caretFromX(mx, textX);
        if (m_clicks >= 3)      { m_core.selectAll();       m_selecting = false; }
        else if (m_clicks == 2) { m_core.selectWordAt(idx); m_selecting = false; }
        else                    { m_core.setCaret(idx, false); m_selecting = true; }
    }
    bool drag(float mx, float textX) {
        if (!m_selecting) return false;
        m_core.setCaret(caretFromX(mx, textX), /*extend=*/true);
        return true;
    }
    void release() { m_selecting = false; }

    size_t caretFromX(float mx, float textX) const {
        if (!JTextHelper::hasAtlas() || m_core.text().empty()) return m_core.text().size();
        const float target = mx - textX + m_scrollX;
        return m_core.caretAtX(target, [this](size_t end) {
            return JTextHelper::measureWidth(m_core.text().substr(0, end));
        });
    }

    // ---- paint -----------------------------------------------------------------------------------------
    // Draws selection, text, suffix and caret clipped to the field rect, scrolling horizontally to keep the
    // caret visible. The host draws its own surface, border and steppers.
    void draw(JPrimitiveBuffer& buf, const JRect& field, const JStyleOption& o,
              float pad, const std::string& suffix) {
        const std::string& txt = m_core.text();
        const float innerX = field.x + pad;
        const float innerW = std::max(1.f, field.width - 2.f * pad);

        if (JTextHelper::hasAtlas()) {
            const size_t caret = std::min(m_core.caret(), txt.size());
            const float caretW = JTextHelper::measureWidth(txt.substr(0, caret));
            const float fullW  = JTextHelper::measureWidth(txt) + JTextHelper::measureWidth(suffix);
            if (caretW - m_scrollX > innerW) m_scrollX = caretW - innerW;
            if (caretW - m_scrollX < 0.f)    m_scrollX = caretW;
            if (m_scrollX > fullW - innerW)  m_scrollX = fullW - innerW;
            if (m_scrollX < 0.f)             m_scrollX = 0.f;
            const float ox = innerX - m_scrollX;
            const float ty = field.y + (field.height - JTextHelper::lineHeight()) * 0.5f;

            buf.pushClip(innerX, field.y, innerW, field.height);

            if (m_active && m_core.hasSelection() && !txt.empty()) {
                const float xLo = ox + JTextHelper::measureWidth(txt.substr(0, m_core.selectionStart()));
                const float xHi = ox + JTextHelper::measureWidth(txt.substr(0, m_core.selectionEnd()));
                const JColor sc = withAlpha(jstyle::role(JColorRole::Highlight, o), 90);
                buf.pushRectangle(xLo, field.y + 4.f, std::max(1.f, xHi - xLo), field.height - 8.f, sc.data(), 2.f);
            }

            uint8_t vc[4] = {Colors::FieldText[0], Colors::FieldText[1], Colors::FieldText[2], 220};
            JTextHelper::pushText(buf, ox, ty, txt, vc, 0.f);
            if (!suffix.empty()) {
                uint8_t sc[4] = {Colors::MutedText[0], Colors::MutedText[1], Colors::MutedText[2], 200};
                JTextHelper::pushText(buf, ox + JTextHelper::measureWidth(txt), ty, suffix, sc, 0.f);
            }
            if (m_active) {
                buf.pushRectangle(ox + caretW, field.y + 6.f, 1.5f, field.height - 12.f,
                                  jstyle::role(JColorRole::Accent, o).data());
            }
            buf.popClip();
        } else {
            uint8_t vc[4] = {Colors::LabelText[0], Colors::LabelText[1], Colors::LabelText[2], 180};
            buf.pushRectangle(innerX, field.y + (field.height - 7.f) * 0.5f, innerW * 0.6f, 7.f, vc, 2.f);
        }
    }

private:
    // Every PARTIAL state a person types through is acceptable — "", "-", ".", "12." — because rejecting them
    // would make the value unreachable one keystroke at a time. Only characters that can never appear in a
    // number, a second sign, or a second decimal point are refused.
    bool _acceptable(const std::string& s) const {
        size_t i = 0;
        bool dot = false;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
            if (!m_negative) return false;
            ++i;
        }
        for (; i < s.size(); ++i) {
            if (s[i] == '.') { if (!m_decimal || dot) return false; dot = true; }
            else if (s[i] < '0' || s[i] > '9') return false;
        }
        return true;
    }

    JTextEditCore m_core;
    bool  m_active   = false;
    bool  m_dirty    = false;
    bool  m_decimal  = true;
    bool  m_negative = true;
    bool  m_selecting = false;
    float m_scrollX  = 0.f;
    int   m_clicks   = 0;
    std::chrono::steady_clock::time_point m_lastClick{};
    float m_lastClickX = 0.f;
};

} // inline namespace jf
