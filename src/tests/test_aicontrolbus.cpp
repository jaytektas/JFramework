#include <genesis/core/AiControlBus.h>
#include <genesis/core/SceneGraph.h>
#include <cassert>
#include <iostream>

using namespace jf;

void test_telemetry_sync() {
    SharedBusMemory sharedMem;
    JAiControlBus bus;
    bus.attach(&sharedMem);
    
    JSceneGraph graph;
    NodeId root = graph.createNode("Root");
    graph.getLayout(root).boundingBox = {0, 0, 800, 600};
    
    NodeId child = graph.createNode("Child");
    graph.addChild(root, child);
    graph.getLayout(child).boundingBox = {10, 20, 100, 50};
    
    bus.updateTelemetry(graph);
    
    assert(sharedMem.nodeCount == 2);
    assert(sharedMem.nodes[0].id == 0);
    assert(sharedMem.nodes[0].width == 800);
    assert(sharedMem.nodes[1].id == 1);
    assert(sharedMem.nodes[1].x == 10);
    assert(sharedMem.telemetryFrameCounter.load() == 1);
    
    std::cout << "test_telemetry_sync passed" << std::endl;
}

void test_inbound_commands() {
    SharedBusMemory sharedMem;
    JAiControlBus bus;
    bus.attach(&sharedMem);
    
    AiVirtualInput cmd;
    
    // No command initially
    assert(bus.pollInboundCommand(cmd) == false);
    
    // AI injects a command
    sharedMem.inboundCommand.type.store(AiVirtualInput::JCommandType::MouseClick);
    sharedMem.inboundCommand.targetX = 150.0f;
    sharedMem.inboundCommand.targetY = 200.0f;
    sharedMem.inboundCommand.sequenceId.store(1, std::memory_order_release);
    
    assert(bus.pollInboundCommand(cmd) == true);
    assert(cmd.type.load() == AiVirtualInput::JCommandType::MouseClick);
    assert(cmd.targetX == 150.0f);
    
    // Command already processed
    assert(bus.pollInboundCommand(cmd) == false);
    
    // New command
    sharedMem.inboundCommand.sequenceId.store(2, std::memory_order_release);
    assert(bus.pollInboundCommand(cmd) == true);
    
    std::cout << "test_inbound_commands passed" << std::endl;
}

void test_publish_signal() {
    SharedBusMemory sharedMem;
    JAiControlBus bus;
    bus.attach(&sharedMem);

    // Initially: seq must be 0 after attach
    assert(sharedMem.outboundSignal.signalSeq.load() == 0);
    assert(sharedMem.outboundSignal.targetId == 0xFFFFFFFFu);

    // Publish a click signal
    bus.publishSignal(42, "click", "Primary JAction");
    assert(sharedMem.outboundSignal.signalSeq.load() == 1);
    assert(sharedMem.outboundSignal.targetId == 42);
    assert(std::string(sharedMem.outboundSignal.signalName)  == "click");
    assert(std::string(sharedMem.outboundSignal.signalValue) == "Primary JAction");

    // Publish another — sequence must increment
    bus.publishSignal(7, "checked", "true");
    assert(sharedMem.outboundSignal.signalSeq.load() == 2);
    assert(sharedMem.outboundSignal.targetId == 7);
    assert(std::string(sharedMem.outboundSignal.signalName)  == "checked");

    // Value overflow protection: very long value must not overflow the 64-byte field
    std::string longVal(200, 'x');
    bus.publishSignal(1, "text_changed", longVal);
    assert(sharedMem.outboundSignal.signalSeq.load() == 3);
    // signalValue is 64 bytes; last byte must be '\0'
    assert(sharedMem.outboundSignal.signalValue[63] == '\0');

    std::cout << "test_publish_signal passed" << std::endl;
}

void test_outbound_initial_state() {
    SharedBusMemory sharedMem{};
    JAiControlBus bus;
    bus.attach(&sharedMem);

    // After attach the outbound channel must be zeroed
    assert(sharedMem.outboundSignal.signalSeq.load()   == 0);
    assert(sharedMem.outboundSignal.targetId           == 0xFFFFFFFFu);
    assert(sharedMem.outboundSignal.signalName[0]      == '\0');
    assert(sharedMem.outboundSignal.signalValue[0]     == '\0');

    // Version bump: we must be at ABI version 4
    assert(sharedMem.version == 4u);

    std::cout << "test_outbound_initial_state passed" << std::endl;
}

int main() {
    test_telemetry_sync();
    test_inbound_commands();
    test_publish_signal();
    test_outbound_initial_state();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
