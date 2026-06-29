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

inline namespace jf {

// ============================================================================
// JFloatingDragBehavior — defines the drag behavior of the floating host window:
//
//   Legacy                    — Tab/title drag tears out dock (no global bar).
//                               Drag window via keyboard Alt + Drag.
//   AlwaysGlobalTitleBar      — Always show a global window title bar at the top.
//   ConditionalGlobalTitleBar — Show global window title bar only when nested (docks > 1).
// ============================================================================
enum class JFloatingDragBehavior : uint8_t {
    Legacy,
    AlwaysGlobalTitleBar,
    ConditionalGlobalTitleBar
};

struct JFloatingDockOptions {
    JFloatingDragBehavior dragBehavior{JFloatingDragBehavior::ConditionalGlobalTitleBar};
    bool singleDockDragMovesWindow{true};
    bool tabDragTearsOut{true};
    bool splitDragTearsOut{true};
    bool livePreviewEnabled{true};
};

// ============================================================================
// JFloatingDockWindow — a JDockWidget container that lives in its own OS-level Popup
// window and can be dragged anywhere on the desktop, including over and into
// any JDockHost registered with JDockRegistry.
// ============================================================================

class JFloatingDockWindow {
public:
    static constexpr uint32_t kDefaultW = 340;
    static constexpr uint32_t kDefaultH = 260;
    static constexpr float    kGlobalTitleH = 26.0f;

    struct JPollResult {
        enum class JType {
            None,
            CommitDrop,     // drag ended, commit drop to dropHost
            WantsFloat,     // dock wants to float
        } type{JType::None};

        JDockHost*   dropHost{nullptr};
        JDockWidget* wantsFloatDock{nullptr};
        JRect        wantsFloatRect{};
    };

#if defined(_WIN32)
    using PlatformWinType = JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType = JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    // Construct from a JDockWidget extracted from a JDockHost (WantsFloat path).
    JFloatingDockWindow(JDockWidget dock,
                       int screenX, int screenY,
                       uint32_t winW, uint32_t winH,
                       int dragOffX, int dragOffY,
                       JGpuHal& hal,
                       bool initialDrag = false,
                       JFloatingDockOptions options = {},
                       NativeWinHandleType parentWindow = {})
        : m_window(std::make_unique<PlatformWinType>(
              dock.title().c_str(), winW, winH, screenX, screenY,
#if defined(_WIN32)
              (parentWindow != nullptr) ? JPlatformWindowStyle::Borderless : JPlatformWindowStyle::Popup,
#else
              (parentWindow != 0) ? JPlatformWindowStyle::Borderless : JPlatformWindowStyle::Popup,
#endif
              parentWindow))
        , m_surface(hal.createSurface(m_window->nativeHandle(), winW, winH))
        , m_winW(winW), m_winH(winH)
        , m_state(initialDrag ? JState::InitialDrag : JState::Idle)
        , m_dragOffX(dragOffX)
        , m_dragOffY(dragOffY)
        , m_options(options)
    {

        m_dockHost = std::make_unique<JDockHost>();
        m_dockHost->setLivePreviewEnabled(m_options.livePreviewEnabled);
        m_dockHost->setRootSplit(JSplitDir::Horizontal);
        JDockNodeId leaf = m_dockHost->addLeaf(m_dockHost->rootId(), "floating-leaf", 1.0f);

        auto d = std::make_unique<JDockWidget>(std::move(dock));
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

        JDockRegistry::instance().registerHost(*m_dockHost, screenX, screenY, m_winW, m_winH);
    }

    // Construct from a JTornTabState (JTabBar tear-off path).
    JFloatingDockWindow(JTornTabState state,
                       int screenX, int screenY,
                       int dragOffX, int dragOffY,
                       JGpuHal& hal,
                       JFloatingDockOptions options = {},
                       NativeWinHandleType parentWindow = {})
        : JFloatingDockWindow(
              JDockWidget(std::move(state), 0.f, 0.f,
                         static_cast<float>(kDefaultW), static_cast<float>(kDefaultH)),
              screenX, screenY, kDefaultW, kDefaultH,
              dragOffX, dragOffY, hal, /*initialDrag=*/true, options, parentWindow)
    {}

    JFloatingDockWindow(const JFloatingDockWindow&)            = delete;
    JFloatingDockWindow& operator=(const JFloatingDockWindow&) = delete;

    JFloatingDockWindow(JFloatingDockWindow&& other) noexcept
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

    JFloatingDockWindow& operator=(JFloatingDockWindow&& other) noexcept {
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

    ~JFloatingDockWindow() {
        if (m_dockHost) {
            JDockRegistry::instance().unregisterHost(*m_dockHost);
        }
    }

    bool isGlobalTitleBarVisible() const {
        if (m_options.dragBehavior == JFloatingDragBehavior::AlwaysGlobalTitleBar) return true;
        if (m_options.dragBehavior == JFloatingDragBehavior::ConditionalGlobalTitleBar && m_docks.size() > 1) return true;
        return false;
    }

    void setOptions(const JFloatingDockOptions& options) {
        m_options = options;
        if (m_dockHost) {
            m_dockHost->setLivePreviewEnabled(m_options.livePreviewEnabled);
        }
        float topOffset = isGlobalTitleBarVisible() ? kGlobalTitleH : 0.f;
        m_dockHost->computeLayout({0.f, topOffset, static_cast<float>(m_winW), static_cast<float>(m_winH) - topOffset});
    }

    const JFloatingDockOptions& options() const { return m_options; }

    void setDragBehavior(JFloatingDragBehavior behavior) {
        m_options.dragBehavior = behavior;
        float topOffset = isGlobalTitleBarVisible() ? kGlobalTitleH : 0.f;
        m_dockHost->computeLayout({0.f, topOffset, static_cast<float>(m_winW), static_cast<float>(m_winH) - topOffset});
    }

    JFloatingDragBehavior dragBehavior() const { return m_options.dragBehavior; }

    // -------------------------------------------------------------------------
    // Per-frame update.
    // -------------------------------------------------------------------------
    JPollResult pollAndMove() {
        m_window->pollNativeEvents();
        m_lastWheel = m_window->consumeWheel();

        const bool btnDown = m_window->isLeftButtonDown();
        const bool press   = m_window->consumePress();
        const bool release = m_window->consumeRelease();
        (void)release;

        // Keep bounds current if WM moved or resized the window, or if we are actively resizing
        if (m_state == JState::Idle || m_state == JState::Resizing) {
            float topOffset = isGlobalTitleBarVisible() ? kGlobalTitleH : 0.f;
            int kMinWidth = static_cast<int>(std::ceil(m_dockHost->minWidthNeeded()));
            int kMinHeight = static_cast<int>(std::ceil(m_dockHost->minHeightNeeded() + topOffset));

            if (m_state == JState::Idle) {
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
            JDockRegistry::instance().updateBounds(*m_dockHost, m_window->screenX(), m_window->screenY(), m_winW, m_winH);
        }

        switch (m_state) {
        case JState::InitialDrag: {
            auto [gx, gy] = m_window->globalCursorPos();
            m_window->setPosition(gx - m_dragOffX, gy - m_dragOffY);
            JDockRegistry::instance().updateBounds(*m_dockHost, gx - m_dragOffX, gy - m_dragOffY, m_winW, m_winH);

            JDockHost* hoveredHost = nullptr;
            if (auto hit = JDockRegistry::instance().hitTest(gx, gy)) {
                if (hit->host != m_dockHost.get()) {
                    hoveredHost = hit->host;
                    JDockWidget* draggedDock = m_docks.empty() ? nullptr : m_docks[0].get();
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
                m_state = JState::Idle;
                if (hoveredHost) {
                    _clearHostDragsExcept(hoveredHost);
                    return JPollResult{JPollResult::JType::CommitDrop, hoveredHost, nullptr, {}};
                }
                _clearAllHostDrags();
            }
            return JPollResult{};
        }

        case JState::HeaderDrag: {
            auto [gx, gy] = m_window->globalCursorPos();
            m_window->setPosition(gx - m_dragOffX, gy - m_dragOffY);
            JDockRegistry::instance().updateBounds(*m_dockHost, gx - m_dragOffX, gy - m_dragOffY, m_winW, m_winH);

            JDockHost* hoveredHost = nullptr;
            if (auto hit = JDockRegistry::instance().hitTest(gx, gy)) {
                if (hit->host != m_dockHost.get()) {
                    hoveredHost = hit->host;
                    JDockWidget* draggedDock = m_docks.empty() ? nullptr : m_docks[0].get();
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
                m_state = JState::Idle;
                if (hoveredHost) {
                    _clearHostDragsExcept(hoveredHost);
                    return JPollResult{JPollResult::JType::CommitDrop, hoveredHost, nullptr, {}};
                }
                _clearAllHostDrags();
            }
            return JPollResult{};
        }

        case JState::Resizing: {
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
                case JResizeDir::Right:
                    newW = std::max(kMinWidth, static_cast<int>(m_startWinW) + dx);
                    break;
                case JResizeDir::Bottom:
                    newH = std::max(kMinHeight, static_cast<int>(m_startWinH) + dy);
                    break;
                case JResizeDir::Left: {
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
                case JResizeDir::Top: {
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
                case JResizeDir::TopLeft: {
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
                case JResizeDir::TopRight: {
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
                case JResizeDir::BottomLeft: {
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
                case JResizeDir::BottomRight:
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

            JDockRegistry::instance().updateBounds(*m_dockHost, m_window->screenX(), m_window->screenY(), m_winW, m_winH);

            if (!btnDown) {
                m_state = JState::Idle;
                m_window->setCursor(JPlatformCursor::Default);
            }
            return JPollResult{};
        }

        case JState::Idle: {
            float mx = m_window->mouseX();
            float my = m_window->mouseY();

            // Determine hover / resize zones
            constexpr float kResizeBorder = 8.0f;
            bool left   = mx < kResizeBorder;
            bool right  = mx > static_cast<float>(m_winW) - kResizeBorder;
            bool top    = my < kResizeBorder;
            bool bottom = my > static_cast<float>(m_winH) - kResizeBorder;

            JResizeDir hoverDir = JResizeDir::None;
            if (left && top)          hoverDir = JResizeDir::TopLeft;
            else if (right && top)    hoverDir = JResizeDir::TopRight;
            else if (left && bottom)  hoverDir = JResizeDir::BottomLeft;
            else if (right && bottom) hoverDir = JResizeDir::BottomRight;
            else if (left)            hoverDir = JResizeDir::Left;
            else if (right)           hoverDir = JResizeDir::Right;
            else if (top)             hoverDir = JResizeDir::Top;
            else if (bottom)          hoverDir = JResizeDir::Bottom;

            // Set corresponding cursor
            JPlatformCursor pc = JPlatformCursor::Default;
            switch (hoverDir) {
                case JResizeDir::Left:
                case JResizeDir::Right:       pc = JPlatformCursor::ResizeLeftRight; break;
                case JResizeDir::Top:
                case JResizeDir::Bottom:      pc = JPlatformCursor::ResizeUpDown;    break;
                case JResizeDir::TopLeft:     pc = JPlatformCursor::ResizeTopLeft;    break;
                case JResizeDir::TopRight:    pc = JPlatformCursor::ResizeTopRight;   break;
                case JResizeDir::BottomLeft:  pc = JPlatformCursor::ResizeBottomLeft; break;
                case JResizeDir::BottomRight: pc = JPlatformCursor::ResizeBottomRight;break;
                default:                     pc = JPlatformCursor::Default;          break;
            }
            if (pc == JPlatformCursor::Default) {
                auto hc = m_dockHost->getHoverCursor(mx, my);
                if (hc == JDockHost::JHoverCursor::Horiz)      pc = JPlatformCursor::ResizeLeftRight;
                else if (hc == JDockHost::JHoverCursor::Vert)  pc = JPlatformCursor::ResizeUpDown;
            }
            m_window->setCursor(pc);

            if (hoverDir != JResizeDir::None && press) {
                m_state = JState::Resizing;
                m_resizeDir = hoverDir;
                m_startWinW = m_winW;
                m_startWinH = m_winH;
                m_startWinX = m_window->screenX();
                m_startWinY = m_window->screenY();
                auto [gx, gy] = m_window->globalCursorPos();
                m_startMouseX = gx;
                m_startMouseY = gy;
                return JPollResult{};
            }

            bool altDrag = m_window->isAltDown() && press;
            bool titleBarDrag = isGlobalTitleBarVisible() && press && my < kGlobalTitleH;

            if (altDrag || titleBarDrag) {
                m_state    = JState::HeaderDrag;
                m_dragOffX = static_cast<int>(mx);
                m_dragOffY = static_cast<int>(my);
                return JPollResult{};
            }

            auto ev = m_dockHost->handleMouse(mx, my, press, !btnDown && m_wasDown);

            if (ev) {
                if (ev->type == JDockHost::JDockEvent::JType::WantsFloat) {
                    bool allowTear = true;
                    if (m_docks.size() == 1) {
                        if (m_options.singleDockDragMovesWindow) {
                            m_state    = JState::HeaderDrag;
                            m_dragOffX = static_cast<int>(mx);
                            m_dragOffY = static_cast<int>(my);
                            allowTear  = false;
                        } else {
                            allowTear  = true;
                        }
                    } else {
                        JDockNodeId loc = m_dockHost->findDock(ev->dock);
                        const JDockNode* leaf = m_dockHost->node(loc);
                        bool isTabGroup = (leaf && leaf->tabs.size() > 1);
                        if (isTabGroup) {
                            allowTear = m_options.tabDragTearsOut;
                        } else {
                            allowTear = m_options.splitDragTearsOut;
                        }
                    }

                    if (allowTear) {
                        JDockNodeId loc = m_dockHost->findDock(ev->dock);
                        JRect r = m_dockHost->node(loc)->rect;
                        m_dockHost->removeDock(ev->dock);
                        m_wasDown = btnDown;
                        return JPollResult{JPollResult::JType::WantsFloat, nullptr, ev->dock, r};
                    }
                }
                if (ev->type == JDockHost::JDockEvent::JType::CloseRequested) {
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
            return JPollResult{};
        }
        }
        return JPollResult{};
    }

    // -------------------------------------------------------------------------
    // Render the dock into this window's private GPU surface.
    // -------------------------------------------------------------------------
    void render(JGpuHal& hal, JPrimitiveBuffer& buf) {
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

            if (JTextHelper::hasAtlas()) {
                uint8_t tc[4] = {180, 180, 190, 255};
                float ty = (kGlobalTitleH - JTextHelper::lineHeight()) * 0.5f;
                std::string title = m_docks.empty() ? "Genesis JWindow" : m_docks[0]->title();
                if (m_docks.size() > 1) {
                    title += " (+" + std::to_string(m_docks.size() - 1) + " panels)";
                }
                JTextHelper::pushText(buf, 10.f, ty, title, tc, static_cast<float>(m_winW) - 30.f);
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

    void setContentRenderHost(std::function<void(JPrimitiveBuffer&)> fn) { m_contentRenderHost = std::move(fn); }
    void setContentInputHost(std::function<void(float, float, bool, bool, float)> fn) { m_contentInputHost = std::move(fn); }

    // -------------------------------------------------------------------------
    // Destroy the Vulkan surface.  Call before erasing this object.
    // -------------------------------------------------------------------------
    void destroySurface(JGpuHal& hal) {
        if (m_surface != kPrimarySurface) {
            hal.destroySurface(m_surface);
            m_surface = kPrimarySurface;
        }
    }

    // ---- Accessors ----------------------------------------------------------

    bool isInInitialDrag() const { return m_state == JState::InitialDrag; }
    bool isDragging()      const { return m_state != JState::Idle; }
    bool shouldClose()     const { return m_shouldClose; }
    float lastWheel()      const { return m_lastWheel; }

    PlatformWinType& window() { return *m_window; }
    const PlatformWinType& window() const { return *m_window; }

    JDockHost& dockHost() { return *m_dockHost; }
    const JDockHost& dockHost() const { return *m_dockHost; }

    JDockWidget& dock() {
        return *m_docks.at(0);
    }

    JDockWidget takeDock() {
        auto d = std::move(m_docks.at(0));
        m_docks.clear();
        return std::move(*d);
    }

    std::unique_ptr<JDockWidget> releaseDock(JDockWidget* ptr) {
        for (auto it = m_docks.begin(); it != m_docks.end(); ++it) {
            if (it->get() == ptr) {
                auto d = std::move(*it);
                m_docks.erase(it);
                return d;
            }
        }
        return nullptr;
    }

    void adoptDock(std::unique_ptr<JDockWidget> d, JDockWidget* oldPtr) {
        JDockWidget* raw = d.get();
        m_docks.push_back(std::move(d));
        if (m_dockHost) {
            m_dockHost->retargetDock(oldPtr, raw);
        }
    }

private:
    enum class JState { InitialDrag, Idle, HeaderDrag, Resizing };
    enum class JResizeDir : uint8_t {
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
        for (const auto& e : JDockRegistry::instance().entries())
            e.host->updateDrag(0.f, 0.f, 0.f, 0.f, 0, 0, nullptr);
    }

    void _clearHostDragsExcept(JDockHost* keep) {
        for (const auto& e : JDockRegistry::instance().entries())
            if (e.host != keep)
                e.host->updateDrag(0.f, 0.f, 0.f, 0.f, 0, 0, nullptr);
    }

    std::unique_ptr<PlatformWinType> m_window;
    GpuSurfaceId m_surface{kPrimarySurface};
    std::unique_ptr<JDockHost>            m_dockHost;
    std::vector<std::unique_ptr<JDockWidget>> m_docks;
    uint32_t     m_winW{kDefaultW}, m_winH{kDefaultH};

    std::function<void(JPrimitiveBuffer&)> m_contentRenderHost;
    std::function<void(float, float, bool, bool, float)> m_contentInputHost;

    JState m_state{JState::Idle};
    bool  m_shouldClose{false};
    bool  m_wasDown{false};
    int   m_dragOffX{0}, m_dragOffY{0};
    JFloatingDockOptions m_options;
    float               m_lastWheel{0.0f};

    JResizeDir m_resizeDir{JResizeDir::None};
    bool      m_needsSurfaceResize{false};
    uint32_t  m_startWinW{0}, m_startWinH{0};
    int       m_startWinX{0}, m_startWinY{0};
    int       m_startMouseX{0}, m_startMouseY{0};
};

} // inline namespace jf
