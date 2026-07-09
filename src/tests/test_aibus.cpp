// Headless test for JAiBus: enable → publish a widget snapshot → service a click action, all without a
// display. Proves the transport + action dispatch work on the main thread with no per-widget AI code.
#include <j/core/JAiBus.h>
#include <j/core/JAiBusAbi.h>
#include <j/core/SceneGraph.h>
#include <j/core/JButton.h>
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

    munmap(bus, sizeof(JAiBusShared)); ::close(fd); shm_unlink(kName);
    std::printf("All JAiBus tests passed.\n");
    return 0;
}
