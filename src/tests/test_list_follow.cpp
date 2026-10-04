// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// A list that grows (a log) follows its newest row: at the end it says so, and
// scrolled back it does not; its place can be read and put back after setItems.
#include <j/core/JListView.h>
#include <j/core/SceneGraph.h>
#include <cstdio>

using namespace jf;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    JSceneGraph graph;
    JListView list(graph);
    list.setBounds({ 0.f, 0.f, 200.f, 100.f });
    std::vector<std::string> rows;
    for (int i = 0; i < 3; ++i) rows.push_back("row " + std::to_string(i));
    list.setItems(rows);
    check(list.isAtEnd(), "rows that fit: at the end already");
    for (int i = 3; i < 100; ++i) rows.push_back("row " + std::to_string(i));
    list.setItems(rows);
    check(!list.isAtEnd() && list.scrollY() == 0.f, "more rows than fit: at the top, not the end");
    list.scrollToEnd();
    check(list.isAtEnd() && list.scrollY() > 0.f, "scrolled to the end: at the end");
    const float back = list.scrollY() / 2;
    list.setScrollY(back);
    check(!list.isAtEnd(), "scrolled back: not at the end");
    rows.push_back("row 100");
    list.setItems(rows);
    list.setScrollY(back);
    check(list.scrollY() == back, "its place put back after new rows");
    std::printf(fails ? "=== %d FAILED ===\n" : "=== lists follow ===\n", fails);
    return fails ? 1 : 0;
}
