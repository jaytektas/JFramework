#include <genesis/core/BaseWidgets.h>
#include <genesis/core/MenuSystem.h>
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

void test_textarea_logic() {
    SceneGraph graph;
    TextArea ta(graph, "Placeholder");
    
    std::string textOut = "";
    ta.onTextChanged.connect([&textOut](const std::string& s) {
        textOut = s;
    });

    assert(ta.text().empty());
    assert(ta.placeholder() == "Placeholder");

    ta.setText("Line 1\nLine 2");
    assert(ta.text() == "Line 1\nLine 2");
    assert(textOut == "Line 1\nLine 2");

    auto lines = ta.getLines();
    assert(lines.size() == 2);
    assert(lines[0] == "Line 1");
    assert(lines[1] == "Line 2");

    KeyEvent ke;
    ke.pressed = true;
    ke.utf8[0] = '!';
    ke.utf8[1] = '\0';
    bool handled = ta.handleKeyEvent(ke);
    assert(handled == true);
    assert(ta.text() == "Line 1\nLine 2!");

    ke.key = KeyEvent::Key::Backspace;
    ke.utf8[0] = '\0';
    handled = ta.handleKeyEvent(ke);
    assert(handled == true);
    assert(ta.text() == "Line 1\nLine 2");

    // Test Space key entry
    ke.key = KeyEvent::Key::Space;
    ke.utf8[0] = ' ';
    ke.utf8[1] = '\0';
    handled = ta.handleKeyEvent(ke);
    assert(handled == true);
    assert(ta.text() == "Line 1\nLine 2 ");

    // Test Question mark Shift key resolution
    ke.key = KeyEvent::Key::Unknown;
    ke.utf8[0] = '?';
    ke.utf8[1] = '\0';
    handled = ta.handleKeyEvent(ke);
    assert(handled == true);
    assert(ta.text() == "Line 1\nLine 2 ?");

    // Test Unicode character entry (e.g., 'ü' -> C3 BC in UTF-8)
    ke.utf8[0] = '\xc3';
    ke.utf8[1] = '\xbc';
    ke.utf8[2] = '\0';
    handled = ta.handleKeyEvent(ke);
    assert(handled == true);
    assert(ta.text() == "Line 1\nLine 2 ?\xc3\xbc");

    std::cout << "test_textarea_logic passed" << std::endl;
}

void test_scrollarea_logic() {
    SceneGraph graph;
    ScrollArea sa(graph, 200.0f, 100.0f);
    
    Button* b1 = new Button(graph, "Child Button 1", 180.0f, 40.0f);
    Button* b2 = new Button(graph, "Child Button 2", 180.0f, 80.0f);
    sa.addChildWidget(b1);
    sa.addChildWidget(b2);

    assert(sa.children().size() == 2);
    assert(sa.children()[0] == b1);
    assert(sa.children()[1] == b2);

    PrimitiveBuffer buf;
    sa.populateRenderPrimitives(buf);

    auto l1 = graph.getLayoutConst(b1->getNodeId()).boundingBox;
    auto l2 = graph.getLayoutConst(b2->getNodeId()).boundingBox;
    
    assert(l1.width == 200.0f - 16.0f);
    assert(l2.y == l1.y + l1.height + 6.0f);

    sa.handleScroll(10.0f, 10.0f, -1.0f); // Scroll down
    sa.populateRenderPrimitives(buf);

    auto l1_scrolled = graph.getLayoutConst(b1->getNodeId()).boundingBox;
    assert(l1_scrolled.y < l1.y);

    delete b1;
    delete b2;
    std::cout << "test_scrollarea_logic passed" << std::endl;
}

void test_listview_logic() {
    SceneGraph graph;
    ListView lv(graph, {"Item 1", "Item 2", "Item 3"});

    int lastSelection = -1;
    lv.onSelectionChanged.connect([&lastSelection](int idx) {
        lastSelection = idx;
    });

    assert(lv.items().size() == 3);
    assert(lv.selectedIndex() == -1);

    lv.setSelectedIndex(1);
    assert(lv.selectedIndex() == 1);
    assert(lastSelection == 1);

    KeyEvent ke;
    ke.pressed = true;
    ke.key = KeyEvent::Key::Down;
    bool handled = lv.handleKeyEvent(ke);
    assert(handled == true);
    assert(lv.selectedIndex() == 2);
    assert(lastSelection == 2);

    ke.key = KeyEvent::Key::Up;
    handled = lv.handleKeyEvent(ke);
    assert(handled == true);
    assert(lv.selectedIndex() == 1);
    assert(lastSelection == 1);

    auto& l = graph.getLayout(lv.getNodeId());
    l.boundingBox = {0, 0, 100, 100};

    int activatedItem = -1;
    lv.onItemActivated.connect([&activatedItem](int idx) {
        activatedItem = idx;
    });

    lv.handleMousePress(10.f, 10.f);
    assert(lv.selectedIndex() == 0);
    assert(activatedItem == 0);

    std::cout << "test_listview_logic passed" << std::endl;
}

void test_treeview_logic() {
    SceneGraph graph;
    TreeView tv(graph);

    TreeViewNode rootNode = {
        "Root", true, false, {
            {"Child 1", false, false, {}},
            {"Child 2", true, false, {
                {"Grandchild 2.1", false, false, {}}
            }}
        }
    };
    tv.setRootNode(rootNode);

    auto flatNodes = tv.getFlatNodes();
    assert(flatNodes.size() == 3);
    assert(flatNodes[0].node->label == "Child 1");
    assert(flatNodes[1].node->label == "Child 2");
    assert(flatNodes[2].node->label == "Grandchild 2.1");

    KeyEvent ke;
    ke.pressed = true;
    ke.key = KeyEvent::Key::Down;
    
    bool handled = tv.handleKeyEvent(ke);
    assert(handled == true);
    assert(tv.selectedNode() != nullptr);
    assert(tv.selectedNode()->label == "Child 1");

    handled = tv.handleKeyEvent(ke);
    assert(handled == true);
    assert(tv.selectedNode()->label == "Child 2");

    ke.key = KeyEvent::Key::Left;
    handled = tv.handleKeyEvent(ke);
    assert(handled == true);
    assert(flatNodes[1].node->expanded == false);

    flatNodes = tv.getFlatNodes();
    assert(flatNodes.size() == 2);

    std::cout << "test_treeview_logic passed" << std::endl;
}

void test_datagrid_logic() {
    SceneGraph graph;
    DataGrid dg(graph, {"ID", "Name", "Role"});
    
    std::vector<std::vector<std::string>> rows = {
        {"1", "Alice", "Admin"},
        {"2", "Bob", "Developer"},
        {"3", "Charlie", "Designer"}
    };
    dg.setRows(rows);

    assert(dg.headers().size() == 3);
    assert(dg.rows().size() == 3);
    assert(dg.selectedIndex() == -1);

    int lastSelected = -1;
    dg.onSelectionChanged.connect([&lastSelected](int r) {
        lastSelected = r;
    });

    dg.setSelectedIndex(1);
    assert(dg.selectedIndex() == 1);
    assert(lastSelected == 1);

    KeyEvent ke;
    ke.pressed = true;
    ke.key = KeyEvent::Key::Down;
    bool handled = dg.handleKeyEvent(ke);
    assert(handled == true);
    assert(dg.selectedIndex() == 2);
    assert(lastSelected == 2);

    ke.key = KeyEvent::Key::Up;
    handled = dg.handleKeyEvent(ke);
    assert(handled == true);
    assert(dg.selectedIndex() == 1);
    assert(lastSelected == 1);

    int activatedRow = -1;
    dg.onRowActivated.connect([&activatedRow](int r) {
        activatedRow = r;
    });

    ke.key = KeyEvent::Key::Return;
    handled = dg.handleKeyEvent(ke);
    assert(handled == true);
    assert(activatedRow == 1);

    std::cout << "test_datagrid_logic passed" << std::endl;
}

void test_menu_and_shortcuts() {
    SceneGraph graph;
    Menu menu("File");
    
    auto* item1 = menu.add(graph, "New", {KeyEvent::Key::N, true});
    auto* item2 = menu.add(graph, "Open", {KeyEvent::Key::O, true});
    item2->setCheckable(true);
    item2->setChecked(true);

    assert(item1->label() == "New");
    assert(item1->shortcut().key == KeyEvent::Key::N);
    assert(item1->shortcut().ctrl == true);

    assert(item2->isCheckable() == true);
    assert(item2->isChecked() == true);

    bool triggered = false;
    MenuManager::instance().clearShortcuts();
    MenuManager::instance().registerShortcut(item1->shortcut(), [&triggered]() {
        triggered = true;
    });

    KeyEvent ke;
    ke.pressed = true;
    ke.key = KeyEvent::Key::N;
    ke.ctrl = true;
    
    bool handled = MenuManager::instance().processAccelerator(ke);
    assert(handled == true);
    assert(triggered == true);

    std::cout << "test_menu_and_shortcuts passed" << std::endl;
}

void test_tooltips() {
    SceneGraph graph;
    Button btn(graph, "TooltipButton");
    btn.setTooltip("Click this button");
    
    auto& layout = graph.getLayout(btn.getNodeId());
    layout.boundingBox = {10, 10, 100, 40};

    assert(btn.tooltip() == "Click this button");
    assert(btn.hitTest(50, 30) == true);
    assert(btn.hitTest(200, 30) == false);

    bool found = false;
    for (Widget* w : Widget::s_activeWidgets) {
        if (w == &btn) {
            found = true;
            break;
        }
    }
    assert(found == true);

    std::cout << "test_tooltips passed" << std::endl;
}

int main() {
    test_button_interaction();
    test_widget_rendering();
    test_slider_logic();
    test_combobox_logic();
    test_textarea_logic();
    test_scrollarea_logic();
    test_listview_logic();
    test_treeview_logic();
    test_datagrid_logic();
    test_menu_and_shortcuts();
    test_tooltips();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
