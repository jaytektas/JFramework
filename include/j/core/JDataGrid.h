#pragma once

// JDataGrid.

#include "JControl.h"
#include "JTextHelper.h"
#include "KeyEvent.h"

inline namespace jf {

class JDataGrid : public JControl {
public:
    jf::JSignal<int> onSelectionChanged;
    jf::JSignal<int> onRowActivated;

    JDataGrid(JSceneGraph& graph, std::vector<std::string> headers = {}, float w = 400.0f, float h = 250.0f)
        : JControl(graph, "JDataGrid"), m_headers(std::move(headers))
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().menuItemHeight;
        l.minWidth = 100.0f;
        l.minHeight = 80.0f;
    }

    void setHeaders(const std::vector<std::string>& headers) {
        m_headers = headers;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    void setColumnWidths(const std::vector<float>& widths) {
        m_columnWidths = widths;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    void setRows(const std::vector<std::vector<std::string>>& rows) {
        m_rows = rows;
        if (m_selectedIndex != -1) {
            m_selectedIndex = m_rows.empty() ? -1 : std::clamp(m_selectedIndex, 0, (int)m_rows.size()-1);
        }
        m_scrollY = 0.0f;
        m_scrollX = 0.0f;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    const std::vector<std::string>& headers() const { return m_headers; }
    const std::vector<std::vector<std::string>>& rows() const { return m_rows; }

    void setSelectedIndex(int index) {
        int nextIdx = (m_rows.empty()) ? -1 : std::clamp(index, 0, (int)m_rows.size()-1);
        if (m_selectedIndex != nextIdx) {
            m_selectedIndex = nextIdx;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onSelectionChanged.emit(nextIdx);
        }
    }
    int selectedIndex() const { return m_selectedIndex; }

    float rowHeight() const { return m_rowHeight; }
    void setRowHeight(float h) { m_rowHeight = h; m_graph.invalidateNode(m_nodeId, DirtySelf); }

    float headerHeight() const { return m_headerHeight; }
    void setHeaderHeight(float h) { m_headerHeight = h; m_graph.invalidateNode(m_nodeId, DirtySelf); }

    float cellPadding() const { return m_cellPadding; }
    void setCellPadding(float p) { m_cellPadding = p; m_graph.invalidateNode(m_nodeId, DirtySelf); }

    float columnWidth(int colIdx, float totalW) const {
        if (colIdx >= 0 && colIdx < (int)m_columnWidths.size()) {
            return m_columnWidths[colIdx];
        }
        if (m_headers.empty()) return totalW;
        return totalW / m_headers.size();
    }

    void handleMousePress(float mx, float my) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            onClicked.emit();
            float scrollBarW = 10.0f;
            float headerH = m_headerHeight;
            float rowH = m_rowHeight;

            float trackX = b.x + b.width - scrollBarW;
            if (mx >= trackX && my >= b.y + headerH) {
                m_draggingVScroll = true;
                m_dragStartY = my;
                m_dragStartScrollY = m_scrollY;
                return;
            }

            float trackY = b.y + b.height - scrollBarW;
            if (my >= trackY && mx < trackX) {
                m_draggingHScroll = true;
                m_dragStartX = mx;
                m_dragStartScrollX = m_scrollX;
                return;
            }

            if (my >= b.y + headerH && my < b.y + b.height - (hasHScroll(b) ? scrollBarW : 0.0f)) {
                float relativeY = my - (b.y + headerH) + m_scrollY;
                int clickedIndex = static_cast<int>(relativeY / rowH);
                if (clickedIndex >= 0 && clickedIndex < (int)m_rows.size()) {
                    setSelectedIndex(clickedIndex);
                    onRowActivated.emit(clickedIndex);
                }
            }
        }
    }

    void handleMouseRelease(float mx, float my) override {
        m_draggingVScroll = false;
        m_draggingHScroll = false;
        JControl::handleMouseRelease(mx, my);
    }

    void handleMouseMove(float mx, float my) override {
        if (m_draggingVScroll) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            bool hasH = hasHScroll(b);
            float scrollBarW = 10.0f;
            float trackH = b.height - m_headerHeight - (hasH ? scrollBarW : 0.0f);
            float totalH = m_rows.size() * m_rowHeight + m_headerHeight + (hasH ? scrollBarW : 0.0f);
            float maxScrollY = std::max(0.0f, totalH - b.height);
            float handleH = std::max(20.0f, (trackH / totalH) * trackH);
            float thumbRange = trackH - handleH;
            if (thumbRange > 0.0f) {
                m_scrollY = std::clamp(m_dragStartScrollY + (my - m_dragStartY) * maxScrollY / thumbRange, 0.0f, maxScrollY);
                m_graph.invalidateNode(m_nodeId, DirtySelf);
            }
            return;
        }
        if (m_draggingHScroll) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            bool hasV = hasVScroll(b);
            float scrollBarW = 10.0f;
            float trackW = b.width - (hasV ? scrollBarW : 0.0f);
            float totalColW = 0.0f;
            for (int i = 0; i < (int)m_headers.size(); ++i)
                totalColW += columnWidth(i, b.width);
            float maxScrollX = std::max(0.0f, totalColW - trackW);
            float handleW = std::max(20.0f, (trackW / totalColW) * trackW);
            float thumbRange = trackW - handleW;
            if (thumbRange > 0.0f) {
                m_scrollX = std::clamp(m_dragStartScrollX + (mx - m_dragStartX) * maxScrollX / thumbRange, 0.0f, maxScrollX);
                m_graph.invalidateNode(m_nodeId, DirtySelf);
            }
            return;
        }
        JControl::handleMouseMove(mx, my);
    }

    bool handleScroll(float mx, float my, float wheel) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            float headerH = m_headerHeight;
            float rowH = m_rowHeight;
            float hScrollH = hasHScroll(b) ? 10.0f : 0.0f;
            float totalH = m_rows.size() * rowH + headerH + hScrollH;
            float maxScrollY = std::max(0.0f, totalH - b.height);
            m_scrollY = std::clamp(m_scrollY - wheel * 30.0f, 0.0f, maxScrollY);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        }
        return false;
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed || m_rows.empty()) return false;
        using K = JKeyEvent::JKey;

        if (ke.key == K::Down) {
            setSelectedIndex(m_selectedIndex + 1);
            _ensureRowVisible(m_selectedIndex);
            return true;
        } else if (ke.key == K::Up) {
            setSelectedIndex(m_selectedIndex - 1);
            _ensureRowVisible(m_selectedIndex);
            return true;
        } else if (ke.key == K::Return || ke.key == K::Space) {
            if (m_selectedIndex >= 0 && m_selectedIndex < (int)m_rows.size()) {
                onRowActivated.emit(m_selectedIndex);
            }
            return true;
        } else if (ke.key == K::Right) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            float totalColW = 0.0f;
            for (int i = 0; i < (int)m_headers.size(); ++i) {
                totalColW += columnWidth(i, b.width);
            }
            float maxScrollX = std::max(0.0f, totalColW - b.width + (hasVScroll(b) ? 10.0f : 0.0f));
            m_scrollX = std::clamp(m_scrollX + 20.0f, 0.0f, maxScrollX);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        } else if (ke.key == K::Left) {
            m_scrollX = std::clamp(m_scrollX - 20.0f, 0.0f, m_scrollX);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        }
        return false;
    }

    bool hasVScroll(const JRect& b) const {
        float headerH = m_headerHeight;
        float rowH = m_rowHeight;
        float totalH = m_rows.size() * rowH + headerH;
        return totalH > b.height;
    }

    bool hasHScroll(const JRect& b) const {
        float totalColW = 0.0f;
        for (int i = 0; i < (int)m_headers.size(); ++i) {
            totalColW += columnWidth(i, b.width);
        }
        return totalColW > b.width;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        bool focused = isFocused();

        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface1, 6.0f,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);

        float headerH = m_headerHeight;
        float rowH = m_rowHeight;
        float scrollBarW = 10.0f;

        bool hasV = hasVScroll(b);
        bool hasH = hasHScroll(b);
        float visibleW = b.width - (hasV ? scrollBarW : 0.0f) - 2.0f;
        float visibleH = b.height - (hasH ? scrollBarW : 0.0f) - 2.0f;

        buf.pushRectangle(b.x + 1.0f, b.y + 1.0f, visibleW, headerH - 1.0f, Colors::Surface3, 4.0f);

        buf.pushClip(b.x + 1.0f, b.y + 1.0f, visibleW, visibleH);

        std::vector<float> colStartX;
        float currentX = 0.0f;
        for (int i = 0; i < (int)m_headers.size(); ++i) {
            colStartX.push_back(currentX);
            currentX += columnWidth(i, b.width);
        }
        float totalColW = currentX;

        float maxScrollY = std::max(0.0f, m_rows.size() * rowH + headerH + (hasH ? scrollBarW : 0.0f) - b.height);
        m_scrollY = std::clamp(m_scrollY, 0.0f, maxScrollY);

        float maxScrollX = std::max(0.0f, totalColW - visibleW);
        m_scrollX = std::clamp(m_scrollX, 0.0f, maxScrollX);

        for (int i = 0; i < (int)m_headers.size(); ++i) {
            float colW = columnWidth(i, b.width);
            float startX = b.x + 1.0f + colStartX[i] - m_scrollX;

            drawHeaderCell(buf, i, {startX, b.y + 1.0f, colW, headerH - 1.0f}, m_headers[i]);

            if (i > 0) {
                buf.pushRectangle(startX, b.y + 1.0f, 1.0f, headerH - 1.0f, Colors::Border);
            }
        }

        int startIdx = static_cast<int>(m_scrollY / rowH);
        int endIdx = static_cast<int>((m_scrollY + visibleH - headerH) / rowH) + 1;
        startIdx = std::clamp(startIdx, 0, (int)m_rows.size() - 1);
        endIdx = std::clamp(endIdx, 0, (int)m_rows.size() - 1);

        for (int r = startIdx; r <= endIdx; ++r) {
            float rowY = b.y + headerH + r * rowH - m_scrollY;

            if (r == m_selectedIndex) {
                uint8_t selBg[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 60};
                buf.pushRectangle(b.x + 1.0f, rowY, visibleW, rowH, selBg);
            } else if (r % 2 == 1) {
                buf.pushRectangle(b.x + 1.0f, rowY, visibleW, rowH, Colors::RowAltBg);
            }

            for (int c = 0; c < (int)m_headers.size() && c < (int)m_rows[r].size(); ++c) {
                float colW = columnWidth(c, b.width);
                float cellX = b.x + 1.0f + colStartX[c] - m_scrollX;

                drawRowCell(buf, r, c, {cellX, rowY, colW, rowH}, m_rows[r][c], r == m_selectedIndex);

                if (c > 0) {
                    buf.pushRectangle(cellX, rowY, 1.0f, rowH, Colors::GridLine);
                }
            }

            buf.pushRectangle(b.x + 1.0f, rowY + rowH - 1.0f, visibleW, 1.0f, Colors::GridLine);
        }

        buf.popClip();

        if (hasV) {
            float trackX = b.x + b.width - scrollBarW - 1.0f;
            buf.pushRectangle(trackX, b.y + headerH, scrollBarW, b.height - headerH - (hasH ? scrollBarW : 0.0f), Colors::Surface2, 3.0f);
            
            float totalH = m_rows.size() * rowH + headerH + (hasH ? scrollBarW : 0.0f);
            float trackH = b.height - headerH - (hasH ? scrollBarW : 0.0f);
            float handleH = std::max(20.0f, (trackH / totalH) * trackH);
            float handleY = b.y + headerH + (m_scrollY / maxScrollY) * (trackH - handleH);
            buf.pushRectangle(trackX + 1.0f, handleY, scrollBarW - 2.0f, handleH, Colors::Surface3, 2.0f);
        }

        if (hasH) {
            float trackY = b.y + b.height - scrollBarW - 1.0f;
            buf.pushRectangle(b.x, trackY, b.width - (hasV ? scrollBarW : 0.0f), scrollBarW, Colors::Surface2, 3.0f);

            float trackW = b.width - (hasV ? scrollBarW : 0.0f);
            float handleW = std::max(20.0f, (trackW / totalColW) * trackW);
            float handleX = b.x + (m_scrollX / maxScrollX) * (trackW - handleW);
            buf.pushRectangle(handleX, trackY + 1.0f, handleW, scrollBarW - 2.0f, Colors::Surface3, 2.0f);
        }
    }



protected:
    virtual void drawHeaderCell(JPrimitiveBuffer& buf, int colIdx, const JRect& bounds, const std::string& title) {
        if (JTextHelper::hasAtlas()) {
            float ty = bounds.y + (bounds.height - JTextHelper::lineHeight()) * 0.5f;
            JTextHelper::pushText(buf, bounds.x + m_cellPadding, ty, tr(title), Colors::GridHeaderText, bounds.width - m_cellPadding * 2.0f);
        }
    }

    virtual void drawRowCell(JPrimitiveBuffer& buf, int rowIdx, int colIdx, const JRect& bounds, const std::string& val, bool selected) {
        if (JTextHelper::hasAtlas()) {
            uint8_t tc[4] = {Colors::LabelText[0], Colors::LabelText[1], Colors::LabelText[2], 220};
            float ty = bounds.y + (bounds.height - JTextHelper::lineHeight()) * 0.5f;
            JTextHelper::pushText(buf, bounds.x + m_cellPadding, ty, tr(val), tc, bounds.width - m_cellPadding * 2.0f);
        }
    }

private:
    void _ensureRowVisible(int index) {
        if (index < 0 || index >= (int)m_rows.size()) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float headerH = m_headerHeight;
        float rowH = m_rowHeight;
        float hScrollH = hasHScroll(b) ? 10.0f : 0.0f;
        float visibleAreaH = b.height - headerH - hScrollH;

        float rowY = index * rowH;
        if (rowY < m_scrollY) {
            m_scrollY = rowY;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        } else if (rowY + rowH > m_scrollY + visibleAreaH) {
            m_scrollY = rowY + rowH - visibleAreaH;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        }
    }

    std::vector<std::string>              m_headers;
    std::vector<float>                    m_columnWidths;
    std::vector<std::vector<std::string>> m_rows;
    int                                   m_selectedIndex{-1};
    float                                 m_scrollY{0.0f};
    float                                 m_scrollX{0.0f};
    float                                 m_rowHeight{24.0f};
    float                                 m_headerHeight{28.0f};
    float                                 m_cellPadding{8.0f};
    bool                                  m_draggingVScroll{false};
    bool                                  m_draggingHScroll{false};
    float                                 m_dragStartY{0.0f};
    float                                 m_dragStartScrollY{0.0f};
    float                                 m_dragStartX{0.0f};
    float                                 m_dragStartScrollX{0.0f};
};

} // inline namespace jf
