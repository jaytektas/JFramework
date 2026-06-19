#pragma once

#include <genesis/core/BaseWidgets.h>
#include <genesis/core/DockWidget.h>
#include <genesis/core/DockManager.h>
#include <genesis/core/DockRegistry.h>
#include <genesis/graphics/GpuHal.h>
#include <genesis/graphics/RenderPrimitive.h>
#include <genesis/platforms/linux/LinuxPlatformWindow.h>

#include <memory>
#include <functional>

namespace Genesis {

// ============================================================================
// FloatingDockWindow — a DockWidget that lives in its own OS-level Popup
// window and can be dragged anywhere on the desktop, including over and into
// any DockHost registered with DockRegistry.
//
// There are three internal states:
//
//   InitialDrag — created while the mouse button is still held from the drag
//                 that spawned it (grab is on the originating window).  The OS
//                 window is invisible / unmoved; the caller draws a ghost in
//                 the main window.  On button-up we snap the OS window to the
//                 cursor and transition to Idle (or immediately drop).
//
//   Idle        — fully placed and rendered.  The user can interact with the
//                 dock's chrome (close, pin, resize) or grab the title bar to
//                 start a HeaderDrag.
//
//   HeaderDrag  — user grabbed the title bar; pointer is now grabbed by THIS
//                 window.  We move the OS window every frame to follow the
//                 cursor and drive updateDrag() on whichever registered
//                 DockHost is under the cursor.
//
// Caller pattern per frame:
//
//   DockHost* drop = fd.pollAndMove();          // step 1
//   if (drop && drop->tryCommitDrop()) {        // step 2: re-dock
//       fd.destroySurface(hal);
//       // move dock into the host's ownership
//   } else if (!fd.isInInitialDrag()) {         // step 3
//       fd.render(hal, buf);
//   }
// ============================================================================

class FloatingDockWindow {
public:
    static constexpr uint32_t kDefaultW = 340;
    static constexpr uint32_t kDefaultH = 260;

    // Construct from a DockWidget extracted from a DockHost (WantsFloat path).
    // screenX/Y: top-left of the OS window in screen coords.
    // winW/H:    pixel size of the OS window.
    // dragOffX/Y: cursor position within the window at creation time.
    // initialDrag: pass true when the button is still held — keeps the window
    //             invisible until button-up then snaps it under the cursor.
    FloatingDockWindow(DockWidget dock,
                       int screenX, int screenY,
                       uint32_t winW, uint32_t winH,
                       int dragOffX, int dragOffY,
                       GpuHal& hal,
                       bool initialDrag = false)
        : m_window(std::make_unique<LinuxPlatformWindow>(
              dock.title().c_str(), winW, winH, screenX, screenY, WindowStyle::Popup))
        , m_surface(hal.createSurface(m_window->nativeHandle(), winW, winH))
        , m_dock(std::move(dock))
        , m_winW(winW), m_winH(winH)
        , m_state(initialDrag ? State::InitialDrag : State::Idle)
        , m_dragOffX(dragOffX)
        , m_dragOffY(dragOffY)
    {
        m_dock.setPosition(0.f, 0.f);
        m_dock.setSize(static_cast<float>(winW), static_cast<float>(winH));
    }

    // Construct from a TornTabState (TabBar tear-off path).
    FloatingDockWindow(TornTabState state,
                       int screenX, int screenY,
                       int dragOffX, int dragOffY,
                       GpuHal& hal)
        : FloatingDockWindow(
              DockWidget(std::move(state), 0.f, 0.f,
                         static_cast<float>(kDefaultW), static_cast<float>(kDefaultH)),
              screenX, screenY, kDefaultW, kDefaultH,
              dragOffX, dragOffY, hal, /*initialDrag=*/true)
    {}

    FloatingDockWindow(const FloatingDockWindow&)            = delete;
    FloatingDockWindow& operator=(const FloatingDockWindow&) = delete;
    FloatingDockWindow(FloatingDockWindow&&)                 = default;
    FloatingDockWindow& operator=(FloatingDockWindow&&)      = default;

    // -------------------------------------------------------------------------
    // Per-frame update.
    //
    // Polls OS events, moves the OS window when dragging, and drives
    // updateDrag() on registered DockHosts so drop indicators appear.
    //
    // Returns the DockHost under the cursor the moment the drag ends (button
    // released while over it) — the caller should then call tryCommitDrop() on
    // that host.  Returns nullptr in all other cases.
    // -------------------------------------------------------------------------
    DockHost* pollAndMove() {
        m_window->pollNativeEvents();

        const bool btnDown = m_window->isLeftButtonDown();
        const bool press   = m_window->consumePress();
        const bool release = m_window->consumeRelease();
        (void)release;  // used implicitly through btnDown transitions

        switch (m_state) {
        case State::InitialDrag: {
            // Pointer grab is on the originating window so normal events don't
            // arrive here — poll global state via xcb_query_pointer.
            auto [gx, gy] = m_window->globalCursorPos();

            // Follow the cursor every frame so the window is always visible,
            // even outside the bounds of the window that owns the grab.
            m_window->setPosition(gx - m_dragOffX, gy - m_dragOffY);

            // Drive drop indicators on whatever host is under the cursor.
            DockHost* hoveredHost = nullptr;
            if (auto hit = DockRegistry::instance().hitTest(gx, gy)) {
                hoveredHost = hit->host;
                hoveredHost->updateDrag(
                    hit->localX, hit->localY,
                    static_cast<float>(gx), static_cast<float>(gy),
                    gx - static_cast<int>(hit->localX),
                    gy - static_cast<int>(hit->localY),
                    &m_dock);
            } else {
                _clearAllHostDrags();
            }

            if (!btnDown) {
                m_state = State::Idle;
                if (hoveredHost) {
                    _clearHostDragsExcept(hoveredHost);
                    return hoveredHost;
                }
                _clearAllHostDrags();
            }
            return nullptr;
        }

        case State::HeaderDrag: {
            auto [gx, gy] = m_window->globalCursorPos();

            // Move OS window to follow the cursor.
            m_window->setPosition(gx - m_dragOffX, gy - m_dragOffY);

            // Drive drop indicators.
            DockHost* hoveredHost = nullptr;
            if (auto hit = DockRegistry::instance().hitTest(gx, gy)) {
                hoveredHost = hit->host;
                hoveredHost->updateDrag(
                    hit->localX, hit->localY,
                    static_cast<float>(gx), static_cast<float>(gy),
                    gx - static_cast<int>(hit->localX),
                    gy - static_cast<int>(hit->localY),
                    &m_dock);
            } else {
                _clearAllHostDrags();
            }

            if (!btnDown) {
                m_state = State::Idle;
                if (hoveredHost) {
                    // Leave hoveredHost's drag state intact so the caller's
                    // tryCommitDrop() can read m_activeTarget.  Clear all others.
                    _clearHostDragsExcept(hoveredHost);
                    return hoveredHost;
                }
                _clearAllHostDrags();
            }
            return nullptr;
        }

        case State::Idle:
            // Detect title-bar grab → HeaderDrag.
            // The title bar is the top TAB_BAR_SZ logical pixels.
            if (press && m_window->mouseY() < DockHost::TAB_BAR_SZ + 4.f) {
                // Title bar: let the dock detect a close-button click first; only start a
                // window drag if the press wasn't on the close button.
                m_dock.handleMouse(m_window->mouseX(), m_window->mouseY(), press, false);
                if (!m_dock.closeRequested()) {
                    m_state    = State::HeaderDrag;
                    m_dragOffX = static_cast<int>(m_window->mouseX());
                    m_dragOffY = static_cast<int>(m_window->mouseY());
                }
            } else if (m_contentInput) {
                m_contentInput(m_window->mouseX(), m_window->mouseY(), press, !btnDown && m_wasDown);
            } else {
                m_dock.handleMouse(m_window->mouseX(), m_window->mouseY(), press, !btnDown && m_wasDown);
            }
            m_shouldClose = m_dock.closeRequested() || m_window->shouldClose();
            m_wasDown     = btnDown;
            return nullptr;
        }
        return nullptr;
    }

    // -------------------------------------------------------------------------
    // Render the dock into this window's private GPU surface.
    // Do NOT call during InitialDrag (window is offscreen).
    // -------------------------------------------------------------------------
    void render(GpuHal& hal, PrimitiveBuffer& buf) {
        buf.clear();
        m_dock.populateRenderPrimitives(buf);
        if (m_contentRender)
            m_contentRender(buf, static_cast<float>(m_winW), static_cast<float>(m_winH));
        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
    }

    // Wire the floated panel's content (rendered/driven by the catalog) into this window.
    void setContentRender(std::function<void(PrimitiveBuffer&, float, float)> fn) { m_contentRender = std::move(fn); }
    void setContentInput (std::function<void(float, float, bool, bool)> fn)       { m_contentInput  = std::move(fn); }

    // -------------------------------------------------------------------------
    // Destroy the Vulkan surface.  Call before erasing this object.
    // -------------------------------------------------------------------------
    void destroySurface(GpuHal& hal) {
        hal.destroySurface(m_surface);
    }

    // ---- Accessors ----------------------------------------------------------

    bool isInInitialDrag() const { return m_state == State::InitialDrag; }
    bool isDragging()      const { return m_state != State::Idle; }
    bool shouldClose()     const { return m_shouldClose; }

    DockWidget& dock()          { return m_dock; }
    DockWidget  takeDock()      { return std::move(m_dock); }

    // Cursor offset within this window at the start of the current drag.
    int dragOffX() const { return m_dragOffX; }
    int dragOffY() const { return m_dragOffY; }

    float dockWidth()  const { return m_dock.width(); }
    float dockHeight() const { return m_dock.height(); }

private:
    enum class State { InitialDrag, Idle, HeaderDrag };

    void _clearAllHostDrags() {
        for (const auto& e : DockRegistry::instance().entries())
            e.host->updateDrag(0.f, 0.f, 0.f, 0.f, 0, 0, nullptr);
    }

    void _clearHostDragsExcept(DockHost* keep) {
        for (const auto& e : DockRegistry::instance().entries())
            if (e.host != keep)
                e.host->updateDrag(0.f, 0.f, 0.f, 0.f, 0, 0, nullptr);
    }

    std::unique_ptr<LinuxPlatformWindow> m_window;
    GpuSurfaceId m_surface{kPrimarySurface};
    DockWidget   m_dock;
    uint32_t     m_winW{kDefaultW}, m_winH{kDefaultH};

    // Optional: render & drive the floated panel's catalog content inside this window.
    // Coordinates are window-local; the content area sits below the title bar.
    std::function<void(PrimitiveBuffer&, float, float)> m_contentRender;
    std::function<void(float, float, bool, bool)>       m_contentInput;

    State m_state{State::Idle};
    bool  m_shouldClose{false};
    bool  m_wasDown{false};  // previous-frame button state for release detection in Idle
    int   m_dragOffX{0}, m_dragOffY{0};
};

} // namespace Genesis
