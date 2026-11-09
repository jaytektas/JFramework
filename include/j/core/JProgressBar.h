// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

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
    // What the fill says about the work, as a STYLE VARIANT: each tone is one of the theme's status
    // colours, so one bar can show different kinds of progress (rendering, then checking) and a theme
    // decides what each looks like. Success is the default and the look every bar had before tones.
    enum class Tone { Success, Accent, Warning, Danger };

    JProgressBar(JSceneGraph& graph, float w = 280.0f, float h = 12.0f)
        : JWidget(graph, "JProgressBar"), m_progress(0.0f)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().menuItemHeight;
        l.minWidth = 50.0f;
        l.minHeight = h;
    }

    void setProgress(float p) { m_progress = std::clamp(p, 0.0f, 1.0f); m_graph.invalidateNode(m_nodeId, DirtySelf); notifyAccessibility(); }
    float getProgress() const { return m_progress; }

    void setTone(Tone t) { if (t != m_tone) { m_tone = t; m_graph.invalidateNode(m_nodeId, DirtySelf); } }
    Tone tone() const    { return m_tone; }

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
                          JStyle::current().hint(JStyleHint::ControlRadius));
    }
    virtual void drawProgressFill(JPrimitiveBuffer& buf, const JRect& b, float progress) {
        // Progress fill is a status colour (the tone's) with no palette role — kept on its
        // themed JStyle field so it still follows a theme swap.
        if (progress > 0.005f)
            buf.pushRectangle(b.x, b.y, b.width * progress, b.height, toneColor(),
                              JStyle::current().hint(JStyleHint::ControlRadius));
    }

    const uint8_t* toneColor() const {
        switch (m_tone) {
            case Tone::Accent:  return Colors::Accent;
            case Tone::Warning: return Colors::Warning;
            case Tone::Danger:  return Colors::Danger;
            case Tone::Success: break;
        }
        return Colors::Success;
    }

private:
    float m_progress;
    Tone  m_tone = Tone::Success;
};

} // inline namespace jf
