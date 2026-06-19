#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>
#include <algorithm>
#include <cmath>
#include "BaseWidgets.h"         // TornTabState, Colors, PrimitiveBuffer

namespace Genesis {

// ============================================================================
// DockWidget — a floating panel that renders at an absolute screen position,
// independent of the SceneGraph layout.
//
// Lifecycle:
//   1. Created fresh by the app (general panel).
//   2. Created from a TornTabState (torn-off tab) — carries re-dock provenance.
//   3. Destroyed when the user presses Close, or when re-docked.
//
// The DockWidget does NOT inherit Widget — it lives outside the layout tree
// and positions itself via explicit x/y coordinates.  Content is rendered by
// the owner (app/catalog) which calls contentWidget->populateRenderPrimitives
// then offsets by contentArea().
// ============================================================================

class DockWidget {
public:
    static constexpr float TITLE_H     = 30.0f;
    static constexpr float BTN_SZ      = 16.0f;  // close / pin button size
    static constexpr float BORDER_R    = 8.0f;
    static constexpr float SHADOW_OFF  = 4.0f;
    static constexpr float SNAP_DIST   = 14.0f;

    // Bit positions for setAllowedDrops().
    // These match the ordinal values of DropPos (Center=0, Left=1, Right=2,
    // Top=3, Bottom=4) so DockManager can compute the bit as 1<<DropPos.
    static constexpr uint8_t kDropCenter = 1 << 0;
    static constexpr uint8_t kDropLeft   = 1 << 1;
    static constexpr uint8_t kDropRight  = 1 << 2;
    static constexpr uint8_t kDropTop    = 1 << 3;
    static constexpr uint8_t kDropBottom = 1 << 4;
    static constexpr uint8_t kDropSides  = kDropLeft | kDropRight | kDropTop | kDropBottom;
    static constexpr uint8_t kDropAll    = 0x1Fu;

    // --- Constructors ---

    DockWidget(std::string title, float x, float y, float w, float h)
        : m_title(std::move(title)), m_x(x), m_y(y), m_w(w), m_h(h) {}

    DockWidget(TornTabState state, float x, float y, float w = 320.0f, float h = 240.0f)
        : m_title(state.title), m_x(x), m_y(y), m_w(w), m_h(h)
        , m_tornState(std::move(state)) {}

    // Non-copyable, movable
    DockWidget(const DockWidget&)            = delete;
    DockWidget& operator=(const DockWidget&) = delete;
    DockWidget(DockWidget&&)                 = default;
    DockWidget& operator=(DockWidget&&)      = default;

    // --- Geometry ---

    float x()      const { return m_x; }
    float y()      const { return m_y; }
    float width()  const { return m_w; }
    float height() const { return m_h; }

    // Rectangle the content widget should render into
    Rect contentArea() const {
        return { m_x + 1.0f, m_y + TITLE_H,
                 m_w - 2.0f, m_h - TITLE_H - 1.0f };
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

    // --- State queries ---

    bool closeRequested() const { return m_closeRequested; }
    bool isPinned()       const { return m_pinned; }
    bool isDragging()     const { return m_dragging; }

    bool hasTornState()               const { return m_tornState.has_value(); }
    TornTabState& tornState()               { return *m_tornState; }
    const TornTabState& tornState()   const { return *m_tornState; }

    const std::string& title() const { return m_title; }

    // Affinity tag used by DockHost to accept/reject docks per leaf zone.
    void setTag(std::string t) { m_tag = std::move(t); }
    const std::string& tag() const { return m_tag; }

    // --- Size constraints ---
    // Applied during floating resize and respected by DockHost layout.

    void  setMinSize(float w, float h) { m_minW = w; m_minH = h; }
    void  setMaxSize(float w, float h) { m_maxW = w; m_maxH = h; }
    float minW() const { return m_minW; }
    float minH() const { return m_minH; }
    float maxW() const { return m_maxW; }
    float maxH() const { return m_maxH; }

    // --- Behaviour flags ---

    // Can be torn out of the DockHost into a floating OS window.
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

    // Dock-side affinity — which leaf labels this dock is willing to join.
    // Independent of the leaf's own DockAffinityRule (both must pass).
    // Empty accept list = accept any leaf label.
    void setAcceptLeafLabels(std::vector<std::string> v) { m_acceptLeafLabels = std::move(v); }
    void setRejectLeafLabels(std::vector<std::string> v) { m_rejectLeafLabels = std::move(v); }
    bool acceptsLeafLabel(std::string_view label) const {
        for (const auto& r : m_rejectLeafLabels) if (r == label) return false;
        if (m_acceptLeafLabels.empty()) return true;
        for (const auto& a : m_acceptLeafLabels) if (a == label) return true;
        return false;
    }

    // --- Input ---

    void handleMouse(float mx, float my, bool pressed, bool released) {
        m_hoverClose  = _inCloseBtn(mx, my);
        m_hoverPin    = _inPinBtn(mx, my);
        m_hoverResize = _inResizeHandle(mx, my);

        if (pressed) {
            if (m_hoverClose)  { m_closeRequested = true; return; }
            if (m_hoverPin)    { m_pinned = !m_pinned; return; }
            if (m_hoverResize) {
                m_resizing    = true;
                m_resizeInitW = m_w;
                m_resizeInitH = m_h;
                m_resizeAnchorX = mx;
                m_resizeAnchorY = my;
                return;
            }
            if (_inTitleBar(mx, my) && !m_pinned) {
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

    // --- Rendering ---

    void populateRenderPrimitives(PrimitiveBuffer& buf) const {
        // 1. Drop shadow
        uint8_t shadow[4] = {0, 0, 0, 100};
        buf.pushRectangle(m_x + SHADOW_OFF, m_y + SHADOW_OFF,
                          m_w, m_h, shadow, BORDER_R);

        // 2. Body
        uint8_t body[4] = {22, 22, 26, 245};
        buf.pushRectangle(m_x, m_y, m_w, m_h, body, BORDER_R,
                          1.0f, Colors::Border);

        // 3. Title bar
        const uint8_t* titleFill = m_pinned ? Colors::AccentPress : Colors::Surface2;
        buf.pushRectangle(m_x + 1.0f, m_y + 1.0f, m_w - 2.0f, TITLE_H - 1.0f,
                          titleFill, BORDER_R - 1.0f);

        // 4. Title text (or placeholder bars if atlas not loaded)
        float titleBarY = m_y + (TITLE_H - 7.0f) * 0.5f;
        if (TextHelper::hasAtlas()) {
            uint8_t tc[4] = {210, 210, 220, 220};
            float ty = m_y + (TITLE_H - TextHelper::lineHeight()) * 0.5f;
            float maxTitleW = m_w - BTN_SZ * 2.0f - 24.0f; // leave room for buttons
            TextHelper::pushText(buf, m_x + 10.0f, ty, m_title, tc, maxTitleW);
        } else {
            uint8_t tc[4] = {200, 200, 210, 180};
            buf.pushRectangle(m_x + 10.0f, titleBarY, m_w * 0.30f, 7.0f, tc, 2.0f);
        }

        // 5. Pin button (lock icon hint)
        float pinX = m_x + m_w - BTN_SZ * 2.0f - 8.0f;
        float pinY = m_y + (TITLE_H - BTN_SZ) * 0.5f;
        uint8_t pinFill[4] = {m_pinned ? (uint8_t)10  : (uint8_t)50,
                               m_pinned ? (uint8_t)132 : (uint8_t)50,
                               m_pinned ? (uint8_t)255 : (uint8_t)60,
                               m_hoverPin ? (uint8_t)220 : (uint8_t)140};
        buf.pushRectangle(pinX, pinY, BTN_SZ, BTN_SZ, pinFill, 3.0f);
        // Pin needle (vertical bar in centre)
        uint8_t needle[4] = {200, 200, 210, 180};
        buf.pushRectangle(pinX + BTN_SZ * 0.42f, pinY + 2.0f, 2.5f, BTN_SZ - 4.0f, needle, 1.0f);

        // 6. Close button
        float closeX = m_x + m_w - BTN_SZ - 6.0f;
        float closeY = pinY;
        uint8_t closeFill[4] = {m_hoverClose ? (uint8_t)220 : (uint8_t)60,
                                 m_hoverClose ? (uint8_t)50  : (uint8_t)40,
                                 m_hoverClose ? (uint8_t)50  : (uint8_t)44,
                                 m_hoverClose ? (uint8_t)255 : (uint8_t)160};
        buf.pushRectangle(closeX, closeY, BTN_SZ, BTN_SZ, closeFill, 3.0f);
        // × mark: two diagonal rects
        uint8_t xc[4] = {255, 255, 255, 200};
        buf.pushRectangle(closeX + 3.0f, closeY + BTN_SZ * 0.42f, BTN_SZ - 6.0f, 2.5f, xc, 1.0f);
        buf.pushRectangle(closeX + BTN_SZ * 0.42f, closeY + 3.0f, 2.5f, BTN_SZ - 6.0f, xc, 1.0f);

        // 7. Content area background
        uint8_t content[4] = {14, 14, 16, 240};
        buf.pushRectangle(m_x + 1.0f, m_y + TITLE_H,
                          m_w - 2.0f, m_h - TITLE_H - 1.0f, content, 0.0f);

        // 8. Re-dock hint badge (only for torn tabs)
        if (m_tornState.has_value()) {
            uint8_t badge[4] = {10, 132, 255, 120};
            buf.pushRectangle(m_x + 6.0f, m_y + m_h - 14.0f, 60.0f, 8.0f, badge, 4.0f);
        }

        // 9. Resize corner handle (bottom-right) — brighter when hovered
        uint8_t handle[4] = {m_hoverResize ? (uint8_t)140 : (uint8_t)70,
                             m_hoverResize ? (uint8_t)140 : (uint8_t)70,
                             m_hoverResize ? (uint8_t)160 : (uint8_t)80,
                             m_hoverResize ? (uint8_t)240 : (uint8_t)160};
        float hSz = 12.0f;
        buf.pushRectangle(m_x + m_w - hSz - 2.0f, m_y + m_h - hSz - 2.0f,
                          hSz, hSz, handle, 3.0f);
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

    std::optional<TornTabState> m_tornState;
};

} // namespace Genesis
