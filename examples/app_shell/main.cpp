// Proof of the JAppWindow runner: a complete windowed app with NO Vulkan/window/font/
// loop boilerplate. Compare against examples/controls_catalog/main.cpp, which hand-rolls
// ~900 lines of the same plumbing.

#include <j/app/JAppWindow.h>
#include <j/core/GenesisComponents.h>
#include <j/core/BaseWidgets.h>

#include <iostream>
#include <string>

using namespace jf;

int main() {
    JGuiApplication app;
    JAppWindow win("JFramework App Shell", 640, 360);
    if (!win.valid()) { std::cerr << "window/HAL init failed\n"; return -1; }

    auto& g = app.sceneGraph();
    JLabel  title(g, "JFramework app-runner — a window in ~40 lines, zero Vulkan boilerplate", 600, 24);
    JButton button(g, "Click me");
    JLabel  status(g, "clicks: 0", 600, 22);

    g.getLayout(title.getNodeId()).boundingBox  = {20.f, 20.f, 600.f, 24.f};
    g.getLayout(button.getNodeId()).boundingBox = {20.f, 70.f, 160.f, 36.f};
    g.getLayout(status.getNodeId()).boundingBox = {20.f, 120.f, 600.f, 22.f};

    int clicks = 0;
    button.onClicked.connect([&]{ status.setText("clicks: " + std::to_string(++clicks)); });

    win.onRender = [&](JPrimitiveBuffer& buf) {
        title.populateRenderPrimitives(buf);
        button.populateRenderPrimitives(buf);
        status.populateRenderPrimitives(buf);
    };
    win.onInput = [&] {
        float mx = win.window().mouseX(), my = win.window().mouseY();
        button.handleMouseMove(mx, my);
        if (win.window().consumePress())   button.handleMousePress(mx, my);
        if (win.window().consumeRelease()) button.handleMouseRelease(mx, my);
    };

    std::cout << "[app_shell] running on the JAppWindow runner\n";
    return win.run();
}
