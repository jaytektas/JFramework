#pragma once

// JSeparator.

#include "JWidget.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
// JSeparator
// ============================================================================

class JSeparator : public JWidget {
public:
    enum class JOrientation { Horizontal, Vertical };

    JSeparator(JSceneGraph& graph, JOrientation orient = JOrientation::Horizontal,
              float size = 280.0f)
        : JWidget(graph, "JSeparator"), m_orient(orient)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        if (orient == JOrientation::Horizontal) { l.boundingBox.width = size; l.boundingBox.height = 1.0f; }
        else                                   { l.boundingBox.width = 1.0f; l.boundingBox.height = size; }
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Border);
    }


private:
    JOrientation m_orient;
};

} // inline namespace jf
