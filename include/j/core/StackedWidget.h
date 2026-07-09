#pragma once
// ============================================================================
// JStackedWidget — a stack of same-rect pages, exactly ONE visible at a time.
// ----------------------------------------------------------------------------
// Holds N child widgets that all occupy the SAME rectangle (the stack's own
// bounds) and shows precisely one of them. Switching pages is index-driven; the
// classic use is a settings pane, a wizard body, or any "swap this region's
// content" surface where the pages never overlap visually.
//
// Idiom (matches JScrollArea): children are NON-OWNING — the caller owns the
// page widgets exactly as it owns the stack. Pages are tracked in a private
// vector rather than wired into the scene-graph hierarchy, so a page can be
// removed cleanly without a graph detach. On paint the current page is sized to
// fill the stack's box, only the current page paints, and input routes only to
// the current page. Hidden pages get setVisible(false): the focus manager
// already skips !visible widgets, so a tabbed-behind page can never steal a
// click, a keystroke, or a Tab stop.
//
// Concept reference only (no source adapted): a single-visible-child stack.
// ============================================================================

#include "BaseWidgets.h"
#include <vector>
#include <algorithm>

inline namespace jf {

class JStackedWidget : public JWidget {
public:
    // Fires with the new current index whenever it changes (or -1 when the last
    // page is removed and the stack goes empty). Not emitted for a no-op set.
    jf::JSignal<int> currentChanged;

    JStackedWidget(JSceneGraph& graph, float w = 200.0f, float h = 100.0f)
        : JWidget(graph, "JStackedWidget")
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width  = w;
        l.boundingBox.height = h;
    }

    // ------------------------------------------------------------------
    // Page management. Non-owning: the caller retains ownership of `w`.
    // ------------------------------------------------------------------

    // Append a page; returns its index. The first page added becomes current.
    int addWidget(JWidget* w) {
        if (!w) return -1;
        return insertWidget(static_cast<int>(m_pages.size()), w);
    }

    // Insert a page at `index` (clamped to [0, count]); returns the actual index.
    // Adding the first page makes it current; otherwise the current PAGE is kept
    // (its index shifts if the insertion precedes it) with no currentChanged.
    int insertWidget(int index, JWidget* w) {
        if (!w) return -1;
        index = std::clamp(index, 0, static_cast<int>(m_pages.size()));
        m_pages.insert(m_pages.begin() + index, w);
        if (m_current < 0) {
            // First page — force selection (emit) from the empty state.
            m_current = -1;
            setCurrentIndex(0);
        } else {
            if (index <= m_current) ++m_current;   // keep pointing at the same page
            _applyVisibility();
        }
        return index;
    }

    // Remove a page by pointer (no-op if not present). Handed back visible so the
    // caller can reuse it. If the current page is removed the next page at that
    // slot (clamped) becomes current and currentChanged fires; removing any other
    // page keeps the current page (its index quietly re-based), no signal.
    void removeWidget(JWidget* w) { removeAt(indexOf(w)); }

    // Remove the page at `index` (no-op if out of range). See removeWidget.
    void removeAt(int index) {
        if (index < 0 || index >= static_cast<int>(m_pages.size())) return;
        JWidget* cur     = (m_current >= 0) ? m_pages[m_current] : nullptr;
        JWidget* removed = m_pages[index];
        m_pages.erase(m_pages.begin() + index);
        removed->setVisible(true);   // detached — restore to a usable state

        if (m_pages.empty()) {
            m_current = -1;
            currentChanged.emit(-1);
            return;
        }
        if (removed == cur) {
            int newCur = std::min(index, static_cast<int>(m_pages.size()) - 1);
            m_current = -1;             // force the switch to register + emit
            setCurrentIndex(newCur);
        } else {
            m_current = indexOf(cur);   // same page, new slot — no signal
            _applyVisibility();
        }
    }

    // ------------------------------------------------------------------
    // Selection.
    // ------------------------------------------------------------------

    void setCurrentIndex(int index) {
        if (m_pages.empty()) { m_current = -1; return; }
        index = std::clamp(index, 0, static_cast<int>(m_pages.size()) - 1);
        if (index == m_current) { _applyVisibility(); return; }
        m_current = index;
        _applyVisibility();
        currentChanged.emit(m_current);
    }

    void setCurrentWidget(JWidget* w) {
        int idx = indexOf(w);
        if (idx >= 0) setCurrentIndex(idx);
    }

    // ------------------------------------------------------------------
    // Accessors.
    // ------------------------------------------------------------------

    int      count()        const { return static_cast<int>(m_pages.size()); }
    int      currentIndex() const { return m_current; }
    JWidget* currentWidget() const {
        return (m_current >= 0 && m_current < static_cast<int>(m_pages.size()))
                   ? m_pages[m_current] : nullptr;
    }
    JWidget* widget(int index) const {
        return (index >= 0 && index < static_cast<int>(m_pages.size()))
                   ? m_pages[index] : nullptr;
    }
    int indexOf(JWidget* w) const {
        auto it = std::find(m_pages.begin(), m_pages.end(), w);
        return (it == m_pages.end()) ? -1
                                     : static_cast<int>(it - m_pages.begin());
    }

    // ------------------------------------------------------------------
    // Paint / input — current page only.
    // ------------------------------------------------------------------

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        _arrange();                          // size the current page to fill our box
        if (JWidget* w = currentWidget())
            if (w->isVisible()) w->populateRenderPrimitives(buf);
    }

    void handleMouseMove(float mx, float my) override {
        if (JWidget* w = currentWidget()) w->handleMouseMove(mx, my);
    }
    void handleMousePress(float mx, float my) override {
        if (JWidget* w = currentWidget()) w->handleMousePress(mx, my);
    }
    void handleMouseRelease(float mx, float my) override {
        if (JWidget* w = currentWidget()) w->handleMouseRelease(mx, my);
    }
    bool handleKeyEvent(const JKeyEvent& e) override {
        JWidget* w = currentWidget();
        return w ? w->handleKeyEvent(e) : false;
    }
    bool handleScroll(float mx, float my, float wheel) override {
        JWidget* w = currentWidget();
        return w ? w->handleScroll(mx, my, wheel) : false;
    }

    JVariant getRef(const std::string& key) const override {
        if (key == "currentIndex") return static_cast<int64_t>(m_current);
        if (key == "count")        return static_cast<int64_t>(m_pages.size());
        return JWidget::getRef(key);
    }

private:
    // Size the current page to exactly fill the stack's own box.
    void _arrange() {
        if (JWidget* w = currentWidget())
            w->setBounds(m_graph.getLayoutConst(m_nodeId).boundingBox);
    }
    // Show the current page, hide every other — so hidden pages take no input,
    // no focus, and no Tab stop.
    void _applyVisibility() {
        for (int i = 0; i < static_cast<int>(m_pages.size()); ++i)
            m_pages[i]->setVisible(i == m_current);
    }

    std::vector<JWidget*> m_pages;   // non-owning
    int                   m_current{-1};
};

} // inline namespace jf
