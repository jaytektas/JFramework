// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// Headless test for JAiBus: enable → publish a widget snapshot → service a click action, all without a
// display. Proves the transport + action dispatch work on the main thread with no per-widget AI code.
#include <j/core/JAiBus.h>
#include <j/core/JAiBusAbi.h>
#include <j/core/SceneGraph.h>
#include <j/core/JButton.h>
#include <j/core/JLineEdit.h>
#include <j/core/FocusManager.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace jf;

int main() {
    const char* kName = "/jf_aibus_test";
    shm_unlink(kName);   // fresh

    JSceneGraph g;
    JFocusManager focus;
    JButton btn(g, "Go");
    btn.setBounds({10.f, 10.f, 80.f, 24.f});
    bool clicked = false;
    btn.onClicked.connect([&]{ clicked = true; });
    JLineEdit field(g, "value");
    field.setBounds({10.f, 40.f, 120.f, 24.f});
    bool returned = false;
    field.onReturnPressed.connect([&]{ returned = true; });

    JAiBus::instance().enable(kName);
    assert(JAiBus::instance().enabled() && "bus should enable");

    // Publish, then read the segment the way a client would.
    JAiBus::instance().tick(JWidget::s_activeWidgets, &focus);

    int fd = shm_open(kName, O_RDWR, 0600);
    assert(fd >= 0);
    auto* bus = static_cast<JAiBusShared*>(mmap(nullptr, sizeof(JAiBusShared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(bus != MAP_FAILED);
    assert(bus->magic == kAiBusMagic && bus->version == kAiBusVersion && "ABI header");

    // The button must appear in the snapshot with its role + name.
    bool found = false; uint32_t btnId = 0;
    for (uint32_t i = 0; i < bus->nodeCount; ++i) {
        if (std::string(bus->nodes[i].name) == "Go" && std::string(bus->nodes[i].role) == "JButton") {
            found = true; btnId = bus->nodes[i].id;
            assert(bus->nodes[i].w == 80.f && bus->nodes[i].h == 24.f && "geometry published");
        }
    }
    assert(found && "button node published");
    std::printf("  [OK] snapshot published (%u nodes, button id=%u)\n", bus->nodeCount, btnId);
    assert(!(bus->seq.load() & 1u) && "seqlock even (frame complete)");

    // Send a click action to the button id and service it.
    bus->action.targetId = btnId;
    aiBusCopy(bus->action.action, sizeof(bus->action.action), "click");
    const uint32_t req = bus->action.requestSeq.load() + 1;
    bus->action.requestSeq.store(req);
    const bool acted = JAiBus::instance().tick(JWidget::s_activeWidgets, &focus);
    assert(acted && "tick serviced the action");
    assert(bus->action.ackSeq.load() == req && "action acked");
    assert(bus->action.resultCode == 1 && "click handled");
    assert(clicked && "button onClicked fired via the bus");
    std::printf("  [OK] click action serviced (ack=%u result=%d, onClicked fired)\n", req, bus->action.resultCode);

    // Bad target id → resultCode -1.
    bus->action.targetId = 0xDEADBEEF;
    aiBusCopy(bus->action.action, sizeof(bus->action.action), "click");
    bus->action.requestSeq.store(req + 1);
    JAiBus::instance().tick(JWidget::s_activeWidgets, &focus);
    assert(bus->action.resultCode == -1 && "bad target → -1");
    std::printf("  [OK] bad target id → resultCode -1\n");

    // Typing into a field, then Return, through the field's own keyboard path.
    JAiBus::instance().tick(JWidget::s_activeWidgets, &focus);   // publish the field
    uint32_t fieldId = 0;
    for (uint32_t i = 0; i < bus->nodeCount; ++i)
        if (std::string(bus->nodes[i].role) == "JLineEdit") fieldId = bus->nodes[i].id;
    assert(fieldId && "field published");
    auto act = [&](const char* action, uint32_t seq) {
        bus->action.targetId = fieldId;
        aiBusCopy(bus->action.action, sizeof(bus->action.action), action);
        bus->action.requestSeq.store(seq);
        JAiBus::instance().tick(JWidget::s_activeWidgets, &focus);
        return bus->action.resultCode;
    };
    assert(act("type:137.5", req + 2) == 1);
    assert(field.text() == "137.5" && "typed text arrives");
    assert(act("key:Return", req + 3) == 1 && returned && "Return reaches onReturnPressed");
    assert(act("key:NoSuchKey", req + 4) == 0 && "an unknown key is not handled");
    std::printf("  [OK] type: and key: drive a field through its keyboard path\n");

    // The window's own nodes and actions: a native dialog draws no widgets, so the window publishes it and
    // answers it ("dialog:ok") before the app's handler is asked.
    bool answered = false, appAsked = false;
    JAiBus::instance().windowNodes = [] {
        JA11yNode d;
        jA11yCopyStr(d.role, sizeof(d.role), "Dialog");
        jA11yCopyStr(d.name, sizeof(d.name), "Square the Machine");
        jA11yCopyStr(d.value, sizeof(d.value), "Correct for it?");
        return std::vector<JA11yNode>{ d };
    };
    JAiBus::instance().onWindowAction = [&](uint32_t, const std::string& action) {
        if (action != "dialog:ok") return 0;
        answered = true;
        return 1;
    };
    JAiBus::instance().onAction = [&](uint32_t, const std::string&) { appAsked = true; return 0; };
    JAiBus::instance().tick(JWidget::s_activeWidgets, &focus);
    bool dialogSeen = false;
    for (uint32_t i = 0; i < bus->nodeCount; ++i)
        if (std::string(bus->nodes[i].role) == "Dialog" && std::string(bus->nodes[i].name) == "Square the Machine")
            dialogSeen = bus->nodes[i].id >= JAiBus::kWindowNodeIds;
    assert(dialogSeen && "a window node is published after the widgets");
    bus->action.targetId = 0;
    aiBusCopy(bus->action.action, sizeof(bus->action.action), "dialog:ok");
    bus->action.requestSeq.store(req + 5);
    JAiBus::instance().tick(JWidget::s_activeWidgets, &focus);
    assert(bus->action.resultCode == 1 && answered && !appAsked && "the window answers before the app");
    aiBusCopy(bus->action.action, sizeof(bus->action.action), "dock:Jog");
    bus->action.requestSeq.store(req + 6);
    JAiBus::instance().tick(JWidget::s_activeWidgets, &focus);
    assert(appAsked && "what the window does not answer goes on to the app");
    std::printf("  [OK] window nodes published, window actions before the app's\n");

    munmap(bus, sizeof(JAiBusShared)); ::close(fd); shm_unlink(kName);
    std::printf("All JAiBus tests passed.\n");
    return 0;
}
