// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// FLOW LAYOUT: children at their own size along a line, a new line where the width runs out.
//
// Five buttons 60 wide, 10 apart, in a container 200 wide hold two lines of three and two (60 + 10 +
// 60 + 10 + 60 = 200 fits exactly; a fourth would not); the lines 20 and 30 tall (each as tall as its
// tallest child), 10 apart, so the container is 60 tall. Made wider, they take one line. A child
// centred in a taller line sits in its middle. The minimum is the widest and tallest child.
#include <j/core/SceneGraph.h>

#include <cstdio>
#include <string>

using namespace jf;

static int fails = 0;
static void check(const char* what, bool ok, const std::string& d = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, d.empty() ? "" : " — ", d.c_str());
    if (!ok) ++fails;
}

int main() {
    JSceneGraph g;
    const NodeId root = g.createNode("flow");
    {
        auto& L = g.getLayout(root);
        L.mode = JLayoutMode::Flow;
        L.gap = 10.f;
        L.alignItems = JAlignItems::Center;
    }
    NodeId kids[5];
    const float heights[5] = { 20, 20, 20, 30, 20 };
    for (int i = 0; i < 5; ++i) {
        kids[i] = g.createNode("button");
        auto& kl = g.getLayout(kids[i]);
        kl.boundingBox.width = kl.minWidth = 60.f;
        kl.boundingBox.height = kl.minHeight = heights[i];
        g.addChild(root, kids[i]);
    }

    g.computeMinSize(root);
    check("the minimum is the widest child", g.getLayoutConst(root).minWidth == 60.f);
    check("the minimum is the tallest child", g.getLayoutConst(root).minHeight == 30.f);

    g.getLayout(root).boundingBox = JRect{ 0, 0, 200, 500 };
    g.computeLayout(root, JConstraints{ 200, 200, 0, 500 });
    auto at = [&](int i) { return g.getLayoutConst(kids[i]).boundingBox; };
    check("three to the first line", at(0).y == 0 && at(1).y == 0 && at(2).y == 0 && at(2).x == 140,
          std::to_string(at(2).x));
    check("the fourth starts the second line", at(3).x == 0 && at(3).y == 30, std::to_string(at(3).y));
    check("a shorter child centred in its line", at(4).x == 70 && at(4).y == 35, std::to_string(at(4).y));
    check("as tall as its lines", g.getLayoutConst(root).boundingBox.height == 60,
          std::to_string(g.getLayoutConst(root).boundingBox.height));

    g.invalidateNode(root);   // resized: laid out again
    g.computeLayout(root, JConstraints{ 400, 400, 0, 500 });
    check("wider, one line (centred in it)", at(4).y == 5 && at(4).x == 280 && g.getLayoutConst(root).boundingBox.height == 30,
          std::to_string(at(4).x) + ", " + std::to_string(at(4).y) + ", " + std::to_string(g.getLayoutConst(root).boundingBox.height));

    std::printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
