#include <genesis/core/StyleEngine.h>
#include <genesis/core/SceneGraph.h>
#include <cassert>
#include <iostream>

using namespace Genesis;

namespace Genesis::Style {
    constexpr StyleKey<Color> ThemeColor{ 0x10 };
}

void test_style_inheritance() {
    SceneGraph graph;
    StyleEngine styles(graph);
    
    NodeId root = graph.createNode("Root");
    NodeId parent = graph.createNode("Parent");
    NodeId child = graph.createNode("Child");
    
    graph.addChild(root, parent);
    graph.addChild(parent, child);
    
    Color rootColor = {255, 0, 0, 255}; // Red
    Color parentColor = {0, 255, 0, 255}; // Green
    Color defaultColor = {0, 0, 0, 255}; // Black
    
    // 1. Root defines style
    styles.setLocal(root, Style::ThemeColor, rootColor);
    
    // Child should inherit from Root
    assert(styles.lookup(child, Style::ThemeColor, defaultColor) == rootColor);
    
    // 2. Parent overrides style
    styles.setLocal(parent, Style::ThemeColor, parentColor);
    
    // Child should now inherit from Parent (the closest ancestor)
    assert(styles.lookup(child, Style::ThemeColor, defaultColor) == parentColor);
    
    // 3. Child overrides locally
    Color childColor = {0, 0, 255, 255}; // Blue
    styles.setLocal(child, Style::ThemeColor, childColor);
    assert(styles.lookup(child, Style::ThemeColor, defaultColor) == childColor);
    
    std::cout << "test_style_inheritance passed" << std::endl;
}

int main() {
    test_style_inheritance();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
