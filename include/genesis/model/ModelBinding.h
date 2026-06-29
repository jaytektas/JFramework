#pragma once

// ============================================================================
// ModelBinding — free functions to bind Genesis models to widgets.
//
// Include this header when you want to connect a JTableModel/JTreeModel to a
// JDataGrid or JTreeView. The binding stays live until the returned JSlotTracker
// is destroyed (or its disconnectAll() is called).
//
// Usage:
//   JTableModel model;
//   JDataGrid   grid(graph, {"Name","Value"});
//   auto conn = bindDataGrid(grid, model);  // grid stays in sync with model
//
//   JSortFilterModel proxy(model);
//   proxy.sort(0);
//   auto conn2 = bindDataGrid(grid, proxy);
//
//   JTreeModel treeModel;
//   JTreeView  tree(graph);
//   auto conn3 = bindTreeView(tree, treeModel);
//
// Lifetime: conn, conn2, conn3 must outlive grid/tree or be explicitly
// disconnected before either the widget or the model is destroyed.
// ============================================================================

#include <genesis/model/TableModel.h>
#include <genesis/model/TreeModel.h>
#include <genesis/model/SortFilterModel.h>
#include <genesis/core/BaseWidgets.h>

namespace Genesis {

// ---- Internal helpers -------------------------------------------------------

namespace detail {

inline JTreeViewNode treeItemToNode(const JTreeItem& item) {
    JTreeViewNode node;
    node.label    = item.label;
    node.expanded = item.expanded;
    node.selected = false;
    for (const auto& child : item.children)
        node.children.push_back(treeItemToNode(child));
    return node;
}

} // namespace detail

// ---- JDataGrid ↔ JTableModel --------------------------------------------------

inline Core::JSlotTracker bindDataGrid(JDataGrid& grid, JTableModel& model) {
    grid.setHeaders(model.headers());
    grid.setRows(model.rows());
    Core::JSlotTracker tracker;
    tracker.addConnection(model.onChanged.connect([&grid, &model]{
        grid.setHeaders(model.headers());
        grid.setRows(model.rows());
    }));
    return tracker;
}

// ---- JDataGrid ↔ JSortFilterModel ---------------------------------------------

inline Core::JSlotTracker bindDataGrid(JDataGrid& grid, JSortFilterModel& proxy) {
    grid.setHeaders(proxy.headers());
    grid.setRows(proxy.rows());
    Core::JSlotTracker tracker;
    tracker.addConnection(proxy.onChanged.connect([&grid, &proxy]{
        grid.setHeaders(proxy.headers());
        grid.setRows(proxy.rows());
    }));
    return tracker;
}

// ---- JTreeView ↔ JTreeModel ---------------------------------------------------

inline Core::JSlotTracker bindTreeView(JTreeView& tree, JTreeModel& model) {
    // Build root node from JTreeModel root's children (JTreeView shows children of root).
    auto sync = [&tree, &model]{
        JTreeViewNode root = detail::treeItemToNode(model.root());
        tree.setRootNode(std::move(root));
    };
    sync();
    Core::JSlotTracker tracker;
    tracker.addConnection(model.onChanged.connect([sync]{ sync(); }));
    return tracker;
}

} // namespace Genesis
