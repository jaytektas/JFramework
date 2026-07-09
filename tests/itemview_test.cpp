// Headless test for the concrete model/view widgets (j/core/ItemView.h):
// a JItemListView bound to a JStringListModel — model reflection, keyboard current-
// index navigation, click-to-select through the selection model, and repaint on
// model dataChanged. Also spot-checks that a JContainer honours JSizePolicy so a
// child's Expanding policy actually claims leftover space.
//
// Compile:
//   g++ -std=c++20 -I<repo>/include -I<repo>/third_party \
//       tests/itemview_test.cpp -o /tmp/iv_test
// Prints PASS/FAIL per case; exits non-zero on any failure.

#include "j/core/ItemView.h"
#include "j/model/ItemModel.h"

#include <cstdio>
#include <cmath>

using namespace jf;

static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}
static bool feq(float a, float b) { return std::fabs(a - b) < 0.5f; }

int main() {
    JSceneGraph graph;

    // ---- Model reflected by the view --------------------------------------
    JStringListModel model({"crank", "cam", "map", "clt", "tps"});
    JItemListView view(graph, 200.f, 200.f);
    view.setModel(&model);

    check("view rowCount reflects model (5)", view.rowCount() == 5);
    check("view has an owned selection model", view.selectionModel() != nullptr);
    check("initial current index is invalid", !view.currentIndex().isValid());

    JItemSelectionModel* sel = view.selectionModel();

    // ---- Keyboard current-index navigation --------------------------------
    // Seed current at row 0, then Down should move 0 -> 1.
    sel->setCurrentIndex(model.index(0, 0),
                         JItemSelectionModel::ClearAndSelect | JItemSelectionModel::Current);
    check("current starts at row 0", view.currentIndex().row() == 0);

    auto key = [](JKeyEvent::JKey k) {
        JKeyEvent e; e.key = k; e.pressed = true; return e;
    };
    view.handleKeyEvent(key(JKeyEvent::JKey::Down));
    check("Down moves current 0 -> 1", view.currentIndex().row() == 1);

    view.handleKeyEvent(key(JKeyEvent::JKey::End));
    check("End moves current to last (4)", view.currentIndex().row() == 4);

    view.handleKeyEvent(key(JKeyEvent::JKey::Home));
    check("Home moves current to first (0)", view.currentIndex().row() == 0);

    view.handleKeyEvent(key(JKeyEvent::JKey::Up));
    check("Up at row 0 clamps to 0", view.currentIndex().row() == 0);

    // ---- Click-to-select sets the selection model -------------------------
    // Row height defaults to 22; the widget box is at origin (0,0). Row 2 spans
    // y in [44,66); click its middle.
    view.setRowHeight(22.f);
    view.handleMousePress(10.f, 44.f + 11.f);
    check("click hit-tests row 2", view.currentIndex().row() == 2);
    check("click selects row 2 in the selection model",
          sel->isSelected(model.index(2, 0)));
    check("plain click replaced the selection (size 1)",
          sel->selectedIndexes().size() == 1);

    // Ctrl+click a second row toggles it into a multi-selection.
    JWidget::s_ctrlDown = true;
    view.handleMousePress(10.f, 0.f + 11.f);   // row 0
    JWidget::s_ctrlDown = false;
    check("ctrl+click adds row 0 (multi-select size 2)",
          sel->selectedIndexes().size() == 2 && sel->isSelected(model.index(0, 0)));

    // ---- dataChanged is observed by the view (repaint counter) ------------
    int before = view.repaintCount();
    model.setData(model.index(1, 0), JVariant(std::string("cam2")),
                  roleId(JItemRole::Edit));   // fires dataChanged
    check("view observed dataChanged (repaint counter advanced)",
          view.repaintCount() > before);

    // rowsInserted is observed too.
    before = view.repaintCount();
    model.append("iat");
    check("view observed rowsInserted", view.repaintCount() > before);
    check("view rowCount now 6", view.rowCount() == 6);

    // ---- Size-policy adoption: a JContainer honours Expanding -------------
    {
        JContainer box(graph, 300.f, 100.f);
        box.setDirection(JFlexDirection::JRow)->setGap(0.f)->setPadding(JEdges{0.f});

        JItemListView a(graph, 50.f, 80.f);   // Fixed-ish, Preferred default
        JItemListView b(graph, 50.f, 80.f);   // will be Expanding
        a.setHSizePolicy(JSizePolicyMode::Fixed);
        b.setHSizePolicy(JSizePolicyMode::Expanding, 1);
        box.add(&a)->add(&b);

        box.setBounds(JRect{0.f, 0.f, 300.f, 100.f});
        JPrimitiveBuffer buf;
        box.populateRenderPrimitives(buf);   // triggers computeLayout

        float wa = graph.getLayoutConst(a.getNodeId()).boundingBox.width;
        float wb = graph.getLayoutConst(b.getNodeId()).boundingBox.width;
        check("container keeps Fixed child ~50w", feq(wa, 50.f));
        check("container lets Expanding child claim leftover (>200w)", wb > 200.f);
    }

    if (g_fail == 0) std::printf("\nALL PASS\n");
    else             std::printf("\n%d FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
