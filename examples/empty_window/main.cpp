// Minimal empty Genesis window — for testing window-management behavior
// (drag, snap/tile, maximize/restore, minimize, edge-resize) in isolation,
// without any dock content inflating the minimum size.
//
// This used to hand-roll the whole event loop + custom title bar (~240 lines). All of
// that is window-management plumbing the framework now owns: JAppWindow::run() draws the
// title bar / window controls and delegates drag, snap/tile and edge-resize to the WM.
// With no docks declared, the minimum size stays small so the WM can freely tile it.
#include <j/app/JAppWindow.h>
#include <j/core/GenesisComponents.h>

#include <iostream>

using namespace jf;

int main() {
    std::cout << "[GENESIS] Empty JWindow test starting...\n";

    JGuiApplication app;
    JAppWindow win("Genesis Empty JWindow", 600, 400);
    if (!win.valid()) { std::cerr << "[GENESIS] window/HAL init failed\n"; return -1; }

    int rc = win.run();   // runner owns the loop, chrome and all window management

    std::cout << "[GENESIS] Empty JWindow test exiting.\n";
    return rc;
}
