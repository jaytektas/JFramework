#pragma once

// ============================================================================
// DockManager — the dock zone management system for Genesis.
//
// The dock layout is a TREE of DockNodes stored in a flat arena inside
// JDockHost.  Every node is either a SplitNode (divides space between N
// children along H or V with per-child weights) or a TabLeaf (holds 1..N
// JDockWidget pointers as a tab group).
//
// Named zones (Left/Right/Top/Bottom/Center) are just labels on top-level
// SplitNode children — there is no special "zone" type; the tree nests
// arbitrarily deep.
//
// Header-only, like the rest of the toolkit core.  No exceptions, no per-frame
// heap allocation in the hot paths (layout/render reuse the arena's vectors).
// ============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>
#include <algorithm>
#include <cmath>

#include "BaseWidgets.h"   // Colors, Rect, PrimitiveBuffer, TextHelper
#include "DockWidget.h"    // DockWidget
#include "DockRegistry.h"
#include "Style.h"         // JStyle/style() cascade — JTabBarEdge, JTabFill

inline namespace jf {

// ----------------------------------------------------------------------------
// Public enums / value types
// ----------------------------------------------------------------------------

enum class JSplitDir : uint8_t { Horizontal, Vertical };

// JTabBarEdge / JTabFill now live in Style.h (shared with the global stylesheet).

// Drop placement relative to a node.
enum class JDropPos : uint8_t {
    Center,                       // tabify with the target leaf
    Left, Right, Top, Bottom,     // split the target, new dock on that side
    SplitBefore, SplitAfter       // insert before/after in parent split
};

struct JDockNodeId {
    uint32_t v{UINT32_MAX};
    bool valid() const { return v != UINT32_MAX; }
    bool operator==(const JDockNodeId& o) const { return v == o.v; }
    bool operator!=(const JDockNodeId& o) const { return v != o.v; }
};
static constexpr JDockNodeId InvalidDockNodeId{};

struct JDockConstraints {
    float minW{48.f}, minH{48.f};
    float maxW{1e9f}, maxH{1e9f};
    float preferredW{0.f}, preferredH{0.f};  // 0 = no preference
    bool  allowTabDrop{true};                // false = docks can split AROUND this leaf
                                             // (edge drops) but never tabify INTO it
                                             // — e.g. a non-dockable centre/editor area.
};

struct JDockAffinityRule {
    std::vector<std::string> accept;  // empty = accept all tags
    std::vector<std::string> reject;  // these tags always blocked
    bool accepts(std::string_view tag) const {
        for (auto& r : reject) if (r == tag) return false;
        if (accept.empty()) return true;
        for (auto& a : accept) if (a == tag) return true;
        return false;
    }
};

// ----------------------------------------------------------------------------
// JDockNode — a node in the tree, stored in JDockHost's flat arena.
// ----------------------------------------------------------------------------

struct JDockNode {
    enum class JType : uint8_t { Split, Leaf } type{JType::Split};
    JDockNodeId id;
    JDockNodeId parent;

    // Split fields (type == Split)
    JSplitDir                 splitDir{JSplitDir::Horizontal};
    std::vector<JDockNodeId>  children;
    std::vector<float>       weights;   // one per child, sum == 1.0

    // Leaf fields (type == Leaf)
    std::vector<JDockWidget*> tabs;      // non-owning
    int                      activeTab{0};
    JTabBarEdge               tabBarEdge{JTabBarEdge::Top};
    JDockAffinityRule         affinity;
    JDockConstraints          constraints;
    std::string              label;     // optional zone name

    // Runtime — computed each frame by JDockHost::computeLayout
    JRect                     rect{};

    // Runtime drag handle rects (SplitNodes: one per gap between children)
    std::vector<JRect>        handleRects;  // count == children.size() - 1
};

// ============================================================================
// JDockLayoutNode — one node in a serializable snapshot of the dock tree.
// ============================================================================

struct JDockLayoutNode {
    enum class JKind : uint8_t { Split, Leaf } kind{JKind::Leaf};

    // Split
    JSplitDir                    splitDir{JSplitDir::Horizontal};
    std::vector<float>          weights;
    std::vector<JDockLayoutNode> children;

    // Leaf
    std::string              label;
    JTabBarEdge               tabBarEdge{JTabBarEdge::Top};
    int                      activeTab{0};
    std::vector<std::string> tabTitles;
};

// ============================================================================
// dock_detail — serialization helpers (not part of the public API)
// ============================================================================

namespace dock_detail {

inline std::string quoteStr(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        if      (c == '\\') out += "\\\\";
        else if (c == '"')  out += "\\\"";
        else                out += c;
    }
    out += '"';
    return out;
}

// Returns chars consumed (including surrounding quotes), or 0 on error.
inline size_t unquoteStr(const char* p, size_t len, std::string& out) {
    if (len < 2 || *p != '"') return 0;
    out.clear();
    size_t i = 1;
    while (i < len) {
        if (p[i] == '"') { return i + 1; }
        if (p[i] == '\\' && i + 1 < len) { ++i; out += p[i]; }
        else                              { out += p[i]; }
        ++i;
    }
    return 0; // unterminated
}

inline const char* edgeStr(JTabBarEdge e) noexcept {
    switch (e) {
        case JTabBarEdge::Top:    return "top";
        case JTabBarEdge::Bottom: return "bottom";
        case JTabBarEdge::Left:   return "left";
        case JTabBarEdge::Right:  return "right";
    }
    return "top";
}

inline JTabBarEdge parseEdge(const std::string& s) noexcept {
    if (s == "bottom") return JTabBarEdge::Bottom;
    if (s == "left")   return JTabBarEdge::Left;
    if (s == "right")  return JTabBarEdge::Right;
    return JTabBarEdge::Top;
}

// 6 decimal-place float without <cstdio>.
inline std::string fmtWeight(float w) {
    if (w < 0.f) w = 0.f;
    if (w > 1.f) w = 1.f;
    int whole = static_cast<int>(w);
    int frac  = static_cast<int>((w - static_cast<float>(whole)) * 1000000.f + 0.5f);
    if (frac >= 1000000) { ++whole; frac = 0; }
    std::string fs = std::to_string(frac);
    while (fs.size() < 6) fs = "0" + fs;
    return std::to_string(whole) + "." + fs;
}

inline void serializeNode(const JDockLayoutNode& n, std::string& out) {
    if (n.kind == JDockLayoutNode::JKind::Split) {
        out += "split ";
        out += (n.splitDir == JSplitDir::Horizontal ? 'H' : 'V');
        out += ' ';
        out += std::to_string(n.children.size());
        for (float w : n.weights) { out += ' '; out += fmtWeight(w); }
        out += '\n';
        for (const auto& child : n.children) serializeNode(child, out);
    } else {
        out += "leaf ";
        out += quoteStr(n.label);
        out += ' ';
        out += edgeStr(n.tabBarEdge);
        out += ' ';
        out += std::to_string(n.activeTab);
        out += ' ';
        out += std::to_string(n.tabTitles.size());
        out += '\n';
        for (const auto& t : n.tabTitles) {
            out += "tab "; out += quoteStr(t); out += '\n';
        }
    }
}

struct JTok {
    const char* begin{nullptr};
    size_t      len{0};
    std::string str() const { return {begin, len}; }
};
using Lines = std::vector<std::pair<const char*, size_t>>;

inline std::vector<JTok> tokenizeLine(const char* p, size_t len) {
    std::vector<JTok> toks;
    size_t i = 0;
    while (i < len) {
        while (i < len && (p[i] == ' ' || p[i] == '\r')) ++i;
        if (i >= len) break;
        if (p[i] == '"') {
            size_t start = i++;
            while (i < len) {
                if (p[i] == '\\' && i + 1 < len) { i += 2; continue; }
                if (p[i] == '"') { ++i; break; }
                ++i;
            }
            toks.push_back({p + start, i - start});
        } else {
            size_t start = i;
            while (i < len && p[i] != ' ') ++i;
            toks.push_back({p + start, i - start});
        }
    }
    return toks;
}

inline bool parseNode(const Lines& lines, size_t& idx, JDockLayoutNode& out) {
    if (idx >= lines.size()) return false;
    auto toks = tokenizeLine(lines[idx].first, lines[idx].second);
    ++idx;
    if (toks.empty()) return false;

    const std::string kw = toks[0].str();

    if (kw == "split") {
        if (toks.size() < 3) return false;
        out.kind     = JDockLayoutNode::JKind::Split;
        out.splitDir = (toks[1].str() == "H") ? JSplitDir::Horizontal : JSplitDir::Vertical;
        size_t n = 0;
        try { n = std::stoul(toks[2].str()); } catch (...) { return false; }
        out.weights.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            float w = 0.f;
            if (3 + i < toks.size())
                try { w = std::stof(toks[3 + i].str()); } catch (...) {}
            out.weights.push_back(w);
        }
        out.children.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            JDockLayoutNode child;
            if (!parseNode(lines, idx, child)) return false;
            out.children.push_back(std::move(child));
        }
    } else if (kw == "leaf") {
        if (toks.size() < 5) return false;
        out.kind = JDockLayoutNode::JKind::Leaf;
        unquoteStr(toks[1].begin, toks[1].len, out.label);
        out.tabBarEdge = parseEdge(toks[2].str());
        try { out.activeTab = std::stoi(toks[3].str()); } catch (...) { out.activeTab = 0; }
        size_t ntabs = 0;
        try { ntabs = std::stoul(toks[4].str()); } catch (...) {}
        out.tabTitles.reserve(ntabs);
        for (size_t i = 0; i < ntabs; ++i) {
            if (idx >= lines.size()) return false;
            auto tt = tokenizeLine(lines[idx].first, lines[idx].second);
            ++idx;
            std::string title;
            if (tt.size() >= 2) unquoteStr(tt[1].begin, tt[1].len, title);
            out.tabTitles.push_back(std::move(title));
        }
    } else {
        return false;
    }
    return true;
}

} // namespace dock_detail

// ============================================================================
// JDockLayoutSnapshot — complete serializable snapshot of a JDockHost tree.
//
//   JDockLayoutSnapshot snap = host.snapshot();
//   std::string text = snap.toText();                  // save to file
//
//   auto snap2 = JDockLayoutSnapshot::fromText(text);
//   if (snap2) host.restore(*snap2, [&](std::string_view title) -> JDockWidget* { ... });
// ============================================================================

struct JDockLayoutSnapshot {
    JDockLayoutNode root;

    std::string toText() const {
        std::string out = "genesis_dock_layout 1\n";
        dock_detail::serializeNode(root, out);
        return out;
    }

    static std::optional<JDockLayoutSnapshot> fromText(std::string_view text) {
        dock_detail::Lines lines;
        const char* p = text.data();
        size_t      n = text.size();
        for (size_t i = 0, s = 0; i <= n; ++i) {
            if (i == n || p[i] == '\n') {
                if (i > s) lines.push_back({p + s, i - s});
                s = i + 1;
            }
        }
        size_t idx = 0;
        if (idx >= lines.size()) return std::nullopt;
        // Validate header (strip trailing \r for Windows line endings).
        std::string hdr(lines[idx].first, lines[idx].second);
        if (!hdr.empty() && hdr.back() == '\r') hdr.pop_back();
        if (hdr != "genesis_dock_layout 1") return std::nullopt;
        ++idx;

        JDockLayoutSnapshot snap;
        if (!dock_detail::parseNode(lines, idx, snap.root)) return std::nullopt;
        return snap;
    }
};

// ----------------------------------------------------------------------------
// JDockHost — the façade owning the tree.
// ----------------------------------------------------------------------------

class JDockHost {
public:
    static constexpr float TAB_BAR_SZ   = 28.0f;  // tab bar / title bar thickness (Top/Bottom)
    static constexpr float V_TAB_BAR_W  = 32.0f;  // tab bar thickness for Left/Right (rotated labels)
    static constexpr float HANDLE_HALF  = 3.0f;   // half the 6px split handle hit width
    static constexpr float HANDLE_VIS   = 2.0f;   // visible divider line thickness
    static constexpr float ARROW_SZ     = 32.0f;  // drop indicator icon size
    static constexpr float BTN_SZ       = 14.0f;  // close button size on tabs

    float handleHoverPad() const {
        if (m_options.handleHoverPad.has_value()) return *m_options.handleHoverPad;
        return JDockRegistry::instance().defaultOptions().handleHoverPad.value_or(4.0f);
    }
    bool enforceMinSizes() const {
        if (m_options.enforceMinSizes.has_value()) return *m_options.enforceMinSizes;
        return JDockRegistry::instance().defaultOptions().enforceMinSizes.value_or(true);
    }
    bool showResizeCursors() const {
        if (m_options.showResizeCursors.has_value()) return *m_options.showResizeCursors;
        return JDockRegistry::instance().defaultOptions().showResizeCursors.value_or(true);
    }

    void setOptions(const JDockOptions& options) { m_options = options; }
    JDockOptions& options() { return m_options; }
    const JDockOptions& options() const { return m_options; }

    float minWidthNeeded() const {
        if (!enforceMinSizes()) return 48.f;
        if (m_nodes.empty()) return 48.f;
        return _minWidthOfNode(rootId());
    }

    float minHeightNeeded() const {
        if (!enforceMinSizes()) return 48.f;
        if (m_nodes.empty()) return 48.f;
        return _minHeightOfNode(rootId());
    }

    enum class JHoverCursor : uint8_t { Default, Horiz, Vert };

    JHoverCursor getHoverCursor(float mx, float my) const {
        if (!showResizeCursors()) return JHoverCursor::Default;
        if (m_handleDrag.active) {
            const JDockNode* sp = node(m_handleDrag.parentSplit);
            if (sp) {
                return (sp->splitDir == JSplitDir::Horizontal) ? JHoverCursor::Horiz : JHoverCursor::Vert;
            }
        }
        for (const auto& n : m_nodes) {
            if (n.type != JDockNode::JType::Split) continue;
            for (int i = 0; i < static_cast<int>(n.handleRects.size()); ++i) {
                const JRect& h = n.handleRects[i];
                float hitPad = handleHoverPad();
                float hx = h.x - (n.splitDir == JSplitDir::Horizontal ? hitPad : 0.f);
                float hw = h.width + (n.splitDir == JSplitDir::Horizontal ? hitPad * 2.f : 0.f);
                float hy = h.y - (n.splitDir == JSplitDir::Vertical ? hitPad : 0.f);
                float hh = h.height + (n.splitDir == JSplitDir::Vertical ? hitPad * 2.f : 0.f);
                if (mx >= hx && mx <= hx + hw &&
                    my >= hy && my <= hy + hh)
                {
                    return (n.splitDir == JSplitDir::Horizontal) ? JHoverCursor::Horiz : JHoverCursor::Vert;
                }
            }
        }
        return JHoverCursor::Default;
    }

    // --- Construction ---

    JDockHost() { _allocNode(); }  // root SplitNode(Horizontal)

    JDockNodeId rootId() const { return JDockNodeId{0}; }

    void setRootSplit(JSplitDir dir) {
        if (!m_nodes.empty()) m_nodes[0].splitDir = dir;
    }

    JDockNodeId addLeaf(JDockNodeId parentId,
                       std::string label = "",
                       float weight = 0.f,
                       JDockAffinityRule affinity = {},
                       JDockConstraints constraints = {})
    {
        JDockNode* parent = node(parentId);
        if (!parent || parent->type != JDockNode::JType::Split) return InvalidDockNodeId;

        JDockNodeId id = _allocNode();
        JDockNode& leaf = m_nodes[id.v];
        leaf.type        = JDockNode::JType::Leaf;
        leaf.parent      = parentId;
        leaf.label       = std::move(label);
        leaf.affinity    = std::move(affinity);
        leaf.constraints = constraints;

        // _allocNode may have reallocated m_nodes — refetch the parent.
        parent = node(parentId);
        parent->children.push_back(id);
        parent->weights.push_back(weight > 0.f ? weight : 0.f);
        _rebalanceWeights(parentId);
        return id;
    }

    JDockNodeId addSplit(JDockNodeId parentId, JSplitDir dir, float weight = 0.f) {
        JDockNode* parent = node(parentId);
        if (!parent || parent->type != JDockNode::JType::Split) return InvalidDockNodeId;

        JDockNodeId id = _allocNode();
        JDockNode& sp = m_nodes[id.v];
        sp.type     = JDockNode::JType::Split;
        sp.parent   = parentId;
        sp.splitDir = dir;

        parent = node(parentId);
        parent->children.push_back(id);
        parent->weights.push_back(weight > 0.f ? weight : 0.f);
        _rebalanceWeights(parentId);
        return id;
    }

    bool insertDock(JDockWidget* dock, JDockNodeId leafId, int tabIdx = -1) {
        JDockNode* leaf = node(leafId);
        if (!dock || !leaf || leaf->type != JDockNode::JType::Leaf) return false;
        if (!leaf->affinity.accepts(dock->tag())) return false;
        if (!dock->acceptsLeafLabel(leaf->label)) return false;
        // Non-tabifiable dock must occupy a solo leaf.
        if (!dock->isTabifiable() && !leaf->tabs.empty()) return false;
        // Can't add any dock to a leaf that already contains a non-tabifiable dock.
        for (auto* t : leaf->tabs)
            if (!t->isTabifiable()) return false;

        if (tabIdx < 0 || tabIdx > static_cast<int>(leaf->tabs.size()))
            tabIdx = static_cast<int>(leaf->tabs.size());
        leaf->tabs.insert(leaf->tabs.begin() + tabIdx, dock);
        leaf->activeTab = tabIdx;
        return true;
    }

    // Convenience: place a dock in this host in one call. Tabifies into the first leaf if
    // there is one, otherwise creates a leaf (with optional affinity, e.g. to restrict who
    // may dock here). Lets callers just declare "this dock lives here" — no leaf plumbing.
    JDockNodeId addDock(JDockWidget* dock, JDockAffinityRule affinity = {}) {
        for (auto& n : m_nodes)
            if (n.type == JDockNode::JType::Leaf && !n.tabs.empty()) { insertDock(dock, n.id); return n.id; }
        JDockNodeId leaf = addLeaf(rootId(), "", 1.0f, std::move(affinity));
        insertDock(dock, leaf);
        return leaf;
    }

    void removeDock(JDockWidget* dock) {
        for (auto& n : m_nodes) {
            if (n.type != JDockNode::JType::Leaf) continue;
            auto it = std::find(n.tabs.begin(), n.tabs.end(), dock);
            if (it == n.tabs.end()) continue;
            n.tabs.erase(it);
            if (n.activeTab >= static_cast<int>(n.tabs.size()))
                n.activeTab = std::max(0, static_cast<int>(n.tabs.size()) - 1);
            if (n.tabs.empty())
                _pruneLeaf(n.id);
            return;
        }
    }

    // Swap a tab pointer in-place — used when a dock's storage relocates
    // (e.g. a FloatingDock's JDockWidget is moved into heap-owned storage after
    // a drop has already inserted the old address into the tree).
    void retargetDock(JDockWidget* oldPtr, JDockWidget* newPtr) {
        for (auto& n : m_nodes) {
            if (n.type != JDockNode::JType::Leaf) continue;
            for (auto& t : n.tabs)
                if (t == oldPtr) { t = newPtr; return; }
        }
    }

    JDockNodeId findDock(const JDockWidget* dock) const {
        for (auto& n : m_nodes) {
            if (n.type != JDockNode::JType::Leaf) continue;
            if (std::find(n.tabs.begin(), n.tabs.end(), dock) != n.tabs.end())
                return n.id;
        }
        return InvalidDockNodeId;
    }

    // --- Layout ---

    void computeLayout(JRect hostRect) {
        m_hostRect = hostRect;
        if (!m_nodes.empty()) _computeNodeLayout(rootId(), hostRect);
    }

    void setLivePreviewEnabled(bool enabled) { m_livePreviewEnabled = enabled; }

    // Where this host draws its leaves' tab bars (Top / Bottom / Left / Right). Applies to
    // every existing leaf and becomes the default for new ones.
    // Tab style overrides for THIS host. Unset = inherit the global stylesheet (style()).
    // Read effective-* each frame so a global change reflows un-overridden hosts live.
    void setTabEdge(JTabBarEdge e) { m_tabEdge = e; }
    void clearTabEdge()            { m_tabEdge.reset(); }
    JTabBarEdge effectiveTabEdge() const { return m_tabEdge.value_or(style().tabEdge); }

    void setTabFill(JTabFill f) { m_tabFill = f; }
    void clearTabFill()         { m_tabFill.reset(); }
    JTabFill effectiveTabFill() const { return m_tabFill.value_or(style().tabFill); }
    bool isLivePreviewEnabled() const { return m_livePreviewEnabled; }

    // --- Per-frame drag tracking ---

    void updateDrag(float /*cursorWindowX*/, float /*cursorWindowY*/,
                    float cursorScreenX, float cursorScreenY,
                    int   hostScreenX,   int   hostScreenY,
                    JDockWidget* draggedDock)
    {
        if (!draggedDock) {
            if (m_livePreviewEnabled && m_hasSavedState) {
                m_nodes = m_savedNodes;
                m_nextId = m_savedNextId;
                m_hasSavedState = false;
                computeLayout(m_hostRect);
            }
            m_dropTargets.clear();
            m_activeTarget = nullptr;
            m_draggedDock  = nullptr;
            return;
        }
        m_draggedDock = draggedDock;

        if (m_livePreviewEnabled) {
            if (m_hasSavedState) {
                m_nodes = m_savedNodes;
                m_nextId = m_savedNextId;
            } else {
                m_savedNodes = m_nodes;
                m_savedNextId = m_nextId;
                m_hasSavedState = true;
            }
        }

        _buildDropTargets(draggedDock, cursorScreenX, cursorScreenY,
                          hostScreenX, hostScreenY);

        float localX = cursorScreenX - static_cast<float>(hostScreenX);
        float localY = cursorScreenY - static_cast<float>(hostScreenY);

        m_activeTarget = nullptr;
        for (auto& dt : m_dropTargets) dt.highlighted = false;

        // Compass-zone detection: divide each leaf into 5 regions.
        // 25% edge strips activate a directional split; the inner 50% activates
        // tabify (Center).  This gives large, forgiving drop zones instead of
        // requiring the dock center to land on a 32×32 indicator arrow.
        for (const auto& n : m_nodes) {
            if (n.type != JDockNode::JType::Leaf) continue;
            const JRect& r = n.rect;
            if (localX < r.x || localX > r.x + r.width ||
                localY < r.y || localY > r.y + r.height) continue;

            float rx = (r.width  > 0.f) ? (localX - r.x) / r.width  : 0.5f;
            float ry = (r.height > 0.f) ? (localY - r.y) / r.height : 0.5f;
            constexpr float kEdge = 0.25f;

            JDropPos zone;
            if      (n.tabs.empty())    zone = JDropPos::Center;  // empty leaf: only fill it
            else if (rx < kEdge)        zone = JDropPos::Left;
            else if (rx > 1.f - kEdge)  zone = JDropPos::Right;
            else if (ry < kEdge)        zone = JDropPos::Top;
            else if (ry > 1.f - kEdge)  zone = JDropPos::Bottom;
            else                        zone = JDropPos::Center;

            for (auto& dt : m_dropTargets) {
                if (dt.leaf == n.id && dt.pos == zone && dt.allowed) {
                    dt.highlighted = true;
                    m_activeTarget = &dt;
                    break;
                }
            }
            break;  // leaves don't overlap — stop after first match
        }

        if (m_livePreviewEnabled && m_activeTarget && m_activeTarget->allowed && m_activeTarget->pos != JDropPos::Center) {
            JDockNodeId newLeaf = _splitLeaf(m_activeTarget->leaf, m_activeTarget->pos);
            _computeNodeLayout(rootId(), m_hostRect);
            // The blue highlight IS the live preview: use the new leaf's actual laid-out
            // rect (where the dock will land) rather than a separate estimate, so the
            // highlight always matches the preview exactly.
            if (const JDockNode* nl = node(newLeaf)) m_activeTarget->previewRect = nl->rect;
        } else {
            _computeNodeLayout(rootId(), m_hostRect);
        }
    }

    struct JDropResult { JDockNodeId targetLeaf; JDropPos pos; };

    std::optional<JDropResult> tryCommitDrop() {
        if (!m_activeTarget || !m_activeTarget->allowed || !m_draggedDock) {
            _endDrag();
            return std::nullopt;
        }

        JDockNodeId leafId = m_activeTarget->leaf;
        JDropPos    pos    = m_activeTarget->pos;
        JDockWidget* dock  = m_draggedDock;

        if (m_hasSavedState) {
            m_nodes = m_savedNodes;
            m_nextId = m_savedNextId;
            m_hasSavedState = false;
        }

        JDockNode* leaf = node(leafId);
        if (!leaf || leaf->type != JDockNode::JType::Leaf) { _endDrag(); return std::nullopt; }

        if (pos == JDropPos::Center) {
            insertDock(dock, leafId);
        } else {
            JDockNodeId newLeaf = _splitLeaf(leafId, pos);
            if (!newLeaf.valid()) { _endDrag(); return std::nullopt; }
            insertDock(dock, newLeaf);
        }

        computeLayout(m_hostRect);
        JDropResult result{leafId, pos};
        _endDrag();
        return result;
    }

    // --- Inline mouse routing ---

    struct JDockEvent {
        JDockWidget* dock{nullptr};
        enum class JType { WantsFloat, CloseRequested } type{JType::WantsFloat};
    };

    std::optional<JDockEvent> handleMouse(float mx, float my,
                                         bool pressed, bool released)
    {
        // A new press guarantees the previous button-release was delivered (X11
        // cannot send two presses without a release between them), so any
        // lingering drag state must be stale (missed release event). Clear it
        // so it doesn't block all further mouse handling.
        if (pressed && (m_handleDrag.active || m_titleDrag.active)) {
            m_handleDrag = {};
            m_titleDrag  = {};
        }

        // 1. Resize handles take priority.
        if (m_handleDrag.active) {
            JDockNode* sp = node(m_handleDrag.parentSplit);
            if (sp && sp->type == JDockNode::JType::Split) {
                int a = m_handleDrag.handleIdx;
                int b = a + 1;
                if (b < static_cast<int>(sp->weights.size())) {
                    bool horizD  = (sp->splitDir == JSplitDir::Horizontal);
                    float cursor = horizD ? mx : my;

                    // If either side is a fixed-size (preferred-px) leaf, the handle drag
                    // must resize that leaf's PIXEL size — its weight is ignored by layout,
                    // so adjusting weights would silently push the flexible siblings around.
                    if (m_handleDrag.startPrefA > 0.f || m_handleDrag.startPrefB > 0.f) {
                        float pxDelta = cursor - m_handleDrag.startCursor;
                        JDockNode* ca = node(sp->children[a]);
                        JDockNode* cb = node(sp->children[b]);
                        if (m_handleDrag.startPrefA > 0.f && ca) {
                            float np = std::max(48.f, m_handleDrag.startPrefA + pxDelta);
                            if (horizD) ca->constraints.preferredW = np; else ca->constraints.preferredH = np;
                        }
                        if (m_handleDrag.startPrefB > 0.f && cb) {
                            float np = std::max(48.f, m_handleDrag.startPrefB - pxDelta);
                            if (horizD) cb->constraints.preferredW = np; else cb->constraints.preferredH = np;
                        }
                        computeLayout(m_hostRect);
                        if (released) m_handleDrag = {};
                        return std::nullopt;
                    }

                    float total  = horizD ? sp->rect.width : sp->rect.height;
                    float delta  = (total > 1.f)
                                       ? (cursor - m_handleDrag.startCursor) / total : 0.f;
                    float wa = m_handleDrag.startWeightA + delta;
                    float wb = m_handleDrag.startWeightB - delta;
                    // Keep both children above their minimum sizes.
                    float sum = wa + wb;
                    float minFrac_a = 0.05f;
                    float minFrac_b = 0.05f;
                    if (!enforceMinSizes()) {
                        minFrac_a = 0.0f;
                        minFrac_b = 0.0f;
                    }
                    bool horiz = (sp->splitDir == JSplitDir::Horizontal);
                    float totalDim = horiz ? sp->rect.width : sp->rect.height;
                    float handleSpace = HANDLE_HALF * 2.0f;
                    float usable = std::max(0.f, totalDim - handleSpace * static_cast<float>(sp->children.size() - 1));
                    if (usable > 0.f && enforceMinSizes()) {
                        float minVal_a = horiz ? _minWidthOfNode(sp->children[a]) : _minHeightOfNode(sp->children[a]);
                        float minVal_b = horiz ? _minWidthOfNode(sp->children[b]) : _minHeightOfNode(sp->children[b]);
                        minFrac_a = minVal_a / usable;
                        minFrac_b = minVal_b / usable;
                    }
                    if (minFrac_a + minFrac_b > sum) {
                        float totalMin = minFrac_a + minFrac_b;
                        minFrac_a = (minFrac_a / totalMin) * sum;
                        minFrac_b = (minFrac_b / totalMin) * sum;
                    }
                    wa = std::clamp(wa, minFrac_a, sum - minFrac_b);
                    wb = sum - wa;
                    sp->weights[a] = wa;
                    sp->weights[b] = wb;
                    computeLayout(m_hostRect);
                }
            }
            if (released) m_handleDrag = {};
            return std::nullopt;
        }

        if (pressed) {
            // Begin a resize-handle drag if a handle was hit.
            for (auto& n : m_nodes) {
                if (n.type != JDockNode::JType::Split) continue;
                for (int i = 0; i < static_cast<int>(n.handleRects.size()); ++i) {
                    const JRect& h = n.handleRects[i];
                    float hitPad = handleHoverPad();
                    float hx = h.x - (n.splitDir == JSplitDir::Horizontal ? hitPad : 0.f);
                    float hw = h.width + (n.splitDir == JSplitDir::Horizontal ? hitPad * 2.f : 0.f);
                    float hy = h.y - (n.splitDir == JSplitDir::Vertical ? hitPad : 0.f);
                    float hh = h.height + (n.splitDir == JSplitDir::Vertical ? hitPad * 2.f : 0.f);
                    if (mx >= hx && mx <= hx + hw &&
                        my >= hy && my <= hy + hh)
                    {
                        // Guard against a stale handle on a node whose weights/children
                        // were shrunk (tombstoned) without its handleRects being cleared.
                        if (i + 1 >= static_cast<int>(n.weights.size()) ||
                            i + 1 >= static_cast<int>(n.children.size())) continue;
                        m_handleDrag.parentSplit = n.id;
                        m_handleDrag.handleIdx   = i;
                        m_handleDrag.active      = true;
                        m_handleDrag.startCursor =
                            (n.splitDir == JSplitDir::Horizontal) ? mx : my;
                        m_handleDrag.startWeightA = n.weights[i];
                        m_handleDrag.startWeightB = n.weights[i + 1];
                        bool h = (n.splitDir == JSplitDir::Horizontal);
                        const JDockNode* ca = node(n.children[i]);
                        const JDockNode* cb = node(n.children[i + 1]);
                        m_handleDrag.startPrefA = (ca && ca->type == JDockNode::JType::Leaf)
                                                      ? (h ? ca->constraints.preferredW : ca->constraints.preferredH) : 0.f;
                        m_handleDrag.startPrefB = (cb && cb->type == JDockNode::JType::Leaf)
                                                      ? (h ? cb->constraints.preferredW : cb->constraints.preferredH) : 0.f;
                        return std::nullopt;
                    }
                }
            }
        }

        // 2. Leaf chrome — close button + title-bar drag-out.
        for (auto& n : m_nodes) {
            if (n.type != JDockNode::JType::Leaf || n.tabs.empty()) continue;

            JRect barRect = _tabBarRect(n);
            JDockWidget* active =
                (n.activeTab >= 0 && n.activeTab < static_cast<int>(n.tabs.size()))
                    ? n.tabs[n.activeTab] : nullptr;
            if (!active) continue;

            // Close button on the active tab.
            JRect closeR = _closeBtnRect(n);
            if (pressed && _inRect(closeR, mx, my))
                return JDockEvent{active, JDockEvent::JType::CloseRequested};

            // Title/tab bar drag → float.
            if (pressed && _inRect(barRect, mx, my) && !_inRect(closeR, mx, my)) {
                m_titleDrag.active = true;
                m_titleDrag.dock   = active;
                m_titleDrag.startX = mx;
                m_titleDrag.startY = my;
            }
        }

        if (m_titleDrag.active) {
            // A release ends the gesture FIRST: if the button-up event is polled in the same
            // frame as the threshold-crossing motion, this is a click that happened to drift —
            // NOT a tear-out. Firing WantsFloat here would spawn a float with the button already
            // up, which can't be dragged and instantly commits/vanishes. So cancel on release
            // before testing the threshold.
            if (released) {
                m_titleDrag = {};
            } else {
                float dx = mx - m_titleDrag.startX;
                float dy = my - m_titleDrag.startY;
                // A title drag past the threshold (while still held) becomes a float request.
                if (std::sqrt(dx * dx + dy * dy) > 16.0f) {
                    JDockWidget* d = m_titleDrag.dock;
                    m_titleDrag = {};
                    if (d->isFloatable())
                        return JDockEvent{d, JDockEvent::JType::WantsFloat};
                    // Non-floatable: silently cancel the drag.
                }
            }
        }

        // 3. Route tab clicks to switch the active tab.
        if (pressed) {
            for (auto& n : m_nodes) {
                if (n.type != JDockNode::JType::Leaf || n.tabs.size() < 2) continue;
                if (!_inRect(_tabBarRect(n), mx, my)) continue;
                const auto slots = _tabSlots(n);
                for (int i = 0; i < static_cast<int>(slots.size()); ++i)
                    if (_inRect(slots[i], mx, my)) { n.activeTab = i; break; }
            }
        }

        return std::nullopt;
    }

    // --- Rendering ---

    void populateRenderPrimitives(JPrimitiveBuffer& buf) const {
        if (m_nodes.empty()) return;
        _renderNode(rootId(), buf);
    }

    void populateOverlay(JPrimitiveBuffer& buf) const {
        for (auto& dt : m_dropTargets) _renderDropTarget(dt, buf);

        // Highlight an active resize handle.
        if (m_handleDrag.active) {
            const JDockNode* sp = node(m_handleDrag.parentSplit);
            if (sp && m_handleDrag.handleIdx <
                          static_cast<int>(sp->handleRects.size()))
            {
                const JRect& h = sp->handleRects[m_handleDrag.handleIdx];
                buf.pushRectangle(h.x, h.y, h.width, h.height, Colors::Accent, 2.0f);
            }
        }
    }

    // --- Accessors ---

    JDockNode* node(JDockNodeId id) {
        return id.valid() && id.v < m_nodes.size() ? &m_nodes[id.v] : nullptr;
    }
    const JDockNode* node(JDockNodeId id) const {
        return id.valid() && id.v < m_nodes.size() ? &m_nodes[id.v] : nullptr;
    }

    size_t dockCount() const {
        size_t count = 0;
        for (const auto& n : m_nodes) {
            if (n.type == JDockNode::JType::Leaf) {
                count += n.tabs.size();
            }
        }
        return count;
    }

    // Enumerate currently-docked panels (for AI telemetry / introspection).  Calls
    // fn(dock, leafRect, isActiveTab, tabCount) for every tab in every leaf.  Floating
    // panels live in their own OS windows and are not included here.
    template <class F>
    void forEachDockPanel(F&& fn) const {
        for (const auto& n : m_nodes) {
            if (n.type != JDockNode::JType::Leaf) continue;
            for (int i = 0; i < static_cast<int>(n.tabs.size()); ++i)
                if (n.tabs[i]) fn(n.tabs[i], n.rect, i == n.activeTab,
                                  static_cast<int>(n.tabs.size()));
        }
    }

    // Activate (bring to front) a docked panel by title.  Returns true if found.
    bool activatePanelByTitle(const std::string& title) {
        for (auto& n : m_nodes) {
            if (n.type != JDockNode::JType::Leaf) continue;
            for (int i = 0; i < static_cast<int>(n.tabs.size()); ++i)
                if (n.tabs[i] && n.tabs[i]->title() == title) { n.activeTab = i; return true; }
        }
        return false;
    }

    JRect contentArea(JDockNodeId leafId) const {
        const JDockNode* leaf = node(leafId);
        return leaf ? contentArea(leafId, leaf->activeTab) : JRect{};
    }

    JRect contentArea(JDockNodeId leafId, int /*tabIdx*/) const {
        const JDockNode* leaf = node(leafId);
        if (!leaf || leaf->type != JDockNode::JType::Leaf) return JRect{};
        return _leafContentRect(*leaf);
    }

    // --- Raw tree save/restore (deep copy of the node arena) ---
    // Preserves EVERYTHING — structure, weights, fixed-size constraints — unlike the
    // title-based snapshot below (which is for serialization). Used to revert a drag to
    // its exact pre-drag state. Dock pointers are copied as-is; retargetDock() fixes any
    // that moved (e.g. a torn-out dock that was re-homed).
    struct JSavedTree { std::vector<JDockNode> nodes; uint32_t nextId{0}; };
    JSavedTree saveTree() const { return { m_nodes, m_nextId }; }
    void restoreTree(const JSavedTree& s) {
        m_nodes = s.nodes; m_nextId = s.nextId;
        m_handleDrag = {}; m_dropTargets.clear(); m_activeTarget = nullptr; m_draggedDock = nullptr;
        computeLayout(m_hostRect);
    }

    // --- Layout snapshot / restore ---

    // Capture the current tree as a serializable value.
    JDockLayoutSnapshot snapshot() const {
        JDockLayoutSnapshot snap;
        if (!m_nodes.empty()) snap.root = _snapshotNode(rootId());
        return snap;
    }

    // Rebuild the tree from a snapshot.
    // `resolver(title)` must return the JDockWidget* to insert, or nullptr to skip.
    // The host rect is NOT re-applied — call computeLayout() after restore().
    bool restore(const JDockLayoutSnapshot& snap,
                 const std::function<JDockWidget*(std::string_view)>& resolver)
    {
        m_nodes.clear();
        m_nextId       = 0;
        m_hostRect     = {};
        m_handleDrag   = {};
        m_titleDrag    = {};
        m_dropTargets.clear();
        m_activeTarget = nullptr;
        m_draggedDock  = nullptr;
        _allocNode();  // root at id 0
        return _restoreNode(snap.root, rootId(), InvalidDockNodeId, resolver);
    }

    JDockNodeId splitLeaf(JDockNodeId leafId, JDropPos pos) { return _splitLeaf(leafId, pos); }
    JDockNodeId edgeLeaf() const { return _edgeLeaf(); }
    JRect hostRect() const { return m_hostRect; }

private:
    // ----------------------------------------------------------------------
    // Drop target state — declared before any use in member functions.
    // ----------------------------------------------------------------------

    struct JDropTarget {
        JDockNodeId leaf;
        JDropPos    pos;
        JRect       indicatorRect;
        JRect       previewRect;
        bool       highlighted{false};
        bool       allowed{false};
    };

    // ----------------------------------------------------------------------
    // Arena management
    // ----------------------------------------------------------------------

    float _minWidthOfNode(JDockNodeId id) const {
        const JDockNode* n = node(id);
        if (!n) return 48.f;
        if (n->type == JDockNode::JType::Leaf) {
            int tabCount = static_cast<int>(n->tabs.size());
            if (tabCount == 0) return 48.f;
            if (tabCount == 1) {
                float lw = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(n->tabs[0]->title()) : 50.f;
                float contentMinW = n->tabs[0]->minW();
                return std::max(lw + 50.f, contentMinW);
            }
            float maxLw = 0.f;
            float maxWidgetMinW = 0.f;
            for (int i = 0; i < tabCount; ++i) {
                float lw = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(n->tabs[i]->title()) : 50.f;
                if (lw > maxLw) maxLw = lw;
                if (n->tabs[i]) {
                    float w = n->tabs[i]->minW();
                    if (w > maxWidgetMinW) maxWidgetMinW = w;
                }
            }
            float tabsMinW = static_cast<float>(tabCount) * (maxLw + 16.f) + 30.f;
            return std::max(tabsMinW, maxWidgetMinW);
        }
        bool horiz = (n->splitDir == JSplitDir::Horizontal);
        float sum = 0.f;
        float maxVal = 0.f;
        for (JDockNodeId childId : n->children) {
            float childMin = _minWidthOfNode(childId);
            if (horiz) {
                sum += childMin;
            } else {
                if (childMin > maxVal) maxVal = childMin;
            }
        }
        if (horiz) {
            float handleSpace = 6.0f; // HANDLE_HALF * 2
            sum += handleSpace * static_cast<float>(n->children.size() - 1);
            return sum;
        }
        return maxVal;
    }

    float _minHeightOfNode(JDockNodeId id) const {
        const JDockNode* n = node(id);
        if (!n) return 48.f;
        if (n->type == JDockNode::JType::Leaf) {
            int tabCount = static_cast<int>(n->tabs.size());
            float maxWidgetMinH = 40.f;
            for (int i = 0; i < tabCount; ++i) {
                if (n->tabs[i]) {
                    float h = n->tabs[i]->minH();
                    if (h > maxWidgetMinH) maxWidgetMinH = h;
                }
            }
            return TAB_BAR_SZ + maxWidgetMinH;
        }
        bool horiz = (n->splitDir == JSplitDir::Horizontal);
        float sum = 0.f;
        float maxVal = 0.f;
        for (JDockNodeId childId : n->children) {
            float childMin = _minHeightOfNode(childId);
            if (horiz) {
                if (childMin > maxVal) maxVal = childMin;
            } else {
                sum += childMin;
            }
        }
        if (!horiz) {
            float handleSpace = 6.0f; // HANDLE_HALF * 2
            sum += handleSpace * static_cast<float>(n->children.size() - 1);
            return sum;
        }
        return maxVal;
    }

    JDockNodeId _allocNode() {
        JDockNodeId id{m_nextId++};
        JDockNode n;
        n.id = id;
        m_nodes.push_back(std::move(n));
        return id;
    }

    void _rebalanceWeights(JDockNodeId parentId) {
        JDockNode* p = node(parentId);
        if (!p) return;
        size_t n = p->children.size();
        if (n == 0) return;
        p->weights.resize(n, 0.f);

        float assigned = 0.f;
        int   unset    = 0;
        for (float w : p->weights) {
            if (w > 0.f) assigned += w;
            else         ++unset;
        }
        if (assigned > 1.0f) {
            // Normalize explicit weights down to fit.
            for (auto& w : p->weights) if (w > 0.f) w /= assigned;
            assigned = 1.0f;
        }
        float remaining = std::max(0.f, 1.0f - assigned);
        if (unset > 0) {
            float each = remaining / static_cast<float>(unset);
            for (auto& w : p->weights) if (w <= 0.f) w = each;
        } else {
            // Renormalize so the sum is exactly 1.
            float sum = 0.f;
            for (float w : p->weights) sum += w;
            if (sum > 1e-6f) for (auto& w : p->weights) w /= sum;
        }
    }

    // ----------------------------------------------------------------------
    // Layout
    // ----------------------------------------------------------------------

    void _computeNodeLayout(JDockNodeId id, JRect rect) {
        JDockNode* n = node(id);
        if (!n) return;
        n->rect = rect;

        if (n->type == JDockNode::JType::Leaf) {
            n->handleRects.clear();
            return;
        }

        // Split: divide rect along splitDir by weights.
        size_t count = n->children.size();
        n->handleRects.clear();
        if (count == 0) return;
        if (n->weights.size() != count) _rebalanceWeights(id);

        bool horiz = (n->splitDir == JSplitDir::Horizontal);
        float total = horiz ? rect.width : rect.height;

        // Reserve space for the (count-1) handles so children never overlap them.
        float handleSpace = HANDLE_HALF * 2.0f;
        float usable = std::max(0.f, total - handleSpace * static_cast<float>(count - 1));

        // Snapshot weights/children: child layout may reallocate the arena.
        std::vector<JDockNodeId> children = n->children;
        std::vector<float>      weights  = n->weights;
        std::vector<JRect>       handles;
        handles.reserve(count > 0 ? count - 1 : 0);

        // Fixed-size pass: a leaf child that declares a preferred size along this axis
        // (JDockConstraints::preferredW/H) gets exactly that many pixels; the remaining
        // space distributes among the flexible children by weight. No preferred sizes ->
        // identical to a pure weighted split.
        std::vector<float> fixed(count, 0.f);
        float fixedTotal = 0.f, flexWeight = 0.f;
        for (size_t i = 0; i < count; ++i) {
            const JDockNode* c = node(children[i]);
            // Honour a preferred (fixed) size on leaves AND splits — a split that wraps a
            // fixed-width column (e.g. after a vertical split) carries the fixed width.
            float pref = c ? (horiz ? c->constraints.preferredW : c->constraints.preferredH) : 0.f;
            if (pref > 0.f) { fixed[i] = pref; fixedTotal += pref; }
            else            { flexWeight += weights[i]; }
        }
        float flexSpace = std::max(0.f, usable - fixedTotal);

        // Safety net: if every child is fixed (no flexible sibling to absorb slack), scale
        // them to fill — rare now that a split target always flexes, but prevents a gap.
        if (flexWeight <= 0.f && fixedTotal > 0.f) {
            float scale = usable / fixedTotal;
            for (auto& f : fixed) f *= scale;
            flexSpace = 0.f;
        }

        float cursor = horiz ? rect.x : rect.y;
        for (size_t i = 0; i < count; ++i) {
            float seg = (fixed[i] > 0.f)
                            ? fixed[i]
                            : (flexWeight > 0.f ? flexSpace * (weights[i] / flexWeight) : 0.f);
            JRect childRect;
            if (horiz) childRect = { cursor, rect.y, seg, rect.height };
            else       childRect = { rect.x, cursor, rect.width, seg };
            _computeNodeLayout(children[i], childRect);
            cursor += seg;

            if (i + 1 < count) {
                if (horiz) handles.push_back({ cursor, rect.y, handleSpace, rect.height });
                else       handles.push_back({ rect.x, cursor, rect.width, handleSpace });
                cursor += handleSpace;
            }
        }

        // Re-fetch after recursion (arena may have grown) and store handles.
        n = node(id);
        if (n) n->handleRects = std::move(handles);
    }

    JRect _tabBarRect(const JDockNode& leaf) const {
        const JRect& r = leaf.rect;
        switch (effectiveTabEdge()) {
            case JTabBarEdge::Top:    return { r.x, r.y, r.width, TAB_BAR_SZ };
            case JTabBarEdge::Bottom: return { r.x, r.y + r.height - TAB_BAR_SZ, r.width, TAB_BAR_SZ };
            case JTabBarEdge::Left:   return { r.x, r.y, V_TAB_BAR_W, r.height };
            case JTabBarEdge::Right:  return { r.x + r.width - V_TAB_BAR_W, r.y, V_TAB_BAR_W, r.height };
        }
        return { r.x, r.y, r.width, TAB_BAR_SZ };
    }

    // Per-tab rects along the bar, honouring the effective tab edge + fill. Horizontal bars
    // (Top/Bottom) lay tabs left-to-right; vertical bars (Left/Right) stack them top-to-bottom.
    //   Fill     — equal share of the bar
    //   Left     — natural extent, justified to the start (trailing space empty)
    //   Compress — natural if they fit, else shrink equally
    std::vector<JRect> _tabSlots(const JDockNode& leaf) const {
        const JRect bar = _tabBarRect(leaf);
        const int n = static_cast<int>(leaf.tabs.size());
        std::vector<JRect> out;
        if (n <= 0) return out;
        const JTabBarEdge edge = effectiveTabEdge();
        const bool vert = (edge == JTabBarEdge::Left || edge == JTabBarEdge::Right);
        const float barLen = vert ? bar.height : bar.width;
        std::vector<float> ext(n);
        float sum = 0.f;
        for (int i = 0; i < n; ++i) {
            // Natural extent along the bar = the label run length (rotated for vertical bars).
            float nat = (JTextHelper::hasAtlas() && leaf.tabs[i])
                            ? JTextHelper::measureWidth(leaf.tabs[i]->title()) + 28.f : 90.f;
            ext[i] = nat; sum += nat;
        }
        const JTabFill fill = effectiveTabFill();
        if (fill == JTabFill::Fill || (fill == JTabFill::Compress && sum > barLen)) {
            const float e = barLen / static_cast<float>(n);
            for (auto& x : ext) x = e;
        }
        float off = 0.f;
        for (int i = 0; i < n; ++i) {
            out.push_back(vert ? JRect{ bar.x, bar.y + off, bar.width, ext[i] }
                               : JRect{ bar.x + off, bar.y, ext[i], bar.height });
            off += ext[i];
        }
        return out;
    }

    JRect _leafContentRect(const JDockNode& leaf) const {
        const JRect& r = leaf.rect;
        switch (effectiveTabEdge()) {
            case JTabBarEdge::Top:    return { r.x, r.y + TAB_BAR_SZ, r.width, r.height - TAB_BAR_SZ };
            case JTabBarEdge::Bottom: return { r.x, r.y, r.width, r.height - TAB_BAR_SZ };
            case JTabBarEdge::Left:   return { r.x + V_TAB_BAR_W, r.y, r.width - V_TAB_BAR_W, r.height };
            case JTabBarEdge::Right:  return { r.x, r.y, r.width - V_TAB_BAR_W, r.height };
        }
        return { r.x, r.y + TAB_BAR_SZ, r.width, r.height - TAB_BAR_SZ };
    }

    // Close button sits at the right edge of the (top) tab bar.
    JRect _closeBtnRect(const JDockNode& leaf) const {
        JRect bar = _tabBarRect(leaf);
        float pad = (TAB_BAR_SZ - BTN_SZ) * 0.5f;
        return { bar.x + bar.width - BTN_SZ - pad,
                 bar.y + (bar.height - BTN_SZ) * 0.5f,
                 BTN_SZ, BTN_SZ };
    }

    static bool _inRect(const JRect& r, float x, float y) {
        return x >= r.x && x <= r.x + r.width &&
               y >= r.y && y <= r.y + r.height;
    }

    // ----------------------------------------------------------------------
    // Rendering
    // ----------------------------------------------------------------------

    void _renderNode(JDockNodeId id, JPrimitiveBuffer& buf) const {
        const JDockNode* n = node(id);
        if (!n) return;
        if (n->type == JDockNode::JType::Leaf) {
            _renderLeaf(*n, buf);
        } else {
            for (JDockNodeId c : n->children) _renderNode(c, buf);
            _renderSplitHandles(*n, buf);
        }
    }

    void _renderLeaf(const JDockNode& leaf, JPrimitiveBuffer& buf) const {
        const JRect& r = leaf.rect;
        if (r.width < 1.f || r.height < 1.f) return;

        // Body background.
        uint8_t body[4] = {22, 22, 26, 255};
        buf.pushRectangle(r.x, r.y, r.width, r.height, body, 0.0f, 1.0f, Colors::Border);

        // Content area (slightly darker than the body).
        JRect content = _leafContentRect(leaf);
        uint8_t contentBg[4] = {14, 14, 16, 255};
        buf.pushRectangle(content.x, content.y, content.width, content.height, contentBg, 0.0f);

        // Tab / title bar.
        JRect bar = _tabBarRect(leaf);
        buf.pushRectangle(bar.x, bar.y, bar.width, bar.height, Colors::Surface1, 0.0f);

        int tabCount = static_cast<int>(leaf.tabs.size());
        if (tabCount == 0) return;

        if (tabCount == 1) {
            // Single dock — render a title bar like JDockWidget's.
            buf.pushRectangle(bar.x + 1.f, bar.y + 1.f, bar.width - 2.f, bar.height - 2.f,
                              Colors::Surface2, 0.0f);
            if (JTextHelper::hasAtlas()) {
                uint8_t tc[4] = {210, 210, 220, 220};
                float ty = bar.y + (bar.height - JTextHelper::lineHeight()) * 0.5f;
                JTextHelper::pushText(buf, bar.x + 10.f, ty,
                                     leaf.tabs[0]->title(), tc, bar.width - BTN_SZ - 24.f);
            }
        } else {
            const JTabBarEdge edge = effectiveTabEdge();
            const bool vert = (edge == JTabBarEdge::Left || edge == JTabBarEdge::Right);
            const float lineH = JTextHelper::lineHeight();
            const auto slots = _tabSlots(leaf);
            for (int i = 0; i < tabCount && i < static_cast<int>(slots.size()); ++i) {
                const JRect& t = slots[i];
                bool active = (i == leaf.activeTab);
                const uint8_t* fill = active ? Colors::Surface3 : Colors::Surface1;
                buf.pushRectangle(t.x + 1.f, t.y + 1.f, t.width - 2.f, t.height - 2.f, fill, 0.0f);
                if (active) {
                    if (vert) {
                        float sx = (edge == JTabBarEdge::Left) ? (t.x + t.width - 3.f) : t.x + 0.5f;
                        buf.pushRectangle(sx, t.y + 4.f, 2.5f, t.height - 8.f, Colors::Accent, 1.0f);
                    } else {
                        buf.pushRectangle(t.x + 4.f, t.y + t.height - 3.f, t.width - 8.f, 2.5f, Colors::Accent, 1.0f);
                    }
                }
                if (JTextHelper::hasAtlas() && leaf.tabs[i]) {
                    std::string label = leaf.tabs[i]->title();
                    float lw = JTextHelper::measureWidth(label);
                    uint8_t lc[4] = {active ? (uint8_t)220 : (uint8_t)150,
                                     active ? (uint8_t)220 : (uint8_t)150,
                                     active ? (uint8_t)228 : (uint8_t)158,
                                     active ? (uint8_t)220 : (uint8_t)150};
                    if (vert) {
                        // Left edge reads bottom->top (CCW), right edge top->bottom (CW).
                        const bool cw = (edge == JTabBarEdge::Right);
                        const float run = std::min(lw, t.height - 12.f);
                        const float px = cw ? (t.x + t.width * 0.5f + lineH * 0.5f)
                                            : (t.x + t.width * 0.5f - lineH * 0.5f);
                        const float py = cw ? (t.y + (t.height - run) * 0.5f)
                                            : (t.y + (t.height + run) * 0.5f);
                        JTextHelper::pushTextVertical(buf, px, py, label, lc, t.height - 12.f, cw);
                    } else {
                        float ly = t.y + (t.height - lineH) * 0.5f;
                        JTextHelper::pushText(buf, t.x + std::max(6.f, (t.width - lw) * 0.5f), ly,
                                             label, lc, t.width - 10.f);
                    }
                }
            }
        }

        // Close button on the active tab / single title bar.
        JRect closeR = _closeBtnRect(leaf);
        uint8_t closeFill[4] = {60, 40, 44, 180};
        buf.pushRectangle(closeR.x, closeR.y, closeR.width, closeR.height, closeFill, 3.0f);
        uint8_t xc[4] = {235, 235, 240, 210};
        buf.pushRectangle(closeR.x + 3.f, closeR.y + closeR.height * 0.42f,
                          closeR.width - 6.f, 2.0f, xc, 1.0f);
        buf.pushRectangle(closeR.x + closeR.width * 0.42f, closeR.y + 3.f,
                          2.0f, closeR.height - 6.f, xc, 1.0f);

        // Pin / constraint badge on single title bars.
        // Colour encodes the active dock's behaviour flags so the user has a
        // visual hint without needing a tooltip:
        //   amber = not floatable (locked to the host)
        //   blue  = not tabifiable (must be solo in its leaf)
        //   dim   = default (fully flexible dock)
        if (tabCount == 1) {
            float pad = (TAB_BAR_SZ - BTN_SZ) * 0.5f;
            float pinX = closeR.x - BTN_SZ - pad;
            const JDockWidget* d = leaf.tabs[0];
            uint8_t pinFill[4];
            if (!d->isFloatable()) {
                pinFill[0]=180; pinFill[1]=80;  pinFill[2]=20;  pinFill[3]=220;
            } else if (!d->isTabifiable()) {
                pinFill[0]=20;  pinFill[1]=100; pinFill[2]=200; pinFill[3]=200;
            } else {
                pinFill[0]=50;  pinFill[1]=50;  pinFill[2]=60;  pinFill[3]=140;
            }
            buf.pushRectangle(pinX, closeR.y, BTN_SZ, BTN_SZ, pinFill, 3.0f);
            uint8_t needle[4] = {200, 200, 210, 180};
            buf.pushRectangle(pinX + BTN_SZ * 0.42f, closeR.y + 2.f, 2.0f, BTN_SZ - 4.f, needle, 1.0f);
        }
    }

    void _renderSplitHandles(const JDockNode& split, JPrimitiveBuffer& buf) const {
        bool horiz = (split.splitDir == JSplitDir::Horizontal);
        for (const JRect& h : split.handleRects) {
            // Subtle divider line centered in the 6px hit area.
            uint8_t line[4] = {60, 60, 64, 255};
            if (horiz) {
                float lx = h.x + (h.width - HANDLE_VIS) * 0.5f;
                buf.pushRectangle(lx, h.y, HANDLE_VIS, h.height, line, 1.0f);
            } else {
                float ly = h.y + (h.height - HANDLE_VIS) * 0.5f;
                buf.pushRectangle(h.x, ly, h.width, HANDLE_VIS, line, 1.0f);
            }
        }
    }

    void _renderDropTarget(const JDropTarget& dt, JPrimitiveBuffer& buf) const {
        if (!dt.allowed) return;
        const JRect& a = dt.indicatorRect;

        if (dt.highlighted) {
            // Semi-transparent preview of where the dock would land — scheme accent.
            uint8_t preview[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 70};
            uint8_t pBorder[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 200};
            buf.pushRectangle(dt.previewRect.x, dt.previewRect.y,
                              dt.previewRect.width, dt.previewRect.height,
                              preview, 4.0f, 2.0f, pBorder);
            // Bright accent arrow background.
            uint8_t bg[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 235};
            buf.pushRectangle(a.x, a.y, a.width, a.height, bg, 6.0f);
            _renderArrow(dt, buf, /*white=*/true);
        } else {
            // Faint grey arrow background.
            uint8_t bg[4]     = {40, 40, 46, 170};
            uint8_t border[4] = {90, 90, 96, 180};
            buf.pushRectangle(a.x, a.y, a.width, a.height, bg, 6.0f, 1.0f, border);
            _renderArrow(dt, buf, /*white=*/false);
        }
    }

    // Pure-geometry chevron: short rects forming a directional arrow.
    void _renderArrow(const JDropTarget& dt, JPrimitiveBuffer& buf, bool white) const {
        const JRect& a = dt.indicatorRect;
        uint8_t c[4] = { white ? (uint8_t)255 : (uint8_t)180,
                         white ? (uint8_t)255 : (uint8_t)180,
                         white ? (uint8_t)255 : (uint8_t)190,
                         white ? (uint8_t)235 : (uint8_t)200 };
        float cx = a.x + a.width * 0.5f;
        float cy = a.y + a.height * 0.5f;
        float arm = a.width * 0.22f;
        float th  = 3.0f;

        if (dt.pos == JDropPos::Center) {
            // A small square outline to denote "tabify here".
            uint8_t sq[4] = {c[0], c[1], c[2], c[3]};
            float s = a.width * 0.34f;
            buf.pushRectangle(cx - s * 0.5f, cy - s * 0.5f, s, s, Colors::Transparent, 0.0f, 2.0f, sq);
            return;
        }

        switch (dt.pos) {
            case JDropPos::Left:
            case JDropPos::SplitBefore:
                buf.pushRectangle(cx - arm * 0.4f, cy - arm, th, arm + th, c, 1.0f);
                buf.pushRectangle(cx - arm * 0.4f, cy,       th, arm,      c, 1.0f);
                buf.pushRectangle(cx - arm,        cy - th * 0.5f, arm, th, c, 1.0f);
                break;
            case JDropPos::Right:
            case JDropPos::SplitAfter:
                buf.pushRectangle(cx + arm * 0.4f - th, cy - arm, th, arm + th, c, 1.0f);
                buf.pushRectangle(cx + arm * 0.4f - th, cy,       th, arm,      c, 1.0f);
                buf.pushRectangle(cx,                   cy - th * 0.5f, arm, th, c, 1.0f);
                break;
            case JDropPos::Top:
                buf.pushRectangle(cx - arm, cy - arm * 0.4f, arm + th, th, c, 1.0f);
                buf.pushRectangle(cx,       cy - arm * 0.4f, arm,      th, c, 1.0f);
                buf.pushRectangle(cx - th * 0.5f, cy - arm, th, arm, c, 1.0f);
                break;
            case JDropPos::Bottom:
                buf.pushRectangle(cx - arm, cy + arm * 0.4f - th, arm + th, th, c, 1.0f);
                buf.pushRectangle(cx,       cy + arm * 0.4f - th, arm,      th, c, 1.0f);
                buf.pushRectangle(cx - th * 0.5f, cy, th, arm, c, 1.0f);
                break;
            default: break;
        }
    }

    // ----------------------------------------------------------------------
    // Drop target generation
    // ----------------------------------------------------------------------

    void _buildDropTargets(JDockWidget* dock, float /*screenX*/, float /*screenY*/,
                           int /*hostScreenX*/, int /*hostScreenY*/)
    {
        m_dropTargets.clear();
        if (!dock) return;
        std::string_view tag = dock->tag();

        // Per-leaf cross of 5 indicators (Center + 4 sides).
        for (const auto& n : m_nodes) {
            if (n.type != JDockNode::JType::Leaf) continue;
            const JRect& r = n.rect;
            if (r.width < ARROW_SZ * 2.f || r.height < ARROW_SZ * 2.f) continue;

            // Whether any existing tab in this leaf is non-tabifiable (blocks
            // further tabs from being added via Center drop).
            bool leafBlocksTab = false;
            for (auto* t : n.tabs)
                if (!t->isTabifiable()) { leafBlocksTab = true; break; }

            // Per-position allowed check combining all four axes:
            //   1. Leaf affinity (leaf's accept/reject list vs dock tag)
            //   2. Dock affinity (dock's accept/reject list vs leaf label)
            //   3. Dock position mask (kDropLeft etc.)
            //   4. Tabify rules (Center blocked when tabify not possible)
            auto isAllowed = [&](JDropPos pos) -> bool {
                if (!n.affinity.accepts(tag)) return false;
                if (!dock->acceptsLeafLabel(n.label)) return false;
                // An empty leaf (e.g. a collapsed area's placeholder) can only be FILLED —
                // splitting nothing yields a silly thin slice, so offer Center only.
                if (n.tabs.empty() && pos != JDropPos::Center) return false;
                if (static_cast<uint8_t>(pos) <= static_cast<uint8_t>(JDropPos::Bottom)) {
                    uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(pos));
                    if (!dock->allowsDrop(bit)) return false;
                }
                if (pos == JDropPos::Center) {
                    if (!n.constraints.allowTabDrop) return false;   // non-dockable centre
                    if (!n.tabs.empty()) {
                        if (!dock->isTabifiable()) return false;
                        if (leafBlocksTab)         return false;
                    }
                }
                return true;
            };

            float cx = r.x + r.width * 0.5f;
            float cy = r.y + r.height * 0.5f;
            float half = ARROW_SZ * 0.5f;
            float gap = ARROW_SZ + 6.f;

            auto centerRect = [&](float ox, float oy) -> JRect {
                return { cx + ox - half, cy + oy - half, ARROW_SZ, ARROW_SZ };
            };
            // The landing preview matches the size the dock will actually take on drop —
            // its retained PIXEL size (clamped to the target), mirroring the fixed-px split
            // in _splitLeaf — so the highlight equals where the dock lands, not a fraction.
            auto sidePreview = [&](JDropPos pos) -> JRect {
                bool vert = (pos == JDropPos::Top || pos == JDropPos::Bottom);
                float targetDim = vert ? r.height : r.width;
                float dockDim   = vert ? dock->height() : dock->width();
                float sz = (dockDim > 0.f) ? std::min(dockDim, targetDim) : targetDim * 0.5f;
                switch (pos) {
                    case JDropPos::Left:   return { r.x, r.y, sz, r.height };
                    case JDropPos::Right:  return { r.x + r.width - sz, r.y, sz, r.height };
                    case JDropPos::Top:    return { r.x, r.y, r.width, sz };
                    case JDropPos::Bottom: return { r.x, r.y + r.height - sz, r.width, sz };
                    default:              return r;
                }
            };

            m_dropTargets.push_back({ n.id, JDropPos::Center, centerRect(0, 0),    r,                            false, isAllowed(JDropPos::Center) });
            m_dropTargets.push_back({ n.id, JDropPos::Left,   centerRect(-gap, 0), sidePreview(JDropPos::Left),   false, isAllowed(JDropPos::Left)   });
            m_dropTargets.push_back({ n.id, JDropPos::Right,  centerRect(gap, 0),  sidePreview(JDropPos::Right),  false, isAllowed(JDropPos::Right)  });
            m_dropTargets.push_back({ n.id, JDropPos::Top,    centerRect(0, -gap), sidePreview(JDropPos::Top),    false, isAllowed(JDropPos::Top)    });
            m_dropTargets.push_back({ n.id, JDropPos::Bottom, centerRect(0, gap),  sidePreview(JDropPos::Bottom), false, isAllowed(JDropPos::Bottom) });
        }

        // JWindow-border edge strips: drop onto the largest leaf along that edge.
        const JRect& host = m_hostRect;
        JDockNodeId edgeLeaf = _edgeLeaf();
        if (edgeLeaf.valid()) {
            const JDockNode* el = node(edgeLeaf);
            auto edgeAllowed = [&](JDropPos pos) -> bool {
                if (!el) return false;
                // Empty host (only a placeholder): no host-edge splits — fill it (per-leaf
                // Center) only, so an empty area offers exactly one drop position.
                if (el->tabs.empty()) return false;
                if (!el->affinity.accepts(tag)) return false;
                if (!dock->acceptsLeafLabel(el->label)) return false;
                if (static_cast<uint8_t>(pos) <= static_cast<uint8_t>(JDropPos::Bottom)) {
                    uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(pos));
                    if (!dock->allowsDrop(bit)) return false;
                }
                return true;
            };
            float m = 8.f;
            m_dropTargets.push_back({ edgeLeaf, JDropPos::Left,
                { host.x + m, host.y + host.height * 0.5f - ARROW_SZ * 0.5f, ARROW_SZ, ARROW_SZ },
                { host.x, host.y, host.width * 0.33f, host.height }, false, edgeAllowed(JDropPos::Left) });
            m_dropTargets.push_back({ edgeLeaf, JDropPos::Right,
                { host.x + host.width - m - ARROW_SZ, host.y + host.height * 0.5f - ARROW_SZ * 0.5f, ARROW_SZ, ARROW_SZ },
                { host.x + host.width * 0.67f, host.y, host.width * 0.33f, host.height }, false, edgeAllowed(JDropPos::Right) });
            m_dropTargets.push_back({ edgeLeaf, JDropPos::Top,
                { host.x + host.width * 0.5f - ARROW_SZ * 0.5f, host.y + m, ARROW_SZ, ARROW_SZ },
                { host.x, host.y, host.width, host.height * 0.33f }, false, edgeAllowed(JDropPos::Top) });
            m_dropTargets.push_back({ edgeLeaf, JDropPos::Bottom,
                { host.x + host.width * 0.5f - ARROW_SZ * 0.5f, host.y + host.height - m - ARROW_SZ, ARROW_SZ, ARROW_SZ },
                { host.x, host.y + host.height * 0.67f, host.width, host.height * 0.33f }, false, edgeAllowed(JDropPos::Bottom) });
        }
    }

    // The leaf with the largest area — used as the target for window-edge drops.
    JDockNodeId _edgeLeaf() const {
        JDockNodeId best = InvalidDockNodeId;
        float bestArea = -1.f;
        for (const auto& n : m_nodes) {
            if (n.type != JDockNode::JType::Leaf) continue;
            float a = n.rect.width * n.rect.height;
            if (a > bestArea) { bestArea = a; best = n.id; }
        }
        return best;
    }

    // ----------------------------------------------------------------------
    // Tree healing — remove an empty leaf and collapse the parent split if
    // it ends up with only one child.
    // ----------------------------------------------------------------------

    void _pruneLeaf(JDockNodeId leafId) {
        JDockNode* leaf = node(leafId);
        if (!leaf || leaf->type != JDockNode::JType::Leaf) return;

        JDockNodeId parentId = leaf->parent;
        if (!parentId.valid()) return;

        JDockNode* parent = node(parentId);
        if (!parent || parent->type != JDockNode::JType::Split) return;

        // Remove the empty leaf from its parent.
        auto cit = std::find(parent->children.begin(), parent->children.end(), leafId);
        if (cit == parent->children.end()) return;
        size_t idx = static_cast<size_t>(cit - parent->children.begin());
        parent->children.erase(parent->children.begin() + idx);
        if (idx < parent->weights.size())
            parent->weights.erase(parent->weights.begin() + idx);
        _rebalanceWeights(parentId);

        // Tombstone the leaf: make it an orphaned empty split so it is skipped
        // by all leaf-scanning loops without needing a separate dead flag.
        leaf->type     = JDockNode::JType::Split;
        leaf->parent   = InvalidDockNodeId;
        leaf->tabs.clear();
        leaf->handleRects.clear();

        // If the parent split now has only one child, lift that child up to
        // replace the parent in the grandparent, eliminating the redundant split.
        if (parent->children.size() == 1)
            _collapseSingleChildSplit(parentId);

        // If the host is now completely empty (root has no children), plant a
        // placeholder Leaf so the JDockHost remains a valid drop target even
        // with zero docks.  The placeholder renders as a plain dark background
        // and _renderLeaf already exits early when tabs is empty.
        JDockNode* root = node(rootId());
        if (root && root->children.empty()) {
            JDockNodeId ph = _allocNode();
            // _allocNode may reallocate m_nodes — refetch root.
            root = node(rootId());
            JDockNode* phNode = node(ph);
            if (phNode) {
                phNode->type   = JDockNode::JType::Leaf;
                phNode->parent = rootId();
            }
            if (root) {
                root->children.push_back(ph);
                root->weights.push_back(1.0f);
            }
        }

        // Recompute layout so the surviving nodes fill the freed space with
        // correct rects.  Without this the surviving leaf keeps its old rect
        // and renders at the wrong position / size.
        computeLayout(m_hostRect);
    }

    // Replace a single-child split with its sole child in the grandparent.
    void _collapseSingleChildSplit(JDockNodeId splitId) {
        JDockNode* split = node(splitId);
        if (!split || split->type != JDockNode::JType::Split || split->children.size() != 1) return;

        JDockNodeId survivorId = split->children[0];
        JDockNodeId grandpaId  = split->parent;

        if (!grandpaId.valid()) return;  // split is root — can't lift further

        JDockNode* granpa = node(grandpaId);
        if (!granpa) return;

        // Swap the split out of the grandparent and put the survivor in its slot.
        for (auto& c : granpa->children)
            if (c == splitId) { c = survivorId; break; }

        JDockNode* survivor = node(survivorId);
        if (survivor) survivor->parent = grandpaId;

        // Tombstone the old split (orphaned empty split, never reached from root).
        // Clear handleRects too: leaf/handle-scan loops key off these, and a stale
        // handle on a node with no weights causes an out-of-bounds weights access.
        split->children.clear();
        split->weights.clear();
        split->handleRects.clear();
        split->parent = InvalidDockNodeId;
    }

    // ----------------------------------------------------------------------
    // Tree mutation — split a leaf, inserting a new sibling leaf.
    // Returns the id of the new empty leaf (caller inserts the dock into it).
    // ----------------------------------------------------------------------

    JDockNodeId _splitLeaf(JDockNodeId leafId, JDropPos pos) {
        JDockNode* leaf = node(leafId);
        if (!leaf || leaf->type != JDockNode::JType::Leaf) return InvalidDockNodeId;
        JDockNodeId parentId = leaf->parent;
        if (!parentId.valid()) return InvalidDockNodeId;

        bool wantVertical = (pos == JDropPos::Top || pos == JDropPos::Bottom);
        JSplitDir desired = wantVertical ? JSplitDir::Vertical : JSplitDir::Horizontal;
        bool before = (pos == JDropPos::Left || pos == JDropPos::Top || pos == JDropPos::SplitBefore);

        JDockNode* parent = node(parentId);
        if (!parent) return InvalidDockNodeId;

        int idx = -1;
        for (int i = 0; i < static_cast<int>(parent->children.size()); ++i)
            if (parent->children[i] == leafId) { idx = i; break; }
        if (idx < 0) return InvalidDockNodeId;

        // A dock re-docked via an edge split keeps its RETAINED size: the new leaf is
        // pinned to the dragged dock's pixel dimension along the split axis (a fixed-size
        // leaf), so it reserves its own space and the target keeps its size instead of a
        // 50/50 carve-up. This also makes splitter drags between the resulting docks behave
        // (both sides are fixed-px). `frac` is the weighted-split fallback for programmatic
        // splits with no active drag. Captured before any _allocNode invalidates pointers.
        float dockDim  = m_draggedDock ? (wantVertical ? m_draggedDock->height()
                                                        : m_draggedDock->width()) : 0.f;
        // Clamp the dropped dock's size so the target keeps at least its min — otherwise a
        // dock as large as the target takes everything and squishes the target to zero.
        if (m_draggedDock) {
            const float tdim = wantVertical ? leaf->rect.height : leaf->rect.width;
            const float tmin = wantVertical ? _minHeightOfNode(leafId) : _minWidthOfNode(leafId);
            const float dmin = wantVertical ? m_draggedDock->minH() : m_draggedDock->minW();
            if (tdim - tmin > dmin) dockDim = std::clamp(dockDim, dmin, tdim - tmin);
        }
        bool  fixedNew = (dockDim > 0.f);
        // Target's fixed sizes, captured before any _allocNode invalidates `leaf` — a
        // Case B split must inherit the fixed size on the axis perpendicular to the split
        // so a fixed-width column stays fixed-width when stacked top/bottom.
        float origPrefW = leaf->constraints.preferredW;
        float origPrefH = leaf->constraints.preferredH;
        float frac     = 0.5f;
        if (fixedNew) {
            float targetDim = wantVertical ? leaf->rect.height : leaf->rect.width;
            if (targetDim > 0.f) frac = std::clamp(dockDim / targetDim, 0.1f, 0.9f);
        }
        auto pinNewLeaf = [&](JDockNode& nl) {
            if (!fixedNew) return;
            if (desired == JSplitDir::Horizontal) nl.constraints.preferredW = dockDim;
            else                                  nl.constraints.preferredH = dockDim;
        };

        // The split target flexes along the split axis so the dropped (fixed-size) dock
        // keeps its exact size and the two together fill the region — the dropped dock's
        // size decides where it lands, the target absorbs the rest (no gap, no 50/50).
        if (fixedNew) {
            if (desired == JSplitDir::Horizontal) leaf->constraints.preferredW = 0.f;
            else                                  leaf->constraints.preferredH = 0.f;
        }

        // Case A: parent already splits along the desired direction — just
        // insert the new leaf as a sibling next to the target.
        if (parent->splitDir == desired) {
            JDockNodeId newLeaf = _allocNode();
            { JDockNode& nl = m_nodes[newLeaf.v]; nl.type = JDockNode::JType::Leaf; nl.parent = parentId; pinNewLeaf(nl); }
            parent = node(parentId);
            float w = parent->weights[idx];
            int insertAt = before ? idx : idx + 1;
            if (fixedNew) {
                // Fixed-px new leaf reserves its own pixels — target keeps its weight.
                parent->children.insert(parent->children.begin() + insertAt, newLeaf);
                parent->weights.insert(parent->weights.begin() + insertAt, w);
            } else {
                float newW = w * frac;
                parent->weights[idx] = w - newW;
                parent->children.insert(parent->children.begin() + insertAt, newLeaf);
                parent->weights.insert(parent->weights.begin() + insertAt, newW);
            }
            _rebalanceWeights(parentId);
            return newLeaf;
        }

        // Case B: introduce a new SplitNode in the target's slot, holding the
        // original leaf and the new leaf.
        JDockNodeId splitId = _allocNode();
        { JDockNode& sp = m_nodes[splitId.v]; sp.type = JDockNode::JType::Split; sp.parent = parentId; sp.splitDir = desired;
          // Keep the target's fixed size on the perpendicular axis (column width stays
          // fixed when split into top/bottom; row height when split left/right).
          if (desired == JSplitDir::Vertical) sp.constraints.preferredW = origPrefW;
          else                                sp.constraints.preferredH = origPrefH; }
        JDockNodeId newLeaf = _allocNode();
        { JDockNode& nl = m_nodes[newLeaf.v]; nl.type = JDockNode::JType::Leaf; nl.parent = splitId; pinNewLeaf(nl); }

        // Reparent the original leaf under the new split.
        leaf = node(leafId);
        leaf->parent = splitId;

        JDockNode& sp = m_nodes[splitId.v];
        if (before) { sp.children = { newLeaf, leafId }; sp.weights = { frac, 1.f - frac }; }
        else        { sp.children = { leafId, newLeaf }; sp.weights = { 1.f - frac, frac }; }

        // Swap the split node into the slot the leaf occupied in its parent.
        parent = node(parentId);
        parent->children[idx] = splitId;
        return newLeaf;
    }

    void _endDrag() {
        if (m_hasSavedState) {
            m_nodes = m_savedNodes;
            m_nextId = m_savedNextId;
            m_hasSavedState = false;
            computeLayout(m_hostRect);
        }
        m_dropTargets.clear();
        m_activeTarget = nullptr;
        m_draggedDock  = nullptr;
    }

    // ----------------------------------------------------------------------
    // Snapshot / restore helpers
    // ----------------------------------------------------------------------

    JDockLayoutNode _snapshotNode(JDockNodeId id) const {
        JDockLayoutNode out;
        const JDockNode* n = node(id);
        if (!n) return out;
        if (n->type == JDockNode::JType::Split) {
            out.kind     = JDockLayoutNode::JKind::Split;
            out.splitDir = n->splitDir;
            out.weights  = n->weights;
            for (JDockNodeId c : n->children)
                out.children.push_back(_snapshotNode(c));
        } else {
            out.kind       = JDockLayoutNode::JKind::Leaf;
            out.label      = n->label;
            out.tabBarEdge = n->tabBarEdge;
            out.activeTab  = n->activeTab;
            for (const JDockWidget* w : n->tabs)
                out.tabTitles.push_back(w ? w->title() : "");
        }
        return out;
    }

    bool _restoreNode(const JDockLayoutNode& src, JDockNodeId id, JDockNodeId parentId,
                      const std::function<JDockWidget*(std::string_view)>& resolver)
    {
        JDockNode* n = node(id);
        if (!n) return false;
        n->parent = parentId;

        if (src.kind == JDockLayoutNode::JKind::Split) {
            n->type     = JDockNode::JType::Split;
            n->splitDir = src.splitDir;
            n->weights  = src.weights;

            for (size_t i = 0; i < src.children.size(); ++i) {
                JDockNodeId childId = _allocNode();
                n = node(id);            // refetch: _allocNode may reallocate m_nodes
                if (!n) return false;
                n->children.push_back(childId);
                if (!_restoreNode(src.children[i], childId, id, resolver))
                    return false;
                // n is potentially stale after recursion; refetched at top of next iteration
            }
            _rebalanceWeights(id);  // uses node(id) internally, always safe
        } else {
            n->type       = JDockNode::JType::Leaf;
            n->label      = src.label;
            n->tabBarEdge = src.tabBarEdge;
            for (const auto& title : src.tabTitles) {
                if (JDockWidget* w = resolver ? resolver(title) : nullptr)
                    n->tabs.push_back(w);
            }
            int tabCount = static_cast<int>(n->tabs.size());
            n->activeTab = (src.activeTab < tabCount)
                               ? src.activeTab
                               : std::max(0, tabCount - 1);
        }
        return true;
    }

    // ----------------------------------------------------------------------
    // Members
    // ----------------------------------------------------------------------

    std::vector<JDockNode> m_nodes;
    uint32_t              m_nextId{0};
    JRect                  m_hostRect{};

    std::vector<JDropTarget> m_dropTargets;
    JDropTarget*             m_activeTarget{nullptr};
    JDockWidget*             m_draggedDock{nullptr};

    std::optional<JTabBarEdge> m_tabEdge;   // unset = inherit style().tabEdge
    std::optional<JTabFill>    m_tabFill;   // unset = inherit style().tabFill
    std::vector<JDockNode>   m_savedNodes;
    uint32_t                m_savedNextId{0};
    bool                    m_hasSavedState{false};
    bool                    m_livePreviewEnabled{true};

    struct JHandleDrag {
        JDockNodeId parentSplit;
        int        handleIdx{0};
        float      startCursor{0.f};
        float      startWeightA{0.f}, startWeightB{0.f};
        float      startPrefA{0.f},   startPrefB{0.f};   // fixed-size leaves on each side
        bool       active{false};
    } m_handleDrag{};

    struct JTitleDrag {
        JDockWidget* dock{nullptr};
        float       startX{0.f}, startY{0.f};
        bool        active{false};
    } m_titleDrag{};

    JDockOptions m_options;
};

} // inline namespace jf
