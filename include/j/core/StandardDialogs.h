#pragma once

// ============================================================================
// JStandardDialogs — the standard dialog set in the framework's own idiom.
//
//   • JMessageBox   — information / warning / critical / question message box with
//                     a configurable button set { Ok, Cancel, Yes, No, Save,
//                     Discard, Apply, Close }, a resolved default + escape button,
//                     an icon kind, and a uniform {accepted, button} result.
//   • JInputDialog  — a single line of text / an integer / a double / a combo
//                     choice with Ok/Cancel, returning {accepted, value}.
//   • JDialogResult — one uniform result model shared by all of the above so every
//                     dialog reports {accepted, button, value…} consistently.
//
// DESIGN RULE (matches the rest of j/core/Dialog.h): the DECISION logic — which
// button is default, which is escape, how a click/keypress maps to a result — is
// pure, header-only and needs no GPU; it is exercised headless by
// tests/dialogs_test.cpp. Only render()/handleMouse()/handleKey() touch the
// primitive-buffer path, exactly like JDialogManager::renderAndHandle().
//
// The font and colour choosers are NOT reimplemented here: JColorPickerDialog and
// JFontPickerDialog already own that job. They report their result through an
// onAccept(std::string) / onCancel() callback pair — a "#rrggbb" hex for the
// colour picker, a "family|size|b|i" spec for the font picker. resultFromPicker()
// below folds those string callbacks into the SAME JDialogResult model so callers
// see one uniform accepted/value shape across the whole dialog set.
// ============================================================================

#include "JTextHelper.h"
#include "JTitleBar.h"
#include "KeyEvent.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

inline namespace jf {

// ---- JDialogButton ----------------------------------------------------------
// The standard button roles. `None` is the null / "no button" sentinel.
enum class JDialogButton : uint8_t {
    None = 0, Ok, Cancel, Yes, No, Save, Discard, Apply, Close
};

// ---- JMessageIcon -----------------------------------------------------------
enum class JMessageIcon : uint8_t {
    NoIcon = 0, Information, Warning, Critical, Question
};

// True for the affirmative roles (accept / commit); false for reject / dismiss.
// Ok/Yes/Save/Apply commit; Cancel/No/Discard/Close do not. Matches the common
// desktop convention (Close and Discard dismiss without committing).
inline bool jIsAffirmative(JDialogButton b) {
    switch (b) {
        case JDialogButton::Ok:
        case JDialogButton::Yes:
        case JDialogButton::Save:
        case JDialogButton::Apply: return true;
        default:                   return false;
    }
}

inline const char* jButtonLabel(JDialogButton b) {
    switch (b) {
        case JDialogButton::Ok:      return "OK";
        case JDialogButton::Cancel:  return "Cancel";
        case JDialogButton::Yes:     return "Yes";
        case JDialogButton::No:      return "No";
        case JDialogButton::Save:    return "Save";
        case JDialogButton::Discard: return "Discard";
        case JDialogButton::Apply:   return "Apply";
        case JDialogButton::Close:   return "Close";
        default:                     return "";
    }
}

// ---- JDialogResult ----------------------------------------------------------
// The single uniform result model. Every dialog fills `accepted` + `button`;
// input dialogs additionally fill the value field for their mode.
struct JDialogResult {
    bool          accepted = false;               // affirmative close?
    JDialogButton button   = JDialogButton::None; // which button ended it

    // Input-dialog payload (only the field for the active mode is meaningful).
    std::string   text;
    long long     intValue    = 0;
    double        doubleValue = 0.0;
    int           choiceIndex = -1;               // combo selection, -1 if none

    explicit operator bool() const { return accepted; }
};

// ============================================================================
// JMessageBox
// ============================================================================
class JMessageBox {
public:
    JMessageBox() = default;
    JMessageBox(JMessageIcon icon, std::string title, std::string text,
                std::vector<JDialogButton> buttons = { JDialogButton::Ok })
        : m_icon(icon), m_title(std::move(title)), m_text(std::move(text)),
          m_buttons(std::move(buttons)) {}

    // ---- Builders ----------------------------------------------------------
    JMessageBox& setIcon(JMessageIcon i)                 { m_icon = i; return *this; }
    JMessageBox& setTitle(std::string t)                 { m_title = std::move(t); return *this; }
    JMessageBox& setText(std::string t)                  { m_text = std::move(t); return *this; }
    JMessageBox& setButtons(std::vector<JDialogButton> b){ m_buttons = std::move(b); return *this; }
    JMessageBox& addButton(JDialogButton b)              { if (b != JDialogButton::None) m_buttons.push_back(b); return *this; }
    JMessageBox& setDefaultButton(JDialogButton b)       { m_defaultButton = b; return *this; }
    JMessageBox& setEscapeButton(JDialogButton b)        { m_escapeButton  = b; return *this; }
    JMessageBox& onResult(std::function<void(JDialogResult)> cb) { m_onResult = std::move(cb); return *this; }

    // ---- Introspection (pure, headless) ------------------------------------
    JMessageIcon icon() const                          { return m_icon; }
    const std::string& title() const                   { return m_title; }
    const std::string& text() const                    { return m_text; }
    const std::vector<JDialogButton>& buttons() const  { return m_buttons; }
    bool hasButton(JDialogButton b) const {
        return std::find(m_buttons.begin(), m_buttons.end(), b) != m_buttons.end();
    }

    // Resolved default: the explicit choice if present in the set, else the
    // first button, else None.
    JDialogButton defaultButton() const {
        if (m_defaultButton != JDialogButton::None && hasButton(m_defaultButton))
            return m_defaultButton;
        return m_buttons.empty() ? JDialogButton::None : m_buttons.front();
    }

    // Resolved escape: explicit if present, else the first reject-role button in
    // preference order (Cancel, No, Close, Discard), else the sole button, else None.
    JDialogButton escapeButton() const {
        if (m_escapeButton != JDialogButton::None && hasButton(m_escapeButton))
            return m_escapeButton;
        for (JDialogButton pref : { JDialogButton::Cancel, JDialogButton::No,
                                    JDialogButton::Close,  JDialogButton::Discard })
            if (hasButton(pref)) return pref;
        if (m_buttons.size() == 1) return m_buttons.front();
        return JDialogButton::None;
    }

    // ---- Decision logic (headless-simulatable) -----------------------------
    // Choose a button; builds the result, marks done, fires the callback once.
    JDialogResult clickButton(JDialogButton b) {
        JDialogResult r;
        r.button   = b;
        r.accepted = jIsAffirmative(b);
        return _finish(r);
    }
    // Activate the resolved default button (Enter / double-click default).
    JDialogResult activateDefault() { return clickButton(defaultButton()); }
    // Escape / close: the resolved escape button.
    JDialogResult escape()          { return clickButton(escapeButton()); }

    bool isDone() const                 { return m_done; }
    const JDialogResult& result() const { return m_result; }

    // ---- Convenience constructors (build + return a drivable request) ------
    static JMessageBox information(std::string title, std::string text,
                                   std::vector<JDialogButton> buttons = { JDialogButton::Ok },
                                   JDialogButton def = JDialogButton::None,
                                   JDialogButton esc = JDialogButton::None) {
        return _make(JMessageIcon::Information, std::move(title), std::move(text),
                     std::move(buttons), def, esc);
    }
    static JMessageBox warning(std::string title, std::string text,
                               std::vector<JDialogButton> buttons = { JDialogButton::Ok },
                               JDialogButton def = JDialogButton::None,
                               JDialogButton esc = JDialogButton::None) {
        return _make(JMessageIcon::Warning, std::move(title), std::move(text),
                     std::move(buttons), def, esc);
    }
    static JMessageBox critical(std::string title, std::string text,
                                std::vector<JDialogButton> buttons = { JDialogButton::Ok },
                                JDialogButton def = JDialogButton::None,
                                JDialogButton esc = JDialogButton::None) {
        return _make(JMessageIcon::Critical, std::move(title), std::move(text),
                     std::move(buttons), def, esc);
    }
    static JMessageBox question(std::string title, std::string text,
                                std::vector<JDialogButton> buttons = { JDialogButton::Yes, JDialogButton::No },
                                JDialogButton def = JDialogButton::Yes,
                                JDialogButton esc = JDialogButton::No) {
        return _make(JMessageIcon::Question, std::move(title), std::move(text),
                     std::move(buttons), def, esc);
    }

    // ---- Rendering / input (primitive-buffer path) -------------------------
    // Button rectangles, right-aligned inside the box, first button leftmost.
    // Pure geometry — no GPU — so hit-testing is testable too.
    std::vector<JRect> buttonRects(float boxX, float boxY, float boxW, float boxH) const {
        const float kBtnH = JStyle::current().buttonHeight;   // height from JStyle (single source of truth)
        constexpr float kGap = 10.f, kPadX = 12.f, kMinW = 84.f;
        const float btnY = boxY + boxH - kBtnH - 14.f;
        std::vector<float> widths;
        widths.reserve(m_buttons.size());
        for (JDialogButton b : m_buttons) {
            float tw = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(jButtonLabel(b)) : 0.f;
            widths.push_back(std::max(kMinW, tw + kPadX * 2.f));
        }
        float total = 0.f;
        for (float w : widths) total += w;
        total += kGap * (widths.empty() ? 0.f : (widths.size() - 1));
        float x = boxX + boxW - 14.f - total;
        std::vector<JRect> out;
        out.reserve(widths.size());
        for (float w : widths) { out.push_back({ x, btnY, w, kBtnH }); x += w + kGap; }
        return out;
    }

    // Draw the box; call once per frame. Does not consume input.
    void render(JPrimitiveBuffer& buf, float screenW, float screenH,
                float mx = -1.f, float my = -1.f) const {
        constexpr float kRadius = 8.f, kTitleH = 32.f;
        const float boxW = std::min(440.f, screenW * 0.8f);
        const float boxH = 170.f;
        const float boxX = (screenW - boxW) * 0.5f;
        const float boxY = (screenH - boxH) * 0.5f;
        const float lh   = JTextHelper::lineHeight();

        buf.pushRectangle(0.f, 0.f, screenW, screenH, Colors::OverlayScrim, 0.f);

        buf.pushRectangle(boxX, boxY, boxW, boxH, Colors::DialogBg, kRadius, 1.f, Colors::Border);
        // Title bar — the framework's ONE canonical styled bar (no custom chrome); leftPad clears the icon.
        JTitleBar::draw(buf, boxX, boxY, boxW, kTitleH, m_title, kRadius, 0, 44.f);
        _drawIcon(buf, boxX + 14.f, boxY + (kTitleH - 18.f) * 0.5f);

        if (JTextHelper::hasAtlas()) {
            uint8_t sc[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, sc);
            JTextHelper::pushText(buf, boxX + 16.f, boxY + kTitleH + 18.f, m_text, sc, boxW - 32.f);
        }

        const JDialogButton def = defaultButton();
        auto rects = buttonRects(boxX, boxY, boxW, boxH);
        for (size_t i = 0; i < rects.size(); ++i) {
            const JRect& r = rects[i];
            const bool  isDef = (m_buttons[i] == def);
            const bool  hov   = (mx >= r.x && mx < r.x + r.width && my >= r.y && my < r.y + r.height);
            if (isDef) {
                uint8_t bg[4] = {Colors::PrimaryBtnBg[0], Colors::PrimaryBtnBg[1], Colors::PrimaryBtnBg[2],
                                 static_cast<uint8_t>(hov ? 255 : 220)};
                buf.pushRectangle(r.x, r.y, r.width, r.height, bg, JStyle::current().cornerRadius, 1.f, Colors::PrimaryBtnBorder);
            } else {
                uint8_t bg[4] = {Colors::CancelBtnBg[0], Colors::CancelBtnBg[1], Colors::CancelBtnBg[2], static_cast<uint8_t>(hov ? 255 : 220)};
                buf.pushRectangle(r.x, r.y, r.width, r.height, bg, JStyle::current().cornerRadius, 1.f, Colors::CancelBtnBorder);
            }
            if (JTextHelper::hasAtlas()) {
                const char* lbl = jButtonLabel(m_buttons[i]);
                JTextHelper::pushText(buf, r.x + (r.width - JTextHelper::measureWidth(lbl)) * 0.5f,
                                      r.y + (r.height - lh) * 0.5f, lbl, isDef ? Colors::PrimaryBtnText : Colors::CancelBtnText);
            }
        }
    }

    // Route a mouse press. Returns true (and finishes) if a button was hit.
    bool handleMousePress(float mx, float my, float screenW, float screenH) {
        const float boxW = std::min(440.f, screenW * 0.8f);
        const float boxH = 170.f;
        const float boxX = (screenW - boxW) * 0.5f;
        const float boxY = (screenH - boxH) * 0.5f;
        auto rects = buttonRects(boxX, boxY, boxW, boxH);
        for (size_t i = 0; i < rects.size(); ++i) {
            const JRect& r = rects[i];
            if (mx >= r.x && mx < r.x + r.width && my >= r.y && my < r.y + r.height) {
                clickButton(m_buttons[i]);
                return true;
            }
        }
        return false;
    }

    // Route a key event. Enter → default, Escape → escape. Returns true if handled.
    bool handleKey(const JKeyEvent& ke) {
        if (!ke.pressed) return false;
        using K = JKeyEvent::JKey;
        if (ke.key == K::Return || ke.key == K::Space) { activateDefault(); return true; }
        if (ke.key == K::Escape)                        { escape();          return true; }
        return false;
    }

private:
    static JMessageBox _make(JMessageIcon icon, std::string title, std::string text,
                             std::vector<JDialogButton> buttons,
                             JDialogButton def, JDialogButton esc) {
        JMessageBox m(icon, std::move(title), std::move(text), std::move(buttons));
        if (def != JDialogButton::None) m.setDefaultButton(def);
        if (esc != JDialogButton::None) m.setEscapeButton(esc);
        return m;
    }

    JDialogResult _finish(JDialogResult r) {
        m_result = r;
        if (!m_done) { m_done = true; if (m_onResult) m_onResult(r); }
        return r;
    }

    void _drawIcon(JPrimitiveBuffer& buf, float x, float y) const {
        if (m_icon == JMessageIcon::NoIcon) return;
        uint8_t col[4];
        switch (m_icon) {
            case JMessageIcon::Information: col[0]=60;  col[1]=140; col[2]=230; break;
            case JMessageIcon::Warning:     col[0]=235; col[1]=170; col[2]=40;  break;
            case JMessageIcon::Critical:    col[0]=220; col[1]=60;  col[2]=60;  break;
            case JMessageIcon::Question:    col[0]=110; col[1]=110; col[2]=200; break;
            default:                        col[0]=120; col[1]=120; col[2]=120; break;
        }
        col[3] = 255;
        buf.pushRectangle(x, y, 18.f, 18.f, col, 9.f);            // disc
        if (JTextHelper::hasAtlas()) {
            const char* glyph = (m_icon == JMessageIcon::Question) ? "?"
                              : (m_icon == JMessageIcon::Critical) ? "x" : "i";
            uint8_t w[4] = {255, 255, 255, 255};
            JTextHelper::pushText(buf, x + (18.f - JTextHelper::measureWidth(glyph)) * 0.5f,
                                  y + (18.f - JTextHelper::lineHeight()) * 0.5f, glyph, w);
        }
    }

    JMessageIcon                m_icon = JMessageIcon::NoIcon;
    std::string                 m_title, m_text;
    std::vector<JDialogButton>  m_buttons { JDialogButton::Ok };
    JDialogButton               m_defaultButton = JDialogButton::None;
    JDialogButton               m_escapeButton  = JDialogButton::None;
    std::function<void(JDialogResult)> m_onResult;
    JDialogResult               m_result;
    bool                        m_done = false;
};

// ============================================================================
// JInputDialog
// ============================================================================
class JInputDialog {
public:
    enum class JMode : uint8_t { Text, Integer, Double, Choice };

    JInputDialog() = default;

    // ---- Builders ----------------------------------------------------------
    JInputDialog& setTitle(std::string t) { m_title = std::move(t); return *this; }
    JInputDialog& setLabel(std::string l) { m_label = std::move(l); return *this; }
    JInputDialog& onResult(std::function<void(JDialogResult)> cb) { m_onResult = std::move(cb); return *this; }

    // Seed a text value.
    JInputDialog& setTextValue(std::string v) {
        m_mode = JMode::Text; m_buffer = std::move(v); return *this;
    }
    // Seed an integer value + inclusive range.
    JInputDialog& setIntValue(long long v, long long lo = -1000000000LL, long long hi = 1000000000LL) {
        m_mode = JMode::Integer; m_intMin = lo; m_intMax = hi;
        m_buffer = std::to_string(std::clamp(v, lo, hi)); return *this;
    }
    // Seed a double value + inclusive range.
    JInputDialog& setDoubleValue(double v, double lo = -1e300, double hi = 1e300) {
        m_mode = JMode::Double; m_dblMin = lo; m_dblMax = hi;
        char t[64]; std::snprintf(t, sizeof(t), "%g", std::clamp(v, lo, hi));
        m_buffer = t; return *this;
    }
    // Seed a combo choice list + current index.
    JInputDialog& setChoices(std::vector<std::string> items, int current = 0) {
        m_mode = JMode::Choice; m_choices = std::move(items);
        m_choiceIndex = m_choices.empty() ? -1
                      : std::clamp(current, 0, static_cast<int>(m_choices.size()) - 1);
        return *this;
    }

    // ---- Live editing (headless) -------------------------------------------
    // For Text/Integer/Double modes: mutate the edit buffer as if typing.
    void setBuffer(const std::string& s) { m_buffer = s; }
    const std::string& buffer() const     { return m_buffer; }
    // For Choice mode: move the selection.
    void setChoiceIndex(int i) {
        if (!m_choices.empty())
            m_choiceIndex = std::clamp(i, 0, static_cast<int>(m_choices.size()) - 1);
    }

    // ---- Introspection -----------------------------------------------------
    JMode mode() const                  { return m_mode; }
    const std::string& title() const    { return m_title; }
    const std::string& label() const    { return m_label; }
    const std::vector<std::string>& choices() const { return m_choices; }
    int  choiceIndex() const            { return m_choiceIndex; }
    bool isDone() const                 { return m_done; }
    const JDialogResult& result() const { return m_result; }

    // ---- Decision logic (headless-simulatable) -----------------------------
    // Accept: parse the current buffer/selection into the result, accepted=true.
    JDialogResult accept() {
        JDialogResult r;
        r.accepted = true;
        r.button   = JDialogButton::Ok;
        switch (m_mode) {
            case JMode::Text:
                r.text = m_buffer;
                break;
            case JMode::Integer: {
                long long v = 0;
                try { v = std::stoll(m_buffer); } catch (...) { v = 0; }
                v = std::clamp(v, m_intMin, m_intMax);
                r.intValue = v; r.text = std::to_string(v);
                break;
            }
            case JMode::Double: {
                double v = 0.0;
                try { v = std::stod(m_buffer); } catch (...) { v = 0.0; }
                v = std::clamp(v, m_dblMin, m_dblMax);
                r.doubleValue = v; r.text = m_buffer;
                break;
            }
            case JMode::Choice:
                r.choiceIndex = m_choiceIndex;
                if (m_choiceIndex >= 0 && m_choiceIndex < static_cast<int>(m_choices.size()))
                    r.text = m_choices[m_choiceIndex];
                break;
        }
        return _finish(r);
    }
    // Cancel: accepted=false, no value committed.
    JDialogResult cancel() {
        JDialogResult r;
        r.accepted = false;
        r.button   = JDialogButton::Cancel;
        return _finish(r);
    }

    // ---- Convenience constructors ------------------------------------------
    static JInputDialog getText(std::string title, std::string label, std::string initial = "") {
        JInputDialog d; d.setTitle(std::move(title)).setLabel(std::move(label)).setTextValue(std::move(initial));
        return d;
    }
    static JInputDialog getInt(std::string title, std::string label, long long value,
                               long long lo = -1000000000LL, long long hi = 1000000000LL) {
        JInputDialog d; d.setTitle(std::move(title)).setLabel(std::move(label)).setIntValue(value, lo, hi);
        return d;
    }
    static JInputDialog getDouble(std::string title, std::string label, double value,
                                  double lo = -1e300, double hi = 1e300) {
        JInputDialog d; d.setTitle(std::move(title)).setLabel(std::move(label)).setDoubleValue(value, lo, hi);
        return d;
    }
    static JInputDialog getItem(std::string title, std::string label,
                                std::vector<std::string> items, int current = 0) {
        JInputDialog d; d.setTitle(std::move(title)).setLabel(std::move(label)).setChoices(std::move(items), current);
        return d;
    }

    // ---- Input routing (buffer editing for the text-ish modes) -------------
    // Returns true when the dialog closes (accept/cancel).
    bool handleKey(const JKeyEvent& ke) {
        if (!ke.pressed) return false;
        using K = JKeyEvent::JKey;
        if (ke.key == K::Return) { accept(); return true; }
        if (ke.key == K::Escape) { cancel(); return true; }
        if (m_mode == JMode::Choice) {
            if (ke.key == K::Up)   { setChoiceIndex(m_choiceIndex - 1); return false; }
            if (ke.key == K::Down) { setChoiceIndex(m_choiceIndex + 1); return false; }
            return false;
        }
        if (ke.key == K::Backspace) { if (!m_buffer.empty()) m_buffer.pop_back(); return false; }
        if (static_cast<unsigned char>(ke.utf8[0]) >= 0x20) {
            // Integer/Double modes accept only value characters.
            const char c = ke.utf8[0];
            const bool valueCh = (c >= '0' && c <= '9') || c == '-' || c == '+' ||
                                 (m_mode == JMode::Double && (c == '.' || c == 'e' || c == 'E'));
            if (m_mode == JMode::Text || valueCh) m_buffer += ke.utf8;
        }
        return false;
    }

private:
    JDialogResult _finish(JDialogResult r) {
        m_result = r;
        if (!m_done) { m_done = true; if (m_onResult) m_onResult(r); }
        return r;
    }

    JMode                    m_mode = JMode::Text;
    std::string              m_title, m_label, m_buffer;
    long long                m_intMin = -1000000000LL, m_intMax = 1000000000LL;
    double                   m_dblMin = -1e300, m_dblMax = 1e300;
    std::vector<std::string> m_choices;
    int                      m_choiceIndex = -1;
    std::function<void(JDialogResult)> m_onResult;
    JDialogResult            m_result;
    bool                     m_done = false;
};

// ============================================================================
// Picker result adaptor
// ----------------------------------------------------------------------------
// JColorPickerDialog / JFontPickerDialog are NOT reimplemented here (they own the
// GPU-side chooser). They report via onAccept(std::string)/onCancel(). Fold that
// pair into the uniform JDialogResult so callers handle every dialog the same way:
//
//   picker.onAccept([&](std::string v){ handle(jResultFromPicker(true,  v)); });
//   picker.onCancel([&]{               handle(jResultFromPicker(false, {})); });
//
// The chooser's payload (hex "#rrggbb" or "family|size|b|i") lands in result.text.
// ============================================================================
inline JDialogResult jResultFromPicker(bool accepted, std::string value) {
    JDialogResult r;
    r.accepted = accepted;
    r.button   = accepted ? JDialogButton::Ok : JDialogButton::Cancel;
    r.text     = std::move(value);
    return r;
}

} // inline namespace jf
