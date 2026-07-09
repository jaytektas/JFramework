#include <j/core/Command.h>
#include <j/core/UndoStack.h>
#include <cassert>
#include <cstdio>

using namespace jf;

// A value-setting command with optional merge (same id absorbs the next, keeping the newest value).
struct SetVal : JUndoCommand {
    int* target; int oldV, newV; int mid;
    SetVal(int* t, int v, int id = -1) : target(t), oldV(*t), newV(v), mid(id) {}
    void redo() override { *target = newV; }
    void undo() override { *target = oldV; }
    std::string text() const override { return "Set"; }
    int id() const override { return mid; }
    bool mergeWith(const JUndoCommand* n) override {
        newV = static_cast<const SetVal*>(n)->newV; return true;   // keep our oldV, take their newV
    }
};

int main() {
    // 1) Basic push / undo / redo -------------------------------------------------
    {
        int v = 0; JUndoStack s;
        int idxEvents = 0; s.indexChanged.connect([&](int){ ++idxEvents; });
        s.push(new SetVal(&v, 10));  assert(v == 10);
        s.push(new SetVal(&v, 20));  assert(v == 20);
        s.push(new SetVal(&v, 30));  assert(v == 30);
        assert(s.count() == 3 && s.index() == 3);
        assert(s.canUndo() && !s.canRedo());
        s.undo(); assert(v == 20);
        s.undo(); assert(v == 10);
        assert(s.canUndo() && s.canRedo());
        s.redo(); assert(v == 20);
        s.undo(); s.undo(); assert(v == 0 && !s.canUndo());
        s.undo(); assert(v == 0);                  // no-op past the start
        assert(idxEvents >= 6);
        assert(s.undoText().empty());
        s.redo(); assert(v == 10); assert(s.redoText() == "Set");
    }

    // 2) Redo-tail truncation: pushing after an undo discards the tail --------------
    {
        int v = 0; JUndoStack s;
        s.push(new SetVal(&v, 1));
        s.push(new SetVal(&v, 2));
        s.undo(); assert(v == 1 && s.canRedo());
        s.push(new SetVal(&v, 9));                 // truncates the redoable "2"
        assert(v == 9 && !s.canRedo() && s.count() == 2);
    }

    // 3) Merge: consecutive same-id commands collapse to one undo step --------------
    {
        int v = 0; JUndoStack s;
        s.push(new SetVal(&v, 5,  /*id*/7));
        s.push(new SetVal(&v, 6,  /*id*/7));       // merges
        s.push(new SetVal(&v, 7,  /*id*/7));       // merges
        assert(v == 7 && s.count() == 1);
        s.undo(); assert(v == 0);                  // one undo reverts the whole burst
        assert(!s.canUndo());
        // Different id does NOT merge.
        s.clear(); v = 0;
        s.push(new SetVal(&v, 1, 1));
        s.push(new SetVal(&v, 2, 2));
        assert(s.count() == 2);
    }

    // 4) Clean-state tracking -------------------------------------------------------
    {
        int v = 0; JUndoStack s;
        int cleanEvents = 0; s.cleanChanged.connect([&](bool){ ++cleanEvents; });
        s.push(new SetVal(&v, 1));
        s.setClean(); assert(s.isClean());
        s.push(new SetVal(&v, 2)); assert(!s.isClean());
        s.undo(); assert(s.isClean());             // back at the saved point
        s.undo(); assert(!s.isClean());
        s.redo(); assert(s.isClean());
        assert(cleanEvents >= 2);
    }

    // 5) Undo limit trims the oldest ------------------------------------------------
    {
        int v = 0; JUndoStack s; s.setUndoLimit(2);
        for (int i = 1; i <= 5; ++i) s.push(new SetVal(&v, i));
        assert(v == 5 && s.count() == 2);          // only the last 2 retained
        s.undo(); s.undo(); assert(!s.canUndo());  // can only walk back 2
    }

    // 6) Macro: several pushes undo/redo as ONE step --------------------------------
    {
        int v = 0; JUndoStack s;
        s.push(new SetVal(&v, 1));
        s.beginMacro("burst");
        s.push(new SetVal(&v, 10));
        s.push(new SetVal(&v, 20));
        s.endMacro();
        assert(v == 20 && s.count() == 2);
        s.undo(); assert(v == 1);                  // whole macro reverts at once
        s.redo(); assert(v == 20);                 // whole macro replays at once
    }

    // 7) JUndoGroup routes to the active stack --------------------------------------
    {
        int a = 0, b = 0; JUndoStack sa, sb; JUndoGroup g;
        g.add(&sa); g.add(&sb); g.setActive(&sa);
        sa.push(new SetVal(&a, 1));
        sb.push(new SetVal(&b, 1));
        g.undo(); assert(a == 0 && b == 1);        // undoes the ACTIVE (sa) only
        g.setActive(&sb); g.undo(); assert(b == 0);
    }

    std::puts("ALL UNDO/REDO TESTS PASSED");
    return 0;
}
