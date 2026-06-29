// dock_window_styles — tests three window decoration strategies and
// multi-monitor positioning.  Run it and inspect the screenshots to see
// which approach gives borderless floating-dock windows on your setup.
//
// Screenshots written to /tmp/:
//   genesis_normal.ppm       — Normal (WM-decorated)
//   genesis_borderless.ppm   — Borderless via _MOTIF_WM_HINTS
//   genesis_popup.ppm        — Popup via override_redirect
//   genesis_moved.ppm        — Popup moved to secondary monitor coords
//   genesis_fullscreen.ppm   — Borderless after _NET_WM_STATE_FULLSCREEN

#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>
#include <j/graphics/FontEngine.h>
#if defined(_WIN32)
#include <j/platforms/windows/WindowsPlatformWindow.h>
#else
#include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <iostream>
#include <thread>
#include <chrono>

using namespace jf;
using namespace std::chrono_literals;

// ---- tiny helpers ----------------------------------------------------------

static void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

#if defined(_WIN32)
using PlatformWindowImpl = JWindowsPlatformWindow;
#else
using PlatformWindowImpl = JLinuxPlatformWindow;
#endif

struct TestWindow {
    std::unique_ptr<PlatformWindowImpl> win;
    GpuSurfaceId                         surface{kPrimarySurface};
    std::string                          label;
};

static TestWindow makeWindow(const std::string& title,
                             int x, int y, uint32_t w, uint32_t h,
                             JPlatformWindowStyle style, JGpuHal& hal)
{
    TestWindow tw;
    tw.label = title;
    tw.win   = std::make_unique<PlatformWindowImpl>(title, w, h, x, y, style);
    tw.surface = hal.createSurface(tw.win->nativeHandle(), w, h);
    return tw;
}

// Render a solid-color frame and return.
static void renderSolid(TestWindow& tw, JGpuHal& hal, JPrimitiveBuffer& buf,
                        uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t col[4] = {r, g, b, 255};
    buf.clear();
    buf.pushRectangle(0, 0, 2000, 2000, col, 0.f);
    auto frame = hal.beginFrame(tw.surface);
    hal.drawPrimitives(buf);
    hal.submitAndPresentFrame(frame);
}

// Poll events and render N frames.  If capturePath given, captures on the
// second-to-last frame so the following frame's beginFrame flushes the write.
static void runFrames(TestWindow& tw, JGpuHal& hal, JPrimitiveBuffer& buf,
                      uint8_t r, uint8_t g, uint8_t b, int frames,
                      const char* capturePath = nullptr)
{
    int captureOn = capturePath ? frames - 2 : -1;
    for (int i = 0; i < frames; ++i) {
        tw.win->pollNativeEvents();
        if (i == captureOn)
            hal.captureNextFrame(capturePath, tw.surface);
        renderSolid(tw, hal, buf, r, g, b);
        sleepMs(16);
    }
}

// ---- main ------------------------------------------------------------------

int main()
{
    std::cout << "[dock_window_styles] Starting...\n";

    // Use lavapipe for headless-capable Vulkan.
    auto primaryWin = std::make_unique<PlatformWindowImpl>(
        "primary", 1, 1, -1000, -1000, JPlatformWindowStyle::Popup);

    JNativeWindowHandle handle = primaryWin->nativeHandle();

    auto hal = JGpuHal::create(JGpuApiType::Vulkan, handle);
    if (!hal) { std::cerr << "Failed to create Vulkan HAL\n"; return 1; }
    hal->resizeSwapchain(1, 1);

    JPrimitiveBuffer buf;

    // ---- Query virtual desktop ----
    auto [deskW, deskH] = primaryWin->virtualDesktopSize();
    std::cout << "[dock_window_styles] Virtual desktop: " << deskW << " x " << deskH << "\n";
    bool hasSecondMonitor = (deskW > 1920 || deskH > 1080);
    std::cout << "[dock_window_styles] Second monitor detected: "
              << (hasSecondMonitor ? "YES" : "NO (single display or Xvfb)\n");
    if (hasSecondMonitor) {
        std::cout << "[dock_window_styles] Secondary monitor region starts at x="
                  << 1920 << " (assumed 1920-wide primary)\n";
    }

    // ---- TEST 1: Normal decorated window (blue) ----
    std::cout << "\n[TEST 1] Normal decorated window...\n";
    {
        auto tw = makeWindow("Normal JWindow", 50, 50, 400, 300,
                             JPlatformWindowStyle::Normal, *hal);
        runFrames(tw, *hal, buf, 30, 80, 200, 35, "/tmp/genesis_normal.ppm");
        hal->destroySurface(tw.surface);
        std::cout << "[TEST 1] Screenshot: /tmp/genesis_normal.ppm\n";
    }
    sleepMs(200);

    // ---- TEST 2: Borderless via _MOTIF_WM_HINTS (green) ----
    std::cout << "\n[TEST 2] Borderless (_MOTIF_WM_HINTS)...\n";
    {
        auto tw = makeWindow("Borderless (MOTIF)", 100, 100, 400, 300,
                             JPlatformWindowStyle::Borderless, *hal);
        runFrames(tw, *hal, buf, 40, 180, 80, 35, "/tmp/genesis_borderless.ppm");
        hal->destroySurface(tw.surface);
        std::cout << "[TEST 2] Screenshot: /tmp/genesis_borderless.ppm\n";
    }
    sleepMs(200);

    // ---- TEST 3: Popup via override_redirect (red) ----
    std::cout << "\n[TEST 3] Popup (override_redirect)...\n";
    {
        auto tw = makeWindow("Popup", 150, 150, 400, 300,
                             JPlatformWindowStyle::Popup, *hal);
        runFrames(tw, *hal, buf, 200, 50, 50, 35, "/tmp/genesis_popup.ppm");
        hal->destroySurface(tw.surface);
        std::cout << "[TEST 3] Screenshot: /tmp/genesis_popup.ppm\n";
    }
    sleepMs(200);

    // ---- TEST 4: setPosition — move Popup to secondary monitor coords ----
    std::cout << "\n[TEST 4] setPosition (Popup → secondary monitor coords)...\n";
    {
        auto tw = makeWindow("Popup on Second Monitor", 200, 200, 600, 400,
                             JPlatformWindowStyle::Popup, *hal);
        runFrames(tw, *hal, buf, 180, 100, 220, 15);

        int secondX = 1920 + 200;  // beyond typical 1080p primary
        int secondY = 200;
        std::cout << "[TEST 4] Moving window to (" << secondX << ", " << secondY << ")...\n";
        tw.win->setPosition(secondX, secondY);
        runFrames(tw, *hal, buf, 180, 100, 220, 25, "/tmp/genesis_moved.ppm");
        std::cout << "[TEST 4] JWindow reported screenX after move: "
                  << tw.win->screenX() << "\n";
        hal->destroySurface(tw.surface);
        std::cout << "[TEST 4] Screenshot: /tmp/genesis_moved.ppm\n";
    }
    sleepMs(200);

    // ---- TEST 5: setFullscreen on Borderless (yellow) ----
    std::cout << "\n[TEST 5] setFullscreen on Borderless window...\n";
    {
        auto tw = makeWindow("Fullscreen Borderless", 300, 200, 800, 600,
                             JPlatformWindowStyle::Borderless, *hal);
        runFrames(tw, *hal, buf, 220, 200, 30, 20);
        std::cout << "[TEST 5] Requesting fullscreen...\n";
        tw.win->setFullscreen(true);
        runFrames(tw, *hal, buf, 220, 200, 30, 50, "/tmp/genesis_fullscreen.ppm");
        tw.win->setFullscreen(false);
        runFrames(tw, *hal, buf, 220, 200, 30, 10);
        hal->destroySurface(tw.surface);
        std::cout << "[TEST 5] Screenshot: /tmp/genesis_fullscreen.ppm\n";
    }

    // ---- TEST 6: Borderless → secondary monitor → fullscreen ----
    if (hasSecondMonitor) {
        std::cout << "\n[TEST 6] Borderless → move to monitor 2 → fullscreen...\n";
        auto tw = makeWindow("Dock on Monitor 2", 100, 100, 600, 400,
                             JPlatformWindowStyle::Borderless, *hal);
        runFrames(tw, *hal, buf, 30, 200, 180, 20);
        tw.win->setPosition(1920 + 100, 100);
        runFrames(tw, *hal, buf, 30, 200, 180, 30);
        tw.win->setFullscreen(true);
        runFrames(tw, *hal, buf, 30, 200, 180, 70, "/tmp/genesis_monitor2_fullscreen.ppm");
        tw.win->setFullscreen(false);
        runFrames(tw, *hal, buf, 30, 200, 180, 10);
        hal->destroySurface(tw.surface);
        std::cout << "[TEST 6] Screenshot: /tmp/genesis_monitor2_fullscreen.ppm\n";
    } else {
        std::cout << "\n[TEST 6] Skipped (no second monitor detected on this display)\n";
    }

    hal->waitIdle();
    std::cout << "\n[dock_window_styles] Done.  Check screenshots in /tmp/\n";
    return 0;
}
