#include <j/core/ApplicationCore.h>
#include <j/core/GenesisComponents.h>
#include <j/core/Log.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

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

using namespace jf;

// Simple internal helper to avoid including concrete platform headers here if they are not exposed
#if defined(__linux__)
inline namespace jf {
    // Forward declaration or local re-definition if needed for the host
    class JLinuxPlatformWindow;
}
#endif

int main() {
    JLOGC("Platform", JLogLevel::Info) << "Bootstrapping Native Genesis Host...";

    jf::JGuiApplication app;
    jf::JMainWindow mainWindow("Genesis Host");
    mainWindow.show();

    JLOGC("Platform", JLogLevel::Info) << "Host initialized successfully. Platform-agnostic core is ready.";
    JLOGC("Platform", JLogLevel::Info) << "Use 'genesis_controls_catalog' for the visual widget showcase.";
    
    return 0;
}
