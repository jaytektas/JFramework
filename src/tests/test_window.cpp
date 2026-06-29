#include <j/core/Window.h>
#include <j/core/SceneGraph.h>
#include <j/core/StyleEngine.h>
#include <j/graphics/RenderPrimitive.h>
#include <j/platforms/linux/LinuxPlatformWindow.h>
#include <cassert>
#include <iostream>
#include <xcb/xcb.h>

using namespace jf;

void test_hello_world_window() {
    JSceneGraph graph;
    JStyleEngine styles(graph);
    JPrimitiveBuffer buffer;
    
    // 1. Create a "Hello World" JWindow
    JWindow win(graph, "Hello Genesis");
    NodeId winId = win.getNodeId();
    
    // 2. Setup initial layout constraints (JWindow size)
    auto& layout = graph.getLayout(winId);
    layout.boundingBox = { 100, 100, 400, 300 }; // Pos X:100, Y:100, Dim 400x300
    
    // 3. Apply a custom style override (Blue Title Bar)
    JColor customTitle = { 0, 120, 215, 255 }; // Modern UI Blue
    styles.setLocal(winId, WindowStyle::TitleBarColor, customTitle);
    
    // 4. Perform Layout Pass
    // For this isolated test, we skip the parent-constraint pass because we defined bounds manually
    // graph.computeLayout(winId, constraints);
    
    // 5. Render the JWindow
    win.renderWithStyles(buffer, styles);
    
    // 6. Validation
    const auto& cmds = buffer.getCommands();

    // Count rect commands — expect at least 2 (body + title bar)
    int rectCount = 0;
    bool foundBlue = false;
    for (const auto& cmd : cmds) {
        if (cmd.kind != JPrimitiveBuffer::JDrawCommand::JKind::JRect) continue;
        ++rectCount;
        if (cmd.rect.color[0] == 0 && cmd.rect.color[1] == 120 && cmd.rect.color[2] == 215)
            foundBlue = true;
    }
    assert(rectCount >= 2);

    // Verify our custom blue color reached the instance buffer for the title bar
    assert(foundBlue);
    
    std::cout << "[GENESIS] Hello World JWindow Logical Bootstrapping: PASSED" << std::endl;
    std::cout << "[GENESIS] JWindow bounds: " << layout.boundingBox.width << "x" << layout.boundingBox.height << std::endl;
}

void test_window_transient_layering() {
    // 1. Create a parent window
    JLinuxPlatformWindow parentWin("Parent JWindow", 400, 300);
    xcb_window_t parentId = parentWin.nativeWindow();
    xcb_connection_t* conn = parentWin.nativeConnection();
    assert(parentId != 0);

    // 2. Create a child borderless window with parentId as transient parent
    JLinuxPlatformWindow childWin("Child JWindow", 200, 150, 150, 150, JPlatformWindowStyle::Borderless, parentId);
    xcb_window_t childId = childWin.nativeWindow();
    assert(childId != 0);

    // 3. Query WM_TRANSIENT_FOR property of child window
    auto transient_cookie = xcb_get_property(conn, 0, childId, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 0, 1);
    auto* transient_reply = xcb_get_property_reply(conn, transient_cookie, nullptr);
    assert(transient_reply != nullptr);
    assert(transient_reply->value_len == 1);
    xcb_window_t queried_parent = *static_cast<xcb_window_t*>(xcb_get_property_value(transient_reply));
    assert(queried_parent == parentId);
    free(transient_reply);

    // 4. Query _NET_WM_WINDOW_TYPE property of child window
    xcb_intern_atom_cookie_t type_cookie = xcb_intern_atom(conn, 0, 19, "_NET_WM_WINDOW_TYPE");
    auto* type_reply = xcb_intern_atom_reply(conn, type_cookie, nullptr);
    assert(type_reply != nullptr);
    xcb_atom_t type_atom = type_reply->atom;
    free(type_reply);

    auto prop_cookie = xcb_get_property(conn, 0, childId, type_atom, XCB_ATOM_ANY, 0, 100);
    auto* prop_reply = xcb_get_property_reply(conn, prop_cookie, nullptr);
    assert(prop_reply != nullptr);
    assert(prop_reply->value_len == 1);
    xcb_atom_t queried_type = *static_cast<xcb_atom_t*>(xcb_get_property_value(prop_reply));
    free(prop_reply);

    // Get the _NET_WM_WINDOW_TYPE_NORMAL atom to verify
    xcb_intern_atom_cookie_t normal_cookie = xcb_intern_atom(conn, 0, 26, "_NET_WM_WINDOW_TYPE_NORMAL");
    auto* normal_reply = xcb_intern_atom_reply(conn, normal_cookie, nullptr);
    assert(normal_reply != nullptr);
    xcb_atom_t normal_atom = normal_reply->atom;
    free(normal_reply);

    assert(queried_type == normal_atom);

    std::cout << "[GENESIS] JWindow Transient Layering Hint Verification: PASSED" << std::endl;
}

int main() {
    test_hello_world_window();
    test_window_transient_layering();
    return 0;
}
