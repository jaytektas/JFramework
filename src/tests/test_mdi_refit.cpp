// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// A child window follows its area's SIZE, not only its corner: a strip appearing across the top of the
// app (a notice) shrinks the area, and the window opened before it must fit inside; the strip going must
// give the window back the size its content asked for. A window resized by hand keeps its size.
#include <j/core/MdiArea.h>
#include <cstdio>
#include <cmath>
using namespace jf;
static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}
static bool feq(float a, float b) { return std::fabs(a - b) < 0.5f; }

struct Page : JWidget {
    float w, h;
    Page(JSceneGraph& g, float w_, float h_) : JWidget(g, "page"), w(w_), h(h_) {}
    JRect preferredSize() const override { return { 0.f, 0.f, w, h }; }
    void populateRenderPrimitives(JPrimitiveBuffer&) override {}
};

static void frame(JMdiArea& m) { JPrimitiveBuffer buf; m.populateRenderPrimitives(buf); }

int main() {
    JSceneGraph g;
    const float chromeH = JMdiChild::kTitleH + JMdiChild::kBorder;

    std::puts("1. a page taller than the area: the area shrinks under it, then grows back");
    {
        JMdiArea mdi(g);
        mdi.setBounds({ 0.f, 0.f, 1000.f, 700.f });
        Page p(g, 600.f, 900.f);                       // wants 900 + chrome: more than 700
        JMdiChild* c = mdi.open("page", &p);
        frame(mdi);
        check(!c->maximised(), "opened as an ordinary window (narrower than the area)");
        check(feq(c->frame().height, 700.f), "opened clamped to the area's height");

        mdi.setBounds({ 0.f, 40.f, 1000.f, 660.f });   // a notice strip appears above
        frame(mdi);
        check(feq(c->frame().y, 40.f), "moved down with the area's corner");
        check(feq(c->frame().y + c->frame().height, 700.f), "fits inside the shorter area (not off the bottom)");

        mdi.setBounds({ 0.f, 0.f, 1000.f, 700.f });    // the strip goes
        frame(mdi);
        check(feq(c->frame().y, 0.f), "moved back up with the corner");
        check(feq(c->frame().height, 700.f), "grew back to the full area height");
    }

    std::puts("1b. opened WHILE the strip is up (key off), then the strip goes (key on)");
    {
        JMdiArea mdi(g);
        mdi.setBounds({ 0.f, 40.f, 1000.f, 660.f });
        Page p(g, 600.f, 900.f);
        JMdiChild* c = mdi.open("page", &p);
        frame(mdi);
        check(feq(c->frame().height, 660.f), "opened clamped to the short area");
        mdi.setBounds({ 0.f, 0.f, 1000.f, 700.f });
        frame(mdi);
        check(feq(c->frame().y, 0.f) && feq(c->frame().height, 700.f), "grew to the full area, not left short");
    }

    std::puts("2. a page that fits keeps its own size when the area grows");
    {
        JMdiArea mdi(g);
        mdi.setBounds({ 0.f, 0.f, 1000.f, 700.f });
        Page p(g, 400.f, 300.f);
        JMdiChild* c = mdi.open("page", &p);
        frame(mdi);
        mdi.setBounds({ 0.f, 0.f, 1000.f, 900.f });
        frame(mdi);
        check(feq(c->frame().height, 300.f + chromeH), "still the size its content asked for");
    }

    std::puts("3. a window resized by hand keeps the reader's size");
    {
        JMdiArea mdi(g);
        mdi.setBounds({ 0.f, 0.f, 1000.f, 700.f });
        Page p(g, 600.f, 900.f);
        JMdiChild* c = mdi.open("page", &p);
        frame(mdi);
        const JRect f = c->frame();
        const float bx = f.x + f.width - 2.f, by = f.y + f.height - 2.f;   // bottom-right corner
        JWidget::s_leftDown = true;
        mdi.handleMousePress(bx, by);
        mdi.handleMouseMove(bx, by - 200.f);
        mdi.handleMouseRelease(bx, by - 200.f);
        JWidget::s_leftDown = false;
        check(feq(c->frame().height, 500.f), "the corner drag resized it");
        mdi.setBounds({ 0.f, 0.f, 1000.f, 800.f });
        frame(mdi);
        check(feq(c->frame().height, 500.f), "an area change leaves a hand-sized window alone");
    }

    std::printf("%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}
