#pragma once

// ============================================================================
// ModelBinding — free functions to bind Genesis models to widgets.
//
// Include this header when you want to connect a TableModel/TreeModel to a
// DataGrid or TreeView. The binding stays live until the returned SlotTracker
// is destroyed (or its disconnectAll() is called).
//
// Usage:
//   TableModel model;
//   DataGrid   grid(graph, {"Name","Value"});
//   auto conn = bindDataGrid(grid, model);  // grid stays in sync with model
//
//   SortFilterModel proxy(model);
//   proxy.sort(0);
//   auto conn2 = bindDataGrid(grid, proxy);
//
//   TreeModel treeModel;
//   TreeView  tree(graph);
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

inline TreeViewNode treeItemToNode(const TreeItem& item) {
    TreeViewNode node;
    node.label    = item.label;
    node.expanded = item.expanded;
    node.selected = false;
    for (const auto& child : item.children)
        node.children.push_back(treeItemToNode(child));
    return node;
}

} // namespace detail

// ---- DataGrid ↔ TableModel --------------------------------------------------

inline Core::SlotTracker bindDataGrid(DataGrid& grid, TableModel& model) {
    grid.setHeaders(model.headers());
    grid.setRows(model.rows());
    Core::SlotTracker tracker;
    tracker.addConnection(model.onChanged.connect([&grid, &model]{
        grid.setHeaders(model.headers());
        grid.setRows(model.rows());
    }));
    return tracker;
}

// ---- DataGrid ↔ SortFilterModel ---------------------------------------------

inline Core::SlotTracker bindDataGrid(DataGrid& grid, SortFilterModel& proxy) {
    grid.setHeaders(proxy.headers());
    grid.setRows(proxy.rows());
    Core::SlotTracker tracker;
    tracker.addConnection(proxy.onChanged.connect([&grid, &proxy]{
        grid.setHeaders(proxy.headers());
        grid.setRows(proxy.rows());
    }));
    return tracker;
}

// ---- TreeView ↔ TreeModel ---------------------------------------------------

inline Core::SlotTracker bindTreeView(TreeView& tree, TreeModel& model) {
    // Build root node from TreeModel root's children (TreeView shows children of root).
    auto sync = [&tree, &model]{
        TreeViewNode root = detail::treeItemToNode(model.root());
        tree.setRootNode(std::move(root));
    };
    sync();
    Core::SlotTracker tracker;
    tracker.addConnection(model.onChanged.connect([sync]{ sync(); }));
    return tracker;
}

} // namespace Genesis
