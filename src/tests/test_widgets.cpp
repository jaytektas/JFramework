#include <genesis/core/BaseWidgets.h>
#include <genesis/core/SceneGraph.h>
#include <genesis/graphics/RenderPrimitive.h>
#include <cassert>
#include <iostream>

using namespace Genesis;

void test_button_interaction() {
    SceneGraph graph;
    Button btn(graph, "Click Me");
    
    // Mock layout
    auto& layout = graph.getLayout(btn.getNodeId());
    layout.boundingBox = {10, 10, 100, 40};
    
    bool clicked = false;
    btn.onClicked.connect([&clicked]() {
        clicked = true;
    });
    
    // Press inside
    btn.handleMousePress(50, 30);
    assert(clicked == true);
    assert(btn.getState() == WidgetState::Pressed);
    
    std::cout << "test_button_interaction passed" << std::endl;
}

void test_widget_rendering() {
    SceneGraph graph;
    Button btn(graph, "Render");
    auto& layout = graph.getLayout(btn.getNodeId());
    layout.boundingBox = {0, 0, 100, 100};
    
    PrimitiveBuffer buffer;
    btn.populateRenderPrimitives(buffer);

    const auto& cmds = buffer.getCommands();
    // Button renders at least a body rect
    assert(!cmds.empty());
    assert(cmds[0].kind == PrimitiveBuffer::DrawCommand::Kind::Rect);
    assert(cmds[0].rect.rectBounds[2] == 100);
    
    std::cout << "test_widget_rendering passed" << std::endl;
}

void test_slider_logic() {
    SceneGraph graph;
    Slider slider(graph);
    
    float lastVal = 0.0f;
    slider.onValueChanged.connect([&lastVal](float v) {
        lastVal = v;
    });
    
    slider.setValue(0.75f);
    assert(lastVal == 0.75f);
    
    std::cout << "test_slider_logic passed" << std::endl;
}

int main() {
    test_button_interaction();
    test_widget_rendering();
    test_slider_logic();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
