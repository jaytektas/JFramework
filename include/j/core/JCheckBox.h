#pragma once

// JCheckBox.

#include "JControl.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
// JCheckBox
// ============================================================================

class JCheckBox : public JControl {
public:
    // Tri-state model. A plain (two-state) box only ever visits Unchecked/Checked.
    enum CheckState { Unchecked, PartiallyChecked, Checked };

    jf::JSignal<bool>       onStateChanged;       // fires with (state==Checked) — legacy two-state signal
    jf::JSignal<CheckState> onCheckStateChanged;  // fires with the full tri-state on any change

    JCheckBox(JSceneGraph& graph, const std::string& label, float w = 200.0f, float h = 0.0f)
        : JControl(graph, "JCheckBox"), m_label(label)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().checkHeight;
        l.minWidth = JTextHelper::hasAtlas() ? (JTextHelper::measureWidth(m_label) + 28.f) : w;
        l.minHeight = h;
    }

    // ---- Tri-state API ---------------------------------------------------------------------------------
    // Opt-in: enables the middle (PartiallyChecked) state in the click cycle.
    void setTristate(bool on) { m_tristate = on; }
    bool isTristate() const { return m_tristate; }
    void setCheckState(CheckState s) {
        if (m_state_cb != s) {
            m_state_cb = s;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onCheckStateChanged.emit(s);
            onStateChanged.emit(s == Checked);
            notifyAccessibility();
        }
    }
    CheckState checkState() const { return m_state_cb; }

    JA11yNode a11yNode() const override {
        const char* v = m_state_cb == Checked ? "checked"
                      : m_state_cb == PartiallyChecked ? "mixed" : "unchecked";
        JA11yNode n; _a11yFillCommon(n, JA11yRole::CheckBox, m_label, v);
        if (m_state_cb == Checked)               n.stateFlags |= JA11yChecked;
        else if (m_state_cb == PartiallyChecked) n.stateFlags |= JA11yMixed;
        return n;
    }

    // ---- Two-state compatibility ----------------------------------------------------------------------
    void setChecked(bool v) { setCheckState(v ? Checked : Unchecked); }
    bool isChecked() const { return m_state_cb == Checked; }

    JVariant getRef(const std::string& key) const override {
        if (key == "checked")   return m_state_cb == Checked;
        if (key == "value" || key == "checkState") return static_cast<int64_t>(m_state_cb);
        return JWidget::getRef(key);
    }

    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) _cycle();
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float boxSz = b.height;
        // MIGRATED to the role/state styler: resolve the indicator fill by semantic ROLE
        // from the live palette instead of naming a raw shade. checked/partial -> Highlight,
        // unchecked -> Base — which map onto the old Accent / Surface1 exactly.
        const bool on = m_state_cb != Unchecked;
        JStyleOption opt;
        opt.rect = {b.x, b.y, boxSz, boxSz};
        opt.set(State_Focused, isFocused());
        opt.set(State_On | State_Selected, on);
        const JColor fill = JStyle::controlFill(opt, JTheme::current().palette());
        drawBox(buf, b, boxSz, fill.data(), isFocused());
        drawLabel(buf, b, boxSz);
    }


protected:
    // Click cycle. Two-state: Unchecked <-> Checked. Tri-state: Unchecked -> Checked ->
    // PartiallyChecked -> Unchecked (documented order).
    void _cycle() {
        if (!m_tristate) { setChecked(m_state_cb != Checked); return; }
        switch (m_state_cb) {
            case Unchecked:        setCheckState(Checked);          break;
            case Checked:          setCheckState(PartiallyChecked); break;
            case PartiallyChecked: setCheckState(Unchecked);        break;
        }
    }
    virtual void drawBox(JPrimitiveBuffer& buf, const JRect& b, float boxSz, const uint8_t* fill, bool focused) {
        // Border colour by ROLE: Accent ring when focused, else Border (was hardcoded).
        JStyleOption opt; opt.set(State_Focused, focused);
        const JColor border = JStyle::borderColor(opt, JTheme::current().palette());
        buf.pushRectangle(b.x, b.y, boxSz, boxSz, fill, 4.0f,
                          focused ? 2.0f : 1.5f,
                          border.data());
        if (m_state_cb == Checked) {                 // full mark: the original plus/tick
            uint8_t white[4] = {255, 255, 255, 220};
            buf.pushRectangle(b.x + 3.0f, b.y + boxSz*0.5f - 1.5f, boxSz - 6.0f, 3.0f, white, 1.5f);
            buf.pushRectangle(b.x + boxSz*0.5f - 1.5f, b.y + 3.0f, 3.0f, boxSz - 6.0f, white, 1.5f);
        } else if (m_state_cb == PartiallyChecked) { // partial: a single centred dash
            uint8_t white[4] = {255, 255, 255, 200};
            buf.pushRectangle(b.x + 4.0f, b.y + boxSz*0.5f - 1.5f, boxSz - 8.0f, 3.0f, white, 1.5f);
        }
    }
    virtual void drawLabel(JPrimitiveBuffer& buf, const JRect& b, float boxSz) {
        if (JTextHelper::hasAtlas()) {
            uint8_t lc[4] = {200, 200, 210, 200};
            JTextHelper::pushText(buf, b.x + boxSz + 8.0f,
                                 b.y + (b.height - JTextHelper::lineHeight()) * 0.5f,
                                 tr(m_label), lc, b.width - boxSz - 8.0f);
        } else {
            uint8_t lc[4] = {180, 180, 190, 140};
            buf.pushRectangle(b.x + boxSz + 8.0f, b.y + (b.height - 6.0f)*0.5f,
                               b.width - boxSz - 8.0f, 6.0f, lc, 2.0f);
        }
    }

private:
    std::string m_label;
    CheckState  m_state_cb{Unchecked};
    bool        m_tristate{false};
};

} // inline namespace jf
