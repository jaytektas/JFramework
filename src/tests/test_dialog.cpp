// test_dialog.cpp — JDialog / JDialogManager unit tests
// Tests queue logic, renderAndHandle in headless mode, and AI signal emission.

#include <genesis/core/Dialog.h>
#include <genesis/core/AiBusHook.h>
#include <cassert>
#include <string>
#include <iostream>
#include <vector>

using namespace jf;

static void _clearDialogs() {
    while (JDialogManager::instance().hasPending())
        JDialogManager::instance().pop();
}

static void test_message_queues() {
    _clearDialogs();
    bool dismissed = false;
    JDialog::message("Title", "Body", [&]{ dismissed = true; });
    assert(JDialogManager::instance().hasPending());
    const auto* req = JDialogManager::instance().front();
    assert(req);
    assert(req->title == "Title");
    assert(req->body  == "Body");
    assert(req->kind  == JDialogRequest::JKind::Message);
    // Simulate OK click by calling onOk
    if (req->onOk) req->onOk();
    JDialogManager::instance().pop();
    assert(dismissed);
    assert(!JDialogManager::instance().hasPending());
    std::cout << "  [OK] message() queues request and fires onDismiss\n";
}

static void test_confirm_queues() {
    _clearDialogs();
    bool confirmed = false, cancelled = false;
    JDialog::confirm("Sure?", "Are you sure?",
        [&]{ confirmed = true; },
        [&]{ cancelled = true; });
    auto* req = JDialogManager::instance().front();
    assert(req);
    assert(req->kind == JDialogRequest::JKind::Confirm);
    if (req->onOk) req->onOk();
    JDialogManager::instance().pop();
    assert(confirmed && !cancelled);
    std::cout << "  [OK] confirm() queues request with onOk/onCancel\n";
}

static void test_input_queues() {
    _clearDialogs();
    std::string received;
    JDialog::input("Enter", "Name:", [&](std::string s){ received = s; }, {}, "placeholder");
    auto* req = JDialogManager::instance().front();
    assert(req);
    assert(req->kind == JDialogRequest::JKind::Input);
    assert(req->placeholder == "placeholder");
    if (req->onInput) req->onInput("hello");
    JDialogManager::instance().pop();
    assert(received == "hello");
    std::cout << "  [OK] input() queues request with onInput and placeholder\n";
}

static void test_queue_is_fifo() {
    _clearDialogs();
    JDialog::message("A", "first");
    JDialog::message("B", "second");
    assert(JDialogManager::instance().front()->title == "A");
    JDialogManager::instance().pop();
    assert(JDialogManager::instance().front()->title == "B");
    JDialogManager::instance().pop();
    std::cout << "  [OK] queue is FIFO\n";
}

static void test_render_no_dialog_returns_false() {
    _clearDialogs();
    JPrimitiveBuffer buf;
    bool active = JDialogManager::renderAndHandle(buf, 800.f, 600.f, 0.f, 0.f, false, false);
    assert(!active);
    assert(buf.getCommands().size() == 0);
    std::cout << "  [OK] renderAndHandle returns false when no dialog pending\n";
}

static void test_render_message_dialog_draws_overlay() {
    _clearDialogs();
    JDialog::message("Hello", "World");
    JPrimitiveBuffer buf;
    bool active = JDialogManager::renderAndHandle(buf, 800.f, 600.f, 0.f, 0.f, false, false);
    assert(active);
    // Should draw backdrop + box + buttons — at least 2 primitives
    assert(buf.getCommands().size() >= 2);
    _clearDialogs();
    std::cout << "  [OK] renderAndHandle draws primitives for message dialog\n";
}

static void test_render_ok_click_dismisses() {
    _clearDialogs();
    bool dismissed = false;
    JDialog::message("Click me", "Click OK", [&]{ dismissed = true; });
    JPrimitiveBuffer buf;
    // Simulate clicking in the center-bottom area where OK button would be
    // Box: x=(800-440)/2=180, y=(600-160)/2=220, h=160
    // btnY = 220+160-30-16 = 334
    // okX = center: 180+(440-88)/2 = 180+176 = 356
    float okX = 356.f + 44.f; // center of OK button
    float okY = 334.f + 15.f;
    JDialogManager::renderAndHandle(buf, 800.f, 600.f, okX, okY, true, true);
    assert(dismissed);
    assert(!JDialogManager::instance().hasPending());
    std::cout << "  [OK] renderAndHandle dismisses on OK click\n";
}

static void test_ai_signals_on_dialog() {
    _clearDialogs();
    std::vector<std::string> sigs;
    JAiBusHook::install([&](uint32_t, const char* sig, const char*) {
        sigs.push_back(sig);
    });
    JDialog::message("Sig", "Test");
    assert(!sigs.empty() && sigs.back() == "dialog.message");
    JDialog::confirm("Sig2", "Q", []{});
    assert(sigs.back() == "dialog.confirm");
    JDialog::input("Sig3", "Q", [](std::string){});
    assert(sigs.back() == "dialog.input");
    JAiBusHook::install(nullptr);
    _clearDialogs();
    std::cout << "  [OK] AI bus signals emitted on dialog.message/confirm/input\n";
}

static void test_execute_action() {
    _clearDialogs();
    bool okFired = false;
    JDialog::message("JAction", "Test", [&]{ okFired = true; });
    bool handled = JDialogManager::executeAction("ok");
    assert(handled);
    assert(okFired);
    assert(!JDialogManager::instance().hasPending());
    std::cout << "  [OK] JDialogManager::executeAction(\"ok\") fires callback\n";
}

int main() {
    std::cout << "JDialog tests:\n";
    test_message_queues();
    test_confirm_queues();
    test_input_queues();
    test_queue_is_fifo();
    test_render_no_dialog_returns_false();
    test_render_message_dialog_draws_overlay();
    test_render_ok_click_dismisses();
    test_ai_signals_on_dialog();
    test_execute_action();
    std::cout << "All JDialog tests passed.\n";
    return 0;
}
