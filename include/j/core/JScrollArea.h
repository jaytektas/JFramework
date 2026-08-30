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
        return adopt(std::move(child));      // parents it AND owns the lifetime
    }
    // Non-owning add: register a widget OWNED ELSEWHERE (an adopt()-ed child, or a member) into this scroll
    // area. It is laid out + painted here but its lifetime is the caller's — for re-addable content that
    // survives clearChildren(). For create-and-forget content, prefer the owning addChildWidget above.
    void addChildWidget(JWidget* w) { addChild(w); }
    // Detach all children and DESTROY the ones this scroll area owns; non-owned children live on.
    void clearChildren() {                       // rebuildable content (per-selection form)
        disownAll();
        removeAllChildren();
        m_scrollY = 0.0f;
    }

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
        m_scrollY = std::clamp(m_scrollY + delta, 0.0f, _maxScrollY());
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    // THE CONTENT'S HEIGHT, by the same arithmetic the painter lays it out with — its own padding and its
    // own gap. This was written out as "12 + sum(h + 6)" here and in three other places, which is the
    // DEFAULT padding spelled as a constant: a list that sets its own (a dropdown calls
    // setContentPadding(0,0,0), because a gap between rows is a strip that swallows clicks) had every one
    // of those disagree with what was drawn. On a 42-entry combo list that is 1440px of content believed
    // against 1176 painted — so the scrollbar hit-tested a thumb 17px shorter than the one on screen, and
    // grabbing the visible thumb near its lower end PAGED the list instead of dragging it.
    float _contentHeight() const {
        float totalH = 2.0f * m_padY;
        for (JWidget* w : m_children)
            totalH += m_graph.getLayoutConst(w->getNodeId()).boundingBox.height + m_gap;
        return totalH;
    }
    float _maxScrollY() const {
        return std::max(0.0f, _contentHeight() - m_graph.getLayoutConst(m_nodeId).boundingBox.height);
    }
    // WHERE THE SCROLLBAR IS — one answer for the painter and for the hit test. They had two: the bar was
    // drawn in the last 12px and hit-tested from 16px in, so a 4px column of perfectly visible row armed a
    // scroll instead of selecting the row under it — and then swallowed the release, because a release that
    // ends a scroll drag is deliberately not forwarded to the children.
    static constexpr float kTrackW = 10.0f;
    float _trackX() const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        return b.x + b.width - kTrackW - 2.0f;
    }
    bool _scrolls() const {
        return _contentHeight() > m_graph.getLayoutConst(m_nodeId).boundingBox.height;
    }

public:
    void handleMouseMove(float mx, float my) override {
        // A thumb drag ends with the button, not with the event: if the release went to someone else (a
        // drag-drop session withholds it, a grab redirects it) this is the only thing that lets go.
        if (m_draggingScroll && !JWidget::s_leftDown) m_draggingScroll = false;
        if (m_draggingScroll) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            const float totalH = _contentHeight();
            const float maxScrollY = _maxScrollY();
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
        return my >= b.y && my <= b.y + b.height && mx >= _trackX() && mx <= b.x + b.width;
    }
    bool isDraggingScroll() const { return m_draggingScroll; }

    void handleMousePress(float mx, float my) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            const float trackX = _trackX();            // the bar as DRAWN — see _trackX()
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

            m_scrollY = std::clamp(m_scrollY - wheel * 40.0f, 0.0f, _maxScrollY());
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

        for (JWidget* w : m_children) {
            auto& wl = m_graph.getLayout(w->getNodeId());
            wl.boundingBox.x = b.x + m_padX;
            wl.boundingBox.y = curY;
            wl.boundingBox.width = innerW;

            curY += wl.boundingBox.height + m_gap;
        }

        // The same arithmetic the hit tests use — literally, so the thumb you grab is the thumb that is
        // drawn and a wheel cannot scroll past where the rows end.
        const float totalH = _contentHeight();
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
            const float trackW = kTrackW;
            const float trackH = b.height - 4.0f;
            const float trackX = _trackX();            // the hit test asks the same question — see _trackX()
            const float trackY = b.y + 2.0f;

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
    float   m_scrollY{0.0f};
    float   m_padX{8.0f}, m_padY{6.0f}, m_gap{6.0f};
    bool    m_hovered{false};
    bool    m_draggingScroll{false};
    float   m_dragStartY{0.0f};
    float   m_dragStartScrollY{0.0f};
};

} // inline namespace jf
