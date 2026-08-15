// Column-major grid fill — what a menu WRAPPED for height needs.
//
// Row-major reads across, so child 1 is the second cell of row 0. A list that wraps because it is too
// tall for the screen must not do that: its order has to keep running top-to-bottom, starting a new
// column only when it reaches the bottom. Otherwise every item moves the moment the list grows past one
// screen — and a menu people navigate by position becomes unusable exactly when it gets big.
#include <j/core/SceneGraph.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace jf;

static int fails = 0;
static void check(const char* what, bool ok, const std::string& d = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, d.empty() ? "" : " — ", d.c_str());
    if (!ok) ++fails;
}

// A root of `n` equal cells, `cols` wide, filled in the requested order.
static void build(JSceneGraph& g, NodeId& root, std::vector<NodeId>& kids, int n, int cols, bool colMajor) {
    root = g.createNode("root");
    auto& L = g.getLayout(root);
    L.mode = JLayoutMode::Grid;
    L.columns = cols;
    L.gridColumnMajor = colMajor;
    L.gap = 0.f;
    for (int i = 0; i < n; ++i) {
        NodeId k = g.createNode("item");
        auto& kl = g.getLayout(k);
        kl.minWidth = 100.f; kl.minHeight = 20.f;
        kl.boundingBox.width = 100.f; kl.boundingBox.height = 20.f;
        g.addChild(root, k);
        kids.push_back(k);
    }
    g.computeMinSize(root);
    g.computeLayout(root, JConstraints{ 300.f, 300.f, 0.f, 1000.f });
}

int main() {
    std::printf("=== grid column-major fill ===\n");

    // 7 children, 3 columns => 3 rows. Column-major fills 0,1,2 | 3,4,5 | 6.
    {
        JSceneGraph g; NodeId root{}; std::vector<NodeId> k;
        build(g, root, k, 7, 3, true);
        auto box = [&](int i) { return g.getLayoutConst(k[i]).boundingBox; };
        check("0,1,2 share a column", box(0).x == box(1).x && box(1).x == box(2).x,
              "x " + std::to_string(box(0).x) + "/" + std::to_string(box(1).x) + "/" + std::to_string(box(2).x));
        check("...and descend it", box(1).y > box(0).y && box(2).y > box(1).y);
        check("3 starts a NEW column", box(3).x > box(0).x, "x " + std::to_string(box(3).x));
        check("...back at the top", box(3).y == box(0).y);
        check("6 starts the third column", box(6).x > box(3).x && box(6).y == box(0).y);
    }

    // Row-major is unchanged: child 1 is the second CELL of the first row.
    {
        JSceneGraph g; NodeId root{}; std::vector<NodeId> k;
        build(g, root, k, 7, 3, false);
        const auto b0 = g.getLayoutConst(k[0]).boundingBox, b1 = g.getLayoutConst(k[1]).boundingBox;
        check("row-major still reads across", b1.x > b0.x && b1.y == b0.y);
    }

    // One column is the degenerate case a short list hits — it must stay a plain vertical list.
    {
        JSceneGraph g; NodeId root{}; std::vector<NodeId> k;
        build(g, root, k, 4, 1, true);
        auto box = [&](int i) { return g.getLayoutConst(k[i]).boundingBox; };
        check("a single column is still a list", box(0).x == box(3).x && box(3).y > box(0).y);
    }

    std::printf(fails ? "=== %d FAILED ===\n" : "=== column-major fills down, row-major across ===\n", fails);
    return fails ? 1 : 0;
}
