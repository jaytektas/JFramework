// Headless test for the JFramework model/view foundation (j/model/*).
// Compile:
//   g++ -std=c++20 -I<repo>/include -I<repo>/third_party \
//       tests/model_view_test.cpp -o /tmp/mv_test
// Prints PASS/FAIL per case; exits non-zero on any failure.

#include "j/model/ItemModel.h"
#include "j/model/SelectionModel.h"
#include "j/model/AbstractItemView.h"
#include "j/model/ItemDelegate.h"

#include <cstdio>

using namespace jf;

static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}

int main() {
    // 1) JStringListModel structure + data read.
    {
        JStringListModel m({"crank", "cam", "map"});
        check("StringListModel rowCount==3", m.rowCount() == 3);
        check("StringListModel columnCount==1", m.columnCount() == 1);
        check("StringListModel data(index(1),Display)=='cam'",
              m.index(1, 0).data(JItemRole::Display).toString() == "cam");
        check("StringListModel index(9) invalid", !m.index(9, 0).isValid());
    }

    // 2) setData mutates AND fires dataChanged with the right index.
    {
        JStringListModel m({"crank", "cam", "map"});
        bool fired = false;
        JModelIndex firedTL, firedBR;
        m.dataChanged.connect([&](JModelIndex tl, JModelIndex br) {
            fired = true; firedTL = tl; firedBR = br;
        });
        bool ok = m.setData(m.index(1, 0), JVariant("intake-cam"), roleId(JItemRole::Edit));
        check("StringListModel setData returns true", ok);
        check("StringListModel value updated",
              m.data(m.index(1, 0)).toString() == "intake-cam");
        check("StringListModel dataChanged fired", fired);
        check("StringListModel dataChanged carried row 1",
              firedTL.row() == 1 && firedBR.row() == 1);
    }

    // 3) rowsInserted / rowsRemoved signals.
    {
        JStringListModel m({"a", "b"});
        int insFirst = -1, insLast = -1, remFirst = -1;
        m.rowsInserted.connect([&](JModelIndex, int f, int l) { insFirst = f; insLast = l; });
        m.rowsRemoved.connect([&](JModelIndex, int f, int) { remFirst = f; });
        m.append("c");
        check("StringListModel append -> rowsInserted@2", insFirst == 2 && insLast == 2);
        check("StringListModel rowCount==3 after append", m.rowCount() == 3);
        m.removeRow(0);
        check("StringListModel removeRow -> rowsRemoved@0", remFirst == 0);
        check("StringListModel rowCount==2 after remove", m.rowCount() == 2);
    }

    // 4) JItemSelectionModel: current + selection + signals.
    {
        JStringListModel m({"a", "b", "c"});
        JItemSelectionModel sel(&m, JItemSelectionModel::SelectionMode::Extended);

        bool curFired = false;
        JModelIndex gotCur, gotPrev;
        sel.currentChanged.connect([&](JModelIndex c, JModelIndex p) {
            curFired = true; gotCur = c; gotPrev = p;
        });
        bool selFired = false;
        sel.selectionChanged.connect([&](std::vector<JModelIndex> s, std::vector<JModelIndex>) {
            if (!s.empty()) selFired = true;
        });

        JModelIndex idx2 = m.index(2, 0);
        sel.setCurrentIndex(idx2, JItemSelectionModel::Select | JItemSelectionModel::Current);

        check("SelectionModel isSelected(index 2)", sel.isSelected(idx2));
        check("SelectionModel currentChanged fired", curFired);
        check("SelectionModel currentIndex==2", sel.currentIndex().row() == 2);
        check("SelectionModel selectionChanged fired", selFired);
        check("SelectionModel not selected other", !sel.isSelected(m.index(0, 0)));

        // Single mode replaces prior selection.
        sel.setSelectionMode(JItemSelectionModel::SelectionMode::Single);
        sel.select(m.index(0, 0), JItemSelectionModel::Select);
        check("SelectionModel single-mode replaced set",
              sel.isSelected(m.index(0, 0)) && !sel.isSelected(idx2) &&
              sel.selectedIndexes().size() == 1);

        sel.clearSelection();
        check("SelectionModel clearSelection empties", !sel.hasSelection());
    }

    // 5) JGridModel 2x2 round-trip.
    {
        JGridModel t(2, 2);
        check("TableModel rowCount==2", t.rowCount() == 2);
        check("TableModel columnCount==2", t.columnCount() == 2);
        t.setData(t.index(0, 0), JVariant(11));
        t.setData(t.index(0, 1), JVariant("hi"));
        t.setData(t.index(1, 0), JVariant(3.5));
        t.setData(t.index(1, 1), JVariant(true));
        check("TableModel (0,0)==11", t.data(t.index(0, 0)).toInt() == 11);
        check("TableModel (0,1)=='hi'", t.data(t.index(0, 1)).toString() == "hi");
        check("TableModel (1,0)==3.5", t.data(t.index(1, 0)).toDouble() == 3.5);
        check("TableModel (1,1)==true", t.data(t.index(1, 1)).toBool() == true);

        bool tblFired = false;
        t.dataChanged.connect([&](JModelIndex, JModelIndex) { tblFired = true; });
        t.setData(t.index(1, 1), JVariant(false));
        check("TableModel setData fires dataChanged", tblFired);
        check("TableModel (1,1) updated to false", t.data(t.index(1, 1)).toBool() == false);
    }

    // 6) JModelIndex parent()/validity + delegate display text.
    {
        JStringListModel m({"x", "y"});
        JModelIndex i = m.index(0, 0);
        check("ModelIndex valid + flat parent invalid", i.isValid() && !i.parent().isValid());
        check("ModelIndex default invalid", !JModelIndex().isValid());
        JStyledItemDelegate d;
        check("Delegate displayText matches Display role", d.displayText(i) == "x");
    }

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
