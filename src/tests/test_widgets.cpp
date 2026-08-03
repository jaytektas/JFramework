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
#include <memory>
#include <cstdio>
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

// A spin box is a JLineEdit plus two steppers, so the value edits like any other text field: caret placement,
// partial edits, selection, clipboard. This guards that contract, plus the value semantics layered on top --
// the grammar the validator enforces, the commit points, and the rule that a live data source may never
// overwrite text the user is typing.
void test_spinbox_text_editing() {
    JSceneGraph graph;
    JDoubleSpinBox spin(graph, -100.0, 100.0, /*step=*/1.0, /*decimals=*/1);
    auto& layout = graph.getLayout(spin.getNodeId());
    layout.boundingBox = {0, 0, 120, 24};
    spin.setValue(12.5);
    // A real focus chain: focus in/out must go through the manager, because the box distinguishes a genuine
    // blur (the user moved focus elsewhere) from focus bookkeeping by asking whether it is in the chain at all.
    JFocusManager focus;
    focus.registerWidget(&spin);

    JKeyEvent ke; ke.pressed = true;
    auto press = [&](JKeyEvent::JKey k, char c = '\0', bool ctrl = false) {
        ke.key = k; ke.ctrl = ctrl; ke.utf8[0] = c; ke.utf8[1] = '\0';
        return spin.handleKeyEvent(ke);
    };
    auto ch = [&](char c) { return press(JKeyEvent::JKey::Unknown, c); };

    // Focus arrives -> the field is live with the value in it, selected: Tab in and type to replace.
    focus.setFocus(&spin);
    assert(spin.selectedText() == "12.5");

    // EDIT PART OF THE VALUE: End, then swap one digit -> 12.7, not 7.
    assert(press(JKeyEvent::JKey::End));
    assert(!spin.hasSelection());
    assert(press(JKeyEvent::JKey::Backspace));
    assert(ch('7'));
    assert(spin.text() == "12.7");
    assert(press(JKeyEvent::JKey::Return));
    assert(spin.value() == 12.7);
    // Return leaves you IN the field with the caret where it was -- re-selecting the whole value would mean the
    // next keystroke silently wiped it.
    assert(!spin.hasSelection());

    // A keystroke that cannot lead to an in-range value is refused as you type (Home + '9' would be 912.7,
    // above the maximum), so a bad number never reaches the value at all.
    assert(press(JKeyEvent::JKey::Home));
    ch('9');
    assert(spin.text() == "12.7");
    assert(spin.value() == 12.7);

    // Escape restores the value focus arrived with.
    assert(press(JKeyEvent::JKey::Escape));
    assert(spin.value() == 12.5);

    // Characters the grammar cannot use are never offered to the field, so a host's own shortcuts (letters over
    // a canvas) keep working over a spin box.
    assert(!ch('x'));
    assert(spin.value() == 12.5);

    // Below the range is Intermediate, not Invalid: a value stays reachable one keystroke at a time.
    assert(ch('8'));                       // replaces the selection Escape left
    assert(ch('.')); assert(ch('5'));
    assert(spin.text() == "8.5");
    assert(press(JKeyEvent::JKey::Return));
    assert(spin.value() == 8.5);

    // Ctrl+C lifts the selected text to the clipboard -- what copying a value out of a properties panel needs.
    spin.selectAll();
    assert(press(JKeyEvent::JKey::C, 'c', /*ctrl=*/true));
    assert(JWidget::clipboardGet() == "8.5");

    // Up/Down step the value and show the result, unselected so the next keystroke edits rather than replaces.
    assert(press(JKeyEvent::JKey::Up));
    assert(spin.value() == 9.5);
    assert(spin.text() == "9.5");
    assert(!spin.hasSelection());
    assert(press(JKeyEvent::JKey::Down));
    assert(spin.value() == 8.5);

    // A value pushed from a data source NEVER overwrites an edit in progress, and the stale text it left on
    // screen is not committed back over the new value on blur.
    spin.selectAll();
    assert(ch('3'));                       // the user is now typing (replacing the selection)
    spin.setValue(7.5);                    // e.g. the source repainting underneath
    assert(spin.text() == "3");            // typing survived
    focus.setFocus(nullptr);                // focus-out commits what was TYPED
    assert(spin.value() == 3.0);
    focus.setFocus(&spin);
    spin.setValue(6.5);                    // untouched field -> follows the source
    focus.setFocus(nullptr);
    assert(spin.value() == 6.5);           // ...and blur did not push the old text back

    // Integer boxes get the same field, with no decimal point in the grammar.
    JSpinBox ispin(graph, 0, 500);
    auto& il = graph.getLayout(ispin.getNodeId());
    il.boundingBox = {0, 0, 120, 24};
    ispin.setValue(42);
    focus.registerWidget(&ispin);
    focus.setFocus(&ispin);
    ke.key = JKeyEvent::JKey::End; ke.ctrl = false; ke.utf8[0] = '\0';
    assert(ispin.handleKeyEvent(ke));
    ke.key = JKeyEvent::JKey::Unknown; ke.utf8[0] = '.'; ke.utf8[1] = '\0';
    assert(!ispin.handleKeyEvent(ke));     // integers have no decimal point: never offered, so it bubbles
    assert(ispin.text() == "42");
    ke.utf8[0] = '7';
    assert(ispin.handleKeyEvent(ke));
    ke.key = JKeyEvent::JKey::Return; ke.utf8[0] = '\0';
    assert(ispin.handleKeyEvent(ke));
    assert(ispin.value() == 427);          // appended a digit instead of replacing the value

    std::cout << "test_spinbox_text_editing passed" << std::endl;
}

// A composed control's inner field is NoFocus: it must never OWN focus, or the next syncOrder() clears it and
// takes the caret with it. Its focus request goes to the nearest focusable ancestor (Qt's focus proxy), and the
// edit it started survives the focus bookkeeping that a host driving its own controls generates every frame.
void test_focus_proxy_and_edit_survival() {
    JSceneGraph graph;
    JFocusManager focus;
    JDoubleSpinBox spin(graph, 0.0, 100.0, 1.0, 1);
    graph.getLayout(spin.getNodeId()).boundingBox = {0, 0, 120, 24};
    focus.registerWidget(&spin);

    JLineEdit* field = spin.lineEdit();
    assert(field != nullptr);
    assert(!field->isFocusable());          // the field is not a tab stop; the spin box is

    field->requestFocus();                  // the field asks for focus...
    assert(focus.focused() == &spin);       // ...and the SPIN BOX takes it

    spin.handleMousePress(10.0f, 12.0f);    // click in the value area -> editing, caret placed
    assert(field->isFocused());

    // The host (studio canvas) keeps its controls out of the focus chain, so a rebuild clears the manager's
    // focus. That is bookkeeping, NOT the user leaving the field: the edit must survive it.
    focus.syncOrder();
    assert(focus.focused() == nullptr);
    assert(field->isFocused());

    // Only an explicit end -- a real blur, or the host saying so -- tears it down.
    spin.endEdit();
    assert(!field->isFocused());

    std::cout << "test_focus_proxy_and_edit_survival passed" << std::endl;
}

// The caret must reach the PRIMITIVE BUFFER, not just the model: the spin box paints through its adopted
// JLineEdit, so a composition mistake (child not painted, wrong bounds, clip swallowing it) would leave the
// value looking inert no matter how correct the caret index was.
void test_spinbox_paints_caret() {
    JSceneGraph graph;
    JFocusManager focus;
    JDoubleSpinBox spin(graph, 0.0, 100.0, 1.0, 1);
    graph.getLayout(spin.getNodeId()).boundingBox = {10, 20, 120, 24};
    focus.registerWidget(&spin);
    spin.setValue(98.0);

    auto rectCount = [](const JPrimitiveBuffer& b) {
        size_t n = 0;
        for (const auto& c : b.getCommands())
            if (c.kind == JPrimitiveBuffer::JDrawCommand::JKind::JRect) ++n;
        return n;
    };

    JPrimitiveBuffer idle;
    spin.populateRenderPrimitives(idle);
    const size_t nIdle = rectCount(idle);
    assert(nIdle >= 3);                        // field + two steppers at least

    spin.handleMousePress(40.0f, 30.0f);       // click the value area -> editing
    assert(spin.lineEdit()->isFocused());
    JPrimitiveBuffer editing;
    spin.populateRenderPrimitives(editing);
    const size_t nEdit = rectCount(editing);
    assert(nEdit > nIdle);                     // the caret is an extra rect

    // The caret sits inside the FIELD, left of the steppers, and spans most of the height -- i.e. it is really a
    // caret and really on screen, not a stray rect somewhere outside the widget.
    const jf::JRect fb = spin.lineEdit()->bounds();
    bool caretFound = false;
    for (const auto& c : editing.getCommands()) {
        if (c.kind != JPrimitiveBuffer::JDrawCommand::JKind::JRect) continue;
        const float x = c.rect.rectBounds[0], y = c.rect.rectBounds[1];
        const float w = c.rect.rectBounds[2], h = c.rect.rectBounds[3];
        if (w <= 2.0f && h >= fb.height * 0.4f &&
            x >= fb.x && x <= fb.x + fb.width && y >= fb.y && y <= fb.y + fb.height)
            caretFound = true;
    }
    assert(caretFound);

    std::cout << "test_spinbox_paints_caret passed" << std::endl;
}

// Shift+Tab reaches the app in two different spellings: X11 sends its own key (BackTab), Win32 and macOS send
// Tab with the shift modifier. Focus-domain code that matches only one navigates FORWARD on the other
// platforms -- a bug invisible on the machine it was written on. jIsTabNav/jTabNavDir are the single rule.
void test_tab_nav_is_portable() {
    JKeyEvent tab;      tab.pressed = true;  tab.key = JKeyEvent::JKey::Tab;
    JKeyEvent x11Back;  x11Back.pressed = true; x11Back.key = JKeyEvent::JKey::BackTab;      // X11
    JKeyEvent winBack;  winBack.pressed = true; winBack.key = JKeyEvent::JKey::Tab; winBack.shift = true;  // Win32 / macOS

    assert(jIsTabNav(tab) && jIsTabNav(x11Back) && jIsTabNav(winBack));
    assert(jTabNavDir(tab) == 1);
    assert(jTabNavDir(x11Back) == -1);
    assert(jTabNavDir(winBack) == -1);   // the spelling that used to walk the wrong way

    JKeyEvent other; other.pressed = true; other.key = JKeyEvent::JKey::Return;
    assert(!jIsTabNav(other));

    std::cout << "test_tab_nav_is_portable passed" << std::endl;
}

// Destroying a focused widget must not leave the focus manager holding it. A properties form rebuilt on a new
// selection, or a dialog page swapped out, destroys widgets that may hold focus; the manager kept raw pointers
// and released the focus LATER -- by calling setFocused(false) on freed memory. That crashed the studio on the
// next mouse move.
void test_focus_released_when_widget_dies() {
    JSceneGraph graph;
    JFocusManager focus;

    auto btn = std::make_unique<JButton>(graph, "Doomed");
    graph.getLayout(btn->getNodeId()).boundingBox = {0, 0, 80, 24};
    focus.registerWidget(btn.get());
    focus.setFocus(btn.get());
    assert(focus.focused() == btn.get());
    assert(focus.isInOrder(btn.get()));

    btn.reset();                       // destroyed while focused AND while in the order
    assert(focus.focused() == nullptr);
    assert(focus.order().empty());

    focus.syncOrder();                 // the path that used to dereference the dead widget
    focus.focusAt(10.f, 10.f);
    assert(focus.focused() == nullptr);

    std::cout << "test_focus_released_when_widget_dies passed" << std::endl;
}

// A rebuild must not move the reader. setRootNode is called for a mode flip, a filter change or a
// one-row edit, and it already keeps the SELECTION across one; the scroll position deserves the same,
// or renaming a node in a long tree scrolls every other row out from under the user.
void test_treeview_keeps_scroll_on_rebuild() {
    JSceneGraph graph;
    JTreeView tv(graph);
    graph.getLayout(tv.getNodeId()).boundingBox = {0, 0, 240, 200};   // ~8 rows visible

    auto build = [] {
        JTreeViewNode root{"Root", true, false, {}};
        for (int i = 0; i < 60; ++i)
            root.children.push_back(JTreeViewNode{"row_" + std::to_string(i), false, false, {}});
        return root;
    };
    tv.setRootNode(build());
    assert(tv.scrollY() == 0.0f);

    tv.handleScroll(10.f, 10.f, -20.f);          // wheel down
    const float scrolled = tv.scrollY();
    assert(scrolled > 0.0f);

    tv.setRootNode(build());                      // same shape: the position must survive
    assert(tv.scrollY() == scrolled);

    // A tree that got SHORTER clamps into range rather than leaving the view past the end.
    JTreeViewNode small{"Root", true, false, {}};
    small.children.push_back(JTreeViewNode{"only", false, false, {}});
    tv.setRootNode(std::move(small));
    assert(tv.scrollY() == 0.0f);

    std::cout << "test_treeview_keeps_scroll_on_rebuild passed" << std::endl;
}

int main() {
    test_button_interaction();
    test_widget_rendering();
    test_slider_logic();
    test_spinbox_text_editing();
    test_focus_proxy_and_edit_survival();
    test_spinbox_paints_caret();
    test_tab_nav_is_portable();
    test_focus_released_when_widget_dies();
    test_combobox_logic();
    test_textarea_logic();
    test_scrollarea_logic();
    test_listview_logic();
    test_treeview_logic();
    test_treeview_keeps_scroll_on_rebuild();
    test_datagrid_logic();
    test_menu_and_shortcuts();
    test_tooltips();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
