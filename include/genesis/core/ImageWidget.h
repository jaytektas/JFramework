#pragma once

#include <genesis/core/BaseWidgets.h>
#include <genesis/graphics/GpuHal.h>

namespace Genesis {

// ============================================================================
// JImageWidget — displays a GPU-resident RGBA texture.
//
// Usage:
//   // Upload pixels through the platform window's HAL, then:
//   auto tex = hal.uploadTexture(rgba, w, h);
//   JImageWidget img(graph, tex, w, h);
//   img.setTint({255, 200, 200, 255});  // red tint (optional)
//
//   // Release when no longer needed:
//   img.releaseTexture(hal);
// ============================================================================
class JImageWidget : public JWidget {
public:
    JImageWidget(JSceneGraph& graph,
                TextureHandle tex = kNullTexture,
                float w = 100.0f,
                float h = 100.0f)
        : JWidget(graph, "JImageWidget")
        , m_tex(tex)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width  = w;
        l.boundingBox.height = h;
    }

    void setTexture(TextureHandle tex, float w = -1, float h = -1) {
        m_tex = tex;
        if (w > 0 && h > 0) {
            auto& l = m_graph.getLayout(m_nodeId);
            l.boundingBox.width  = w;
            l.boundingBox.height = h;
        }
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    TextureHandle texture() const { return m_tex; }

    void setTint(const uint8_t rgba[4]) {
        for (int i = 0; i < 4; ++i) m_tint[i] = rgba[i];
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    // Set UV sub-rect for sprite sheets (0,0,1,1 = full texture).
    void setUVRect(float u0, float v0, float u1, float v1) {
        m_u0 = u0; m_v0 = v0; m_u1 = u1; m_v1 = v1;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    // Releases the underlying texture handle via the HAL.
    void releaseTexture(JGpuHal& hal) {
        if (m_tex != kNullTexture) {
            hal.releaseTexture(m_tex);
            m_tex = kNullTexture;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        }
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        if (m_tex == kNullTexture) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        buf.pushImage(b.x, b.y, b.width, b.height, m_tex, m_tint,
                      m_u0, m_v0, m_u1, m_v1);
    }

    JAISemanticNode getSemanticNode() const override {
        return {"JImageWidget", m_debugName, "", false};
    }

private:
    TextureHandle m_tex{kNullTexture};
    uint8_t       m_tint[4]{255, 255, 255, 255};
    float         m_u0{0.f}, m_v0{0.f}, m_u1{1.f}, m_v1{1.f};
};

} // namespace Genesis
