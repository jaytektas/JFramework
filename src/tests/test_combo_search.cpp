// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// test_combo_search.cpp — a searchable combo finds what is typed.
//
// JComboBox::Search narrows a long list to the entries that answer what is typed: every word, in any case,
// in the entry or its cells; an entry one of them starts before one that only holds it; in the scope
// chosen; ranked by the app's own measure when it gives one.

#include <j/core/JComboBox.h>
#include <j/core/SceneGraph.h>

#include <cstdio>
#include <vector>

using namespace jf;

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

static void test_every_word_in_the_entry_or_its_cells() {
    JSceneGraph g;
    JComboBox cb(g, { "R_0603_10k", "C_0603_100n", "LQFP-48", "C_0805_10u" });
    JComboBox::Search s;
    s.columns = { "Part", "Kind" };
    s.cells = { { "R_0603_10k", "Resistor" }, { "C_0603_100n", "Capacitor" }, { "LQFP-48", "MCU" }, { "C_0805_10u", "Capacitor" } };
    cb.setSearch(s);
    CHECK((cb.searchCells(2) == std::vector<std::string>{ "LQFP-48", "MCU" }));
    CHECK(cb.isSearchable());
    CHECK((cb.searchMatches("") == std::vector<int>{ 0, 1, 2, 3 }));       // nothing typed: all, in order
    CHECK((cb.searchMatches("lq") == std::vector<int>{ 2 }));              // any case
    CHECK((cb.searchMatches("0603 cap") == std::vector<int>{ 1 }));        // every word, its cells too
    CHECK((cb.searchMatches("capacitor") == std::vector<int>{ 1, 3 }));
    CHECK(cb.searchMatches("zzz").empty());
}

static void test_a_start_comes_first() {
    JSceneGraph g;
    JComboBox cb(g, { "Crystal", "R_0603_10k", "C_0603_100n" });
    cb.setSearch({});
    CHECK((cb.searchCells(1) == std::vector<std::string>{ "R_0603_10k" }));   // no columns: the entry itself
    // "r" is in the first two; the one it starts comes first.
    CHECK((cb.searchMatches("r") == std::vector<int>{ 1, 0 }));
}

static void test_the_scope_and_the_apps_rank() {
    JSceneGraph g;
    JComboBox cb(g, { "a1", "a2", "b1", "b2" });
    JComboBox::Search s;
    s.scopes = { "All", "Bs" };
    s.inScope = [](int scope, int i) { return scope == 0 || i >= 2; };
    s.rank = [](int i, const std::string&) { return i % 2 == 0 ? 1 : 0; };   // odd entries first
    cb.setSearch(s);
    CHECK((cb.searchMatches("") == std::vector<int>{ 1, 3, 0, 2 }));
    cb.setSearchScope(1);
    CHECK((cb.searchMatches("") == std::vector<int>{ 3, 2 }));
}

int main() {
    test_every_word_in_the_entry_or_its_cells();
    test_a_start_comes_first();
    test_the_scope_and_the_apps_rank();
    if (g_fails) { std::printf("test_combo_search: %d failure(s)\n", g_fails); return 1; }
    std::printf("test_combo_search: OK\n");
    return 0;
}
