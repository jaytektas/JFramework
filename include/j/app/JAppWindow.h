#pragma once

// ============================================================================
// jf::JAppWindow — a top-level application window that owns the native window,
// the Vulkan GPU HAL, the font atlas, AND the framework's own window chrome
// (frame + title bar + min/max/close + drag + edge-resize), then runs the
// standard render/present/resize loop.
//
// The window is opened BORDERLESS so the OS chrome is gone and the framework
// draws its own frame (JWindowSkin) — consistent custom look across platforms.
// The application supplies only its content: onRender(buffer) draws into the
// content area (origin at contentTop()), and onInput(mx,my,pressed,released)
// receives clicks the chrome did not consume.
//
//   JGuiApplication app;
//   JAppWindow win("My App", 1280, 800);
//   win.onRender = [&](JPrimitiveBuffer& buf){ ...draw content below win.contentTop()... };
//   win.onInput  = [&](float x,float y,bool p,bool r){ ...dispatch... };
//   return win.run();
// ============================================================================

#include <j/core/ApplicationCore.h>      // JPlatformWindow
#include <j/core/PlatformCommon.h>       // JPlatformWindowStyle
#include <j/core/GenesisComponents.h>    // JGuiApplication, + JTextHelper/Colors via BaseWidgets
#include <j/core/DockManager.h>          // JDockHost (auto-managed dock layout)
#include <j/core/DockSpace.h>            // JDockSpace (centre + 4 dock areas)
#include <j/core/DockRegistry.h>         // host registration for floating re-dock
#include <j/core/MenuSystem.h>           // JMenuBar
#include <j/core/MenuRuntime.h>          // JMenuRuntime (popup menu engine)
#include <j/core/ToolBar.h>              // JToolBar
#include <j/platforms/PlatformWindow.h>  // createPlatformWindow
#if defined(__linux__)
#  include <j/platforms/linux/FloatingDockWindow.h>   // tear-out / floating docks
#endif
#include <j/graphics/GpuHal.h>
#include <j/graphics/FontEngine.h>
#include <j/graphics/RenderPrimitive.h>

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <thread>
#include <functional>
#include <memory>
#include <string>
#include <vector>

inline namespace jf {

class JAppWindow {
public:
    JAppWindow(const std::string& title, uint32_t width, uint32_t height)
        : m_title(title), m_w(width), m_h(height) {
        // Borderless: the OS draws no chrome — the framework draws its own frame.
        m_window = createPlatformWindow(title, width, height, 100, 100,
                                        JPlatformWindowStyle::Borderless);
        if (!m_window) return;
        m_window->setMinSize(200, 150);   // declare a sane minimum -> WM treats it resizable
        m_hal = JGpuHal::create(JGpuApiType::Vulkan, m_window->nativeHandle());
        if (m_hal) m_hal->resizeSwapchain(width, height);
        if (m_font.loadSystemFont() && m_hal) {
            auto atlas = m_font.buildAtlas(14.0f * m_window->dpiScale());
            JTextHelper::setAtlas(atlas);
            m_hal->uploadFontAtlas(atlas.bitmap.data(), atlas.width, atlas.height);
        }

        // Publish the dock-space areas on the AI bus so the layout is externally monitorable
        // (the hosts aren't JWidgets, so they wouldn't otherwise appear).
        if (auto* app = JGuiApplication::instance())
            app->addSnapshotContributor([this](std::vector<JAiNodeDescriptor>& out) {
                static const char* nm[] = {"Left", "Right", "Top", "Bottom", "Center"};
                for (int a = 0; a < JDockSpace::AreaCount; ++a) {
                    const auto area = JDockSpace::Area(a);
                    const JRect& r  = m_space.rect(area);
                    JAiNodeDescriptor d{};
                    d.id = 0xDA000000u | static_cast<uint32_t>(a);
                    d.x = r.x; d.y = r.y; d.width = r.width; d.height = r.height;
                    d.stateFlags = AiVisible | (m_space.active(area) ? AiEnabled : 0u);
                    aiSetField(d.role, sizeof(d.role), "DockArea");
                    aiSetField(d.name, sizeof(d.name), nm[a]);
                    char val[24];
                    std::snprintf(val, sizeof(val), "%s n=%zu",
                                  m_space.active(area) ? "active" : "collapsed",
                                  m_space.host(area).dockCount());
                    aiSetField(d.value, sizeof(d.value), val);
                    out.push_back(d);
                }
            });
    }

    bool valid() const { return m_window && m_hal; }

    JPlatformWindow& window() { return *m_window; }
    JGpuHal&         hal()    { return *m_hal; }
    uint32_t width()  const { return m_w; }
    uint32_t height() const { return m_h; }
    // Content y-origin: below the title bar + menu bar + toolbar.
    float    contentTop() const { return m_titleH + m_menuH + m_toolbarH; }

    // The window's dock host — build the layout on this; the runner auto-lays-it-out,
    // renders it, and routes input to it (no per-app plumbing). Empty = no docks.
    // The window's dock space: a protected centre framed by Left/Right/Top/Bottom areas,
    // each a full dock host. Build the layout on the areas; the runner lays it out, renders
    // it, routes input, and handles tear-out / re-dock across areas.
    JDockSpace& dockSpace() { return m_space; }

    // The window's menu bar — built lazily on first call (reserves a 28px strip below the
    // title bar and wires the popup engine). Add JMenu* with addMenu(); the app owns the
    // JMenu objects. Returns the bar so the app just declares menus.
    JMenuBar& menuBar() {
        if (!m_menuBar) {
            auto& g = JGuiApplication::instance()->sceneGraph();
            m_menuBar = std::make_unique<JMenuBar>(g);
            m_menuBar->onQueryScreenPos = [this](float lx, float ly) -> std::pair<int,int> {
                return { m_window->screenX() + static_cast<int>(lx),
                         m_window->screenY() + static_cast<int>(ly) };
            };
            m_menuRuntime.wire(m_hal.get(),
                               static_cast<JPopupWindow::NativeWinHandleType>(m_window->rawWindowId()),
                               m_menuBar.get());
            m_menuH = 28.f;
            layoutDocks();
        }
        return *m_menuBar;
    }

    // Status bar (reserves a strip at the bottom; framework draws the text).
    void setStatusText(std::string s) {
        m_statusText = std::move(s);
        if (m_statusH == 0.f) { m_statusH = 24.f; layoutDocks(); }
    }

    // The window's toolbar — built lazily (reserves a 40px strip below the menu bar). Add
    // buttons with addButton(label, onClick); the framework lays it out, renders it, and
    // routes input. Returns the bar so the app just declares buttons.
    JToolBar& toolBar() {
        if (!m_toolBar) { m_toolBar = std::make_unique<JToolBar>(); m_toolbarH = 40.f; layoutDocks(); }
        return *m_toolBar;
    }

    // App hooks.
    std::function<void(JPrimitiveBuffer&)>              onRender;  // draw content (below contentTop())
    std::function<void(float,float,bool,bool)>          onInput;   // mx,my,pressed,released (chrome-filtered)
    std::function<void(uint32_t,uint32_t)>              onResize;  // new client size (px)

    void requestRedraw() { m_needRedraw = true; }   // app asks for a frame (e.g. animation)

    int run() {
        if (!valid()) return -1;
        m_window->setVSync(true);
        layoutDocks();
        int   redraw = 4;                  // frames left to render (armed by activity)
        float lastMx = -1.f, lastMy = -1.f;
        while (!m_window->shouldClose()) {
            m_window->pollNativeEvents();
            bool activity = false;

            // Sync the swapchain to the window's CURRENT size every iteration, by direct
            // comparison rather than the resize flag (which can lag/coalesce). If the
            // swapchain ever trails the window by a frame, the newly exposed edge flashes
            // — the main source of resize flicker. width()/height() keep them in lockstep.
            m_window->consumeWasResized();   // drain the flag; we resize by size-compare
            const uint32_t curW = m_window->width(), curH = m_window->height();
            if (curW > 0 && curH > 0 && (curW != m_w || curH != m_h)) {
                m_w = curW; m_h = curH;
                m_hal->resizeSwapchain(m_w, m_h);
                layoutDocks();
                if (onResize) onResize(m_w, m_h);
                activity = true;
            }

            const float mx = m_window->mouseX(), my = m_window->mouseY();
            m_mx = mx; m_my = my;   // remembered for chrome hover in drawChrome()
            // Resize-cursor feedback — window edges first, then dock splitters. Both are
            // framework-managed: the app never sets a resize cursor itself.
            // Window-edge resize cursor only when not maximized (no edges to drag then),
            // but dock-splitter cursors apply either way.
            JPlatformCursor c = m_window->isMaximized()
                                    ? JPlatformCursor::Default
                                    : cursorForDir(resizeDirAt(mx, my));
            if (c == JPlatformCursor::Default) {
                switch (m_space.getHoverCursor(mx, my)) {
                    case JDockHost::JHoverCursor::Horiz: c = JPlatformCursor::ResizeLeftRight; break;
                    case JDockHost::JHoverCursor::Vert:  c = JPlatformCursor::ResizeUpDown;    break;
                    default: break;
                }
            }
            m_window->setCursor(c);
            const bool pressed  = m_window->consumePress();
            const bool released = m_window->consumeRelease();
            if (pressed || released || mx != lastMx || my != lastMy) activity = true;
            // While a button is held (drag / WM resize), render continuously — never sleep
            // through resize events, or the swapchain lags the window and the edge flashes.
            if (m_window->isLeftButtonDown()) activity = true;
            lastMx = mx; lastMy = my;

            const bool chromeAte = handleChrome(mx, my, pressed);
            const bool menusOpen = m_menuRuntime.hasOpenMenus();
            // While a menu popup is open it owns input modally (its own grab); the main UI is
            // frozen out so it can't fight the popup. The menu bar still gets hover so the
            // active title stays lit.
            bool menuAte = menusOpen;
            if (!chromeAte && m_menuBar) {
                // While a menu is open the popup has grabbed the pointer, so the main
                // window's mouse is frozen. Query the GLOBAL cursor instead (works under a
                // grab) so the menu bar can detect hover over a sibling title and switch to
                // it — the open menu follows the mouse along the bar.
                float bx = mx, by = my;
                if (menusOpen) {
                    auto [gx, gy] = m_window->globalCursorPos();
                    bx = static_cast<float>(gx - m_window->screenX());
                    by = static_cast<float>(gy - m_window->screenY());
                }
                m_menuBar->handleMouseMove(bx, by);
                if (!menusOpen && my >= m_titleH && my < m_titleH + m_menuH) {
                    if (pressed)  { m_menuBar->handleMousePress(mx, my); menuAte = true; }
                    if (released)   m_menuBar->handleMouseRelease(mx, my);
                }
            }
            if (m_toolBar && m_toolbarH > 0.f) {
                const float ty = m_titleH + m_menuH;
                m_toolBar->setRect(JRect{0.f, ty, static_cast<float>(m_w), m_toolbarH});
                const bool act = !chromeAte && !menusOpen;
                if (m_toolBar->handleMouse(mx, my, act && pressed, act && released) && !menusOpen)
                    menuAte = true;   // swallow input over the toolbar strip
            }
            if (!chromeAte && !menuAte) {
                auto res = m_space.handleMouse(mx, my, pressed, released);
                if (res.ev && res.host) {
                    if (res.ev->type == JDockHost::JDockEvent::JType::WantsFloat)
                        spawnFloat(res.host, res.ev->dock);   // framework owns tear-out
                    else if (res.ev->type == JDockHost::JDockEvent::JType::CloseRequested)
                        res.host->removeDock(res.ev->dock);
                }
            }
            if (onInput) onInput(mx, my, (chromeAte || menuAte) ? false : pressed, released);

            if (auto* app = JGuiApplication::instance()) app->serviceFrame();

            // Floating docks (tear-out): keep the host's screen rect current for re-dock
            // hit-testing, then poll/render/re-dock/close each floating window.
            m_space.registerAll(m_window->screenX(), m_window->screenY());
#if defined(__linux__)
            // Escape reverts an in-progress dock drag to its exact pre-drag state. A float
            // may hold keyboard focus (it flags consumeAbortRequest) or the main window may
            // (poll its keys while dragging) — either way the host owns the revert.
            bool escAbort = false, anyDragging = false;
            for (auto& fd : m_floating) {
                if (fd.isDragging()) anyDragging = true;
                if (fd.consumeAbortRequest()) escAbort = true;
            }
            if (anyDragging)
                for (const auto& k : m_window->consumeAllKeys())
                    if (k.key == JKeyEvent::JKey::Escape) escAbort = true;
            if (escAbort) revertActiveDrag();
#endif
            serviceFloats();
#if defined(__linux__)
            if (!m_floating.empty()) activity = true;   // keep polling while docks float
#endif
            // Re-derive the area layout each frame so tear-out/re-dock changes take effect —
            // an emptied area collapses and the centre reclaims its space.
            m_space.computeLayout({0.f, contentTop(), static_cast<float>(m_w),
                                   static_cast<float>(m_h) - contentTop() - m_statusH});

            // Menu popups: poll (modal grab / dismiss-outside) + render their own surfaces.
            if (m_menuBar) {
                m_menuRuntime.updateAndRender(*m_hal);
                if (m_menuRuntime.hasOpenMenus()) activity = true;
            }

            if (m_needRedraw) { activity = true; m_needRedraw = false; }
            if (activity) redraw = 4;

            // Event-driven: present only while armed, idle otherwise. Continuously
            // presenting static content flickered under the compositor — the toolkit's
            // own loop renders in bursts and stays solid, so we do the same.
            if (redraw > 0) {
                --redraw;
                JPrimitiveBuffer buffer;
                auto frame = m_hal->beginFrame();
                drawChrome(buffer);
                drawBars(buffer);                           // menu bar / toolbar / status bar
                m_space.render(buffer);                     // all areas: content + overlays
                if (onRender) onRender(buffer);
                m_hal->drawPrimitives(buffer);
                m_hal->submitAndPresentFrame(frame);
                m_window->swapBuffers();   // frame presented -> echo _NET_WM_SYNC_REQUEST
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
        }
        m_hal->waitIdle();
        return 0;
    }

private:
    static constexpr float kBtnW = 28.0f;   // width of each window-control button (toolkit value)

    // Resize direction under (mx,my): _NET_WM_MOVERESIZE dirs 3=R,4=BR,5=B,6=BL,7=L, or
    // -1. Top edge is intentionally skipped — it conflicts with the title bar (catalog).
    int resizeDirAt(float mx, float my) const {
        const float W = static_cast<float>(m_w), H = static_cast<float>(m_h);
        constexpr float kEdge = 6.0f, kCorn = 14.0f;
        const bool onLeft   = mx < kEdge   && my >= m_titleH;
        const bool onRight  = mx >= W-kEdge && my >= m_titleH;
        const bool onBottom = my >= H-kEdge;
        const bool onBL     = mx < kCorn   && my >= H-kCorn;
        const bool onBR     = mx >= W-kCorn && my >= H-kCorn;
        if (onBL)     return 6;
        if (onBR)     return 4;
        if (onBottom) return 5;
        if (onLeft)   return 7;
        if (onRight)  return 3;
        return -1;
    }

    void layoutDocks() {
        const float top = contentTop();
        m_space.computeLayout({0.f, top, static_cast<float>(m_w),
                               static_cast<float>(m_h) - top - m_statusH});
        if (m_menuBar) {
            auto& g = JGuiApplication::instance()->sceneGraph();
            g.getLayout(m_menuBar->getNodeId()).boundingBox = {0.f, m_titleH, static_cast<float>(m_w), m_menuH};
            g.invalidateNode(m_menuBar->getNodeId(), DirtySelf);
            g.computeLayout(m_menuBar->getNodeId(),
                            {static_cast<float>(m_w), static_cast<float>(m_w), m_menuH, m_menuH});
        }
    }

    // Draw the menu bar / toolbar / status bar strips into the main frame.
    void drawBars(JPrimitiveBuffer& buf) {
        const float W = static_cast<float>(m_w);
        uint8_t bg[4]  = {Colors::Surface1[0], Colors::Surface1[1], Colors::Surface1[2], 255};
        uint8_t sep[4] = {Colors::Border[0], Colors::Border[1], Colors::Border[2], Colors::Border[3]};
        if (m_menuBar) {
            buf.pushRectangle(0.f, m_titleH, W, m_menuH, bg, 0.f);
            buf.pushRectangle(0.f, m_titleH + m_menuH - 1.f, W, 1.f, sep, 0.f);
            m_menuBar->populateRenderPrimitives(buf);
        }
        if (m_toolBar && m_toolbarH > 0.f) {
            const float y = m_titleH + m_menuH;
            buf.pushRectangle(0.f, y, W, m_toolbarH, bg, 0.f);
            buf.pushRectangle(0.f, y + m_toolbarH - 1.f, W, 1.f, sep, 0.f);
            m_toolBar->setRect(JRect{0.f, y, W, m_toolbarH});
            m_toolBar->render(buf);
        }
        if (m_statusH > 0.f) {
            const float y = static_cast<float>(m_h) - m_statusH;
            buf.pushRectangle(0.f, y, W, m_statusH, bg, 0.f);
            buf.pushRectangle(0.f, y, W, 1.f, sep, 0.f);   // separator at the top edge
            if (JTextHelper::hasAtlas() && !m_statusText.empty()) {
                uint8_t tc[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, tc);
                JTextHelper::pushText(buf, 10.f, y + (m_statusH - JTextHelper::lineHeight()) * 0.5f, m_statusText, tc);
            }
        }
    }

    // Tear a dock out of the host into its own floating window (the host only emits
    // WantsFloat for docks the app declared floatable).
    void spawnFloat(JDockHost* host, JDockWidget* dw) {
#if defined(__linux__)
        if (!host || !dw) return;
        const JDockNode* n = host->node(host->findDock(dw));
        if (!n) return;
        const JRect r = n->rect;
        // Tear out at the dock's CURRENT docked size (its leaf rect) — so a panel resized
        // while docked floats at that size. (Re-docking then uses the float's own size,
        // which the float keeps in sync as it's resized.)
        const float fw = std::max(r.width,  dw->minW());
        const float fh = std::max(r.height, dw->minH());
        const int sx = m_window->screenX() + static_cast<int>(r.x);
        const int sy = m_window->screenY() + static_cast<int>(r.y);
        auto [gx, gy] = m_window->globalCursorPos();
        const int offX = std::clamp(gx - sx, 0, static_cast<int>(fw) - 1);
        const int offY = std::clamp(gy - sy, 0, static_cast<int>(fh) - 1);

        m_revert = { true, host->saveTree(), dw, host };   // enable Escape-to-revert this drag
        host->removeDock(dw);
        JDockWidget moved = std::move(*dw);
        moved.setPosition(0.f, 0.f);
        moved.setSize(fw, fh);
        m_floating.emplace_back(std::move(moved), sx, sy,
                                static_cast<uint32_t>(fw), static_cast<uint32_t>(fh),
                                offX, offY, *m_hal, /*initialDrag=*/true,
                                JFloatingDockOptions{},
                                static_cast<xcb_window_t>(m_window->rawWindowId()));
#else
        (void)host; (void)dw;
#endif
    }

    // Escape-to-revert: restore the host to its exact pre-drag tree and re-home the dock,
    // undoing the tear-out (instead of leaving it floating mid-flight).
    void revertActiveDrag() {
#if defined(__linux__)
        if (!m_revert.active) return;
        for (auto it = m_floating.begin(); it != m_floating.end(); ++it) {
            if (!it->isInInitialDrag()) continue;
            m_ownedDocks.push_back(std::make_unique<JDockWidget>(it->takeDock()));
            JDockWidget* back = m_ownedDocks.back().get();
            it->destroySurface(*m_hal);
            m_floating.erase(it);
            if (m_revert.host) {
                m_revert.host->restoreTree(m_revert.tree);
                m_revert.host->retargetDock(m_revert.oldPtr, back);  // saved tree had the moved-from ptr
            }
            layoutDocks();
            break;
        }
        m_revert.active = false;
#endif
    }

    // Per-frame: drive each floating window — move/drag, re-dock to the host on drop,
    // close, and render to its own surface.
    void serviceFloats() {
#if defined(__linux__)
        // A settled (released/committed) tear-out is no longer revertible.
        bool anyInitial = false;
        for (auto& fd : m_floating) if (fd.isInInitialDrag()) { anyInitial = true; break; }
        if (!anyInitial) m_revert.active = false;

        for (auto it = m_floating.begin(); it != m_floating.end(); ) {
            auto pr = it->pollAndMove();
            if (pr.type == JFloatingDockWindow::JPollResult::JType::CommitDrop &&
                pr.dropHost && pr.dropHost->tryCommitDrop()) {
                it->destroySurface(*m_hal);
                JDockWidget* oldPtr = &it->dock();                       // pointer the host re-inserted
                m_ownedDocks.push_back(std::make_unique<JDockWidget>(it->takeDock()));
                pr.dropHost->retargetDock(oldPtr, m_ownedDocks.back().get()); // fix host -> owned copy
                it = m_floating.erase(it);
                continue;
            }
            if (it->shouldClose()) { it->destroySurface(*m_hal); it = m_floating.erase(it); continue; }
            JPrimitiveBuffer fbuf;
            it->render(*m_hal, fbuf);
            ++it;
        }
#endif
    }

    static JPlatformCursor cursorForDir(int d) {
        switch (d) {
            case 4: return JPlatformCursor::ResizeBottomRight;
            case 6: return JPlatformCursor::ResizeBottomLeft;
            case 5: return JPlatformCursor::ResizeUpDown;
            case 3: case 7: return JPlatformCursor::ResizeLeftRight;
            default: return JPlatformCursor::Default;
        }
    }

    // Hit-test the chrome on a fresh press; act on it. Returns true if it consumed the press.
    bool handleChrome(float mx, float my, bool pressed) {
        if (!pressed) return false;
        const float W = static_cast<float>(m_w);

        if (!m_window->isMaximized()) {
            const int dir = resizeDirAt(mx, my);
            if (dir >= 0) { m_window->startWindowResize(static_cast<uint32_t>(dir)); return true; }
        }

        // Title bar: window-control buttons (right-aligned), else drag.
        if (my < m_titleH) {
            if (mx >= W - kBtnW)     { m_window->requestClose(); return true; }
            if (mx >= W - 2 * kBtnW) { m_window->setMaximized(!m_window->isMaximized()); return true; }
            if (mx >= W - 3 * kBtnW) { m_window->minimize(); return true; }
            m_window->startWindowMove();
            return true;
        }
        return false;
    }

    // The toolkit's own title bar — matches controls_catalog exactly (flat 28px strip,
    // Colors palette, a 1px separator, and vector-drawn min/max/close with hover).
    void drawChrome(JPrimitiveBuffer& buf) {
        const float W  = static_cast<float>(m_w), H = static_cast<float>(m_h);
        const float lh = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() : 14.0f;

        // Opaque full-window background FIRST — every pixel is written each frame, so the
        // swapchain's images can't cycle stale content (that was the resize flicker).
        uint8_t bg[4] = {Colors::Surface0[0], Colors::Surface0[1], Colors::Surface0[2], 255};
        buf.pushRectangle(0.f, 0.f, W, H, bg, 0.f);

        uint8_t tbg[4] = {22, 22, 28, 255};
        buf.pushRectangle(0.f, 0.f, W, m_titleH, tbg, 0.f);

        if (JTextHelper::hasAtlas()) {
            uint8_t tc[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, tc);
            JTextHelper::pushText(buf, 10.f, (m_titleH - lh) * 0.5f, m_title, tc,
                                  W - kBtnW * 3.f - 20.f);
        }
        uint8_t sep[4] = {Colors::Border[0], Colors::Border[1], Colors::Border[2], Colors::Border[3]};
        buf.pushRectangle(0.f, m_titleH - 1.f, W, 1.f, sep, 0.f);

        const float closeX = W - kBtnW, maxX = W - kBtnW * 2.f, minX = W - kBtnW * 3.f;
        auto hov = [&](float bx) {
            return m_my >= 0.f && m_my < m_titleH && m_mx >= bx && m_mx < bx + kBtnW;
        };

        // Minimize  −
        {
            if (hov(minX)) { uint8_t hb[4]={60,60,70,200}; buf.pushRectangle(minX,0.f,kBtnW,m_titleH,hb,0.f); }
            float cx = minX + kBtnW*0.5f - 4.f, cy = m_titleH*0.5f - 1.f;
            uint8_t ic[4] = {190,190,200,220};
            buf.pushRectangle(cx, cy, 9.f, 2.f, ic, 0.f);
        }
        // Maximize □ / restore ⊡
        {
            if (hov(maxX)) { uint8_t hb[4]={60,60,70,200}; buf.pushRectangle(maxX,0.f,kBtnW,m_titleH,hb,0.f); }
            float cx = maxX + kBtnW*0.5f - 4.f, cy = m_titleH*0.5f - 4.f;
            uint8_t ic[4]={190,190,200,220}, nf[4]={0,0,0,0}, wbg[4]={22,22,28,255};
            if (!m_window->isMaximized()) {
                buf.pushRectangle(cx, cy, 9.f, 9.f, nf, 1.f, 1.5f, ic);
            } else {
                buf.pushRectangle(cx+2.f, cy,     7.f, 7.f, nf,  0.f, 1.f, ic);
                buf.pushRectangle(cx,     cy+2.f,  7.f, 7.f, wbg, 0.f, 1.f, ic);
            }
        }
        // Close  ×
        {
            if (hov(closeX)) { uint8_t hb[4]={180,40,40,220}; buf.pushRectangle(closeX,0.f,kBtnW,m_titleH,hb,0.f); }
            float cx = closeX + kBtnW*0.5f - 4.f, cy = m_titleH*0.5f - 1.f;
            uint8_t ic[4] = {210,210,220,230};
            buf.pushRectangle(cx,        cy,       9.f, 2.f, ic, 1.f);
            buf.pushRectangle(cx + 3.5f, cy - 3.5f, 2.f, 9.f, ic, 1.f);
        }
    }

    std::unique_ptr<JPlatformWindow> m_window;
    std::unique_ptr<JGpuHal>         m_hal;
    JDockSpace                       m_space;
    std::vector<std::unique_ptr<JDockWidget>> m_ownedDocks;   // docks adopted back from floats
#if defined(__linux__)
    std::vector<JFloatingDockWindow> m_floating;
#endif
    // Pre-drag tree snapshot for Escape-to-revert (active only during a tear-out drag).
    struct { bool active{false}; JDockHost::JSavedTree tree; JDockWidget* oldPtr{nullptr};
             JDockHost* host{nullptr}; } m_revert;

    std::unique_ptr<JMenuBar> m_menuBar;     // lazily created on menuBar()
    JMenuRuntime              m_menuRuntime; // popup menu engine
    std::unique_ptr<JToolBar> m_toolBar;     // lazily created on toolBar()
    float                     m_menuH{0.f}, m_toolbarH{0.f}, m_statusH{0.f};
    std::string               m_statusText;
    JFontEngine                      m_font;
    std::string                      m_title;
    uint32_t                         m_w{0}, m_h{0};
    float                            m_mx{-1.0f}, m_my{-1.0f};
    float                            m_titleH{28.0f};
    bool                             m_needRedraw{false};
};

}  // namespace jf
