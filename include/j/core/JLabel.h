#pragma once

// JLabel.

#include "JWidget.h"
#include <vector>
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
// JLabel
// ============================================================================

class JLabel : public JWidget {
public:
    JLabel(JSceneGraph& graph, const std::string& text, float w = 240.0f, float h = 0.0f)
        : JWidget(graph, "JLabel: " + text), m_text(text)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().labelHeight;
        l.minWidth = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(m_text) : w;
        l.minHeight = h;
    }

    void setText(const std::string& t) { m_text = t; m_graph.invalidateNode(m_nodeId, DirtySelf); notifyAccessibility(); }

    // WORD WRAP. Without it a label is one line that CLIPS at its width, so any sentence longer than
    // its box ends mid-word — and every consumer that needed a paragraph wrote its own wrapper (the
    // studio had one inside a dialog, and a second caller was about to copy it). A label is the right
    // place for this: it already knows its width, its font and its line height.
    void setWordWrap(bool on) { m_wrap = on; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    bool wordWrap() const { return m_wrap; }

    // How tall this label needs to be at `w`, wrapped. A form can ask before it lays out, which is
    // the only way a wrapped label gets the room it needs rather than the room somebody guessed.
    float heightFor(float w) const {
        if (!m_wrap || !JTextHelper::hasAtlas()) return JTextHelper::lineHeight();
        return float(_lines(tr(m_text), w).size()) * JTextHelper::lineHeight();
    }

    JA11yNode a11yNode() const override {
        JA11yNode n; _a11yFillCommon(n, JA11yRole::Label, m_text, ""); return n;
    }

    // Greedy word wrap: fill a line until the next word would not fit. A word longer than the whole
    // width is left to the renderer's own clip rather than broken mid-word — a hard break inside a
    // channel name reads as two channels.
    static std::vector<std::string> _lines(const std::string& text, float w) {
        std::vector<std::string> out;
        if (w <= 0.0f) { out.push_back(text); return out; }
        std::string line, word;
        auto flushWord = [&] {
            if (word.empty()) return;
            const std::string cand = line.empty() ? word : line + " " + word;
            if (!line.empty() && JTextHelper::measureWidth(cand) > w) { out.push_back(line); line = word; }
            else line = cand;
            word.clear();
        };
        for (const char ch : text) {
            if (ch == '\n') { flushWord(); out.push_back(line); line.clear(); }
            else if (ch == ' ') flushWord();
            else word += ch;
        }
        flushWord();
        if (!line.empty() || out.empty()) out.push_back(line);
        return out;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        // Resolve the theme's TEXT role (so a custom global palette applies), not a raw label shade; the
        // muted label look comes from the alpha, not a separate hardcoded colour.
        // jstyle::role() returns a JColor BY VALUE — keep it alive. Taking .data() from the temporary
        // left this pointer dangling the moment the expression ended (ASan: stack-use-after-scope),
        // and the bytes it then read were whatever the next call happened to leave on the stack.
        const JColor baseCol = jstyle::role(JColorRole::Text, jstyle::option(m_state, false));
        const uint8_t* base = baseCol.data();
        if (JTextHelper::hasAtlas() && m_wrap) {
            uint8_t c[4] = {base[0], base[1], base[2], 200};
            const float lh = JTextHelper::lineHeight();
            const auto lines = _lines(tr(m_text), b.width);
            // Top-aligned when wrapped: a paragraph centred in a box that is taller than it needs
            // drifts away from the row it belongs to, and a form reads by its left column.
            float ty = b.y;
            for (const std::string& ln : lines) { JTextHelper::pushText(buf, b.x, ty, ln, c, b.width); ty += lh; }
        } else if (JTextHelper::hasAtlas()) {
            uint8_t c[4] = {base[0], base[1], base[2], 200};
            float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            JTextHelper::pushText(buf, b.x, ty, tr(m_text), c, b.width);
        } else {
            // Fallback placeholder bars
            float cy = b.y + b.height * 0.5f - 3.0f;
            uint8_t c[4] = {base[0], base[1], base[2], 140};
            buf.pushRectangle(b.x, cy, b.width * 0.55f, 6.0f, c, 2.0f);
        }
    }


private:
    std::string m_text;
    bool        m_wrap = false;   // off by default: a label is one line unless it is asked to be more
};

} // inline namespace jf
