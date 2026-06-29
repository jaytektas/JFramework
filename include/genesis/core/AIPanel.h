#pragma once

#include "BaseWidgets.h"
#include <vector>
#include <string>
#include <deque>
#include <algorithm>

inline namespace jf {

// ============================================================================
// JAIPanel — AI companion side panel.
//
// Shows the live semantic widget tree (what the AI sees), a scrollable action
// log of recent AI operations, and a text input for issuing AI commands.
//
// Typical usage — add as a JDockWidget content or as a side child of the root:
//
//   auto* ai = graph.add<JAIPanel>();
//   ai->onCommand.connect([](const std::string& cmd) {
//       myAiDispatch(cmd);
//   });
//
// Call ai->logAction("did X") from your AI bus callback to append to the log.
// ============================================================================
class JAIPanel : public JWidget {
public:
    static constexpr float HEADER_H  = 28.f;
    static constexpr float SECTION_H = 18.f;
    static constexpr float ITEM_H    = 22.f;
    static constexpr float LOG_ROWS  = 4.f;
    static constexpr float INPUT_H   = 32.f;
    static constexpr float SEND_W    = 32.f;

    JAIPanel(JSceneGraph& graph)
        : JWidget(graph, "JAIPanel")
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.minWidth  = 220.f;
        l.minHeight = 300.f;
        l.flexGrow  = 1.f;
    }

    // Append an entry to the action log. Thread-safe to call from callbacks.
    void logAction(const std::string& entry) {
        m_log.push_front(entry);
        if (m_log.size() > 64) m_log.pop_back();
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    // Fires when the user submits a command via the input field or Enter key.
    jf::JSignal<std::string> onCommand;

    JAISemanticNode getSemanticNode() const override {
        return {"JAIPanel", "AI Panel", "", false};
    }
    bool executeSemanticAction(const std::string&) override { return false; }
    bool isFocusable() const override { return true; }

    // ── Rendering ────────────────────────────────────────────────────────────

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        if (!m_visible) return;
        const auto& bb = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float x = bb.x, y = bb.y, w = bb.width, h = bb.height;

        // Background
        buf.pushRectangle(x, y, w, h, Colors::Surface0, 0.f, 1.f, Colors::Border);

        // ── Header ──────────────────────────────────────────────────────────
        buf.pushRectangle(x, y, w, HEADER_H, Colors::Surface2, 0.f);
        if (JTextHelper::hasAtlas()) {
            uint8_t tc[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 255};
            JTextHelper::pushText(buf,
                x + 10.f, y + (HEADER_H - JTextHelper::lineHeight()) * 0.5f,
                "AI  \xe2\x80\xa2  Genesis", tc);
            // Status dot
            uint8_t dot[4] = {Colors::Success[0], Colors::Success[1], Colors::Success[2], 200};
            buf.pushRectangle(x + w - 16.f, y + (HEADER_H - 8.f) * 0.5f, 8.f, 8.f, dot, 4.f);
        }

        float cy = y + HEADER_H + 4.f;

        // ── JWidget tree ──────────────────────────────────────────────────────
        _sectionLabel(buf, x, cy, w, "WIDGET TREE");
        cy += SECTION_H;

        float logTop    = y + h - INPUT_H - LOG_ROWS * ITEM_H - SECTION_H - 12.f;
        int   maxItems  = std::max(0, static_cast<int>((logTop - cy) / ITEM_H));
        int   shown     = 0;

        for (auto* widget : JWidget::s_activeWidgets) {
            if (shown >= maxItems) break;
            if (!widget->isVisible()) continue;
            auto node = widget->getSemanticNode();
            if (node.role == "JAIPanel" || node.role == "JSeparator") continue;

            bool hov = (shown == m_hoveredTreeIdx);
            if (hov) {
                buf.pushRectangle(x + 4.f, cy, w - 8.f, ITEM_H,
                                  Colors::Surface2, 3.f);
            }

            if (JTextHelper::hasAtlas()) {
                uint8_t roleC[4] = {Colors::Accent[0], Colors::Accent[1],
                                    Colors::Accent[2], 170};
                uint8_t lblC[4]  = {Colors::TextPrimary[0], Colors::TextPrimary[1],
                                    Colors::TextPrimary[2], 200};
                float ty = cy + (ITEM_H - JTextHelper::lineHeight()) * 0.5f;
                float roleW = w * 0.38f;
                JTextHelper::pushText(buf, x + 10.f, ty, node.role, roleC, roleW);
                std::string lbl = node.label;
                if (!node.value.empty()) lbl += " = " + node.value;
                JTextHelper::pushText(buf, x + 10.f + roleW + 4.f, ty, lbl, lblC,
                                     w - roleW - 22.f);
            }
            cy += ITEM_H;
            ++shown;
        }

        // Divider
        cy = logTop;
        buf.pushRectangle(x + 8.f, cy, w - 16.f, 1.f, Colors::Border, 0.f);
        cy += 5.f;

        // ── JAction log ───────────────────────────────────────────────────────
        _sectionLabel(buf, x, cy, w, "ACTION LOG");
        cy += SECTION_H;

        for (size_t i = 0; i < m_log.size() && i < static_cast<size_t>(LOG_ROWS); ++i) {
            if (JTextHelper::hasAtlas()) {
                uint8_t lc[4] = {Colors::TextSecondary[0], Colors::TextSecondary[1],
                                  Colors::TextSecondary[2], 190};
                JTextHelper::pushText(buf, x + 10.f,
                    cy + (ITEM_H - JTextHelper::lineHeight()) * 0.5f,
                    "> " + m_log[i], lc, w - 18.f);
            }
            cy += ITEM_H;
        }

        // ── Input row ────────────────────────────────────────────────────────
        float iy = y + h - INPUT_H;
        buf.pushRectangle(x, iy, w, INPUT_H, Colors::Surface1, 0.f, 1.f, Colors::Border);

        // Send button
        float bx = x + w - SEND_W - 4.f;
        float by = iy + 4.f;
        const uint8_t* btnFill = m_sendHovered ? Colors::AccentHover : Colors::Accent;
        buf.pushRectangle(bx, by, SEND_W, INPUT_H - 8.f, btnFill, 4.f);
        if (JTextHelper::hasAtlas()) {
            uint8_t wc[4] = {255, 255, 255, 230};
            float arrowW = JTextHelper::measureWidth(">");
            JTextHelper::pushText(buf, bx + (SEND_W - arrowW) * 0.5f,
                iy + (INPUT_H - JTextHelper::lineHeight()) * 0.5f, ">", wc);
        }

        // Input text or placeholder
        if (JTextHelper::hasAtlas()) {
            bool placeholder = m_input.empty();
            uint8_t tc[4] = {
                placeholder ? Colors::TextSecondary[0] : Colors::TextPrimary[0],
                placeholder ? Colors::TextSecondary[1] : Colors::TextPrimary[1],
                placeholder ? Colors::TextSecondary[2] : Colors::TextPrimary[2],
                placeholder ? (uint8_t)110 : (uint8_t)230
            };
            JTextHelper::pushText(buf, x + 8.f,
                iy + (INPUT_H - JTextHelper::lineHeight()) * 0.5f,
                placeholder ? "Enter AI command..." : m_input,
                tc, w - SEND_W - 20.f);

            // Cursor blink when focused
            if (m_focused && !placeholder) {
                float curX = x + 8.f + JTextHelper::measureWidth(m_input);
                uint8_t cur[4] = {Colors::Accent[0], Colors::Accent[1],
                                   Colors::Accent[2], 220};
                buf.pushRectangle(curX, iy + 8.f, 1.5f, INPUT_H - 16.f, cur, 0.f);
            }
        }
    }

    // ── Input dispatch ───────────────────────────────────────────────────────

    void handleMouseMove(float mx, float my) override {
        const auto& bb = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float bx = bb.x + bb.width - SEND_W - 4.f;
        float iy = bb.y + bb.height - INPUT_H;
        bool nowHov = (mx >= bx && mx <= bx + SEND_W && my >= iy + 4.f && my <= iy + INPUT_H - 4.f);
        if (nowHov != m_sendHovered) {
            m_sendHovered = nowHov;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        }

        // Track which tree row the mouse is over
        float cy = bb.y + HEADER_H + 4.f + SECTION_H;
        int idx = static_cast<int>((my - cy) / ITEM_H);
        int newHov = (my >= cy && my < bb.y + bb.height - INPUT_H - LOG_ROWS * ITEM_H - SECTION_H - 12.f)
                     ? idx : -1;
        if (newHov != m_hoveredTreeIdx) {
            m_hoveredTreeIdx = newHov;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        }
    }

    void handleMousePress(float mx, float my) override {
        const auto& bb = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float bx = bb.x + bb.width - SEND_W - 4.f;
        float iy = bb.y + bb.height - INPUT_H;
        bool inSend = (mx >= bx && mx <= bx + SEND_W && my >= iy && my <= iy + INPUT_H);
        if (inSend) _submit();
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed || !m_focused) return false;
        using K = JKeyEvent::JKey;
        if (ke.key == K::Return)    { _submit(); return true; }
        if (ke.key == K::Backspace) {
            if (!m_input.empty()) {
                m_input.pop_back();
                m_graph.invalidateNode(m_nodeId, DirtySelf);
            }
            return true;
        }
        if (ke.utf8[0] >= 0x20 && ke.utf8[0] != 0) {
            m_input += ke.utf8[0];
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        }
        return false;
    }

private:
    void _submit() {
        if (m_input.empty()) return;
        onCommand.emit(m_input);
        logAction(m_input);
        m_input.clear();
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    void _sectionLabel(JPrimitiveBuffer& buf, float x, float y, float /*w*/, const char* label) {
        if (!JTextHelper::hasAtlas()) return;
        uint8_t c[4] = {Colors::TextSecondary[0], Colors::TextSecondary[1],
                        Colors::TextSecondary[2], 130};
        JTextHelper::pushText(buf, x + 8.f, y + 2.f, label, c);
    }

    std::string          m_input;
    std::deque<std::string> m_log;
    bool                 m_sendHovered{false};
    int                  m_hoveredTreeIdx{-1};
};

} // inline namespace jf
