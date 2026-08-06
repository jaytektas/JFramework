#pragma once

// JTreeView (+ JTreeViewNode).

#include "JControl.h"
#include "JArrow.h"
#include "JTextHelper.h"
#include "JLineEdit.h"
#include "KeyEvent.h"
#include "DragDrop.h"
#include "Log.h"
#include "../graphics/VectorGraphics.h"

inline namespace jf {

struct JTreeViewNode {
    std::string label;
    bool expanded{false};
    bool selected{false};
    std::vector<JTreeViewNode> children;
    std::string userData;   // opaque app payload (e.g. a binding path) carried by a node but not displayed
    int         icon{0};    // small type glyph drawn before the label (0 = none; app-defined kinds)
    bool        hidden{false};   // transient: run-mode visibility filter hides the row + its subtree (not persisted)
    bool        placeholder{false};  // transient edit-mode "New node…" add-affordance row: drawn dimmed, never persisted (app owns promotion)
    bool        separator{false};    // a RULE between groups, not an entry: drawn as a line, never selectable
};

class JTreeView : public JControl {
public:
    jf::JSignal<JTreeViewNode*> onSelectionChanged;
    jf::JSignal<JTreeViewNode*> onNodeActivated;
    jf::JSignal<JTreeViewNode*> onNodeRenamed;    // fired after an in-place label edit commits
    jf::JSignal<JTreeViewNode*> onNodeDragStarted; // press-and-drag on a node past a threshold (app starts a JDragDrop)
    jf::JSignal<> onDeleteKey;   // Delete/Backspace pressed with a node selected (app removes it)
    jf::JSignal<> onEnterKey;    // Return pressed with a node selected and not renaming (app adds a sibling)
    // Optional: mark a node as a "hyperlink" (e.g. it owns a viewport/page). When set, hovering such a node
    // underlines its label like a web link — a hint that clicking it navigates somewhere. App supplies the
    // predicate (it knows what backs each node); left unset, no node underlines.
    std::function<bool(const JTreeViewNode*)> onIsHyperlink;
    // Internal drag-reorder: fires on drop with (moved node, new parent, insert index). Enable with
    // setInternalReorder(true); then a node drag reorders IN the tree instead of firing onNodeDragStarted.
    jf::JSignal<JTreeViewNode*, JTreeViewNode*, int> onNodeMoved;
    void setInternalReorder(bool on) { m_internalReorder = on; }
    // Whether a node drag may begin at all. Off makes the tree a pure navigator — press selects, but no
    // drag (neither in-tree reorder nor an external node-placement payload) ever starts. Orthogonal to
    // setInternalReorder, which only chooses WHICH drag a started gesture becomes.
    void setDragEnabled(bool on) { m_dragEnabled = on; }
    bool isDragEnabled() const { return m_dragEnabled; }

    // Begin an in-place rename of the selected node (context-menu "Rename" / F2 / double-click). A real
    // JLineEdit is mounted over the row so the rename is a FULL text editor — click to place the caret,
    // double-click to select, arrows/Home/End, drag-select — not a bare append/backspace buffer. The whole
    // label starts SELECTED, so the first keystroke replaces it (typing a name over "New node" just works).
    // Enter commits (fires onNodeRenamed), Escape cancels.
    void beginRename() {
        if (!m_selectedNode) return;
        m_editNode = m_selectedNode;
        m_editField.setText(m_selectedNode->label);
        m_editField.selectAll();       // first keystroke replaces the whole label
        m_editField.setFocused(true);  // render caret/selection (the tree keeps keyboard focus + forwards keys)
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    bool isEditing() const { return m_editNode != nullptr; }

    // Commit an in-place rename (Enter, click elsewhere, or focus loss): apply the field text + fire
    // onNodeRenamed. Cancel (Escape) just clears m_editNode without applying.
    void commitRename() {
        if (!m_editNode) return;
        m_editNode->label = m_editField.text();
        JTreeViewNode* n = m_editNode;
        m_editNode = nullptr;
        m_editField.setFocused(false);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        onNodeRenamed.emit(n);
    }

    // Whether F2 / double-activate may start an in-place rename (default on). Apps with read-only
    // trees (e.g. a fixed config navigator) can disable it.
    // Hold m_scrollY inside what the CURRENT tree can scroll to. Called after a rebuild, since keeping the
    // position is only right while the position still exists.
    void _clampScroll() {
        // getItemHeight(), NOT rowHeight(): m_rowHeight is -1 until a host overrides it, meaning "take the
        // style's". Using the raw member made the content height negative, so every rebuild clamped the
        // position to 0 — the very reset this was added to prevent. Same two helpers the wheel handler uses,
        // so the bounds agree.
        const auto  b      = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const auto  flat   = getFlatNodes();
        const float totalH = flat.size() * getItemHeight() + 8.0f;
        const float maxY   = std::max(0.0f, totalH - b.height);
        m_scrollY = std::clamp(m_scrollY, 0.0f, maxY);
    }

    void setEditable(bool e) { m_editable = e; }
    bool isEditable() const { return m_editable; }

    JTreeView(JSceneGraph& graph, float w = 240.0f, float h = 300.0f)
        : JControl(graph, "JTreeView"), m_root{"Root", true, false, {}}, m_editField(graph)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().menuItemHeight;
        l.minWidth = 80.0f;
        l.minHeight = 40.0f;
        // The rename field is driven entirely BY THE TREE (keys/mouse forwarded, caret shown via the visual
        // focus flag). It must never be a focus-manager target itself, or a click would steal keyboard focus
        // from the tree and the forwarding would stop. NoFocus keeps focusAt from ever picking it.
        m_editField.setFocusPolicy(JFocusPolicy::NoFocus);
    }

    JTreeViewNode& root() { return m_root; }
    const JTreeViewNode& root() const { return m_root; }

    void setRootNode(JTreeViewNode rootNode) {
        // A structural rebuild must not silently drop the selection: callers swap the whole tree on
        // mode flips and filter changes, and losing the selected row each time is a bug, not a reset.
        // Identity is the node's label-path (its chain of labels from the root) — the visible tree
        // structure — NOT userData, which is app-opaque (conditions, binding sigils) and often empty.
        // Re-anchor to the node at that same path if it survived the rebuild. Pure state restore — no
        // onSelectionChanged (the selection didn't logically change), so panels driven off it don't churn.
        std::vector<std::string> keepPath;
        if (m_selectedNode) _pathOf(m_root, m_selectedNode, keepPath);
        m_root = std::move(rootNode);
        m_selectedNode = m_anchorNode = nullptr;   // old pointers dangle into the freed tree
        m_pressNode = m_pendingSelect = m_pendingCollapse = m_hoverNode = nullptr;   // gesture pointers into the freed tree
        // KEEP the scroll position, for the same reason the selection is kept: callers swap the whole tree
        // for a mode flip, a filter change or a one-row edit, and none of those mean "take the reader back
        // to the top". Renaming one node in a long tree scrolled the other three hundred rows away from
        // them. It is clamped after the rebuild (below), so a tree that got shorter still lands in range.
        if (!keepPath.empty()) {
            if (JTreeViewNode* n = _nodeAtPath(m_root, keepPath, 0)) {
                m_selectedNode = m_anchorNode = n;
                n->selected = true;
                JLOGC("treeview", jf::JLogLevel::Debug) << "setRootNode: kept selection '" << _joinPath(keepPath) << "'";
            } else {
                JLOGC("treeview", jf::JLogLevel::Warn) << "setRootNode: selected node '" << _joinPath(keepPath)
                                                       << "' did not survive the rebuild — selection cleared";
            }
        }
        _clampScroll();   // the new tree may be shorter — keep the kept position inside it
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    // Expand / collapse the whole tree. The (synthetic) root stays expanded so its top-level rows
    // remain visible; every descendant is set accordingly.
    // Filter rows to those whose label — or a descendant's — contains `f` (case-insensitive). Empty
    // clears the filter. Matching subtrees are auto-revealed. Drives the dock search boxes.
    void setFilter(const std::string& f) {
        std::string lo = f; for (char& c : lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lo != m_filter) { m_filter = std::move(lo); m_scrollY = 0.f; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    }

    // Opt-in: also match a node's userData (the app payload — e.g. a binding path / raw signal id) against the
    // filter, not only its displayed label. Off by default so trees whose userData holds opaque sigils/conditions
    // don't match on hidden text. The signal picker enables this so typing a raw signal name finds it too.
    void setFilterMatchesUserData(bool on) { m_filterUserData = on; }

    void expandAll()   { for (auto& c : m_root.children) _setExpandedRec(c, true);  m_root.expanded = true; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    void collapseAll() { for (auto& c : m_root.children) _setExpandedRec(c, false); m_root.expanded = true; m_graph.invalidateNode(m_nodeId, DirtySelf); }

    // Select the node at a "/"-joined label path, expanding its ancestors so the highlight is visible, and
    // emit onSelectionChanged exactly as a user click would. Lets an app drive the selection to match a
    // programmatic view change (e.g. following a hyperlink) so the tree highlight stays in sync. No-op if the
    // path doesn't resolve.
    void selectByPath(const std::string& path) {
        if (path.empty()) return;
        std::vector<std::string> parts;
        for (size_t start = 0; start <= path.size(); ) {
            const size_t sep = path.find('/', start);
            if (sep == std::string::npos) { parts.push_back(path.substr(start)); break; }
            parts.push_back(path.substr(start, sep - start));
            start = sep + 1;
        }
        JTreeViewNode* cur = &m_root;
        for (size_t d = 0; d < parts.size(); ++d) {
            JTreeViewNode* next = nullptr;
            for (auto& c : cur->children) if (c.label == parts[d]) { next = &c; break; }
            if (!next) return;                                   // path doesn't resolve → leave selection as-is
            if (d + 1 < parts.size()) next->expanded = true;     // reveal ancestors (not the leaf itself)
            cur = next;
        }
        // Update the selected pointer/flag if it changed, then ALWAYS emit onSelectionChanged. selectByPath's
        // job is to drive the view to `cur`; the app's view (e.g. the surface's active node) can be out of sync
        // with the tree's selection — notably right after setRootNode re-anchored the selection to this same
        // node — so a plain _selectNode() would no-op and the view would never follow. Force the notify.
        if (m_selectedNode != cur) {
            if (m_selectedNode) m_selectedNode->selected = false;
            m_selectedNode = cur;
            m_selectedNode->selected = true;
        }
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        onSelectionChanged.emit(cur);
    }

    // Run-mode condition filtering (mirrors the original EditTree::applyConditions): hide every node whose
    // predicate returns false — and its whole subtree — in place, so selection + scroll survive. Re-run each
    // telemetry frame; only invalidates when the visible set actually changes (no per-frame flicker/rebuild).
    void applyVisibility(const std::function<bool(const JTreeViewNode&)>& visible) {
        bool changed = false;
        for (auto& c : m_root.children) _applyVis(c, visible, changed);
        if (changed) { if (m_selectedNode && _isHidden(m_root, m_selectedNode)) _selectNode(nullptr); m_graph.invalidateNode(m_nodeId, DirtySelf); }
    }
    void clearVisibility() {
        bool changed = false;
        for (auto& c : m_root.children) _clearVis(c, changed);
        if (changed) m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    float rowHeight() const { return m_rowHeight; }
    float scrollY()   const { return m_scrollY; }   // vertical offset, for tests/hosts that persist it
    void setRowHeight(float h) { m_rowHeight = h; m_graph.invalidateNode(m_nodeId, DirtySelf); }

    float getItemHeight() const {
        return m_rowHeight > 0.0f ? m_rowHeight : (JTextHelper::hasAtlas() ? JTextHelper::lineHeight() + 8.0f : 22.0f);
    }

    struct JFlatNode {
        JTreeViewNode* node;
        int depth;
        size_t flatIndex;
    };

    std::vector<JFlatNode> getFlatNodes() {
        std::vector<JFlatNode> flat;
        for (auto& child : m_root.children) {
            _flatten(child, 0, flat);
        }
        return flat;
    }

    // Screen rect of the mounted rename field (empty if not editing / the row is scrolled off).
    JRect _editRect() {
        if (!m_editNode) return {};
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        auto flat = getFlatNodes();
        const float itemH = getItemHeight();
        for (int i = 0; i < static_cast<int>(flat.size()); ++i) if (flat[i].node == m_editNode) {
            const float itemY = b.y + 4.0f + i * itemH - m_scrollY;
            const float textX = b.x + (flat[i].depth * 16.0f + 6.0f) + 16.0f;
            const float ex = textX - 3.0f;
            return { ex, itemY + 2.0f, b.x + b.width - 16.0f - ex, itemH - 4.0f };
        }
        return {};
    }

    void handleMousePress(float mx, float my) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        // A press INSIDE the rename field positions the caret / selects (double-click) — route it to the
        // field and stop. A press anywhere else commits the rename before the click is handled as a normal
        // tree interaction (clicking another node, the same node, or empty space all end editing).
        const bool wasEditing = (m_editNode != nullptr);
        if (wasEditing) {
            const JRect er = _editRect();
            if (mx >= er.x && mx < er.x + er.width && my >= er.y && my < er.y + er.height) {
                m_editField.handleMousePress(mx, my);
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                return;
            }
            commitRename();
        }
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            onClicked.emit();
            float trackW = 10.0f;
            float trackX = b.x + b.width - trackW;
            if (mx >= trackX) {
                m_draggingScroll = true;
                m_dragStartY = my;
                m_dragStartScrollY = m_scrollY;
            } else {
                auto flatNodes = getFlatNodes();
                float itemH = getItemHeight();
                float relativeY = my - b.y + m_scrollY - 4.0f;
                int clickedIndex = static_cast<int>(relativeY / itemH);
                if (clickedIndex >= 0 && clickedIndex < (int)flatNodes.size()) {
                    auto& flat = flatNodes[clickedIndex];
                    float indent = flat.depth * 16.0f + 6.0f;
                    float arrowW = 16.0f;
                    if (flat.node->separator) return;   // a rule is not a row you can select or rename
                    if (!flat.node->children.empty() && mx >= b.x + indent && mx <= b.x + indent + arrowW) {
                        flat.node->expanded = !flat.node->expanded;
                        m_graph.invalidateNode(m_nodeId, DirtySelf);
                    } else {
                        m_pressNode = flat.node; m_pressX = mx; m_pressY = my;   // arm a potential drag
                        // A DOUBLE-click (same node twice within 400 ms) begins an in-place rename;
                        // a single click selects + activates.
                        const auto nowT = std::chrono::steady_clock::now();
                        const bool dbl = m_editable && !wasEditing && flat.node == m_lastClickNode &&
                                         std::chrono::duration_cast<std::chrono::milliseconds>(nowT - m_lastClickTime).count() < 400;
                        m_lastClickNode = flat.node; m_lastClickTime = nowT;
                        if (m_multiSelect && (JWidget::s_ctrlDown || JWidget::s_shiftDown)) {
                            if (JWidget::s_shiftDown) { auto fn = flatNodes; _selectRangeTo(flat.node, fn); }
                            else {   // Ctrl: toggle this node in/out of the set
                                flat.node->selected = !flat.node->selected;
                                m_selectedNode = m_anchorNode = flat.node;
                                m_graph.invalidateNode(m_nodeId, DirtySelf);
                                onSelectionChanged.emit(flat.node);
                            }
                            m_pendingCollapse = nullptr;
                        } else if (m_multiSelect && flat.node->selected && _selectedCount() > 1) {
                            // Keep the multi-selection so a drag carries all of it; collapse to just this
                            // node on release only if the press turns out to be a click (no drag).
                            m_selectedNode = flat.node; m_pendingCollapse = flat.node;
                        } else {
                            m_pendingCollapse = nullptr;
                            if (dbl) {                       // double-click: select now, then rename in place
                                if (m_multiSelect) _selectSingle(flat.node); else _selectNode(flat.node);
                                beginRename();
                            } else {
                                // Defer select + activate to RELEASE. Selecting on press fires
                                // onSelectionChanged (which switches the view) the instant a drag begins, so a
                                // node could never be dragged out as a link / onto a surface without the view
                                // jumping away first. A drag clears this (handleMouseMove); a plain click commits
                                // it (handleMouseRelease). Mouse-up on the node == the user's intent to view it.
                                m_pendingSelect = flat.node;
                            }
                        }
                    }
                }
            }
        }
    }

    void handleMouseRelease(float mx, float my) override {
        if (m_editNode) { m_editField.handleMouseRelease(mx, my); return; }   // end a drag-select in the rename field
        m_draggingScroll = false;
        // A click (no drag) on an already-multi-selected node collapses the set to just that node. A drag
        // cleared m_pressNode (external) or set m_dragging (reorder), so either of those means "not a click".
        if (m_pendingCollapse) { if (m_pressNode && !m_dragging) _selectSingle(m_pendingCollapse); m_pendingCollapse = nullptr; }
        if (m_dragging) {
            JDragDrop::cancel();   // released back inside the tree → a reorder, not a surface placement
            JTreeViewNode* moved = m_pressNode;
            _computeDrop(mx, my);
            JTreeViewNode* target = m_dropTarget; int mode = m_dropMode;
            m_dragging = false; m_externalLive = false; m_dropTarget = nullptr; m_pressNode = nullptr;
            bool didMove = false;
            if (moved && target && moved != target && !_isAncestor(moved, target)) {
                std::vector<int> srcPath = _pathOf(moved);
                std::vector<int> destParentPath; int destIdx = 0;
                if (mode == 1) {                       // drop as last child of target
                    destParentPath = _pathOf(target);
                    destIdx = (int)target->children.size();
                } else {                               // insert before/after target (as its sibling)
                    std::vector<int> tp = _pathOf(target);
                    if (!tp.empty()) { destIdx = tp.back() + (mode == 2 ? 1 : 0); tp.pop_back(); destParentPath = tp; }
                }
                didMove = _moveNode(srcPath, destParentPath, destIdx);
            }
            m_selectedNode = nullptr;   // vector reallocation invalidated node pointers
            m_lastClickNode = nullptr;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            if (didMove) onNodeMoved.emit(nullptr, nullptr, 0);   // app re-reads root() + persists
            return;
        }
        m_pressNode = nullptr;
        // A plain click (no drag intervened, so m_pendingSelect survived): NOW select + activate. This is the
        // deferred single-click selection — mouse-up on the node commits it as the view.
        if (m_pendingSelect) {
            JTreeViewNode* n = m_pendingSelect; m_pendingSelect = nullptr;
            if (m_multiSelect) _selectSingle(n); else _selectNode(n);
            onNodeActivated.emit(n);
        }
        JControl::handleMouseRelease(mx, my);
    }

    // The flat node whose row is under (mx,my), or null if outside the tree / on the scrollbar / past the rows.
    JTreeViewNode* _nodeAtPoint(float mx, float my) {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx < b.x || mx > b.x + b.width - 10.0f || my < b.y || my > b.y + b.height) return nullptr;  // -10: scrollbar col
        auto flatNodes = getFlatNodes();
        const float itemH = getItemHeight();
        const int idx = static_cast<int>((my - b.y + m_scrollY - 4.0f) / itemH);
        return (idx >= 0 && idx < static_cast<int>(flatNodes.size())) ? flatNodes[idx].node : nullptr;
    }

    void handleMouseMove(float mx, float my) override {
        if (m_editNode) { m_editField.handleMouseMove(mx, my); m_graph.invalidateNode(m_nodeId, DirtySelf); return; }   // drag-select in the rename field
        // Hyperlink-hover underline: track the row under the cursor (none while a drag/press gesture is live)
        // and repaint when it changes, so the underline follows the mouse.
        JTreeViewNode* hov = (m_dragging || m_draggingScroll || m_pressNode) ? nullptr : _nodeAtPoint(mx, my);
        if (hov != m_hoverNode) { m_hoverNode = hov; m_graph.invalidateNode(m_nodeId, DirtySelf); }
        if (m_draggingScroll) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            auto flatNodes = getFlatNodes();
            float itemH = getItemHeight();
            float totalH = flatNodes.size() * itemH + 8.0f;
            float maxScrollY = std::max(0.0f, totalH - b.height);
            float trackH = b.height - 8.0f;
            float handleH = std::max(20.0f, (b.height / totalH) * trackH);
            float thumbRange = trackH - handleH;
            if (thumbRange > 0.0f) {
                m_scrollY = std::clamp(m_dragStartScrollY + (my - m_dragStartY) * maxScrollY / thumbRange, 0.0f, maxScrollY);
                m_graph.invalidateNode(m_nodeId, DirtySelf);
            }
            return;
        }
        // An internal-reorder drag in progress: track the in-tree drop target while the cursor is over the
        // tree. The external payload was already armed at drag start (mirroring the original studio's dual
        // internal-move + node-path drag), so once the cursor leaves the tree we just stop drawing the
        // drop indicator — a surface under the cursor now owns the drop, and the reorder only applies if
        // the drag is released back INSIDE the tree.
        if (m_dragging) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            const bool inside = mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height;
            if (inside) {
                if (m_externalLive) { JDragDrop::cancel(); m_externalLive = false; }   // back inside → resume in-tree reorder
                _computeDrop(mx, my);
            } else {
                m_dropTarget = nullptr;
                // Left the tree → NOW arm the external node-placement payload for a surface drop. Deferring it to
                // here (not drag start) means a reorder released INSIDE the tree has no active JDragDrop, so the
                // app's active-drag release choke doesn't withhold the release from us and the reorder completes.
                if (!m_externalLive && m_pressNode) { m_externalLive = true; onNodeDragStarted.emit(m_pressNode); }
            }
            m_graph.invalidateNode(m_nodeId, DirtySelf); return;
        }
        // Press-and-drag on a node past a small threshold starts a drag. With internal reorder enabled
        // it becomes an in-tree move (drop indicator + onNodeMoved); otherwise the app turns it into a
        // JDragDrop of the node payload (one-shot: clear the armed node so it fires once).
        if (m_pressNode && !m_editNode && m_dragEnabled) {
            const float ddx = mx - m_pressX, ddy = my - m_pressY;
            if (ddx * ddx + ddy * ddy > 25.0f) {
                m_pendingSelect = nullptr;   // moved past the threshold → this is a drag, not a view-select click
                if (m_internalReorder) {
                    // Begin an IN-TREE reorder. The external node-placement payload is armed lazily (in the
                    // m_dragging branch above) only once the cursor leaves the tree — NOT here — so a drag
                    // released inside the tree has no active JDragDrop to withhold its reorder release.
                    m_dragging = true; m_externalLive = false; _computeDrop(mx, my);
                    m_graph.invalidateNode(m_nodeId, DirtySelf);
                }
                else { JTreeViewNode* n = m_pressNode; m_pressNode = nullptr; onNodeDragStarted.emit(n); }
            }
        }
        JControl::handleMouseMove(mx, my);
    }

    bool handleScroll(float mx, float my, float wheel) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            auto flatNodes = getFlatNodes();
            float itemH = getItemHeight();
            float totalH = flatNodes.size() * itemH + 8.0f;
            float maxScrollY = std::max(0.0f, totalH - b.height);
            m_scrollY = std::clamp(m_scrollY - wheel * 30.0f, 0.0f, maxScrollY);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        }
        return false;
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed) return false;
        using EK = JKeyEvent::JKey;

        // In-place rename: the tree owns keyboard while editing a label. Return/Escape commit/cancel; every
        // other key (text, caret motion, selection, backspace/delete) goes to the mounted JLineEdit, which
        // is the real editor. m_editField holds the visual focus flag so it renders its caret; the tree stays
        // the focus-manager target and forwards here.
        if (m_editNode) {
            if (ke.key == EK::Return) { commitRename(); return true; }
            if (ke.key == EK::Escape) { m_editNode = nullptr; m_editField.setFocused(false); m_graph.invalidateNode(m_nodeId, DirtySelf); return true; }
            m_editField.handleKeyEvent(ke);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;   // swallow all keys while editing
        }

        // F2 begins an in-place rename of the selected node (standard rename shortcut).
        if (m_editable && ke.key == EK::F2 && m_selectedNode) { beginRename(); return true; }

        // Structural shortcuts (mirror the original studio's EditTree): Delete/Backspace remove the
        // selected node, Return adds a sibling. The app connects these and applies its own edit-mode gate.
        if ((ke.key == EK::Delete || ke.key == EK::Backspace) && m_selectedNode) { onDeleteKey.emit(); return true; }
        if (ke.key == EK::Return && m_selectedNode) { onEnterKey.emit(); return true; }

        auto flatNodes = getFlatNodes();
        if (flatNodes.empty()) return false;

        int selIdx = -1;
        if (m_selectedNode) {
            for (int i = 0; i < (int)flatNodes.size(); ++i) {
                if (flatNodes[i].node == m_selectedNode) {
                    selIdx = i;
                    break;
                }
            }
        }

        using K = JKeyEvent::JKey;
        if (ke.key == K::Down) {
            int nextIdx = (selIdx == -1) ? 0 : std::clamp(selIdx + 1, 0, (int)flatNodes.size() - 1);
            _selectNode(flatNodes[nextIdx].node);
            _ensureIndexVisible(nextIdx);
            return true;
        } else if (ke.key == K::Up) {
            int nextIdx = (selIdx == -1) ? 0 : std::clamp(selIdx - 1, 0, (int)flatNodes.size() - 1);
            _selectNode(flatNodes[nextIdx].node);
            _ensureIndexVisible(nextIdx);
            return true;
        } else if (ke.key == K::Right) {
            if (selIdx != -1) {
                auto* n = flatNodes[selIdx].node;
                if (!n->children.empty()) {
                    if (!n->expanded) {
                        n->expanded = true;
                        m_graph.invalidateNode(m_nodeId, DirtySelf);
                    } else {
                        int nextIdx = std::clamp(selIdx + 1, 0, (int)flatNodes.size() - 1);
                        _selectNode(flatNodes[nextIdx].node);
                        _ensureIndexVisible(nextIdx);
                    }
                    return true;
                }
            }
        } else if (ke.key == K::Left) {
            if (selIdx != -1) {
                auto* n = flatNodes[selIdx].node;
                if (!n->children.empty() && n->expanded) {
                    n->expanded = false;
                    m_graph.invalidateNode(m_nodeId, DirtySelf);
                    return true;
                } else {
                    JTreeViewNode* parent = _findParent(&m_root, n);
                    if (parent && parent != &m_root) {
                        _selectNode(parent);
                        for (int i = 0; i < (int)flatNodes.size(); ++i) {
                            if (flatNodes[i].node == parent) {
                                _ensureIndexVisible(i);
                                break;
                            }
                        }
                        return true;
                    }
                }
            }
        } else if (ke.key == K::Return || ke.key == K::Space) {
            if (selIdx != -1) {
                onNodeActivated.emit(flatNodes[selIdx].node);
                return true;
            }
        }

        return false;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        // A reorder drag that ended OUTSIDE the tree (dropped on a surface) never delivered a release here.
        // Once the external payload we armed at drag start has gone live and then cleared (accepted/cancelled
        // elsewhere), drop our stale internal-drag state. The two-step guard avoids clearing in the very first
        // frame, before the app's onNodeDragStarted handler has started the global drag.
        if (m_dragging) {
            if (JDragDrop::isDragging()) m_externalLive = true;
            else if (m_externalLive) { m_dragging = false; m_externalLive = false; m_dropTarget = nullptr; m_pressNode = nullptr; }
        }
        bool focused = isFocused();
        if (m_editNode && !focused) commitRename();   // focus moved elsewhere (another widget) — end the edit

        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface1, 6.0f,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);

        auto flatNodes = getFlatNodes();
        if (flatNodes.empty()) return;

        float itemH = getItemHeight();
        float totalH = flatNodes.size() * itemH + 8.0f;
        float maxScrollY = std::max(0.0f, totalH - b.height);
        m_scrollY = std::clamp(m_scrollY, 0.0f, maxScrollY);

        int startIdx = static_cast<int>(m_scrollY / itemH);
        int endIdx = static_cast<int>((m_scrollY + b.height) / itemH) + 1;
        startIdx = std::clamp(startIdx, 0, (int)flatNodes.size() - 1);
        endIdx = std::clamp(endIdx, 0, (int)flatNodes.size() - 1);

        buf.pushClip(b.x + 1.0f, b.y + 1.0f, b.width - 13.0f, b.height - 2.0f);

        for (int i = startIdx; i <= endIdx; ++i) {
            auto& flat = flatNodes[i];
            float itemY = b.y + 4.0f + i * itemH - m_scrollY;
            float indent = flat.depth * 16.0f + 6.0f;

            // EVERY selected row is drawn as selected. This used to paint only m_selectedNode — the row
            // that happens to be current — so in a multi-selection the other rows looked untouched: the
            // only other cue is the label colour, and Text vs HighlightedText differ by five levels on the
            // dark theme, which is no cue at all. You could not tell what you had picked, or what a drag
            // was about to carry.
            if (flat.node->selected || flat.node == m_selectedNode) {
                drawNodeBackground(buf, flat.node, {b.x + 4.0f, itemY, b.width - 18.0f, itemH});
                // The CURRENT row (the anchor a Shift-range extends from, the one the keyboard moves) is
                // outlined on top of the same fill, so "selected" and "current" stay distinguishable.
                if (flat.node == m_selectedNode && _selectedCount() > 1) {
                    const JColor a = jstyle::pal().color(JColorRole::Accent, JColorGroup::Active);
                    const uint8_t ring[4] = { a.r, a.g, a.b, 200 };
                    buf.pushRectangle(b.x + 4.0f, itemY, b.width - 18.0f, itemH,
                                      Colors::Transparent, 4.0f, 1.0f, ring);
                }
            }

            if (!flat.node->children.empty()) {
                float ax = b.x + indent + 8.0f;
                float ay = itemY + itemH * 0.5f;
                // While filtering, subtrees are force-opened (_flatten), so show ▼ then too — not a stale ▶.
                drawNodeChevron(buf, flat.node, ax, ay, 10.0f, flat.node->expanded || !m_filter.empty());
            }

            float textX = b.x + indent + 16.0f;
            float ty = itemY + (itemH - (JTextHelper::hasAtlas() ? JTextHelper::lineHeight() : 8.0f)) * 0.5f;
            if (flat.node == m_editNode) {
                // In-place edit field: the mounted JLineEdit draws its own box, text, selection + caret.
                const float ex = textX - 3.0f, ew = b.x + b.width - 16.0f - ex;
                m_editField.setBounds({ ex, itemY + 2.0f, ew, itemH - 4.0f });
                m_editField.populateRenderPrimitives(buf);
            } else {
                float tx = textX;
                if (flat.node->icon != 0) { _drawTreeIcon(buf, textX + 1.0f, itemY + itemH * 0.5f, flat.node->icon); tx += 15.0f; }
                const float maxW = b.width - (tx - b.x) - 14.0f;
                drawNodeText(buf, flat.node, tx, ty, maxW);
                // Hyperlink-hover underline: while the cursor is over a node the app marks as a link, underline
                // its label like a web link (a hint that clicking navigates there).
                if (flat.node == m_hoverNode && onIsHyperlink && onIsHyperlink(flat.node) && JTextHelper::hasAtlas()) {
                    const float lw = std::min(JTextHelper::measureWidth(tr(flat.node->label)), maxW);
                    const JStyleOption o = jstyle::option(m_state, isFocused());
                    const uint8_t* uc = jstyle::role(flat.node->selected ? JColorRole::HighlightedText : JColorRole::Text, o).data();
                    uint8_t line[4] = { uc[0], uc[1], uc[2], 230 };
                    buf.pushRectangle(tx, ty + JTextHelper::lineHeight() - 1.0f, lw, 1.0f, line, 0.0f);
                }
            }
        }

        // Drop indicator during an internal-reorder drag: a boxed row for "into", else a caret line
        // at the top (before) or bottom (after) of the hovered row.
        if (m_dragging && m_dropTarget) {
            int di = -1;
            for (int i = 0; i < (int)flatNodes.size(); ++i) if (flatNodes[i].node == m_dropTarget) { di = i; break; }
            if (di >= 0) {
                float ry = b.y + 4.0f + di * itemH - m_scrollY;
                const uint8_t* ind = Colors::Accent;
                if (m_dropMode == 1) {
                    buf.pushRectangle(b.x + 3.0f, ry, b.width - 16.0f, itemH, Colors::Transparent, 4.0f, 2.0f, ind);
                } else {
                    float ly = (m_dropMode == 2) ? ry + itemH - 1.0f : ry - 1.0f;
                    buf.pushRectangle(b.x + 6.0f, ly, b.width - 20.0f, 2.5f, ind, 1.0f);
                }
            }
        }

        buf.popClip();

        if (totalH > b.height) {
            float trackW = 8.0f;
            float trackX = b.x + b.width - trackW - 2.0f;
            buf.pushRectangle(trackX, b.y + 2.0f, trackW, b.height - 4.0f, Colors::Surface2, 4.0f);

            float handleH = std::max(20.0f, (b.height / totalH) * (b.height - 8.0f));
            float handleY = b.y + 4.0f + (m_scrollY / maxScrollY) * (b.height - 8.0f - handleH);
            buf.pushRectangle(trackX + 1.0f, handleY, trackW - 2.0f, handleH, Colors::Surface3, 3.0f);
        }
    }



    JTreeViewNode* selectedNode() const { return m_selectedNode; }

    // Multi-select (opt-in): Ctrl+click toggles a node, Shift+click extends a range from the anchor, a
    // plain click collapses to one. Pressing an already-selected node keeps the set (so a drag carries
    // every selected node) and only collapses to it if the press turns out to be a click, not a drag.
    void setMultiSelect(bool on) { m_multiSelect = on; }
    bool multiSelect() const { return m_multiSelect; }
    std::vector<JTreeViewNode*> selectedNodes() {
        std::vector<JTreeViewNode*> out;
        for (auto& c : m_root.children) _collectSelected(c, out);
        return out;
    }

protected:
    virtual void drawNodeBackground(JPrimitiveBuffer& buf, JTreeViewNode* node, const JRect& bounds) {
        uint8_t selBg[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 60};
        buf.pushRectangle(bounds.x, bounds.y, bounds.width, bounds.height, selBg, 4.0f);
    }

    // A small ~10px type glyph before a leaf label (app-assigned JTreeViewNode::icon). Shapes/colours
    // distinguish kinds without needing font symbol glyphs: 1 grid, 2 rounded, 3 bar, 4 pill, 5 filled,
    // else a hollow outline. Kinds are app-defined; unknown ones fall through to the hollow default.
    void _drawTreeIcon(JPrimitiveBuffer& buf, float x, float cy, int kind) {
        const float s = 10.0f, y = cy - s * 0.5f, h = s * 0.45f;
        // Node-kind glyph colours route through the themed TreeIcon* roles (byte-identical defaults).
        const uint8_t* orange = Colors::TreeIconTable; const uint8_t* blue = Colors::TreeIconConfig;
        const uint8_t* green  = Colors::TreeIconToggle; const uint8_t* purple = Colors::TreeIconEnum;
        const uint8_t* cyan   = Colors::TreeIconCurve;
        switch (kind) {
            case 1:  // table — 2×2 grid
                buf.pushRectangle(x, y, h, h, orange);           buf.pushRectangle(x + h + 1.f, y, h, h, orange);
                buf.pushRectangle(x, y + h + 1.f, h, h, orange); buf.pushRectangle(x + h + 1.f, y + h + 1.f, h, h, orange);
                break;
            case 2:  buf.pushRectangle(x, y, s, s, cyan, 3.0f); break;                 // curve
            case 3:  buf.pushRectangle(x, y + s * 0.28f, s, s * 0.44f, purple, 2.0f); break;   // enum — bar
            case 4:  buf.pushRectangle(x, y + s * 0.15f, s, s * 0.7f, green, s * 0.35f); break; // toggle — pill
            case 5:  buf.pushRectangle(x, y, s, s, blue, 2.0f); break;                 // config — filled
            default: buf.pushRectangle(x, y, s, s, Colors::Transparent, 2.0f, 1.5f, green); break;   // value/channel — hollow
        }
    }

    virtual void drawNodeChevron(JPrimitiveBuffer& buf, JTreeViewNode* /*node*/, float ax, float ay, float /*size*/, bool expanded) {
        // Disclosure indicator (▶ collapsed / ▼ expanded) — drawn by JArrow, the one triangle in the
        // toolkit, so the tree and the data grid's sort mark cannot drift apart.
        const uint8_t col[4] = { Colors::MutedText[0], Colors::MutedText[1], Colors::MutedText[2], 230 };
        JArrow::draw(buf, ax, ay, expanded ? JArrow::Direction::Down : JArrow::Direction::Right, col);
    }

    virtual void drawNodeText(JPrimitiveBuffer& buf, JTreeViewNode* node, float tx, float ty, float maxW) {
        if (node->separator) {   // a group RULE — a menu's own grouping, carried through as a line
            const uint8_t* b = Colors::Border;
            const uint8_t col[4] = { b[0], b[1], b[2], 160 };
            buf.pushRectangle(tx, ty + JTextHelper::lineHeight() * 0.5f, maxW, 1.0f, col);
            return;
        }
        // Resolve the theme's TEXT role (so a custom global palette applies), not a raw shade. Selected rows
        // take HighlightedText for contrast on the selection fill. A placeholder ("New node…") add-affordance
        // row draws dimmed so it reads as a ghost hint, not a real node (still selectable/renamable).
        const JStyleOption o = jstyle::option(m_state, isFocused());
        const uint8_t* base = jstyle::role(node->selected ? JColorRole::HighlightedText : JColorRole::Text, o).data();
        if (JTextHelper::hasAtlas()) {
            uint8_t tc[4] = {base[0], base[1], base[2], static_cast<uint8_t>(node->placeholder ? 150 : 230)};
            JTextHelper::pushText(buf, tx, ty, tr(node->label), tc, maxW);
        } else {
            uint8_t tc[4] = {base[0], base[1], base[2], 180};
            buf.pushRectangle(tx, ty + 2.0f, 60.0f, 8.0f, tc, 2.0f);
        }
    }

private:
    void _flatten(JTreeViewNode& node, int depth, std::vector<JFlatNode>& result, bool ancestorMatch = false) {
        if (node.hidden) return;                                // run-mode condition filter (self + descendants)
        // Once a node matches (or an ancestor did), its WHOLE subtree stays present — so a matched category
        // reveals its signals and a matched sensor reveals its fields. Otherwise a node only survives as a path
        // down to a deeper match. This mirrors the old studio's ancestorMatch propagation.
        bool subtreeMatch = ancestorMatch;
        if (!m_filter.empty() && !ancestorMatch) {
            if (_selfMatches(node))            subtreeMatch = true;   // this node matched → keep its subtree
            else if (!_descendantMatches(node)) return;               // neither self nor descendant → filtered out
        }
        result.push_back({&node, depth, result.size()});
        // While filtering, force subtrees open so matches deep in the tree are revealed.
        if (node.expanded || !m_filter.empty()) {
            for (auto& child : node.children) {
                _flatten(child, depth + 1, result, subtreeMatch);
            }
        }
    }
    // A node matches on its own label — or, when enabled, its userData (raw id / binding path).
    bool _selfMatches(const JTreeViewNode& n) const {
        std::string l = n.label; for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (l.find(m_filter) != std::string::npos) return true;
        if (m_filterUserData && !n.userData.empty()) {
            std::string u = n.userData; for (char& c : u) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (u.find(m_filter) != std::string::npos) return true;
        }
        return false;
    }
    // True when some descendant matches — the node is then shown as a path to that match (subtree NOT force-kept).
    bool _descendantMatches(const JTreeViewNode& n) const {
        for (const auto& c : n.children) if (_selfMatches(c) || _descendantMatches(c)) return true;
        return false;
    }

    void _selectNode(JTreeViewNode* node) {
        if (m_selectedNode != node) {
            if (m_selectedNode) m_selectedNode->selected = false;
            m_selectedNode = node;
            if (m_selectedNode) m_selectedNode->selected = true;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onSelectionChanged.emit(node);
        }
    }

    // --- Multi-select helpers ------------------------------------------------------------------------
    void _clearSelRec(JTreeViewNode& n) { n.selected = false; for (auto& c : n.children) _clearSelRec(c); }
    void _clearAllSelected() { for (auto& c : m_root.children) _clearSelRec(c); }
    void _collectSelected(JTreeViewNode& n, std::vector<JTreeViewNode*>& out) {
        if (n.selected) out.push_back(&n);
        for (auto& c : n.children) _collectSelected(c, out);
    }
    int  _selectedCount() { std::vector<JTreeViewNode*> v; for (auto& c : m_root.children) _collectSelected(c, v); return (int)v.size(); }
    void _selectSingle(JTreeViewNode* node) {   // clear the whole set, select just `node`, make it the anchor
        _clearAllSelected();
        m_selectedNode = m_anchorNode = node;
        if (node) node->selected = true;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        onSelectionChanged.emit(node);
    }
    void _selectRangeTo(JTreeViewNode* target, std::vector<JFlatNode>& flat) {   // anchor..target inclusive
        int ai = -1, ti = -1;
        for (int i = 0; i < (int)flat.size(); ++i) { if (flat[i].node == m_anchorNode) ai = i; if (flat[i].node == target) ti = i; }
        if (ai < 0) { _selectSingle(target); return; }
        if (ti < 0) ti = ai;
        _clearAllSelected();
        if (ai > ti) std::swap(ai, ti);
        for (int i = ai; i <= ti; ++i) flat[i].node->selected = true;
        m_selectedNode = target;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        onSelectionChanged.emit(target);
    }

    // The label-path (chain of labels from root down to `target`) — a stable identity across a
    // structural rebuild. Root is synthetic, so it contributes no path element.
    static bool _pathOf(const JTreeViewNode& n, const JTreeViewNode* target, std::vector<std::string>& out) {
        for (const auto& c : n.children) {
            out.push_back(c.label);
            if (&c == target || _pathOf(c, target, out)) return true;
            out.pop_back();
        }
        return false;
    }

    // Descend a label-path to the node it addresses, or nullptr if any segment is gone.
    static JTreeViewNode* _nodeAtPath(JTreeViewNode& n, const std::vector<std::string>& path, size_t depth) {
        if (depth >= path.size()) return &n;
        for (auto& c : n.children)
            if (c.label == path[depth]) return _nodeAtPath(c, path, depth + 1);
        return nullptr;
    }

    static std::string _joinPath(const std::vector<std::string>& p) {
        std::string s; for (const auto& seg : p) { s += '/'; s += seg; } return s.empty() ? "/" : s;
    }

    JTreeViewNode* _findParent(JTreeViewNode* current, JTreeViewNode* target) {
        for (auto& child : current->children) {
            if (&child == target) return current;
            auto* p = _findParent(&child, target);
            if (p) return p;
        }
        return nullptr;
    }

    void _ensureIndexVisible(int index) {
        auto flatNodes = getFlatNodes();
        if (index < 0 || index >= (int)flatNodes.size()) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float itemH = getItemHeight();
        float itemY = index * itemH;
        if (itemY < m_scrollY) {
            m_scrollY = itemY;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        } else if (itemY + itemH > m_scrollY + b.height) {
            m_scrollY = itemY + itemH - b.height;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        }
    }

    static void _setExpandedRec(JTreeViewNode& n, bool e) { n.expanded = e; for (auto& c : n.children) _setExpandedRec(c, e); }

    // Run-mode condition filter helpers (applyVisibility / clearVisibility). A node is hidden when its own
    // predicate is false; a hidden node hides its whole subtree (children not evaluated). `changed` tracks
    // whether the visible set moved, so the caller only invalidates on a real transition.
    static void _applyVis(JTreeViewNode& n, const std::function<bool(const JTreeViewNode&)>& visible, bool& changed) {
        const bool hide = !visible(n);
        if (n.hidden != hide) { n.hidden = hide; changed = true; }
        if (hide) { _clearVis(n, changed, /*childrenOnly*/ true); return; }   // subtree follows a hidden ancestor
        for (auto& c : n.children) _applyVis(c, visible, changed);
    }
    static void _clearVis(JTreeViewNode& n, bool& changed, bool childrenOnly = false) {
        if (!childrenOnly && n.hidden) { n.hidden = false; changed = true; }
        for (auto& c : n.children) _clearVis(c, changed);
    }
    // True if `target` lies anywhere in `node`'s subtree AND that path passes through a hidden node — i.e. the
    // node is currently filtered out. Used to drop a selection that a filter just hid.
    static bool _isHidden(JTreeViewNode& node, JTreeViewNode* target) {
        for (auto& c : node.children) {
            if (&c == target) return c.hidden;
            if (c.hidden) { if (_contains(c, target)) return true; }   // hidden ancestor hides the target
            else if (_isHidden(c, target)) return true;
        }
        return false;
    }
    static bool _contains(JTreeViewNode& node, JTreeViewNode* target) {
        for (auto& c : node.children) { if (&c == target) return true; if (_contains(c, target)) return true; }
        return false;
    }

    // --- internal drag-reorder ---
    // Decide the drop target + mode (0 before / 1 into / 2 after) for the cursor at (mx,my).
    void _computeDrop(float mx, float my) {
        (void)mx;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        auto flat = getFlatNodes();
        m_dropTarget = nullptr; m_dropMode = 0;
        if (flat.empty()) return;
        float itemH = getItemHeight();
        float rel = my - b.y + m_scrollY - 4.0f;
        int idx = std::clamp((int)(rel / itemH), 0, (int)flat.size() - 1);
        m_dropTarget = flat[idx].node;
        float frac = (rel - idx * itemH) / itemH;
        m_dropMode = (frac < 0.25f) ? 0 : (frac > 0.75f) ? 2 : 1;
    }
    bool _isAncestor(JTreeViewNode* a, JTreeViewNode* b) {
        if (a == b) return true;
        for (auto& c : a->children) if (_isAncestor(&c, b)) return true;
        return false;
    }
    std::vector<int> _pathOf(JTreeViewNode* target) {
        std::vector<int> path;
        std::function<bool(JTreeViewNode&)> rec = [&](JTreeViewNode& n) -> bool {
            for (int i = 0; i < (int)n.children.size(); ++i) {
                path.push_back(i);
                if (&n.children[i] == target) return true;
                if (rec(n.children[i])) return true;
                path.pop_back();
            }
            return false;
        };
        if (rec(m_root)) return path;
        return {};
    }
    JTreeViewNode* _atPath(const std::vector<int>& p) {
        JTreeViewNode* n = &m_root;
        for (int i : p) { if (i < 0 || i >= (int)n->children.size()) return nullptr; n = &n->children[i]; }
        return n;
    }
    // Move the node at srcPath to be child #destIdx of the node at destParentPath, correcting for the
    // index shifts that removing the source introduces (same parent, or a dest branch past the source).
    bool _moveNode(std::vector<int> srcPath, std::vector<int> destParentPath, int destIdx) {
        if (srcPath.empty()) return false;
        int srcIdx = srcPath.back();
        std::vector<int> srcParentPath(srcPath.begin(), srcPath.end() - 1);
        JTreeViewNode* srcParent = _atPath(srcParentPath);
        if (!srcParent || srcIdx < 0 || srcIdx >= (int)srcParent->children.size()) return false;
        JTreeViewNode moved = std::move(srcParent->children[srcIdx]);
        srcParent->children.erase(srcParent->children.begin() + srcIdx);
        // If the destination path descends through srcParent past the removed index, shift it down one.
        if (destParentPath.size() > srcParentPath.size() &&
            std::equal(srcParentPath.begin(), srcParentPath.end(), destParentPath.begin())) {
            int& branch = destParentPath[srcParentPath.size()];
            if (branch > srcIdx) branch -= 1;
        }
        JTreeViewNode* destParent = _atPath(destParentPath);
        if (!destParent) {   // shouldn't happen; put it back where it came from
            srcParent->children.insert(srcParent->children.begin() + std::min(srcIdx, (int)srcParent->children.size()), std::move(moved));
            return false;
        }
        if (destParent == srcParent && destIdx > srcIdx) destIdx -= 1;
        destIdx = std::clamp(destIdx, 0, (int)destParent->children.size());
        destParent->children.insert(destParent->children.begin() + destIdx, std::move(moved));
        return true;
    }

    JTreeViewNode  m_root;
    JTreeViewNode* m_selectedNode{nullptr};
    bool           m_multiSelect{false};        // Ctrl/Shift multi-select (opt-in)
    JTreeViewNode* m_anchorNode{nullptr};       // range-select anchor
    JTreeViewNode* m_pendingCollapse{nullptr};  // click-on-selected: collapse to this on release-without-drag
    JTreeViewNode* m_pendingSelect{nullptr};    // single-click candidate: select+activate on release IF no drag intervened
    JTreeViewNode* m_hoverNode{nullptr};        // row under the cursor (for the hyperlink-hover underline)
    JTreeViewNode* m_editNode{nullptr};   // node whose label is being edited in place
    JLineEdit      m_editField;           // the real text editor mounted over a row during an in-place rename
    bool           m_editable{true};      // F2 / click-selected may start an in-place rename
    std::string    m_filter;              // lower-cased row filter ("" = show all)
    bool           m_filterUserData{false}; // also match userData (raw id / binding path), not just the label
    float         m_scrollY{0.0f};
    float         m_rowHeight{-1.0f};
    bool          m_draggingScroll{false};
    float         m_dragStartY{0.0f};
    float         m_dragStartScrollY{0.0f};
    JTreeViewNode* m_pressNode{nullptr};   // node pressed (candidate for a drag), cleared once the drag fires/ends
    float          m_pressX{0.0f}, m_pressY{0.0f};
    bool           m_internalReorder{false};   // drag = in-tree move (vs. onNodeDragStarted external DnD)
    bool           m_dragEnabled{true};         // whether a node drag may start at all (off = a pure navigator, e.g. run mode)
    bool           m_dragging{false};          // an internal-reorder drag is live
    bool           m_externalLive{false};      // the drag's external payload has been observed active (drop-out guard)
    JTreeViewNode* m_dropTarget{nullptr};      // row under the cursor during a drag
    int            m_dropMode{0};              // 0 = insert before, 1 = drop as child, 2 = insert after
    JTreeViewNode* m_lastClickNode{nullptr};                 // double-click-to-rename tracking
    std::chrono::steady_clock::time_point m_lastClickTime{};
};

} // inline namespace jf
