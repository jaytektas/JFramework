#pragma once

// JListView.

#include "JControl.h"
#include "JTextHelper.h"
#include "KeyEvent.h"

inline namespace jf {

// ============================================================================
// JListView
// ============================================================================

class JListView : public JControl {
public:
    jf::JSignal<int> onSelectionChanged;
    jf::JSignal<int> onItemActivated;

    JListView(JSceneGraph& graph, std::vector<std::string> items = {},
             float w = 240.0f, float h = 200.0f)
        : JControl(graph, "JListView"), m_items(std::move(items))
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().menuItemHeight;
        l.minWidth = 80.0f;
        l.minHeight = 40.0f;
    }

    void setItems(const std::vector<std::string>& items) {
        m_items = items;
        m_selectedIndex = (m_items.empty()) ? -1 : std::clamp(m_selectedIndex, 0, (int)m_items.size()-1);
        m_scrollY = 0.0f;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    const std::vector<std::string>& items() const { return m_items; }

    void setSelectedIndex(int index) {
        int nextIdx = (m_items.empty()) ? -1 : std::clamp(index, 0, (int)m_items.size()-1);
        if (m_selectedIndex != nextIdx) {
            m_selectedIndex = nextIdx;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onSelectionChanged.emit(nextIdx);
            notifyAccessibility();
        }
    }
    int selectedIndex() const { return m_selectedIndex; }

    JA11yNode a11yNode() const override {
        const std::string cur = (m_selectedIndex >= 0 && m_selectedIndex < (int)m_items.size())
                                ? m_items[m_selectedIndex] : std::string();
        JA11yNode n; _a11yFillCommon(n, JA11yRole::List, m_debugName, cur);
        n.hasRange = true; n.curValue = (float)m_selectedIndex;
        n.minValue = 0.0f; n.maxValue = m_items.empty() ? 0.0f : (float)(m_items.size() - 1);
        if (m_selectedIndex >= 0) n.stateFlags |= JA11ySelected;
        return n;
    }

    void handleMouseMove(float mx, float my) override {
        if (m_draggingScroll) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            float itemH = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() + 8.0f : 20.0f;
            float totalH = m_items.size() * itemH + 8.0f;
            float maxScrollY = std::max(0.0f, totalH - b.height);
            float trackH = b.height - 4.0f;
            float thumbH = std::max(20.0f, trackH * (b.height / totalH));
            float thumbRange = trackH - thumbH;
            if (thumbRange > 0.0f) {
                m_scrollY = std::clamp(m_dragStartScrollY + (my - m_dragStartY) * maxScrollY / thumbRange, 0.0f, maxScrollY);
                m_graph.invalidateNode(m_nodeId, DirtySelf);
            }
            return;
        }
        JControl::handleMouseMove(mx, my);
    }

    void handleMousePress(float mx, float my) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            onClicked.emit();
            float trackW = 10.0f;
            float trackX = b.x + b.width - trackW - 6.0f;
            if (mx >= trackX) {
                m_draggingScroll = true;
                m_dragStartY = my;
                m_dragStartScrollY = m_scrollY;
            } else {
                float itemH = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() + 8.0f : 20.0f;
                float relativeY = my - b.y + m_scrollY - 4.0f;
                int clickedIndex = static_cast<int>(relativeY / itemH);
                if (clickedIndex >= 0 && clickedIndex < (int)m_items.size()) {
                    setSelectedIndex(clickedIndex);
                    onItemActivated.emit(clickedIndex);
                }
            }
        }
    }

    void handleMouseRelease(float mx, float my) override {
        m_draggingScroll = false;
        JControl::handleMouseRelease(mx, my);
    }

    bool handleScroll(float mx, float my, float wheel) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            float itemH = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() + 8.0f : 20.0f;
            float totalH = m_items.size() * itemH + 8.0f;
            float maxScrollY = std::max(0.0f, totalH - b.height);
            m_scrollY = std::clamp(m_scrollY - wheel * 30.0f, 0.0f, maxScrollY);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        }
        return false;
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed || m_items.empty()) return false;
        using K = JKeyEvent::JKey;

        if (ke.key == K::Down) {
            setSelectedIndex(m_selectedIndex + 1);
            _ensureIndexVisible(m_selectedIndex);
            return true;
        } else if (ke.key == K::Up) {
            setSelectedIndex(m_selectedIndex - 1);
            _ensureIndexVisible(m_selectedIndex);
            return true;
        } else if (ke.key == K::Return || ke.key == K::Space) {
            if (m_selectedIndex >= 0 && m_selectedIndex < (int)m_items.size()) {
                onItemActivated.emit(m_selectedIndex);
            }
            return true;
        }
        return false;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        bool focused = isFocused();
        JStyleOption o = jstyle::option(m_state, focused);

        // Background = Base role; border = Accent ring when focused else Border.
        buf.pushRectangle(b.x, b.y, b.width, b.height, jstyle::fieldFill(o).data(),
                          JStyle::current().hint(JStyleHint::ControlRadius),
                          jstyle::borderW(focused), jstyle::border(o).data());

        if (m_items.empty()) return;

        float itemH = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() + 8.0f : 20.0f;
        float totalH = m_items.size() * itemH + 8.0f;
        float maxScrollY = std::max(0.0f, totalH - b.height);
        m_scrollY = std::clamp(m_scrollY, 0.0f, maxScrollY);

        int startIdx = static_cast<int>(m_scrollY / itemH);
        int endIdx = static_cast<int>((m_scrollY + b.height) / itemH) + 1;
        startIdx = std::clamp(startIdx, 0, (int)m_items.size() - 1);
        endIdx = std::clamp(endIdx, 0, (int)m_items.size() - 1);

        buf.pushClip(b.x + 1.0f, b.y + 1.0f, b.width - 13.0f, b.height - 2.0f);

        for (int i = startIdx; i <= endIdx; ++i) {
            float itemY = b.y + 4.0f + i * itemH - m_scrollY;
            float itemW = b.width - 14.0f;

            if (i == m_selectedIndex) {
                // Selection fill = Highlight role (old Accent).
                buf.pushRectangle(b.x + 4.0f, itemY, itemW - 4.0f, itemH - 2.0f,
                                  jstyle::role(JColorRole::Highlight, o).data(), 3.0f);
            }

            if (JTextHelper::hasAtlas()) {
                uint8_t tc[4] = {Colors::ControlText[0], Colors::ControlText[1], Colors::ControlText[2], 220};
                if (i == m_selectedIndex) {   // selected text takes HighlightedText's rgb (white), same alpha
                    const JColor ht = jstyle::role(JColorRole::HighlightedText, o);
                    tc[0] = ht.r; tc[1] = ht.g; tc[2] = ht.b;
                }
                JTextHelper::pushText(buf, b.x + 8.0f, itemY + 4.0f, tr(m_items[i]), tc, itemW - 12.0f);
            } else {
                uint8_t tc[4] = {Colors::LabelText[0], Colors::LabelText[1], Colors::LabelText[2], 180};
                if (i == m_selectedIndex) {
                    tc[0] = 255; tc[1] = 255; tc[2] = 255;
                }
                buf.pushRectangle(b.x + 8.0f, itemY + (itemH - 6.0f) * 0.5f, itemW * 0.7f, 6.0f, tc, 2.0f);
            }
        }
        buf.popClip();

        if (maxScrollY > 0.0f) {
            float trackW = 10.0f;
            float trackH = b.height - 4.0f;
            float trackX = b.x + b.width - trackW - 2.0f;
            float trackY = b.y + 2.0f;

            buf.pushRectangle(trackX, trackY, trackW, trackH, Colors::ScrollTrack, 3.0f);

            float visibleRatio = b.height / totalH;
            float thumbH = std::max(20.0f, trackH * visibleRatio);
            float scrollRatio = m_scrollY / maxScrollY;
            float thumbY = trackY + scrollRatio * (trackH - thumbH);

            const uint8_t* thumbColor = m_draggingScroll ? Colors::ScrollThumbActive : Colors::ScrollThumb;
            buf.pushRectangle(trackX + 1.0f, thumbY, trackW - 2.0f, thumbH, thumbColor, 3.0f);
        }
    }



private:
    void _ensureIndexVisible(int index) {
        if (index < 0 || index >= (int)m_items.size()) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float itemH = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() + 8.0f : 20.0f;
        float itemY = index * itemH;
        if (itemY < m_scrollY) {
            m_scrollY = itemY;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        } else if (itemY + itemH > m_scrollY + b.height) {
            m_scrollY = itemY + itemH - b.height;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        }
    }

    std::vector<std::string> m_items;
    int                      m_selectedIndex{-1};
    float                    m_scrollY{0.0f};
    bool                     m_draggingScroll{false};
    float                    m_dragStartY{0.0f};
    float                    m_dragStartScrollY{0.0f};
};

} // inline namespace jf
