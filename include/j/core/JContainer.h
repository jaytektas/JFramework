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
// hand-drawing. Non-owning children (the app owns the widgets, as it owns the container).
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

    // Add a child to the tree; registers it with the layout engine so it's arranged/measured.
    JContainer* add(JWidget* w) {
        if (!w) return this;
        m_children.push_back(w);
        m_graph.addChild(m_nodeId, w->getNodeId());
        return this;
    }
    const std::vector<JWidget*>& children() const { return m_children; }

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
        for (JWidget* w : m_children) if (w->isVisible()) w->populateRenderPrimitives(buf);
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
    std::vector<JWidget*> m_children;   // non-owning
};

} // inline namespace jf
