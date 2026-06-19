#include <genesis/core/SceneGraph.h>
#include <cassert>
#include <iostream>

using namespace Genesis;

void test_simple_layout() {
    SceneGraph graph;
    
    NodeId root = graph.createNode("Root");
    NodeId child1 = graph.createNode("Child1");
    NodeId child2 = graph.createNode("Child2");
    
    graph.addChild(root, child1);
    graph.addChild(root, child2);
    
    // Set child sizes (simulated leaf node sizes)
    graph.getLayout(child1).boundingBox.width = 100;
    graph.getLayout(child1).boundingBox.height = 50;
    
    graph.getLayout(child2).boundingBox.width = 100;
    graph.getLayout(child2).boundingBox.height = 50;
    
    // Root as a Column (Default)
    Constraints constraints{0, 800, 0, 600};
    graph.computeLayout(root, constraints);
    
    assert(graph.getLayout(root).boundingBox.width == 100);
    assert(graph.getLayout(root).boundingBox.height == 100);
    assert(graph.getLayout(child1).boundingBox.y == 0);
    assert(graph.getLayout(child2).boundingBox.y == 50);
    
    std::cout << "test_simple_layout passed" << std::endl;
}

void test_row_layout() {
    SceneGraph graph;
    
    NodeId root = graph.createNode("Root");
    graph.getLayout(root).direction = FlexDirection::Row;
    
    NodeId child1 = graph.createNode("Child1");
    NodeId child2 = graph.createNode("Child2");
    
    graph.addChild(root, child1);
    graph.addChild(root, child2);
    
    graph.getLayout(child1).boundingBox.width = 100;
    graph.getLayout(child1).boundingBox.height = 50;
    
    graph.getLayout(child2).boundingBox.width = 100;
    graph.getLayout(child2).boundingBox.height = 50;
    
    Constraints constraints{0, 800, 0, 600};
    graph.computeLayout(root, constraints);
    
    assert(graph.getLayout(root).boundingBox.width == 200);
    assert(graph.getLayout(root).boundingBox.height == 50);
    assert(graph.getLayout(child1).boundingBox.x == 0);
    assert(graph.getLayout(child2).boundingBox.x == 100);
    
    std::cout << "test_row_layout passed" << std::endl;
}

void test_flex_grow() {
    SceneGraph g;
    NodeId root = g.createNode("Root");
    g.getLayout(root).direction = FlexDirection::Column;
    NodeId a = g.createNode(), b = g.createNode();
    g.getLayout(a).boundingBox.width = 100; g.getLayout(a).boundingBox.height = 50;
    g.getLayout(b).boundingBox.width = 100; g.getLayout(b).boundingBox.height = 50;
    g.getLayout(b).flexGrow = 1.0f;                 // b absorbs leftover space
    g.addChild(root, a); g.addChild(root, b);

    g.computeLayout(root, Constraints{0, 800, 300, 300});  // tight 300 tall
    assert(g.getLayout(root).boundingBox.height == 300);
    assert(g.getLayout(b).boundingBox.height == 250);      // 50 + (300-100) leftover
    assert(g.getLayout(a).boundingBox.y == 0);
    assert(g.getLayout(b).boundingBox.y == 50);
    std::cout << "test_flex_grow passed" << std::endl;
}

void test_align_stretch() {
    SceneGraph g;
    NodeId root = g.createNode("Root");
    g.getLayout(root).direction  = FlexDirection::Column;
    g.getLayout(root).alignItems = AlignItems::Stretch;
    NodeId a = g.createNode();
    g.getLayout(a).boundingBox.width = 50; g.getLayout(a).boundingBox.height = 30;
    g.addChild(root, a);

    g.computeLayout(root, Constraints{200, 200, 0, 600});  // tight 200 wide
    assert(g.getLayout(a).boundingBox.width == 200);       // stretched across
    std::cout << "test_align_stretch passed" << std::endl;
}

void test_justify_center() {
    SceneGraph g;
    NodeId root = g.createNode("Root");
    g.getLayout(root).direction      = FlexDirection::Column;
    g.getLayout(root).justifyContent = JustifyContent::Center;
    NodeId a = g.createNode(), b = g.createNode();
    g.getLayout(a).boundingBox.height = 50; g.getLayout(b).boundingBox.height = 50;
    g.addChild(root, a); g.addChild(root, b);

    g.computeLayout(root, Constraints{0, 800, 300, 300});  // 300 tall, 100 content -> 200 free
    assert(g.getLayout(a).boundingBox.y == 100);           // centered: 200/2
    assert(g.getLayout(b).boundingBox.y == 150);
    std::cout << "test_justify_center passed" << std::endl;
}

void test_margin() {
    SceneGraph g;
    NodeId root = g.createNode("Root");
    NodeId a = g.createNode();
    g.getLayout(a).boundingBox.width = 30; g.getLayout(a).boundingBox.height = 50;
    g.getLayout(a).margin = 10.0f;                          // 10 on all sides
    g.addChild(root, a);

    g.computeLayout(root, Constraints{0, 800, 0, 600});
    assert(g.getLayout(a).boundingBox.x == 10);
    assert(g.getLayout(a).boundingBox.y == 10);
    std::cout << "test_margin passed" << std::endl;
}

void test_nested_positions() {
    SceneGraph g;
    NodeId root = g.createNode("Root");
    g.getLayout(root).padding = 20.0f;
    NodeId box = g.createNode("Box");                      // intermediate container
    g.getLayout(box).padding = 5.0f;
    NodeId leaf = g.createNode("Leaf");
    g.getLayout(leaf).boundingBox.width = 60; g.getLayout(leaf).boundingBox.height = 40;
    g.addChild(root, box); g.addChild(box, leaf);

    g.computeLayout(root, Constraints{0, 800, 0, 600});
    // grandchild must inherit both paddings: 20 (root) + 5 (box) = 25
    assert(g.getLayout(leaf).boundingBox.x == 25);
    assert(g.getLayout(leaf).boundingBox.y == 25);
    std::cout << "test_nested_positions passed" << std::endl;
}

int main() {
    test_simple_layout();
    test_row_layout();
    test_flex_grow();
    test_align_stretch();
    test_justify_center();
    test_margin();
    test_nested_positions();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
