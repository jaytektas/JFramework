#include <genesis/core/BaseWidgets.h>
#include <genesis/core/SceneGraph.h>
#include <cassert>
#include <iostream>

using namespace Genesis;

void test_button_interaction() {
    SceneGraph graph;
    Button btn(graph, "Submit");
    
    // Initial state
    assert(btn.getState() == WidgetState::Normal);
    
    // Test Hover
    btn.handleMouseMove(50, 20); // Inside default 100x40 at 0,0
    assert(btn.getState() == WidgetState::Hovered);
    
    btn.handleMouseMove(250, 20); // Outside (button is 160px wide by default)
    assert(btn.getState() == WidgetState::Normal);
    
    // Test Click
    bool clicked = false;
    btn.onClicked.connect([&clicked]() { clicked = true; });
    
    btn.handleMousePress(50, 20);
    assert(btn.getState() == WidgetState::Pressed);
    assert(clicked == true);
    
    btn.handleMouseRelease(50, 20);
    assert(btn.getState() == WidgetState::Hovered);
    
    std::cout << "test_button_interaction passed" << std::endl;
}

void test_slider_interaction() {
    SceneGraph graph;
    Slider slider(graph);
    // Fix bounds explicitly so coordinate math doesn't depend on default widget size
    graph.getLayout(slider.getNodeId()).boundingBox = {0.0f, 0.0f, 200.0f, 20.0f};

    float value = slider.getValue(); // read initial value (0.5)
    slider.onValueChanged.connect([&value](float val) { value = val; });

    slider.handleMousePress(160, 10); // press at 160/200 = 0.8
    assert(std::abs(value - 0.8f) < 0.001f);

    slider.handleMouseMove(200, 10); // drag to end
    assert(std::abs(value - 1.0f) < 0.001f);

    std::cout << "test_slider_interaction passed" << std::endl;
}

void test_ai_semantics() {
    SceneGraph graph;
    Button btn(graph, "AI Button");
    
    auto semantic = btn.getSemanticNode();
    assert(semantic.role == "Button");
    assert(semantic.label == "AI Button");
    
    bool clicked = false;
    btn.onClicked.connect([&clicked]() { clicked = true; });
    
    bool result = btn.executeSemanticAction("click");
    assert(result == true);
    assert(clicked == true);
    
    Slider slider(graph);
    result = slider.executeSemanticAction("set_value:0.75");
    assert(result == true);
    assert(slider.getSemanticNode().value == std::to_string(0.750000f));
    
    std::cout << "test_ai_semantics passed" << std::endl;
}

int main() {
    test_button_interaction();
    test_slider_interaction();
    test_ai_semantics();
    std::cout << "All Controls Foundation tests passed!" << std::endl;
    return 0;
}
