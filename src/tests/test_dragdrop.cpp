#include <genesis/core/DragDrop.h>
#include <cassert>
#include <iostream>

using namespace Genesis;

void test_basic_typed_drag() {
    assert(!DragDrop::isDragging());
    DragDrop::start<int>(42, 10.f, 20.f, "integer");
    assert(DragDrop::isDragging());
    assert(DragDrop::label() == "integer");
    assert(DragDrop::canAccept<int>());
    assert(!DragDrop::canAccept<std::string>());  // wrong type

    int received = -1;
    bool ok = DragDrop::accept<int>(100.f, 200.f,
        [&](const int& v, float, float){ received = v; });
    assert(ok);
    assert(received == 42);
    assert(!DragDrop::isDragging());  // consumed
    std::cout << "test_basic_typed_drag passed\n";
}

void test_cancel() {
    DragDrop::start<std::string>(std::string("hello"), 0.f, 0.f);
    assert(DragDrop::isDragging());
    DragDrop::cancel();
    assert(!DragDrop::isDragging());
    // accept should return false after cancel
    bool called = false;
    bool ok = DragDrop::accept<std::string>(
        [&](const std::string&){ called = true; });
    assert(!ok && !called);
    std::cout << "test_cancel passed\n";
}

void test_type_mismatch() {
    DragDrop::start<FileListPayload>(FileListPayload{{"a.txt","b.txt"}}, 0.f, 0.f);
    bool calledString = false;
    bool ok = DragDrop::accept<std::string>([&](const std::string&){ calledString = true; });
    assert(!ok && !calledString);  // wrong type rejected
    assert(DragDrop::isDragging());  // still live

    std::vector<std::string> got;
    bool ok2 = DragDrop::accept<FileListPayload>(
        [&](const FileListPayload& p){ got = p.paths; });
    assert(ok2);
    assert(got.size() == 2 && got[0] == "a.txt");
    std::cout << "test_type_mismatch passed\n";
}

void test_update_cursor() {
    DragDrop::start<TextPayload>(TextPayload{"drag me"}, 10.f, 10.f);
    DragDrop::update(50.f, 80.f);
    assert(DragDrop::cursorX() == 50.f);
    assert(DragDrop::cursorY() == 80.f);
    DragDrop::cancel();
    std::cout << "test_update_cursor passed\n";
}

void test_widget_reorder_payload() {
    WidgetIdPayload p{7, "Button"};
    DragDrop::start<WidgetIdPayload>(p, 0.f, 0.f, "Button");
    assert(DragDrop::canAccept<WidgetIdPayload>());
    uint32_t gotId = 0;
    DragDrop::accept<WidgetIdPayload>([&](const WidgetIdPayload& w){ gotId = w.nodeId; });
    assert(gotId == 7);
    std::cout << "test_widget_reorder_payload passed\n";
}

int main() {
    test_basic_typed_drag();
    test_cancel();
    test_type_mismatch();
    test_update_cursor();
    test_widget_reorder_payload();
    std::cout << "All DragDrop tests passed!\n";
    return 0;
}
