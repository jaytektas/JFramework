#pragma once

#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include <iostream>
#include <algorithm>

#include <genesis/core/muted_logging_mock.h>
namespace { inline constexpr auto& LogLayoutEngine = Genesis::Log::Layout; }

namespace Genesis {

using NodeId = uint32_t;
constexpr NodeId InvalidNodeId = 0xFFFFFFFF;

enum class FlexDirection { Row, Column };
enum class JustifyContent { FlexStart, Center, SpaceBetween };

/**
 * @brief Bitmask flags for the layout invalidation vector.
 */
enum LayoutFlag : uint8_t {
    Clean = 0,
    DirtySelf = 1 << 0,     // Properties of this node changed
    DirtyChildren = 1 << 1  // A child is dirty, requiring parent re-measurement/re-positioning
};

/**
 * @brief Geometric primitives decoupled from layout logic to preserve raw cache locality.
 */
struct Rect {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
};

struct Constraints {
    float minWidth{0.0f};
    float maxWidth{100000.0f};
    float minHeight{0.0f};
    float maxHeight{100000.0f};
};

/**
 * @brief Pure Layout Node Properties. 
 */
struct LayoutComponent {
    FlexDirection direction{FlexDirection::Column};
    JustifyContent justifyContent{JustifyContent::FlexStart};
    float flexGrow{0.0f};
    float padding{0.0f};   // inset on all four sides
    float gap{0.0f};       // space inserted between consecutive children
    float margin{0.0f};

    // Calculated output geometry bounds
    Rect boundingBox;
};

/**
 * @brief Flat, array-backed Scene Graph with Invalidation Vector.
 */
class SceneGraph {
public:
    SceneGraph() = default;
    ~SceneGraph() = default;

    SceneGraph(const SceneGraph&) = delete;
    SceneGraph& operator=(const SceneGraph&) = delete;

    /**
     * @brief Instantiates a node flatly inside the memory arena.
     */
    NodeId createNode(const std::string& debugName = "") {
        NodeId id = static_cast<NodeId>(m_layouts.size());
        m_layouts.emplace_back(LayoutComponent{});
        m_hierarchy.emplace_back(HierarchyComponent{InvalidNodeId, {}, debugName});
        m_dirtyFlags.emplace_back(DirtySelf); // New nodes start dirty
        return id;
    }

    /**
     * @brief Declares a strict Parent-Child layout ownership rule.
     */
    void addChild(NodeId parentId, NodeId childId) {
        if (parentId >= m_hierarchy.size() || childId >= m_hierarchy.size()) {
            qCWarning(LogLayoutEngine) << "Malformed NodeId layout assignment attempted." << std::endl;
            return;
        }
        m_hierarchy[childId].parentId = parentId;
        m_hierarchy[parentId].childrenIds.push_back(childId);
        invalidateNode(parentId, DirtyChildren);
    }

    /**
     * @brief Mark a node as dirty and propagate invalidation up the tree.
     */
    void invalidateNode(NodeId id, LayoutFlag flag = DirtySelf) {
        if (id >= m_dirtyFlags.size()) return;

        m_dirtyFlags[id] |= flag;

        // Propagate DirtyChildren upward until we hit the root or an already marked ancestor
        NodeId parent = m_hierarchy[id].parentId;
        while (parent != InvalidNodeId) {
            if (m_dirtyFlags[parent] & DirtyChildren) break;
            m_dirtyFlags[parent] |= DirtyChildren;
            parent = m_hierarchy[parent].parentId;
        }
    }

    LayoutComponent& getLayout(NodeId id) { 
        invalidateNode(id, DirtySelf); 
        return m_layouts[id]; 
    }

    const LayoutComponent& getLayoutConst(NodeId id) const { return m_layouts[id]; }
    const std::vector<NodeId>& getChildren(NodeId id) const { return m_hierarchy[id].childrenIds; }
    NodeId getParent(NodeId id) const { return m_hierarchy[id].parentId; }
    size_t totalNodes() const { return m_layouts.size(); }
    
    bool isDirty(NodeId id) const { return m_dirtyFlags[id] != Clean; }

    /**
     * @brief Linear Single-Pass Constraint Layout Solver with Invalidation skipping.
     * Respects padding (inset on all sides) and gap (space between children).
     */
    void computeLayout(NodeId nodeId, const Constraints& constraints) {
        if (nodeId >= m_layouts.size()) return;

        if (m_dirtyFlags[nodeId] == Clean) return;

        auto& layout = m_layouts[nodeId];
        const auto& children = m_hierarchy[nodeId].childrenIds;

        layout.boundingBox.width  = constraints.minWidth;
        layout.boundingBox.height = constraints.minHeight;

        if (children.empty()) {
            m_dirtyFlags[nodeId] = Clean;
            return;
        }

        const float pad = layout.padding;
        const float gap = layout.gap;
        const size_t n  = children.size();

        // --- Measure pass ---
        float accumulatedMain = 2.0f * pad;
        float maxCross = 0.0f;

        for (size_t i = 0; i < n; ++i) {
            NodeId childId = children[i];
            auto& childLayout = m_layouts[childId];
            Constraints childConstraints{
                childLayout.boundingBox.width,  constraints.maxWidth  - 2.0f * pad,
                childLayout.boundingBox.height, constraints.maxHeight - 2.0f * pad
            };
            computeLayout(childId, childConstraints);

            if (layout.direction == FlexDirection::Row) {
                accumulatedMain += childLayout.boundingBox.width;
                maxCross = std::max(maxCross, childLayout.boundingBox.height);
            } else {
                accumulatedMain += childLayout.boundingBox.height;
                maxCross = std::max(maxCross, childLayout.boundingBox.width);
            }
            if (i < n - 1) accumulatedMain += gap;
        }

        if (layout.direction == FlexDirection::Row) {
            layout.boundingBox.width  = std::clamp(accumulatedMain, constraints.minWidth,  constraints.maxWidth);
            layout.boundingBox.height = std::clamp(maxCross + 2.0f * pad, constraints.minHeight, constraints.maxHeight);
        } else {
            layout.boundingBox.height = std::clamp(accumulatedMain, constraints.minHeight, constraints.maxHeight);
            layout.boundingBox.width  = std::clamp(maxCross + 2.0f * pad, constraints.minWidth,  constraints.maxWidth);
        }

        // --- Position pass (padding offset + gap between children) ---
        float currentPos = pad;
        for (NodeId childId : children) {
            auto& childLayout = m_layouts[childId];
            if (layout.direction == FlexDirection::Row) {
                childLayout.boundingBox.x = layout.boundingBox.x + currentPos;
                childLayout.boundingBox.y = layout.boundingBox.y + pad;
                currentPos += childLayout.boundingBox.width + gap;
            } else {
                childLayout.boundingBox.x = layout.boundingBox.x + pad;
                childLayout.boundingBox.y = layout.boundingBox.y + currentPos;
                currentPos += childLayout.boundingBox.height + gap;
            }
        }

        m_dirtyFlags[nodeId] = Clean;
    }

private:
    struct HierarchyComponent {
        NodeId parentId{InvalidNodeId};
        std::vector<NodeId> childrenIds;
        std::string name;
    };

    std::vector<LayoutComponent> m_layouts;
    std::vector<HierarchyComponent> m_hierarchy;
    std::vector<uint8_t> m_dirtyFlags;
};

} // namespace Genesis
