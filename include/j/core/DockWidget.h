#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>
#include <algorithm>
#include <cmath>
#include "BaseWidgets.h"         // TornTabState, Colors, PrimitiveBuffer

inline namespace jf {

// ============================================================================
// JDockWidget — a floating panel that renders at an absolute screen position,
// independent of the JSceneGraph layout.
//
// Lifecycle:
//   1. Created fresh by the app (general panel).
//   2. Created from a JTornTabState (torn-off tab) — carries re-dock provenance.
//   3. Destroyed when the user presses Close, or when re-docked.
//
// The JDockWidget does NOT inherit JWidget — it lives outside the layout tree
// and positions itself via explicit x/y coordinates.  Content is rendered by
// the owner (app/catalog) which calls contentWidget->populateRenderPrimitives
// then offsets by contentArea().
// ============================================================================

class JDockWidget : public JIAIState {
public:
    // All live DockWidgets register here so JGuiApplication::publishSemanticSnapshot()
    // can include floating panels in the AI bus alongside regular widgets.
    inline static std::vector<JDockWidget*> s_activeDocks;

    static constexpr float TITLE_H     = 30.0f;
    static constexpr float BTN_SZ      = 16.0f;  // close / pin button size
    static constexpr float BORDER_R    = 8.0f;
    static constexpr float SHADOW_OFF  = 4.0f;
    static constexpr float SNAP_DIST   = 14.0f;

    // Bit positions for setAllowedDrops().
    // These match the ordinal values of JDropPos (Center=0, Left=1, Right=2,
    // Top=3, Bottom=4) so DockManager can compute the bit as 1<<JDropPos.
    static constexpr uint8_t kDropCenter = 1 << 0;
    static constexpr uint8_t kDropLeft   = 1 << 1;
    static constexpr uint8_t kDropRight  = 1 << 2;
    static constexpr uint8_t kDropTop    = 1 << 3;
    static constexpr uint8_t kDropBottom = 1 << 4;
    static constexpr uint8_t kDropSides  = kDropLeft | kDropRight | kDropTop | kDropBottom;
    static constexpr uint8_t kDropAll    = 0x1Fu;

    // --- Constructors ---

    JDockWidget(std::string title, float x, float y, float w, float h)
        : m_title(std::move(title)), m_x(x), m_y(y), m_w(w), m_h(h)
    { s_activeDocks.push_back(this); }

    JDockWidget(JTornTabState state, float x, float y, float w = 320.0f, float h = 240.0f)
        : m_title(state.title), m_x(x), m_y(y), m_w(w), m_h(h)
        , m_tornState(std::move(state))
    { s_activeDocks.push_back(this); }

    ~JDockWidget() {
        auto& v = s_activeDocks;
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
    }

    // Move transfers registry entry
    JDockWidget(JDockWidget&& o) noexcept
        : m_title(std::move(o.m_title)), m_tag(std::move(o.m_tag))
        , m_x(o.m_x), m_y(o.m_y), m_w(o.m_w), m_h(o.m_h)
        , m_tornState(std::move(o.m_tornState))
        , m_minW(o.m_minW), m_minH(o.m_minH), m_maxW(o.m_maxW), m_maxH(o.m_maxH)
        , m_floatable(o.m_floatable), m_tabifiable(o.m_tabifiable)
        , m_allowedDrops(o.m_allowedDrops)
        , m_dockable(o.m_dockable), m_closeable(o.m_closeable)
        , m_pinnable(o.m_pinnable), m_resizable(o.m_resizable)
        , m_titleVisible(o.m_titleVisible)
        , m_acceptLeafLabels(std::move(o.m_acceptLeafLabels))
        , m_rejectLeafLabels(std::move(o.m_rejectLeafLabels))
        , m_dragging(o.m_dragging), m_dragOffX(o.m_dragOffX), m_dragOffY(o.m_dragOffY)
        , m_resizing(o.m_resizing), m_resizeInitW(o.m_resizeInitW), m_resizeInitH(o.m_resizeInitH)
        , m_resizeAnchorX(o.m_resizeAnchorX), m_resizeAnchorY(o.m_resizeAnchorY)
        , m_closeRequested(o.m_closeRequested), m_pinned(o.m_pinned)
        , m_hoverClose(o.m_hoverClose), m_hoverPin(o.m_hoverPin), m_hoverResize(o.m_hoverResize)
        , onRenderContent(std::move(o.onRenderContent))
        , onInputContent(std::move(o.onInputContent))
        , m_content(o.m_content)
    {
        // Replace old pointer in registry with this
        auto& v = s_activeDocks;
        auto it = std::find(v.begin(), v.end(), &o);
        if (it != v.end()) *it = this;
        else v.push_back(this);
    }
    JDockWidget& operator=(JDockWidget&&) = delete;

    JDockWidget(const JDockWidget&)            = delete;
    JDockWidget& operator=(const JDockWidget&) = delete;

    // --- Geometry ---

    float x()      const { return m_x; }
    float y()      const { return m_y; }
    float width()  const { return m_w; }
    float height() const { return m_h; }

    // Rectangle the content widget should render into
    JRect contentArea() const {
        float topOff = m_titleVisible ? TITLE_H : 0.f;
        return { m_x + 1.0f, m_y + topOff, m_w - 2.0f, m_h - topOff - 1.0f };
    }

    void setPosition(float x, float y) { m_x = x; m_y = y; }
    void setSize(float w, float h)     { m_w = w; m_h = h; }

    // Snap towards window edges if within SNAP_DIST pixels
    void snapToWindow(float windowW, float windowH) {
        if (m_x             < SNAP_DIST)          m_x = 0.0f;
        if (m_y             < SNAP_DIST)          m_y = 0.0f;
        if (m_x + m_w > windowW - SNAP_DIST)     m_x = windowW - m_w;
        if (m_y + m_h > windowH - SNAP_DIST)     m_y = windowH - m_h;
        m_x = std::max(0.0f, m_x);
        m_y = std::max(0.0f, m_y);
    }

    // --- JState queries ---

    bool closeRequested() const { return m_closeRequested; }
    bool isPinned()       const { return m_pinned; }
    bool isDragging()     const { return m_dragging; }

    bool hasTornState()               const { return m_tornState.has_value(); }
    JTornTabState& tornState()               { return *m_tornState; }
    const JTornTabState& tornState()   const { return *m_tornState; }

    const std::string& title() const { return m_title; }

    // Affinity tag used by JDockHost to accept/reject docks per leaf zone.
    void setTag(std::string t) { m_tag = std::move(t); }
    const std::string& tag() const { return m_tag; }

    // --- Size constraints ---
    // Applied during floating resize and respected by JDockHost layout.

    void  setMinSize(float w, float h) { m_minW = w; m_minH = h; }
    void  setMaxSize(float w, float h) { m_maxW = w; m_maxH = h; }
    float minW() const { return m_minW; }
    float minH() const { return m_minH; }
    float maxW() const { return m_maxW; }
    float maxH() const { return m_maxH; }

    // --- Behaviour flags ---

    // Can be torn out of the JDockHost into a floating OS window.
    void setFloatable(bool v)  { m_floatable  = v; }
    bool isFloatable()  const  { return m_floatable; }

    // Can share a leaf tab bar with other docks.  A non-tabifiable dock must
    // always occupy its own leaf (Center drops are blocked when the leaf already
    // has tabs, and other docks can't Center-drop onto a leaf it occupies).
    void setTabifiable(bool v) { m_tabifiable = v; }
    bool isTabifiable() const  { return m_tabifiable; }

    // Bitmask of allowed drop positions (use kDrop* constants).
    // Arrows for disallowed positions still render but are dimmed and inert.
    void  setAllowedDrops(uint8_t mask) { m_allowedDrops = mask; }
    bool  allowsDrop(uint8_t bit) const { return (m_allowedDrops & bit) != 0u; }
    uint8_t allowedDrops() const        { return m_allowedDrops; }

    // Whether drop-zones are ever offered when dragging this dock into a JDockHost.
    // When false the dock floats only — it never shows snap arrows.
    void setDockable(bool v)        { m_dockable      = v; }
    bool isDockable()  const        { return m_dockable; }

    // Show/hide the close button in the title bar.
    void setCloseable(bool v)       { m_closeable     = v; }
    bool isCloseable() const        { return m_closeable; }

    // Show/hide the pin button in the title bar.
    void setPinnable(bool v)        { m_pinnable      = v; }
    bool isPinnable() const         { return m_pinnable; }

    // Allow/disallow the drag-resize handle in the bottom-right corner.
    void setResizable(bool v)       { m_resizable     = v; }
    bool isResizable() const        { return m_resizable; }

    // Hide the title bar entirely — content fills the whole dock panel.
    // Useful for toolbar-style panels.  Also disables drag-to-move (pinned=true).
    void setTitleBarVisible(bool v) { m_titleVisible  = v; }
    bool isTitleBarVisible() const  { return m_titleVisible; }

    // Dock-side affinity — which leaf labels this dock is willing to join.
    // Independent of the leaf's own JDockAffinityRule (both must pass).
    // Empty accept list = accept any leaf label.
    void setAcceptLeafLabels(std::vector<std::string> v) { m_acceptLeafLabels = std::move(v); }
    void setRejectLeafLabels(std::vector<std::string> v) { m_rejectLeafLabels = std::move(v); }
    bool acceptsLeafLabel(std::string_view label) const {
        for (const auto& r : m_rejectLeafLabels) if (r == label) return false;
        if (m_acceptLeafLabels.empty()) return true;
        for (const auto& a : m_acceptLeafLabels) if (a == label) return true;
        return false;
    }

    // --- Content hooks ---
    // Let the framework host a dock's inner content wherever the dock lives — inline in a
    // JDockHost leaf, or torn out into a floating window. The hooks travel WITH the dock
    // (moved by the move-ctor), so tear-out / re-dock carries the content automatically and
    // the app never re-wires anything. Without hooks a dock shows only its chrome.
    //   onRenderContent(buf, contentRect): draw into contentRect (host-local coordinates).
    //   onInputContent(mx,my,pressed,released,wheel): route input (host-local coordinates).
    std::function<void(JPrimitiveBuffer&, const JRect&)> onRenderContent;
    std::function<void(float, float, bool, bool, float)> onInputContent;

    // --- Hosted content widget ---
    // The primary content path: a framework widget tree the framework lays out, renders, and
    // routes input to. A dock is thus a window that manages its own content — identically whether
    // it's docked inline in a host leaf or torn out into a float. If no content widget is set, the
    // onRenderContent/onInputContent paint hooks above are used instead (custom studio-drawn
    // controls). The pointer is non-owning; the app owns the widget (as it owns the dock).
    void     setContent(JWidget* w) { m_content = w; }
    JWidget* content() const        { return m_content; }

    // Render this dock's content into `area` (host-local). Prefers the hosted content widget
    // (placed at `area` then drawn via its own render); else the paint hook. Called by
    // JDockHost::_renderLeaf for inline leaves and floats alike.
    void renderContent(JPrimitiveBuffer& buf, const JRect& area) {
        if (m_content) { m_content->setBounds(area); m_content->populateRenderPrimitives(buf); }
        else if (onRenderContent) onRenderContent(buf, area);
    }

    // Route content input (host-local coords) to the hosted content widget's own handlers; else
    // the input hook. One method, both call sites (inline runner + float input bridge).
    void dispatchContentInput(float mx, float my, bool pressed, bool released, float wheel) {
        if (m_content) {
            m_content->handleMouseMove(mx, my);
            if (pressed)      m_content->handleMousePress(mx, my);
            if (released)     m_content->handleMouseRelease(mx, my);
            if (wheel != 0.f) m_content->handleScroll(mx, my, wheel);
        } else if (onInputContent) {
            onInputContent(mx, my, pressed, released, wheel);
        }
    }

    // --- Input ---

    void handleMouse(float mx, float my, bool pressed, bool released) {
        m_hoverClose  = m_closeable  && m_titleVisible && _inCloseBtn(mx, my);
        m_hoverPin    = m_pinnable   && m_titleVisible && _inPinBtn(mx, my);
        m_hoverResize = m_resizable  && _inResizeHandle(mx, my);

        if (pressed) {
            if (m_hoverClose)  {
                m_closeRequested = true;
                if (JAiBusHook::emit) JAiBusHook::emit(0, "dock.close", m_title.c_str());
                return;
            }
            if (m_hoverPin) {
                m_pinned = !m_pinned;
                if (JAiBusHook::emit) JAiBusHook::emit(0, m_pinned ? "dock.pin" : "dock.unpin", m_title.c_str());
                return;
            }
            if (m_hoverResize) {
                m_resizing    = true;
                m_resizeInitW = m_w;
                m_resizeInitH = m_h;
                m_resizeAnchorX = mx;
                m_resizeAnchorY = my;
                return;
            }
            if (m_titleVisible && _inTitleBar(mx, my) && !m_pinned) {
                m_dragging = true;
                m_dragOffX = mx - m_x;
                m_dragOffY = my - m_y;
            }
        }

        if (m_resizing) {
            m_w = std::clamp(m_resizeInitW + (mx - m_resizeAnchorX), m_minW, m_maxW);
            m_h = std::clamp(m_resizeInitH + (my - m_resizeAnchorY), m_minH, m_maxH);
        } else if (m_dragging) {
            m_x = mx - m_dragOffX;
            m_y = my - m_dragOffY;
        }

        if (released) {
            m_dragging = false;
            m_resizing = false;
        }
    }

    // --- AI bus interface ---

    JAISemanticNode getSemanticNode() const override {
        std::string state = m_pinned ? "pinned" : "floating";
        return {"JDockWidget", m_title, state, true};
    }

    bool executeSemanticAction(const std::string& a) override {
        if (a == "close")  { m_closeRequested = true;  return true; }
        if (a == "pin")    { m_pinned = true;           return true; }
        if (a == "unpin")  { m_pinned = false;          return true; }
        if (a.rfind("move:", 0) == 0) {
            // "move:x,y"
            auto comma = a.find(',', 5);
            if (comma != std::string::npos) {
                try {
                    m_x = std::stof(a.substr(5, comma - 5));
                    m_y = std::stof(a.substr(comma + 1));
                    return true;
                } catch (...) {}
            }
        }
        if (a.rfind("resize:", 0) == 0) {
            // "resize:w,h"
            auto comma = a.find(',', 7);
            if (comma != std::string::npos) {
                try {
                    m_w = std::max(m_minW, std::stof(a.substr(7, comma - 7)));
                    m_h = std::max(m_minH, std::stof(a.substr(comma + 1)));
                    return true;
                } catch (...) {}
            }
        }
        return false;
    }

    // --- Rendering ---

    void populateRenderPrimitives(JPrimitiveBuffer& buf) const {
        // 1. Drop shadow
        uint8_t shadow[4] = {0, 0, 0, 100};
        buf.pushRectangle(m_x + SHADOW_OFF, m_y + SHADOW_OFF,
                          m_w, m_h, shadow, BORDER_R);

        // 2. Body
        uint8_t body[4] = {22, 22, 26, 245};
        buf.pushRectangle(m_x, m_y, m_w, m_h, body, BORDER_R,
                          1.0f, Colors::Border);

        if (m_titleVisible) {
            // 3. Title bar
            const uint8_t* titleFill = m_pinned ? Colors::AccentPress : Colors::Surface2;
            buf.pushRectangle(m_x + 1.0f, m_y + 1.0f, m_w - 2.0f, TITLE_H - 1.0f,
                              titleFill, BORDER_R - 1.0f);

            // 4. Title text
            float titleBarY = m_y + (TITLE_H - 7.0f) * 0.5f;
            float btnAreaW  = (m_closeable ? BTN_SZ + 2.f : 0.f)
                            + (m_pinnable  ? BTN_SZ + 2.f : 0.f);
            if (JTextHelper::hasAtlas()) {
                uint8_t tc[4] = {210, 210, 220, 220};
                float ty = m_y + (TITLE_H - JTextHelper::lineHeight()) * 0.5f;
                float maxTitleW = m_w - btnAreaW - 14.0f;
                JTextHelper::pushText(buf, m_x + 10.0f, ty, m_title, tc, maxTitleW);
            } else {
                uint8_t tc[4] = {200, 200, 210, 180};
                buf.pushRectangle(m_x + 10.0f, titleBarY, m_w * 0.30f, 7.0f, tc, 2.0f);
            }

            // 5. Pin button (only if pinnable)
            float btnY = m_y + (TITLE_H - BTN_SZ) * 0.5f;
            if (m_pinnable) {
                float pinX = m_x + m_w - BTN_SZ * (m_closeable ? 2.0f : 1.0f) - (m_closeable ? 8.0f : 6.0f);
                uint8_t pinFill[4] = {m_pinned ? (uint8_t)10  : (uint8_t)50,
                                       m_pinned ? (uint8_t)132 : (uint8_t)50,
                                       m_pinned ? (uint8_t)255 : (uint8_t)60,
                                       m_hoverPin ? (uint8_t)220 : (uint8_t)140};
                buf.pushRectangle(pinX, btnY, BTN_SZ, BTN_SZ, pinFill, 3.0f);
                uint8_t needle[4] = {200, 200, 210, 180};
                buf.pushRectangle(pinX + BTN_SZ * 0.42f, btnY + 2.0f, 2.5f, BTN_SZ - 4.0f, needle, 1.0f);
            }

            // 6. Close button (only if closeable)
            if (m_closeable) {
                float closeX = m_x + m_w - BTN_SZ - 6.0f;
                const auto& closeSrc = m_hoverClose ? Colors::CloseBtnHover : Colors::CloseBtn;
                uint8_t closeFill[4] = {closeSrc[0], closeSrc[1], closeSrc[2], closeSrc[3]};
                buf.pushRectangle(closeX, btnY, BTN_SZ, BTN_SZ, closeFill, 3.0f);
                uint8_t xc[4] = {Colors::CloseBtnMark[0], Colors::CloseBtnMark[1],
                                  Colors::CloseBtnMark[2], Colors::CloseBtnMark[3]};
                buf.pushRectangle(closeX + 3.0f, btnY + BTN_SZ * 0.42f, BTN_SZ - 6.0f, 2.5f, xc, 1.0f);
                buf.pushRectangle(closeX + BTN_SZ * 0.42f, btnY + 3.0f, 2.5f, BTN_SZ - 6.0f, xc, 1.0f);
            }
        } // end m_titleVisible

        // 7. Content area background
        uint8_t content[4] = {14, 14, 16, 240};
        buf.pushRectangle(m_x + 1.0f, m_y + TITLE_H,
                          m_w - 2.0f, m_h - TITLE_H - 1.0f, content, 0.0f);

        // 8. Re-dock hint badge (only for torn tabs)
        if (m_tornState.has_value()) {
            uint8_t badge[4] = {10, 132, 255, 120};
            buf.pushRectangle(m_x + 6.0f, m_y + m_h - 14.0f, 60.0f, 8.0f, badge, 4.0f);
        }

        // 9. Resize corner handle (bottom-right) — only if resizable
        if (m_resizable) {
            uint8_t handle[4] = {m_hoverResize ? (uint8_t)140 : (uint8_t)70,
                                 m_hoverResize ? (uint8_t)140 : (uint8_t)70,
                                 m_hoverResize ? (uint8_t)160 : (uint8_t)80,
                                 m_hoverResize ? (uint8_t)240 : (uint8_t)160};
            float hSz = 12.0f;
            buf.pushRectangle(m_x + m_w - hSz - 2.0f, m_y + m_h - hSz - 2.0f,
                              hSz, hSz, handle, 3.0f);
        }
    }

private:
    bool _inTitleBar(float mx, float my) const {
        return mx >= m_x && mx <= m_x + m_w &&
               my >= m_y && my <= m_y + TITLE_H;
    }
    bool _inCloseBtn(float mx, float my) const {
        float cx = m_x + m_w - BTN_SZ - 6.0f;
        float cy = m_y + (TITLE_H - BTN_SZ) * 0.5f;
        return mx >= cx && mx <= cx + BTN_SZ && my >= cy && my <= cy + BTN_SZ;
    }
    bool _inPinBtn(float mx, float my) const {
        float px = m_x + m_w - BTN_SZ * 2.0f - 8.0f;
        float py = m_y + (TITLE_H - BTN_SZ) * 0.5f;
        return mx >= px && mx <= px + BTN_SZ && my >= py && my <= py + BTN_SZ;
    }
    bool _inResizeHandle(float mx, float my) const {
        // 16×16 grab zone at bottom-right corner
        return mx >= m_x + m_w - 16.0f && mx <= m_x + m_w &&
               my >= m_y + m_h - 16.0f && my <= m_y + m_h;
    }

    std::string m_title;
    std::string m_tag{};
    float m_x, m_y, m_w, m_h;

    // Size constraints
    float m_minW{120.f}, m_minH{60.f};
    float m_maxW{1e9f},  m_maxH{1e9f};

    // Behaviour flags
    bool    m_floatable{true};
    bool    m_tabifiable{true};
    uint8_t m_allowedDrops{kDropAll};
    bool    m_dockable{true};
    bool    m_closeable{true};
    bool    m_pinnable{true};
    bool    m_resizable{true};
    bool    m_titleVisible{true};

    // Dock-side affinity
    std::vector<std::string> m_acceptLeafLabels;
    std::vector<std::string> m_rejectLeafLabels;

    // Drag-to-move
    bool  m_dragging{false};
    float m_dragOffX{0}, m_dragOffY{0};

    // Drag-to-resize
    bool  m_resizing{false};
    float m_resizeInitW{0}, m_resizeInitH{0};
    float m_resizeAnchorX{0}, m_resizeAnchorY{0};

    bool m_pinned{false};
    bool m_closeRequested{false};
    bool m_hoverClose{false};
    bool m_hoverPin{false};
    bool m_hoverResize{false};

    std::optional<JTornTabState> m_tornState;

    // Framework-hosted content widget tree (non-owning); null = use the paint/input hooks.
    JWidget* m_content{nullptr};
};

} // inline namespace jf
