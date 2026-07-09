// Headless test for the general (format-tagged) drag & drop model in
// DragDrop.h + the JWidget hooks + the jDragTick driver.
// Compile:
//   g++ -std=c++20 -I<repo>/include -I<repo>/third_party \
//       tests/dragdrop_test.cpp -o /tmp/dnd_test
// Prints PASS/FAIL per case; exits non-zero on any failure.

#include "j/core/BaseWidgets.h"
#include <cstdio>
#include <string>

using namespace jf;

static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}

// --- A drag source: plain widget, initiates a drag on demand. ----------------
struct SourceW : JWidget {
    SourceW(JSceneGraph& g) : JWidget(g, "SourceW") {}
    void populateRenderPrimitives(JPrimitiveBuffer&) override {}
};

// --- A drop target that accepts text/plain and records what it saw. ----------
struct TargetW : JWidget {
    TargetW(JSceneGraph& g) : JWidget(g, "TargetW") {}
    void populateRenderPrimitives(JPrimitiveBuffer&) override {}

    int         enters{0}, moves{0}, leaves{0}, drops{0};
    std::string gotText;
    JDropAction gotAction{JDropAction::Ignore};

    bool canDrop(const JMimeData& m) const override { return m.hasFormat("text/plain"); }
    void onDragEnter(JDragSession&) override { ++enters; }
    void onDragMove (JDragSession&) override { ++moves; }
    void onDragLeave(JDragSession&) override { ++leaves; }
    bool onDrop(JDragSession& s) override {
        ++drops;
        gotText   = s.mime.text();
        gotAction = s.proposed;
        return true;
    }
};

// --- A widget that refuses everything: must never be entered or dropped on. ---
struct RejectW : JWidget {
    RejectW(JSceneGraph& g) : JWidget(g, "RejectW") {}
    void populateRenderPrimitives(JPrimitiveBuffer&) override {}
    int enters{0}, drops{0};
    bool canDrop(const JMimeData&) const override { return false; }
    void onDragEnter(JDragSession&) override { ++enters; }
    bool onDrop(JDragSession&) override { ++drops; return true; }
};

int main() {
    // ---- JMimeData basics ---------------------------------------------------
    {
        JMimeData m;
        m.setText("hello");
        m.setData("application/x-jf-node", "42");
        check("mime hasText", m.hasText() && m.text() == "hello");
        check("mime hasFormat custom", m.hasFormat("application/x-jf-node"));
        check("mime data lookup", m.data("application/x-jf-node") == "42");
        check("mime formats order", m.formats().size() == 2 &&
                                    m.formats()[0] == "text/plain");
        m.setText("world");   // replace, not append
        check("mime setData replaces", m.formats().size() == 2 && m.text() == "world");
    }

    JSceneGraph graph;
    SourceW src(graph);
    TargetW tgt(graph);
    RejectW rej(graph);

    src.setBounds({  0.f, 0.f, 50.f, 50.f});
    tgt.setBounds({100.f, 0.f, 50.f, 50.f});   // cursor (120,20) lands here
    rej.setBounds({200.f, 0.f, 50.f, 50.f});   // cursor (220,20) lands here

    // ---- Full drag → move over target → release =============================
    {
        JMimeData m; m.setText("payload!");
        src.startDrag(std::move(m), JDropAction::Move);
        check("drag active after startDrag", jDragActive());
        check("drag source is src", jCurrentDrag().source == &src);

        // Move over the accepting target.
        bool droppedOnMove = jDragTick(120.f, 20.f, false, false);
        check("no drop on plain move", !droppedOnMove);
        check("target onDragEnter fired", tgt.enters == 1);
        check("proposed action is Move on enter", jCurrentDrag().proposed == JDropAction::Move);

        // A second move within the same target → onDragMove, no re-enter.
        jDragTick(125.f, 22.f, false, false);
        check("target onDragMove fired", tgt.moves >= 1 && tgt.enters == 1);

        // Release over the target → onDrop.
        bool dropped = jDragTick(125.f, 22.f, false, true);
        check("jDragTick reports drop", dropped);
        check("target onDrop fired once", tgt.drops == 1);
        check("payload text arrived", tgt.gotText == "payload!");
        check("Move action reported to target", tgt.gotAction == JDropAction::Move);
        check("session cleared after drop", !jDragActive());
    }

    // ---- Enter then leave (move off the target) =============================
    {
        tgt.enters = tgt.moves = tgt.leaves = tgt.drops = 0;
        JMimeData m; m.setText("x");
        src.startDrag(std::move(m), JDropAction::Copy);
        jDragTick(120.f, 20.f, false, false);   // enter target
        check("re-enter target", tgt.enters == 1);
        jDragTick(400.f, 400.f, false, false);  // move off into empty space
        check("target onDragLeave fired", tgt.leaves == 1);
        check("no target while over empty space", jCurrentDrag().over == nullptr);
        jCancelDrag();
    }

    // ---- Non-accepting widget must NOT receive enter or drop ================
    {
        JMimeData m; m.setText("nope");
        src.startDrag(std::move(m), JDropAction::Move);
        jDragTick(220.f, 20.f, false, false);         // over the RejectW
        bool dropped = jDragTick(220.f, 20.f, false, true);  // release over it
        check("reject never entered", rej.enters == 0);
        check("reject never dropped on", rej.drops == 0);
        check("no drop delivered to non-target", !dropped);
        check("session cleared after miss-release", !jDragActive());
    }

    if (g_fail) { std::printf("\n%d FAILURE(S)\n", g_fail); return 1; }
    std::printf("\nALL PASS\n");
    return 0;
}
