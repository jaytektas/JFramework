#pragma once

#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include <iostream>
#include <algorithm>

#include <j/core/muted_logging_mock.h>
namespace { inline constexpr auto& LogLayoutEngine = jf::Log::Layout; }

inline namespace jf {

using NodeId = uint32_t;
constexpr NodeId InvalidNodeId = 0xFFFFFFFF;

enum class JFlexDirection { JRow, Column };
enum class JJustifyContent { FlexStart, Center, FlexEnd, SpaceBetween, SpaceAround };
// Cross-axis alignment of children within a container.
enum class JAlignItems { Start, Center, End, Stretch };

/**
 * @brief Per-side edge values (padding / margin).  Implicitly constructs/assigns from a
 *        single float so `layout.padding = 12.0f` still means "12 on all sides".
 */
struct JEdges {
    float left{0.0f}, top{0.0f}, right{0.0f}, bottom{0.0f};
    JEdges() = default;
    JEdges(float all) : left(all), top(all), right(all), bottom(all) {}
    JEdges(float h, float v) : left(h), top(v), right(h), bottom(v) {}
    JEdges(float l, float t, float r, float b) : left(l), top(t), right(r), bottom(b) {}
    JEdges& operator=(float all) { left = top = right = bottom = all; return *this; }
    float horizontal() const { return left + right; }
    float vertical()   const { return top + bottom; }
    // Main / cross axis helpers, resolved against a flex direction.
    float mainLeading (JFlexDirection d) const { return d == JFlexDirection::JRow ? left : top; }
    float mainTrailing(JFlexDirection d) const { return d == JFlexDirection::JRow ? right : bottom; }
    float mainSum     (JFlexDirection d) const { return d == JFlexDirection::JRow ? horizontal() : vertical(); }
    float crossLeading(JFlexDirection d) const { return d == JFlexDirection::JRow ? top : left; }
    float crossSum    (JFlexDirection d) const { return d == JFlexDirection::JRow ? vertical() : horizontal(); }
};

/**
 * @brief Bitmask flags for the layout invalidation vector.
 */
enum JLayoutFlag : uint8_t {
    Clean = 0,
    DirtySelf = 1 << 0,     // Properties of this node changed
    DirtyChildren = 1 << 1  // A child is dirty, requiring parent re-measurement/re-positioning
};

/**
 * @brief Geometric primitives decoupled from layout logic to preserve raw cache locality.
 */
struct JRect {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
};

struct JConstraints {
    float minWidth{0.0f};
    float maxWidth{100000.0f};
    float minHeight{0.0f};
    float maxHeight{100000.0f};
};

/**
 * @brief Pure Layout Node Properties. 
 */
struct JLayoutComponent {
    JFlexDirection  direction{JFlexDirection::Column};
    JJustifyContent justifyContent{JJustifyContent::FlexStart};  // main-axis distribution
    JAlignItems     alignItems{JAlignItems::Start};              // cross-axis for children
    // Per-child override of the parent's alignItems (-1 = inherit, else JAlignItems value).
    int            alignSelf{-1};
    float          flexGrow{0.0f};   // share of leftover main-axis space this node claims
    JEdges          padding{};        // inner inset (per-side; float = all sides)
    JEdges          margin{};         // outer spacing (per-side; float = all sides)
    float          gap{0.0f};        // space inserted between consecutive children
    float          minWidth{0.0f};
    float          minHeight{0.0f};

    // Calculated output geometry bounds
    JRect boundingBox;
};

/**
 * @brief Flat, array-backed Scene Graph with Invalidation Vector.
 */
class JSceneGraph {
public:
    JSceneGraph() = default;
    ~JSceneGraph() = default;

    JSceneGraph(const JSceneGraph&) = delete;
    JSceneGraph& operator=(const JSceneGraph&) = delete;

    /**
     * @brief Instantiates a node flatly inside the memory arena.
     */
    // Global layout defaults applied to every new node — set these once at startup for
    // app-wide spacing/alignment, then override any individual node locally via getLayout().
    JLayoutComponent& defaultLayout() { return m_defaultLayout; }

    NodeId createNode(const std::string& debugName = "") {
        NodeId id = static_cast<NodeId>(m_layouts.size());
        m_layouts.emplace_back(m_defaultLayout);   // inherit global defaults; override locally
        m_hierarchy.emplace_back(JHierarchyComponent{InvalidNodeId, {}, debugName});
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
    void invalidateNode(NodeId id, JLayoutFlag flag = DirtySelf) {
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

    JLayoutComponent& getLayout(NodeId id) { 
        invalidateNode(id, DirtySelf); 
        return m_layouts[id]; 
    }

    const JLayoutComponent& getLayoutConst(NodeId id) const { return m_layouts[id]; }
    const std::vector<NodeId>& getChildren(NodeId id) const { return m_hierarchy[id].childrenIds; }
    NodeId getParent(NodeId id) const { return m_hierarchy[id].parentId; }
    size_t totalNodes() const { return m_layouts.size(); }
    
    bool isDirty(NodeId id) const { return m_dirtyFlags[id] != Clean; }

    /**
     * @brief Linear Single-Pass Constraint Layout Solver with Invalidation skipping.
     * Respects padding (inset on all sides) and gap (space between children).
     */
    void computeMinSize(NodeId nodeId) {
        if (nodeId >= m_layouts.size()) return;
        _computeMinSize(nodeId);
    }

    void computeLayout(NodeId nodeId, const JConstraints& constraints) {
        if (nodeId >= m_layouts.size()) return;
        if (m_dirtyFlags[nodeId] == Clean) return;
        
        _computeMinSize(nodeId);

        // Two phases keep nested layouts correct: size the whole subtree bottom-up, then
        // position it top-down from this node's current origin.
        _measure(nodeId, constraints);
        auto& bb = m_layouts[nodeId].boundingBox;
        _arrange(nodeId, bb.x, bb.y);
        _markCleanRec(nodeId);
    }

    // clamp that never asserts: if an upstream constraint inverts (max < min), the min
    // bound wins.  Keeps a degenerate panel size from aborting the whole app.
    static float clampF(float v, float lo, float hi) {
        if (hi < lo) hi = lo;
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // --- Phase 1: size the subtree (bottom-up). Applies flexGrow + cross-axis stretch. ---
    void _measure(NodeId nodeId, const JConstraints& c) {
        auto& L = m_layouts[nodeId];
        const auto& kids = m_hierarchy[nodeId].childrenIds;
        const JFlexDirection d = L.direction;
        const bool row = (d == JFlexDirection::JRow);
        auto mainOf  = [&](const JRect& r){ return row ? r.width  : r.height; };
        auto crossOf = [&](const JRect& r){ return row ? r.height : r.width;  };

        if (kids.empty()) {
            L.boundingBox.width  = clampF(L.boundingBox.width,  std::max(c.minWidth, L.minWidth),  c.maxWidth);
            L.boundingBox.height = clampF(L.boundingBox.height, std::max(c.minHeight, L.minHeight), c.maxHeight);
            return;
        }

        const JEdges& pad = L.padding;
        const float  gap = L.gap;
        const size_t n   = kids.size();

        float usedMain = 0.0f, maxCross = 0.0f, totalGrow = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            auto& cl = m_layouts[kids[i]];
            float availW = std::max(0.0f, c.maxWidth  - pad.horizontal() - cl.margin.horizontal());
            float availH = std::max(0.0f, c.maxHeight - pad.vertical()   - cl.margin.vertical());
            // A child wants its intrinsic size but must fit the available space — clamp the
            // min bound to avail so an oversized control shrinks instead of asserting.
            float childMinW = std::max(cl.minWidth, std::min(cl.boundingBox.width, availW));
            float childMinH = std::max(cl.minHeight, std::min(cl.boundingBox.height, availH));
            JConstraints cc{
                childMinW, availW,
                childMinH, availH
            };
            _measure(kids[i], cc);
            usedMain += mainOf(cl.boundingBox) + cl.margin.mainSum(d);
            maxCross  = std::max(maxCross, crossOf(cl.boundingBox) + cl.margin.crossSum(d));
            totalGrow += cl.flexGrow;
            if (i + 1 < n) usedMain += gap;
        }

        float selfMain  = clampF(usedMain + pad.mainSum(d),
                                     row ? c.minWidth  : c.minHeight, row ? c.maxWidth  : c.maxHeight);
        float selfCross = clampF(maxCross + pad.crossSum(d),
                                     row ? c.minHeight : c.minWidth,  row ? c.maxHeight : c.maxWidth);
        if (row) { L.boundingBox.width = selfMain; L.boundingBox.height = selfCross; }
        else     { L.boundingBox.height = selfMain; L.boundingBox.width = selfCross; }

        // flexGrow — distribute leftover main-axis space; re-measure grown containers.
        float innerMain = selfMain - pad.mainSum(d);
        float freeMain  = innerMain - usedMain;
        if (freeMain > 0.5f && totalGrow > 0.0f) {
            for (NodeId k : kids) {
                auto& cl = m_layouts[k];
                if (cl.flexGrow <= 0.0f) continue;
                float nm = mainOf(cl.boundingBox) + freeMain * (cl.flexGrow / totalGrow);
                if (row) cl.boundingBox.width = nm; else cl.boundingBox.height = nm;
                if (!m_hierarchy[k].childrenIds.empty()) {
                    float w = cl.boundingBox.width, h = cl.boundingBox.height;
                    _measure(k, JConstraints{w, w, h, h});
                }
            }
        }

        // cross-axis Stretch — fill the container's inner cross extent.
        float innerCross = selfCross - pad.crossSum(d);
        for (NodeId k : kids) {
            auto& cl = m_layouts[k];
            JAlignItems a = cl.alignSelf >= 0 ? static_cast<JAlignItems>(cl.alignSelf) : L.alignItems;
            if (a == JAlignItems::Stretch) {
                float t = std::max(0.0f, innerCross - cl.margin.crossSum(d));
                if (row) cl.boundingBox.height = t; else cl.boundingBox.width = t;
                if (!m_hierarchy[k].childrenIds.empty()) {
                    float w = cl.boundingBox.width, h = cl.boundingBox.height;
                    _measure(k, JConstraints{w, w, h, h});
                }
            }
        }
    }

    // --- Phase 2: position the subtree (top-down). Applies justifyContent + align + margins. ---
    void _arrange(NodeId nodeId, float x, float y) {
        auto& L = m_layouts[nodeId];
        L.boundingBox.x = x;
        L.boundingBox.y = y;
        const auto& kids = m_hierarchy[nodeId].childrenIds;
        if (kids.empty()) return;

        const JFlexDirection d = L.direction;
        const bool row = (d == JFlexDirection::JRow);
        const JEdges& pad = L.padding;
        const float  gap = L.gap;
        const size_t n   = kids.size();
        auto mainOf  = [&](const JRect& r){ return row ? r.width  : r.height; };
        auto crossOf = [&](const JRect& r){ return row ? r.height : r.width;  };

        float innerMain  = (row ? L.boundingBox.width  : L.boundingBox.height) - pad.mainSum(d);
        float innerCross = (row ? L.boundingBox.height : L.boundingBox.width)  - pad.crossSum(d);

        float used = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            auto& cl = m_layouts[kids[i]];
            used += mainOf(cl.boundingBox) + cl.margin.mainSum(d);
            if (i + 1 < n) used += gap;
        }
        float remaining = innerMain - used;
        float startOff  = pad.mainLeading(d);
        float between   = gap;
        if (remaining > 0.5f) {
            switch (L.justifyContent) {
                case JJustifyContent::FlexStart:    break;
                case JJustifyContent::Center:       startOff += remaining * 0.5f; break;
                case JJustifyContent::FlexEnd:      startOff += remaining; break;
                case JJustifyContent::SpaceBetween: if (n > 1) between += remaining / (n - 1); break;
                case JJustifyContent::SpaceAround:  { float s = remaining / n; startOff += s * 0.5f; between += s; } break;
            }
        }

        float originMain  = row ? L.boundingBox.x : L.boundingBox.y;
        float originCross = row ? L.boundingBox.y : L.boundingBox.x;
        float cursor = originMain + startOff;
        for (size_t i = 0; i < n; ++i) {
            NodeId k = kids[i];
            auto& cl = m_layouts[k];
            JAlignItems a = cl.alignSelf >= 0 ? static_cast<JAlignItems>(cl.alignSelf) : L.alignItems;
            float crossPos  = originCross + pad.crossLeading(d) + cl.margin.crossLeading(d);
            float crossFree = innerCross - cl.margin.crossSum(d) - crossOf(cl.boundingBox);
            if      (a == JAlignItems::Center) crossPos += std::max(0.0f, crossFree) * 0.5f;
            else if (a == JAlignItems::End)    crossPos += std::max(0.0f, crossFree);

            cursor += cl.margin.mainLeading(d);
            _arrange(k, row ? cursor : crossPos, row ? crossPos : cursor);
            cursor += mainOf(cl.boundingBox) + cl.margin.mainTrailing(d) + between;
        }
    }

    void _computeMinSize(NodeId nodeId) {
        auto& L = m_layouts[nodeId];
        const auto& kids = m_hierarchy[nodeId].childrenIds;
        if (kids.empty()) {
            return;
        }

        const JFlexDirection d = L.direction;
        const bool row = (d == JFlexDirection::JRow);
        const JEdges& pad = L.padding;
        const float  gap = L.gap;
        const size_t n   = kids.size();

        float kidsMinMain = 0.f;
        float kidsMinCross = 0.f;
        for (size_t i = 0; i < n; ++i) {
            NodeId kid = kids[i];
            _computeMinSize(kid);
            auto& cl = m_layouts[kid];

            float clMinMain = row ? cl.minWidth : cl.minHeight;
            float clMinCross = row ? cl.minHeight : cl.minWidth;

            kidsMinMain += clMinMain + (row ? cl.margin.horizontal() : cl.margin.vertical());
            kidsMinCross = std::max(kidsMinCross, clMinCross + (row ? cl.margin.vertical() : cl.margin.horizontal()));
            if (i + 1 < n) kidsMinMain += gap;
        }

        float totalMinMain = kidsMinMain + pad.mainSum(d);
        float totalMinCross = kidsMinCross + pad.crossSum(d);

        float computedMinW = row ? totalMinMain : totalMinCross;
        float computedMinH = row ? totalMinCross : totalMinMain;

        L.minWidth = std::max(L.minWidth, computedMinW);
        L.minHeight = std::max(L.minHeight, computedMinH);
    }

    void _markCleanRec(NodeId nodeId) {
        m_dirtyFlags[nodeId] = Clean;
        for (NodeId k : m_hierarchy[nodeId].childrenIds) _markCleanRec(k);
    }

private:
    struct JHierarchyComponent {
        NodeId parentId{InvalidNodeId};
        std::vector<NodeId> childrenIds;
        std::string name;
    };

    JLayoutComponent              m_defaultLayout{};   // global defaults for new nodes
    std::vector<JLayoutComponent> m_layouts;
    std::vector<JHierarchyComponent> m_hierarchy;
    std::vector<uint8_t> m_dirtyFlags;
};

} // inline namespace jf
