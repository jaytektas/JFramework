#pragma once

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <genesis/core/Signal.h>
#include <genesis/core/MainThreadDispatcher.h>

namespace Genesis {

// ============================================================================
// TreeItem — node in a TreeModel tree.
//
// id    — unique key; use for programmatic find/update/remove.
// label — display text shown in TreeView.
// tag   — optional app payload (run ID, channel number, serialised JSON, …).
// ============================================================================
struct TreeItem {
    std::string id;
    std::string label;
    std::string tag;
    bool expanded{false};
    std::vector<TreeItem> children;
};

// ============================================================================
// TreeModel — observable tree of TreeItems.
//
// onChanged fires on the main thread via MainThreadDispatcher, so mutations
// from a background thread are safe. However, since trees are almost always
// managed by user-interaction code, mutations from the main thread are the
// common case.
//
// Usage:
//   TreeModel model;
//   model.insert("", {"lib0", "Engine Tests"});       // child of root
//   model.insert("lib0", {"run1", "Run 2024-01-15"});
//   model.setExpanded("lib0", true);
//
//   // Bind to a TreeView (include <genesis/model/ModelBinding.h>):
//   auto conn = bindTreeView(treeView, model);
// ============================================================================
class TreeModel {
public:
    Core::Signal<> onChanged;  // always fires on the main thread

    TreeModel() : m_root{"", "Root", "", true, {}} {}

    ~TreeModel() {
        m_alive->store(false, std::memory_order_release);
    }

    // ---- Root access --------------------------------------------------------
    TreeItem&       root()       { return m_root; }
    const TreeItem& root() const { return m_root; }

    // ---- Find ---------------------------------------------------------------
    TreeItem*       find(const std::string& id)       { return _find(m_root, id); }
    const TreeItem* find(const std::string& id) const { return _find(const_cast<TreeItem&>(m_root), id); }

    // ---- Mutations ----------------------------------------------------------

    // Insert as a child of parentId (empty string = root). pos=-1 appends.
    bool insert(const std::string& parentId, TreeItem item, int pos = -1) {
        TreeItem* parent = parentId.empty() ? &m_root : _find(m_root, parentId);
        if (!parent) return false;
        auto& children = parent->children;
        if (pos < 0 || pos >= (int)children.size())
            children.push_back(std::move(item));
        else
            children.insert(children.begin() + pos, std::move(item));
        _notify();
        return true;
    }

    bool remove(const std::string& id) {
        if (!_remove(m_root, id)) return false;
        _notify();
        return true;
    }

    bool setLabel(const std::string& id, const std::string& label) {
        if (auto* n = _find(m_root, id)) { n->label = label; _notify(); return true; }
        return false;
    }

    bool setTag(const std::string& id, const std::string& tag) {
        if (auto* n = _find(m_root, id)) { n->tag = tag; _notify(); return true; }
        return false;
    }

    bool setExpanded(const std::string& id, bool expanded) {
        if (auto* n = _find(m_root, id)) { n->expanded = expanded; _notify(); return true; }
        return false;
    }

    void clear() {
        m_root.children.clear();
        _notify();
    }

private:
    TreeItem                               m_root;
    std::shared_ptr<std::atomic<bool>>     m_alive{
        std::make_shared<std::atomic<bool>>(true)};

    void _notify() {
        auto alive = m_alive;
        MainThreadDispatcher::instance().post([this, alive]{
            if (alive->load(std::memory_order_acquire)) onChanged.emit();
        });
    }

    static TreeItem* _find(TreeItem& node, const std::string& id) {
        if (node.id == id) return &node;
        for (auto& child : node.children)
            if (auto* p = _find(child, id)) return p;
        return nullptr;
    }

    static bool _remove(TreeItem& parent, const std::string& id) {
        for (auto it = parent.children.begin(); it != parent.children.end(); ++it) {
            if (it->id == id) { parent.children.erase(it); return true; }
            if (_remove(*it, id)) return true;
        }
        return false;
    }
};

} // namespace Genesis
