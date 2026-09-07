// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// Searching a tree reaches rows the app's condition filter HIDES.
//
// The point of the feature: the node you cannot find by browsing is exactly the feature you have not
// enabled yet. So a search reveals hidden matches — dimmed, selectable, and gone again once the search
// clears — which is what lets someone search for a disabled feature, open its page, and switch it on.
//
// Pinned here because every one of these is a way it can silently go wrong:
//   - a hidden row stays hidden with NO search running
//   - a hidden row that matches is revealed, and marked dimmed
//   - the whole subtree under a revealed row is dimmed too (children of a hidden node are not themselves
//     flagged hidden — the flag lives on the ROW, not the node)
//   - a hidden row that does NOT match stays out; revealing is search-driven, not a blanket unhide
//   - a visible match is never dimmed
//   - the selection survives a re-filter while revealed, and drops when the search clears
//     (that second one is what makes the "enable it, then clear the search" flow work)

// Compile:
//   g++ -std=c++20 -I<repo>/include -I<repo>/third_party tests/tree_reveal_test.cpp -o /tmp/tree_reveal_test
// Prints PASS/FAIL per case; exits non-zero on any failure.

#include "j/core/JTreeView.h"
#include <cstdio>
#include <string>

using namespace jf;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("[tree-reveal] %-64s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

static JTreeViewNode leaf(const std::string& l) { JTreeViewNode n; n.label = l; return n; }

// The row for `label`, or nullptr when the tree isn't showing it.
static const JTreeView::JFlatNode* row(std::vector<JTreeView::JFlatNode>& flat, const std::string& label) {
    for (auto& f : flat) if (f.node->label == label) return &f;
    return nullptr;
}

int main() {
    JSceneGraph graph;
    JTreeView tv(graph);

    // Fuel is ENABLED; Nitrous is not (its condition is false) and owns a "Stage 1" page.
    JTreeViewNode root;
    JTreeViewNode fuel = leaf("Fuel");     fuel.expanded = true;  fuel.children.push_back(leaf("Injectors"));
    JTreeViewNode nitrous = leaf("Nitrous"); nitrous.expanded = true; nitrous.children.push_back(leaf("Stage 1"));
    root.children.push_back(fuel);
    root.children.push_back(nitrous);
    tv.setRootNode(std::move(root));

    tv.applyVisibility([](const JTreeViewNode& n) { return n.label != "Nitrous"; });

    // --- no search: hidden means hidden -------------------------------------------------------------
    auto flat = tv.getFlatNodes();
    check(row(flat, "Fuel") != nullptr,      "an enabled node is listed");
    check(row(flat, "Nitrous") == nullptr,   "a disabled node is NOT listed with no search running");
    check(row(flat, "Stage 1") == nullptr,   "…nor is its subtree");

    // --- searching reveals it, dimmed ---------------------------------------------------------------
    tv.setFilter("nitrous");
    flat = tv.getFlatNodes();
    const auto* nit = row(flat, "Nitrous");
    check(nit != nullptr,                    "a search REVEALS the disabled node it matches");
    check(nit && nit->dimmed,                "…and marks it dimmed, so it reads as off");
    const auto* st1 = row(flat, "Stage 1");
    check(st1 != nullptr,                    "the revealed node's subtree comes with it");
    check(st1 && st1->dimmed,                "…dimmed too (the flag is the ROW's, not the node's)");

    // --- a hidden row that doesn't match stays out --------------------------------------------------
    tv.setFilter("injectors");
    flat = tv.getFlatNodes();
    check(row(flat, "Nitrous") == nullptr,   "a search does not blanket-unhide: no match, still hidden");
    const auto* inj = row(flat, "Injectors");
    check(inj != nullptr && !inj->dimmed,    "a match in an enabled subtree is listed and NOT dimmed");

    // --- selection: kept while revealed, dropped when the search clears ------------------------------
    tv.setFilter("nitrous");
    tv.selectByPath("Nitrous");
    check(tv.selectedNode() != nullptr && tv.selectedNode()->label == "Nitrous",
          "a revealed row can be selected like any other");

    // A re-filter (any config edit re-runs the conditions) must not yank the selection away — the page it
    // opened is where the switch that enables the feature lives.
    tv.applyVisibility([](const JTreeViewNode& n) { return n.label != "Nitrous"; });
    check(tv.selectedNode() != nullptr && tv.selectedNode()->label == "Nitrous",
          "…and survives the conditions being re-applied while the search stands");

    // Clearing the search re-hides the row, so the selection standing on it goes too.
    tv.setFilter("");
    check(tv.selectedNode() == nullptr,      "clearing the search drops a selection it had revealed");

    // The whole point of the flow: enable the feature, and the node is simply there, undimmed.
    tv.applyVisibility([](const JTreeViewNode&) { return true; });
    flat = tv.getFlatNodes();
    const auto* on = row(flat, "Nitrous");
    check(on != nullptr && !on->dimmed,      "once enabled it joins the live tree, no search needed");

    std::printf("\n[tree-reveal] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
