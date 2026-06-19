#include <genesis/core/Window.h>
#include <genesis/core/SceneGraph.h>
#include <genesis/core/StyleEngine.h>
#include <genesis/graphics/RenderPrimitive.h>
#include <cassert>
#include <iostream>

using namespace Genesis;

void test_hello_world_window() {
    SceneGraph graph;
    StyleEngine styles(graph);
    PrimitiveBuffer buffer;
    
    // 1. Create a "Hello World" Window
    Window win(graph, "Hello Genesis");
    NodeId winId = win.getNodeId();
    
    // 2. Setup initial layout constraints (Window size)
    auto& layout = graph.getLayout(winId);
    layout.boundingBox = { 100, 100, 400, 300 }; // Pos X:100, Y:100, Dim 400x300
    
    // 3. Apply a custom style override (Blue Title Bar)
    Color customTitle = { 0, 120, 215, 255 }; // Modern UI Blue
    styles.setLocal(winId, WindowStyle::TitleBarColor, customTitle);
    
    // 4. Perform Layout Pass
    // For this isolated test, we skip the parent-constraint pass because we defined bounds manually
    // graph.computeLayout(winId, constraints);
    
    // 5. Render the Window
    win.renderWithStyles(buffer, styles);
    
    // 6. Validation
    const auto& cmds = buffer.getCommands();

    // Count rect commands — expect at least 2 (body + title bar)
    int rectCount = 0;
    bool foundBlue = false;
    for (const auto& cmd : cmds) {
        if (cmd.kind != PrimitiveBuffer::DrawCommand::Kind::Rect) continue;
        ++rectCount;
        if (cmd.rect.color[0] == 0 && cmd.rect.color[1] == 120 && cmd.rect.color[2] == 215)
            foundBlue = true;
    }
    assert(rectCount >= 2);

    // Verify our custom blue color reached the instance buffer for the title bar
    assert(foundBlue);
    
    std::cout << "[GENESIS] Hello World Window Logical Bootstrapping: PASSED" << std::endl;
    std::cout << "[GENESIS] Window bounds: " << layout.boundingBox.width << "x" << layout.boundingBox.height << std::endl;
}

int main() {
    test_hello_world_window();
    return 0;
}
