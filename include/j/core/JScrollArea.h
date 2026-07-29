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
            for (JWidget* w : m_children) {
                if (w->isVisible()) w->handleMouseMove(mx, my);
            }
        } else {
            m_hovered = false;
        }
    }

    void handleMousePress(float mx, float my) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            float trackW = 10.0f;
            float trackX = b.x + b.width - trackW - 6.0f;
            if (mx >= trackX) {
                m_draggingScroll = true;
                m_dragStartY = my;
                m_dragStartScrollY = m_scrollY;
            } else {
                for (JWidget* w : m_children) {
                    if (w->isVisible()) w->handleMousePress(mx, my);
                }
            }
        }
    }

    void handleMouseRelease(float mx, float my) override {
        m_draggingScroll = false;
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
        float curY = b.y + 6.0f - m_scrollY;
        float innerW = b.width - 16.0f;
        float totalH = 12.0f;

        for (JWidget* w : m_children) {
            auto& wl = m_graph.getLayout(w->getNodeId());
            wl.boundingBox.x = b.x + 8.0f;
            wl.boundingBox.y = curY;
            wl.boundingBox.width = innerW;
            
            curY += wl.boundingBox.height + 6.0f;
            totalH += wl.boundingBox.height + 6.0f;
        }

        float maxScrollY = std::max(0.0f, totalH - b.height);
        m_scrollY = std::clamp(m_scrollY, 0.0f, maxScrollY);

        curY = b.y + 6.0f - m_scrollY;
        for (JWidget* w : m_children) {
            auto& wl = m_graph.getLayout(w->getNodeId());
            wl.boundingBox.y = curY;
            curY += wl.boundingBox.height + 6.0f;
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
    bool    m_hovered{false};
    bool    m_draggingScroll{false};
    float   m_dragStartY{0.0f};
    float   m_dragStartScrollY{0.0f};
};

} // inline namespace jf
