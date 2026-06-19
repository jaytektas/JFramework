#include <genesis/core/AiControlBus.h>
#include <genesis/core/SceneGraph.h>
#include <cassert>
#include <iostream>

using namespace Genesis;

void test_telemetry_sync() {
    SharedBusMemory sharedMem;
    AiControlBus bus;
    bus.attach(&sharedMem);
    
    SceneGraph graph;
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
    AiControlBus bus;
    bus.attach(&sharedMem);
    
    AiVirtualInput cmd;
    
    // No command initially
    assert(bus.pollInboundCommand(cmd) == false);
    
    // AI injects a command
    sharedMem.inboundCommand.type.store(AiVirtualInput::CommandType::MouseClick);
    sharedMem.inboundCommand.targetX = 150.0f;
    sharedMem.inboundCommand.targetY = 200.0f;
    sharedMem.inboundCommand.sequenceId.store(1, std::memory_order_release);
    
    assert(bus.pollInboundCommand(cmd) == true);
    assert(cmd.type.load() == AiVirtualInput::CommandType::MouseClick);
    assert(cmd.targetX == 150.0f);
    
    // Command already processed
    assert(bus.pollInboundCommand(cmd) == false);
    
    // New command
    sharedMem.inboundCommand.sequenceId.store(2, std::memory_order_release);
    assert(bus.pollInboundCommand(cmd) == true);
    
    std::cout << "test_inbound_commands passed" << std::endl;
}

int main() {
    test_telemetry_sync();
    test_inbound_commands();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
