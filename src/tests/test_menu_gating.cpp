// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// What a menu OFFERS: JMenu::shownItems(). A hidden item is left out entirely, and the separators around
// a hidden stretch collapse, so gating a menu never leaves a blank group, a leading or trailing rule, or
// two rules in a row.

#include <j/core/MenuSystem.h>
#include <j/core/SceneGraph.h>

#include <cstdio>
#include <string>

using namespace jf;

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

// "A|-|B" : labels for items, '-' for a separator.
static std::string shape(const JMenu& m) {
    std::string s;
    for (JWidget* w : m.shownItems()) {
        if (!s.empty()) s += "|";
        if (auto* mi = dynamic_cast<JMenuItem*>(w)) s += mi->label();
        else s += "-";
    }
    return s;
}

int main() {
    std::printf("=== menu gating ===\n");
    JSceneGraph g;

    JMenu m("Tools");
    JMenuItem* a = m.add(g, "A");
    m.addSeparator(g);
    JMenuItem* b = m.add(g, "B");
    JMenuItem* c = m.add(g, "C");
    m.addSeparator(g);
    JMenuItem* d = m.add(g, "D");

    CHECK(shape(m) == "A|-|B|C|-|D");

    // Hiding one of a group keeps the group and its rules.
    b->setVisible(false);
    CHECK(shape(m) == "A|-|C|-|D");

    // Hiding a whole group drops ONE of the two rules around it, not both, not neither.
    c->setVisible(false);
    CHECK(shape(m) == "A|-|D");

    // Hiding the first item: no leading rule.
    a->setVisible(false);
    CHECK(shape(m) == "D");

    // Hiding the last: no trailing rule.
    a->setVisible(true); d->setVisible(false);
    CHECK(shape(m) == "A");

    // Everything hidden: nothing offered.
    a->setVisible(false);
    CHECK(shape(m).empty());

    // Disabled is not hidden: it is still offered (greyed), and still does not fire.
    a->setVisible(true);
    a->setEnabled(false);
    CHECK(shape(m) == "A");
    CHECK(!a->isEnabled());

    std::printf(g_fails ? "  %d FAILED\n" : "  all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
