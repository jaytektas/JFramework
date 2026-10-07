// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// test_menu_gone.cpp — a menu says when it is destroyed (JMenuManager::onMenuGone), with its own address, so the
// menu runtime can put away an open popup built from it before anything reads it.
//
// The crash this guards: an app made a menu afresh (or lost the panel owning it, made again after a setting was
// saved) while its popup was open; hovering the popup's submenu entry then built a popup from the freed menu.

#include <j/core/MenuSystem.h>
#include <j/core/SceneGraph.h>

#include <cstdio>
#include <memory>
#include <vector>

using namespace jf;

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

int main() {
    std::vector<const JMenu*> gone;
    JMenuManager::instance().onMenuGone = [&gone](const JMenu* m) { gone.push_back(m); };
    JSceneGraph graph;
    const JMenu* address = nullptr;
    {
        auto sub = std::make_unique<JMenu>("Manual Change");
        auto menu = std::make_unique<JMenu>("Nozzle Tip");
        menu->add(graph, "Load");
        menu->add(graph, "Manual Change", {}, sub.get());
        address = menu.get();
        menu.reset();                       // made afresh: the old one goes first
        CHECK(gone.size() == 1 && gone[0] == address);
        sub.reset();
        CHECK(gone.size() == 2);
    }
    JMenuManager::instance().onMenuGone = nullptr;
    { JMenu quiet("No one listening"); }    // none set: nothing called
    CHECK(gone.size() == 2);
    std::printf(g_fails ? "menu gone: FAILED (%d)\n" : "menu gone: OK\n", g_fails);
    return g_fails ? 1 : 0;
}
