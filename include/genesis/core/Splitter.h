#pragma once

#include <genesis/core/BaseWidgets.h>

#include <algorithm>
#include <numeric>
#include <vector>

inline namespace jf {

// ============================================================================
// JSplitter — resizable split-pane container
//
// Children are separated by draggable dividers.  Call layout() each frame
// before rendering to update child bounding boxes.
//
// Usage:
//   JSplitter split(graph, JSplitter::JOrientation::Horizontal, 600, 400);
//   JSplitter vSplit(graph, JSplitter::JOrientation::Vertical, 400, 300);
// ============================================================================

class JSplitter : public JWidget {
public:
    enum class JOrientation { Horizontal, Vertical };

    static constexpr float kDividerWidth = 5.0f;

    struct JPane {
        JWidget* widget;
        float   fraction;
    };

    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    JSplitter(JSceneGraph& graph,
             JOrientation orient = JOrientation::Horizontal,
             float w = 600.0f,
             float h = 400.0f)
        : JWidget(graph, "JSplitter")
        , m_orient(orient)
    {
        auto& bb = m_graph.getLayout(m_nodeId).boundingBox;
        bb.width  = w;
        bb.height = h;
    }

    // -------------------------------------------------------------------------
    // JPane management
    // -------------------------------------------------------------------------

    void addPane(JWidget* widget, float fraction = -1.0f)
    {
        if (!widget) return;

        if (fraction < 0.0f) {
            // Auto-distribute: reassign equal share to all panes including new one.
            float share = 1.0f / static_cast<float>(m_panes.size() + 1);
            for (auto& p : m_panes)
                p.fraction = share;
            m_panes.push_back({widget, share});
            // After auto-distribution fractions are already correct (sum == 1).
        } else {
            // Explicit weight: just record it; layout() normalizes before use.
            m_panes.push_back({widget, fraction});
        }
    }

    const std::vector<JPane>& panes() const { return m_panes; }
    JOrientation orientation()        const { return m_orient; }

    // -------------------------------------------------------------------------
    // Layout — call once per frame before rendering
    // -------------------------------------------------------------------------

    void layout()
    {
        if (m_panes.empty()) return;
        normalizeFractions(); // ensure fractions sum to 1 before computing pixel sizes

        const auto& selfBB = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const float selfX  = selfBB.x;
        const float selfY  = selfBB.y;
        const float selfW  = selfBB.width;
        const float selfH  = selfBB.height;

        const int   n         = static_cast<int>(m_panes.size());
        const float divTotal  = kDividerWidth * static_cast<float>(std::max(0, n - 1));
        const float available = (m_orient == JOrientation::Horizontal)
                                    ? selfW - divTotal
                                    : selfH - divTotal;

        float cursor = 0.0f;

        for (int i = 0; i < n; ++i) {
            JWidget* child  = m_panes[i].widget;
            float   frac   = m_panes[i].fraction;
            float   paneSize = available * frac;

            auto& bb = m_graph.getLayout(child->getNodeId()).boundingBox;

            if (m_orient == JOrientation::Horizontal) {
                bb.x      = selfX + cursor;
                bb.y      = selfY;
                bb.width  = paneSize;
                bb.height = selfH;
            } else {
                bb.x      = selfX;
                bb.y      = selfY + cursor;
                bb.width  = selfW;
                bb.height = paneSize;
            }

            cursor += paneSize;

            // Advance past divider (except after the last pane)
            if (i < n - 1)
                cursor += kDividerWidth;
        }
    }

    // -------------------------------------------------------------------------
    // Input handling
    // -------------------------------------------------------------------------

    void handleMouseMove(float mx, float my) override
    {
        if (m_draggingDivider >= 0) {
            // Compute available space
            const auto& selfBB = m_graph.getLayoutConst(m_nodeId).boundingBox;
            const int   n         = static_cast<int>(m_panes.size());
            const float divTotal  = kDividerWidth * static_cast<float>(std::max(0, n - 1));
            const float available = (m_orient == JOrientation::Horizontal)
                                        ? selfBB.width  - divTotal
                                        : selfBB.height - divTotal;

            const float pos   = (m_orient == JOrientation::Horizontal) ? mx : my;
            const float delta = pos - m_dragStart;
            const float df    = (available > 0.0f) ? delta / available : 0.0f;

            const float minFrac = 0.02f;
            const float sumAB   = m_dragFractionA + m_dragFractionB;

            float newA = m_dragFractionA + df;
            newA = std::clamp(newA, minFrac, sumAB - minFrac);
            float newB = sumAB - newA;

            m_panes[m_draggingDivider].fraction     = newA;
            m_panes[m_draggingDivider + 1].fraction = newB;

            m_graph.invalidateNode(m_nodeId, DirtySelf);
        } else {
            m_hoveredDivider = hitTestDivider(mx, my);
        }
    }

    void handleMousePress(float mx, float my) override
    {
        int hit = hitTestDivider(mx, my);
        if (hit >= 0) {
            m_draggingDivider = hit;
            m_dragStart       = (m_orient == JOrientation::Horizontal) ? mx : my;
            m_dragFractionA   = m_panes[hit].fraction;
            m_dragFractionB   = m_panes[hit + 1].fraction;
        }
    }

    void handleMouseRelease(float, float) override
    {
        m_draggingDivider = -1;
    }

    // -------------------------------------------------------------------------
    // Rendering
    // -------------------------------------------------------------------------

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override
    {
        if (m_panes.size() < 2) return;

        const auto& selfBB = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const float selfX  = selfBB.x;
        const float selfY  = selfBB.y;
        const float selfW  = selfBB.width;
        const float selfH  = selfBB.height;

        const int   n         = static_cast<int>(m_panes.size());
        const float divTotal  = kDividerWidth * static_cast<float>(std::max(0, n - 1));
        const float available = (m_orient == JOrientation::Horizontal)
                                    ? selfW - divTotal
                                    : selfH - divTotal;

        float cursor = 0.0f;

        for (int i = 0; i < n - 1; ++i) {
            cursor += available * m_panes[i].fraction;

            const bool active = (i == m_draggingDivider || i == m_hoveredDivider);
            const uint8_t* color = active ? Colors::Accent : Colors::Surface3;

            if (m_orient == JOrientation::Horizontal) {
                buf.pushRectangle(selfX + cursor, selfY,
                                  kDividerWidth, selfH,
                                  color);
            } else {
                buf.pushRectangle(selfX, selfY + cursor,
                                  selfW, kDividerWidth,
                                  color);
            }

            cursor += kDividerWidth;
        }
    }

    // -------------------------------------------------------------------------
    // AI interface
    // -------------------------------------------------------------------------

    JAISemanticNode getSemanticNode() const override {
        return {"JSplitter", m_debugName, "", false};
    }

private:
    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    int hitTestDivider(float mx, float my) const
    {
        if (m_panes.size() < 2) return -1;

        const auto& selfBB = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const float selfX  = selfBB.x;
        const float selfY  = selfBB.y;
        const float selfW  = selfBB.width;
        const float selfH  = selfBB.height;

        const int   n         = static_cast<int>(m_panes.size());
        const float divTotal  = kDividerWidth * static_cast<float>(std::max(0, n - 1));
        const float available = (m_orient == JOrientation::Horizontal)
                                    ? selfW - divTotal
                                    : selfH - divTotal;

        // JHit zone expands 4px on each side beyond kDividerWidth for easier grabbing
        const float hitPad = 4.0f;

        float cursor = 0.0f;

        for (int i = 0; i < n - 1; ++i) {
            cursor += available * m_panes[i].fraction;

            if (m_orient == JOrientation::Horizontal) {
                float divX = selfX + cursor;
                if (mx >= divX - hitPad && mx <= divX + kDividerWidth + hitPad &&
                    my >= selfY          && my <= selfY + selfH)
                    return i;
            } else {
                float divY = selfY + cursor;
                if (my >= divY - hitPad && my <= divY + kDividerWidth + hitPad &&
                    mx >= selfX          && mx <= selfX + selfW)
                    return i;
            }

            cursor += kDividerWidth;
        }

        return -1;
    }

    void normalizeFractions()
    {
        if (m_panes.empty()) return;

        float sum = 0.0f;
        for (const auto& p : m_panes)
            sum += p.fraction;

        if (sum <= 0.0f) {
            // Degenerate: assign equal shares
            float share = 1.0f / static_cast<float>(m_panes.size());
            for (auto& p : m_panes)
                p.fraction = share;
            return;
        }

        for (auto& p : m_panes)
            p.fraction /= sum;
    }

    // -------------------------------------------------------------------------
    // JState
    // -------------------------------------------------------------------------

    JOrientation      m_orient;
    std::vector<JPane> m_panes;
    int              m_draggingDivider{-1};
    int              m_hoveredDivider{-1};
    float            m_dragStart{0.0f};
    float            m_dragFractionA{0.5f};
    float            m_dragFractionB{0.5f};
};

} // inline namespace jf
