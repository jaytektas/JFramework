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

#include <iostream>
#include <string>

using namespace jf;

// A scrap of content, so a child window has something in it that is plainly not the frame.
class Pad : public JWidget {
public:
    Pad(JSceneGraph& g, std::string text, const uint8_t* fill)
        : JWidget(g, "Pad"), m_text(std::move(text)), m_fill(fill) {}
    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const JRect& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        buf.pushRectangle(b.x, b.y, b.width, b.height, m_fill);
        if (JTextHelper::hasAtlas())
            JTextHelper::pushText(buf, b.x + 12.f, b.y + 12.f, m_text, Colors::TextPrimary, b.width - 24.f);
    }
private:
    std::string    m_text;
    const uint8_t* m_fill;
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

    Pad one(g, "First page. Drag my title bar off every edge of the area.",  Colors::Surface0);
    Pad two(g, "Second page. Click me to raise; the active frame is heavier.", Colors::Surface0);
    JMdiChild* a = mdi.open("First Page",  &one);
    JMdiChild* b = mdi.open("Second Page", &two);
    // Both restored rather than maximised: a maximised child has nothing to drag or size, and the
    // geometry paths are the ones under test.
    a->setMaximised(false, mdi.area());
    b->setMaximised(false, mdi.area());
    JRect fb = b->frame(); fb.x += 60.f; fb.y += 60.f; b->setFrame(fb);

    std::cerr << "[mdi_demo] two child windows in the centre\n";
    return win.run();
}
