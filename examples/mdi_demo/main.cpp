// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// JMdiArea in a real application frame — child windows in the protected centre, with a toolbar,
// a status bar and docks around them.
//
// The chrome is the point. An MDI child is dragged and sized by hand, which means every one of its
// gestures ends up somewhere the area does not own: over the toolbar, off the bottom, across a dock.
// Those are exactly the paths that broke and cannot be seen in a static screenshot of a window
// sitting still — so this demo exists to have something to drag:
//
//   * drag a title bar up over the toolbar and the menu bar. The window keeps following the cursor
//     (chrome must not swallow a gesture the centre owns) and STOPS at the area edge, so it can never
//     be pushed somewhere it cannot be grabbed back from.
//   * let go while the cursor is over the toolbar or the status strip. The window stays put, and the
//     toolbar button under the cursor does NOT fire.
//   * restore a window and size it from the corner grip, which is drawn over the content so it can
//     actually be seen.
//
// Two children, one behind the other, so raise-on-click and the active/inactive frame are visible.

#include <j/app/JAppWindow.h>
#include <j/core/MdiArea.h>
#include <j/core/JLabel.h>
#include <j/core/JButton.h>

#include <algorithm>
#include <iostream>
#include <string>

using namespace jf;

// A scrap of content, so a child window has something in it that is plainly not the frame.
class Pad : public JWidget {
public:
    Pad(JSceneGraph& g, std::string text, float w, float h)
        : JWidget(g, "Pad"), m_text(std::move(text)), m_w(w), m_h(h) {}

    // The size this content wants: the window opens around it, so nothing is clipped and there is
    // nothing to scroll — which is what a page window does in every application that has them.
    JRect preferredSize() const override { return { 0.f, 0.f, m_w, m_h }; }

    // Scrolled by the wheel when the window is smaller than that. Proves the wheel reaches the content
    // through the area rather than being eaten on the way.
    bool handleScroll(float, float, float wheel) override {
        const JRect& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const float hidden = std::max(0.f, m_h - b.height);
        if (hidden <= 0.f) return false;
        m_scroll = std::clamp(m_scroll - wheel * 24.f, 0.f, hidden);
        return true;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const JRect& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface0);
        // Ruled rows, so a scroll is unmistakable: the numbers move, the window does not.
        for (int i = 0; i * 28.f < m_h; ++i) {
            const float y = b.y + static_cast<float>(i) * 28.f - m_scroll;
            if (y < b.y - 28.f || y > b.y + b.height) continue;
            if (JTextHelper::hasAtlas())
                JTextHelper::pushText(buf, b.x + 12.f, y + 6.f,
                                      "row " + std::to_string(i) + "  -  " + m_text,
                                      Colors::TextPrimary, b.width - 24.f);
        }
    }
private:
    std::string m_text;
    float       m_w, m_h, m_scroll{0.f};
};

int main() {
    JGuiApplication app;
    // The light scheme, because that is the one the frame's border has to hold up in — a near-black
    // frame on near-black content hides exactly the problem this demo is for.
    JStyle::apply(JStyle::light());
    JAppWindow win("JFramework MDI", 1000, 640);
    if (!win.valid()) { std::cerr << "window/HAL init failed\n"; return -1; }
    auto& g = app.sceneGraph();

    // Chrome all round the centre: these are the things that used to eat a drag.
    win.toolBar().addButton("New",  []{ std::cerr << "[toolbar] New fired\n";  });
    win.toolBar().addButton("Open", []{ std::cerr << "[toolbar] Open fired\n"; });
    win.toolBar().addSeparator();
    win.toolBar().addButton("Burn", []{ std::cerr << "[toolbar] Burn fired\n"; });
    win.statusBar().showMessage("Drag a window over the toolbar and let go - nothing up there may fire.");

    auto& space = win.dockSpace();
    JDockWidget nav("Navigation", 0.f, 0.f, 200.f, 160.f);
    space.left().addDock(&nav);

    JMdiArea mdi(g);
    win.setCentralWidget(&mdi);

    // Two different content sizes, so it is obvious the window is sized by what is IN it: the short
    // page opens small, the long one opens as tall as the area allows and scrolls the rest.
    Pad one(g, "short page", 420.f, 220.f);
    Pad two(g, "long page",  520.f, 1400.f);
    mdi.open("Short Page", &one);
    JMdiChild* b = mdi.open("Long Page", &two);
    JRect fb = b->frame(); fb.x += 300.f; fb.y += 40.f; b->setFrame(fb);

    std::cerr << "[mdi_demo] two child windows in the centre\n";
    return win.run();
}
