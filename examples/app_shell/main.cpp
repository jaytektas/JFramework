// Proof of the JAppWindow runner: a complete windowed app with NO Vulkan/window/font/
// loop boilerplate. Compare against examples/controls_catalog/main.cpp, which hand-rolls
// ~900 lines of the same plumbing.
//
// Also demonstrates the dock space: the runner frames a protected centre with Left /
// Right / Bottom areas. We only declare which docks live where — the runner lays the
// areas out, renders them, routes input, and handles tab tear-out / re-dock across areas.

#include <j/app/JAppWindow.h>
#include <j/core/GenesisComponents.h>
#include <j/core/BaseWidgets.h>

#include <iostream>
#include <string>

using namespace jf;

int main() {
    JGuiApplication app;
    JAppWindow win("JFramework App Shell", 800, 480);
    if (!win.valid()) { std::cerr << "window/HAL init failed\n"; return -1; }

    // --- Dock layout: side/bottom panels framing a protected centre --------------
    auto& space = win.dockSpace();
    JDockWidget explorer  ("Explorer",   0.f, 0.f, 220.f, 160.f);
    JDockWidget outline   ("Outline",    0.f, 0.f, 220.f, 160.f);
    JDockWidget properties("Properties", 0.f, 0.f, 220.f, 160.f);
    JDockWidget output    ("Output",     0.f, 0.f, 220.f, 140.f);
    space.left().addDock(&explorer);
    space.left().addDock(&outline);      // tabbed with Explorer in the left area
    space.right().addDock(&properties);
    space.bottom().addDock(&output);

    // --- A live widget in the protected centre -----------------------------------
    // Repositioned each frame to the centre rect, proving app content and the dock
    // space coexist under one runner (the areas resize; the content follows).
    auto& g = app.sceneGraph();
    JLabel  title(g, "JFramework app-runner — dock space + centre content, zero boilerplate", 600, 24);
    JButton button(g, "Click me");
    JLabel  status(g, "clicks: 0", 600, 22);

    int clicks = 0;
    button.onClicked.connect([&]{ status.setText("clicks: " + std::to_string(++clicks)); });

    auto placeCentre = [&] {
        const JRect& c = space.rect(JDockSpace::Center);
        g.getLayout(title.getNodeId()).boundingBox  = {c.x + 20.f, c.y + 16.f, c.width - 40.f, 24.f};
        g.getLayout(button.getNodeId()).boundingBox = {c.x + 20.f, c.y + 56.f, 160.f, 36.f};
        g.getLayout(status.getNodeId()).boundingBox = {c.x + 20.f, c.y + 104.f, c.width - 40.f, 22.f};
    };

    win.onRender = [&](JPrimitiveBuffer& buf) {
        placeCentre();
        title.populateRenderPrimitives(buf);
        button.populateRenderPrimitives(buf);
        status.populateRenderPrimitives(buf);
    };
    win.onInput = [&](float mx, float my, bool pressed, bool released) {
        placeCentre();
        button.handleMouseMove(mx, my);
        if (pressed)  button.handleMousePress(mx, my);
        if (released) button.handleMouseRelease(mx, my);
    };

    std::cout << "[app_shell] running on the JAppWindow runner (dock space + centre content)\n";
    return win.run();
}
