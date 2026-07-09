#pragma once

// JTabBar (+ JTornTabState).

#include "JControl.h"
#include "JTextHelper.h"
#include "DragDrop.h"

inline namespace jf {

// ============================================================================
// JTornTabState — provenance carried by a JDockWidget born from a tear-off.
// Value type: no vtable, cheap to move.
// ============================================================================

struct JTornTabState {
    std::string title;
    int         originIndex{-1};
    NodeId      contentNode{InvalidNodeId};
    // Called when the JDockWidget is dragged back over a valid DockZone.
    // Args: (originIndex, contentNode) — the bar re-inserts the tab.
    std::function<void(int, NodeId)> reattach;
};

// ============================================================================
// JTabBar
// ============================================================================

class JTabBar : public JControl {
public:
    static constexpr float TEAR_THRESHOLD = 8.0f; // px of movement to initiate a tear

    jf::JSignal<int>          onTabChanged;
    jf::JSignal<int, NodeId>  onTabTorn;   // (tabIndex, contentNode) — app should create a JDockWidget

    JTabBar(JSceneGraph& graph, std::vector<std::string> tabs = {},
           float w = 400.0f, float h = 0.0f)
        : JControl(graph, "JTabBar"), m_tabs(std::move(tabs))
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().menuItemHeight;
        if (!m_tabs.empty()) m_activeIndex = 0;
        _updateMinSize();
    }

    void addTab(const std::string& label, NodeId contentNode = InvalidNodeId) {
        m_tabs.push_back(label);
        m_contentNodes.push_back(contentNode);
        if (m_activeIndex < 0) m_activeIndex = 0;
        _updateMinSize();
    }

    void setActiveTab(int i) {
        if (i < 0 || i >= (int)m_tabs.size() || i == m_activeIndex) return;
        m_activeIndex = i;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        onTabChanged.emit(i);
        notifyAccessibility();
    }
    int activeTab() const { return m_activeIndex; }

    JA11yNode a11yNode() const override {
        const std::string cur = (m_activeIndex >= 0 && m_activeIndex < (int)m_tabs.size())
                                ? m_tabs[m_activeIndex] : std::string();
        JA11yNode n; _a11yFillCommon(n, JA11yRole::TabList, m_debugName, cur);
        n.hasRange = true; n.curValue = (float)m_activeIndex;
        n.minValue = 0.0f; n.maxValue = m_tabs.empty() ? 0.0f : (float)(m_tabs.size() - 1);
        return n;
    }

    // --- Tearable mode ---
    void setTearable(bool t) { m_tearable = t; }
    bool isTearable() const  { return m_tearable; }

    // Associate a JSceneGraph content subtree with a tab (required for tear-off)
    void setTabContentNode(int index, NodeId node) {
        if (index >= 0 && index < (int)m_contentNodes.size())
            m_contentNodes[index] = node;
    }

    // Peek whether a tab was torn this frame — consumed by the application
    bool hasTornTab() const { return m_pendingTorn.has_value(); }
    JTornTabState consumeTornTab() {
        auto s = std::move(*m_pendingTorn);
        m_pendingTorn.reset();
        return s;
    }

    // Last drag cursor position — used to place the new JDockWidget on creation
    float lastDragX() const { return m_drag.curX; }
    float lastDragY() const { return m_drag.curY; }

    // Called by the app when a JDockWidget is re-docked onto this bar
    void reinsertTab(int originIndex, NodeId contentNode, const std::string& title) {
        int clampedIdx = std::clamp(originIndex, 0, (int)m_tabs.size());
        m_tabs.insert(m_tabs.begin() + clampedIdx, title);
        m_contentNodes.insert(m_contentNodes.begin() + clampedIdx, contentNode);
        if (m_activeIndex >= clampedIdx) m_activeIndex++;
        setActiveTab(clampedIdx);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        _updateMinSize();
    }

    // Draw a translucent ghost tab at the current drag position (call from render loop)
    void populateDragGhost(JPrimitiveBuffer& buf) const {
        if (!m_drag.active || !m_drag.tornOff) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float tabW = m_tabs.empty() ? 120.0f : b.width / static_cast<float>(m_tabs.size() + 1);
        float gx = m_drag.curX - tabW * 0.5f;
        float gy = m_drag.curY - b.height * 0.5f;
        buf.pushRectangle(gx, gy, tabW, b.height, Colors::TabGhostFill,
                          JStyle::current().hint(JStyleHint::ControlRadius), 1.5f, Colors::TabGhostBorder);
        const uint8_t* lc = Colors::TabGhostBar;
        buf.pushRectangle(gx + 8.0f, gy + (b.height - 6.0f) * 0.5f, tabW - 16.0f, 6.0f, lc, 2.0f);
    }

    // --- Input handling ---
    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my) || m_tabs.empty()) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float tabW = b.width / static_cast<float>(m_tabs.size());
        int idx = std::clamp(static_cast<int>((mx - b.x) / tabW), 0, (int)m_tabs.size()-1);

        if (m_tearable) {
            m_drag = { idx, mx, my, mx, my, false, false };
        } else {
            setActiveTab(idx);
        }
    }

    void handleMouseMove(float mx, float my) override {
        JControl::handleMouseMove(mx, my);
        if (!m_drag.active && m_drag.pressedIndex < 0) return;

        m_drag.curX = mx;
        m_drag.curY = my;

        float dx = mx - m_drag.pressX;
        float dy = my - m_drag.pressY;
        float dist = std::sqrt(dx*dx + dy*dy);

        if (!m_drag.active && dist > TEAR_THRESHOLD) {
            m_drag.active = true;
            setActiveTab(m_drag.pressedIndex);
        }

        if (m_drag.active && !m_drag.tornOff) {
            // Cross the vertical threshold to commit the tear
            if (std::abs(dy) > TEAR_THRESHOLD * 2.0f) {
                m_drag.tornOff = true;
                _emitTear(m_drag.pressedIndex);
            }
        }
    }

    void handleMouseRelease(float mx, float my) override {
        if (m_drag.pressedIndex >= 0 && !m_drag.active) {
            // Short press with no drag — just select the tab
            setActiveTab(m_drag.pressedIndex);
        }
        m_drag = {};
        JControl::handleMouseRelease(mx, my);
    }

    // Programmatically tear off a tab — useful for keyboard shortcuts and testing.
    void forceTear(int idx) { _emitTear(idx); }

    // Close a tab outright (dynamic tab sets: open on demand, close when done). Unlike a tear this
    // doesn't spawn a float — the tab is gone. Emits onTabChanged with the new active index (-1 when
    // the bar empties) so the host can swap the shown content.
    void removeTab(int idx) {
        if (idx < 0 || idx >= (int)m_tabs.size()) return;
        m_tabs.erase(m_tabs.begin() + idx);
        if (idx < (int)m_contentNodes.size()) m_contentNodes.erase(m_contentNodes.begin() + idx);
        if (m_tabs.empty())                        m_activeIndex = -1;
        else if (m_activeIndex >= (int)m_tabs.size()) m_activeIndex = (int)m_tabs.size() - 1;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        _updateMinSize();
        onTabChanged.emit(m_activeIndex);
    }
    int         tabCount() const { return (int)m_tabs.size(); }
    std::string tabLabel(int idx) const { return (idx >= 0 && idx < (int)m_tabs.size()) ? m_tabs[idx] : std::string(); }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        if (m_tabs.empty()) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float tabW = b.width / static_cast<float>(m_tabs.size());

        bool focused = isFocused();
        JStyleOption so = jstyle::option(m_state, focused);
        // Strip = Base role; Accent ring when focused (borderW 0 when not, as before).
        buf.pushRectangle(b.x, b.y, b.width, b.height, jstyle::fieldFill(so).data(),
                          JStyle::current().hint(JStyleHint::ControlRadius),
                          focused ? jstyle::borderW(true) : 0.0f, jstyle::border(so).data());

        const JColor baseFill   = jstyle::role(JColorRole::Base, so);        // Surface1
        const JColor activeFill  = jstyle::role(JColorRole::ToolTipBase, so); // Surface3
        const JColor accent      = jstyle::role(JColorRole::Highlight, so);   // Accent
        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            float tx   = b.x + i * tabW;
            bool active = (i == m_activeIndex);
            // Highlight the tab being dragged (pre-tear)
            bool dragging = m_drag.active && !m_drag.tornOff && (i == m_drag.pressedIndex);

            // dragging => themed AccentPress (no palette role); active => Surface3; else Base.
            const JColor fill = dragging ? JColor::fromArray(Colors::AccentPress)
                                : active  ? activeFill
                                          : baseFill;
            float radius = (i == 0) ? 6.0f : (i == (int)m_tabs.size()-1) ? 6.0f : 0.0f;
            buf.pushRectangle(tx + 1.0f, b.y + 1.0f, tabW - 2.0f, b.height - 2.0f, fill.data(), radius);

            if (active && !dragging)
                buf.pushRectangle(tx + 4.0f, b.y + b.height - 3.0f, tabW - 8.0f, 2.5f, accent.data(), 1.0f);

            // Tearable indicator: small dot in top-right of each tab when tearable
            if (m_tearable && !active) {
                buf.pushRectangle(tx + tabW - 8.0f, b.y + 5.0f, 3.0f, 3.0f, Colors::TabTearDot, 1.5f);
            }

            if (JTextHelper::hasAtlas()) {
                std::string label = tr(m_tabs[i]);
                float lw = JTextHelper::measureWidth(label);
                // active label = ControlText, inactive = TabInactiveText; alpha follows the active state.
                const uint8_t* lbase = active ? Colors::ControlText : Colors::TabInactiveText;
                uint8_t lc[4] = {lbase[0], lbase[1], lbase[2], active ? (uint8_t)220 : (uint8_t)140};
                float ly = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
                JTextHelper::pushText(buf, tx + (tabW - lw) * 0.5f, ly, label, lc);
            } else {
                const uint8_t* lbase = active ? Colors::ControlText : Colors::TabInactiveText;
                uint8_t lc[4] = {lbase[0], lbase[1], lbase[2], active ? (uint8_t)220 : (uint8_t)140};
                float lw = tabW * 0.5f;
                buf.pushRectangle(tx + (tabW - lw)*0.5f, b.y + (b.height-6.0f)*0.5f, lw, 6.0f, lc, 2.0f);
            }
        }
    }


private:
    void _updateMinSize() {
        auto& l = m_graph.getLayout(m_nodeId);
        float totalTextW = 0.0f;
        for (const auto& tab : m_tabs) {
            float lw = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(tr(tab)) : 50.0f;
            totalTextW += lw + 24.0f;
        }
        l.minWidth = totalTextW + 8.0f;
        l.minHeight = m_graph.getLayoutConst(m_nodeId).boundingBox.height;
    }

    void _emitTear(int idx) {
        if (idx < 0 || idx >= (int)m_tabs.size()) return;

        std::string title     = m_tabs[idx];
        NodeId      content   = (idx < (int)m_contentNodes.size()) ? m_contentNodes[idx] : InvalidNodeId;

        // Remove the tab from the bar
        m_tabs.erase(m_tabs.begin() + idx);
        if (idx < (int)m_contentNodes.size()) m_contentNodes.erase(m_contentNodes.begin() + idx);
        m_activeIndex = m_tabs.empty() ? -1 : std::clamp(m_activeIndex, 0, (int)m_tabs.size()-1);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        _updateMinSize();

        // Build JTornTabState with a re-attach closure bound to this JTabBar
        JTornTabState state;
        state.title       = title;
        state.originIndex = idx;
        state.contentNode = content;
        state.reattach    = [this, title](int origIdx, NodeId node) {
            reinsertTab(origIdx, node, title);
        };

        m_pendingTorn = std::move(state);
        onTabTorn.emit(idx, content);
    }

    struct JDragState {
        int   pressedIndex{-1};
        float pressX{0}, pressY{0};
        float curX{0},   curY{0};
        bool  active{false};
        bool  tornOff{false};
    };

    std::vector<std::string> m_tabs;
    std::vector<NodeId>      m_contentNodes;
    int    m_activeIndex{-1};
    bool   m_tearable{false};
    JDragState                m_drag{};
    std::optional<JTornTabState> m_pendingTorn;
};

} // inline namespace jf
