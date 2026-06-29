#include <genesis/core/ApplicationCore.h>
#include <genesis/core/GenesisComponents.h>
#include <genesis/graphics/GpuHal.h>
#include <genesis/graphics/RenderPrimitive.h>

#include <iostream>
#include <memory>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#elif defined(__linux__)
#define VK_USE_PLATFORM_XCB_KHR
#include <xcb/xcb.h>
#endif

using namespace Genesis;

// Simple internal helper to avoid including concrete platform headers here if they are not exposed
#if defined(__linux__)
namespace Genesis {
    // Forward declaration or local re-definition if needed for the host
    class JLinuxPlatformWindow;
}
#endif

int main() {
    std::cout << "[GENESIS] Bootstrapping Native Genesis Host...\n";

    Genesis::JGuiApplication app;
    Genesis::JMainWindow mainWindow("Genesis Host");
    mainWindow.show();

    std::cout << "[GENESIS] Host initialized successfully. Platform-agnostic core is ready.\n";
    std::cout << "[GENESIS] Use 'genesis_controls_catalog' for the visual widget showcase.\n";
    
    return 0;
}
