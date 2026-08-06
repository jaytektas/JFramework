#pragma once

// JScrollArea.

#include "JWidget.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
// JScrollArea
// ============================================================================

class JScrollArea : public JWidget {
public:
    JScrollArea(JSceneGraph& graph, float w = 320.0f, float h = 200.0f)
        : JWidget(graph, "JScrollArea")
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width  = w;
        l.boundingBox.height = h;
    }

    // Owning add (PREFERRED — Qt model): the scroll area owns the child and destroys it with itself / on
    // clearChildren(), so callers drop their parallel unique_ptr vectors. Returns the raw pointer for wiring.
    template <class T>
    T* addChildWidget(std::unique_ptr<T> child) {
        if (!child) return nullptr;
        T* p = adopt(std::move(child));
        m_children.push_back(p);
        return p;
    }
    // Non-owning add: register a widget OWNED ELSEWHERE (an adopt()-ed child, or a member) into this scroll
    // area. It is laid out + painted here but its lifetime is the caller's — for re-addable content that
    // survives clearChildren(). For create-and-forget content, prefer the owning addChildWidget above.
    void addChildWidget(JWidget* w) {
        m_children.push_back(w);
    }
    // Detach all children and DESTROY the ones this scroll area owns; non-owned children live on.
    void clearChildren() { m_children.clear(); m_scrollY = 0.0f; disownAll(); }   // rebuildable content (per-selection form)

    const std::vector<JWidget*>& children() const { return m_children; }

    // Scroll to an offset WITHOUT needing a layout pass first. Children are positioned during painting, so
    // before the first paint there are no boxes for revealChild() to aim at — which is why a list opened on
    // an entry 57 rows down still opened at the top. Clamped on the next pass, like every other scroll.
    void setScrollY(float y) { m_scrollY = std::max(0.0f, y); }
    float scrollY() const    { return m_scrollY; }
    // How children are inset and spaced. A form wants breathing room; a LIST does not — a dropdown row has
    // to be clickable across the full width of the list, and a gap between rows is a strip that swallows
    // clicks. Defaults match what this area has always used.
    void setContentPadding(float sideX, float topY, float childGap) {
        m_padX = sideX; m_padY = topY; m_gap = childGap;
    }
    float childGap() const { return m_gap; }
    float topPad()   const { return m_padY; }

    // Non-owned children participate in focus traversal exactly like owned ones.
    void collectChildren(std::vector<JWidget*>& out) const override {
        JWidget::collectChildren(out);
        for (JWidget* c : m_children) if (c) out.push_back(c);
    }

    // Scroll `w` (a direct or nested child) into view. Called by the focus manager when focus moves, so
    // tabbing to a control that is scrolled off simply brings it on screen instead of the focus ring
    // vanishing somewhere below the fold.
    void revealChild(JWidget* w) override {
        if (!w || m_children.empty()) return;
        const auto& b  = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const auto& wb = m_graph.getLayoutConst(w->getNodeId()).boundingBox;
        if (wb.height <= 0.f || b.height <= 0.f) return;
        const float pad    = 6.0f;
        const float topGap = wb.y - (b.y + pad);                       // <0 => above the viewport
        const float botGap = (wb.y + wb.height) - (b.y + b.height - pad);  // >0 => below it
        float delta = 0.f;
        if (topGap < 0.f)      delta = topGap;      // scroll up to reveal
        else if (botGap > 0.f) delta = botGap;      // scroll down to reveal
        if (delta == 0.f) return;
        float totalH = 12.0f;
        for (JWidget* c : m_children) totalH += m_graph.getLayoutConst(c->getNodeId()).boundingBox.height + 6.0f;
        const float maxScrollY = std::max(0.0f, totalH - b.height);
        m_scrollY = std::clamp(m_scrollY + delta, 0.0f, maxScrollY);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    float _contentHeight() const {
        float totalH = 12.0f;
        for (JWidget* w : m_children) totalH += m_graph.getLayoutConst(w->getNodeId()).boundingBox.height + 6.0f;
        return totalH;
    }
    bool _scrolls() const {
        return _contentHeight() > m_graph.getLayoutConst(m_nodeId).boundingBox.height;
    }

public:
    void handleMouseMove(float mx, float my) override {
        if (m_draggingScroll) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            float totalH = 12.0f;
            for (JWidget* w : m_children)
                totalH += m_graph.getLayoutConst(w->getNodeId()).boundingBox.height + 6.0f;
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
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            m_hovered = true;
            // On the SCROLLBAR, the rows underneath are not what you are pointing at. Lighting one up there
            // reads as "this is the entry you are about to get" — which, in a combo list where the pointer
            // also moves the keyboard highlight, it very nearly was. Move them out of hover instead.
            const bool onBar = pointInScrollbar(mx, my);
            for (JWidget* w : m_children) {
                if (w->isVisible()) w->handleMouseMove(onBar ? -1.f : mx, onBar ? -1.f : my);
            }
        } else {
            m_hovered = false;
        }
    }

    // Is the pointer over the scrollbar column? Public because a HOST needs to know: a popup that moves its
    // highlight to whatever the pointer is over must not do that while the pointer is on the scrollbar —
    // the row behind the bar is not what you are pointing at, and in a combo list that highlight IS the
    // selection Enter will commit.
    bool pointInScrollbar(float mx, float my) const {
        if (!_scrolls()) return false;                 // no bar drawn, no dead column
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        return my >= b.y && my <= b.y + b.height && mx >= b.x + b.width - 16.0f && mx <= b.x + b.width;
    }
    bool isDraggingScroll() const { return m_draggingScroll; }

    void handleMousePress(float mx, float my) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            float trackW = 10.0f;
            float trackX = b.x + b.width - trackW - 6.0f;
            if (mx >= trackX && _scrolls()) {
                // ON the thumb: drag it. Above or below it: PAGE, as every scrollbar does — clicking the
                // empty track used to arm a drag that never moved, so the list sat there doing nothing
                // while the row behind the bar took the highlight.
                const float trackY = b.y + 2.0f, trackH = b.height - 4.0f;
                const float totalH = _contentHeight();
                const float maxScrollY = std::max(0.0f, totalH - b.height);
                const float thumbH = std::max(20.0f, trackH * (b.height / totalH));
                const float thumbY = trackY + (maxScrollY > 0.f ? (m_scrollY / maxScrollY) : 0.f) * (trackH - thumbH);
                if (my < thumbY || my > thumbY + thumbH) {
                    const float page = b.height * 0.9f;              // a page, less a sliver of overlap
                    m_scrollY = std::clamp(m_scrollY + (my < thumbY ? -page : page), 0.0f, maxScrollY);
                    m_graph.invalidateNode(m_nodeId, DirtySelf);
                }
                m_draggingScroll = true;
                m_dragStartY = my;
                m_dragStartScrollY = m_scrollY;
            } else if (mx >= trackX && !_scrolls()) {
                // Nothing to scroll: the right-hand strip is ordinary content, not a dead margin.
                for (JWidget* w : m_children) if (w->isVisible()) w->handleMousePress(mx, my);
            } else {
                for (JWidget* w : m_children) {
                    if (w->isVisible()) w->handleMousePress(mx, my);
                }
            }
        }
    }

    void handleMouseRelease(float mx, float my) override {
        // A release that ENDS A SCROLLBAR DRAG belongs to the scrollbar, not to whatever is underneath it.
        // Forwarding it anyway handed the release to the child behind the track — and a control that
        // activates on release (a popup list item) treated dragging the scrollbar as picking it: the combo
        // box closed, having quietly selected whichever entry the thumb happened to be over.
        const bool wasScrollDrag = m_draggingScroll;
        m_draggingScroll = false;
        if (wasScrollDrag) return;
        for (JWidget* w : m_children) {
            if (w->isVisible()) w->handleMouseRelease(mx, my);
        }
    }

    bool handleScroll(float mx, float my, float wheel) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            bool consumed = false;
            for (JWidget* w : m_children) {
                if (w->isVisible()) {
                    if (w->handleScroll(mx, my, wheel)) {
                        consumed = true;
                    }
                }
            }
            if (consumed) return true;

            float totalH = 12.0f;
            for (JWidget* w : m_children) {
                totalH += m_graph.getLayoutConst(w->getNodeId()).boundingBox.height + 6.0f;
            }
            float maxScrollY = std::max(0.0f, totalH - b.height);
            m_scrollY = std::clamp(m_scrollY - wheel * 40.0f, 0.0f, maxScrollY);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        }
        return false;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        
        // Background
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::ScrollAreaBg,
                          JStyle::current().hint(JStyleHint::ControlRadius), 1.0f, Colors::Border);

        if (m_children.empty()) return;

        // Perform Layout
        float curY = b.y + m_padY - m_scrollY;
        float innerW = b.width - 2.0f * m_padX;
        float totalH = 2.0f * m_padY;

        for (JWidget* w : m_children) {
            auto& wl = m_graph.getLayout(w->getNodeId());
            wl.boundingBox.x = b.x + m_padX;
            wl.boundingBox.y = curY;
            wl.boundingBox.width = innerW;

            curY += wl.boundingBox.height + m_gap;
            totalH += wl.boundingBox.height + m_gap;
        }

        float maxScrollY = std::max(0.0f, totalH - b.height);
        m_scrollY = std::clamp(m_scrollY, 0.0f, maxScrollY);

        curY = b.y + m_padY - m_scrollY;                 // re-place with the clamped offset
        for (JWidget* w : m_children) {
            auto& wl = m_graph.getLayout(w->getNodeId());
            wl.boundingBox.y = curY;
            curY += wl.boundingBox.height + m_gap;
        }

        // Render with clip scissor
        buf.pushClip(b.x + 1.0f, b.y + 1.0f, b.width - 13.0f, b.height - 2.0f);
        for (JWidget* w : m_children) {
            const auto& wb = m_graph.getLayoutConst(w->getNodeId()).boundingBox;
            if (wb.y + wb.height >= b.y && wb.y <= b.y + b.height) {
                if (w->isVisible()) {
                    w->populateRenderPrimitives(buf);
                }
            }
        }
        buf.popClip();

        // Render Scrollbar
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
    std::vector<JWidget*> m_children;
    float   m_scrollY{0.0f};
    float   m_padX{8.0f}, m_padY{6.0f}, m_gap{6.0f};
    bool    m_hovered{false};
    bool    m_draggingScroll{false};
    float   m_dragStartY{0.0f};
    float   m_dragStartScrollY{0.0f};
};

} // inline namespace jf
