#include <j/core/BaseWidgets.h>
#include <j/core/SceneGraph.h>
#include <cassert>
#include <iostream>

using namespace jf;

void test_button_interaction() {
    JSceneGraph graph;
    JButton btn(graph, "Submit");
    
    // Initial state
    assert(btn.getState() == JWidgetState::Normal);
    
    // Test Hover
    btn.handleMouseMove(50, 20); // Inside default 100x40 at 0,0
    assert(btn.getState() == JWidgetState::Hovered);
    
    btn.handleMouseMove(250, 20); // Outside (button is 160px wide by default)
    assert(btn.getState() == JWidgetState::Normal);
    
    // Test Click
    bool clicked = false;
    btn.onClicked.connect([&clicked]() { clicked = true; });
    
    btn.handleMousePress(50, 20);
    assert(btn.getState() == JWidgetState::Pressed);
    assert(clicked == true);
    
    btn.handleMouseRelease(50, 20);
    assert(btn.getState() == JWidgetState::Hovered);
    
    std::cout << "test_button_interaction passed" << std::endl;
}

void test_slider_interaction() {
    JSceneGraph graph;
    JSlider slider(graph);
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
    JSceneGraph graph;
    JButton btn(graph, "AI JButton");
    
    auto semantic = btn.getSemanticNode();
    assert(semantic.role == "JButton");
    assert(semantic.label == "AI JButton");
    
    bool clicked = false;
    btn.onClicked.connect([&clicked]() { clicked = true; });
    
    bool result = btn.executeSemanticAction("click");
    assert(result == true);
    assert(clicked == true);
    
    JSlider slider(graph);
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
