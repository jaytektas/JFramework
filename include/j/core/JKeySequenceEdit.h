#pragma once

// JKeySequenceEdit.

#include "JControl.h"
#include "JTextHelper.h"
#include "KeyEvent.h"
#include "KeySequence.h"

inline namespace jf {

// ============================================================================
// JKeySequenceEdit — a keyboard-shortcut capture box. Shows the current binding
// (e.g. "Shift+Up"); click to
// arm ("Press a key..."), and the next non-modifier keystroke is captured and
// reported via onCaptured with the raw event. The owner turns that event into
// whatever binding representation it uses and calls setText() to show the label
// — the framework owns the capture UX + rendering; the app owns the semantics.
// Sized from the scheme (controlHeight) like every other control.
// ============================================================================

class JKeySequenceEdit : public JControl {
public:
    jf::JSignal<JKeyEvent> onCaptured;   // emitted once when a keystroke is captured (capture auto-disarms)

    JKeySequenceEdit(JSceneGraph& graph, const std::string& text = "",
                     float w = 160.0f, float h = 0.0f)
        : JControl(graph, "JKeySequenceEdit"), m_text(text)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().controlHeight;
        l.minWidth = 60.0f;
        l.minHeight = h;
    }

    void setText(const std::string& t) {
        if (m_text != t) { m_text = t; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    }
    const std::string& text() const { return m_text; }

    bool capturing() const { return m_capturing; }
    void setCapturing(bool c) {
        if (m_capturing != c) { m_capturing = c; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    }

    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) { setCapturing(true); onClicked.emit(); }
        else                        setCapturing(false);
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!m_capturing || !ke.pressed) return false;
        using K = JKeyEvent::JKey;
        // A modifier-only press carries no base key and no text — stay armed and wait for the real key.
        const bool hasKey = (ke.key != K::Unknown) || (ke.utf8[0] != '\0');
        if (!hasKey) return true;
        if (ke.key == K::Escape) { setCapturing(false); return true; }   // cancel, keep the old binding
        m_capturing = false;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        onCaptured.emit(ke);
        return true;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface1, 6.0f,
                          m_capturing ? 1.5f : 1.0f,
                          m_capturing ? Colors::Accent : Colors::Border);
        const float pad = textPadding();
        const float innerX = b.x + pad, innerW = b.width - 2.0f * pad;
        if (JTextHelper::hasAtlas()) {
            const float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            if (m_capturing) {
                uint8_t pc[4] = {150, 190, 255, 210};
                JTextHelper::pushText(buf, innerX, ty, "Press a key...", pc, innerW);
            } else if (m_text.empty()) {
                uint8_t pc[4] = {100, 100, 110, 160};
                JTextHelper::pushText(buf, innerX, ty, "None", pc, innerW);
            } else {
                uint8_t tc[4] = {220, 220, 228, 220};
                JTextHelper::pushText(buf, innerX, ty, m_text, tc, innerW);
            }
        }
    }


private:
    std::string m_text;
    bool        m_capturing{false};
};

} // inline namespace jf
