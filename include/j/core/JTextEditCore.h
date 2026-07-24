#pragma once

// ============================================================================
// JTextEditCore — the one text-editing model behind every single-line text field.
// ============================================================================
//
// Owns the text buffer, the caret and selection (byte offsets kept on UTF-8 char
// boundaries), UTF-8 + word navigation, the editing operations, the clipboard, and
// the key map — everything EXCEPT rendering. It touches no widget, no font, no
// layout. A host drives it (feeding keys/mouse hits) and draws the text, caret and
// selection itself, however it likes:
//
//   JLineEdit          — screen-space field with chrome, wraps a core.
//   LabelWidget (edit)  — in-canvas caption, scaled/rotated, wraps a core.
//   JTreeView rename    — via JLineEdit.
//
// Because the editing logic lives here once, improving it (word select, clipboard,
// caret motion, IME later) improves every field at once — no per-widget copies.
//
// The host supplies the render-dependent bits as callbacks: an "accept" predicate
// (a validator gate) and, for hit-testing, a prefix-width measure (so the core stays
// font/echo-agnostic). Ctrl+C/Ctrl+X copy can be suppressed (masked fields) via
// setCopyEnabled. handleKey never renders or emits — it returns what changed and the
// host reacts (invalidate, fire signals, run a commit-time validator on Return).

#include "KeyEvent.h"
#include "JWidget.h"   // JWidget::clipboardGet / clipboardSet (static hooks)

#include <string>
#include <functional>
#include <algorithm>

inline namespace jf {

class JTextEditCore {
public:
    // ---- text ----------------------------------------------------------------
    const std::string& text() const { return t_; }
    void setText(const std::string& s) {
        t_ = s;
        if (max_ && t_.size() > max_) t_.resize(max_);
        caret_ = anchor_ = t_.size();   // caret to end + collapse selection on set
    }

    // ---- caret / selection ---------------------------------------------------
    size_t caret()  const { return caret_; }
    size_t anchor() const { return anchor_; }
    void   setCaret(size_t c, bool extend) { c = std::min(c, t_.size()); caret_ = c; if (!extend) anchor_ = c; }

    bool        hasSelection()   const { return anchor_ != caret_; }
    size_t      selectionStart() const { return std::min(anchor_, caret_); }
    size_t      selectionEnd()   const { return std::max(anchor_, caret_); }
    std::string selectedText()   const {
        return hasSelection() ? t_.substr(selectionStart(), selectionEnd() - selectionStart()) : std::string();
    }
    void selectAll()      { anchor_ = 0; caret_ = t_.size(); }
    void clearSelection() { anchor_ = caret_; }
    // Select the contiguous run (word OR whitespace/punct) of the same class as the char at `i`.
    void selectWordAt(size_t i) {
        const size_t n = t_.size();
        if (n == 0) { anchor_ = caret_ = 0; return; }
        if (i >= n) i = n - 1;
        const bool w = isWordChar(static_cast<unsigned char>(t_[i]));
        size_t s = i, e = i;
        while (s > 0 && isWordChar(static_cast<unsigned char>(t_[s - 1])) == w) --s;
        while (e < n && isWordChar(static_cast<unsigned char>(t_[e]))     == w) ++e;
        anchor_ = s; caret_ = e;
    }

    // ---- config --------------------------------------------------------------
    void   setMaxLength(size_t n) { max_ = n; if (max_ && t_.size() > max_) { t_.resize(max_); clampCaret(); } }
    size_t maxLength() const { return max_; }
    void   setReadOnly(bool ro) { readOnly_ = ro; }
    bool   isReadOnly() const { return readOnly_; }
    // Multi-line mode: Return inserts a newline (instead of signalling returnPressed), paste keeps its
    // newlines (single-line mode flattens them), and Tab is an insertable character. Vertical/row-local
    // motion (Up/Down, wrap-aware Home/End) stays with the host, which owns the line layout. Default off.
    void   setMultiline(bool m) { multiline_ = m; }
    bool   isMultiline() const { return multiline_; }
    // Non-owning gate: a candidate string a keystroke/paste would produce is rejected if this returns false.
    void   setAcceptFn(std::function<bool(const std::string&)> f) { accept_ = std::move(f); }
    // Host suppresses Ctrl+C / Ctrl+X copy-to-clipboard (e.g. a masked password field) — the delete still runs.
    void   setCopyEnabled(bool on) { copyEnabled_ = on; }

    // ---- UTF-8 + word navigation (public: hosts use them for hit-tests / tests) ----
    size_t nextCharStart(size_t i) const {
        if (i >= t_.size()) return t_.size();
        ++i; while (i < t_.size() && (static_cast<uint8_t>(t_[i]) & 0xC0) == 0x80) ++i; return i;
    }
    size_t prevCharStart(size_t i) const {
        if (i == 0) return 0;
        --i; while (i > 0 && (static_cast<uint8_t>(t_[i]) & 0xC0) == 0x80) --i; return i;
    }
    // A "word" char: ASCII alphanumerics, '_', and any UTF-8 byte (multibyte glyphs count as content).
    static bool isWordChar(unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80;
    }
    size_t nextWord(size_t i) const {
        const size_t n = t_.size();
        while (i < n &&  isWordChar(static_cast<unsigned char>(t_[i]))) ++i;
        while (i < n && !isWordChar(static_cast<unsigned char>(t_[i]))) ++i;
        return i;
    }
    size_t prevWord(size_t i) const {
        while (i > 0 && !isWordChar(static_cast<unsigned char>(t_[i - 1]))) --i;
        while (i > 0 &&  isWordChar(static_cast<unsigned char>(t_[i - 1]))) --i;
        return i;
    }

    // Nearest caret byte-boundary to `targetX` (already in text-run local space). measurePrefix(byteEnd)
    // must return the rendered width of text()[0..byteEnd) as the host draws it (font/scale/echo applied).
    size_t caretAtX(float targetX, const std::function<float(size_t)>& measurePrefix) const {
        if (t_.empty() || targetX <= 0.f) return targetX <= 0.f ? 0 : t_.size();
        float prevW = 0.f;
        for (size_t i = 0; i < t_.size(); ) {
            const size_t nxt = nextCharStart(i);
            const float w = measurePrefix(nxt);
            if (targetX < (prevW + w) * 0.5f) return i;
            prevW = w; i = nxt;
        }
        return t_.size();
    }

    // ---- editing -------------------------------------------------------------
    bool deleteSelection() {
        if (!hasSelection()) return false;
        const size_t lo = selectionStart(), hi = selectionEnd();
        t_.erase(lo, hi - lo);
        caret_ = anchor_ = lo;
        return true;
    }

    // The unified key map. Never renders or emits — returns what happened; the host reacts.
    struct KeyResult { bool consumed = false; bool changed = false; bool returnPressed = false; };
    KeyResult handleKey(const JKeyEvent& ke) {
        KeyResult r;
        if (!ke.pressed) return r;
        using K = JKeyEvent::JKey;
        clampCaret();

        // Clipboard + select-all (Ctrl). Match on the JKey AND the folded printable, since a Ctrl-chord can
        // arrive as a named key or a control-code utf8 byte depending on the platform.
        if (ke.ctrl && !ke.alt) {
            const char lc = static_cast<char>(ke.utf8[0] | 0x20);
            if (ke.key == K::A || lc == 'a') { selectAll(); r.consumed = true; return r; }
            if (ke.key == K::C || lc == 'c') { if (hasSelection() && copyEnabled_) JWidget::clipboardSet(selectedText()); r.consumed = true; return r; }
            if (ke.key == K::X || lc == 'x') {
                r.consumed = true; if (readOnly_) return r;
                if (hasSelection()) { if (copyEnabled_) JWidget::clipboardSet(selectedText()); deleteSelection(); r.changed = true; }
                return r;
            }
            if (ke.key == K::V || lc == 'v') {
                r.consumed = true; if (readOnly_) return r;
                std::string clip = JWidget::clipboardGet();
                if (!multiline_) {                                                     // single-line: drop newlines
                    clip.erase(std::remove(clip.begin(), clip.end(), '\n'), clip.end());
                    clip.erase(std::remove(clip.begin(), clip.end(), '\r'), clip.end());
                }
                if (insertText(clip, /*trimToFit=*/true)) r.changed = true;
                return r;
            }
        }

        switch (ke.key) {
            case K::Backspace:
                r.consumed = true; if (readOnly_) return r;
                if (hasSelection())      { deleteSelection(); r.changed = true; }
                else if (ke.ctrl)        { const size_t p = prevWord(caret_); if (p < caret_) { t_.erase(p, caret_ - p); caret_ = anchor_ = p; r.changed = true; } }
                else if (caret_ > 0)     { const size_t p = prevCharStart(caret_); t_.erase(p, caret_ - p); caret_ = anchor_ = p; r.changed = true; }
                return r;
            case K::Delete:
                r.consumed = true; if (readOnly_) return r;
                if (hasSelection())          { deleteSelection(); r.changed = true; }
                else if (ke.ctrl)            { const size_t e = nextWord(caret_); if (e > caret_) { t_.erase(caret_, e - caret_); anchor_ = caret_; r.changed = true; } }
                else if (caret_ < t_.size()) { t_.erase(caret_, nextCharStart(caret_) - caret_); anchor_ = caret_; r.changed = true; }
                return r;
            case K::Left: {
                size_t pos = ke.ctrl ? prevWord(caret_) : prevCharStart(caret_);
                if (!ke.shift && !ke.ctrl && hasSelection()) pos = selectionStart();   // plain arrow collapses to edge
                caret_ = pos; if (!ke.shift) anchor_ = pos; r.consumed = true; return r;
            }
            case K::Right: {
                size_t pos = ke.ctrl ? nextWord(caret_) : nextCharStart(caret_);
                if (!ke.shift && !ke.ctrl && hasSelection()) pos = selectionEnd();
                caret_ = pos; if (!ke.shift) anchor_ = pos; r.consumed = true; return r;
            }
            case K::Home: caret_ = 0;          if (!ke.shift) anchor_ = caret_; r.consumed = true; return r;
            case K::End:  caret_ = t_.size();  if (!ke.shift) anchor_ = caret_; r.consumed = true; return r;
            case K::Return:
                r.consumed = true;
                if (multiline_) { if (insertText("\n", /*trimToFit=*/false)) r.changed = true; }   // newline, not commit
                else            { r.returnPressed = true; }
                return r;
            default: break;
        }

        // Printable (plus Tab when multi-line) → replace the selection + insert.
        if (!ke.ctrl && !ke.alt && ke.utf8[0] != '\0' &&
            (static_cast<uint8_t>(ke.utf8[0]) >= 32 || (multiline_ && ke.utf8[0] == '\t'))) {
            r.consumed = true; if (readOnly_) return r;
            if (insertText(std::string(ke.utf8), /*trimToFit=*/false)) r.changed = true;
            return r;
        }
        return r;
    }

private:
    void clampCaret() { caret_ = std::min(caret_, t_.size()); anchor_ = std::min(anchor_, t_.size()); }
    bool accepts(const std::string& cand) const { return !accept_ || accept_(cand); }

    // Replace the selection with `ins`, gated by max length + the accept predicate. trimToFit shortens an
    // over-length paste to what fits (returns the trimmed insert); a single printable that would overflow is
    // rejected outright (trimToFit=false). Returns whether the text changed.
    bool insertText(const std::string& ins, bool trimToFit) {
        if (readOnly_ || ins.empty()) return false;
        std::string clip = ins;
        const size_t lo = selectionStart(), hi = selectionEnd();
        std::string cand = t_; cand.replace(lo, hi - lo, clip);
        if (max_ && cand.size() > max_) {
            if (!trimToFit) return false;
            const size_t room = max_ > (t_.size() - (hi - lo)) ? max_ - (t_.size() - (hi - lo)) : 0;
            clip.resize(std::min(clip.size(), room));
            cand = t_; cand.replace(lo, hi - lo, clip);
        }
        if (clip.empty() || !accepts(cand)) return false;
        deleteSelection();
        t_.insert(caret_, clip);
        caret_ += clip.size();
        anchor_ = caret_;
        return true;
    }

    std::string t_;
    size_t      caret_  = 0;   // insertion index (bytes; on a UTF-8 boundary)
    size_t      anchor_ = 0;   // selection anchor (== caret_ ⇒ no selection)
    size_t      max_    = 0;   // 0 = unlimited
    bool        readOnly_    = false;
    bool        copyEnabled_ = true;
    bool        multiline_   = false;
    std::function<bool(const std::string&)> accept_;   // validator gate (non-owning)
};

} // inline namespace jf
