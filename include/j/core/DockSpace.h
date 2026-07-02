#pragma once

// JDockSpace — an IDE-style dock composer: a protected CENTER region framed by four dock
// areas (Left / Right / Top / Bottom). Each area — including the center — is a full JDockHost,
// so all the dock machinery (split, tab, tear-out, drop offers, fixed-size, revert) works
// inside every area unchanged. The areas claim their sizes; the center is the residual (Qt's
// "docks are the sizers, the centre is the leftover"). Cross-area movement is free because a
// drag always tears out to a float, which re-docks into whichever host the registry finds.
//
// Per-area rules live on each host (affinity); per-dock rules live on each JDockWidget (tag /
// floatable / tabifiable / accept). The center is just "a host whose rules repel outsiders."

#include <j/core/DockManager.h>
#include <j/core/DockRegistry.h>
#include <j/graphics/RenderPrimitive.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>

inline namespace jf {

class JDockSpace {
public:
    enum Area { Left, Right, Top, Bottom, Center, AreaCount };

    JDockHost& left()   { return m_host[Left]; }
    JDockHost& right()  { return m_host[Right]; }
    JDockHost& top()    { return m_host[Top]; }
    JDockHost& bottom() { return m_host[Bottom]; }
    JDockHost& host(Area a) { return m_host[a]; }

    // The window's central content. The centre is NOT a dock host — it holds one widget directly, while
    // the four edges are the dock hosts. If you actually want docks in the centre, opt in by making that
    // widget a dock host. Layout/render/input for the centre rect route straight to this widget.
    void         setCentralWidget(JWidget* w) { m_central = w; }
    JWidget*     centralWidget() const { return m_central; }
    const JRect& centerRect() const { return m_rect[Center]; }

    // An outer area is shown only when it has a reserved size; the centre is always present.
    void setLeftWidth(float w)    { m_size[Left] = w; }
    void setRightWidth(float w)   { m_size[Right] = w; }
    void setTopHeight(float h)    { m_size[Top] = h; }
    void setBottomHeight(float h) { m_size[Bottom] = h; }
    // Corner ownership: true = Left/Right run full height (own the corners) and Top/Bottom sit
    // between them; false = Top/Bottom span full width and the sides sit between them.
    void setSidesOwnCorners(bool v) { m_sidesOwnCorners = v; }

    // The centre is always present; an outer area shows only while it has a reserved size
    // AND still holds docks — so emptying it (tear-out) collapses it and the centre reclaims
    // the space.
    bool active(Area a) const {
        return a == Center ? true : (m_size[a] > 0.f && m_host[a].dockCount() > 0);
    }
    const JRect& rect(Area a) const { return m_rect[a]; }

    // The active-tab dock whose content area contains (mx,my) across all active areas, or
    // nullptr. Lets the runner route content input (clicks / wheel) to the dock's hook.
    JDockWidget* contentDockAt(float mx, float my) {
        for (int a = 0; a < AreaCount; ++a)
            if (a != Center && active(Area(a)))
                if (JDockWidget* d = m_host[a].contentDockAt(mx, my)) return d;
        return nullptr;
    }

    void dump(const char* tag) const {
        static const char* nm[] = {"L", "R", "T", "B", "C"};
        std::fprintf(stderr, "[space %s]", tag);
        for (int a = 0; a < AreaCount; ++a)
            std::fprintf(stderr, " %s{act=%d sz=%.0f docks=%zu rect=%.0f,%.0f,%.0fx%.0f strip=%.0fx%.0f}",
                         nm[a], active(Area(a)) ? 1 : 0, m_size[a], m_host[a].dockCount(),
                         m_rect[a].x, m_rect[a].y, m_rect[a].width, m_rect[a].height,
                         m_strip[a].width, m_strip[a].height);
        std::fprintf(stderr, "\n");
    }

    void computeLayout(JRect content) {
        m_content = content;
        const float x = content.x, y = content.y, W = content.width, H = content.height;
        const float lw = active(Left)   ? std::min(m_size[Left],   W * 0.45f) : 0.f;
        const float rw = active(Right)  ? std::min(m_size[Right],  W * 0.45f) : 0.f;
        const float th = active(Top)    ? std::min(m_size[Top],    H * 0.45f) : 0.f;
        const float bh = active(Bottom) ? std::min(m_size[Bottom], H * 0.45f) : 0.f;

        if (m_sidesOwnCorners) {
            m_rect[Left]   = {x, y, lw, H};
            m_rect[Right]  = {x + W - rw, y, rw, H};
            const float cx = x + lw, cw = W - lw - rw;
            m_rect[Top]    = {cx, y, cw, th};
            m_rect[Bottom] = {cx, y + H - bh, cw, bh};
            m_rect[Center] = {cx, y + th, cw, H - th - bh};
        } else {
            m_rect[Top]    = {x, y, W, th};
            m_rect[Bottom] = {x, y + H - bh, W, bh};
            const float cy = y + th, ch = H - th - bh;
            m_rect[Left]   = {x, cy, lw, ch};
            m_rect[Right]  = {x + W - rw, cy, rw, ch};
            m_rect[Center] = {x + lw, cy, W - lw - rw, ch};
        }
        for (int a = 0; a < AreaCount; ++a)
            if (a != Center && active(Area(a))) m_host[a].computeLayout(m_rect[a]);
        if (m_central) m_central->setBounds(m_rect[Center]);   // the centre is a widget, not a host

        // Collapsed-but-reserved areas get an invisible edge strip — a drop zone so a float
        // dragged to the window edge can restore that side (re-dock into its host). Must be
        // >= 64px (ARROW_SZ*2) or _buildDropTargets skips the leaf and offers nothing.
        const float strip = 80.f;
        const JRect& cc = m_rect[Center];
        // Corner-aware owner extents (match the active-area geometry + the divider rule):
        // a side seam runs full content height when sides own corners, else the centre row;
        // a top/bottom seam runs the centre column when sides own corners, else full width.
        const float vy = m_sidesOwnCorners ? y : cc.y,      vh = m_sidesOwnCorners ? H : cc.height;
        const float hx = m_sidesOwnCorners ? cc.x : x,      hw = m_sidesOwnCorners ? cc.width : W;
        for (int a : {Left, Right, Top, Bottom}) {
            if (active(Area(a)) || m_size[a] <= 0.f) { m_strip[a] = {}; continue; }
            const float sz = m_size[a];
            // Lay the placeholder out at the area's RESTORED size so the drop highlight
            // previews where the dock actually lands; the thin strip is only the hit-zone.
            JRect restored;
            if      (a == Left)  { restored = {x, vy, sz, vh};          m_strip[a] = {x, vy, strip, vh}; }
            else if (a == Right) { restored = {x + W - sz, vy, sz, vh}; m_strip[a] = {x + W - strip, vy, strip, vh}; }
            else if (a == Top)   { restored = {hx, y, hw, sz};          m_strip[a] = {hx, y, hw, strip}; }
            else                 { restored = {hx, y + H - sz, hw, sz}; m_strip[a] = {hx, y + H - strip, hw, strip}; }
            m_host[a].computeLayout(restored);
        }
    }

    // Register every active area for float re-dock. Each area occupies its area screen rect
    // but shares the window's coordinate origin, so its nodes stay in window space.
    void registerAll(int winSx, int winSy) {
        for (int a = 0; a < AreaCount; ++a) {
            if (a == Center) continue;   // the centre is a plain widget — nothing docks into it
            // Active areas register their full rect; a collapsed-but-reserved area registers
            // its edge strip (so it can be restored); a truly-unused area unregisters.
            const JRect* r = active(Area(a)) ? &m_rect[a]
                           : (a != Center && m_size[a] > 0.f) ? &m_strip[a] : nullptr;
            if (r) {
                JDockRegistry::instance().registerHostEx(
                    m_host[a], winSx + static_cast<int>(r->x), winSy + static_cast<int>(r->y),
                    static_cast<uint32_t>(r->width), static_cast<uint32_t>(r->height), winSx, winSy);
            } else {
                JDockRegistry::instance().unregisterHost(m_host[a]);
            }
        }
    }

    void render(JPrimitiveBuffer& buf) {
        for (int a = 0; a < AreaCount; ++a) if (a != Center && active(Area(a))) m_host[a].populateRenderPrimitives(buf);
        if (m_central) m_central->populateRenderPrimitives(buf);   // centre content (not a host)

        // Divider lines at the seams, following corner ownership: the corner owner's seam
        // runs the FULL content extent (its column/row spans the corners); the other runs
        // only the centre column/row between the owners.
        uint8_t sep[4] = {Colors::Border[0], Colors::Border[1], Colors::Border[2], 255};
        const JRect& c = m_rect[Center];
        const JRect& F = m_content;
        // vertical seam (left|centre, right|centre): full height if sides own corners, else centre height
        const float vy = m_sidesOwnCorners ? F.y      : c.y;
        const float vh = m_sidesOwnCorners ? F.height : c.height;
        // horizontal seam (top|centre, bottom|centre): full width if top/bottom own corners, else centre width
        const float hx = m_sidesOwnCorners ? c.x      : F.x;
        const float hw = m_sidesOwnCorners ? c.width  : F.width;
        if (active(Left))   buf.pushRectangle(m_rect[Left].x + m_rect[Left].width - 1.f, vy, 1.f, vh, sep, 0.f);
        if (active(Right))  buf.pushRectangle(m_rect[Right].x, vy, 1.f, vh, sep, 0.f);
        if (active(Top))    buf.pushRectangle(hx, m_rect[Top].y + m_rect[Top].height - 1.f, hw, 1.f, sep, 0.f);
        if (active(Bottom)) buf.pushRectangle(hx, m_rect[Bottom].y, hw, 1.f, sep, 0.f);

        // Overlays (drop indicators) for active areas AND collapsed-but-reserved edge strips,
        // so the restore drop zone shows a preview while dragging toward the window edge.
        for (int a = 0; a < AreaCount; ++a)
            if (a != Center && (active(Area(a)) || m_size[a] > 0.f))
                m_host[a].populateOverlay(buf);
    }

    // Route the mouse: an inter-area splitter drag first, otherwise the area host under the
    // cursor. Returns the host that handled it plus any dock event (WantsFloat/CloseRequested).
    // True while a dock-area splitter is being dragged — the app skips central-widget input so a
    // resize-divider grab (which sits on the centre boundary) doesn't also start a surface marquee.
    bool isResizing() const { return m_dragSplit >= 0; }

    struct Result { JDockHost* host{nullptr}; std::optional<JDockHost::JDockEvent> ev; };
    Result handleMouse(float mx, float my, bool pressed, bool released) {
        if (m_dragSplit >= 0) {
            dragSplitter(mx, my);
            if (released) m_dragSplit = -1;
            return {};
        }
        if (pressed) {
            int s = splitterAt(mx, my);
            if (s >= 0) {
                m_dragSplit = s;
                m_dragStart = (s == Top || s == Bottom) ? my : mx;
                m_dragStartSize = m_size[s];
                return {};
            }
        }
        // Mouse capture: a press captures the host under the cursor, and every subsequent
        // event routes there until release — even if the cursor leaves that host's rect. Without
        // this, dragging a tab OUT of its host (e.g. tearing it toward the centre) would hand
        // events to whatever host is now under the cursor, so the source host never sees the
        // drag cross its tear-out threshold and the drag silently dies. Capture follows the
        // gesture, not the pointer.
        Area a;
        if (m_captureArea != AreaCount && !pressed) {
            a = m_captureArea;                 // mid-gesture: stay with the captured host
        } else {
            a = areaAt(mx, my);
            if (pressed) m_captureArea = a;    // press (re)arms capture on the host under cursor
        }
        if (released) m_captureArea = AreaCount;
        if (a == AreaCount) return {};
        return { &m_host[a], m_host[a].handleMouse(mx, my, pressed, released) };
    }

    // The app calls this when a tear-out (WantsFloat) settles, so a capture that won't see its
    // own release (the button comes up over the floating window) doesn't stay stuck on a host.
    void releaseMouseCapture() { m_captureArea = AreaCount; }

    JDockHost::JHoverCursor getHoverCursor(float mx, float my) {
        using HC = JDockHost::JHoverCursor;
        int s = (m_dragSplit >= 0) ? m_dragSplit : splitterAt(mx, my);
        if (s >= 0) return (s == Top || s == Bottom) ? HC::Vert : HC::Horiz;
        Area a = areaAt(mx, my);
        return (a != AreaCount) ? m_host[a].getHoverCursor(mx, my) : HC::Default;
    }

private:
    int splitterAt(float mx, float my) const {
        const float pad = 4.f;
        const JRect& c = m_rect[Center];
        for (int a : {Left, Right, Top, Bottom}) {
            if (!active(Area(a))) continue;
            if (a == Left || a == Right) {
                // Side seam is a vertical line; with sides owning the corners it runs the
                // FULL content height (it borders top/centre/bottom alike).
                float line = (a == Left) ? m_rect[Left].x + m_rect[Left].width : m_rect[Right].x;
                float y0 = m_sidesOwnCorners ? m_content.y : c.y;
                float y1 = m_sidesOwnCorners ? m_content.y + m_content.height : c.y + c.height;
                if (std::fabs(mx - line) <= pad && my >= y0 && my <= y1) return a;
            } else {
                // Top/bottom seam is a horizontal line spanning the centre column width
                // (or full width when top/bottom own the corners).
                float line = (a == Top) ? m_rect[Top].y + m_rect[Top].height : m_rect[Bottom].y;
                float x0 = m_sidesOwnCorners ? c.x : m_content.x;
                float x1 = m_sidesOwnCorners ? c.x + c.width : m_content.x + m_content.width;
                if (std::fabs(my - line) <= pad && mx >= x0 && mx <= x1) return a;
            }
        }
        return -1;
    }

    void dragSplitter(float mx, float my) {
        const int a = m_dragSplit;
        const float delta = (a == Top || a == Bottom) ? (my - m_dragStart) : (mx - m_dragStart);
        const float sign = (a == Left || a == Top) ? 1.f : -1.f;   // grow toward the centre
        // Clamp the stored size to the SAME cap computeLayout displays (45% of the content),
        // so it can't overshoot the visible splitter — otherwise dragging back out has a
        // dead-zone equal to the overshoot before the splitter moves.
        const float cap = ((a == Left || a == Right) ? m_content.width : m_content.height) * 0.45f;
        m_size[a] = std::clamp(m_dragStartSize + sign * delta, 60.f, cap);
        computeLayout(m_content);
    }

    Area areaAt(float mx, float my) const {
        for (int a = 0; a < AreaCount; ++a) {
            if (a == Center || !active(Area(a))) continue;   // centre isn't a host — routed to the central widget
            const JRect& r = m_rect[a];
            if (mx >= r.x && mx < r.x + r.width && my >= r.y && my < r.y + r.height) return Area(a);
        }
        return AreaCount;
    }

    std::array<JDockHost, AreaCount> m_host;      // the Center slot is inert — the centre is a widget
    JWidget*                         m_central{nullptr};   // the window's central content
    std::array<float, AreaCount>     m_size{};    // Left/Right = width, Top/Bottom = height
    std::array<JRect, AreaCount>     m_rect{};
    std::array<JRect, AreaCount>     m_strip{};   // edge drop-zone for a collapsed area
    JRect m_content{};
    bool  m_sidesOwnCorners{true};
    int   m_dragSplit{-1};
    Area  m_captureArea{AreaCount};   // host that owns the in-progress press gesture (capture)
    float m_dragStart{0.f}, m_dragStartSize{0.f};
};

} // inline namespace jf
