#pragma once

// JContainer.

#include "JWidget.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
// JContainer — a plain container widget: holds a child widget tree, arranges it with the
// scene-graph layout engine (Flex / Grid / Form via its JLayoutComponent), renders it, and
// routes input to it. No chrome of its own. The reusable building block for panels, property
// forms, toolbars, and dock content (JDockWidget::setContent) — the app composes it instead of
// hand-drawing. A container OWNS the children added via add(std::unique_ptr<T>) (Qt QObject model)
// and destroys them with itself; the legacy add(JWidget*) is non-owning for existing call sites.
// ============================================================================
class JContainer : public JWidget {
public:
    JContainer(JSceneGraph& graph, float w = 200.0f, float h = 100.0f)
        : JWidget(graph, "JContainer")
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width  = w;
        l.boundingBox.height = h;
    }

    // Owning add (PREFERRED — Qt QObject model): the container takes ownership of the child and destroys it
    // with itself, so callers no longer keep a parallel std::unique_ptr vector. Registers it with the layout
    // engine and returns the raw pointer for wiring.
    template <class T>
    T* add(std::unique_ptr<T> child) {
        if (!child) return nullptr;
        T* p = adopt(std::move(child));                 // JWidget owns the lifetime (RAII)
        m_children.push_back(p);                        // layout/paint child list
        m_graph.addChild(m_nodeId, p->getNodeId());
        return p;
    }
    // Non-owning add (legacy): the caller retains ownership of `w`. Kept for existing call sites during the
    // ownership migration; prefer add(std::unique_ptr<T>) for anything new.
    JContainer* add(JWidget* w) {
        if (!w) return this;
        m_children.push_back(w);
        m_graph.addChild(m_nodeId, w->getNodeId());
        return this;
    }
    const std::vector<JWidget*>& children() const { return m_children; }

    // Detach all children and DESTROY the ones this container owns (adopted via add(unique_ptr)); non-owned
    // children live on. Used to rebuild a form with a new set of rows.
    void clear() { m_graph.clearChildren(m_nodeId); m_children.clear(); disownAll(); }

    // Layout configuration — thin pass-throughs to this node's layout component (chainable).
    JContainer* setLayoutMode(JLayoutMode m)   { m_graph.getLayout(m_nodeId).mode = m; return this; }
    JContainer* setColumns(int n)              { m_graph.getLayout(m_nodeId).columns = n; return this; }
    JContainer* setDirection(JFlexDirection d) { m_graph.getLayout(m_nodeId).direction = d; return this; }
    JContainer* setGap(float g)                { m_graph.getLayout(m_nodeId).gap = g; return this; }
    JContainer* setPadding(JEdges p)           { m_graph.getLayout(m_nodeId).padding = p; return this; }
    JContainer* setAlignItems(JAlignItems a)   { m_graph.getLayout(m_nodeId).alignItems = a; return this; }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        // Arrange the subtree into our current box (set by the host via setBounds), then paint
        // each child. Fixed constraints pin the container to its box; children flow within it.
        const JRect b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        m_graph.computeLayout(m_nodeId, JConstraints{b.width, b.width, b.height, b.height});
        // Confine children to the container's box — a child cannot paint outside its parent (Qt/GTK clip
        // children to the parent's geometry; overflow lives in separate top-level windows: menus, popups,
        // tooltips). Nested containers intersect their clips, so the subtree stays within every ancestor.
        buf.pushClip(b.x, b.y, b.width, b.height);
        for (JWidget* w : m_children) if (w->isVisible()) w->populateRenderPrimitives(buf);
        buf.popClip();
    }

    void handleMouseMove(float mx, float my) override {
        for (JWidget* w : m_children) if (w->isVisible()) w->handleMouseMove(mx, my);
    }
    void handleMousePress(float mx, float my) override {
        for (JWidget* w : m_children) if (w->isVisible()) w->handleMousePress(mx, my);
    }
    void handleMouseRelease(float mx, float my) override {
        for (JWidget* w : m_children) if (w->isVisible()) w->handleMouseRelease(mx, my);
    }
    bool handleScroll(float mx, float my, float wheel) override {
        bool consumed = false;
        for (JWidget* w : m_children) if (w->isVisible()) consumed |= w->handleScroll(mx, my, wheel);
        return consumed;
    }


private:
    std::vector<JWidget*> m_children;   // layout/paint list (raw); lifetime of owned children is JWidget::m_ownedChildren
};

} // inline namespace jf
