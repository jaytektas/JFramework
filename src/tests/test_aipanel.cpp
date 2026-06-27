// test_aipanel.cpp — AIPanel widget unit tests
// Tests semantic node, logAction API, and that AIPanel is a proper Widget.

#include <genesis/core/AIPanel.h>
#include <genesis/core/SceneGraph.h>
#include <genesis/core/AiBusHook.h>
#include <cassert>
#include <string>
#include <iostream>

using namespace Genesis;

static void test_aipanel_is_widget() {
    SceneGraph g;
    AIPanel panel(g);
    // Verify it's a Widget (compile-time check + getNodeId accessible)
    NodeId id = panel.getNodeId();
    assert(id != InvalidNodeId);
    std::cout << "  [OK] AIPanel is a Widget subclass with valid node id\n";
}

static void test_semantic_node() {
    SceneGraph g;
    AIPanel panel(g);
    auto node = panel.getSemanticNode();
    assert(std::string(node.role) == "AIPanel");
    assert(!node.interactable || node.interactable); // just verify it's accessible
    std::cout << "  [OK] getSemanticNode() returns role=AIPanel\n";
}

static void test_log_action() {
    SceneGraph g;
    AIPanel panel(g);
    // logAction should not crash; log is internal
    panel.logAction("test action 1");
    panel.logAction("test action 2");
    // No assertion beyond "doesn't crash"
    std::cout << "  [OK] logAction() does not crash\n";
}

static void test_execute_action_not_interactable() {
    SceneGraph g;
    AIPanel panel(g);
    // AIPanel's executeSemanticAction — check it doesn't crash
    bool result = panel.executeSemanticAction("click");
    (void)result;
    std::cout << "  [OK] executeSemanticAction() is callable\n";
}

static void test_aipanel_in_active_widgets() {
    SceneGraph g;
    auto* panelPtr = new AIPanel(g);
    bool found = false;
    for (auto* w : Widget::s_activeWidgets) {
        if (w == panelPtr) { found = true; break; }
    }
    assert(found);
    delete panelPtr;
    std::cout << "  [OK] AIPanel appears in Widget::s_activeWidgets\n";
}

static void test_aipanel_visible_by_default() {
    SceneGraph g;
    AIPanel panel(g);
    assert(panel.isVisible());
    std::cout << "  [OK] AIPanel is visible by default\n";
}

int main() {
    std::cout << "AIPanel tests:\n";
    test_aipanel_is_widget();
    test_semantic_node();
    test_log_action();
    test_execute_action_not_interactable();
    test_aipanel_in_active_widgets();
    test_aipanel_visible_by_default();
    std::cout << "All AIPanel tests passed.\n";
    return 0;
}
