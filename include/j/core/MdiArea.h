#pragma once

// JMdiArea / JMdiChild — child windows inside a widget, the way a tuning application wants them.
//
// NOT A DOCK HOST. A dock host TILES: its docks snap into splitters and tabs, and there is no free
// position or overlap. That is the right primitive for tool panels and the wrong one for documents —
// a page opened from a menu wants to be somewhere, at some size, over the top of what was there, and
// to be moved and sized by the person reading it. This is that: frames positioned freely inside their
// area, front-to-back, each with its own title bar.
//
// OPENS MAXIMISED, because a page is nearly always what you want to look at when you have just asked
// for it, and hunting a small frame in a big empty area is not a feature. Restore gives it back its
// last free geometry, and from there the edges and corners size it.
//
// The AREA owns the frames; the APPLICATION owns the content widgets. A child is a title bar and a
// rectangle around somebody else's widget, so a page can be handed straight to it and taken back.

#include "JWidget.h"
#include "FocusManager.h"
#include "JStyle.h"
#include "JTitleBar.h"
#include "JTextHelper.h"
#include "../graphics/RenderPrimitive.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

inline namespace jf {

// A WINDOW OPENS INSIDE ITS AREA, WHOLE. Wanting more room than there is does not entitle a window to hang
// off the bottom of the space it lives in: a page taller than the centre opened with its footer — its Burn
// button — below the edge of the application, unreachable and with nothing to say it was there. Clamping
// the size alone was not enough either, since the frame is offset from the corner to cascade: a height
// clamped to the area, placed 24 px down it, still ends 24 px past the bottom. So the offset is only taken
// while there is room to take it from, and whatever is left over the content scrolls, which is the whole
// reason the surface underneath has scroll bars.
inline JRect jMdiFitted(float w, float h, const JRect& a) {
    JRect f{ a.x, a.y, std::min(w, a.width), std::min(h, a.height) };
    f.x += std::max(0.f, std::min(24.f, a.width  - f.width));
    f.y += std::max(0.f, std::min(24.f, a.height - f.height));
    return f;
}

class JMdiChild {
public:
    static constexpr float kTitleH = 26.f;
    // The frame's own inset around the content. Wide enough to READ as a border at the bottom and the
    // sides — 4 px of frame under a page looked like a rendering seam rather than the edge of a window.
    static constexpr float kBorder = 6.f;
    // WHAT YOU CAN ACTUALLY HIT. The inset is 4 px, and a 4 px target is a target nobody hits — the
    // corner especially, which is the one people reach for. The grab band is wider than the border it
    // belongs to, and reaches INTO the content, because catching a resize a few pixels early is a much
    // smaller annoyance than a resize that will not start.
    static constexpr float kGrab   = 10.f;
    static constexpr float kGrip   = 14.f;    // the drawn corner mark, so the corner looks grabbable
    static constexpr float kBtn    = 18.f;

    JMdiChild(std::string title, JWidget* content, JRect frame)
        : m_title(std::move(title)), m_content(content), m_frame(frame), m_restore(frame) {}

    const std::string& title() const { return m_title; }
    JWidget*  content() const { return m_content; }
    JRect     frame()   const { return m_frame; }
    void      setFrame(const JRect& r) { m_frame = r; }

    bool maximised() const { return m_max; }
    void setMaximised(bool on, const JRect& area) {
        if (on == m_max) return;
        if (on) { m_restore = m_frame; m_frame = area; }
        else    { m_frame = m_restore; }
        m_max = on;
    }
    // A maximised child follows its area: the point of maximised is "all of it", not "the size the
    // area happened to be when I was maximised".
    //
    // AND A CHILD IS NEVER LEFT SOMEWHERE IT CANNOT BE GRABBED. Two ways that happened. A child opened
    // before the first layout pass had no area to be placed against — area() was still empty — so it
    // took a restore rect built from nothing and came back from Restore with its title bar above the
    // top of the area, clipped away, with no way to drag it down again. And a child positioned legally
    // can be orphaned later by the area itself shrinking under it (the window resized, a dock opened).
    // Both end the same way, so both are healed here, on the frame that knows what the area really is.
    void refit(const JRect& area) {
        if (area.width <= 0.f || area.height <= 0.f) return;      // nothing to resolve against yet
        if (m_provisional) {                                      // placed before the area was known
            m_restore = jMdiFitted(m_wantW > 0.f ? m_wantW : std::max(320.f, area.width  * 0.66f),
                                         m_wantH > 0.f ? m_wantH : std::max(200.f, area.height * 0.66f), area);
            m_provisional = false;
            if (!m_max) m_frame = m_restore;
        }
        if (m_max) { m_frame = area; return; }
        // Keep a grabbable piece of the title bar inside the area. Enough of it to catch, not so much
        // that a window cannot be pushed mostly off to the side and left there deliberately.
        const float keep = std::min(m_frame.width, 80.f);
        m_frame.x = std::clamp(m_frame.x, area.x - (m_frame.width - keep), area.x + area.width - keep);
        m_frame.y = std::clamp(m_frame.y, area.y, area.y + area.height - kTitleH);
    }

    bool closeRequested() const { return m_close; }
    void requestClose()          { m_close = true; }

    void  setMinSize(float w, float h) { m_minW = w; m_minH = h; }
    float minW() const { return m_minW; }
    float minH() const { return m_minH; }

    // Where the content lives: the frame less its title bar and border.
    JRect contentRect() const {
        return { m_frame.x + kBorder, m_frame.y + kTitleH,
                 std::max(0.f, m_frame.width - 2 * kBorder),
                 std::max(0.f, m_frame.height - kTitleH - kBorder) };
    }

private:
    friend class JMdiArea;
    std::string m_title;
    JWidget*    m_content{nullptr};
    JRect       m_frame{}, m_restore{};
    float       m_minW{240.f}, m_minH{120.f};
    float       m_wantW{0.f}, m_wantH{0.f};   // the restore size the caller asked for, 0 = "a good fraction"
    bool        m_max{true}, m_close{false};
    bool        m_provisional{false};         // the restore rect was built before the area was known
};

class JMdiArea : public JWidget {
public:
    explicit JMdiArea(JSceneGraph& g) : JWidget(g, "JMdiArea") {}

    static JRect fitted(float w, float h, const JRect& a) { return jMdiFitted(w, h, a); }

    // Open a child, maximised, at the front. `w`/`h` are the size it restores to; 0 means "a good
    // fraction of the area", which is what a first restore should give rather than a 1x1 sliver.
    // WHAT IS BEHIND THE WINDOWS. An MDI area is not a blank slab: TunerStudio keeps its gauge cluster and
    // its live readouts on the page behind the dialogs, and a tuner reads them WHILE editing — that is the
    // whole point of tuning against a running engine. Moving pages into windows put an empty background
    // where the live instrumentation used to be. The background widget fills the area, paints under every
    // child, and takes the input no child window wanted, so it behaves exactly as it did when it WAS the
    // central widget. Non-owning: the application still owns it.
    void setBackground(JWidget* w) {
        if (m_background == w) return;
        if (m_background) removeChild(m_background);
        m_background = w;
        if (w) addChild(w);
    }
    JWidget* background() const { return m_background; }

    // Told when a child window has been closed by its ✕, so the owner of the CONTENT can let go of it.
    // The area owns frames, never content — it will not delete somebody else's widget.
    std::function<void(JMdiChild*)> onChildClosed;

    JMdiChild* open(std::string title, JWidget* content, float w = 0.f, float h = 0.f) {
        const JRect a = area();
        // THE SIZE THE CONTENT NEEDS BEFORE IT NEEDS SCROLLBARS. A caller that says nothing gets the
        // content's own preferred size plus this frame's chrome — which is the size at which a page is
        // whole: nothing clipped, nothing to scroll. Guessing a fraction of the area instead gave every
        // window scrollbars it did not need, or empty space it could not use. Anything bigger than the
        // area is clamped to the area (that page really does need to scroll), and content with no
        // opinion still opens maximised, since an unknown size is best answered with all of it.
        const JRect want = content ? content->preferredSize() : JRect{};
        const bool  hinted = (w > 0.f && h > 0.f) || (want.width > 1.f && want.height > 1.f);
        const float fw = w > 0.f ? w : want.width  + 2.f * JMdiChild::kBorder;
        const float fh = h > 0.f ? h : want.height + JMdiChild::kTitleH + JMdiChild::kBorder;
        // `a` is empty until the first layout, and fitting against an empty area would open every child at
        // nothing wide. Fit only against an area that exists; the provisional heal below fits the rest once
        // there is something to fit into.
        const bool haveArea = a.width > 0.f && a.height > 0.f;
        JRect f = haveArea ? jMdiFitted(hinted ? fw : std::max(320.f, a.width  * 0.66f),
                                    hinted ? fh : std::max(200.f, a.height * 0.66f), a)
                           : JRect{ a.x + 24.f, a.y + 24.f, fw, fh };
        auto c = std::make_unique<JMdiChild>(std::move(title), content, f);
        c->m_wantW = hinted ? fw : 0.f; c->m_wantH = hinted ? fh : 0.f;
        // AND THAT SIZE IS ALSO THE FLOOR. TunerStudio will not let a table window be dragged smaller
        // than the grid inside it — measured: 1195 px wide down to 925 and it stops, while a window of
        // plain rows shrinks freely and scrolls. That is the honest rule for content that cannot usefully
        // compress: refuse, rather than hand back something unreadable. Capped by the area, since a floor
        // bigger than the space it lives in would be a window that cannot be resized at all.
        if (hinted && haveArea)
            c->setMinSize(std::min(fw, a.width), std::min(fh, a.height));
        c->m_max   = !hinted;      // a size we can trust is a size to open at, not to override
        // An area of no size means the layout has not run yet (a child opened during construction), so
        // `f` above is a rect measured against nothing. Flag it and let the first real frame place it.
        c->m_provisional = (a.width <= 0.f || a.height <= 0.f);
        if (c->m_max) c->m_frame = a;   // no usable hint: all of it. m_restore keeps the free geometry.
        // PARENT THE CONTENT, which is how a host says what is inside it. JWidget::collectChildren is the
        // one edge every tree walk follows — focus traversal above all — and a widget merely POINTED at is
        // not on it. The area routed presses to its content by hand, so the mouse worked and hid the gap:
        // the keyboard could never reach anything in a child window, and focus given to the content was
        // dropped again by the next syncOrder() because nothing could walk to it. Non-owning: the
        // application still owns the widget, exactly as it did before.
        if (content) addChild(content);
        JMdiChild* raw = c.get();
        m_children.push_back(std::move(c));
        return raw;
    }

    void close(JMdiChild* c) {
        m_children.erase(std::remove_if(m_children.begin(), m_children.end(),
                                        [c](const std::unique_ptr<JMdiChild>& p) { return p.get() == c; }),
                         m_children.end());
    }
    void closeAll() {
        for (auto& c : m_children) if (c->content()) removeChild(c->content());   // drop the parent edges too
        m_children.clear();
    }

    // Front-most, which is the one a menu action means by "the open page".
    JMdiChild* active() const { return m_children.empty() ? nullptr : m_children.back().get(); }
    size_t     count()  const { return m_children.size(); }

    void raise(JMdiChild* c) {
        for (size_t i = 0; i < m_children.size(); ++i)
            if (m_children[i].get() == c) {
                auto p = std::move(m_children[i]);
                m_children.erase(m_children.begin() + static_cast<long>(i));
                m_children.push_back(std::move(p));
                return;
            }
    }

    JRect area() const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        return { b.x, b.y, b.width, b.height };
    }

    // ---- input ---------------------------------------------------------------------------------
    void handleMouseMove(float mx, float my) override {
        // A DRAG CANNOT OUTLIVE THE BUTTON. The frame publishes the real button state every tick, so a
        // release this widget never saw (something upstream swallowed it) heals here instead of leaving
        // the window glued to the cursor until the next click — the "mouse sticking" that made a window
        // keep dragging after it had been let go.
        if (m_drag != Drag::None && !JWidget::s_leftDown) { m_drag = Drag::None; m_grab = nullptr; m_edge = 0; }
        if (m_drag != Drag::None && m_grab) {
            // THE POINTER IS CLAMPED TO THE AREA FOR THE LENGTH OF THE DRAG. Clipping stops a window
            // PAINTING over the menu and the docks, but on its own it lets a window be pushed until
            // there is nothing of it left inside the area — dragged out of sight, with no title bar in
            // reach to drag it back. Losing a window is worse than being unable to shove it that far.
            // Clamping the pointer rather than the frame is what makes that stop feel right: motion
            // beyond the edge simply stops moving the window, and coming back in resumes from the edge
            // with no jump, so the window is always still grabbable where it was left.
            const JRect a = area();
            const float px = std::clamp(mx, a.x, a.x + a.width);
            const float py = std::clamp(my, a.y, a.y + a.height);
            JRect f = m_grab->frame();
            const float dx = px - m_lastX, dy = py - m_lastY;
            if (m_drag == Drag::Move) { f.x += dx; f.y += dy; }
            else {
                // THE FAR EDGE STAYS PUT. Dragging the left edge moves the origin and shrinks the width by
                // the same amount, so clamping the width afterwards left the origin where the pointer had
                // pushed it: at the minimum size the window stopped shrinking and started sliding sideways
                // instead, out from under the cursor that was resizing it. Clamp the pair together.
                const float minW = m_grab->minW(), minH = m_grab->minH();
                if (m_edge & Left)   { const float nw = std::max(minW, f.width  - dx);
                                       f.x += f.width  - nw; f.width  = nw; }
                if (m_edge & Right)  {                       f.width  = std::max(minW, f.width  + dx); }
                if (m_edge & Top)    { const float nh = std::max(minH, f.height - dy);
                                       f.y += f.height - nh; f.height = nh; }
                if (m_edge & Bottom) {                       f.height = std::max(minH, f.height + dy); }
            }
            m_grab->setFrame(f);
            m_lastX = px; m_lastY = py;
            return;
        }
        if (JMdiChild* c = childAt(mx, my); c && c->content()) {
            c->content()->setBounds(c->contentRect());
            c->content()->handleMouseMove(mx, my);
        } else if (m_background) {
            m_background->setBounds(area());
            m_background->handleMouseMove(mx, my);
        }
    }

    void handleMousePress(float mx, float my) override {
        JMdiChild* c = childAt(mx, my);
        if (!c) {                                   // nothing over it: the background has the click
            if (m_background) { m_background->setBounds(area()); m_background->handleMousePress(mx, my); }
            return;
        }
        raise(c);
        // CLICKING A WINDOW FOCUSES WHAT IS IN IT, which is what clicking a window does everywhere. The
        // runner's focus-on-click runs before this and hit-tests the FOCUS ORDER; content hosted by a
        // window is not in it, so a press inside a child window was read as a press on nothing and focus
        // was cleared — which blurred the very content being clicked. In the studio that meant a canvas
        // control took the keyboard on press and lost it again in the same frame, so a field could be
        // clicked into and never typed into. Focus goes to the content BEFORE the press reaches it, so
        // whatever the press then focuses inside the content is the thing that keeps it.
        if (JFocusManager::s_active && c->content()) JFocusManager::s_active->setFocus(c->content());
        const JRect f = c->frame();
        // The buttons first — they sit in the title bar and would otherwise start a move.
        if (my >= f.y && my < f.y + JMdiChild::kTitleH) {
            const float bx = f.x + f.width - 6.f;
            if (mx >= bx - JMdiChild::kBtn && mx < bx)                       { c->requestClose(); return; }
            if (mx >= bx - 2 * JMdiChild::kBtn - 6.f && mx < bx - JMdiChild::kBtn - 6.f) {
                c->setMaximised(!c->maximised(), area()); return;
            }
            if (!c->maximised()) { m_drag = Drag::Move; m_grab = c; m_lastX = mx; m_lastY = my; }
            return;
        }
        if (!c->maximised()) {
            if (const int e = edgeAt(*c, mx, my)) {
                m_drag = Drag::Size; m_edge = e; m_grab = c; m_lastX = mx; m_lastY = my; return;
            }
        }
        if (c->content()) { c->content()->setBounds(c->contentRect()); c->content()->handleMousePress(mx, my); }
    }

    // AND SO DO THE KEYS. The runner routes a key to the central widget, the central widget is this, and
    // this had no handler — so nothing inside a child window could be typed into. A text field took its
    // caret and then sat there while the keyboard went nowhere, which is not a text field. They go to the
    // ACTIVE child (the front one, the one whose title bar is lit), because that is what "focused window"
    // means everywhere else.
    bool handleKeyEvent(const JKeyEvent& ke) override {
        JMdiChild* c = active();
        if (!c || !c->content()) return m_background ? m_background->handleKeyEvent(ke) : false;
        c->content()->setBounds(c->contentRect());
        return c->content()->handleKeyEvent(ke);
    }

    // THE WHEEL GOES TO THE CONTENT. Without this the area silently ate every scroll that landed on a
    // child: the runner routes the wheel to the central widget, the central widget is this, and this had
    // no handler — so a page whose content overflowed could not be scrolled at all, wheel or scrollbar.
    bool handleScroll(float mx, float my, float wheel) override {
        JMdiChild* c = childAt(mx, my);
        if (!c || !c->content()) {
            if (!m_background) return false;
            m_background->setBounds(area());
            return m_background->handleScroll(mx, my, wheel);
        }
        c->content()->setBounds(c->contentRect());
        return c->content()->handleScroll(mx, my, wheel);
    }

    void handleMouseRelease(float mx, float my) override {
        if (m_drag != Drag::None) { m_drag = Drag::None; m_grab = nullptr; m_edge = 0; return; }
        if (JMdiChild* c = childAt(mx, my); c && c->content()) {
            c->content()->setBounds(c->contentRect());
            c->content()->handleMouseRelease(mx, my);
        } else if (m_background) {
            m_background->setBounds(area());
            m_background->handleMouseRelease(mx, my);
        }
    }

    // ---- paint ---------------------------------------------------------------------------------
    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        // CLOSE ACTUALLY CLOSES. The button set a flag and nothing ever read it, so the only thing the ✕
        // did was mark the window as wanting to go. Reaped here, at the top of a frame — outside every
        // event handler, so an owner told about it (onChildClosed) can drop the content widget without
        // pulling it out from under a call that is still running.
        for (size_t i = 0; i < m_children.size();) {
            if (!m_children[i]->closeRequested()) { ++i; continue; }
            std::unique_ptr<JMdiChild> gone = std::move(m_children[i]);
            m_children.erase(m_children.begin() + static_cast<long>(i));
            if (gone->content()) removeChild(gone->content());   // the parent edge goes with the window
            if (onChildClosed) onChildClosed(gone.get());
        }
        const JRect a = area();
        buf.pushRectangle(a.x, a.y, a.width, a.height, Colors::Surface0);
        if (m_background && a.width > 1.f && a.height > 1.f) {   // behind every window, filling the area
            buf.pushClip(a.x, a.y, a.width, a.height);
            m_background->setBounds(a);
            m_background->populateRenderPrimitives(buf);
            buf.popClip();
        }
        // A maximised child owns the area even when the area changes under it.
        for (auto& c : m_children) c->refit(a);
        // CLIPPED TO THE AREA. A child is drawn wherever its frame is, and the frame is not the area —
        // so without this a window dragged upward paints its title bar straight over the menu and the
        // toolbar, and one dragged sideways paints over the docks. The content was already clipped to the
        // child; the child was never clipped to its own area.
        buf.pushClip(a.x, a.y, a.width, a.height);
        // Back to front, so the front-most child is the one drawn over the others.
        for (auto& c : m_children) paintChild(buf, *c, c.get() == active());
        buf.popClip();
    }

private:
    enum Edge { Left = 1, Right = 2, Top = 4, Bottom = 8 };
    enum class Drag { None, Move, Size };

    // CLIPPED, NOT CONFINED. A frame may go where it is put — half off the left edge, most of the way
    // under the bottom — and the AREA decides what is visible of it. Clamping a window inside its parent
    // is the easy answer and the wrong one: it fights the drag, it makes a window that is bigger than the
    // area impossible to move around inside, and it takes away the ordinary act of pushing something
    // mostly out of the way while you look at what is behind it. See populateRenderPrimitives for the
    // clip that makes this safe.

    static bool hit(const JRect& r, float x, float y) {
        return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
    }

    // Front-most child under the pointer — the same order input is routed in.
    JMdiChild* childAt(float mx, float my) const {
        for (size_t i = m_children.size(); i-- > 0; )
            if (hit(m_children[i]->frame(), mx, my)) return m_children[i].get();
        return nullptr;
    }

    static int edgeAt(const JMdiChild& c, float mx, float my) {
        const JRect f = c.frame();
        const float b = JMdiChild::kGrab;
        int e = 0;
        if (mx <= f.x + b)                 e |= Left;
        if (mx >= f.x + f.width  - b)      e |= Right;
        if (my <= f.y + b)                 e |= Top;
        if (my >= f.y + f.height - b)      e |= Bottom;
        return e;
    }

    void paintChild(JPrimitiveBuffer& buf, JMdiChild& c, bool activeOne) {
        const JRect f = c.frame();
        const float rad = JStyle::current().cornerRadius;
        // THE FRAMEWORK'S OWN WINDOW CHROME, not a colour invented here. Colors::Accent made the focused
        // window a blue-outlined thing that matched nothing else on screen; WindowFrameBorder is the token
        // the theme already defines for a window frame (and re-defines per theme), so a child window is
        // bordered like a window in whichever theme is loaded. Focus stays legible through weight — the
        // active frame is drawn heavier, the inactive one falls back to the generic surface border.
        // AND THE SURROUND IS CHROME, not more content. Filling the frame with Surface1 put the band
        // around the content in very nearly the content's own colour, so the only thing marking the
        // bottom and sides of the window was the hairline stroke — nothing like the border of a real
        // window. Filling it with the title-bar colour instead makes the title bar and the surround one
        // continuous piece of frame, which is what the eye reads as a window.
        buf.pushRectangle(f.x, f.y, f.width, f.height, Colors::TitleBar, rad);
        JTitleBar::draw(buf, f.x, f.y, f.width, JMdiChild::kTitleH, c.title(), rad, 0, 10.f,
                        2 * JMdiChild::kBtn + 12.f);
        // Restore/maximise, then close — drawn where handleMousePress looks for them.
        const float bx = f.x + f.width - 6.f, by = f.y + (JMdiChild::kTitleH - JMdiChild::kBtn) * 0.5f;
        drawGlyph(buf, bx - 2 * JMdiChild::kBtn - 6.f, by, c.maximised() ? Glyph::Restore : Glyph::Maximise);
        drawGlyph(buf, bx - JMdiChild::kBtn, by, Glyph::Close);
        if (JWidget* w = c.content()) {
            const JRect cr = c.contentRect();
            if (cr.width > 1.f && cr.height > 1.f) {
                buf.pushClip(cr.x, cr.y, cr.width, cr.height);
                w->setBounds(cr);
                w->populateRenderPrimitives(buf);
                buf.popClip();
            }
        }
        // The corner grip: three lines stepping up the diagonal, the shape every resizable corner has
        // used for thirty years and the only hint this frame can give without a cursor to change.
        //
        // DRAWN AFTER THE CONTENT, which is why it was invisible. The grip reaches further in than the
        // border it sits on (it has to — a 6 px target is not a target), so it lands inside the content
        // rect; drawn before the content, the content simply painted over it every frame and the corner
        // looked like any other corner. It goes on top, like the grip of a real window.
        if (!c.maximised()) {
            const uint8_t* g = Colors::WindowFrameBorder;
            const float x1 = f.x + f.width - 3.f, y1 = f.y + f.height - 3.f;
            for (int i = 1; i <= 3; ++i) {
                const float d = static_cast<float>(i) * 4.f;   // three steps in from the corner
                buf.pushRectangle(x1 - d, y1 - 2.f, d, 2.f, g);       // the horizontal leg
                buf.pushRectangle(x1 - 2.f, y1 - d, 2.f, d, g);       // and the vertical one
            }
        }
        // THE BORDER GOES ON LAST, AROUND EVERYTHING. Stroked with the frame fill, it was drawn and then
        // immediately painted over along the top: the title bar covers the frame edge to edge, so the one
        // stretch of border that frames the title — the part that says where the window starts — was the
        // only stretch missing. Drawn over the finished window instead, it encloses the title bar, the
        // content and the grip, which is what a border is.
        static constexpr uint8_t kNoFill[4] = {0, 0, 0, 0};
        buf.pushRectangle(f.x, f.y, f.width, f.height, kNoFill, rad,
                          activeOne ? 2.0f : 1.0f,
                          activeOne ? Colors::WindowFrameBorder : Colors::Border);
    }

    enum class Glyph { Close, Maximise, Restore };

    // BUILT FROM WHAT THE BUFFER ACTUALLY HAS: rectangles and text. There is no line primitive, and a
    // null fill is not "no fill" — it is a crash. A hollow square is a transparent fill with a border,
    // and the close mark is the same UTF-8 multiply sign the tab bar closes with, so it is a glyph the
    // atlas is already carrying.
    static void drawGlyph(JPrimitiveBuffer& buf, float x, float y, Glyph g) {
        const float s = JMdiChild::kBtn, m = 4.f;
        static const uint8_t clear[4] = {0, 0, 0, 0};
        const uint8_t* col = Colors::TitleBarText;
        if (g == Glyph::Close) {
            if (JTextHelper::hasAtlas()) {
                const float w = JTextHelper::measureWidth("\xC3\x97"), lh = JTextHelper::lineHeight();
                JTextHelper::pushText(buf, x + (s - w) * 0.5f, y + (s - lh) * 0.5f, "\xC3\x97", col);
            }
            return;
        }
        if (g == Glyph::Maximise) {                       // one hollow square: "fill the area"
            buf.pushRectangle(x + m, y + m, s - 2 * m, s - 2 * m, clear, 0.f, 1.2f, col);
            return;
        }
        // Restore: a small square with a second one behind it, offset — "give it back its own size".
        buf.pushRectangle(x + m + 3.f, y + m, s - 2 * m - 3.f, s - 2 * m - 3.f, clear, 0.f, 1.2f, col);
        buf.pushRectangle(x + m, y + m + 3.f, s - 2 * m - 3.f, s - 2 * m - 3.f, clear, 0.f, 1.2f, col);
    }

    JWidget* m_background{nullptr};                 // see setBackground()
    std::vector<std::unique_ptr<JMdiChild>> m_children;
    JMdiChild* m_grab{nullptr};
    Drag  m_drag{Drag::None};
    int   m_edge{0};
    float m_lastX{0.f}, m_lastY{0.f};
};

}  // inline namespace jf
