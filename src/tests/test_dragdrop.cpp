#include <genesis/core/DragDrop.h>
#include <cassert>
#include <iostream>

using namespace jf;

void test_basic_typed_drag() {
    assert(!JDragDrop::isDragging());
    JDragDrop::start<int>(42, 10.f, 20.f, "integer");
    assert(JDragDrop::isDragging());
    assert(JDragDrop::label() == "integer");
    assert(JDragDrop::canAccept<int>());
    assert(!JDragDrop::canAccept<std::string>());  // wrong type

    int received = -1;
    bool ok = JDragDrop::accept<int>(100.f, 200.f,
        [&](const int& v, float, float){ received = v; });
    assert(ok);
    assert(received == 42);
    assert(!JDragDrop::isDragging());  // consumed
    std::cout << "test_basic_typed_drag passed\n";
}

void test_cancel() {
    JDragDrop::start<std::string>(std::string("hello"), 0.f, 0.f);
    assert(JDragDrop::isDragging());
    JDragDrop::cancel();
    assert(!JDragDrop::isDragging());
    // accept should return false after cancel
    bool called = false;
    bool ok = JDragDrop::accept<std::string>(
        [&](const std::string&){ called = true; });
    assert(!ok && !called);
    std::cout << "test_cancel passed\n";
}

void test_type_mismatch() {
    JDragDrop::start<JFileListPayload>(JFileListPayload{{"a.txt","b.txt"}}, 0.f, 0.f);
    bool calledString = false;
    bool ok = JDragDrop::accept<std::string>([&](const std::string&){ calledString = true; });
    assert(!ok && !calledString);  // wrong type rejected
    assert(JDragDrop::isDragging());  // still live

    std::vector<std::string> got;
    bool ok2 = JDragDrop::accept<JFileListPayload>(
        [&](const JFileListPayload& p){ got = p.paths; });
    assert(ok2);
    assert(got.size() == 2 && got[0] == "a.txt");
    std::cout << "test_type_mismatch passed\n";
}

void test_update_cursor() {
    JDragDrop::start<JTextPayload>(JTextPayload{"drag me"}, 10.f, 10.f);
    JDragDrop::update(50.f, 80.f);
    assert(JDragDrop::cursorX() == 50.f);
    assert(JDragDrop::cursorY() == 80.f);
    JDragDrop::cancel();
    std::cout << "test_update_cursor passed\n";
}

void test_widget_reorder_payload() {
    JWidgetIdPayload p{7, "JButton"};
    JDragDrop::start<JWidgetIdPayload>(p, 0.f, 0.f, "JButton");
    assert(JDragDrop::canAccept<JWidgetIdPayload>());
    uint32_t gotId = 0;
    JDragDrop::accept<JWidgetIdPayload>([&](const JWidgetIdPayload& w){ gotId = w.nodeId; });
    assert(gotId == 7);
    std::cout << "test_widget_reorder_payload passed\n";
}

int main() {
    test_basic_typed_drag();
    test_cancel();
    test_type_mismatch();
    test_update_cursor();
    test_widget_reorder_payload();
    std::cout << "All JDragDrop tests passed!\n";
    return 0;
}
