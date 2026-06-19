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

int main() {
    test_simple_layout();
    test_row_layout();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
