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

void test_combobox_logic() {
    SceneGraph graph;
    ComboBox cb(graph, {"OptA", "OptB", "OptC"});
    
    assert(cb.mode() == ComboBoxMode::Cycling);
    cb.setCurrentIndex(0);
    assert(cb.currentText() == "OptA");
    
    // Setup bounding box for inside checks
    auto& layout = graph.getLayout(cb.getNodeId());
    layout.boundingBox = {0, 0, 100, 30};
    
    // Cycling mode test
    cb.handleMousePress(10.f, 10.f);
    assert(cb.currentIndex() == 1);
    assert(cb.currentText() == "OptB");
    
    // Popup mode test
    cb.setMode(ComboBoxMode::Popup);
    assert(cb.mode() == ComboBoxMode::Popup);
    
    bool popupRequested = false;
    cb.onPopupRequested.connect([&popupRequested, &cb](ComboBox* source) {
        popupRequested = (source == &cb);
    });
    
    cb.handleMousePress(10.f, 10.f);
    assert(popupRequested == true);
    // Index should not have changed in popup mode until callback sets it
    assert(cb.currentIndex() == 1);
    
    std::cout << "test_combobox_logic passed" << std::endl;
}

int main() {
    test_button_interaction();
    test_widget_rendering();
    test_slider_logic();
    test_combobox_logic();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
