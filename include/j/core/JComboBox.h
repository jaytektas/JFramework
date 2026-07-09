#pragma once

// JComboBox (+ JComboBoxMode).

#include "JControl.h"
#include "JPopupItem.h"
#include "JTextHelper.h"
#include "KeyEvent.h"

inline namespace jf {

// ============================================================================
// JComboBox
// ============================================================================

enum class JComboBoxMode {
    Cycling,
    Popup
};

class JComboBox : public JControl {
public:
    jf::JSignal<int>         onIndexChanged;
    jf::JSignal<std::string> onTextChanged;       // committed text (selection or typed value on Return)
    jf::JSignal<std::string> onEditTextChanged;   // live text while typing in an editable combo
    jf::JSignal<JComboBox*>   onPopupRequested;
    // Framework hook: the app runner installs this to OWN the dropdown — it creates, polls,
    // dismisses the popup window and sets the selected index. When set, it fires on a
    // Popup-mode click so the app needn't wire or service anything. (The per-instance
    // onPopupRequested still fires, for apps not on the runner.)
    static inline std::function<void(JComboBox*)> onOpenPopupHook;

    JComboBox(JSceneGraph& graph, std::vector<std::string> items = {},
             float w = 200.0f, float h = 0.0f)   // h <= 0 → theme control height, so every combo matches
        : JControl(graph, "JComboBox"), m_items(std::move(items))
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().controlHeight;
        _updateMinSize();
    }

    void addItem(const std::string& item) {
        m_items.push_back(item);
        _updateMinSize();
    }
    // Replace the whole item list (dynamic combos). Resets the selection; does not emit onIndexChanged
    // (the caller sets the desired index afterwards).
    void setItems(std::vector<std::string> items) {
        m_items = std::move(items);
        m_currentIndex = m_items.empty() ? -1 : std::clamp(m_currentIndex, 0, (int)m_items.size() - 1);
        _updateMinSize();
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    void clearItems() { setItems({}); }
    void setCurrentIndex(int i) {
        int c = (m_items.empty()) ? -1 : std::clamp(i, 0, (int)m_items.size()-1);
        if (m_currentIndex != c) {
            m_currentIndex = c;
            if (m_editable) { m_editText = (c >= 0) ? m_items[c] : ""; m_edited = false; }   // selection re-seeds the edit box
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onIndexChanged.emit(c);
            if (c >= 0) onTextChanged.emit(m_items[c]);
            notifyAccessibility();
        }
    }
    int         currentIndex() const { return m_currentIndex; }
    std::string currentText()  const {
        if (m_editable && m_edited) return m_editText;           // free-typed value wins in an editable combo
        return (m_currentIndex >= 0 && m_currentIndex < (int)m_items.size())
               ? m_items[m_currentIndex] : "";
    }
    const std::vector<std::string>& items() const { return m_items; }

    JA11yNode a11yNode() const override {
        JA11yNode n; _a11yFillCommon(n, JA11yRole::ComboBox, m_debugName, currentText());
        if (m_editable) n.stateFlags |= JA11yEditable;
        n.hasRange = true; n.curValue = (float)m_currentIndex;
        n.minValue = 0.0f; n.maxValue = m_items.empty() ? 0.0f : (float)(m_items.size() - 1);
        return n;
    }

    JComboBoxMode mode() const { return m_mode; }
    void setMode(JComboBoxMode mode) { m_mode = mode; }

    // ---- Editable combo --------------------------------------------------------------------------------
    // An editable combo lets the user type a value directly; the drop list acts as suggestions. Clicking
    // the text area focuses it for typing, clicking the arrow still opens the list. Non-editable is default.
    void setEditable(bool e) {
        m_editable = e;
        if (e && m_editText.empty() && !m_edited) m_editText = currentText();
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    bool isEditable() const { return m_editable; }
    void setEditText(const std::string& t) {
        m_editText = t; m_edited = true;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        onEditTextChanged.emit(t);
        notifyAccessibility();
    }
    const std::string& editText() const { return m_editText; }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) return;
        onClicked.emit();
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const float arrowW = b.height * 0.75f;
        const bool onArrow = mx >= b.x + b.width - arrowW;
        if (m_editable && !onArrow) {          // click the text area → focus for typing (no cycle/popup)
            requestFocus();
            return;
        }
        if (!m_items.empty()) {
            if (m_mode == JComboBoxMode::Cycling && !m_editable) {
                setCurrentIndex((m_currentIndex + 1) % (int)m_items.size());
            } else {   // Popup (or an editable combo's arrow) opens the suggestion list
                if (onOpenPopupHook) onOpenPopupHook(this);
                onPopupRequested.emit(this);
            }
        }
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!m_editable || !ke.pressed) return false;
        using K = JKeyEvent::JKey;
        if (ke.key == K::Return) { onTextChanged.emit(m_editText); _syncIndexToText(); return true; }
        if (ke.key == K::Backspace) {
            if (!m_editText.empty()) {
                do { m_editText.pop_back(); }   // drop one UTF-8 codepoint
                while (!m_editText.empty() && (static_cast<uint8_t>(m_editText.back()) & 0xC0) == 0x80);
                m_edited = true; m_graph.invalidateNode(m_nodeId, DirtySelf); onEditTextChanged.emit(m_editText);
            }
            return true;
        }
        if (!ke.ctrl && !ke.alt && ke.utf8[0] != '\0' && static_cast<uint8_t>(ke.utf8[0]) >= 32) {
            m_editText += ke.utf8; m_edited = true;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onEditTextChanged.emit(m_editText);
            return true;
        }
        return false;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float arrowW = b.height * 0.75f;
        bool focused = isFocused();
        JStyleOption o = jstyle::option(m_state, focused);
        // Box fill by role: hovered => ToolTipBase (old Surface3), else Button (old Surface2).
        const JColor fill = (m_state == JWidgetState::Hovered)
                              ? jstyle::role(JColorRole::ToolTipBase, o)
                              : jstyle::role(JColorRole::Button, o);
        // Main box
        buf.pushRectangle(b.x, b.y, b.width, b.height, fill.data(),
                          JStyle::current().hint(JStyleHint::ControlRadius),
                          jstyle::borderW(focused), jstyle::border(o).data());
        // Arrow area = ToolTipBase (old Surface3)
        buf.pushRectangle(b.x + b.width - arrowW, b.y + 1.0f, arrowW - 1.0f, b.height - 2.0f,
                          jstyle::role(JColorRole::ToolTipBase, o).data(), 5.0f);
        // Arrow chevron (two rects forming a V)
        float ax = b.x + b.width - arrowW * 0.62f, ay = b.y + b.height * 0.38f;
        uint8_t ac[4] = {Colors::MutedText[0], Colors::MutedText[1], Colors::MutedText[2], 220};
        buf.pushRectangle(ax - 4.0f, ay, 5.0f, 2.0f, ac, 1.0f);
        buf.pushRectangle(ax + 1.0f, ay, 5.0f, 2.0f, ac, 1.0f);
        // Selected item text (or the live edit buffer in an editable combo — untranslated, it's user input).
        const std::string shown = m_editable ? currentText() : tr(currentText());
        const float textAvail = b.width - arrowW - textPadding() - 6.0f;
        if (JTextHelper::hasAtlas() && !shown.empty()) {
            uint8_t tc[4] = {Colors::FieldText[0], Colors::FieldText[1], Colors::FieldText[2], 220};
            float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            JTextHelper::pushText(buf, b.x + textPadding(), ty, shown, tc, textAvail);
        } else if (!JTextHelper::hasAtlas()) {
            uint8_t tc[4] = {Colors::LabelText[0], Colors::LabelText[1], Colors::LabelText[2], 180};
            buf.pushRectangle(b.x + textPadding(), b.y + (b.height-7.0f)*0.5f,
                              textAvail, 7.0f, tc, 2.0f);
        }
        // Caret at the end of the typed text while an editable combo has focus.
        if (m_editable && focused && JTextHelper::hasAtlas()) {
            float cx = b.x + textPadding() + std::min(textAvail, JTextHelper::measureWidth(shown)) + 1.0f;
            buf.pushRectangle(cx, b.y + 6.0f, 1.5f, b.height - 12.0f, jstyle::role(JColorRole::Accent, o).data());
        }
    }


private:
    void _updateMinSize() {
        auto& l = m_graph.getLayout(m_nodeId);
        float maxItemW = 0.f;
        for (const auto& item : m_items) {
            float lw = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(tr(item)) : 40.f;
            if (lw > maxItemW) maxItemW = lw;
        }
        l.minWidth = maxItemW + 36.0f;
        l.minHeight = m_graph.getLayoutConst(m_nodeId).boundingBox.height;
    }

    // If the typed text exactly matches an item, adopt that item's index (keeps selection in sync on Return).
    void _syncIndexToText() {
        for (int i = 0; i < (int)m_items.size(); ++i) {
            if (m_items[i] == m_editText) { if (m_currentIndex != i) { m_currentIndex = i; onIndexChanged.emit(i); } m_edited = false; return; }
        }
    }

    std::vector<std::string> m_items;
    int m_currentIndex{-1};
    JComboBoxMode m_mode{JComboBoxMode::Popup};   // dropdown list by default (the app wires the popup hook)
    bool          m_editable{false};
    bool          m_edited{false};                // user has typed into the edit box
    std::string   m_editText;
};

} // inline namespace jf
