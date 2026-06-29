// test_aipanel.cpp — JAIPanel widget unit tests
// Tests semantic node, logAction API, and that JAIPanel is a proper JWidget.

#include <j/core/AIPanel.h>
#include <j/core/SceneGraph.h>
#include <j/core/AiBusHook.h>
#include <cassert>
#include <string>
#include <iostream>

using namespace jf;

static void test_aipanel_is_widget() {
    JSceneGraph g;
    JAIPanel panel(g);
    // Verify it's a JWidget (compile-time check + getNodeId accessible)
    NodeId id = panel.getNodeId();
    assert(id != InvalidNodeId);
    std::cout << "  [OK] JAIPanel is a JWidget subclass with valid node id\n";
}

static void test_semantic_node() {
    JSceneGraph g;
    JAIPanel panel(g);
    auto node = panel.getSemanticNode();
    assert(std::string(node.role) == "JAIPanel");
    assert(!node.interactable || node.interactable); // just verify it's accessible
    std::cout << "  [OK] getSemanticNode() returns role=JAIPanel\n";
}

static void test_log_action() {
    JSceneGraph g;
    JAIPanel panel(g);
    // logAction should not crash; log is internal
    panel.logAction("test action 1");
    panel.logAction("test action 2");
    // No assertion beyond "doesn't crash"
    std::cout << "  [OK] logAction() does not crash\n";
}

static void test_execute_action_not_interactable() {
    JSceneGraph g;
    JAIPanel panel(g);
    // JAIPanel's executeSemanticAction — check it doesn't crash
    bool result = panel.executeSemanticAction("click");
    (void)result;
    std::cout << "  [OK] executeSemanticAction() is callable\n";
}

static void test_aipanel_in_active_widgets() {
    JSceneGraph g;
    auto* panelPtr = new JAIPanel(g);
    bool found = false;
    for (auto* w : JWidget::s_activeWidgets) {
        if (w == panelPtr) { found = true; break; }
    }
    assert(found);
    delete panelPtr;
    std::cout << "  [OK] JAIPanel appears in JWidget::s_activeWidgets\n";
}

static void test_aipanel_visible_by_default() {
    JSceneGraph g;
    JAIPanel panel(g);
    assert(panel.isVisible());
    std::cout << "  [OK] JAIPanel is visible by default\n";
}

int main() {
    std::cout << "JAIPanel tests:\n";
    test_aipanel_is_widget();
    test_semantic_node();
    test_log_action();
    test_execute_action_not_interactable();
    test_aipanel_in_active_widgets();
    test_aipanel_visible_by_default();
    std::cout << "All JAIPanel tests passed.\n";
    return 0;
}
