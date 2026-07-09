#pragma once

// ============================================================================
// j/core/ItemView.h — concrete model/view widgets that adopt the foundation.
//
// JItemListView / JItemTableView are the first widgets to bind the model layer
// (j/model/*) to the render/input stack (BaseWidgets.h). Each multiply-inherits
// JControl (pixels + input) and JAbstractItemView (the render-agnostic M/V/D
// contract), so it:
//
//   * takes a JAbstractItemModel via setModel() and owns a JItemSelectionModel,
//   * renders each row through the delegate's Display text (+ Checked / Decoration),
//   * click-selects (single, plus Ctrl toggle / Shift range via the selection model),
//   * moves the current index with Up / Down / Home / End,
//   * scrolls with the wheel, and
//   * repaints when the model fires dataChanged / rowsInserted / rowsRemoved /
//     modelReset (the react hooks bump a repaint counter and invalidate()).
//
// These are NEW widgets; no existing view is touched. They sit on the same
// JScrollBar / JTextHelper conventions the rest of BaseWidgets.h uses.
// ============================================================================

#include "j/core/JTextHelper.h"
#include "j/core/JControl.h"
#include "j/model/AbstractItemView.h"

inline namespace jf {

// ----------------------------------------------------------------------------
// JItemListView — a flat, single-column, vertically-scrolling list bound to column 0
// of a JAbstractItemModel. Rows are the model's rowCount(); each row shows the
// delegate's Display text (which folds in a check box for Checkable items).
// ----------------------------------------------------------------------------
class JItemListView : public JControl, public JAbstractItemView {
public:
    JItemListView(JSceneGraph& graph, float w = 200.0f, float h = 200.0f)
        : JControl(graph, "JItemListView")
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width  = w;
        l.boundingBox.height = h;
        l.minWidth  = 40.0f;
        l.minHeight = 40.0f;
    }

    // ---- Geometry knobs ----------------------------------------------------
    void  setRowHeight(float h) { m_rowH = h > 1.0f ? h : 1.0f; invalidate(); }
    float rowHeight() const { return m_rowH; }

    int rowCount() const { return m_model ? m_model->rowCount() : 0; }

    // Observation hook for tests / instrumentation: incremented every time a
    // model/selection change asks the view to repaint.
    int repaintCount() const { return m_repaints; }

    // ---- JAbstractItemView spatial contract --------------------------------
    JModelIndex indexAt(float x, float y) const override {
        if (!m_model) return JModelIndex();
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (x < b.x || x > b.x + b.width || y < b.y || y > b.y + b.height)
            return JModelIndex();
        int row = static_cast<int>((y - b.y + m_scrollY) / m_rowH);
        if (row < 0 || row >= rowCount()) return JModelIndex();
        return m_model->index(row, 0);
    }

    JItemRect visualRect(const JModelIndex& index) const override {
        if (!index.isValid()) return JItemRect{};
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        JItemRect r;
        r.x = b.x;
        r.y = b.y + index.row() * m_rowH - m_scrollY;
        r.w = b.width;
        r.h = m_rowH;
        return r;
    }

    void scrollTo(const JModelIndex& index) override {
        if (!index.isValid()) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float top = index.row() * m_rowH;
        float bot = top + m_rowH;
        if (top < m_scrollY)                m_scrollY = top;
        else if (bot > m_scrollY + b.height) m_scrollY = bot - b.height;
        _clampScroll();
        invalidate();
    }

    // ---- Input -------------------------------------------------------------
    void handleMousePress(float mx, float my) override {
        if (m_state == JWidgetState::Disabled) return;
        if (!isPointInside(mx, my)) return;
        requestFocus();                 // route keys here
        setState(JWidgetState::Pressed);
        onClicked.emit();
        JModelIndex hit = indexAt(mx, my);
        if (!hit.isValid() || !m_selection) return;

        using SM = JItemSelectionModel;
        if (JWidget::s_shiftDown) {
            // Range from the current anchor to the clicked row.
            JModelIndex anchor = m_selection->currentIndex();
            int from = anchor.isValid() ? anchor.row() : hit.row();
            m_selection->selectRange(from, hit.row(), 0, SM::ClearAndSelect);
            m_selection->setCurrentIndex(hit, SM::NoUpdate);
        } else if (JWidget::s_ctrlDown) {
            m_selection->select(hit, SM::Toggle | SM::Current);
        } else {
            m_selection->setCurrentIndex(hit, SM::ClearAndSelect | SM::Current);
        }
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed || !m_model || !m_selection) return false;
        const int n = rowCount();
        if (n <= 0) return false;
        using K = JKeyEvent::JKey;
        int cur = m_selection->currentIndex().isValid()
                      ? m_selection->currentIndex().row() : -1;
        int next = cur;
        switch (ke.key) {
            case K::Down: next = (cur < 0) ? 0 : std::min(cur + 1, n - 1); break;
            case K::Up:   next = (cur < 0) ? 0 : std::max(cur - 1, 0);     break;
            case K::Home: next = 0;     break;
            case K::End:  next = n - 1; break;
            default: return false;
        }
        JModelIndex idx = m_model->index(next, 0);
        using SM = JItemSelectionModel;
        m_selection->setCurrentIndex(idx, SM::ClearAndSelect | SM::Current);
        scrollTo(idx);
        return true;
    }

    bool handleScroll(float mx, float my, float wheel) override {
        if (!isPointInside(mx, my)) return false;
        float before = m_scrollY;
        m_scrollY -= wheel * m_rowH;   // one row per wheel notch (unit wheel)
        _clampScroll();
        if (m_scrollY != before) invalidate();
        return true;
    }

    // ---- Render ------------------------------------------------------------
    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const auto& th = JStyle::current();

        // Frame.
        buf.pushRectangle(b.x, b.y, b.width, b.height, th.Surface1,
                          th.cornerRadius, th.borderWidth, th.Border);
        if (!m_model) { drawFocusRing(buf); return; }

        buf.pushClip(b.x, b.y, b.width, b.height);

        const int n = rowCount();
        const float lh = JTextHelper::lineHeight();
        JAbstractItemDelegate* d = itemDelegate();

        int first = std::max(0, static_cast<int>(m_scrollY / m_rowH));
        int last  = std::min(n - 1,
                             static_cast<int>((m_scrollY + b.height) / m_rowH));
        for (int row = first; row <= last; ++row) {
            JModelIndex idx = m_model->index(row, 0);
            if (!idx.isValid()) continue;
            float ry = b.y + row * m_rowH - m_scrollY;

            bool selected = m_selection && m_selection->isSelected(idx);
            bool current  = m_selection && m_selection->currentIndex() == idx;

            if (selected) {
                uint8_t sel[4] = {th.Accent[0], th.Accent[1], th.Accent[2], 90};
                buf.pushRectangle(b.x + 1.f, ry, b.width - 2.f, m_rowH, sel, 0.f);
            }
            if (current) {
                buf.pushRectangle(b.x + 1.f, ry, b.width - 2.f, m_rowH, Colors::Transparent,
                                  0.f, 1.0f, th.Accent);
            }

            float textX = b.x + th.itemPadding;

            // Decoration swatch (if the model supplies one for this row).
            JVariant deco = m_model->data(idx, roleId(JItemRole::Decoration));
            if (!deco.isNull()) {
                float sw = m_rowH - 8.f;
                uint8_t swatch[4] = {th.TextSecondary[0], th.TextSecondary[1],
                                     th.TextSecondary[2], 255};
                if (deco.isInt()) {
                    uint32_t rgb = static_cast<uint32_t>(deco.toInt());
                    swatch[0] = (rgb >> 16) & 0xFF;
                    swatch[1] = (rgb >> 8)  & 0xFF;
                    swatch[2] = (rgb)       & 0xFF;
                }
                buf.pushRectangle(textX, ry + 4.f, sw, sw, swatch, 2.f);
                textX += sw + th.spacing;
            }

            if (JTextHelper::hasAtlas()) {
                const uint8_t* tc = selected ? th.TextPrimary : th.TextPrimary;
                float ty = ry + (m_rowH - lh) * 0.5f;
                JTextHelper::pushText(buf, textX, ty, d->displayText(idx), tc,
                                      b.x + b.width - textX - th.itemPadding);
            }
        }

        buf.popClip();
        drawFocusRing(buf);
    }

protected:
    // ---- Model/selection react hooks → repaint -----------------------------
    void dataChangedEvent(const JModelIndex&, const JModelIndex&) override { _touch(); }
    void rowsInsertedEvent(const JModelIndex&, int, int) override { _touch(); }
    void rowsRemovedEvent (const JModelIndex&, int, int) override { _clampScroll(); _touch(); }
    void modelResetEvent() override { m_scrollY = 0.f; _touch(); }
    void currentChangedEvent(const JModelIndex&, const JModelIndex&) override { _touch(); }
    void selectionChangedEvent(const std::vector<JModelIndex>&,
                               const std::vector<JModelIndex>&) override { _touch(); }
    void reset() override { _touch(); }

    void _touch() { ++m_repaints; invalidate(); }

    void _clampScroll() {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float contentH = rowCount() * m_rowH;
        float maxScroll = std::max(0.f, contentH - b.height);
        m_scrollY = std::clamp(m_scrollY, 0.f, maxScroll);
    }

    float m_rowH   = 22.0f;
    float m_scrollY = 0.0f;
    int   m_repaints = 0;
};

// ----------------------------------------------------------------------------
// JItemTableView — a cheap 2D grid over the same foundation. Row-oriented selection
// (a click anywhere on a row selects the row); each visible cell renders its
// Display text in an evenly-split column. Shares JItemListView's scrolling +
// keyboard row navigation model, so the two stay behaviourally consistent.
// ----------------------------------------------------------------------------
class JItemTableView : public JControl, public JAbstractItemView {
public:
    JItemTableView(JSceneGraph& graph, float w = 320.0f, float h = 200.0f)
        : JControl(graph, "JItemTableView")
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width  = w;
        l.boundingBox.height = h;
        l.minWidth  = 60.0f;
        l.minHeight = 40.0f;
    }

    void  setRowHeight(float h) { m_rowH = h > 1.0f ? h : 1.0f; invalidate(); }
    float rowHeight() const { return m_rowH; }
    int   rowCount() const { return m_model ? m_model->rowCount() : 0; }
    int   columnCount() const { return m_model ? m_model->columnCount() : 0; }
    int   repaintCount() const { return m_repaints; }

    float _colWidth() const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        int c = std::max(1, columnCount());
        return (b.width - 2.f) / c;
    }

    JModelIndex indexAt(float x, float y) const override {
        if (!m_model) return JModelIndex();
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (x < b.x || x > b.x + b.width || y < b.y || y > b.y + b.height)
            return JModelIndex();
        int row = static_cast<int>((y - b.y + m_scrollY) / m_rowH);
        int col = static_cast<int>((x - b.x - 1.f) / _colWidth());
        if (row < 0 || row >= rowCount()) return JModelIndex();
        col = std::clamp(col, 0, std::max(0, columnCount() - 1));
        return m_model->index(row, col);
    }

    JItemRect visualRect(const JModelIndex& index) const override {
        if (!index.isValid()) return JItemRect{};
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        JItemRect r;
        r.x = b.x + 1.f + index.column() * _colWidth();
        r.y = b.y + index.row() * m_rowH - m_scrollY;
        r.w = _colWidth();
        r.h = m_rowH;
        return r;
    }

    void scrollTo(const JModelIndex& index) override {
        if (!index.isValid()) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float top = index.row() * m_rowH, bot = top + m_rowH;
        if (top < m_scrollY)                m_scrollY = top;
        else if (bot > m_scrollY + b.height) m_scrollY = bot - b.height;
        _clampScroll();
        invalidate();
    }

    void handleMousePress(float mx, float my) override {
        if (m_state == JWidgetState::Disabled) return;
        if (!isPointInside(mx, my)) return;
        requestFocus();
        setState(JWidgetState::Pressed);
        onClicked.emit();
        JModelIndex hit = indexAt(mx, my);
        if (!hit.isValid() || !m_selection) return;
        using SM = JItemSelectionModel;
        if (JWidget::s_ctrlDown) m_selection->select(hit, SM::Toggle | SM::Current);
        else m_selection->setCurrentIndex(hit, SM::ClearAndSelect | SM::Current);
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed || !m_model || !m_selection) return false;
        const int n = rowCount();
        if (n <= 0) return false;
        using K = JKeyEvent::JKey;
        JModelIndex c = m_selection->currentIndex();
        int cur = c.isValid() ? c.row() : -1;
        int col = c.isValid() ? c.column() : 0;
        int next = cur;
        switch (ke.key) {
            case K::Down: next = (cur < 0) ? 0 : std::min(cur + 1, n - 1); break;
            case K::Up:   next = (cur < 0) ? 0 : std::max(cur - 1, 0);     break;
            case K::Home: next = 0;     break;
            case K::End:  next = n - 1; break;
            default: return false;
        }
        JModelIndex idx = m_model->index(next, col);
        using SM = JItemSelectionModel;
        m_selection->setCurrentIndex(idx, SM::ClearAndSelect | SM::Current);
        scrollTo(idx);
        return true;
    }

    bool handleScroll(float mx, float my, float wheel) override {
        if (!isPointInside(mx, my)) return false;
        float before = m_scrollY;
        m_scrollY -= wheel * m_rowH;
        _clampScroll();
        if (m_scrollY != before) invalidate();
        return true;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const auto& th = JStyle::current();
        buf.pushRectangle(b.x, b.y, b.width, b.height, th.Surface1,
                          th.cornerRadius, th.borderWidth, th.Border);
        if (!m_model) { drawFocusRing(buf); return; }

        buf.pushClip(b.x, b.y, b.width, b.height);
        const int n = rowCount();
        const int cols = std::max(1, columnCount());
        const float cw = _colWidth();
        const float lh = JTextHelper::lineHeight();
        JAbstractItemDelegate* d = itemDelegate();

        int first = std::max(0, static_cast<int>(m_scrollY / m_rowH));
        int last  = std::min(n - 1,
                             static_cast<int>((m_scrollY + b.height) / m_rowH));
        for (int row = first; row <= last; ++row) {
            float ry = b.y + row * m_rowH - m_scrollY;
            JModelIndex rowCur = m_selection ? m_selection->currentIndex() : JModelIndex();
            bool rowSelected = false;
            for (int c = 0; c < cols; ++c)
                if (m_selection && m_selection->isSelected(m_model->index(row, c)))
                    { rowSelected = true; break; }

            if (rowSelected) {
                uint8_t sel[4] = {th.Accent[0], th.Accent[1], th.Accent[2], 90};
                buf.pushRectangle(b.x + 1.f, ry, b.width - 2.f, m_rowH, sel, 0.f);
            }
            if (rowCur.isValid() && rowCur.row() == row) {
                buf.pushRectangle(b.x + 1.f, ry, b.width - 2.f, m_rowH, Colors::Transparent,
                                  0.f, 1.0f, th.Accent);
            }

            if (JTextHelper::hasAtlas()) {
                for (int c = 0; c < cols; ++c) {
                    JModelIndex idx = m_model->index(row, c);
                    if (!idx.isValid()) continue;
                    float cx = b.x + 1.f + c * cw + th.itemPadding;
                    float ty = ry + (m_rowH - lh) * 0.5f;
                    JTextHelper::pushText(buf, cx, ty, d->displayText(idx),
                                          th.TextPrimary, cw - th.itemPadding * 2.f);
                }
            }
        }
        buf.popClip();
        drawFocusRing(buf);
    }

protected:
    void dataChangedEvent(const JModelIndex&, const JModelIndex&) override { _touch(); }
    void rowsInsertedEvent(const JModelIndex&, int, int) override { _touch(); }
    void rowsRemovedEvent (const JModelIndex&, int, int) override { _clampScroll(); _touch(); }
    void modelResetEvent() override { m_scrollY = 0.f; _touch(); }
    void currentChangedEvent(const JModelIndex&, const JModelIndex&) override { _touch(); }
    void selectionChangedEvent(const std::vector<JModelIndex>&,
                               const std::vector<JModelIndex>&) override { _touch(); }
    void reset() override { _touch(); }

    void _touch() { ++m_repaints; invalidate(); }
    void _clampScroll() {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float contentH = rowCount() * m_rowH;
        float maxScroll = std::max(0.f, contentH - b.height);
        m_scrollY = std::clamp(m_scrollY, 0.f, maxScroll);
    }

    float m_rowH    = 22.0f;
    float m_scrollY = 0.0f;
    int   m_repaints = 0;
};

}  // inline namespace jf
