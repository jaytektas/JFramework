#pragma once

// JSlider.

#include "JControl.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
// JSlider
// ============================================================================

class JSlider : public JControl {
public:
    jf::JSignal<float> onValueChanged;

    JSlider(JSceneGraph& graph, float w = 280.0f, float h = 0.0f)
        : JControl(graph, "JSlider"), m_value(0.5f)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().sliderHeight;
        l.minWidth = 50.0f;
        l.minHeight = h;
    }

    void setValue(float v) {
        float c = std::clamp(v, 0.0f, 1.0f);
        if (m_value != c) { m_value = c; m_graph.invalidateNode(m_nodeId, DirtySelf); onValueChanged.emit(c); notifyAccessibility(); }
    }
    float getValue() const { return m_value; }

    JVariant getRef(const std::string& key) const override {
        if (key == "value") return static_cast<double>(m_value);
        return JWidget::getRef(key);
    }

    JA11yNode a11yNode() const override {
        char v[24]; std::snprintf(v, sizeof(v), "%.2f", m_value);
        JA11yNode n; _a11yFillCommon(n, JA11yRole::Slider, m_debugName, v);
        n.hasRange = true; n.curValue = m_value; n.minValue = 0.0f; n.maxValue = 1.0f;
        return n;
    }

    void handleMouseMove(float mx, float my) override {
        JControl::handleMouseMove(mx, my);
        if (m_state == JWidgetState::Pressed) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            setValue((mx - b.x) / b.width);
        }
    }
    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) {
            setState(JWidgetState::Pressed);
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            setValue((mx - b.x) / b.width);
        }
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float trackH = 4.0f, trackY = b.y + (b.height - trackH) * 0.5f;
        float fillW  = b.width * m_value;
        drawTrack(buf, b, trackY, trackH, fillW);

        float thumbW = 16.0f, thumbH = b.height;
        float thumbX = b.x + fillW - thumbW * 0.5f;
        // Guard the hi bound: when the slider is narrower than the thumb (e.g. a dock
        // collapsing to ~0 width during a drag), b.x + b.width - thumbW < b.x, which would
        // make std::clamp's lo > hi and abort under libstdc++ hardening.
        thumbX = std::clamp(thumbX, b.x, std::max(b.x, b.x + b.width - thumbW));
        // Thumb: pressed => themed AccentPress (no palette role for the pressed accent),
        // else the Text role (old TextPrimary).
        const JColor tc = (m_state == JWidgetState::Pressed)
                            ? JColor::fromArray(Colors::AccentPress)
                            : jstyle::role(JColorRole::Text, jstyle::option(m_state, isFocused()));
        bool focused = isFocused();
        drawThumb(buf, b, thumbX, thumbW, thumbH, tc.data(), focused);
    }


protected:
    virtual void drawTrack(JPrimitiveBuffer& buf, const JRect& b, float trackY, float trackH, float fillW) {
        JStyleOption o = jstyle::option(m_state, isFocused());
        // Track groove = ToolTipBase (old Surface3); filled portion = Highlight (old Accent).
        buf.pushRectangle(b.x, trackY, b.width, trackH, jstyle::role(JColorRole::ToolTipBase, o).data(), 2.0f);
        if (fillW > 0.5f)
            buf.pushRectangle(b.x, trackY, fillW, trackH, jstyle::role(JColorRole::Highlight, o).data(), 2.0f);
    }
    virtual void drawThumb(JPrimitiveBuffer& buf, const JRect& b, float thumbX, float thumbW, float thumbH, const uint8_t* tc, bool focused) {
        const JColor bd = jstyle::border(jstyle::option(m_state, focused));
        buf.pushRectangle(thumbX, b.y, thumbW, thumbH, tc, thumbW * 0.5f,
                          focused ? jstyle::borderW(true) : 0.0f,
                          bd.data());
    }

private:
    float m_value;
};

} // inline namespace jf
