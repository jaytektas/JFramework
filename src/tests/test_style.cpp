#include <j/core/StyleEngine.h>
#include <j/core/SceneGraph.h>
#include <cassert>
#include <iostream>

using namespace jf;

inline namespace jf { namespace JStyle {
    constexpr JStyleKey<JColor> ThemeColor{ 0x10 };
}}

void test_style_inheritance() {
    JSceneGraph graph;
    JStyleEngine styles(graph);
    
    NodeId root = graph.createNode("Root");
    NodeId parent = graph.createNode("Parent");
    NodeId child = graph.createNode("Child");
    
    graph.addChild(root, parent);
    graph.addChild(parent, child);
    
    JColor rootColor = {255, 0, 0, 255}; // Red
    JColor parentColor = {0, 255, 0, 255}; // Green
    JColor defaultColor = {0, 0, 0, 255}; // Black
    
    // 1. Root defines style
    styles.setLocal(root, JStyle::ThemeColor, rootColor);
    
    // Child should inherit from Root
    assert(styles.lookup(child, JStyle::ThemeColor, defaultColor) == rootColor);
    
    // 2. Parent overrides style
    styles.setLocal(parent, JStyle::ThemeColor, parentColor);
    
    // Child should now inherit from Parent (the closest ancestor)
    assert(styles.lookup(child, JStyle::ThemeColor, defaultColor) == parentColor);
    
    // 3. Child overrides locally
    JColor childColor = {0, 0, 255, 255}; // Blue
    styles.setLocal(child, JStyle::ThemeColor, childColor);
    assert(styles.lookup(child, JStyle::ThemeColor, defaultColor) == childColor);
    
    std::cout << "test_style_inheritance passed" << std::endl;
}

int main() {
    test_style_inheritance();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
