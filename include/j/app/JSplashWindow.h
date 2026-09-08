#pragma once

// JSplashWindow — the mark on screen while an application is getting up.
//
// A window maps long before it is worth looking at. The studio's is on screen in half a second and
// painted at one and a half, so there is a second of empty frame in between: the application looks
// broken at precisely the moment a first impression is formed. A splash fills it with the thing the
// user is waiting for, and says which program they started.
//
// OVERRIDE-REDIRECT, so it is not the window manager's business. A splash has no title bar to drag, no
// entry in a task list, no place in the alt-tab order, and it must not be stacked under the window it
// is covering for — all of which the WM would decide otherwise. It is centred on the root screen and
// then it goes away.
//
// RAW RGBA, drawn at its own size, exactly as JDialogRequest::imageRgba is: the caller decodes, this
// blits. An application that ships a PNG decodes it once at startup; one that embeds its logo as bytes
// hands them straight over. Neither has to be a decision made here.
//
//   JSplashWindow splash(hal, rgba, w, h);
//   splash.present();          // one frame, on screen now
//   …build the application…
//   splash.close(hal);         // or let it destruct
//
// The caller owns the hal and must close (or destroy) the splash before that hal goes.

#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <cstdint>
#include <memory>
#include <utility>

inline namespace jf {

class JSplashWindow {
public:
#if defined(_WIN32)
    using PlatformWinType = JWindowsPlatformWindow;
#else
    using PlatformWinType = JLinuxPlatformWindow;
#endif

    // An empty or mismatched image builds nothing: an application that cannot find its logo starts
    // without a splash rather than with a blank rectangle in the middle of the screen.
    JSplashWindow(JGpuHal& hal, const uint8_t* rgba, uint32_t w, uint32_t h) {
        if (!rgba || w == 0 || h == 0) return;
        // CENTRED ON THE ROOT SCREEN, not on the application window: the application window may not
        // exist yet, and when it does it is usually centred on the same screen anyway. The window is
        // made first and moved second because the screen size is a thing a window can be asked, and
        // there is nothing to see in between — an override-redirect window shows whatever was last
        // presented into it, and nothing is presented until present().
        m_window = std::make_unique<PlatformWinType>("", w, h, 0, 0, JPlatformWindowStyle::Popup);
        const auto [sw, sh] = m_window->screenSize();
        m_window->setPosition((sw - static_cast<int>(w)) / 2, (sh - static_cast<int>(h)) / 2);
        m_surface = hal.createSurface(m_window->nativeHandle(), w, h);
        m_tex     = hal.uploadTexture(rgba, w, h);
        m_w = w; m_h = h;
    }

    ~JSplashWindow() = default;   // close(hal) does the work; see the note on hal lifetime above

    bool valid() const { return m_window && m_tex != kNullTexture; }

    // One frame. Called again it simply redraws, which is what an application with a long start wants
    // to do between steps so the compositor keeps the window fresh.
    void present(JGpuHal& hal) {
        if (!valid()) return;
        m_window->pollNativeEvents();
        JPrimitiveBuffer buf;
        buf.clear();
        buf.pushImage(0.f, 0.f, static_cast<float>(m_w), static_cast<float>(m_h), m_tex);
        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
    }

    // A SPLASH NOBODY SEES IS NOT A SPLASH. Closing it the moment the application is ready sounds
    // right and is not: a fast start puts the whole thing on screen for a few hundred milliseconds, so
    // it reads as a flicker if it registers at all. minVisibleMs holds it for the rest of its welcome
    // and re-presents while it waits, so a compositor that wants a fresh frame gets one.
    //
    // The wait is the REMAINDER, not a delay: an application that took longer than this to start has
    // already shown the splash for longer and closes immediately. Slow starts pay nothing.
    // A CLICK ANYWHERE, and the application running behind it meanwhile.
    //
    // The splash does NOT hold the application shut. It is polled from the frame loop: the window it
    // covers is up and drawing normally, and this sits in front of it until the reader dismisses it —
    // no delay to sit through and nothing frozen underneath.
    //
    // ANYWHERE means the pointer is grabbed. Without it a click only counts where the splash happens to
    // be, and "click to dismiss" that works on a quarter of the screen is worse than none; with it, the
    // first press lands here wherever it was aimed, and does NOT leak through to whatever it was over —
    // which is the behaviour you want, since the reader was dismissing a splash, not pressing a button
    // they could not see.
    //
    // Returns false once it has been dismissed, so the caller can drop it.
    bool poll(JGpuHal& hal) {
        if (!valid()) return false;
        _ensureGrab();
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) return false;
        if (m_window->consumePress() || m_window->consumeRelease()) return false;
        for (const auto& ke : m_window->consumeAllKeys()) if (ke.pressed) return false;
        present(hal);
        return true;
    }

    void close(JGpuHal& hal) {
        _releaseGrab();
        if (m_tex != kNullTexture) { hal.releaseTexture(m_tex); m_tex = kNullTexture; }
        if (m_window) { hal.destroySurface(m_surface); m_window.reset(); }
    }

private:
#if !defined(_WIN32)
    void _ensureGrab() {
        if (m_grabbed || !m_window) return;
        m_grabbed = true;                    // once, whether or not the server gives it to us
        xcb_connection_t* c = m_window->nativeConnection();
        auto ck = xcb_grab_pointer(c, 0, m_window->nativeWindow(),
                                   XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
                                   XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                                   XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(c);
        free(xcb_grab_pointer_reply(c, ck, nullptr));
    }
    void _releaseGrab() {
        if (!m_grabbed || !m_window) return;
        xcb_ungrab_pointer(m_window->nativeConnection(), XCB_CURRENT_TIME);
        xcb_flush(m_window->nativeConnection());
        m_grabbed = false;
    }
#else
    void _ensureGrab()  { if (!m_grabbed && m_window) { SetCapture(m_window->nativeWindow()); m_grabbed = true; } }
    void _releaseGrab() { if (m_grabbed) { ReleaseCapture(); m_grabbed = false; } }
#endif
    bool m_grabbed{ false };

    std::unique_ptr<PlatformWinType> m_window;
    GpuSurfaceId  m_surface{ 0 };
    TextureHandle m_tex{ kNullTexture };
    uint32_t      m_w{ 0 }, m_h{ 0 };
};

}  // inline namespace jf
