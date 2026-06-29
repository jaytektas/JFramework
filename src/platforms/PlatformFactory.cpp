#include <j/platforms/PlatformWindow.h>

#if defined(_WIN32)
#  include <j/platforms/windows/WindowsPlatformWindow.h>
#elif defined(__linux__)
#  include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

inline namespace jf {

std::unique_ptr<JPlatformWindow> createPlatformWindow(
    const std::string& title, uint32_t width, uint32_t height,
    int x, int y, JPlatformWindowStyle style) {
#if defined(_WIN32)
    return std::make_unique<JWindowsPlatformWindow>(title, width, height, x, y, style);
#elif defined(__linux__)
    return std::make_unique<JLinuxPlatformWindow>(title, width, height, x, y, style);
#else
    (void)title; (void)width; (void)height; (void)x; (void)y; (void)style;
    return nullptr;
#endif
}

}  // namespace jf
