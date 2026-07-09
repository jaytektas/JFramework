#pragma once

// JTabWidget — owns a JTabBar + content stack.

#include "JWidget.h"
#include "JTabBar.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
// JTabWidget — content-hosting tabs with close. A dynamic set of tabs, each holding a custom JWidget
// as its content (non-owning; the app owns the widgets). The bar draws label + a × close affordance
// per closable tab; the active tab's content fills the area below and receives input. This is the
// "tabs that come and go, each showing a custom view" primitive (e.g. a studio's surface editor).
// JTabBar (above) is the bar-only, tear-off-into-a-dock variant; this one owns its content stack.
// ============================================================================

class JTabWidget : public JWidget {
public:
    jf::JSignal<int> onTabChanged;   // (active index, or -1 when empty)
    jf::JSignal<int> onTabClosed;    // (index that was closed) — fired after removal

    JTabWidget(JSceneGraph& graph, float w = 640.0f, float h = 400.0f)
        : JWidget(graph, "JTabWidget")
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().menuItemHeight;
    }

    // content is non-owning; caller keeps it alive while the tab exists. Returns the new tab index.
    // closable and draggable are opt-in per tab: only a closable tab gets a ×; only a draggable tab
    // can be dragged to rearrange. A permanent home tab leaves both off — it stays put and stays open.
    int addTab(const std::string& label, JWidget* content, bool closable = false, bool draggable = false) {
        m_tabs.push_back({ label, content, closable, draggable });
        const int idx = (int)m_tabs.size() - 1;
        if (m_active < 0) { m_active = idx; onTabChanged.emit(m_active); }
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        return idx;
    }
    void removeTab(int idx) {
        if (idx < 0 || idx >= (int)m_tabs.size()) return;
        m_tabs.erase(m_tabs.begin() + idx);
        if (m_tabs.empty())            m_active = -1;
        else if (m_active > idx)       --m_active;
        else if (m_active >= (int)m_tabs.size()) m_active = (int)m_tabs.size() - 1;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        onTabClosed.emit(idx);
        onTabChanged.emit(m_active);
    }
    void setActiveTab(int i) {
        if (i < 0 || i >= (int)m_tabs.size() || i == m_active) return;
        m_active = i;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        onTabChanged.emit(i);
        notifyAccessibility();
    }
    int      activeTab() const { return m_active; }
    int      tabCount()  const { return (int)m_tabs.size(); }
    JWidget* content(int i) const { return (i >= 0 && i < (int)m_tabs.size()) ? m_tabs[i].content : nullptr; }
    JWidget* activeContent() const { return content(m_active); }
    void     setTabLabel(int i, const std::string& s) { if (i >= 0 && i < (int)m_tabs.size()) { m_tabs[i].label = s; m_graph.invalidateNode(m_nodeId, DirtySelf); } }

    JA11yNode a11yNode() const override {
        const std::string cur = (m_active >= 0 && m_active < (int)m_tabs.size())
                                ? m_tabs[m_active].label : std::string();
        JA11yNode n; _a11yFillCommon(n, JA11yRole::TabList, m_debugName, cur);
        n.hasRange = true; n.curValue = (float)m_active;
        n.minValue = 0.0f; n.maxValue = m_tabs.empty() ? 0.0f : (float)(m_tabs.size() - 1);
        return n;
    }

    // Pro layout config (opt-in; defaults = a Top strip, tabs at natural width).
    //   edge — which side the strip sits on (Top / Bottom / Left / Right; Left/Right run vertical).
    //   fill — how tabs occupy the strip: Fill = equal share, Left = natural width, Compress = shrink to fit.
    void setTabEdge(JTabBarEdge e) { if (m_edge != e) { m_edge = e; m_graph.invalidateNode(m_nodeId, DirtySelf); } }
    JTabBarEdge tabEdge() const { return m_edge; }
    void setTabFill(JTabFill f)   { if (m_fill != f) { m_fill = f; m_graph.invalidateNode(m_nodeId, DirtySelf); } }
    JTabFill tabFill() const { return m_fill; }
    void  setStripThickness(float t) { m_thick = t; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    float stripThickness() const { return m_thick; }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const JRect strip = _stripRect(b);
        JStyleOption so = jstyle::option(m_state, isFocused());
        const JColor stripFill  = jstyle::role(JColorRole::Base, so);         // Surface1
        const JColor activeFill  = jstyle::role(JColorRole::ToolTipBase, so);  // Surface3
        buf.pushRectangle(strip.x, strip.y, strip.width, strip.height, stripFill.data());   // strip backdrop
        _layoutTabs(b);
        const bool horiz = _horizontal();
        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            const JRect& r = m_tabRect[i];
            const bool active = (i == m_active);
            buf.pushRectangle(r.x + 1.f, r.y + 1.f, r.width - 2.f, r.height - 2.f,
                              (active ? activeFill : stripFill).data(), 5.f);
            _drawActiveEdge(buf, r, active);
            if (JTextHelper::hasAtlas()) {
                // Label text by role: active=WindowText (TextPrimary), inactive=PlaceholderText
                // (TextSecondary). Close glyph: hot=themed Danger, else PlaceholderText.
                const JColor lcRole = jstyle::role(active ? JColorRole::WindowText : JColorRole::PlaceholderText, so);
                uint8_t lc[4]; std::copy(lcRole.data(), lcRole.data() + 4, lc);
                uint8_t cc[4];
                if (i == m_hotClose) { std::copy(Colors::Danger, Colors::Danger + 4, cc); }
                else { const JColor ph = jstyle::role(JColorRole::PlaceholderText, so); std::copy(ph.data(), ph.data() + 4, cc); }
                const float lh = JTextHelper::lineHeight();
                if (horiz) {
                    JTextHelper::pushText(buf, r.x + kPadX, r.y + (r.height - lh) * 0.5f, tr(m_tabs[i].label), lc);
                    if (m_tabs[i].closable)
                        JTextHelper::pushText(buf, r.x + r.width - kCloseW + 3.f, r.y + (r.height - lh) * 0.5f, "\xC3\x97", cc);
                } else {
                    const float px = r.x + (r.width + lh) * 0.5f;   // centre the run across the strip thickness
                    JTextHelper::pushTextVertical(buf, px, r.y + kPadX, tr(m_tabs[i].label), lc, 0.f, m_edge == JTabBarEdge::Right);
                    if (m_tabs[i].closable)
                        JTextHelper::pushTextVertical(buf, px, r.y + r.height - kCloseW + 3.f, "\xC3\x97", cc, 0.f, m_edge == JTabBarEdge::Right);
                }
            }
        }
        if (JWidget* c = activeContent()) {
            c->setBounds(_contentRect(b));
            c->populateRenderPrimitives(buf);
        }
    }

    void handleMousePress(float mx, float my) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (_pointIn(_stripRect(b), mx, my)) {   // in the strip: close / select / begin a reorder drag
            _layoutTabs(b);
            for (int i = 0; i < (int)m_tabs.size(); ++i) {
                if (!_pointIn(m_tabRect[i], mx, my)) continue;
                if (m_tabs[i].closable && _inCloseBox(m_tabRect[i], mx, my)) { removeTab(i); return; }
                setActiveTab(i);
                if (m_tabs[i].draggable) { m_dragIdx = i; m_dragActive = false; m_dragPress = _along(mx, my); }
                return;
            }
            return;
        }
        m_capContent = true;
        if (JWidget* c = activeContent()) c->handleMousePress(mx, my);
    }
    void handleMouseMove(float mx, float my) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (m_dragIdx >= 0) {                     // reordering a draggable tab
            const float a = _along(mx, my);
            if (!m_dragActive && std::abs(a - m_dragPress) > kDragThresh) m_dragActive = true;
            if (m_dragActive) {
                const int target = _tabIndexAt(b, a);
                if (target >= 0 && target != m_dragIdx) { _moveTab(m_dragIdx, target); m_dragIdx = target; }
            }
            return;
        }
        int hot = -1;
        if (_pointIn(_stripRect(b), mx, my)) {
            _layoutTabs(b);
            for (int i = 0; i < (int)m_tabs.size(); ++i)
                if (m_tabs[i].closable && _inCloseBox(m_tabRect[i], mx, my)) { hot = i; break; }
        }
        if (hot != m_hotClose) { m_hotClose = hot; m_graph.invalidateNode(m_nodeId, DirtySelf); }
        if (JWidget* c = activeContent()) c->handleMouseMove(mx, my);
    }
    void handleMouseRelease(float mx, float my) override {
        if (m_dragIdx >= 0) { m_dragIdx = -1; m_dragActive = false; return; }
        if (m_capContent) { if (JWidget* c = activeContent()) c->handleMouseRelease(mx, my); m_capContent = false; return; }
        // A cross-widget drag (e.g. a Dictionary binding dropped onto the canvas) ends with a release that
        // had no preceding press here — so nothing captured. Still deliver it to the active content when the
        // cursor is over the content area, so the drop resolves on release instead of hanging until a click.
        if (JDragDrop::isDragging()) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            if (_pointIn(_contentRect(b), mx, my)) if (JWidget* c = activeContent()) c->handleMouseRelease(mx, my);
        }
    }
    bool handleScroll(float mx, float my, float wheel) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (_pointIn(_contentRect(b), mx, my)) if (JWidget* c = activeContent()) return c->handleScroll(mx, my, wheel);
        return false;
    }
    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (JWidget* c = activeContent()) return c->handleKeyEvent(ke);
        return false;
    }


private:
    struct Tab { std::string label; JWidget* content; bool closable; bool draggable; };

    bool  _horizontal() const { return m_edge == JTabBarEdge::Top || m_edge == JTabBarEdge::Bottom; }
    float _along(float mx, float my) const { return _horizontal() ? mx : my; }
    static bool _pointIn(const JRect& r, float x, float y) {
        return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
    }
    JRect _stripRect(const JRect& b) const {
        switch (m_edge) {
            case JTabBarEdge::Bottom: return { b.x, b.y + b.height - m_thick, b.width, m_thick };
            case JTabBarEdge::Left:   return { b.x, b.y, m_thick, b.height };
            case JTabBarEdge::Right:  return { b.x + b.width - m_thick, b.y, m_thick, b.height };
            default:                  return { b.x, b.y, b.width, m_thick };   // Top
        }
    }
    JRect _contentRect(const JRect& b) const {
        switch (m_edge) {
            case JTabBarEdge::Bottom: return { b.x, b.y, b.width, b.height - m_thick };
            case JTabBarEdge::Left:   return { b.x + m_thick, b.y, b.width - m_thick, b.height };
            case JTabBarEdge::Right:  return { b.x, b.y, b.width - m_thick, b.height };
            default:                  return { b.x, b.y + m_thick, b.width, b.height - m_thick };   // Top
        }
    }
    // Close box sits at the far (trailing) end of a tab along the strip axis.
    bool _inCloseBox(const JRect& r, float mx, float my) const {
        return _horizontal() ? (mx >= r.x + r.width - kCloseW && _pointIn(r, mx, my))
                             : (my >= r.y + r.height - kCloseW && _pointIn(r, mx, my));
    }
    void _layoutTabs(const JRect& b) {
        m_tabRect.assign(m_tabs.size(), JRect{});
        if (m_tabs.empty()) return;
        const bool horiz = _horizontal();
        const float L = (horiz ? b.width : b.height) - 8.f;   // usable strip length (4px pad each end)
        std::vector<float> nat(m_tabs.size());
        float sum = 0.f;
        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            const float lw = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(tr(m_tabs[i].label)) : 60.f;
            nat[i] = kPadX * 2.f + lw + (m_tabs[i].closable ? kCloseW : 0.f);
            sum += nat[i];
        }
        std::vector<float> sz(m_tabs.size());
        if (m_fill == JTabFill::Fill) {
            const float each = L / (float)m_tabs.size();
            for (auto& s : sz) s = each;
        } else if (m_fill == JTabFill::Compress) {
            const float k = (sum > L && sum > 0.f) ? L / sum : 1.f;   // shrink to fit, never past natural
            for (int i = 0; i < (int)m_tabs.size(); ++i) sz[i] = nat[i] * k;
        } else {                                                     // Left: natural width
            sz = nat;
        }
        float along = 4.f;
        const float T = m_thick;
        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            const float s = sz[i];
            switch (m_edge) {
                case JTabBarEdge::Bottom: m_tabRect[i] = { b.x + along, b.y + b.height - T, s, T }; break;
                case JTabBarEdge::Left:   m_tabRect[i] = { b.x, b.y + along, T, s }; break;
                case JTabBarEdge::Right:  m_tabRect[i] = { b.x + b.width - T, b.y + along, T, s }; break;
                default:                  m_tabRect[i] = { b.x + along, b.y, s, T }; break;   // Top
            }
            along += s + 2.f;
        }
    }
    int _tabIndexAt(const JRect& b, float along) {
        _layoutTabs(b);
        if (m_tabs.empty()) return -1;
        const bool horiz = _horizontal();
        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            const float lo = horiz ? m_tabRect[i].x : m_tabRect[i].y;
            const float hi = lo + (horiz ? m_tabRect[i].width : m_tabRect[i].height);
            if (along >= lo && along < hi) return i;
        }
        const float firstLo = horiz ? m_tabRect[0].x : m_tabRect[0].y;
        return (along < firstLo) ? 0 : (int)m_tabs.size() - 1;
    }
    void _moveTab(int from, int to) {
        if (from == to || from < 0 || to < 0 || from >= (int)m_tabs.size() || to >= (int)m_tabs.size()) return;
        Tab t = m_tabs[from];
        m_tabs.erase(m_tabs.begin() + from);
        m_tabs.insert(m_tabs.begin() + to, t);
        if (m_active == from)                 m_active = to;
        else if (from < m_active && m_active <= to) --m_active;
        else if (to <= m_active && m_active < from) ++m_active;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    void _drawActiveEdge(JPrimitiveBuffer& buf, const JRect& r, bool active) const {
        if (!active) return;
        // Accent line on the content-facing edge of the active tab — Highlight role.
        const JColor a = jstyle::role(JColorRole::Highlight, jstyle::option(m_state, isFocused()));
        switch (m_edge) {
            case JTabBarEdge::Bottom: buf.pushRectangle(r.x + 4.f, r.y, r.width - 8.f, 2.f, a.data(), 1.f); break;
            case JTabBarEdge::Left:   buf.pushRectangle(r.x + r.width - 2.5f, r.y + 4.f, 2.f, r.height - 8.f, a.data(), 1.f); break;
            case JTabBarEdge::Right:  buf.pushRectangle(r.x, r.y + 4.f, 2.f, r.height - 8.f, a.data(), 1.f); break;
            default:                  buf.pushRectangle(r.x + 4.f, r.y + r.height - 2.5f, r.width - 8.f, 2.f, a.data(), 1.f); break;
        }
    }

    std::vector<Tab>   m_tabs;
    std::vector<JRect> m_tabRect;
    int         m_active   = -1;
    int         m_hotClose = -1;
    bool        m_capContent = false;
    int         m_dragIdx  = -1;
    bool        m_dragActive = false;
    float       m_dragPress = 0.f;
    JTabBarEdge m_edge = JTabBarEdge::Top;
    JTabFill    m_fill = JTabFill::Left;
    float       m_thick = 34.0f;
    static constexpr float kPadX = 12.0f, kCloseW = 18.0f, kDragThresh = 6.0f;
};

} // inline namespace jf
