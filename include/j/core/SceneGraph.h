#pragma once

#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include <cstdio>
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

// Layout algorithm for a container's children:
//   Flex — single-axis flow (direction / justifyContent / alignItems). The default.
//   Grid — row-major grid `columns` wide; all columns share the inner width equally.
//   Form — two columns (label | field): column 0 auto-sizes to its widest child, column 1
//          takes the rest. Children pair up label,field,label,field… (the classic property form).
enum class JLayoutMode : uint8_t { Flex, Grid, Form };

/**
 * @brief How a child negotiates for space along ONE axis when the box/flex pass has leftover
 *        or deficit space to hand out.  Set per axis (see JSizePolicy) via setHSizePolicy /
 *        setVSizePolicy on a widget.  The default is Preferred with stretch 0, which reproduces
 *        the framework's original behaviour (children keep their size hint; only legacy flexGrow
 *        and the Expanding family claim leftover space).
 *
 *   Fixed             — locked to the size hint; never grows, never shrinks.
 *   Minimum           — the hint is a floor; may grow (only via flexGrow), never shrinks below it.
 *   Maximum           — the hint is a ceiling; may shrink toward min, never grows past the hint.
 *   Preferred         — the default; sits at its hint, may shrink under pressure, does not claim leftover.
 *   Expanding         — actively claims leftover space, split by stretch factor; may also shrink.
 *   MinimumExpanding  — claims leftover like Expanding, but the hint is also a hard floor.
 *   Ignored           — the hint carries no weight; grabs whatever space is going.
 */
enum class JSizePolicyMode : uint8_t {
    Fixed, Minimum, Maximum, Preferred, Expanding, MinimumExpanding, Ignored
};

/**
 * @brief A single axis' space-negotiation policy: a mode plus an integer stretch factor.
 *        Stretch weights how a child splits leftover space against its expanding siblings
 *        (an Expanding child with stretch 2 next to one with stretch 1 takes twice the slack).
 *        A stretch of 0 on an expanding child is treated as weight 1.
 */
struct JSizePolicy {
    JSizePolicyMode mode{JSizePolicyMode::Preferred};
    int             stretch{0};

    JSizePolicy() = default;
    JSizePolicy(JSizePolicyMode m, int s = 0) : mode(m), stretch(s) {}

    // Does this policy actively claim leftover main-axis space?
    bool expands() const {
        return mode == JSizePolicyMode::Expanding
            || mode == JSizePolicyMode::MinimumExpanding
            || mode == JSizePolicyMode::Ignored;
    }
    // May the child shrink below its size hint (down to its minimum)?
    bool canShrink() const {
        return mode == JSizePolicyMode::Maximum
            || mode == JSizePolicyMode::Preferred
            || mode == JSizePolicyMode::Expanding
            || mode == JSizePolicyMode::Ignored;
    }
    bool isDefault() const { return mode == JSizePolicyMode::Preferred && stretch == 0; }
    // Effective grow weight for the leftover-space split (expanding families only).
    float growWeight() const { return expands() ? float(stretch > 0 ? stretch : 1) : 0.0f; }
};

/**
 * @brief Where a child sits inside a cell that is larger than the child (grid columns, or a
 *        box child that opts out of filling).  Fill (the default) stretches the child to the
 *        cell; Start/Center/End place the child at its size hint against the named edge.
 */
enum class JCellAlign : uint8_t { Fill, Start, Center, End };

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
    JLayoutMode     mode{JLayoutMode::Flex};                     // Flex (default) / Grid / Form
    int            columns{2};                                  // Grid column count (>=1)
    // Per-child override of the parent's alignItems (-1 = inherit, else JAlignItems value).
    int            alignSelf{-1};
    float          flexGrow{0.0f};   // share of leftover main-axis space this node claims
    JEdges          padding{};        // inner inset (per-side; float = all sides)
    JEdges          margin{};         // outer spacing (per-side; float = all sides)
    float          gap{0.0f};        // space inserted between consecutive children
    float          minWidth{0.0f};
    float          minHeight{0.0f};
    float          maxWidth{100000.0f};   // upper clamp honoured by the space-distribution pass
    float          maxHeight{100000.0f};

    // Per-axis space-negotiation policy (default = Preferred/0 == original behaviour).
    JSizePolicy    hPolicy{};
    JSizePolicy    vPolicy{};
    // Placement inside an over-sized cell (grid column / opted-out box child).
    JCellAlign     cellAlign{JCellAlign::Fill};
    // True for a layout-only spacer node inserted by addStretch/addSpacing (no widget paints it).
    bool           isSpacer{false};
    // For a rigid (addSpacing) spacer: px reserved along the PARENT's main axis. Resolved to the
    // correct axis at measure time, since the parent's flow direction may be set after the spacer.
    float          spacing{-1.0f};   // <0 == not a rigid spacer

    // Resolve the policy for the container's MAIN axis given a flow direction.
    const JSizePolicy& mainPolicy(JFlexDirection d) const {
        return d == JFlexDirection::JRow ? hPolicy : vPolicy;
    }

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

    // Host window for popups anchored in THIS graph's coordinate space. A window that renders this graph
    // declares its live screen origin + native handle here, so the framework can open combo dropdowns /
    // popups relative to the correct window (not a hardcoded main window). Unset → the caller falls back.
    struct JHostWindow { int screenX = 0, screenY = 0; std::uintptr_t nativeHandle = 0; bool set = false; };
    void setHostWindow(int sx, int sy, std::uintptr_t handle) { m_host = { sx, sy, handle, true }; }
    const JHostWindow& hostWindow() const { return m_host; }

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

    // ------------------------------------------------------------------
    // Flexible layout items (spacers). These are widget-less nodes that exist purely to occupy
    // space in a flex/box container — the toolbox trick of pushing widgets apart or to an edge.
    // ------------------------------------------------------------------

    // Insert a stretchable gap into `parent`: an expanding spacer that soaks up leftover main-axis
    // space (split against other expanding items by `factor`). Put one before a widget to shove it
    // to the trailing edge, or between two to spread them apart. Returns the spacer's node id.
    NodeId addStretch(NodeId parent, int factor = 1) {
        NodeId s = createNode("<stretch>");
        auto& L = m_layouts[s];
        L.isSpacer = true;
        L.boundingBox.width = L.boundingBox.height = 0.0f;
        L.hPolicy = JSizePolicy{JSizePolicyMode::Expanding, factor};
        L.vPolicy = JSizePolicy{JSizePolicyMode::Expanding, factor};
        addChild(parent, s);
        return s;
    }

    // Insert a fixed-size gap of `px` along `parent`'s main axis (a rigid spacer). Returns its id.
    NodeId addSpacing(NodeId parent, float px) {
        NodeId s = createNode("<spacing>");
        auto& L = m_layouts[s];
        L.isSpacer = true;
        L.spacing  = px;                       // resolved to the parent's main axis at measure time
        L.hPolicy = JSizePolicy{JSizePolicyMode::Fixed, 0};
        L.vPolicy = JSizePolicy{JSizePolicyMode::Fixed, 0};
        L.boundingBox.width = L.boundingBox.height = 0.0f;
        addChild(parent, s);
        return s;
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
        if (id >= m_layouts.size()) { _reportStaleNode("getLayout", id); static JLayoutComponent scratch; return scratch = JLayoutComponent{}; }
        invalidateNode(id, DirtySelf);
        return m_layouts[id];
    }

    const JLayoutComponent& getLayoutConst(NodeId id) const {
        if (id >= m_layouts.size()) { _reportStaleNode("getLayoutConst", id); static const JLayoutComponent kDefault{}; return kDefault; }
        return m_layouts[id];
    }
    const std::vector<NodeId>& getChildren(NodeId id) const { return m_hierarchy[id].childrenIds; }
    NodeId getParent(NodeId id) const { return m_hierarchy[id].parentId; }
    size_t totalNodes() const { return m_layouts.size(); }

    // A layout accessor was handed a node id past the end of m_layouts — a stale/removed node (most often
    // a widget that outlived its node, surfaced by an every-frame tree walk). Report it loudly
    // (throttled) rather than overrunning the vector and aborting the whole app; callers get a default box.
    static void _reportStaleNode(const char* who, NodeId id) {
        static int warned = 0;
        if (warned++ < 20)
            JLOGC("SceneGraph", JLogLevel::Warn)
                << who << ": node id " << unsigned(id) << " out of range — stale/freed widget node; "
                << "returning a default layout instead of overrunning (warning " << warned << "/20)";
    }
    
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
        if (L.mode != JLayoutMode::Flex && !kids.empty()) { _measureGrid(nodeId, c); return; }
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
            // Rigid spacer: reserve its px along THIS container's main axis (0 on the cross axis).
            if (cl.isSpacer && cl.spacing >= 0.0f) {
                if (row) { cl.boundingBox.width = cl.minWidth = cl.maxWidth = cl.spacing; cl.boundingBox.height = 0.0f; }
                else     { cl.boundingBox.height = cl.minHeight = cl.maxHeight = cl.spacing; cl.boundingBox.width = 0.0f; }
            }
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

        // Distribute leftover / deficit main-axis space by size policy + stretch, then re-measure
        // any child container whose main-axis size actually changed.
        (void)totalGrow;
        float innerMain = selfMain - pad.mainSum(d);
        float freeMain  = innerMain - usedMain;

        // Main-axis min / max clamps for a child, resolved against the flow direction.
        auto mainMaxOf = [&](const JLayoutComponent& cl){ return row ? cl.maxWidth : cl.maxHeight; };
        auto mainMinOf = [&](const JLayoutComponent& cl){ return row ? cl.minWidth : cl.minHeight; };
        auto setMain   = [&](JLayoutComponent& cl, float v){ if (row) cl.boundingBox.width = v; else cl.boundingBox.height = v; };

        bool policiesEngaged = false;
        for (NodeId k : kids) if (!m_layouts[k].mainPolicy(d).isDefault()) { policiesEngaged = true; break; }

        // --- GROW: hand leftover space to expanding children (+ legacy flexGrow), water-filling so a
        //     child that hits its maximum passes the surplus on to the rest. Respects each child's max. ---
        if (freeMain > 0.5f) {
            std::vector<NodeId> growable;
            for (NodeId k : kids) {
                auto& cl = m_layouts[k];
                float w = cl.flexGrow + cl.mainPolicy(d).growWeight();
                if (w > 0.0f && mainOf(cl.boundingBox) < mainMaxOf(cl) - 0.5f) growable.push_back(k);
            }
            float pool = freeMain;
            // Iterate: each round splits the pool by weight; children that cap are frozen and their
            // leftover recirculates. Bounded by child count so a pathological case can't spin.
            for (size_t guard = 0; guard <= growable.size() && pool > 0.5f && !growable.empty(); ++guard) {
                float totW = 0.0f;
                for (NodeId k : growable) { auto& cl = m_layouts[k]; totW += cl.flexGrow + cl.mainPolicy(d).growWeight(); }
                if (totW <= 0.0f) break;
                float distributed = 0.0f;
                std::vector<NodeId> still;
                for (NodeId k : growable) {
                    auto& cl = m_layouts[k];
                    float w = cl.flexGrow + cl.mainPolicy(d).growWeight();
                    float want = mainOf(cl.boundingBox) + pool * (w / totW);
                    float capped = std::min(want, mainMaxOf(cl));
                    distributed += capped - mainOf(cl.boundingBox);
                    setMain(cl, capped);
                    if (capped < mainMaxOf(cl) - 0.5f) still.push_back(k);
                }
                pool -= distributed;
                if (still.size() == growable.size() && distributed < 0.5f) break;   // fully settled
                growable.swap(still);
            }
        }
        // --- SHRINK: only when policies are engaged (preserves the historical overflow behaviour for
        //     callers that never set a policy). Pull space back from shrinkable children down to min. ---
        else if (freeMain < -0.5f && policiesEngaged) {
            std::vector<NodeId> shrinkable;
            for (NodeId k : kids) {
                auto& cl = m_layouts[k];
                if (cl.mainPolicy(d).canShrink() && mainOf(cl.boundingBox) > mainMinOf(cl) + 0.5f)
                    shrinkable.push_back(k);
            }
            float deficit = -freeMain;
            for (size_t guard = 0; guard <= shrinkable.size() && deficit > 0.5f && !shrinkable.empty(); ++guard) {
                float totHead = 0.0f;   // room available above each child's minimum
                for (NodeId k : shrinkable) { auto& cl = m_layouts[k]; totHead += mainOf(cl.boundingBox) - mainMinOf(cl); }
                if (totHead <= 0.0f) break;
                float removed = 0.0f;
                std::vector<NodeId> still;
                for (NodeId k : shrinkable) {
                    auto& cl = m_layouts[k];
                    float head = mainOf(cl.boundingBox) - mainMinOf(cl);
                    float take = std::min(head, deficit * (head / totHead));
                    setMain(cl, mainOf(cl.boundingBox) - take);
                    removed += take;
                    if (mainOf(cl.boundingBox) > mainMinOf(cl) + 0.5f) still.push_back(k);
                }
                deficit -= removed;
                if (removed < 0.5f) break;
                shrinkable.swap(still);
            }
        }

        // Re-measure any child container whose main-axis extent changed above.
        for (NodeId k : kids) {
            if (m_hierarchy[k].childrenIds.empty()) continue;
            auto& cl = m_layouts[k];
            float w = cl.boundingBox.width, h = cl.boundingBox.height;
            _measure(k, JConstraints{w, w, h, h});
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
        if (L.mode != JLayoutMode::Flex) { _arrangeGrid(nodeId, x, y); return; }

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
        if (L.mode != JLayoutMode::Flex) { _computeMinSizeGrid(nodeId); return; }

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
            // A rigid spacer contributes its reserved px along this container's main axis.
            if (cl.isSpacer && cl.spacing >= 0.0f) { if (row) cl.minWidth = cl.spacing; else cl.minHeight = cl.spacing; }

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

    // --- Grid / Form layout (mode != Flex). Row-major cells; children stretch to column width,
    //     keep their measured height; each row is as tall as its tallest child. ------------------
    void _measureGrid(NodeId nodeId, const JConstraints& c) {
        auto& L = m_layouts[nodeId];
        const auto& kids = m_hierarchy[nodeId].childrenIds;
        const JEdges& pad = L.padding;
        const float gap = L.gap;
        const int n = static_cast<int>(kids.size());
        const bool form = (L.mode == JLayoutMode::Form);
        const int cols = form ? 2 : std::max(1, L.columns);
        const int rows = (n + cols - 1) / cols;
        const float innerMaxW = std::max(0.0f, c.maxWidth - pad.horizontal());

        std::vector<float> colW(cols, 0.0f);
        if (form) {
            // Column 0 = widest label (even indices), capped so the field column keeps room.
            float lab = 0.0f;
            for (int i = 0; i < n; i += 2) {
                _measure(kids[i], JConstraints{0.0f, innerMaxW, 0.0f, c.maxHeight});
                lab = std::max(lab, m_layouts[kids[i]].boundingBox.width);
            }
            lab = std::min(lab, innerMaxW * 0.6f);
            colW[0] = lab;
            colW[1] = std::max(0.0f, innerMaxW - lab - gap);
        } else {
            float w = (innerMaxW - gap * (cols - 1)) / static_cast<float>(cols);
            for (auto& x : colW) x = std::max(0.0f, w);
        }

        std::vector<float> rowH(std::max(1, rows), 0.0f);
        for (int i = 0; i < n; ++i) {
            const int col = i % cols, row = i / cols;
            auto& cl = m_layouts[kids[i]];
            // Fill (default) stretches the child to the whole column; any other cellAlign lets the
            // child keep its own width (capped to the column) so it can sit left/centre/right in the cell.
            if (cl.cellAlign == JCellAlign::Fill) {
                _measure(kids[i], JConstraints{colW[col], colW[col], 0.0f, c.maxHeight});
                cl.boundingBox.width = colW[col];                   // stretch to the column
            } else {
                _measure(kids[i], JConstraints{0.0f, colW[col], 0.0f, c.maxHeight});
                cl.boundingBox.width = std::min(cl.boundingBox.width, colW[col]);
            }
            rowH[row] = std::max(rowH[row], cl.boundingBox.height + cl.margin.vertical());
        }

        float contentH = pad.vertical();
        for (int r = 0; r < rows; ++r) contentH += rowH[r];
        if (rows > 1) contentH += gap * (rows - 1);

        L.boundingBox.width  = clampF(innerMaxW + pad.horizontal(), c.minWidth,  c.maxWidth);
        L.boundingBox.height = clampF(contentH,                     c.minHeight, c.maxHeight);
    }

    void _arrangeGrid(NodeId nodeId, float x, float y) {
        auto& L = m_layouts[nodeId];
        L.boundingBox.x = x;
        L.boundingBox.y = y;
        const auto& kids = m_hierarchy[nodeId].childrenIds;
        const JEdges& pad = L.padding;
        const float gap = L.gap;
        const int n = static_cast<int>(kids.size());
        const bool form = (L.mode == JLayoutMode::Form);
        const int cols = form ? 2 : std::max(1, L.columns);
        const int rows = (n + cols - 1) / cols;

        // Column widths + row heights read back from the measured children.
        std::vector<float> colW(cols, 0.0f);
        std::vector<float> rowH(std::max(1, rows), 0.0f);
        for (int i = 0; i < n; ++i) {
            auto& cl = m_layouts[kids[i]];
            colW[i % cols] = std::max(colW[i % cols], cl.boundingBox.width);
            rowH[i / cols] = std::max(rowH[i / cols], cl.boundingBox.height + cl.margin.vertical());
        }

        float cy = y + pad.top;
        for (int r = 0; r < rows; ++r) {
            float cx = x + pad.left;
            for (int col = 0; col < cols; ++col) {
                const int i = r * cols + col;
                if (i >= n) break;
                auto& cl = m_layouts[kids[i]];
                // Place a narrower-than-column child by its cell alignment (Fill children already
                // span the column, so their offset is 0 either way).
                float slack = std::max(0.0f, colW[col] - cl.margin.horizontal() - cl.boundingBox.width);
                float ax = 0.0f;
                if      (cl.cellAlign == JCellAlign::Center) ax = slack * 0.5f;
                else if (cl.cellAlign == JCellAlign::End)    ax = slack;
                _arrange(kids[i], cx + cl.margin.left + ax, cy + cl.margin.top);
                cx += colW[col] + gap;
            }
            cy += rowH[r] + gap;
        }
    }

    void _computeMinSizeGrid(NodeId nodeId) {
        auto& L = m_layouts[nodeId];
        const auto& kids = m_hierarchy[nodeId].childrenIds;
        const JEdges& pad = L.padding;
        const float gap = L.gap;
        const int n = static_cast<int>(kids.size());
        const int cols = (L.mode == JLayoutMode::Form) ? 2 : std::max(1, L.columns);
        const int rows = (n + cols - 1) / cols;

        std::vector<float> rowMinH(std::max(1, rows), 0.0f);
        for (int i = 0; i < n; ++i) {
            _computeMinSize(kids[i]);
            auto& cl = m_layouts[kids[i]];
            rowMinH[i / cols] = std::max(rowMinH[i / cols], cl.minHeight + cl.margin.vertical());
        }
        float minH = pad.vertical();
        for (int r = 0; r < rows; ++r) minH += rowMinH[r];
        if (rows > 1) minH += gap * (rows - 1);
        L.minHeight = std::max(L.minHeight, minH);
        L.minWidth  = std::max(L.minWidth, pad.horizontal());   // width can shrink; host sets it
    }

private:
    struct JHierarchyComponent {
        NodeId parentId{InvalidNodeId};
        std::vector<NodeId> childrenIds;
        std::string name;
    };

    JHostWindow                   m_host{};            // host window for popups anchored in this graph's space
    JLayoutComponent              m_defaultLayout{};   // global defaults for new nodes
    std::vector<JLayoutComponent> m_layouts;
    std::vector<JHierarchyComponent> m_hierarchy;
    std::vector<uint8_t> m_dirtyFlags;
};

} // inline namespace jf
