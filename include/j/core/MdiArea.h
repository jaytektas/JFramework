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
#include "JStyle.h"
#include "JTitleBar.h"
#include "JTextHelper.h"
#include "../graphics/RenderPrimitive.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

inline namespace jf {

class JMdiChild {
public:
    static constexpr float kTitleH = 26.f;
    static constexpr float kBorder = 4.f;     // the grab band on each edge
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
    void refit(const JRect& area) { if (m_max) m_frame = area; }

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
    bool        m_max{true}, m_close{false};
};

class JMdiArea : public JWidget {
public:
    explicit JMdiArea(JSceneGraph& g) : JWidget(g, "JMdiArea") {}

    // Open a child, maximised, at the front. `w`/`h` are the size it restores to; 0 means "a good
    // fraction of the area", which is what a first restore should give rather than a 1x1 sliver.
    JMdiChild* open(std::string title, JWidget* content, float w = 0.f, float h = 0.f) {
        const JRect a = area();
        JRect f{ a.x + 24.f, a.y + 24.f,
                 w > 0.f ? w : std::max(320.f, a.width  * 0.66f),
                 h > 0.f ? h : std::max(200.f, a.height * 0.66f) };
        auto c = std::make_unique<JMdiChild>(std::move(title), content, f);
        c->m_frame = a;            // opens maximised; m_restore keeps the free geometry above
        JMdiChild* raw = c.get();
        m_children.push_back(std::move(c));
        return raw;
    }

    void close(JMdiChild* c) {
        m_children.erase(std::remove_if(m_children.begin(), m_children.end(),
                                        [c](const std::unique_ptr<JMdiChild>& p) { return p.get() == c; }),
                         m_children.end());
    }
    void closeAll() { m_children.clear(); }

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
        if (m_drag != Drag::None && m_grab) {
            JRect f = m_grab->frame();
            const float dx = mx - m_lastX, dy = my - m_lastY;
            if (m_drag == Drag::Move) { f.x += dx; f.y += dy; }
            else {
                if (m_edge & Left)   { f.x += dx; f.width  -= dx; }
                if (m_edge & Right)  {            f.width  += dx; }
                if (m_edge & Top)    { f.y += dy; f.height -= dy; }
                if (m_edge & Bottom) {            f.height += dy; }
                f.width  = std::max(f.width,  m_grab->minW());
                f.height = std::max(f.height, m_grab->minH());
            }
            m_grab->setFrame(f);
            m_lastX = mx; m_lastY = my;
            return;
        }
        if (JMdiChild* c = childAt(mx, my); c && c->content()) {
            c->content()->setBounds(c->contentRect());
            c->content()->handleMouseMove(mx, my);
        }
    }

    void handleMousePress(float mx, float my) override {
        JMdiChild* c = childAt(mx, my);
        if (!c) return;
        raise(c);
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

    void handleMouseRelease(float mx, float my) override {
        if (m_drag != Drag::None) { m_drag = Drag::None; m_grab = nullptr; m_edge = 0; return; }
        if (JMdiChild* c = childAt(mx, my); c && c->content()) {
            c->content()->setBounds(c->contentRect());
            c->content()->handleMouseRelease(mx, my);
        }
    }

    // ---- paint ---------------------------------------------------------------------------------
    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const JRect a = area();
        buf.pushRectangle(a.x, a.y, a.width, a.height, Colors::Surface0);
        // A maximised child owns the area even when the area changes under it.
        for (auto& c : m_children) c->refit(a);
        // Back to front, so the front-most child is the one drawn over the others.
        for (auto& c : m_children) paintChild(buf, *c, c.get() == active());
    }

private:
    enum Edge { Left = 1, Right = 2, Top = 4, Bottom = 8 };
    enum class Drag { None, Move, Size };

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
        const float b = JMdiChild::kBorder + 2.f;
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
        buf.pushRectangle(f.x, f.y, f.width, f.height, Colors::Surface1, rad,
                          activeOne ? 1.5f : 1.0f, activeOne ? Colors::Accent : Colors::Border);
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

    std::vector<std::unique_ptr<JMdiChild>> m_children;
    JMdiChild* m_grab{nullptr};
    Drag  m_drag{Drag::None};
    int   m_edge{0};
    float m_lastX{0.f}, m_lastY{0.f};
};

}  // inline namespace jf
