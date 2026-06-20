#pragma once

#include <genesis/core/BaseWidgets.h>
#include <genesis/core/DockWidget.h>
#include <genesis/core/DockManager.h>
#include <genesis/core/DockRegistry.h>
#include <genesis/graphics/GpuHal.h>
#include <genesis/graphics/RenderPrimitive.h>

#if defined(_WIN32)
#include <genesis/platforms/windows/WindowsPlatformWindow.h>
#else
#include <genesis/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <memory>
#include <functional>
#include <vector>
#include <algorithm>

namespace Genesis {

// ============================================================================
// FloatingDragBehavior — defines the drag behavior of the floating host window:
//
//   Legacy                    — Tab/title drag tears out dock (no global bar).
//                               Drag window via keyboard Alt + Drag.
//   AlwaysGlobalTitleBar      — Always show a global window title bar at the top.
//   ConditionalGlobalTitleBar — Show global window title bar only when nested (docks > 1).
// ============================================================================
enum class FloatingDragBehavior : uint8_t {
    Legacy,
    AlwaysGlobalTitleBar,
    ConditionalGlobalTitleBar
};

struct FloatingDockOptions {
    FloatingDragBehavior dragBehavior{FloatingDragBehavior::ConditionalGlobalTitleBar};
    bool singleDockDragMovesWindow{true};
    bool tabDragTearsOut{true};
    bool splitDragTearsOut{true};
    bool livePreviewEnabled{true};
};

// ============================================================================
// FloatingDockWindow — a DockWidget container that lives in its own OS-level Popup
// window and can be dragged anywhere on the desktop, including over and into
// any DockHost registered with DockRegistry.
// ============================================================================

class FloatingDockWindow {
public:
    static constexpr uint32_t kDefaultW = 340;
    static constexpr uint32_t kDefaultH = 260;
    static constexpr float    kGlobalTitleH = 26.0f;

    struct PollResult {
        enum class Type {
            None,
            CommitDrop,     // drag ended, commit drop to dropHost
            WantsFloat,     // dock wants to float
        } type{Type::None};

        DockHost*   dropHost{nullptr};
        DockWidget* wantsFloatDock{nullptr};
        Rect        wantsFloatRect{};
    };

#if defined(_WIN32)
    using PlatformWinType = WindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType = LinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    // Construct from a DockWidget extracted from a DockHost (WantsFloat path).
    FloatingDockWindow(DockWidget dock,
                       int screenX, int screenY,
                       uint32_t winW, uint32_t winH,
                       int dragOffX, int dragOffY,
                       GpuHal& hal,
                       bool initialDrag = false,
                       FloatingDockOptions options = {},
                       NativeWinHandleType parentWindow = {})
        : m_window(std::make_unique<PlatformWinType>(
              dock.title().c_str(), winW, winH, screenX, screenY,
#if defined(_WIN32)
              (parentWindow != nullptr) ? PlatformWindowStyle::Borderless : PlatformWindowStyle::Popup,
#else
              (parentWindow != 0) ? PlatformWindowStyle::Borderless : PlatformWindowStyle::Popup,
#endif
              parentWindow))
        , m_surface(hal.createSurface(m_window->nativeHandle(), winW, winH))
        , m_winW(winW), m_winH(winH)
        , m_state(initialDrag ? State::InitialDrag : State::Idle)
        , m_dragOffX(dragOffX)
        , m_dragOffY(dragOffY)
        , m_options(options)
    {

        m_dockHost = std::make_unique<DockHost>();
        m_dockHost->setLivePreviewEnabled(m_options.livePreviewEnabled);
        m_dockHost->setRootSplit(SplitDir::Horizontal);
        DockNodeId leaf = m_dockHost->addLeaf(m_dockHost->rootId(), "floating-leaf", 1.0f);

        auto d = std::make_unique<DockWidget>(std::move(dock));
        d->setPosition(0.f, 0.f);
        d->setSize(static_cast<float>(winW), static_cast<float>(winH));

        m_dockHost->insertDock(d.get(), leaf);
        m_docks.push_back(std::move(d));

        float topOffset = isGlobalTitleBarVisible() ? kGlobalTitleH : 0.f;
        m_dockHost->computeLayout({0.f, topOffset, static_cast<float>(winW), static_cast<float>(winH) - topOffset});

        float minW = m_dockHost->minWidthNeeded();
        float minH = m_dockHost->minHeightNeeded() + topOffset;
        if (static_cast<float>(m_winW) < minW || static_cast<float>(m_winH) < minH) {
            m_winW = std::max(m_winW, static_cast<uint32_t>(std::ceil(minW)));
            m_winH = std::max(m_winH, static_cast<uint32_t>(std::ceil(minH)));
            m_window->setSize(m_winW, m_winH);
            m_needsSurfaceResize = true;
            m_dockHost->computeLayout({0.f, topOffset, static_cast<float>(m_winW), static_cast<float>(m_winH) - topOffset});
        }

        DockRegistry::instance().registerHost(*m_dockHost, screenX, screenY, m_winW, m_winH);
    }

    // Construct from a TornTabState (TabBar tear-off path).
    FloatingDockWindow(TornTabState state,
                       int screenX, int screenY,
                       int dragOffX, int dragOffY,
                       GpuHal& hal,
                       FloatingDockOptions options = {},
                       NativeWinHandleType parentWindow = {})
        : FloatingDockWindow(
              DockWidget(std::move(state), 0.f, 0.f,
                         static_cast<float>(kDefaultW), static_cast<float>(kDefaultH)),
              screenX, screenY, kDefaultW, kDefaultH,
              dragOffX, dragOffY, hal, /*initialDrag=*/true, options, parentWindow)
    {}

    FloatingDockWindow(const FloatingDockWindow&)            = delete;
    FloatingDockWindow& operator=(const FloatingDockWindow&) = delete;

    FloatingDockWindow(FloatingDockWindow&& other) noexcept
        : m_window(std::move(other.m_window))
        , m_surface(other.m_surface)
        , m_dockHost(std::move(other.m_dockHost))
        , m_docks(std::move(other.m_docks))
        , m_winW(other.m_winW)
        , m_winH(other.m_winH)
        , m_contentRenderHost(std::move(other.m_contentRenderHost))
        , m_contentInputHost(std::move(other.m_contentInputHost))
        , m_state(other.m_state)
        , m_shouldClose(other.m_shouldClose)
        , m_wasDown(other.m_wasDown)
        , m_dragOffX(other.m_dragOffX)
        , m_dragOffY(other.m_dragOffY)
        , m_options(other.m_options)
    {
        other.m_surface = GpuSurfaceId{kPrimarySurface};
    }

    FloatingDockWindow& operator=(FloatingDockWindow&& other) noexcept {
        if (this != &other) {
            m_window = std::move(other.m_window);
            m_surface = other.m_surface;
            other.m_surface = GpuSurfaceId{kPrimarySurface};
            m_dockHost = std::move(other.m_dockHost);
            m_docks = std::move(other.m_docks);
            m_winW = other.m_winW;
            m_winH = other.m_winH;
            m_contentRenderHost = std::move(other.m_contentRenderHost);
            m_contentInputHost = std::move(other.m_contentInputHost);
            m_state = other.m_state;
            m_shouldClose = other.m_shouldClose;
            m_wasDown = other.m_wasDown;
            m_dragOffX = other.m_dragOffX;
            m_dragOffY = other.m_dragOffY;
            m_options = other.m_options;
        }
        return *this;
    }

    ~FloatingDockWindow() {
        if (m_dockHost) {
            DockRegistry::instance().unregisterHost(*m_dockHost);
        }
    }

    bool isGlobalTitleBarVisible() const {
        if (m_options.dragBehavior == FloatingDragBehavior::AlwaysGlobalTitleBar) return true;
        if (m_options.dragBehavior == FloatingDragBehavior::ConditionalGlobalTitleBar && m_docks.size() > 1) return true;
        return false;
    }

    void setOptions(const FloatingDockOptions& options) {
        m_options = options;
        if (m_dockHost) {
            m_dockHost->setLivePreviewEnabled(m_options.livePreviewEnabled);
        }
        float topOffset = isGlobalTitleBarVisible() ? kGlobalTitleH : 0.f;
        m_dockHost->computeLayout({0.f, topOffset, static_cast<float>(m_winW), static_cast<float>(m_winH) - topOffset});
    }

    const FloatingDockOptions& options() const { return m_options; }

    void setDragBehavior(FloatingDragBehavior behavior) {
        m_options.dragBehavior = behavior;
        float topOffset = isGlobalTitleBarVisible() ? kGlobalTitleH : 0.f;
        m_dockHost->computeLayout({0.f, topOffset, static_cast<float>(m_winW), static_cast<float>(m_winH) - topOffset});
    }

    FloatingDragBehavior dragBehavior() const { return m_options.dragBehavior; }

    // -------------------------------------------------------------------------
    // Per-frame update.
    // -------------------------------------------------------------------------
    PollResult pollAndMove() {
        m_window->pollNativeEvents();
        m_lastWheel = m_window->consumeWheel();

        const bool btnDown = m_window->isLeftButtonDown();
        const bool press   = m_window->consumePress();
        const bool release = m_window->consumeRelease();
        (void)release;

        // Keep bounds current if WM moved or resized the window, or if we are actively resizing
        if (m_state == State::Idle || m_state == State::Resizing) {
            float topOffset = isGlobalTitleBarVisible() ? kGlobalTitleH : 0.f;
            int kMinWidth = static_cast<int>(std::ceil(m_dockHost->minWidthNeeded()));
            int kMinHeight = static_cast<int>(std::ceil(m_dockHost->minHeightNeeded() + topOffset));

            if (m_state == State::Idle) {
                uint32_t targetW = m_window->width();
                uint32_t targetH = m_window->height();
                if (static_cast<int>(targetW) < kMinWidth || static_cast<int>(targetH) < kMinHeight) {
                    targetW = std::max(targetW, static_cast<uint32_t>(kMinWidth));
                    targetH = std::max(targetH, static_cast<uint32_t>(kMinHeight));
                    m_window->setSize(targetW, targetH);
                }
                if (targetW != m_winW || targetH != m_winH) {
                    m_winW = targetW;
                    m_winH = targetH;
                    m_needsSurfaceResize = true;
                }
            }
            m_dockHost->computeLayout({0.f, topOffset, static_cast<float>(m_winW), static_cast<float>(m_winH) - topOffset});
            DockRegistry::instance().updateBounds(*m_dockHost, m_window->screenX(), m_window->screenY(), m_winW, m_winH);
        }

        switch (m_state) {
        case State::InitialDrag: {
            auto [gx, gy] = m_window->globalCursorPos();
            m_window->setPosition(gx - m_dragOffX, gy - m_dragOffY);
            DockRegistry::instance().updateBounds(*m_dockHost, gx - m_dragOffX, gy - m_dragOffY, m_winW, m_winH);

            DockHost* hoveredHost = nullptr;
            if (auto hit = DockRegistry::instance().hitTest(gx, gy)) {
                if (hit->host != m_dockHost.get()) {
                    hoveredHost = hit->host;
                    DockWidget* draggedDock = m_docks.empty() ? nullptr : m_docks[0].get();
                    hoveredHost->updateDrag(
                        hit->localX, hit->localY,
                        static_cast<float>(gx), static_cast<float>(gy),
                        gx - static_cast<int>(hit->localX),
                        gy - static_cast<int>(hit->localY),
                        draggedDock);
                }
            } else {
                _clearAllHostDrags();
            }

            if (!btnDown) {
                m_state = State::Idle;
                if (hoveredHost) {
                    _clearHostDragsExcept(hoveredHost);
                    return PollResult{PollResult::Type::CommitDrop, hoveredHost, nullptr, {}};
                }
                _clearAllHostDrags();
            }
            return PollResult{};
        }

        case State::HeaderDrag: {
            auto [gx, gy] = m_window->globalCursorPos();
            m_window->setPosition(gx - m_dragOffX, gy - m_dragOffY);
            DockRegistry::instance().updateBounds(*m_dockHost, gx - m_dragOffX, gy - m_dragOffY, m_winW, m_winH);

            DockHost* hoveredHost = nullptr;
            if (auto hit = DockRegistry::instance().hitTest(gx, gy)) {
                if (hit->host != m_dockHost.get()) {
                    hoveredHost = hit->host;
                    DockWidget* draggedDock = m_docks.empty() ? nullptr : m_docks[0].get();
                    hoveredHost->updateDrag(
                        hit->localX, hit->localY,
                        static_cast<float>(gx), static_cast<float>(gy),
                        gx - static_cast<int>(hit->localX),
                        gy - static_cast<int>(hit->localY),
                        draggedDock);
                }
            } else {
                _clearAllHostDrags();
            }

            if (!btnDown) {
                m_state = State::Idle;
                if (hoveredHost) {
                    _clearHostDragsExcept(hoveredHost);
                    return PollResult{PollResult::Type::CommitDrop, hoveredHost, nullptr, {}};
                }
                _clearAllHostDrags();
            }
            return PollResult{};
        }

        case State::Resizing: {
            auto [gx, gy] = m_window->globalCursorPos();
            int dx = gx - m_startMouseX;
            int dy = gy - m_startMouseY;

            int newX = m_startWinX;
            int newY = m_startWinY;
            int newW = static_cast<int>(m_startWinW);
            int newH = static_cast<int>(m_startWinH);

            int kMinWidth = static_cast<int>(std::ceil(m_dockHost->minWidthNeeded()));
            int kMinHeight = static_cast<int>(std::ceil(m_dockHost->minHeightNeeded() + (isGlobalTitleBarVisible() ? kGlobalTitleH : 0.f)));

            switch (m_resizeDir) {
                case ResizeDir::Right:
                    newW = std::max(kMinWidth, static_cast<int>(m_startWinW) + dx);
                    break;
                case ResizeDir::Bottom:
                    newH = std::max(kMinHeight, static_cast<int>(m_startWinH) + dy);
                    break;
                case ResizeDir::Left: {
                    int potentialW = static_cast<int>(m_startWinW) - dx;
                    if (potentialW >= kMinWidth) {
                        newW = potentialW;
                        newX = m_startWinX + dx;
                    } else {
                        newW = kMinWidth;
                        newX = m_startWinX + (static_cast<int>(m_startWinW) - kMinWidth);
                    }
                    break;
                }
                case ResizeDir::Top: {
                    int potentialH = static_cast<int>(m_startWinH) - dy;
                    if (potentialH >= kMinHeight) {
                        newH = potentialH;
                        newY = m_startWinY + dy;
                    } else {
                        newH = kMinHeight;
                        newY = m_startWinY + (static_cast<int>(m_startWinH) - kMinHeight);
                    }
                    break;
                }
                case ResizeDir::TopLeft: {
                    int potentialW = static_cast<int>(m_startWinW) - dx;
                    if (potentialW >= kMinWidth) {
                        newW = potentialW;
                        newX = m_startWinX + dx;
                    } else {
                        newW = kMinWidth;
                        newX = m_startWinX + (static_cast<int>(m_startWinW) - kMinWidth);
                    }
                    int potentialH = static_cast<int>(m_startWinH) - dy;
                    if (potentialH >= kMinHeight) {
                        newH = potentialH;
                        newY = m_startWinY + dy;
                    } else {
                        newH = kMinHeight;
                        newY = m_startWinY + (static_cast<int>(m_startWinH) - kMinHeight);
                    }
                    break;
                }
                case ResizeDir::TopRight: {
                    newW = std::max(kMinWidth, static_cast<int>(m_startWinW) + dx);
                    int potentialH = static_cast<int>(m_startWinH) - dy;
                    if (potentialH >= kMinHeight) {
                        newH = potentialH;
                        newY = m_startWinY + dy;
                    } else {
                        newH = kMinHeight;
                        newY = m_startWinY + (static_cast<int>(m_startWinH) - kMinHeight);
                    }
                    break;
                }
                case ResizeDir::BottomLeft: {
                    int potentialW = static_cast<int>(m_startWinW) - dx;
                    if (potentialW >= kMinWidth) {
                        newW = potentialW;
                        newX = m_startWinX + dx;
                    } else {
                        newW = kMinWidth;
                        newX = m_startWinX + (static_cast<int>(m_startWinW) - kMinWidth);
                    }
                    newH = std::max(kMinHeight, static_cast<int>(m_startWinH) + dy);
                    break;
                }
                case ResizeDir::BottomRight:
                    newW = std::max(kMinWidth, static_cast<int>(m_startWinW) + dx);
                    newH = std::max(kMinHeight, static_cast<int>(m_startWinH) + dy);
                    break;
                default:
                    break;
            }

            if (newW != static_cast<int>(m_winW) || newH != static_cast<int>(m_winH)) {
                m_window->setSize(static_cast<uint32_t>(newW), static_cast<uint32_t>(newH));
                m_winW = static_cast<uint32_t>(newW);
                m_winH = static_cast<uint32_t>(newH);
                m_needsSurfaceResize = true;
            }
            if (newX != m_window->screenX() || newY != m_window->screenY()) {
                m_window->setPosition(newX, newY);
            }

            DockRegistry::instance().updateBounds(*m_dockHost, m_window->screenX(), m_window->screenY(), m_winW, m_winH);

            if (!btnDown) {
                m_state = State::Idle;
                m_window->setCursor(PlatformCursor::Default);
            }
            return PollResult{};
        }

        case State::Idle: {
            float mx = m_window->mouseX();
            float my = m_window->mouseY();

            // Determine hover / resize zones
            constexpr float kResizeBorder = 8.0f;
            bool left   = mx < kResizeBorder;
            bool right  = mx > static_cast<float>(m_winW) - kResizeBorder;
            bool top    = my < kResizeBorder;
            bool bottom = my > static_cast<float>(m_winH) - kResizeBorder;

            ResizeDir hoverDir = ResizeDir::None;
            if (left && top)          hoverDir = ResizeDir::TopLeft;
            else if (right && top)    hoverDir = ResizeDir::TopRight;
            else if (left && bottom)  hoverDir = ResizeDir::BottomLeft;
            else if (right && bottom) hoverDir = ResizeDir::BottomRight;
            else if (left)            hoverDir = ResizeDir::Left;
            else if (right)           hoverDir = ResizeDir::Right;
            else if (top)             hoverDir = ResizeDir::Top;
            else if (bottom)          hoverDir = ResizeDir::Bottom;

            // Set corresponding cursor
            PlatformCursor pc = PlatformCursor::Default;
            switch (hoverDir) {
                case ResizeDir::Left:
                case ResizeDir::Right:       pc = PlatformCursor::ResizeLeftRight; break;
                case ResizeDir::Top:
                case ResizeDir::Bottom:      pc = PlatformCursor::ResizeUpDown;    break;
                case ResizeDir::TopLeft:     pc = PlatformCursor::ResizeTopLeft;    break;
                case ResizeDir::TopRight:    pc = PlatformCursor::ResizeTopRight;   break;
                case ResizeDir::BottomLeft:  pc = PlatformCursor::ResizeBottomLeft; break;
                case ResizeDir::BottomRight: pc = PlatformCursor::ResizeBottomRight;break;
                default:                     pc = PlatformCursor::Default;          break;
            }
            if (pc == PlatformCursor::Default) {
                auto hc = m_dockHost->getHoverCursor(mx, my);
                if (hc == DockHost::HoverCursor::Horiz)      pc = PlatformCursor::ResizeLeftRight;
                else if (hc == DockHost::HoverCursor::Vert)  pc = PlatformCursor::ResizeUpDown;
            }
            m_window->setCursor(pc);

            if (hoverDir != ResizeDir::None && press) {
                m_state = State::Resizing;
                m_resizeDir = hoverDir;
                m_startWinW = m_winW;
                m_startWinH = m_winH;
                m_startWinX = m_window->screenX();
                m_startWinY = m_window->screenY();
                auto [gx, gy] = m_window->globalCursorPos();
                m_startMouseX = gx;
                m_startMouseY = gy;
                return PollResult{};
            }

            bool altDrag = m_window->isAltDown() && press;
            bool titleBarDrag = isGlobalTitleBarVisible() && press && my < kGlobalTitleH;

            if (altDrag || titleBarDrag) {
                m_state    = State::HeaderDrag;
                m_dragOffX = static_cast<int>(mx);
                m_dragOffY = static_cast<int>(my);
                return PollResult{};
            }

            auto ev = m_dockHost->handleMouse(mx, my, press, !btnDown && m_wasDown);

            if (ev) {
                if (ev->type == DockHost::DockEvent::Type::WantsFloat) {
                    bool allowTear = true;
                    if (m_docks.size() == 1) {
                        if (m_options.singleDockDragMovesWindow) {
                            m_state    = State::HeaderDrag;
                            m_dragOffX = static_cast<int>(mx);
                            m_dragOffY = static_cast<int>(my);
                            allowTear  = false;
                        } else {
                            allowTear  = true;
                        }
                    } else {
                        DockNodeId loc = m_dockHost->findDock(ev->dock);
                        const DockNode* leaf = m_dockHost->node(loc);
                        bool isTabGroup = (leaf && leaf->tabs.size() > 1);
                        if (isTabGroup) {
                            allowTear = m_options.tabDragTearsOut;
                        } else {
                            allowTear = m_options.splitDragTearsOut;
                        }
                    }

                    if (allowTear) {
                        DockNodeId loc = m_dockHost->findDock(ev->dock);
                        Rect r = m_dockHost->node(loc)->rect;
                        m_dockHost->removeDock(ev->dock);
                        m_wasDown = btnDown;
                        return PollResult{PollResult::Type::WantsFloat, nullptr, ev->dock, r};
                    }
                }
                if (ev->type == DockHost::DockEvent::Type::CloseRequested) {
                    m_dockHost->removeDock(ev->dock);
                    m_docks.erase(
                        std::remove_if(m_docks.begin(), m_docks.end(),
                            [target = ev->dock](const auto& u){ return u.get() == target; }),
                        m_docks.end());
                    if (m_docks.empty()) {
                        m_shouldClose = true;
                    }
                }
            } else if (m_contentInputHost) {
                m_contentInputHost(mx, my, press, !btnDown && m_wasDown, m_lastWheel);
            }

            m_shouldClose = m_shouldClose || m_window->shouldClose();
            m_wasDown     = btnDown;
            return PollResult{};
        }
        }
        return PollResult{};
    }

    // -------------------------------------------------------------------------
    // Render the dock into this window's private GPU surface.
    // -------------------------------------------------------------------------
    void render(GpuHal& hal, PrimitiveBuffer& buf) {
        if (m_needsSurfaceResize) {
            hal.resizeSurface(m_surface, m_winW, m_winH);
            m_needsSurfaceResize = false;
        }
        buf.clear();

        bool hasTitle = isGlobalTitleBarVisible();
        if (hasTitle) {
            uint8_t barBg[4] = {28, 28, 32, 255};
            buf.pushRectangle(0.f, 0.f, static_cast<float>(m_winW), kGlobalTitleH, barBg, 0.f);

            uint8_t sepColor[4] = {50, 50, 55, 255};
            buf.pushRectangle(0.f, kGlobalTitleH - 1.f, static_cast<float>(m_winW), 1.f, sepColor, 0.f);

            if (TextHelper::hasAtlas()) {
                uint8_t tc[4] = {180, 180, 190, 255};
                float ty = (kGlobalTitleH - TextHelper::lineHeight()) * 0.5f;
                std::string title = m_docks.empty() ? "Genesis Window" : m_docks[0]->title();
                if (m_docks.size() > 1) {
                    title += " (+" + std::to_string(m_docks.size() - 1) + " panels)";
                }
                TextHelper::pushText(buf, 10.f, ty, title, tc, static_cast<float>(m_winW) - 30.f);
            }
        }

        m_dockHost->populateRenderPrimitives(buf);
        if (m_contentRenderHost) {
            m_contentRenderHost(buf);
        }
        m_dockHost->populateOverlay(buf);
        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
    }

    void setContentRenderHost(std::function<void(PrimitiveBuffer&)> fn) { m_contentRenderHost = std::move(fn); }
    void setContentInputHost(std::function<void(float, float, bool, bool, float)> fn) { m_contentInputHost = std::move(fn); }

    // -------------------------------------------------------------------------
    // Destroy the Vulkan surface.  Call before erasing this object.
    // -------------------------------------------------------------------------
    void destroySurface(GpuHal& hal) {
        if (m_surface != kPrimarySurface) {
            hal.destroySurface(m_surface);
            m_surface = kPrimarySurface;
        }
    }

    // ---- Accessors ----------------------------------------------------------

    bool isInInitialDrag() const { return m_state == State::InitialDrag; }
    bool isDragging()      const { return m_state != State::Idle; }
    bool shouldClose()     const { return m_shouldClose; }
    float lastWheel()      const { return m_lastWheel; }

    PlatformWinType& window() { return *m_window; }
    const PlatformWinType& window() const { return *m_window; }

    DockHost& dockHost() { return *m_dockHost; }
    const DockHost& dockHost() const { return *m_dockHost; }

    DockWidget& dock() {
        return *m_docks.at(0);
    }

    DockWidget takeDock() {
        auto d = std::move(m_docks.at(0));
        m_docks.clear();
        return std::move(*d);
    }

    std::unique_ptr<DockWidget> releaseDock(DockWidget* ptr) {
        for (auto it = m_docks.begin(); it != m_docks.end(); ++it) {
            if (it->get() == ptr) {
                auto d = std::move(*it);
                m_docks.erase(it);
                return d;
            }
        }
        return nullptr;
    }

    void adoptDock(std::unique_ptr<DockWidget> d, DockWidget* oldPtr) {
        DockWidget* raw = d.get();
        m_docks.push_back(std::move(d));
        if (m_dockHost) {
            m_dockHost->retargetDock(oldPtr, raw);
        }
    }

private:
    enum class State { InitialDrag, Idle, HeaderDrag, Resizing };
    enum class ResizeDir : uint8_t {
        None,
        Left,
        Right,
        Top,
        Bottom,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight
    };

    void _clearAllHostDrags() {
        for (const auto& e : DockRegistry::instance().entries())
            e.host->updateDrag(0.f, 0.f, 0.f, 0.f, 0, 0, nullptr);
    }

    void _clearHostDragsExcept(DockHost* keep) {
        for (const auto& e : DockRegistry::instance().entries())
            if (e.host != keep)
                e.host->updateDrag(0.f, 0.f, 0.f, 0.f, 0, 0, nullptr);
    }

    std::unique_ptr<PlatformWinType> m_window;
    GpuSurfaceId m_surface{kPrimarySurface};
    std::unique_ptr<DockHost>            m_dockHost;
    std::vector<std::unique_ptr<DockWidget>> m_docks;
    uint32_t     m_winW{kDefaultW}, m_winH{kDefaultH};

    std::function<void(PrimitiveBuffer&)> m_contentRenderHost;
    std::function<void(float, float, bool, bool, float)> m_contentInputHost;

    State m_state{State::Idle};
    bool  m_shouldClose{false};
    bool  m_wasDown{false};
    int   m_dragOffX{0}, m_dragOffY{0};
    FloatingDockOptions m_options;
    float               m_lastWheel{0.0f};

    ResizeDir m_resizeDir{ResizeDir::None};
    bool      m_needsSurfaceResize{false};
    uint32_t  m_startWinW{0}, m_startWinH{0};
    int       m_startWinX{0}, m_startWinY{0};
    int       m_startMouseX{0}, m_startMouseY{0};
};

} // namespace Genesis
