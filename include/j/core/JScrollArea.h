// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

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
        m_scrollX = 0.0f;
    }

    // SIDEWAYS TOO, when asked. Off (as it always was), a child is laid at the area's width and whatever does
    // not fit is cut off at the right. On, a child is laid no narrower than its measured minimum — the room
    // its own children need — and when that is wider than the view, a bar along the bottom scrolls it.
    void setHorizontalScroll(bool on) {
        if (m_horizontal == on) return;
        m_horizontal = on;
        if (!on) m_scrollX = 0.0f;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    bool horizontalScroll() const { return m_horizontal; }
    void setScrollX(float x) { m_scrollX = std::max(0.0f, x); }
    float scrollX() const    { return m_scrollX; }

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
        const float botGap = (wb.y + wb.height) - (b.y + _viewH() - pad);  // >0 => below it
        float delta = 0.f;
        if (topGap < 0.f)      delta = topGap;      // scroll up to reveal
        else if (botGap > 0.f) delta = botGap;      // scroll down to reveal
        // Across the same way, when the area scrolls sideways: a control off to the right brought on.
        float across = 0.f;
        if (_scrollsX()) {
            const float leftGap  = wb.x - (b.x + pad);
            const float rightGap = (wb.x + wb.width) - (b.x + _viewW() - pad);
            if (leftGap < 0.f)       across = leftGap;
            else if (rightGap > 0.f) across = std::min(rightGap, leftGap);   // its left edge kept in view
        }
        if (delta == 0.f && across == 0.f) return;
        m_scrollY = std::clamp(m_scrollY + delta, 0.0f, _maxScrollY());
        m_scrollX = std::clamp(m_scrollX + across, 0.0f, _maxScrollX());
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
    // The content's width when it scrolls sideways: its widest child's measured minimum, with the padding.
    // (Not scrolling sideways, a child is the area's width, and there is nothing to measure.)
    float _contentWidth() const {
        if (!m_horizontal) return 0.0f;
        float widest = 0.0f;
        for (JWidget* w : m_children) widest = std::max(widest, m_graph.getLayoutConst(w->getNodeId()).minWidth);
        return widest + 2.0f * m_padX;
    }
    // A bar's strip: its track and the margin beside it. The view is what the bars leave.
    static constexpr float kTrackW = 10.0f;
    static constexpr float kBarStrip = kTrackW + 2.0f;
    // Whether the bottom bar is there: content wider than the view, the view narrowed by the side bar when
    // the content is taller than the whole area (asked that way round, so neither question needs the other).
    bool _scrollsX() const {
        if (!m_horizontal) return false;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        return _contentWidth() > b.width - (_contentHeight() > b.height ? kBarStrip : 0.0f);
    }
    float _viewH() const {
        return m_graph.getLayoutConst(m_nodeId).boundingBox.height - (_scrollsX() ? kBarStrip : 0.0f);
    }
    float _viewW() const {
        return m_graph.getLayoutConst(m_nodeId).boundingBox.width - (_scrolls() ? kBarStrip : 0.0f);
    }
    float _maxScrollY() const { return std::max(0.0f, _contentHeight() - _viewH()); }
    float _maxScrollX() const { return std::max(0.0f, _contentWidth() - _viewW()); }
    // WHERE THE SCROLLBAR IS — one answer for the painter and for the hit test. They had two: the bar was
    // drawn in the last 12px and hit-tested from 16px in, so a 4px column of perfectly visible row armed a
    // scroll instead of selecting the row under it — and then swallowed the release, because a release that
    // ends a scroll drag is deliberately not forwarded to the children.
    float _trackX() const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        return b.x + b.width - kBarStrip;
    }
    float _trackY() const {                     // the bottom bar's, as _trackX is the side bar's
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        return b.y + b.height - kBarStrip;
    }
    bool _scrolls() const { return _contentHeight() > _viewH(); }
    // A bar's track and thumb along its own axis — the side bar's down, the bottom bar's across — one answer
    // for the painter, the hit tests and a drag.
    struct Thumb { float trackStart, trackLen, start, len; };
    static Thumb _thumb(float trackStart, float view, float content, float scroll) {
        const float trackLen = view - 4.0f;
        const float len = std::max(20.0f, trackLen * (view / std::max(1.0f, content)));
        const float most = std::max(0.0f, content - view);
        return { trackStart, trackLen, trackStart + (most > 0.f ? scroll / most : 0.f) * (trackLen - len), len };
    }
    Thumb _thumbY() const {
        return _thumb(m_graph.getLayoutConst(m_nodeId).boundingBox.y + 2.0f, _viewH(), _contentHeight(), m_scrollY);
    }
    Thumb _thumbX() const {
        return _thumb(m_graph.getLayoutConst(m_nodeId).boundingBox.x + 2.0f, _viewW(), _contentWidth(), m_scrollX);
    }

public:
    void handleMouseMove(float mx, float my) override {
        // A thumb drag ends with the button, not with the event: if the release went to someone else (a
        // drag-drop session withholds it, a grab redirects it) this is the only thing that lets go.
        if (m_draggingScroll && !JWidget::s_leftDown) m_draggingScroll = false;
        if (m_draggingScroll) {
            const Thumb t = m_dragAcross ? _thumbX() : _thumbY();
            const float thumbRange = t.trackLen - t.len;
            if (thumbRange > 0.0f) {
                if (m_dragAcross)
                    m_scrollX = std::clamp(m_dragStartScroll + (mx - m_dragStart) * _maxScrollX() / thumbRange, 0.0f, _maxScrollX());
                else
                    m_scrollY = std::clamp(m_dragStartScroll + (my - m_dragStart) * _maxScrollY() / thumbRange, 0.0f, _maxScrollY());
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
    // (The bottom bar, when the area scrolls sideways, is a scrollbar too.)
    bool pointInScrollbar(float mx, float my) const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx < b.x || mx > b.x + b.width || my < b.y || my > b.y + b.height) return false;
        return (_scrolls() && mx >= _trackX()) || (_scrollsX() && my >= _trackY());   // no bar drawn, no dead strip
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
                const Thumb t = _thumbY();
                if (my < t.start || my > t.start + t.len) {
                    const float page = _viewH() * 0.9f;              // a page, less a sliver of overlap
                    m_scrollY = std::clamp(m_scrollY + (my < t.start ? -page : page), 0.0f, _maxScrollY());
                    m_graph.invalidateNode(m_nodeId, DirtySelf);
                }
                m_draggingScroll = true;
                m_dragAcross = false;
                m_dragStart = my;
                m_dragStartScroll = m_scrollY;
            } else if (my >= _trackY() && _scrollsX()) {
                // The bottom bar, the same way across: drag the thumb, page either side of it.
                const Thumb t = _thumbX();
                if (mx < t.start || mx > t.start + t.len) {
                    const float page = _viewW() * 0.9f;
                    m_scrollX = std::clamp(m_scrollX + (mx < t.start ? -page : page), 0.0f, _maxScrollX());
                    m_graph.invalidateNode(m_nodeId, DirtySelf);
                }
                m_draggingScroll = true;
                m_dragAcross = true;
                m_dragStart = mx;
                m_dragStartScroll = m_scrollX;
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

            // Down and up; across when there is only across to go, or with Shift held (as every
            // toolkit's wheel does).
            if (_scrollsX() && (JWidget::s_shiftDown || !_scrolls()))
                m_scrollX = std::clamp(m_scrollX - wheel * 40.0f, 0.0f, _maxScrollX());
            else
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

        // The same arithmetic the hit tests use — literally, so the thumb you grab is the thumb that is
        // drawn and a wheel cannot scroll past where the rows end.
        const float maxScrollY = _maxScrollY(), maxScrollX = _maxScrollX();
        m_scrollY = std::clamp(m_scrollY, 0.0f, maxScrollY);
        m_scrollX = std::clamp(m_scrollX, 0.0f, maxScrollX);

        // Perform Layout: each child at the area's width, or (scrolling sideways) its own minimum when wider.
        float curY = b.y + m_padY - m_scrollY;
        const float innerW = b.width - 2.0f * m_padX;
        for (JWidget* w : m_children) {
            auto& wl = m_graph.getLayout(w->getNodeId());
            wl.boundingBox.x = b.x + m_padX - m_scrollX;
            wl.boundingBox.y = curY;
            wl.boundingBox.width = m_horizontal ? std::max(innerW, wl.minWidth) : innerW;
            curY += wl.boundingBox.height + m_gap;
        }

        // Render with clip scissor: the view, clear of the bars.
        buf.pushClip(b.x + 1.0f, b.y + 1.0f, b.width - kBarStrip - 1.0f, _viewH() - 2.0f);
        for (JWidget* w : m_children) {
            const auto& wb = m_graph.getLayoutConst(w->getNodeId()).boundingBox;
            if (wb.y + wb.height >= b.y && wb.y <= b.y + b.height) {
                if (w->isVisible()) {
                    w->populateRenderPrimitives(buf);
                }
            }
        }
        buf.popClip();

        // Render the bars: the hit tests ask the same questions — see _thumbY / _thumbX.
        if (maxScrollY > 0.0f) {
            const Thumb t = _thumbY();
            buf.pushRectangle(_trackX(), t.trackStart, kTrackW, t.trackLen, Colors::ScrollTrack, 3.0f);
            const uint8_t* thumbColor = m_draggingScroll && !m_dragAcross ? Colors::ScrollThumbActive : Colors::ScrollThumb;
            buf.pushRectangle(_trackX() + 1.0f, t.start, kTrackW - 2.0f, t.len, thumbColor, 3.0f);
        }
        if (_scrollsX()) {
            const Thumb t = _thumbX();
            buf.pushRectangle(t.trackStart, _trackY(), t.trackLen, kTrackW, Colors::ScrollTrack, 3.0f);
            const uint8_t* thumbColor = m_draggingScroll && m_dragAcross ? Colors::ScrollThumbActive : Colors::ScrollThumb;
            buf.pushRectangle(t.start, _trackY() + 1.0f, t.len, kTrackW - 2.0f, thumbColor, 3.0f);
        }
    }


private:
    float   m_scrollY{0.0f};
    float   m_scrollX{0.0f};
    bool    m_horizontal{false};                 // setHorizontalScroll
    float   m_padX{8.0f}, m_padY{6.0f}, m_gap{6.0f};
    bool    m_hovered{false};
    bool    m_draggingScroll{false};
    bool    m_dragAcross{false};                 // the drag is the bottom bar's
    float   m_dragStart{0.0f};                   // where it began, along its bar
    float   m_dragStartScroll{0.0f};
};

} // inline namespace jf
