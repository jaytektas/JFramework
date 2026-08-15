#pragma once

#include <j/core/JWindowControls.h>
#include <j/core/JCloseButton.h>
#include <j/core/JPopupItem.h>
#include <j/core/JScrollArea.h>
#include <j/graphics/GpuHal.h>

#if defined(_WIN32)
#include <j/platforms/windows/WindowsPlatformWindow.h>
#else
#include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <memory>
#include <vector>
#include <functional>

inline namespace jf {

class JPopupWindow {
public:
    enum class JStyle { Borderless, Bordered };

    struct JPollResult {
        enum class JType {
            None,
            Dismissed,
        } type{JType::None};
        uint32_t activatedNodeId{0};
        std::string activatedLabel;
    };

#if defined(_WIN32)
    using PlatformWinType = JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType = JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    JPopupWindow(int screenX, int screenY,
                uint32_t width, uint32_t height,
                JGpuHal& hal,
                JStyle style = JStyle::Borderless,
                NativeWinHandleType parentWindow = {},
                void* sharedContext = nullptr)
        : m_winW(width), m_winH(height), m_style(style)
        , m_window(std::make_unique<PlatformWinType>(
              "Popup", width, height, screenX, screenY,
              JPlatformWindowStyle::Popup, parentWindow,
#if defined(_WIN32)
              static_cast<HINSTANCE>(sharedContext)
#else
              static_cast<xcb_connection_t*>(sharedContext)
#endif
          ))
        , m_surface(hal.createSurface(m_window->nativeHandle(), 0, 0))
    {
        m_root = m_graph.createNode("PopupRoot");
        auto& l = m_graph.getLayout(m_root);
        l.boundingBox = { 0.f, 0.f, static_cast<float>(width), static_cast<float>(height) };
    }

    ~JPopupWindow() {
#if defined(_WIN32)
        ReleaseCapture();
#else
        if (m_hasPointerGrab) {
            xcb_connection_t* conn = m_window->nativeConnection();
            xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
            xcb_flush(conn);
        }
#endif
    }

    template<typename W, typename... Args>
    W* add(Args&&... args) {
        auto uptr = std::make_unique<W>(m_graph, std::forward<Args>(args)...);
        W* ptr = uptr.get();
        m_graph.addChild(m_root, ptr->getNodeId());
        m_widgets.push_back(std::move(uptr));
        return ptr;
    }

    void computeNaturalHeight() {
        m_graph.computeMinSize(m_root);
        float h = m_graph.getLayoutConst(m_root).minHeight;
        if (m_style == JStyle::Bordered) h += m_kBorderPad * 2.f;
        m_winH = static_cast<uint32_t>(std::max(1.f, h));
        // WIDTH from content too: the popup was created at a caller-chosen width (menus: a flat 180px) and
        // only ever had its height fitted, so any item wider than that guess was clipped. Grow to the
        // widest child; never shrink below the requested width.
        float w = m_graph.getLayoutConst(m_root).minWidth;
        if (m_style == JStyle::Bordered) w += m_kBorderPad * 2.f;
        m_winW = std::max(m_winW, static_cast<uint32_t>(std::max(1.f, w)));
        m_window->setSize(m_winW, m_winH);

        auto& l = m_graph.getLayout(m_root);
        l.boundingBox.width = static_cast<float>(m_winW);
        l.boundingBox.height = static_cast<float>(m_winH);
        m_graph.invalidateNode(m_root, DirtySelf);
        m_graph.computeLayout(m_root, { static_cast<float>(m_winW), static_cast<float>(m_winW), 0.0f, 100000.f });
    }

    // Wrap into COLUMNS when a single column would not fit the room available. A menu of a hundred
    // items is not a placement problem — flip, slide and clamp all do the right thing and it is still
    // taller than the screen — so the shape has to change. Fill is column-major: the list keeps running
    // top-to-bottom, it just starts a new column when it reaches the bottom, so nothing moves except
    // what genuinely could not fit. Returns the column count actually used (1 = unchanged).
    int wrapToHeight(uint32_t availH) {
        computeNaturalHeight();                       // single-column natural size first
        if (availH == 0 || m_winH <= availH) return 1;
        const int n = static_cast<int>(m_graph.getChildren(m_root).size());
        if (n <= 1) return 1;
        const int cols = std::max(2, static_cast<int>((m_winH + availH - 1) / availH));
        auto& l = m_graph.getLayout(m_root);
        l.mode            = JLayoutMode::Grid;
        l.columns         = cols;
        l.gridColumnMajor = true;                     // down each column, not across each row
        // Equal columns, each as wide as the single-column natural width — the widest item still fits
        // and every column lines up, which is what makes a wrapped list scannable.
        setSize(static_cast<uint32_t>(m_winW) * static_cast<uint32_t>(cols), availH);
        m_graph.computeMinSize(m_root);
        const float h = m_graph.getLayoutConst(m_root).minHeight
                      + (m_style == JStyle::Bordered ? m_kBorderPad * 2.f : 0.f);
        setSize(m_winW, static_cast<uint32_t>(std::max(1.f, std::min<float>(h, static_cast<float>(availH)))));
        return cols;
    }

    // Size the popup EXPLICITLY, for content that is deliberately taller than its window — a list clamped
    // to the room on screen, scrolling inside. computeNaturalHeight() is the other case: fit the window to
    // the content. Same bookkeeping either way, so the root lays out against the real window size.
    void setSize(uint32_t w, uint32_t h) {
        m_winW = std::max(1u, w);
        m_winH = std::max(1u, h);
        m_window->setSize(m_winW, m_winH);
        auto& l = m_graph.getLayout(m_root);
        l.boundingBox.width  = static_cast<float>(m_winW);
        l.boundingBox.height = static_cast<float>(m_winH);
        m_graph.invalidateNode(m_root, DirtySelf);
        m_graph.computeLayout(m_root, { static_cast<float>(m_winW), static_cast<float>(m_winW), 0.0f, 100000.f });
    }

    JSceneGraph& graph()  { return m_graph; }
    NodeId      root()   const { return m_root; }
    uint32_t    width()  const { return m_winW; }
    uint32_t    height() const { return m_winH; }
    JStyle       style()  const { return m_style; }

    JPollResult pollEvents(JGpuHal& hal) {
        m_window->pollNativeEvents();
        JPollResult out{};

        if (m_window->shouldClose() || m_window->consumeFocusLost()) {
            out.type = JPollResult::JType::Dismissed;
            return out;
        }

        _ensureGrab();

        float mx = m_window->mouseX();
        float my = m_window->mouseY();
        bool  pressed  = m_window->consumePress();
        bool  released = m_window->consumeRelease();

        // Mouse movement cancels keyboard selection — for a MENU. A list with an explicit nav list (a combo
        // box) behaves the way every other combo box does instead: the pointer MOVES the highlight to
        // whatever it is over, and leaves it alone when it is over nothing. Clearing it outright meant the
        // cursor resting on the list at open — where the click that opened it left the pointer — threw away
        // the selection the list had just opened on, so the first Down key started from the top.
        // …but NOT while the pointer is on the scrollbar, or dragging it. The row behind the bar is not the
        // row you are pointing at, and in a combo list that highlight is the selection Enter commits — so a
        // click on the scrollbar looked exactly like picking whatever entry happened to sit behind it.
        if ((mx != m_lastPollMx || my != m_lastPollMy) && !_pointerOnScrollbar(mx, my)) {
            if (m_navItems.empty()) {
                m_keyNavIdx = -1;
            } else {
                for (int i = 0; i < static_cast<int>(m_navItems.size()); ++i) {
                    JWidget* w = m_navItems[i];
                    if (!w || !w->isVisible()) continue;
                    const auto& bb = m_graph.getLayoutConst(w->getNodeId()).boundingBox;
                    if (mx >= bb.x && mx < bb.x + bb.width && my >= bb.y && my < bb.y + bb.height) {
                        if (m_keyNavIdx != i) { m_keyNavIdx = i; _applyNavHighlight(); }
                        break;
                    }
                }
            }
            m_lastPollMx = mx;
            m_lastPollMy = my;
        }

        bool inside = (mx >= 0.f && mx < static_cast<float>(m_winW) &&
                       my >= 0.f && my < static_cast<float>(m_winH));

        if (!inside && pressed) {
            out.type = JPollResult::JType::Dismissed;
            return out;
        }

        uint32_t clickedId = 0;
        std::string clickedLabel;

        // The wheel, as in poll() above.
        if (const float wheel = m_window->consumeWheel();
            wheel != 0.f && mx >= 0.f && mx < static_cast<float>(m_winW)
                         && my >= 0.f && my < static_cast<float>(m_winH))
            for (auto& w : m_widgets)
                if (w->isVisible() && w->handleScroll(mx, my, wheel)) break;

        for (auto& w : m_widgets) {
            if (!w->isVisible()) continue;
            w->handleMouseMove(mx, my);
            if (pressed) {
                w->handleMousePress(mx, my);
                if (w->hitTest(mx, my)) {
                    clickedId = w->getNodeId();
                    if (auto* pi = dynamic_cast<JPopupItem*>(w.get())) {
                        clickedLabel = pi->label();
                    }
                }
            }
            if (released) {
                w->handleMouseRelease(mx, my);
            }
        }

        if (clickedId != 0) {
            out.activatedNodeId = clickedId;
            out.activatedLabel  = clickedLabel;
        }

        // Keyboard navigation — arrow keys, Enter, Escape. Non-nav keys (text, backspace) are
        // forwarded to widgets' handleKeyEvent so editable popup content (e.g. a colour picker's
        // hex field) works; the first widget that consumes the key wins.
        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            using K = JKeyEvent::JKey;
            if (ke.key == K::Escape) {
                out.type = JPollResult::JType::Dismissed;
                return out;
            }
            if (ke.key == K::Up)   { _navStep(-1); continue; }
            if (ke.key == K::Down) { _navStep(1);  continue; }
            if (ke.key != K::Return && ke.key != K::Space) {
                for (auto& w : m_widgets) { if (w->isVisible() && w->handleKeyEvent(ke)) break; }
                continue;
            }
            if ((ke.key == K::Return || ke.key == K::Space) && !m_navItems.empty()) {
                // Choose the highlighted entry. JPopupItem activates on RELEASE while pressed, so a
                // synthesised press alone would highlight and never choose.
                if (m_keyNavIdx >= 0 && m_keyNavIdx < static_cast<int>(m_navItems.size()))
                    if (auto* pi = dynamic_cast<JPopupItem*>(m_navItems[m_keyNavIdx])) {
                        pi->onActivated.emit();
                        out.activatedNodeId = pi->getNodeId();
                    }
                continue;
            }
            if (ke.key == K::Return || ke.key == K::Space) {
                if (m_keyNavIdx >= 0 && m_keyNavIdx < static_cast<int>(m_widgets.size())) {
                    auto* w = m_widgets[m_keyNavIdx].get();
                    if (w->isVisible() && w->isEnabled()) {
                        const auto& bb = m_graph.getLayoutConst(w->getNodeId()).boundingBox;
                        w->handleMousePress(bb.x + 1.f, bb.y + 1.f);
                        out.activatedNodeId = w->getNodeId();
                    }
                }
            }
        }

        return out;
    }

    // ---- Managed-stack input (driven by JMenuRuntime) --------------------------------
    // A cascade of menu popups can't each hold the X pointer grab — only one window can, so
    // the child would freeze the parent and steal its clicks. Instead the runtime keeps ONE
    // grab on the ROOT popup, reads the global cursor from it, and drives every popup in the
    // stack via these methods. Dismiss is decided by the runtime (press outside ALL popups).

    // Root popup: pump native events + ensure the single pointer grab. Returns true if this
    // popup should dismiss the whole stack (window closed / focus lost).
    // Pump the ROOT popup of a menu stack and hold the pointer grab. Returns true only when the WINDOW
    // MANAGER closed it.
    //
    // Focus is deliberately NOT a dismissal signal here. A menu stack is many X11 windows but ONE input
    // domain: this popup grabs the pointer and the runtime drives every popup from that single stream. Under
    // a grab, X focus says nothing about what the user meant -- and it actively lies, because opening a
    // SUBMENU moves focus off this window. Treating that as "the user clicked away" made a cascade eat
    // itself: parent opens, child opens, parent sees focus loss, the stack closes, hover reopens it, forever.
    // The menu bar was then unusable, because an open menu makes the runtime swallow all main-window input.
    //
    // Every real dismissal already reaches the runtime through the grab: a press outside every popup (which
    // is also how clicking ANOTHER APPLICATION arrives, since the grab routes it to us), Escape, or this
    // shouldClose. The flag is still consumed so it cannot pile up.
    bool pumpAndGrab() {
        m_window->pollNativeEvents();
        (void)m_window->consumeFocusLost();          // consumed, never acted on -- see above
        if (m_window->shouldClose()) return true;
        _ensureGrab();
        return false;
    }

    // Child popup: pump native events only (no grab — the root owns it) so the surface stays
    // alive and repaints. Mark it viewable: isViewable() is normally gated on holding the
    // pointer grab, but a managed child never grabs, so without this it would never render.
    void pumpManaged() { m_window->pollNativeEvents(); m_focusSet = true; }

    std::pair<int,int> globalCursor() const { return m_window->globalCursorPos(); }
    bool takePress()   { return m_window->consumePress(); }
    bool takeRelease() { return m_window->consumeRelease(); }
    std::vector<JKeyEvent> takeKeys() { return m_window->consumeAllKeys(); }

    // Is the global point inside this popup's screen rect?
    bool containsGlobal(int gx, int gy) const {
        const int lx = gx - m_window->screenX(), ly = gy - m_window->screenY();
        return lx >= 0 && lx < static_cast<int>(m_winW) && ly >= 0 && ly < static_cast<int>(m_winH);
    }

    // Drive this popup's widgets with the cursor at (mx,my) popup-local coords. Fires hover
    // (opens submenus) and, on press, activation (onTriggered). Returns clicked info.
    JPollResult driveInput(float mx, float my, bool pressed, bool released) {
        JPollResult out{};
        uint32_t    clickedId = 0;
        std::string clickedLabel;
        // The wheel, as in poll() above.
        if (const float wheel = m_window->consumeWheel();
            wheel != 0.f && mx >= 0.f && mx < static_cast<float>(m_winW)
                         && my >= 0.f && my < static_cast<float>(m_winH))
            for (auto& w : m_widgets)
                if (w->isVisible() && w->handleScroll(mx, my, wheel)) break;

        for (auto& w : m_widgets) {
            if (!w->isVisible()) continue;
            w->handleMouseMove(mx, my);
            if (pressed) {
                w->handleMousePress(mx, my);
                if (w->hitTest(mx, my)) {
                    clickedId = w->getNodeId();
                    if (auto* pi = dynamic_cast<JPopupItem*>(w.get())) clickedLabel = pi->label();
                }
            }
            if (released) w->handleMouseRelease(mx, my);
        }
        if (clickedId != 0) { out.activatedNodeId = clickedId; out.activatedLabel = clickedLabel; }
        return out;
    }

    // Keyboard-navigate the popup: call from outside if the platform routes keys here.
    // Returns true if the key was consumed.
    bool handleKeyNav(const JKeyEvent& ke) {
        if (!ke.pressed) return false;
        using K = JKeyEvent::JKey;
        if (ke.key == K::Up)   { _navStep(-1); return true; }
        if (ke.key == K::Down) { _navStep(1);  return true; }
        return false;
    }

    int keyNavIdx() const { return m_keyNavIdx; }

    // THE LIST THE KEYBOARD WALKS, when it is not simply this popup's top-level widgets — a combo box's
    // entries live inside a scroll area, so stepping over the popup's children would step over exactly one
    // thing. `current` is the entry to open on (a combo opens on its selection, not on the top of the
    // list), and `reveal` scrolls an entry into view as the keyboard reaches it.
    void setKeyNavList(std::vector<JWidget*> items, int current, std::function<void(JWidget*)> reveal = {}) {
        m_navItems = std::move(items);
        m_navReveal = std::move(reveal);
        m_keyNavIdx = (current >= 0 && current < static_cast<int>(m_navItems.size())) ? current : -1;
        _applyNavHighlight();
        if (m_keyNavIdx >= 0 && m_navReveal) m_navReveal(m_navItems[m_keyNavIdx]);
    }

    // Poll in floating mode: no grab, no dismiss-on-outside, JTearOffHandle
    // area (top kFloatHandleH pixels) drags the window using xcb_query_pointer
    // so no pointer grab is required. The top-right 16×16 corner is a close btn.
    enum class JFloatPollResult { Alive, Close };
    JFloatPollResult pollFloating() {
        m_window->pollNativeEvents();
        // A polled floating popup is live → mark it viewable so the render loop draws it. Mirrors
        // pumpManaged() for modal children. Without this a floating menu's SUBMENU (created straight into
        // m_floating, never via the modal stack) keeps m_focusSet=false, so isViewable() is false and its
        // render() is never called — the window maps (and takes clicks) but its surface is never drawn: the
        // "black submenu of a torn-off menu" bug. A torn-off ROOT only worked because it carried m_focusSet
        // over from its modal life.
        m_focusSet = true;
        if (m_window->shouldClose()) return JFloatPollResult::Close;

        float mx = m_window->mouseX();
        float my = m_window->mouseY();
        bool pressed  = m_window->consumePress();
        bool released = m_window->consumeRelease();

        // On the very first poll after tearoff the press event has already been
        // consumed by pollEvents().  If the button is still held, start dragging
        // immediately so the window follows the cursor with no extra click needed.
        if (!m_floatFirstPollDone) {
            m_floatFirstPollDone = true;
#if !defined(_WIN32)
            if (m_window->isLeftButtonDown()) {
                auto [gx, gy] = m_window->globalCursorPos();
                m_floatDragging   = true;
                m_floatDragStartX = gx;
                m_floatDragStartY = gy;
                m_floatWinStartX  = m_window->screenX();
                m_floatWinStartY  = m_window->screenY();
            }
#endif
        }

        bool inHandle = (my >= 0.f && my <= kFloatHandleH);
        float closeX  = static_cast<float>(m_winW) - kFloatHandleH - 1.f;
        bool inClose  = inHandle && (mx >= closeX && mx < closeX + kFloatHandleH);

        // Close button — must be checked before drag so the click isn't consumed as a drag.
        if (pressed && inClose) return JFloatPollResult::Close;

        // Drag-to-move: press in the handle strip (but not close button) starts a move.
        // Consuming the press prevents it reaching JTearOffHandle (would fire onTornOff again).
        if (pressed && inHandle) {
#if !defined(_WIN32)
            auto [gx, gy] = m_window->globalCursorPos();
            m_floatDragging   = true;
            m_floatDragStartX = gx;
            m_floatDragStartY = gy;
            m_floatWinStartX  = m_window->screenX();
            m_floatWinStartY  = m_window->screenY();
#endif
            pressed = false;  // consumed — don't pass to widgets
        }

        if (m_floatDragging) {
#if !defined(_WIN32)
            if (m_window->isLeftButtonDown()) {
                auto [gx, gy] = m_window->globalCursorPos();
                int nx = m_floatWinStartX + (gx - m_floatDragStartX);
                int ny = m_floatWinStartY + (gy - m_floatDragStartY);
                m_window->setPosition(nx, ny);
            } else {
                m_floatDragging = false;
            }
#endif
        }

        // Route input to all widgets normally.
        for (auto& w : m_widgets) {
            if (!w->isVisible()) continue;
            w->handleMouseMove(mx, my);
            if (pressed)  w->handleMousePress(mx, my);
            if (released) w->handleMouseRelease(mx, my);
        }
        return JFloatPollResult::Alive;
    }

    void render(JGpuHal& hal, JPrimitiveBuffer& buf) {
        if (m_surfaceW != m_winW || m_surfaceH != m_winH) {
            hal.resizeSurface(m_surface, m_winW, m_winH);
            m_surfaceW = m_winW;
            m_surfaceH = m_winH;
        }

        buf.clear();  // each popup renders to its own surface — start with a clean buffer

        if (m_style == JStyle::Bordered) {
            buf.pushRectangle(0.f, 0.f,
                              static_cast<float>(m_winW), static_cast<float>(m_winH),
                              Colors::PopupBg, 6.0f, 1.0f, Colors::Border);
        } else {
            buf.pushRectangle(0.f, 0.f,
                              static_cast<float>(m_winW), static_cast<float>(m_winH),
                              Colors::PopupInnerBg, 0.0f);
        }

        for (auto& w : m_widgets) {
            if (w->isVisible())
                w->populateRenderPrimitives(buf);
        }

        // Keyboard-nav selection highlight (drawn on top of normal hover)
        if (m_keyNavIdx >= 0 && m_keyNavIdx < static_cast<int>(m_widgets.size())) {
            auto* w = m_widgets[m_keyNavIdx].get();
            if (w->isVisible()) {
                const auto& bb = m_graph.getLayoutConst(w->getNodeId()).boundingBox;
                uint8_t sel[4]    = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 40};
                uint8_t border[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 160};
                buf.pushRectangle(bb.x, bb.y, bb.width, bb.height, sel, 4.f, 1.f, border);
            }
        }

        // This popup's own widgets are its roots, and its own dwell state: a popup renders its own
        // frame, so sharing either with the window underneath would have them overwrite each other.
        {
            std::vector<JWidget*> roots;
            roots.reserve(m_widgets.size());
            for (auto& w : m_widgets) if (w) roots.push_back(w.get());
            JWidget::renderTooltips(buf, m_tooltipHover, roots,
                                    m_window->mouseX(), m_window->mouseY(),
                                    static_cast<float>(m_winW), static_cast<float>(m_winH));
        }

        // Close button — same style as JDockWidget title-bar close button.
        if (m_showCloseButton) {
            const JRect cr = JCloseButton::rectFor({0.f, 0.f, static_cast<float>(m_winW), kFloatHandleH});
            const bool hov = JCloseButton::hit(cr, m_window->mouseX(), m_window->mouseY());
            JCloseButton::draw(buf, cr, hov);
        }

        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
    }

    void destroySurface(JGpuHal& hal) {
        if (m_surface != kPrimarySurface) {
            hal.destroySurface(m_surface);
            m_surface = kPrimarySurface;
        }
    }

    void releasePointerGrab() {
#if defined(_WIN32)
        ReleaseCapture();
#else
        if (m_hasPointerGrab) {
            xcb_ungrab_pointer(m_window->nativeConnection(), XCB_CURRENT_TIME);
            xcb_flush(m_window->nativeConnection());
            m_hasPointerGrab = false;
        }
#endif
    }

    void enableCloseButton() { m_showCloseButton = true; }

    PlatformWinType&       window()       { return *m_window; }
    const PlatformWinType& window() const { return *m_window; }
    bool                   isViewable() const { return m_focusSet; }
    const std::vector<std::unique_ptr<JWidget>>& widgets() const { return m_widgets; }

private:
    static constexpr float m_kBorderPad  = 4.f;
    static constexpr float kFloatHandleH = 16.f;

    JStyle     m_style;
    uint32_t  m_winW, m_winH;

    JSceneGraph m_graph;
    NodeId     m_root{0};
    std::vector<std::unique_ptr<JWidget>> m_widgets;

    std::unique_ptr<PlatformWinType> m_window;
    GpuSurfaceId m_surface{kPrimarySurface};
    bool m_focusSet{false};
    bool m_hasPointerGrab{false};
    uint32_t m_surfaceW{0}, m_surfaceH{0};
    bool  m_showCloseButton{false};
    bool  m_floatFirstPollDone{false};
    bool  m_floatDragging{false};
    int   m_floatDragStartX{0}, m_floatDragStartY{0};
    int   m_floatWinStartX{0},  m_floatWinStartY{0};
    int   m_keyNavIdx{-1};
    JTooltipHover                  m_tooltipHover; // this popup's own hover dwell
    std::vector<JWidget*>          m_navItems;    // explicit keyboard list (combo entries), else m_widgets
    std::function<void(JWidget*)>  m_navReveal;   // scroll an entry into view as the keyboard reaches it
    float m_lastPollMx{-1.f}, m_lastPollMy{-1.f};

    // Move the keyboard selection by dir (+1 = down, -1 = up), skipping
    // non-focusable items (separators etc.) and wrapping around.
    // Take the modal pointer grab once. Retried next frame if it wasn't acquired — common
    // when the popup opens on a button PRESS the parent still holds an implicit grab for;
    // without the retry the popup never captures the pointer and its hover/clicks stall.
    void _ensureGrab() {
        if (m_focusSet) return;
#if defined(_WIN32)
        SetFocus(m_window->nativeWindow());
        SetCapture(m_window->nativeWindow());
        m_hasPointerGrab = true;
#else
        xcb_connection_t* conn = m_window->nativeConnection();
        xcb_window_t      wid  = m_window->nativeWindow();
        xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, wid, XCB_CURRENT_TIME);
        auto grabCookie = xcb_grab_pointer(conn, 0, wid,
            XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(conn);
        auto* grabReply = xcb_grab_pointer_reply(conn, grabCookie, nullptr);
        m_hasPointerGrab = grabReply && grabReply->status == XCB_GRAB_STATUS_SUCCESS;
        free(grabReply);
#endif
        m_focusSet = m_hasPointerGrab;
    }

    // True while the pointer is over (or dragging) a scroll area's scrollbar.
    bool _pointerOnScrollbar(float mx, float my) const {
        for (const auto& w : m_widgets) {
            if (const auto* sa = dynamic_cast<const JScrollArea*>(w.get()))
                if (sa->isDraggingScroll() || sa->pointInScrollbar(mx, my)) return true;
        }
        return false;
    }

    void _applyNavHighlight() {
        for (int i = 0; i < static_cast<int>(m_navItems.size()); ++i)
            if (auto* pi = dynamic_cast<JPopupItem*>(m_navItems[i])) pi->setHighlighted(i == m_keyNavIdx);
    }

    void _navStep(int dir) {
        if (!m_navItems.empty()) {                       // an explicit list: walk THAT
            const int n = static_cast<int>(m_navItems.size());
            m_keyNavIdx = m_keyNavIdx < 0 ? (dir > 0 ? 0 : n - 1)
                                          : ((m_keyNavIdx + dir) % n + n) % n;
            _applyNavHighlight();
            if (m_navReveal) m_navReveal(m_navItems[m_keyNavIdx]);
            return;
        }
        int n = static_cast<int>(m_widgets.size());
        if (n == 0) return;
        int next = m_keyNavIdx < 0 ? (dir > 0 ? -1 : n) : m_keyNavIdx;
        for (int i = 0; i < n; ++i) {
            next = ((next + dir) % n + n) % n;
            if (m_widgets[next]->isVisible() && m_widgets[next]->isFocusable()) {
                m_keyNavIdx = next;
                return;
            }
        }
    }
};

} // inline namespace jf
