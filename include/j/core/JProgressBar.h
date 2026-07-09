#pragma once

// JProgressBar.

#include "JWidget.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
// JProgressBar
// ============================================================================

class JProgressBar : public JWidget {
public:
    JProgressBar(JSceneGraph& graph, float w = 280.0f, float h = 12.0f)
        : JWidget(graph, "JProgressBar"), m_progress(0.0f)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().menuItemHeight;
        l.minWidth = 50.0f;
        l.minHeight = h;
    }

    void setProgress(float p) { m_progress = std::clamp(p, 0.0f, 1.0f); m_graph.invalidateNode(m_nodeId, DirtySelf); notifyAccessibility(); }
    float getProgress() const { return m_progress; }

    JA11yNode a11yNode() const override {
        char v[24]; std::snprintf(v, sizeof(v), "%d%%", static_cast<int>(m_progress * 100.0f + 0.5f));
        JA11yNode n; _a11yFillCommon(n, JA11yRole::ProgressBar, m_debugName, v);
        n.hasRange = true; n.curValue = m_progress; n.minValue = 0.0f; n.maxValue = 1.0f;
        n.stateFlags |= JA11yReadOnly;
        return n;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        drawTrack(buf, b);
        drawProgressFill(buf, b, m_progress);
    }


protected:
    virtual void drawTrack(JPrimitiveBuffer& buf, const JRect& b) {
        // Trough = Button role (old Surface2).
        const JColor trough = jstyle::role(JColorRole::Button, jstyle::option(m_state, isFocused()));
        buf.pushRectangle(b.x, b.y, b.width, b.height, trough.data(),
                          JTheme::current().hint(JStyleHint::ControlRadius));
    }
    virtual void drawProgressFill(JPrimitiveBuffer& buf, const JRect& b, float progress) {
        // Progress fill is a status colour (Success) with no palette role — kept on its
        // themed JTheme field so it still follows a theme swap.
        if (progress > 0.005f)
            buf.pushRectangle(b.x, b.y, b.width * progress, b.height, Colors::Success,
                              JTheme::current().hint(JStyleHint::ControlRadius));
    }

private:
    float m_progress;
};

} // inline namespace jf
