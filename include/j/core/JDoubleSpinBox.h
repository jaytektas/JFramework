#pragma once

// JDoubleSpinBox.

#include "JControl.h"
#include "JTextHelper.h"
#include "KeyEvent.h"

inline namespace jf {

// ============================================================================
// JDoubleSpinBox — floating-point spin box.
//
// Mirrors JSpinBox exactly, but stores a `double` with configurable step,
// decimal places and an optional textual suffix.  Full precision is kept
// internally; only the rendered text is rounded to `decimals`.
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
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().controlHeight;
        l.minWidth = 60.0f;
        l.minHeight = h;
    }

    void setValue(double v) {
        double c = std::clamp(v, m_min, m_max);
        if (m_value != c) { m_value = c; m_graph.invalidateNode(m_nodeId, DirtySelf); onValueChanged.emit(c); notifyAccessibility(); }
    }
    double value() const { return m_value; }

    JA11yNode a11yNode() const override {
        JA11yNode n; _a11yFillCommon(n, JA11yRole::SpinBox, m_debugName, _formatValue());
        n.hasRange = true; n.curValue = (float)m_value; n.minValue = (float)m_min; n.maxValue = (float)m_max;
        n.stateFlags |= JA11yEditable;
        return n;
    }

    void setRange(double min, double max) { m_min = min; m_max = max; setValue(m_value); }
    void setStep(double step)             { m_step = step; }
    void setDecimals(int decimals)        { m_decimals = decimals; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    void setSuffix(const std::string& s)  { m_suffix = s; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    const std::string& suffix() const     { return m_suffix; }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) { _commitEdit(); return; }
        requestFocus();   // clicking the box focuses it, so typed digits route here (framework focus)
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float btnW = b.height * 0.7f;
        if (mx >= b.x + b.width - btnW) {   // an arrow — commit any edit, then step
            _commitEdit();
            setValue(m_value + (my < b.y + b.height * 0.5f ? m_step : -m_step));
        } else {                            // the value field — begin direct text entry
            setState(JWidgetState::Pressed);
            _beginEdit();
        }
    }

    bool handleScroll(float mx, float my, float wheel) override {
        if (!isPointInside(mx, my)) return false;
        _commitEdit();
        setValue(m_value + (wheel > 0.0f ? m_step : -m_step));
        return true;
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed) return false;
        using K = JKeyEvent::JKey;
        if (ke.key == K::Up)   { _commitEdit(); setValue(m_value + m_step); return true; }
        if (ke.key == K::Down) { _commitEdit(); setValue(m_value - m_step); return true; }
        if (ke.key == K::Return) { _commitEdit(); return true; }
        if (ke.key == K::Escape) { if (m_editing) { m_editing = false; m_selectAll = false; invalidate(); return true; } return false; }
        if (ke.key == K::Backspace) {
            if (!m_editing) return false;
            if (m_selectAll) { m_editBuf.clear(); m_selectAll = false; }   // delete the selection
            else if (!m_editBuf.empty()) m_editBuf.pop_back();
            invalidate();
            return true;
        }
        // Printable characters: accept only what can form a number.
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

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        // Clicking away drops keyboard focus — commit the pending edit so it isn't lost.
        if (m_editing && !isFocused()) _commitEdit();
        float btnW = b.height * 0.7f;
        float fieldW = b.width - btnW;

        bool focused = isFocused();
        // Field surface + border by role; focused OR editing takes the Accent ring.
        JStyleOption o = jstyle::option(m_state, focused || m_editing);
        // Value field
        buf.pushRectangle(b.x, b.y, fieldW, b.height, jstyle::fieldFill(o).data(),
                          JTheme::current().hint(JStyleHint::ControlRadius),
                          jstyle::borderW(focused || m_editing), jstyle::border(o).data());
        // Value text (the live edit buffer while typing, otherwise the formatted value)
        const std::string txt = m_editing ? m_editBuf : _formatValue();
        if (JTextHelper::hasAtlas()) {
            uint8_t vc[4] = {210, 210, 220, 220};
            float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            if (m_editing && m_selectAll && !txt.empty()) {   // selection highlight behind the value
                const JColor sel = withAlpha(jstyle::role(JColorRole::Highlight, o), 90);   // Accent @ 90
                buf.pushRectangle(b.x + textPadding() - 1.0f, b.y + 4.0f,
                                  std::min(fieldW - textPadding(), JTextHelper::measureWidth(txt) + 2.0f), b.height - 8.0f, sel.data(), 2.0f);
            }
            JTextHelper::pushText(buf, b.x + textPadding(), ty, txt, vc, fieldW - textPadding());
            if (m_editing && !m_selectAll) {   // caret at the end of the typed text
                float cx = b.x + textPadding() + JTextHelper::measureWidth(txt) + 1.0f;
                buf.pushRectangle(cx, b.y + 6.0f, 1.5f, b.height - 12.0f, jstyle::role(JColorRole::Accent, o).data());
            }
        } else {
            uint8_t vc[4] = {200, 200, 210, 180};
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
        uint8_t ac[4] = {180, 180, 190, 200};
        buf.pushRectangle(ax, b.y + halfH * 0.35f,        aw, 2.0f, ac);  // up mark
        buf.pushRectangle(ax, b.y + halfH + halfH * 0.55f, aw, 2.0f, ac); // down mark
    }


private:
    std::string _formatValue() const {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", m_decimals, m_value);
        return std::string(buf) + m_suffix;
    }
    // Seed the edit buffer with the current numeric value (no suffix) AND select it, so the first typed
    // digit replaces it (standard entry-field behaviour); backspace/edit keys edit in place instead.
    void _beginEdit() {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", m_decimals, m_value);
        m_editBuf = buf; m_editing = true; m_selectAll = true; invalidate();
    }
    void _commitEdit() {
        if (!m_editing) return;
        m_editing = false; m_selectAll = false;
        try { setValue(std::stod(m_editBuf)); } catch (...) {}
        invalidate();
    }
    // Which typed characters can build a number for this box.
    bool _acceptChar(char c) const {
        if (c >= '0' && c <= '9') return true;
        if (c == '-') return m_min < 0.0 && m_editBuf.find('-') == std::string::npos;
        if (c == '.') return m_decimals > 0 && m_editBuf.find('.') == std::string::npos;
        return false;
    }

    double      m_value, m_min, m_max, m_step;
    int         m_decimals;
    std::string m_suffix;
    bool        m_editing{false};
    bool        m_selectAll{false};
    std::string m_editBuf;
};

} // inline namespace jf
