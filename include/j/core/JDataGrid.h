#pragma once

// JDataGrid.

#include "JControl.h"
#include "JTextHelper.h"
#include "KeyEvent.h"
#include "JArrow.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <numeric>
#include <string>

inline namespace jf {

class JDataGrid : public JControl {
public:
    jf::JSignal<int> onSelectionChanged;
    jf::JSignal<int> onRowActivated;

    JDataGrid(JSceneGraph& graph, std::vector<std::string> headers = {}, float w = 400.0f, float h = 250.0f)
        : JControl(graph, "JDataGrid"), m_headers(std::move(headers))
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().menuItemHeight;
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
        _applyOrder();                 // rebuild view order (identity, or re-apply the active sort)
        if (m_selectedIndex != -1) {
            m_selectedIndex = m_rows.empty() ? -1 : std::clamp(m_selectedIndex, 0, (int)m_rows.size()-1);
        }
        m_scrollY = 0.0f;
        m_scrollX = 0.0f;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    const std::vector<std::string>& headers() const { return m_headers; }
    const std::vector<std::vector<std::string>>& rows() const { return m_rows; }

    // ---- Row tints -------------------------------------------------------
    // Optional per-row background, indexed by SOURCE row (the order passed to setRows), so a caller can
    // colour by state without subclassing just to override drawRowCell. Alpha 0 = no tint. Kept aligned
    // with the rows through sorting because rendering goes via m_order, never the raw index.
    using RowTint = std::array<uint8_t, 4>;
    void setRowTints(std::vector<RowTint> tints) {
        m_rowTints = std::move(tints);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    void clearRowTints() { m_rowTints.clear(); m_graph.invalidateNode(m_nodeId, DirtySelf); }

    // ---- Sorting ---------------------------------------------------------
    // Click a header to sort by that column; clicking the sorted column flips direction. Off by default,
    // so existing grids keep insertion order and their current click behaviour.
    void setSortable(bool on) { m_sortable = on; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    bool sortable() const { return m_sortable; }
    int  sortColumn() const { return m_sortCol; }
    bool sortAscending() const { return m_sortAsc; }
    void sortByColumn(int col, bool ascending) {
        m_sortCol = col; m_sortAsc = ascending;
        _applyOrder();
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    void clearSort() { m_sortCol = -1; _applyOrder(); m_graph.invalidateNode(m_nodeId, DirtySelf); }

    // ---- Column alignment -------------------------------------------------
    // Per-column text alignment. Numeric columns read far better right-aligned, and a grid that cannot
    // express that forces every caller to pad strings by hand. Unset columns stay Left.
    enum class ColAlign : uint8_t { Left, Center, Right };
    void setColumnAlignment(int col, ColAlign a) {
        if (col < 0) return;
        if ((int)m_colAlign.size() <= col) m_colAlign.resize(col + 1, ColAlign::Left);
        m_colAlign[col] = a;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    void setColumnAlignments(std::vector<ColAlign> a) {
        m_colAlign = std::move(a);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    ColAlign columnAlignment(int col) const {
        return (col >= 0 && col < (int)m_colAlign.size()) ? m_colAlign[col] : ColAlign::Left;
    }

    // ---- Interactive column resize --------------------------------------
    // Drag a header divider. Off by default; widths still settable programmatically either way.
    void setColumnsResizable(bool on) { m_colResizable = on; }
    bool columnsResizable() const { return m_colResizable; }
    const std::vector<float>& columnWidths() const { return m_columnWidths; }
    void setColumnWidth(int col, float w) {
        if (col < 0) return;
        _materialiseWidths();
        if ((int)m_columnWidths.size() <= col) m_columnWidths.resize(col + 1, m_defaultColW);
        m_columnWidths[col] = std::max(JStyle::current().gridMinColumnWidth, w);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    // Width that fits the widest cell (and the header) in a column, padding included — the same
    // measurement a double-click on the divider applies.
    float autoFitWidth(int col) const {
        if (!JTextHelper::hasAtlas() || col < 0) return m_defaultColW;
        float w = (col < (int)m_headers.size()) ? JTextHelper::measureWidth(tr(m_headers[col])) : 0.0f;
        if (m_sortable) w += JStyle::current().gridSortGlyphWidth;
        for (const auto& row : m_rows)
            if (col < (int)row.size()) w = std::max(w, JTextHelper::measureWidth(tr(row[col])));
        return std::max(JStyle::current().gridMinColumnWidth, w + m_cellPadding * 2.0f + 2.0f);
    }
    void autoFitColumn(int col) { setColumnWidth(col, autoFitWidth(col)); }
    void autoFitAllColumns() {
        _materialiseWidths();
        for (int i = 0; i < (int)m_headers.size(); ++i) m_columnWidths[i] = autoFitWidth(i);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    // Custom ordering. Return true if `a` sorts before `b`; both are SOURCE row indices, so the whole row
    // is available (sort a display string by a hidden key, sort dates properly, etc.). Unset = the
    // built-in numeric-aware comparator on the sorted column.
    using SortComparator = std::function<bool(int a, int b, int column, bool ascending)>;
    void setSortComparator(SortComparator cmp) { m_sortCmp = std::move(cmp); _applyOrder(); }

    // SOURCE row under a window y coordinate, or -1 (header band, past the last row, outside). Public
    // because a caller that decorates rows — a per-row tooltip, a context menu — needs the same mapping
    // the grid uses internally, and re-deriving it from rowHeight/scroll is how the two drift apart.
    int rowAtY(float my) const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (my < b.y + m_headerHeight || my > b.y + b.height) return -1;
        const float rel = my - (b.y + m_headerHeight) + m_scrollY;
        return sourceRowAt(static_cast<int>(rel / m_rowHeight));
    }

    // View <-> source row mapping. Sorting only reorders the VIEW: indices in signals, setSelectedIndex
    // and drawRowCell stay source indices, so a caller's own arrays never need re-shuffling.
    int sourceRowAt(int viewIdx) const {
        return (viewIdx >= 0 && viewIdx < (int)m_order.size()) ? m_order[viewIdx] : -1;
    }
    int viewRowOf(int sourceIdx) const {
        for (int i = 0; i < (int)m_order.size(); ++i) if (m_order[i] == sourceIdx) return i;
        return -1;
    }

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
            const float scrollBarW = JStyle::current().scrollBarWidth;
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

            // Header band: a divider starts a resize, otherwise a click sorts. Divider wins — its grab
            // zone is only a few px, and a resize that silently re-sorted instead would be maddening.
            if (my < b.y + headerH) {
                const int div = _dividerAt(mx, b);
                if (div >= 0) {
                    // Double-click a divider = auto-fit that column (the usual spreadsheet gesture).
                    // The runner classified the press; this widget owns no clock of its own.
                    if (JWidget::s_doubleClick) { autoFitColumn(div); return; }
                    _materialiseWidths();
                    m_resizeCol    = div;
                    m_resizeStartX = mx;
                    m_resizeStartW = columnWidth(div, b.width);
                    return;
                }
                if (m_sortable && !m_headers.empty()) {
                    float x = b.x + 1.0f - m_scrollX;
                    for (int i = 0; i < (int)m_headers.size(); ++i) {
                        const float w = columnWidth(i, b.width);
                        if (mx >= x && mx < x + w) {
                            sortByColumn(i, (i == m_sortCol) ? !m_sortAsc : true);
                            break;
                        }
                        x += w;
                    }
                }
                return;
            }

            if (my >= b.y + headerH && my < b.y + b.height - (hasHScroll(b) ? scrollBarW : 0.0f)) {
                const int clickedIndex = rowAtY(my);                 // one mapping, shared with callers
                if (clickedIndex >= 0) {
                    setSelectedIndex(clickedIndex);
                    onRowActivated.emit(clickedIndex);
                }
            }
        }
    }

    void handleMouseRelease(float mx, float my) override {
        m_resizeCol = -1;
        m_draggingVScroll = false;
        m_draggingHScroll = false;
        JControl::handleMouseRelease(mx, my);
    }

    void handleMouseMove(float mx, float my) override {
        if (m_resizeCol >= 0) {
            setColumnWidth(m_resizeCol, m_resizeStartW + (mx - m_resizeStartX));
            return;
        }
        if (m_draggingVScroll) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            bool hasH = hasHScroll(b);
            const float scrollBarW = JStyle::current().scrollBarWidth;
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
            const float scrollBarW = JStyle::current().scrollBarWidth;
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
            float maxScrollX = std::max(0.0f, totalColW - b.width + (hasVScroll(b) ? JStyle::current().scrollBarWidth : 0.0f));
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

        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface1, JStyle::current().cornerRadius,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);

        float headerH = m_headerHeight;
        float rowH = m_rowHeight;
        const float scrollBarW = JStyle::current().scrollBarWidth;

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
        const int lastView = (int)m_order.size() - 1;
        startIdx = std::clamp(startIdx, 0, lastView);
        endIdx = std::clamp(endIdx, 0, lastView);

        for (int v = startIdx; v <= endIdx; ++v) {
            const int r = sourceRowAt(v);            // v = position on screen, r = caller's row index
            if (r < 0) continue;
            float rowY = b.y + headerH + v * rowH - m_scrollY;

            // Zebra striping follows the VIEW so it stays even after a sort; the tint and selection
            // follow the SOURCE row so they travel with their data.
            if (v % 2 == 1) buf.pushRectangle(b.x + 1.0f, rowY, visibleW, rowH, Colors::RowAltBg);
            if (r < (int)m_rowTints.size() && m_rowTints[r][3] != 0)
                buf.pushRectangle(b.x + 1.0f, rowY, visibleW, rowH, m_rowTints[r].data());
            if (r == m_selectedIndex) {
                uint8_t selBg[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 60};
                buf.pushRectangle(b.x + 1.0f, rowY, visibleW, rowH, selBg);   // over the tint, not under
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
    // x for `text` within `bounds` under this column's alignment. Right/Center fall back to Left when the
    // text is wider than the cell, so an over-long value never starts off-screen.
    float alignedTextX(int colIdx, const JRect& bounds, const std::string& text, float reserveRight = 0.0f) const {
        const float inner = bounds.width - m_cellPadding * 2.0f - reserveRight;
        const float tw    = JTextHelper::measureWidth(text);
        if (tw >= inner) return bounds.x + m_cellPadding;
        switch (columnAlignment(colIdx)) {
            case ColAlign::Right:  return bounds.x + bounds.width - m_cellPadding - reserveRight - tw;
            case ColAlign::Center: return bounds.x + (bounds.width - reserveRight - tw) * 0.5f;
            default:               return bounds.x + m_cellPadding;
        }
    }

    virtual void drawHeaderCell(JPrimitiveBuffer& buf, int colIdx, const JRect& bounds, const std::string& title) {
        if (!JTextHelper::hasAtlas()) return;
        const bool  sorted  = (m_sortable && colIdx == m_sortCol);
        const float reserve = sorted ? JStyle::current().gridSortGlyphWidth : 0.0f;
        const std::string t = tr(title);
        const float ty = bounds.y + (bounds.height - JTextHelper::lineHeight()) * 0.5f;
        JTextHelper::pushText(buf, alignedTextX(colIdx, bounds, t, reserve), ty, t,
                              Colors::GridHeaderText, bounds.width - m_cellPadding * 2.0f - reserve);
        if (sorted) {
            // Direction mark via JArrow — the toolkit's one triangle, shared with the tree's disclosure.
            JArrow::draw(buf,
                         bounds.x + bounds.width - m_cellPadding - JStyle::current().gridSortGlyphWidth * 0.5f,
                         bounds.y + bounds.height * 0.5f,
                         m_sortAsc ? JArrow::Direction::Up : JArrow::Direction::Down,
                         Colors::GridHeaderText);
        }
    }

    virtual void drawRowCell(JPrimitiveBuffer& buf, int rowIdx, int colIdx, const JRect& bounds, const std::string& val, bool selected) {
        if (JTextHelper::hasAtlas()) {
            uint8_t tc[4] = {Colors::LabelText[0], Colors::LabelText[1], Colors::LabelText[2], 220};
            const std::string t = tr(val);
            float ty = bounds.y + (bounds.height - JTextHelper::lineHeight()) * 0.5f;
            JTextHelper::pushText(buf, alignedTextX(colIdx, bounds, t), ty, t, tc,
                                  bounds.width - m_cellPadding * 2.0f);
        }
    }

private:
    // Rebuild the view order: identity, or the active sort applied over it. Called whenever rows or the
    // sort change, so m_order is always a valid permutation of [0, m_rows.size()).
    void _applyOrder() {
        m_order.resize(m_rows.size());
        std::iota(m_order.begin(), m_order.end(), 0);
        if (m_sortCol < 0) return;
        const int  col = m_sortCol;
        const bool asc = m_sortAsc;
        if (m_sortCmp) {
            std::stable_sort(m_order.begin(), m_order.end(),
                             [&](int a, int b) { return m_sortCmp(a, b, col, asc); });
            return;
        }
        std::stable_sort(m_order.begin(), m_order.end(), [&](int a, int b) {
            const int c = _compareCells(_cell(a, col), _cell(b, col));
            return asc ? (c < 0) : (c > 0);
        });
    }

    const std::string& _cell(int row, int col) const {
        static const std::string kEmpty;
        if (row < 0 || row >= (int)m_rows.size()) return kEmpty;
        return (col >= 0 && col < (int)m_rows[row].size()) ? m_rows[row][col] : kEmpty;
    }

    // Numeric-aware, case-insensitive compare. Two values that both parse as numbers compare
    // numerically, so 9 sorts before 10 rather than after it — the single most common complaint about a
    // string-sorted grid. Anything else falls back to a case-insensitive lexicographic compare.
    static int _compareCells(const std::string& a, const std::string& b) {
        double na = 0.0, nb = 0.0;
        if (_asNumber(a, na) && _asNumber(b, nb))
            return (na < nb) ? -1 : (na > nb) ? 1 : 0;
        const size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i < n; ++i) {
            const unsigned char ca = (unsigned char)std::tolower((unsigned char)a[i]);
            const unsigned char cb = (unsigned char)std::tolower((unsigned char)b[i]);
            if (ca != cb) return (ca < cb) ? -1 : 1;
        }
        return (a.size() == b.size()) ? 0 : (a.size() < b.size() ? -1 : 1);
    }

    // Whole-string numeric parse (leading/trailing space tolerated). Deliberately strict otherwise:
    // "P0117" must NOT be read as 0, or every code would compare equal.
    static bool _asNumber(const std::string& sv, double& out) {
        size_t i = 0, j = sv.size();
        while (i < j && std::isspace((unsigned char)sv[i])) ++i;
        while (j > i && std::isspace((unsigned char)sv[j - 1])) --j;
        if (i >= j) return false;
        const std::string t = sv.substr(i, j - i);
        try {
            size_t used = 0;
            out = std::stod(t, &used);
            return used == t.size();
        } catch (...) { return false; }
    }

    // Give every column a concrete width so a drag has something to modify (until then widths are an
    // even split computed on the fly).
    void _materialiseWidths() const {
        if ((int)m_columnWidths.size() >= (int)m_headers.size() || m_headers.empty()) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const float even = (b.width > 0.0f) ? b.width / m_headers.size() : m_defaultColW;
        m_columnWidths.resize(m_headers.size(), std::max(JStyle::current().gridMinColumnWidth, even));
    }

    // Column whose RIGHT divider is within grab range of x (header-relative), or -1.
    int _dividerAt(float mx, const JRect& b) const {
        if (!m_colResizable || m_headers.empty()) return -1;
        float x = b.x + 1.0f - m_scrollX;
        for (int i = 0; i < (int)m_headers.size(); ++i) {
            x += columnWidth(i, b.width);
            if (std::fabs(mx - x) <= JStyle::current().gridResizeGrab) return i;
        }
        return -1;
    }

    void _ensureRowVisible(int index) {
        if (index < 0 || index >= (int)m_rows.size()) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float headerH = m_headerHeight;
        float rowH = m_rowHeight;
        float hScrollH = hasHScroll(b) ? 10.0f : 0.0f;
        float visibleAreaH = b.height - headerH - hScrollH;

        const int view = viewRowOf(index);        // sorting moves rows: scroll to where it is DRAWN
        float rowY = (view >= 0 ? view : index) * rowH;
        if (rowY < m_scrollY) {
            m_scrollY = rowY;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        } else if (rowY + rowH > m_scrollY + visibleAreaH) {
            m_scrollY = rowY + rowH - visibleAreaH;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        }
    }

    std::vector<std::string>              m_headers;
    mutable std::vector<float>            m_columnWidths;   // mutable: _materialiseWidths() fills lazily
    std::vector<int>                      m_order;          // view index -> source row index
    std::vector<RowTint>                  m_rowTints;       // by SOURCE row; alpha 0 = none
    std::vector<ColAlign>                 m_colAlign;
    SortComparator                        m_sortCmp;
    int                                   m_sortCol{-1};
    bool                                  m_sortAsc{true};
    bool                                  m_sortable{false};
    bool                                  m_colResizable{false};
    int                                   m_resizeCol{-1};
    float                                 m_resizeStartX{0.0f};
    float                                 m_resizeStartW{0.0f};
    float                                 m_defaultColW{JStyle::current().gridDefaultColumnWidth};
    std::vector<std::vector<std::string>> m_rows;
    int                                   m_selectedIndex{-1};
    float                                 m_scrollY{0.0f};
    float                                 m_scrollX{0.0f};
    float                                 m_rowHeight{JStyle::current().gridRowHeight};
    float                                 m_headerHeight{JStyle::current().gridHeaderHeight};
    float                                 m_cellPadding{JStyle::current().gridCellPadding};
    bool                                  m_draggingVScroll{false};
    bool                                  m_draggingHScroll{false};
    float                                 m_dragStartY{0.0f};
    float                                 m_dragStartScrollY{0.0f};
    float                                 m_dragStartX{0.0f};
    float                                 m_dragStartScrollX{0.0f};
};

} // inline namespace jf
