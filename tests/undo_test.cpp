// Headless test for the JFramework undo/redo command framework.
//
//   g++ -std=c++20 -I../include -I../third_party undo_test.cpp -o /tmp/undo_test
//
// Drives an int through JFunctionCommands and exercises: basic push/undo/redo,
// redo-branch truncation, id() merge compression, undo-limit trimming, the clean
// marker (+ cleanChanged signal), and beginMacro/endMacro-as-one-step.
// Prints PASS/FAIL per case; exits non-zero if any case fails.

#include <j/core/Command.h>
#include <j/core/UndoStack.h>

#include <cstdio>
#include <string>

using namespace jf;

static int g_fail = 0;

static void check(const char* name, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}

// Build a command that sets *v to `nv` (remembering the prior value for undo).
static JFunctionCommand* setCmd(int* v, int nv, const std::string& label, int mergeId = -1) {
    int old = *v;
    return new JFunctionCommand(
        label,
        [v, nv]  { *v = nv;  },
        [v, old] { *v = old; },
        mergeId);
}

int main() {
    // 1) Basic push / undo / redo -------------------------------------------------
    {
        int v = 0; JUndoStack s;
        s.push(setCmd(&v, 1, "inc"));
        s.push(setCmd(&v, 2, "inc"));
        s.push(setCmd(&v, 3, "inc"));
        bool ok = (v == 3) && s.canUndo() && !s.canRedo() && s.count() == 3 && s.index() == 3;
        check("push 3 increments -> value 3, canUndo, no redo", ok);

        s.undo(); s.undo();
        check("undo twice -> value 1, canRedo", v == 1 && s.canRedo());

        s.redo();
        check("redo -> value 2", v == 2);

        check("undoText/redoText reflect labels", s.undoText() == "inc" && s.redoText() == "inc");
    }

    // 2) Push after undo truncates the redo branch --------------------------------
    {
        int v = 0; JUndoStack s;
        s.push(setCmd(&v, 1, "a"));
        s.push(setCmd(&v, 2, "b"));
        s.undo();                                   // value 1, redo("b") available
        bool hadRedo = s.canRedo();
        s.push(setCmd(&v, 9, "c"));                 // truncates the "b" tail
        check("push after undo truncates redo branch",
              hadRedo && v == 9 && !s.canRedo() && s.count() == 2);
    }

    // 3) mergeWith compresses two same-id commands into one undo step -------------
    {
        int v = 0; JUndoStack s;
        s.push(setCmd(&v, 5, "type", /*id*/7));
        s.push(setCmd(&v, 6, "type", /*id*/7));     // merges into the first
        s.push(setCmd(&v, 7, "type", /*id*/7));     // merges again
        bool merged = (v == 7) && (s.count() == 1);
        s.undo();                                   // one undo reverts the whole burst
        check("mergeWith compresses same-id burst to one step",
              merged && v == 0 && !s.canUndo());

        // Different id must NOT merge.
        s.clear(); v = 0;
        s.push(setCmd(&v, 1, "x", 1));
        s.push(setCmd(&v, 2, "y", 2));
        check("different id does not merge", s.count() == 2);
    }

    // 4) setUndoLimit drops the oldest --------------------------------------------
    {
        int v = 0; JUndoStack s; s.setUndoLimit(2);
        for (int i = 1; i <= 5; ++i) s.push(setCmd(&v, i, "step"));
        bool ok = (v == 5) && (s.count() == 2);     // only the last two retained
        s.undo(); s.undo();
        check("setUndoLimit(2) keeps only newest two", ok && !s.canUndo());
    }

    // 5) Clean marker + cleanChanged signal ---------------------------------------
    {
        int v = 0; JUndoStack s;
        int cleanEvents = 0; bool lastClean = true;
        s.cleanChanged.connect([&](bool c){ ++cleanEvents; lastClean = c; });

        s.push(setCmd(&v, 1, "a"));                 // dirty (was clean at index 0)
        s.setClean();                               // mark saved here
        bool wasCleanAfterSave = s.isClean();
        int eventsBeforeMutate = cleanEvents;

        s.push(setCmd(&v, 2, "b"));                 // mutate -> dirty
        bool dirtyAfterMutate = !s.isClean();
        bool firedOnDirty = (cleanEvents > eventsBeforeMutate) && (lastClean == false);

        s.undo();                                   // back to the saved index -> clean
        bool cleanAgain = s.isClean() && (lastClean == true);

        check("clean marker: save/mutate/undo-back tracks isClean + fires cleanChanged",
              wasCleanAfterSave && dirtyAfterMutate && firedOnDirty && cleanAgain);
    }

    // 6) beginMacro/endMacro undoes as ONE step -----------------------------------
    {
        int v = 0; JUndoStack s;
        s.push(setCmd(&v, 1, "pre"));
        s.beginMacro("compound");
        s.push(setCmd(&v, 10, "m1"));
        s.push(setCmd(&v, 20, "m2"));
        s.push(setCmd(&v, 30, "m3"));
        s.endMacro();
        bool applied = (v == 30) && (s.count() == 2);   // "pre" + one macro

        s.undo();                                        // whole macro reverts at once
        bool oneStep = (v == 1) && s.canUndo();

        s.redo();                                        // whole macro replays at once
        bool replayed = (v == 30);

        check("beginMacro/endMacro undoes and redoes as one step",
              applied && oneStep && replayed);
    }

    if (g_fail == 0) std::puts("\nALL UNDO/REDO TESTS PASSED");
    else             std::printf("\n%d CASE(S) FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
