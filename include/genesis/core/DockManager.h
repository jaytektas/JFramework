#pragma once

// ============================================================================
// DockManager — the dock zone management system for Genesis.
//
// The dock layout is a TREE of DockNodes stored in a flat arena inside
// DockHost.  Every node is either a SplitNode (divides space between N
// children along H or V with per-child weights) or a TabLeaf (holds 1..N
// DockWidget pointers as a tab group).
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

namespace Genesis {

// ----------------------------------------------------------------------------
// Public enums / value types
// ----------------------------------------------------------------------------

enum class SplitDir : uint8_t { Horizontal, Vertical };

enum class TabBarEdge : uint8_t { Top, Bottom, Left, Right };

// Drop placement relative to a node.
enum class DropPos : uint8_t {
    Center,                       // tabify with the target leaf
    Left, Right, Top, Bottom,     // split the target, new dock on that side
    SplitBefore, SplitAfter       // insert before/after in parent split
};

struct DockNodeId {
    uint32_t v{UINT32_MAX};
    bool valid() const { return v != UINT32_MAX; }
    bool operator==(const DockNodeId& o) const { return v == o.v; }
    bool operator!=(const DockNodeId& o) const { return v != o.v; }
};
static constexpr DockNodeId InvalidDockNodeId{};

struct DockConstraints {
    float minW{48.f}, minH{48.f};
    float maxW{1e9f}, maxH{1e9f};
    float preferredW{0.f}, preferredH{0.f};  // 0 = no preference
};

struct DockAffinityRule {
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
// DockNode — a node in the tree, stored in DockHost's flat arena.
// ----------------------------------------------------------------------------

struct DockNode {
    enum class Type : uint8_t { Split, Leaf } type{Type::Split};
    DockNodeId id;
    DockNodeId parent;

    // Split fields (type == Split)
    SplitDir                 splitDir{SplitDir::Horizontal};
    std::vector<DockNodeId>  children;
    std::vector<float>       weights;   // one per child, sum == 1.0

    // Leaf fields (type == Leaf)
    std::vector<DockWidget*> tabs;      // non-owning
    int                      activeTab{0};
    TabBarEdge               tabBarEdge{TabBarEdge::Top};
    DockAffinityRule         affinity;
    DockConstraints          constraints;
    std::string              label;     // optional zone name

    // Runtime — computed each frame by DockHost::computeLayout
    Rect                     rect{};

    // Runtime drag handle rects (SplitNodes: one per gap between children)
    std::vector<Rect>        handleRects;  // count == children.size() - 1
};

// ============================================================================
// DockLayoutNode — one node in a serializable snapshot of the dock tree.
// ============================================================================

struct DockLayoutNode {
    enum class Kind : uint8_t { Split, Leaf } kind{Kind::Leaf};

    // Split
    SplitDir                    splitDir{SplitDir::Horizontal};
    std::vector<float>          weights;
    std::vector<DockLayoutNode> children;

    // Leaf
    std::string              label;
    TabBarEdge               tabBarEdge{TabBarEdge::Top};
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

inline const char* edgeStr(TabBarEdge e) noexcept {
    switch (e) {
        case TabBarEdge::Top:    return "top";
        case TabBarEdge::Bottom: return "bottom";
        case TabBarEdge::Left:   return "left";
        case TabBarEdge::Right:  return "right";
    }
    return "top";
}

inline TabBarEdge parseEdge(const std::string& s) noexcept {
    if (s == "bottom") return TabBarEdge::Bottom;
    if (s == "left")   return TabBarEdge::Left;
    if (s == "right")  return TabBarEdge::Right;
    return TabBarEdge::Top;
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

inline void serializeNode(const DockLayoutNode& n, std::string& out) {
    if (n.kind == DockLayoutNode::Kind::Split) {
        out += "split ";
        out += (n.splitDir == SplitDir::Horizontal ? 'H' : 'V');
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

struct Tok {
    const char* begin{nullptr};
    size_t      len{0};
    std::string str() const { return {begin, len}; }
};
using Lines = std::vector<std::pair<const char*, size_t>>;

inline std::vector<Tok> tokenizeLine(const char* p, size_t len) {
    std::vector<Tok> toks;
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

inline bool parseNode(const Lines& lines, size_t& idx, DockLayoutNode& out) {
    if (idx >= lines.size()) return false;
    auto toks = tokenizeLine(lines[idx].first, lines[idx].second);
    ++idx;
    if (toks.empty()) return false;

    const std::string kw = toks[0].str();

    if (kw == "split") {
        if (toks.size() < 3) return false;
        out.kind     = DockLayoutNode::Kind::Split;
        out.splitDir = (toks[1].str() == "H") ? SplitDir::Horizontal : SplitDir::Vertical;
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
            DockLayoutNode child;
            if (!parseNode(lines, idx, child)) return false;
            out.children.push_back(std::move(child));
        }
    } else if (kw == "leaf") {
        if (toks.size() < 5) return false;
        out.kind = DockLayoutNode::Kind::Leaf;
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
// DockLayoutSnapshot — complete serializable snapshot of a DockHost tree.
//
//   DockLayoutSnapshot snap = host.snapshot();
//   std::string text = snap.toText();                  // save to file
//
//   auto snap2 = DockLayoutSnapshot::fromText(text);
//   if (snap2) host.restore(*snap2, [&](std::string_view title) -> DockWidget* { ... });
// ============================================================================

struct DockLayoutSnapshot {
    DockLayoutNode root;

    std::string toText() const {
        std::string out = "genesis_dock_layout 1\n";
        dock_detail::serializeNode(root, out);
        return out;
    }

    static std::optional<DockLayoutSnapshot> fromText(std::string_view text) {
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

        DockLayoutSnapshot snap;
        if (!dock_detail::parseNode(lines, idx, snap.root)) return std::nullopt;
        return snap;
    }
};

// ----------------------------------------------------------------------------
// DockHost — the façade owning the tree.
// ----------------------------------------------------------------------------

class DockHost {
public:
    static constexpr float TAB_BAR_SZ   = 28.0f;  // tab bar / title bar thickness
    static constexpr float HANDLE_HALF  = 3.0f;   // half the 6px split handle hit width
    static constexpr float HANDLE_VIS   = 2.0f;   // visible divider line thickness
    static constexpr float ARROW_SZ     = 32.0f;  // drop indicator icon size
    static constexpr float BTN_SZ       = 14.0f;  // close button size on tabs

    float minWidthNeeded() const {
        if (m_nodes.empty()) return 48.f;
        return _minWidthOfNode(rootId());
    }

    float minHeightNeeded() const {
        if (m_nodes.empty()) return 48.f;
        return _minHeightOfNode(rootId());
    }

    // --- Construction ---

    DockHost() { _allocNode(); }  // root SplitNode(Horizontal)

    DockNodeId rootId() const { return DockNodeId{0}; }

    void setRootSplit(SplitDir dir) {
        if (!m_nodes.empty()) m_nodes[0].splitDir = dir;
    }

    DockNodeId addLeaf(DockNodeId parentId,
                       std::string label = "",
                       float weight = 0.f,
                       DockAffinityRule affinity = {},
                       DockConstraints constraints = {})
    {
        DockNode* parent = node(parentId);
        if (!parent || parent->type != DockNode::Type::Split) return InvalidDockNodeId;

        DockNodeId id = _allocNode();
        DockNode& leaf = m_nodes[id.v];
        leaf.type        = DockNode::Type::Leaf;
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

    DockNodeId addSplit(DockNodeId parentId, SplitDir dir, float weight = 0.f) {
        DockNode* parent = node(parentId);
        if (!parent || parent->type != DockNode::Type::Split) return InvalidDockNodeId;

        DockNodeId id = _allocNode();
        DockNode& sp = m_nodes[id.v];
        sp.type     = DockNode::Type::Split;
        sp.parent   = parentId;
        sp.splitDir = dir;

        parent = node(parentId);
        parent->children.push_back(id);
        parent->weights.push_back(weight > 0.f ? weight : 0.f);
        _rebalanceWeights(parentId);
        return id;
    }

    bool insertDock(DockWidget* dock, DockNodeId leafId, int tabIdx = -1) {
        DockNode* leaf = node(leafId);
        if (!dock || !leaf || leaf->type != DockNode::Type::Leaf) return false;
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

    void removeDock(DockWidget* dock) {
        for (auto& n : m_nodes) {
            if (n.type != DockNode::Type::Leaf) continue;
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
    // (e.g. a FloatingDock's DockWidget is moved into heap-owned storage after
    // a drop has already inserted the old address into the tree).
    void retargetDock(DockWidget* oldPtr, DockWidget* newPtr) {
        for (auto& n : m_nodes) {
            if (n.type != DockNode::Type::Leaf) continue;
            for (auto& t : n.tabs)
                if (t == oldPtr) { t = newPtr; return; }
        }
    }

    DockNodeId findDock(const DockWidget* dock) const {
        for (auto& n : m_nodes) {
            if (n.type != DockNode::Type::Leaf) continue;
            if (std::find(n.tabs.begin(), n.tabs.end(), dock) != n.tabs.end())
                return n.id;
        }
        return InvalidDockNodeId;
    }

    // --- Layout ---

    void computeLayout(Rect hostRect) {
        m_hostRect = hostRect;
        if (!m_nodes.empty()) _computeNodeLayout(rootId(), hostRect);
    }

    void setLivePreviewEnabled(bool enabled) { m_livePreviewEnabled = enabled; }
    bool isLivePreviewEnabled() const { return m_livePreviewEnabled; }

    // --- Per-frame drag tracking ---

    void updateDrag(float /*cursorWindowX*/, float /*cursorWindowY*/,
                    float cursorScreenX, float cursorScreenY,
                    int   hostScreenX,   int   hostScreenY,
                    DockWidget* draggedDock)
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
            if (n.type != DockNode::Type::Leaf) continue;
            const Rect& r = n.rect;
            if (localX < r.x || localX > r.x + r.width ||
                localY < r.y || localY > r.y + r.height) continue;

            float rx = (r.width  > 0.f) ? (localX - r.x) / r.width  : 0.5f;
            float ry = (r.height > 0.f) ? (localY - r.y) / r.height : 0.5f;
            constexpr float kEdge = 0.25f;

            DropPos zone;
            if      (rx < kEdge)        zone = DropPos::Left;
            else if (rx > 1.f - kEdge)  zone = DropPos::Right;
            else if (ry < kEdge)        zone = DropPos::Top;
            else if (ry > 1.f - kEdge)  zone = DropPos::Bottom;
            else                        zone = DropPos::Center;

            for (auto& dt : m_dropTargets) {
                if (dt.leaf == n.id && dt.pos == zone && dt.allowed) {
                    dt.highlighted = true;
                    m_activeTarget = &dt;
                    break;
                }
            }
            break;  // leaves don't overlap — stop after first match
        }

        if (m_livePreviewEnabled && m_activeTarget && m_activeTarget->allowed && m_activeTarget->pos != DropPos::Center) {
            _splitLeaf(m_activeTarget->leaf, m_activeTarget->pos);
            _computeNodeLayout(rootId(), m_hostRect);
        } else {
            _computeNodeLayout(rootId(), m_hostRect);
        }
    }

    struct DropResult { DockNodeId targetLeaf; DropPos pos; };

    std::optional<DropResult> tryCommitDrop() {
        if (!m_activeTarget || !m_activeTarget->allowed || !m_draggedDock) {
            _endDrag();
            return std::nullopt;
        }

        DockNodeId leafId = m_activeTarget->leaf;
        DropPos    pos    = m_activeTarget->pos;
        DockWidget* dock  = m_draggedDock;

        if (m_hasSavedState) {
            m_nodes = m_savedNodes;
            m_nextId = m_savedNextId;
            m_hasSavedState = false;
        }

        DockNode* leaf = node(leafId);
        if (!leaf || leaf->type != DockNode::Type::Leaf) { _endDrag(); return std::nullopt; }

        if (pos == DropPos::Center) {
            insertDock(dock, leafId);
        } else {
            DockNodeId newLeaf = _splitLeaf(leafId, pos);
            if (!newLeaf.valid()) { _endDrag(); return std::nullopt; }
            insertDock(dock, newLeaf);
        }

        computeLayout(m_hostRect);
        DropResult result{leafId, pos};
        _endDrag();
        return result;
    }

    // --- Inline mouse routing ---

    struct DockEvent {
        DockWidget* dock{nullptr};
        enum class Type { WantsFloat, CloseRequested } type{Type::WantsFloat};
    };

    std::optional<DockEvent> handleMouse(float mx, float my,
                                         bool pressed, bool released)
    {
        // 1. Resize handles take priority.
        if (m_handleDrag.active) {
            DockNode* sp = node(m_handleDrag.parentSplit);
            if (sp && sp->type == DockNode::Type::Split) {
                int a = m_handleDrag.handleIdx;
                int b = a + 1;
                if (b < static_cast<int>(sp->weights.size())) {
                    float cursor = (sp->splitDir == SplitDir::Horizontal) ? mx : my;
                    float total  = (sp->splitDir == SplitDir::Horizontal)
                                       ? sp->rect.width : sp->rect.height;
                    float delta  = (total > 1.f)
                                       ? (cursor - m_handleDrag.startCursor) / total : 0.f;
                    float wa = m_handleDrag.startWeightA + delta;
                    float wb = m_handleDrag.startWeightB - delta;
                    // Keep both children above their minimum sizes.
                    float sum = wa + wb;
                    float minFrac_a = 0.05f;
                    float minFrac_b = 0.05f;
                    bool horiz = (sp->splitDir == SplitDir::Horizontal);
                    float totalDim = horiz ? sp->rect.width : sp->rect.height;
                    float handleSpace = HANDLE_HALF * 2.0f;
                    float usable = std::max(0.f, totalDim - handleSpace * static_cast<float>(sp->children.size() - 1));
                    if (usable > 0.f) {
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
                if (n.type != DockNode::Type::Split) continue;
                for (int i = 0; i < static_cast<int>(n.handleRects.size()); ++i) {
                    const Rect& h = n.handleRects[i];
                    if (mx >= h.x && mx <= h.x + h.width &&
                        my >= h.y && my <= h.y + h.height)
                    {
                        m_handleDrag.parentSplit = n.id;
                        m_handleDrag.handleIdx   = i;
                        m_handleDrag.active      = true;
                        m_handleDrag.startCursor =
                            (n.splitDir == SplitDir::Horizontal) ? mx : my;
                        m_handleDrag.startWeightA = n.weights[i];
                        m_handleDrag.startWeightB = n.weights[i + 1];
                        return std::nullopt;
                    }
                }
            }
        }

        // 2. Leaf chrome — close button + title-bar drag-out.
        for (auto& n : m_nodes) {
            if (n.type != DockNode::Type::Leaf || n.tabs.empty()) continue;

            Rect barRect = _tabBarRect(n);
            DockWidget* active =
                (n.activeTab >= 0 && n.activeTab < static_cast<int>(n.tabs.size()))
                    ? n.tabs[n.activeTab] : nullptr;
            if (!active) continue;

            // Close button on the active tab.
            Rect closeR = _closeBtnRect(n);
            if (pressed && _inRect(closeR, mx, my))
                return DockEvent{active, DockEvent::Type::CloseRequested};

            // Title/tab bar drag → float.
            if (pressed && _inRect(barRect, mx, my) && !_inRect(closeR, mx, my)) {
                m_titleDrag.active = true;
                m_titleDrag.dock   = active;
                m_titleDrag.startX = mx;
                m_titleDrag.startY = my;
            }
        }

        if (m_titleDrag.active) {
            float dx = mx - m_titleDrag.startX;
            float dy = my - m_titleDrag.startY;
            // A title drag past the threshold becomes a float request.
            if (std::sqrt(dx * dx + dy * dy) > 16.0f) {
                DockWidget* d = m_titleDrag.dock;
                m_titleDrag = {};
                if (d->isFloatable())
                    return DockEvent{d, DockEvent::Type::WantsFloat};
                // Non-floatable: silently cancel the drag.
            }
            if (released) m_titleDrag = {};
        }

        // 3. Route tab clicks to switch the active tab.
        if (pressed) {
            for (auto& n : m_nodes) {
                if (n.type != DockNode::Type::Leaf || n.tabs.size() < 2) continue;
                Rect bar = _tabBarRect(n);
                if (!_inRect(bar, mx, my)) continue;
                float tabW = bar.width / static_cast<float>(n.tabs.size());
                int idx = std::clamp(static_cast<int>((mx - bar.x) / tabW),
                                     0, static_cast<int>(n.tabs.size()) - 1);
                n.activeTab = idx;
            }
        }

        return std::nullopt;
    }

    // --- Rendering ---

    void populateRenderPrimitives(PrimitiveBuffer& buf) const {
        if (m_nodes.empty()) return;
        _renderNode(rootId(), buf);
    }

    void populateOverlay(PrimitiveBuffer& buf) const {
        for (auto& dt : m_dropTargets) _renderDropTarget(dt, buf);

        // Highlight an active resize handle.
        if (m_handleDrag.active) {
            const DockNode* sp = node(m_handleDrag.parentSplit);
            if (sp && m_handleDrag.handleIdx <
                          static_cast<int>(sp->handleRects.size()))
            {
                const Rect& h = sp->handleRects[m_handleDrag.handleIdx];
                buf.pushRectangle(h.x, h.y, h.width, h.height, Colors::Accent, 2.0f);
            }
        }
    }

    // --- Accessors ---

    DockNode* node(DockNodeId id) {
        return id.valid() && id.v < m_nodes.size() ? &m_nodes[id.v] : nullptr;
    }
    const DockNode* node(DockNodeId id) const {
        return id.valid() && id.v < m_nodes.size() ? &m_nodes[id.v] : nullptr;
    }

    size_t dockCount() const {
        size_t count = 0;
        for (const auto& n : m_nodes) {
            if (n.type == DockNode::Type::Leaf) {
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
            if (n.type != DockNode::Type::Leaf) continue;
            for (int i = 0; i < static_cast<int>(n.tabs.size()); ++i)
                if (n.tabs[i]) fn(n.tabs[i], n.rect, i == n.activeTab,
                                  static_cast<int>(n.tabs.size()));
        }
    }

    // Activate (bring to front) a docked panel by title.  Returns true if found.
    bool activatePanelByTitle(const std::string& title) {
        for (auto& n : m_nodes) {
            if (n.type != DockNode::Type::Leaf) continue;
            for (int i = 0; i < static_cast<int>(n.tabs.size()); ++i)
                if (n.tabs[i] && n.tabs[i]->title() == title) { n.activeTab = i; return true; }
        }
        return false;
    }

    Rect contentArea(DockNodeId leafId) const {
        const DockNode* leaf = node(leafId);
        return leaf ? contentArea(leafId, leaf->activeTab) : Rect{};
    }

    Rect contentArea(DockNodeId leafId, int /*tabIdx*/) const {
        const DockNode* leaf = node(leafId);
        if (!leaf || leaf->type != DockNode::Type::Leaf) return Rect{};
        return _leafContentRect(*leaf);
    }

    // --- Layout snapshot / restore ---

    // Capture the current tree as a serializable value.
    DockLayoutSnapshot snapshot() const {
        DockLayoutSnapshot snap;
        if (!m_nodes.empty()) snap.root = _snapshotNode(rootId());
        return snap;
    }

    // Rebuild the tree from a snapshot.
    // `resolver(title)` must return the DockWidget* to insert, or nullptr to skip.
    // The host rect is NOT re-applied — call computeLayout() after restore().
    bool restore(const DockLayoutSnapshot& snap,
                 const std::function<DockWidget*(std::string_view)>& resolver)
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

private:
    // ----------------------------------------------------------------------
    // Drop target state — declared before any use in member functions.
    // ----------------------------------------------------------------------

    struct DropTarget {
        DockNodeId leaf;
        DropPos    pos;
        Rect       indicatorRect;
        Rect       previewRect;
        bool       highlighted{false};
        bool       allowed{false};
    };

    // ----------------------------------------------------------------------
    // Arena management
    // ----------------------------------------------------------------------

    float _minWidthOfNode(DockNodeId id) const {
        const DockNode* n = node(id);
        if (!n) return 48.f;
        if (n->type == DockNode::Type::Leaf) {
            int tabCount = static_cast<int>(n->tabs.size());
            if (tabCount == 0) return 48.f;
            if (tabCount == 1) {
                float lw = TextHelper::hasAtlas() ? TextHelper::measureWidth(n->tabs[0]->title()) : 50.f;
                return lw + 50.f;
            }
            float maxLw = 0.f;
            for (int i = 0; i < tabCount; ++i) {
                float lw = TextHelper::hasAtlas() ? TextHelper::measureWidth(n->tabs[i]->title()) : 50.f;
                if (lw > maxLw) maxLw = lw;
            }
            return static_cast<float>(tabCount) * (maxLw + 16.f) + 30.f;
        }
        bool horiz = (n->splitDir == SplitDir::Horizontal);
        float sum = 0.f;
        float maxVal = 0.f;
        for (DockNodeId childId : n->children) {
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

    float _minHeightOfNode(DockNodeId id) const {
        const DockNode* n = node(id);
        if (!n) return 48.f;
        if (n->type == DockNode::Type::Leaf) {
            return TAB_BAR_SZ + 40.f;
        }
        bool horiz = (n->splitDir == SplitDir::Horizontal);
        float sum = 0.f;
        float maxVal = 0.f;
        for (DockNodeId childId : n->children) {
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

    DockNodeId _allocNode() {
        DockNodeId id{m_nextId++};
        DockNode n;
        n.id = id;
        m_nodes.push_back(std::move(n));
        return id;
    }

    void _rebalanceWeights(DockNodeId parentId) {
        DockNode* p = node(parentId);
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

    void _computeNodeLayout(DockNodeId id, Rect rect) {
        DockNode* n = node(id);
        if (!n) return;
        n->rect = rect;

        if (n->type == DockNode::Type::Leaf) {
            n->handleRects.clear();
            return;
        }

        // Split: divide rect along splitDir by weights.
        size_t count = n->children.size();
        n->handleRects.clear();
        if (count == 0) return;
        if (n->weights.size() != count) _rebalanceWeights(id);

        bool horiz = (n->splitDir == SplitDir::Horizontal);
        float total = horiz ? rect.width : rect.height;

        // Reserve space for the (count-1) handles so children never overlap them.
        float handleSpace = HANDLE_HALF * 2.0f;
        float usable = std::max(0.f, total - handleSpace * static_cast<float>(count - 1));

        // Snapshot weights/children: child layout may reallocate the arena.
        std::vector<DockNodeId> children = n->children;
        std::vector<float>      weights  = n->weights;
        std::vector<Rect>       handles;
        handles.reserve(count > 0 ? count - 1 : 0);

        float cursor = horiz ? rect.x : rect.y;
        for (size_t i = 0; i < count; ++i) {
            float seg = usable * weights[i];
            Rect childRect;
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

    Rect _tabBarRect(const DockNode& leaf) const {
        const Rect& r = leaf.rect;
        switch (leaf.tabBarEdge) {
            case TabBarEdge::Top:    return { r.x, r.y, r.width, TAB_BAR_SZ };
            case TabBarEdge::Bottom: return { r.x, r.y + r.height - TAB_BAR_SZ, r.width, TAB_BAR_SZ };
            case TabBarEdge::Left:   return { r.x, r.y, TAB_BAR_SZ, r.height };
            case TabBarEdge::Right:  return { r.x + r.width - TAB_BAR_SZ, r.y, TAB_BAR_SZ, r.height };
        }
        return { r.x, r.y, r.width, TAB_BAR_SZ };
    }

    Rect _leafContentRect(const DockNode& leaf) const {
        const Rect& r = leaf.rect;
        switch (leaf.tabBarEdge) {
            case TabBarEdge::Top:    return { r.x, r.y + TAB_BAR_SZ, r.width, r.height - TAB_BAR_SZ };
            case TabBarEdge::Bottom: return { r.x, r.y, r.width, r.height - TAB_BAR_SZ };
            case TabBarEdge::Left:   return { r.x + TAB_BAR_SZ, r.y, r.width - TAB_BAR_SZ, r.height };
            case TabBarEdge::Right:  return { r.x, r.y, r.width - TAB_BAR_SZ, r.height };
        }
        return { r.x, r.y + TAB_BAR_SZ, r.width, r.height - TAB_BAR_SZ };
    }

    // Close button sits at the right edge of the (top) tab bar.
    Rect _closeBtnRect(const DockNode& leaf) const {
        Rect bar = _tabBarRect(leaf);
        float pad = (TAB_BAR_SZ - BTN_SZ) * 0.5f;
        return { bar.x + bar.width - BTN_SZ - pad,
                 bar.y + (bar.height - BTN_SZ) * 0.5f,
                 BTN_SZ, BTN_SZ };
    }

    static bool _inRect(const Rect& r, float x, float y) {
        return x >= r.x && x <= r.x + r.width &&
               y >= r.y && y <= r.y + r.height;
    }

    // ----------------------------------------------------------------------
    // Rendering
    // ----------------------------------------------------------------------

    void _renderNode(DockNodeId id, PrimitiveBuffer& buf) const {
        const DockNode* n = node(id);
        if (!n) return;
        if (n->type == DockNode::Type::Leaf) {
            _renderLeaf(*n, buf);
        } else {
            for (DockNodeId c : n->children) _renderNode(c, buf);
            _renderSplitHandles(*n, buf);
        }
    }

    void _renderLeaf(const DockNode& leaf, PrimitiveBuffer& buf) const {
        const Rect& r = leaf.rect;
        if (r.width < 1.f || r.height < 1.f) return;

        // Body background.
        uint8_t body[4] = {22, 22, 26, 255};
        buf.pushRectangle(r.x, r.y, r.width, r.height, body, 0.0f, 1.0f, Colors::Border);

        // Content area (slightly darker than the body).
        Rect content = _leafContentRect(leaf);
        uint8_t contentBg[4] = {14, 14, 16, 255};
        buf.pushRectangle(content.x, content.y, content.width, content.height, contentBg, 0.0f);

        // Tab / title bar.
        Rect bar = _tabBarRect(leaf);
        buf.pushRectangle(bar.x, bar.y, bar.width, bar.height, Colors::Surface1, 0.0f);

        int tabCount = static_cast<int>(leaf.tabs.size());
        if (tabCount == 0) return;

        if (tabCount == 1) {
            // Single dock — render a title bar like DockWidget's.
            buf.pushRectangle(bar.x + 1.f, bar.y + 1.f, bar.width - 2.f, bar.height - 2.f,
                              Colors::Surface2, 0.0f);
            if (TextHelper::hasAtlas()) {
                uint8_t tc[4] = {210, 210, 220, 220};
                float ty = bar.y + (bar.height - TextHelper::lineHeight()) * 0.5f;
                TextHelper::pushText(buf, bar.x + 10.f, ty,
                                     leaf.tabs[0]->title(), tc, bar.width - BTN_SZ - 24.f);
            }
        } else {
            float tabW = bar.width / static_cast<float>(tabCount);
            for (int i = 0; i < tabCount; ++i) {
                float tx = bar.x + i * tabW;
                bool active = (i == leaf.activeTab);
                const uint8_t* fill = active ? Colors::Surface3 : Colors::Surface1;
                buf.pushRectangle(tx + 1.f, bar.y + 1.f, tabW - 2.f, bar.height - 2.f, fill, 0.0f);
                if (active)
                    buf.pushRectangle(tx + 4.f, bar.y + bar.height - 3.f,
                                      tabW - 8.f, 2.5f, Colors::Accent, 1.0f);
                if (TextHelper::hasAtlas()) {
                    std::string label = leaf.tabs[i]->title();
                    float lw = TextHelper::measureWidth(label);
                    uint8_t lc[4] = {active ? (uint8_t)220 : (uint8_t)150,
                                     active ? (uint8_t)220 : (uint8_t)150,
                                     active ? (uint8_t)228 : (uint8_t)158,
                                     active ? (uint8_t)220 : (uint8_t)150};
                    float ly = bar.y + (bar.height - TextHelper::lineHeight()) * 0.5f;
                    TextHelper::pushText(buf, tx + std::max(6.f, (tabW - lw) * 0.5f), ly,
                                         label, lc, tabW - 10.f);
                }
            }
        }

        // Close button on the active tab / single title bar.
        Rect closeR = _closeBtnRect(leaf);
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
            const DockWidget* d = leaf.tabs[0];
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

    void _renderSplitHandles(const DockNode& split, PrimitiveBuffer& buf) const {
        bool horiz = (split.splitDir == SplitDir::Horizontal);
        for (const Rect& h : split.handleRects) {
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

    void _renderDropTarget(const DropTarget& dt, PrimitiveBuffer& buf) const {
        if (!dt.allowed) return;
        const Rect& a = dt.indicatorRect;

        if (dt.highlighted) {
            // Semi-transparent preview of where the dock would land.
            uint8_t preview[4] = {10, 132, 255, 70};
            uint8_t pBorder[4] = {10, 132, 255, 200};
            buf.pushRectangle(dt.previewRect.x, dt.previewRect.y,
                              dt.previewRect.width, dt.previewRect.height,
                              preview, 4.0f, 2.0f, pBorder);
            // Bright accent arrow background.
            uint8_t bg[4] = {10, 132, 255, 235};
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
    void _renderArrow(const DropTarget& dt, PrimitiveBuffer& buf, bool white) const {
        const Rect& a = dt.indicatorRect;
        uint8_t c[4] = { white ? (uint8_t)255 : (uint8_t)180,
                         white ? (uint8_t)255 : (uint8_t)180,
                         white ? (uint8_t)255 : (uint8_t)190,
                         white ? (uint8_t)235 : (uint8_t)200 };
        float cx = a.x + a.width * 0.5f;
        float cy = a.y + a.height * 0.5f;
        float arm = a.width * 0.22f;
        float th  = 3.0f;

        if (dt.pos == DropPos::Center) {
            // A small square outline to denote "tabify here".
            uint8_t sq[4] = {c[0], c[1], c[2], c[3]};
            float s = a.width * 0.34f;
            buf.pushRectangle(cx - s * 0.5f, cy - s * 0.5f, s, s, Colors::Transparent, 0.0f, 2.0f, sq);
            return;
        }

        switch (dt.pos) {
            case DropPos::Left:
            case DropPos::SplitBefore:
                buf.pushRectangle(cx - arm * 0.4f, cy - arm, th, arm + th, c, 1.0f);
                buf.pushRectangle(cx - arm * 0.4f, cy,       th, arm,      c, 1.0f);
                buf.pushRectangle(cx - arm,        cy - th * 0.5f, arm, th, c, 1.0f);
                break;
            case DropPos::Right:
            case DropPos::SplitAfter:
                buf.pushRectangle(cx + arm * 0.4f - th, cy - arm, th, arm + th, c, 1.0f);
                buf.pushRectangle(cx + arm * 0.4f - th, cy,       th, arm,      c, 1.0f);
                buf.pushRectangle(cx,                   cy - th * 0.5f, arm, th, c, 1.0f);
                break;
            case DropPos::Top:
                buf.pushRectangle(cx - arm, cy - arm * 0.4f, arm + th, th, c, 1.0f);
                buf.pushRectangle(cx,       cy - arm * 0.4f, arm,      th, c, 1.0f);
                buf.pushRectangle(cx - th * 0.5f, cy - arm, th, arm, c, 1.0f);
                break;
            case DropPos::Bottom:
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

    void _buildDropTargets(DockWidget* dock, float /*screenX*/, float /*screenY*/,
                           int /*hostScreenX*/, int /*hostScreenY*/)
    {
        m_dropTargets.clear();
        if (!dock) return;
        std::string_view tag = dock->tag();

        // Per-leaf cross of 5 indicators (Center + 4 sides).
        for (const auto& n : m_nodes) {
            if (n.type != DockNode::Type::Leaf) continue;
            const Rect& r = n.rect;
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
            auto isAllowed = [&](DropPos pos) -> bool {
                if (!n.affinity.accepts(tag)) return false;
                if (!dock->acceptsLeafLabel(n.label)) return false;
                if (static_cast<uint8_t>(pos) <= static_cast<uint8_t>(DropPos::Bottom)) {
                    uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(pos));
                    if (!dock->allowsDrop(bit)) return false;
                }
                if (pos == DropPos::Center && !n.tabs.empty()) {
                    if (!dock->isTabifiable()) return false;
                    if (leafBlocksTab)         return false;
                }
                return true;
            };

            float cx = r.x + r.width * 0.5f;
            float cy = r.y + r.height * 0.5f;
            float half = ARROW_SZ * 0.5f;
            float gap = ARROW_SZ + 6.f;

            auto centerRect = [&](float ox, float oy) -> Rect {
                return { cx + ox - half, cy + oy - half, ARROW_SZ, ARROW_SZ };
            };
            auto sidePreview = [&](DropPos pos) -> Rect {
                switch (pos) {
                    case DropPos::Left:   return { r.x, r.y, r.width * 0.5f, r.height };
                    case DropPos::Right:  return { r.x + r.width * 0.5f, r.y, r.width * 0.5f, r.height };
                    case DropPos::Top:    return { r.x, r.y, r.width, r.height * 0.5f };
                    case DropPos::Bottom: return { r.x, r.y + r.height * 0.5f, r.width, r.height * 0.5f };
                    default:              return r;
                }
            };

            m_dropTargets.push_back({ n.id, DropPos::Center, centerRect(0, 0),    r,                            false, isAllowed(DropPos::Center) });
            m_dropTargets.push_back({ n.id, DropPos::Left,   centerRect(-gap, 0), sidePreview(DropPos::Left),   false, isAllowed(DropPos::Left)   });
            m_dropTargets.push_back({ n.id, DropPos::Right,  centerRect(gap, 0),  sidePreview(DropPos::Right),  false, isAllowed(DropPos::Right)  });
            m_dropTargets.push_back({ n.id, DropPos::Top,    centerRect(0, -gap), sidePreview(DropPos::Top),    false, isAllowed(DropPos::Top)    });
            m_dropTargets.push_back({ n.id, DropPos::Bottom, centerRect(0, gap),  sidePreview(DropPos::Bottom), false, isAllowed(DropPos::Bottom) });
        }

        // Window-border edge strips: drop onto the largest leaf along that edge.
        const Rect& host = m_hostRect;
        DockNodeId edgeLeaf = _edgeLeaf();
        if (edgeLeaf.valid()) {
            const DockNode* el = node(edgeLeaf);
            auto edgeAllowed = [&](DropPos pos) -> bool {
                if (!el) return false;
                if (!el->affinity.accepts(tag)) return false;
                if (!dock->acceptsLeafLabel(el->label)) return false;
                if (static_cast<uint8_t>(pos) <= static_cast<uint8_t>(DropPos::Bottom)) {
                    uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(pos));
                    if (!dock->allowsDrop(bit)) return false;
                }
                return true;
            };
            float m = 8.f;
            m_dropTargets.push_back({ edgeLeaf, DropPos::Left,
                { host.x + m, host.y + host.height * 0.5f - ARROW_SZ * 0.5f, ARROW_SZ, ARROW_SZ },
                { host.x, host.y, host.width * 0.33f, host.height }, false, edgeAllowed(DropPos::Left) });
            m_dropTargets.push_back({ edgeLeaf, DropPos::Right,
                { host.x + host.width - m - ARROW_SZ, host.y + host.height * 0.5f - ARROW_SZ * 0.5f, ARROW_SZ, ARROW_SZ },
                { host.x + host.width * 0.67f, host.y, host.width * 0.33f, host.height }, false, edgeAllowed(DropPos::Right) });
            m_dropTargets.push_back({ edgeLeaf, DropPos::Top,
                { host.x + host.width * 0.5f - ARROW_SZ * 0.5f, host.y + m, ARROW_SZ, ARROW_SZ },
                { host.x, host.y, host.width, host.height * 0.33f }, false, edgeAllowed(DropPos::Top) });
            m_dropTargets.push_back({ edgeLeaf, DropPos::Bottom,
                { host.x + host.width * 0.5f - ARROW_SZ * 0.5f, host.y + host.height - m - ARROW_SZ, ARROW_SZ, ARROW_SZ },
                { host.x, host.y + host.height * 0.67f, host.width, host.height * 0.33f }, false, edgeAllowed(DropPos::Bottom) });
        }
    }

    // The leaf with the largest area — used as the target for window-edge drops.
    DockNodeId _edgeLeaf() const {
        DockNodeId best = InvalidDockNodeId;
        float bestArea = -1.f;
        for (const auto& n : m_nodes) {
            if (n.type != DockNode::Type::Leaf) continue;
            float a = n.rect.width * n.rect.height;
            if (a > bestArea) { bestArea = a; best = n.id; }
        }
        return best;
    }

    // ----------------------------------------------------------------------
    // Tree healing — remove an empty leaf and collapse the parent split if
    // it ends up with only one child.
    // ----------------------------------------------------------------------

    void _pruneLeaf(DockNodeId leafId) {
        DockNode* leaf = node(leafId);
        if (!leaf || leaf->type != DockNode::Type::Leaf) return;

        DockNodeId parentId = leaf->parent;
        if (!parentId.valid()) return;

        DockNode* parent = node(parentId);
        if (!parent || parent->type != DockNode::Type::Split) return;

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
        leaf->type     = DockNode::Type::Split;
        leaf->parent   = InvalidDockNodeId;
        leaf->tabs.clear();

        // If the parent split now has only one child, lift that child up to
        // replace the parent in the grandparent, eliminating the redundant split.
        if (parent->children.size() == 1)
            _collapseSingleChildSplit(parentId);

        // If the host is now completely empty (root has no children), plant a
        // placeholder Leaf so the DockHost remains a valid drop target even
        // with zero docks.  The placeholder renders as a plain dark background
        // and _renderLeaf already exits early when tabs is empty.
        DockNode* root = node(rootId());
        if (root && root->children.empty()) {
            DockNodeId ph = _allocNode();
            // _allocNode may reallocate m_nodes — refetch root.
            root = node(rootId());
            DockNode* phNode = node(ph);
            if (phNode) {
                phNode->type   = DockNode::Type::Leaf;
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
    void _collapseSingleChildSplit(DockNodeId splitId) {
        DockNode* split = node(splitId);
        if (!split || split->type != DockNode::Type::Split || split->children.size() != 1) return;

        DockNodeId survivorId = split->children[0];
        DockNodeId grandpaId  = split->parent;

        if (!grandpaId.valid()) return;  // split is root — can't lift further

        DockNode* granpa = node(grandpaId);
        if (!granpa) return;

        // Swap the split out of the grandparent and put the survivor in its slot.
        for (auto& c : granpa->children)
            if (c == splitId) { c = survivorId; break; }

        DockNode* survivor = node(survivorId);
        if (survivor) survivor->parent = grandpaId;

        // Tombstone the old split (orphaned empty split, never reached from root).
        split->children.clear();
        split->weights.clear();
        split->parent = InvalidDockNodeId;
    }

    // ----------------------------------------------------------------------
    // Tree mutation — split a leaf, inserting a new sibling leaf.
    // Returns the id of the new empty leaf (caller inserts the dock into it).
    // ----------------------------------------------------------------------

    DockNodeId _splitLeaf(DockNodeId leafId, DropPos pos) {
        DockNode* leaf = node(leafId);
        if (!leaf || leaf->type != DockNode::Type::Leaf) return InvalidDockNodeId;
        DockNodeId parentId = leaf->parent;
        if (!parentId.valid()) return InvalidDockNodeId;

        bool wantVertical = (pos == DropPos::Top || pos == DropPos::Bottom);
        SplitDir desired = wantVertical ? SplitDir::Vertical : SplitDir::Horizontal;
        bool before = (pos == DropPos::Left || pos == DropPos::Top || pos == DropPos::SplitBefore);

        DockNode* parent = node(parentId);
        if (!parent) return InvalidDockNodeId;

        int idx = -1;
        for (int i = 0; i < static_cast<int>(parent->children.size()); ++i)
            if (parent->children[i] == leafId) { idx = i; break; }
        if (idx < 0) return InvalidDockNodeId;

        // Case A: parent already splits along the desired direction — just
        // insert the new leaf as a sibling next to the target.
        if (parent->splitDir == desired) {
            DockNodeId newLeaf = _allocNode();
            { DockNode& nl = m_nodes[newLeaf.v]; nl.type = DockNode::Type::Leaf; nl.parent = parentId; }
            parent = node(parentId);
            float w = parent->weights[idx];
            float half = w * 0.5f;
            parent->weights[idx] = half;
            int insertAt = before ? idx : idx + 1;
            parent->children.insert(parent->children.begin() + insertAt, newLeaf);
            parent->weights.insert(parent->weights.begin() + insertAt, half);
            _rebalanceWeights(parentId);
            return newLeaf;
        }

        // Case B: introduce a new SplitNode in the target's slot, holding the
        // original leaf and the new leaf as 50/50 children.
        DockNodeId splitId = _allocNode();
        { DockNode& sp = m_nodes[splitId.v]; sp.type = DockNode::Type::Split; sp.parent = parentId; sp.splitDir = desired; }
        DockNodeId newLeaf = _allocNode();
        { DockNode& nl = m_nodes[newLeaf.v]; nl.type = DockNode::Type::Leaf; nl.parent = splitId; }

        // Reparent the original leaf under the new split.
        leaf = node(leafId);
        leaf->parent = splitId;

        DockNode& sp = m_nodes[splitId.v];
        if (before) sp.children = { newLeaf, leafId };
        else        sp.children = { leafId, newLeaf };
        sp.weights = { 0.5f, 0.5f };

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

    DockLayoutNode _snapshotNode(DockNodeId id) const {
        DockLayoutNode out;
        const DockNode* n = node(id);
        if (!n) return out;
        if (n->type == DockNode::Type::Split) {
            out.kind     = DockLayoutNode::Kind::Split;
            out.splitDir = n->splitDir;
            out.weights  = n->weights;
            for (DockNodeId c : n->children)
                out.children.push_back(_snapshotNode(c));
        } else {
            out.kind       = DockLayoutNode::Kind::Leaf;
            out.label      = n->label;
            out.tabBarEdge = n->tabBarEdge;
            out.activeTab  = n->activeTab;
            for (const DockWidget* w : n->tabs)
                out.tabTitles.push_back(w ? w->title() : "");
        }
        return out;
    }

    bool _restoreNode(const DockLayoutNode& src, DockNodeId id, DockNodeId parentId,
                      const std::function<DockWidget*(std::string_view)>& resolver)
    {
        DockNode* n = node(id);
        if (!n) return false;
        n->parent = parentId;

        if (src.kind == DockLayoutNode::Kind::Split) {
            n->type     = DockNode::Type::Split;
            n->splitDir = src.splitDir;
            n->weights  = src.weights;

            for (size_t i = 0; i < src.children.size(); ++i) {
                DockNodeId childId = _allocNode();
                n = node(id);            // refetch: _allocNode may reallocate m_nodes
                if (!n) return false;
                n->children.push_back(childId);
                if (!_restoreNode(src.children[i], childId, id, resolver))
                    return false;
                // n is potentially stale after recursion; refetched at top of next iteration
            }
            _rebalanceWeights(id);  // uses node(id) internally, always safe
        } else {
            n->type       = DockNode::Type::Leaf;
            n->label      = src.label;
            n->tabBarEdge = src.tabBarEdge;
            for (const auto& title : src.tabTitles) {
                if (DockWidget* w = resolver ? resolver(title) : nullptr)
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

    std::vector<DockNode> m_nodes;
    uint32_t              m_nextId{0};
    Rect                  m_hostRect{};

    std::vector<DropTarget> m_dropTargets;
    DropTarget*             m_activeTarget{nullptr};
    DockWidget*             m_draggedDock{nullptr};

    std::vector<DockNode>   m_savedNodes;
    uint32_t                m_savedNextId{0};
    bool                    m_hasSavedState{false};
    bool                    m_livePreviewEnabled{true};

    struct HandleDrag {
        DockNodeId parentSplit;
        int        handleIdx{0};
        float      startCursor{0.f};
        float      startWeightA{0.f}, startWeightB{0.f};
        bool       active{false};
    } m_handleDrag{};

    struct TitleDrag {
        DockWidget* dock{nullptr};
        float       startX{0.f}, startY{0.f};
        bool        active{false};
    } m_titleDrag{};
};

} // namespace Genesis
