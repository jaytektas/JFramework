#include <j/core/JDataGrid.h>
#include <j/core/JTreeView.h>
#include <j/core/JListView.h>
#include <j/core/JScrollArea.h>
#include <j/core/JTextArea.h>
#include <j/core/JComboBox.h>
#include <j/core/JSlider.h>
#include <j/core/JSpinBox.h>
#include <j/core/JDoubleSpinBox.h>
#include <j/core/JButton.h>
#include <j/core/MenuSystem.h>
#include <j/core/SceneGraph.h>
#include <j/graphics/RenderPrimitive.h>
#include <cassert>
#include <iostream>

using namespace jf;

void test_button_interaction() {
    JSceneGraph graph;
    JButton btn(graph, "Click Me");
    
    // Mock layout
    auto& layout = graph.getLayout(btn.getNodeId());
    layout.boundingBox = {10, 10, 100, 40};
    
    bool clicked = false;
    btn.onClicked.connect([&clicked]() {
        clicked = true;
    });
    
    // A click completes on RELEASE INSIDE (Qt semantics), not on press.
    btn.handleMousePress(50, 30);
    assert(clicked == false);                       // press only arms it
    assert(btn.getState() == JWidgetState::Pressed);
    btn.handleMouseRelease(50, 30);
    assert(clicked == true);

    // Press, drag OUTSIDE, release -> cancelled: no clicked().
    clicked = false;
    btn.handleMousePress(50, 30);
    btn.handleMouseMove(500, 500);
    btn.handleMouseRelease(500, 500);
    assert(clicked == false);

    // Press, drag out, drag back IN, release -> fires.
    clicked = false;
    btn.handleMousePress(50, 30);
    btn.handleMouseMove(500, 500);
    btn.handleMouseMove(50, 30);
    btn.handleMouseRelease(50, 30);
    assert(clicked == true);
    
    std::cout << "test_button_interaction passed" << std::endl;
}

void test_widget_rendering() {
    JSceneGraph graph;
    JButton btn(graph, "Render");
    auto& layout = graph.getLayout(btn.getNodeId());
    layout.boundingBox = {0, 0, 100, 100};
    
    JPrimitiveBuffer buffer;
    btn.populateRenderPrimitives(buffer);

    const auto& cmds = buffer.getCommands();
    // JButton renders at least a body rect
    assert(!cmds.empty());
    assert(cmds[0].kind == JPrimitiveBuffer::JDrawCommand::JKind::JRect);
    assert(cmds[0].rect.rectBounds[2] == 100);
    
    std::cout << "test_widget_rendering passed" << std::endl;
}

void test_slider_logic() {
    JSceneGraph graph;
    JSlider slider(graph);
    
    float lastVal = 0.0f;
    slider.onValueChanged.connect([&lastVal](float v) {
        lastVal = v;
    });
    
    slider.setValue(0.75f);
    assert(lastVal == 0.75f);
    
    std::cout << "test_slider_logic passed" << std::endl;
}

void test_combobox_logic() {
    JSceneGraph graph;
    JComboBox cb(graph, {"OptA", "OptB", "OptC"});
    
    // Popup is the DEFAULT mode (JComboBox.h: "dropdown list by default") — the test predated that
    // change and still asserted the old Cycling default.
    assert(cb.mode() == JComboBoxMode::Popup);
    cb.setCurrentIndex(0);
    assert(cb.currentText() == "OptA");
    
    // Setup bounding box for inside checks
    auto& layout = graph.getLayout(cb.getNodeId());
    layout.boundingBox = {0, 0, 100, 30};
    
    // Cycling mode test — opted into explicitly, since it is no longer the default
    cb.setMode(JComboBoxMode::Cycling);
    assert(cb.mode() == JComboBoxMode::Cycling);
    cb.handleMousePress(10.f, 10.f);
    assert(cb.currentIndex() == 1);
    assert(cb.currentText() == "OptB");
    
    // Popup mode test
    cb.setMode(JComboBoxMode::Popup);
    assert(cb.mode() == JComboBoxMode::Popup);
    
    bool popupRequested = false;
    cb.onPopupRequested.connect([&popupRequested, &cb](JComboBox* source) {
        popupRequested = (source == &cb);
    });
    
    cb.handleMousePress(10.f, 10.f);
    assert(popupRequested == true);
    // Index should not have changed in popup mode until callback sets it
    assert(cb.currentIndex() == 1);
    
    std::cout << "test_combobox_logic passed" << std::endl;
}

void test_textarea_logic() {
    JSceneGraph graph;
    JTextArea ta(graph, "Placeholder");
    
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

    JKeyEvent ke;
    ke.pressed = true;
    ke.utf8[0] = '!';
    ke.utf8[1] = '\0';
    bool handled = ta.handleKeyEvent(ke);
    assert(handled == true);
    assert(ta.text() == "Line 1\nLine 2!");

    ke.key = JKeyEvent::JKey::Backspace;
    ke.utf8[0] = '\0';
    handled = ta.handleKeyEvent(ke);
    assert(handled == true);
    assert(ta.text() == "Line 1\nLine 2");

    // Test Space key entry
    ke.key = JKeyEvent::JKey::Space;
    ke.utf8[0] = ' ';
    ke.utf8[1] = '\0';
    handled = ta.handleKeyEvent(ke);
    assert(handled == true);
    assert(ta.text() == "Line 1\nLine 2 ");

    // Test Question mark Shift key resolution
    ke.key = JKeyEvent::JKey::Unknown;
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
    JSceneGraph graph;
    JScrollArea sa(graph, 200.0f, 100.0f);
    
    JButton* b1 = new JButton(graph, "Child JButton 1", 180.0f, 40.0f);
    JButton* b2 = new JButton(graph, "Child JButton 2", 180.0f, 80.0f);
    sa.addChildWidget(b1);
    sa.addChildWidget(b2);

    assert(sa.children().size() == 2);
    assert(sa.children()[0] == b1);
    assert(sa.children()[1] == b2);

    JPrimitiveBuffer buf;
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
    JSceneGraph graph;
    JListView lv(graph, {"Item 1", "Item 2", "Item 3"});

    int lastSelection = -1;
    lv.onSelectionChanged.connect([&lastSelection](int idx) {
        lastSelection = idx;
    });

    assert(lv.items().size() == 3);
    assert(lv.selectedIndex() == -1);

    lv.setSelectedIndex(1);
    assert(lv.selectedIndex() == 1);
    assert(lastSelection == 1);

    JKeyEvent ke;
    ke.pressed = true;
    ke.key = JKeyEvent::JKey::Down;
    bool handled = lv.handleKeyEvent(ke);
    assert(handled == true);
    assert(lv.selectedIndex() == 2);
    assert(lastSelection == 2);

    ke.key = JKeyEvent::JKey::Up;
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
    JSceneGraph graph;
    JTreeView tv(graph);

    JTreeViewNode rootNode = {
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

    JKeyEvent ke;
    ke.pressed = true;
    ke.key = JKeyEvent::JKey::Down;
    
    bool handled = tv.handleKeyEvent(ke);
    assert(handled == true);
    assert(tv.selectedNode() != nullptr);
    assert(tv.selectedNode()->label == "Child 1");

    handled = tv.handleKeyEvent(ke);
    assert(handled == true);
    assert(tv.selectedNode()->label == "Child 2");

    ke.key = JKeyEvent::JKey::Left;
    handled = tv.handleKeyEvent(ke);
    assert(handled == true);
    assert(flatNodes[1].node->expanded == false);

    flatNodes = tv.getFlatNodes();
    assert(flatNodes.size() == 2);

    std::cout << "test_treeview_logic passed" << std::endl;
}

void test_datagrid_logic() {
    JSceneGraph graph;
    JDataGrid dg(graph, {"ID", "Name", "Role"});
    
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

    JKeyEvent ke;
    ke.pressed = true;
    ke.key = JKeyEvent::JKey::Down;
    bool handled = dg.handleKeyEvent(ke);
    assert(handled == true);
    assert(dg.selectedIndex() == 2);
    assert(lastSelected == 2);

    ke.key = JKeyEvent::JKey::Up;
    handled = dg.handleKeyEvent(ke);
    assert(handled == true);
    assert(dg.selectedIndex() == 1);
    assert(lastSelected == 1);

    int activatedRow = -1;
    dg.onRowActivated.connect([&activatedRow](int r) {
        activatedRow = r;
    });

    ke.key = JKeyEvent::JKey::Return;
    handled = dg.handleKeyEvent(ke);
    assert(handled == true);
    assert(activatedRow == 1);

    std::cout << "test_datagrid_logic passed" << std::endl;
}

void test_menu_and_shortcuts() {
    JSceneGraph graph;
    JMenu menu("File");
    
    auto* item1 = menu.add(graph, "New", {JKeyEvent::JKey::N, true});
    auto* item2 = menu.add(graph, "Open", {JKeyEvent::JKey::O, true});
    item2->setCheckable(true);
    item2->setChecked(true);

    assert(item1->label() == "New");
    assert(item1->shortcut().key == JKeyEvent::JKey::N);
    assert(item1->shortcut().ctrl == true);

    assert(item2->isCheckable() == true);
    assert(item2->isChecked() == true);

    bool triggered = false;
    JMenuManager::instance().clearShortcuts();
    JMenuManager::instance().registerShortcut(item1->shortcut(), [&triggered]() {
        triggered = true;
    });

    JKeyEvent ke;
    ke.pressed = true;
    ke.key = JKeyEvent::JKey::N;
    ke.ctrl = true;
    
    bool handled = JMenuManager::instance().processAccelerator(ke);
    assert(handled == true);
    assert(triggered == true);

    std::cout << "test_menu_and_shortcuts passed" << std::endl;
}

void test_tooltips() {
    JSceneGraph graph;
    JButton btn(graph, "TooltipButton");
    btn.setTooltip("Click this button");
    
    auto& layout = graph.getLayout(btn.getNodeId());
    layout.boundingBox = {10, 10, 100, 40};

    assert(btn.tooltip() == "Click this button");
    assert(btn.hitTest(50, 30) == true);
    assert(btn.hitTest(200, 30) == false);

    bool found = false;
    for (JWidget* w : JWidget::s_activeWidgets) {
        if (w == &btn) {
            found = true;
            break;
        }
    }
    assert(found == true);

    std::cout << "test_tooltips passed" << std::endl;
}

// The spin box's value field is a REAL text field (JNumericField over JTextEditCore), so the value can be
// edited in PART: caret placement, selection, clipboard, Home/End, and a commit/revert model. This guards the
// behaviour that the old private edit buffer could not provide — it pinned the caret to the end of the text and
// replaced the whole value on the first keystroke.
void test_spinbox_text_editing() {
    JSceneGraph graph;
    JDoubleSpinBox spin(graph, -100.0, 100.0, /*step=*/1.0, /*decimals=*/1);
    auto& layout = graph.getLayout(spin.getNodeId());
    layout.boundingBox = {0, 0, 120, 24};
    spin.setValue(12.5);

    JKeyEvent ke; ke.pressed = true;
    auto press = [&](JKeyEvent::JKey k, char c = '\0', bool ctrl = false) {
        ke.key = k; ke.ctrl = ctrl; ke.utf8[0] = c; ke.utf8[1] = '\0';
        return spin.handleKeyEvent(ke);
    };
    auto ch = [&](char c) { return press(JKeyEvent::JKey::Unknown, c); };

    // Focus arrives -> the field is live with the value in it, selected: Tab in and type to replace.
    spin.setFocused(true);
    assert(spin.hasSelection());
    assert(spin.selectedText() == "12.5");

    // EDIT PART OF THE VALUE: End, then backspace one digit and type another -> 12.7, not 7.
    assert(press(JKeyEvent::JKey::End));
    assert(!spin.hasSelection());
    assert(press(JKeyEvent::JKey::Backspace));
    assert(ch('7'));
    assert(press(JKeyEvent::JKey::Return));
    assert(spin.value() == 12.7);

    // Return keeps the box editable (it still holds focus) with the committed value selected.
    assert(spin.selectedText() == "12.7");
    // Home + type inserts at the FRONT of the value rather than replacing it.
    assert(press(JKeyEvent::JKey::Home));
    assert(ch('9'));                       // "912.7"
    assert(press(JKeyEvent::JKey::Return));
    assert(spin.value() == 100.0);         // committed and CLAMPED to the range

    // Escape restores the value focus arrived with, whatever was typed since.
    spin.setFocused(false);
    spin.setValue(4.0);
    spin.setFocused(true);
    assert(ch('8'));                       // replaces the selection -> "8"
    assert(press(JKeyEvent::JKey::Escape));
    assert(spin.value() == 4.0);

    // The numeric grammar refuses what cannot build a number. An ACTIVE field still consumes the keystroke
    // (it is a text field with the caret in it, exactly like JLineEdit) but rejects the character...
    assert(ch('x'));
    assert(spin.text() == "4.0");   // Escape above restored the value; 'x' changed nothing
    // ...while an INACTIVE field lets it bubble, so a host's own shortcuts still work over a spin box that
    // nobody is typing into — which is how a canvas-hosted control keeps its authoring keys.
    spin.setFocused(false);
    assert(!ch('x'));
    assert(spin.value() == 4.0);

    // A second decimal point is refused, and the partial states typing passes through ("1", "1.") are not.
    assert(ch('1')); assert(ch('.')); assert(ch('5'));   // the first char re-activates the field
    assert(ch('.'));                                     // consumed by the field...
    assert(press(JKeyEvent::JKey::Return));
    assert(spin.value() == 1.5);                         // ...but rejected, so the value is 1.5, not garbage

    // Ctrl+C lifts the selected text to the clipboard — this is what copying a value out of a properties
    // panel depends on.
    spin.setFocused(true);
    spin.selectAll();
    assert(press(JKeyEvent::JKey::C, 'c', /*ctrl=*/true));
    assert(JWidget::clipboardGet() == "1.5");

    // Up/Down step the value and re-select it, so the next keystroke replaces the stepped number.
    assert(press(JKeyEvent::JKey::Up));
    assert(spin.value() == 2.5);
    assert(spin.selectedText() == "2.5");
    assert(press(JKeyEvent::JKey::Down));
    assert(spin.value() == 1.5);

    // A value pushed from elsewhere refreshes an untouched field, but NEVER overwrites an edit in progress.
    spin.setValue(7.5);
    assert(spin.selectedText() == "7.5" || spin.text() == "7.5");
    assert(ch('3'));                       // now dirty
    spin.setValue(9.5);                    // e.g. a data source repainting underneath
    assert(spin.text() == "3");            // the typing survived
    spin.setFocused(false);              // focus-out commits it
    assert(spin.value() == 3.0);

    // Integer boxes get the same field, with no decimal point in the grammar.
    JSpinBox ispin(graph, 0, 500);
    auto& il = graph.getLayout(ispin.getNodeId());
    il.boundingBox = {0, 0, 120, 24};
    ispin.setValue(42);
    ispin.setFocused(true);
    ke.key = JKeyEvent::JKey::End; ke.ctrl = false; ke.utf8[0] = '\0';
    assert(ispin.handleKeyEvent(ke));
    ke.key = JKeyEvent::JKey::Unknown; ke.utf8[0] = '.'; ke.utf8[1] = '\0';
    assert(ispin.handleKeyEvent(ke));      // consumed by the field...
    assert(ispin.text() == "42");           // ...and rejected: integers have no decimal point
    ke.utf8[0] = '7';
    assert(ispin.handleKeyEvent(ke));
    ke.key = JKeyEvent::JKey::Return; ke.utf8[0] = '\0';
    assert(ispin.handleKeyEvent(ke));
    assert(ispin.value() == 427);          // appended a digit instead of replacing the value

    std::cout << "test_spinbox_text_editing passed" << std::endl;
}

int main() {
    test_button_interaction();
    test_widget_rendering();
    test_slider_logic();
    test_spinbox_text_editing();
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
