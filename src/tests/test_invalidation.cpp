#include <j/core/SceneGraph.h>
#include <cassert>
#include <iostream>

using namespace jf;

void test_invalidation_skipping() {
    JSceneGraph graph;
    
    NodeId root = graph.createNode("Root");
    NodeId child1 = graph.createNode("Child1");
    NodeId child2 = graph.createNode("Child2");
    
    graph.addChild(root, child1);
    graph.addChild(root, child2);
    
    graph.getLayout(child1).boundingBox.width = 100;
    graph.getLayout(child1).boundingBox.height = 50;
    graph.getLayout(child2).boundingBox.width = 100;
    graph.getLayout(child2).boundingBox.height = 50;
    
    // Initial layout (everything is dirty)
    JConstraints constraints{0, 800, 0, 600};
    graph.computeLayout(root, constraints);
    
    // Everything should be clean now
    assert(!graph.isDirty(root));
    assert(!graph.isDirty(child1));
    assert(!graph.isDirty(child2));
    
    // Modify child2
    graph.getLayout(child2).boundingBox.width = 150;
    
    // child2 and root (via DirtyChildren) should be dirty. child1 should remain clean.
    assert(graph.isDirty(child2));
    assert(graph.isDirty(root));
    assert(!graph.isDirty(child1));
    
    // Recompute layout
    graph.computeLayout(root, constraints);
    
    assert(graph.getLayoutConst(root).boundingBox.width == 150);
    assert(graph.getLayoutConst(root).boundingBox.height == 100);
    
    // All clean again
    assert(!graph.isDirty(root));
    assert(!graph.isDirty(child1));
    assert(!graph.isDirty(child2));
    
    std::cout << "test_invalidation_skipping passed" << std::endl;
}

int main() {
    test_invalidation_skipping();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
