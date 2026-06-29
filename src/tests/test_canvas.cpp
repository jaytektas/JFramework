#include <j/core/CanvasWidget.h>
#include <j/core/SceneGraph.h>
#include <cassert>
#include <iostream>
#include <cstring>

using namespace jf;

// Concrete subclass for testing
class CircleWidget : public JCanvasWidget {
public:
    uint8_t fillColor[4] = {255, 0, 0, 255};
    float   radius{40.f};
    int     drawCalls{0};

    CircleWidget(JSceneGraph& g) : JCanvasWidget(g, "CircleWidget") {}

    void draw(JCanvas& c, float w, float h) override {
        ++drawCalls;
        c.fillCircle(w * 0.5f, h * 0.5f, radius, fillColor);
    }
};

class ArcWidget : public JCanvasWidget {
public:
    int drawCalls{0};
    ArcWidget(JSceneGraph& g) : JCanvasWidget(g, "ArcWidget") {}
    void draw(JCanvas& c, float w, float h) override {
        ++drawCalls;
        uint8_t col[4] = {0, 120, 255, 255};
        c.arc(w * 0.5f, h * 0.5f, 40.f, -220.f, 40.f, col, 6.f);
    }
};

class MultiPrimWidget : public JCanvasWidget {
public:
    int drawCalls{0};
    MultiPrimWidget(JSceneGraph& g) : JCanvasWidget(g, "MultiPrimWidget") {}
    void draw(JCanvas& c, float w, float h) override {
        ++drawCalls;
        uint8_t bg[4] = {30, 30, 30, 255};
        uint8_t fg[4] = {255, 255, 255, 255};
        uint8_t bd[4] = {100, 100, 100, 255};
        c.fillRect(0, 0, w, h, bg, 8.f);
        c.strokeRect(4.f, 4.f, w-8.f, h-8.f, bd, 1.f, 4.f);
        c.line(10.f, h/2.f, w-10.f, h/2.f, fg, 2.f);
        c.textCentered(w/2.f, h/2.f - 16.f, "Hello", fg);
    }
};

void test_draw_called() {
    JSceneGraph graph;
    CircleWidget w(graph);
    auto& l = graph.getLayout(w.getNodeId());
    l.boundingBox = {0.f, 0.f, 100.f, 100.f};

    JPrimitiveBuffer buf;
    w.populateRenderPrimitives(buf);
    assert(w.drawCalls == 1);
    // fillCircle emits at least one JRect command (circle = rect with full radius)
    assert(!buf.getCommands().empty());
    std::cout << "test_draw_called passed\n";
}

void test_arc_emits_primitives() {
    JSceneGraph graph;
    ArcWidget w(graph);
    auto& l = graph.getLayout(w.getNodeId());
    l.boundingBox = {0.f, 0.f, 120.f, 120.f};

    JPrimitiveBuffer buf;
    w.populateRenderPrimitives(buf);
    assert(w.drawCalls == 1);
    // Arc approximation emits many circle dots
    assert(buf.getCommands().size() > 10);
    std::cout << "test_arc_emits_primitives passed\n";
}

void test_multi_primitive_widget() {
    JSceneGraph graph;
    MultiPrimWidget w(graph);
    auto& l = graph.getLayout(w.getNodeId());
    l.boundingBox = {0.f, 0.f, 200.f, 60.f};

    JPrimitiveBuffer buf;
    w.populateRenderPrimitives(buf);
    assert(w.drawCalls == 1);
    assert(buf.getCommands().size() >= 2);  // bg + border at minimum
    std::cout << "test_multi_primitive_widget passed\n";
}

void test_canvas_offset() {
    JSceneGraph graph;
    CircleWidget w(graph);
    auto& l = graph.getLayout(w.getNodeId());
    l.boundingBox = {50.f, 80.f, 100.f, 100.f};  // widget placed at (50,80)

    JPrimitiveBuffer buf;
    w.populateRenderPrimitives(buf);
    // All primitives should have x >= 50, y >= 80
    for (const auto& cmd : buf.getCommands()) {
        if (cmd.kind == JPrimitiveBuffer::JDrawCommand::JKind::JRect) {
            assert(cmd.rect.rectBounds[0] >= 50.f - 1.f);  // small float tolerance
            assert(cmd.rect.rectBounds[1] >= 80.f - 1.f);
        }
    }
    std::cout << "test_canvas_offset passed\n";
}

void test_invalidate() {
    JSceneGraph graph;
    CircleWidget w(graph);
    auto& l = graph.getLayout(w.getNodeId());
    l.boundingBox = {0.f, 0.f, 100.f, 100.f};

    // After invalidate, node should be dirty (no crash, no UB)
    w.invalidate();

    JPrimitiveBuffer buf;
    w.populateRenderPrimitives(buf);
    assert(w.drawCalls == 1);
    std::cout << "test_invalidate passed\n";
}

void test_semantic_node() {
    JSceneGraph graph;
    CircleWidget w(graph);
    auto node = w.getSemanticNode();
    assert(node.role == "JCanvasWidget");
    assert(node.label == "CircleWidget");
    std::cout << "test_semantic_node passed\n";
}

int main() {
    test_draw_called();
    test_arc_emits_primitives();
    test_multi_primitive_widget();
    test_canvas_offset();
    test_invalidate();
    test_semantic_node();
    std::cout << "All JCanvasWidget tests passed!\n";
    return 0;
}
