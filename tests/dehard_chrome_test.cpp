// dehard_chrome_test — BYTE-EXACT no-regression gate for the chrome/container colour/size de-hardcoding.
//
// Each container/chrome widget touched by the pass is constructed with a JSceneGraph, rendered into a
// JPrimitiveBuffer under the DEFAULT (dark) theme, and the emitted fill / border / text colours are
// asserted against the EXACT OLD literal bytes that were hardcoded before migration (the expected value
// in every assertion IS the recorded pre-migration literal). We also assert every NEW JTheme role added
// by the pass equals its old bytes under the default theme. PASS/FAIL per case; non-zero exit on any fail.

#include <j/core/JTabWidget.h>
#include <j/core/JTabBar.h>
#include <j/core/JListView.h>
#include <j/core/JTreeView.h>
#include <j/core/JDataGrid.h>
#include <j/core/JGroupBox.h>
#include <j/core/JScrollArea.h>
#include <j/core/JButton.h>
#include <j/core/JTheme.h>

#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>

using namespace jf;

static int g_pass = 0, g_fail = 0;

static void check(const char* name, bool cond) {
    if (cond) { ++g_pass; std::printf("PASS  %s\n", name); }
    else      { ++g_fail; std::printf("FAIL  %s\n", name); }
}

static bool eq4(const uint8_t c[4], uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return c[0]==r && c[1]==g && c[2]==b && c[3]==a;
}
static bool hasText(const JPrimitiveBuffer& buf, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (const auto& cmd : buf.getCommands())
        if (cmd.kind == JPrimitiveBuffer::JDrawCommand::JKind::Text && eq4(cmd.text.color, r,g,b,a)) return true;
    return false;
}
static bool hasFill(const JPrimitiveBuffer& buf, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (const auto& cmd : buf.getCommands())
        if (cmd.kind == JPrimitiveBuffer::JDrawCommand::JKind::JRect && eq4(cmd.rect.color, r,g,b,a)) return true;
    return false;
}
static bool hasBorder(const JPrimitiveBuffer& buf, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (const auto& cmd : buf.getCommands())
        if (cmd.kind == JPrimitiveBuffer::JDrawCommand::JKind::JRect && eq4(cmd.rect.borderColor, r,g,b,a)) return true;
    return false;
}

// Synthetic font atlas so text-emitting branches run.
static void installTestAtlas() {
    JFontAtlas a;
    a.valid = true; a.width = 512; a.height = 256;
    a.pixelSize = 14.f; a.ascent = 11.f; a.descent = -3.f; a.lineHeight = 16.f;
    for (uint32_t cp = 32; cp <= 126; ++cp) {
        JGlyphInfo g{};
        g.u0 = 0; g.v0 = 0; g.u1 = 0.01f; g.v1 = 0.01f;
        g.pixelW = 5.f; g.pixelH = 8.f; g.bearingX = 0.f; g.bearingY = -8.f; g.advanceX = 6.f;
        a.glyphs[cp] = g;
    }
    JTextHelper::setAtlas(std::move(a));
}

int main() {
    JTheme::apply(JTheme::dark());   // default theme = byte-exact baseline
    installTestAtlas();

    // ---- NEW roles: each default must equal the exact old literal bytes under the default theme ----
    {
        const JTheme& t = JTheme::current();
        check("role.ScrollTrack        == {30,30,35,120}",   eq4(t.ScrollTrack,        30,30,35,120));
        check("role.ScrollThumb        == {100,100,110,200}",eq4(t.ScrollThumb,        100,100,110,200));
        check("role.ScrollThumbActive  == {130,130,140,200}",eq4(t.ScrollThumbActive,  130,130,140,200));
        check("role.ScrollAreaBg       == {20,20,24,255}",   eq4(t.ScrollAreaBg,       20,20,24,255));
        check("role.TabGhostFill       == {40,40,50,180}",   eq4(t.TabGhostFill,       40,40,50,180));
        check("role.TabGhostBorder     == {80,130,255,200}", eq4(t.TabGhostBorder,     80,130,255,200));
        check("role.TabGhostBar        == {200,210,255,180}",eq4(t.TabGhostBar,        200,210,255,180));
        check("role.TabTearDot         == {80,80,100,120}",  eq4(t.TabTearDot,         80,80,100,120));
        check("role.TabInactiveText    == {140,140,148,255}",eq4(t.TabInactiveText,    140,140,148,255));
        check("role.DockTabInactiveText== {150,150,158,255}",eq4(t.DockTabInactiveText,150,150,158,255));
        check("role.TreeEditText       == {225,225,232,255}",eq4(t.TreeEditText,       225,225,232,255));
        check("role.TreeIconTable      == {220,150,60,255}", eq4(t.TreeIconTable,      220,150,60,255));
        check("role.TreeIconConfig     == {90,150,230,255}", eq4(t.TreeIconConfig,     90,150,230,255));
        check("role.TreeIconToggle     == {90,200,130,255}", eq4(t.TreeIconToggle,     90,200,130,255));
        check("role.TreeIconEnum       == {175,130,225,255}",eq4(t.TreeIconEnum,       175,130,225,255));
        check("role.TreeIconCurve      == {90,200,220,255}", eq4(t.TreeIconCurve,      90,200,220,255));
        check("role.RowAltBg           == {34,34,36,120}",   eq4(t.RowAltBg,           34,34,36,120));
        check("role.GridLine           == {50,50,54,180}",   eq4(t.GridLine,           50,50,54,180));
        check("role.GridHeaderText     == {230,230,240,255}",eq4(t.GridHeaderText,     230,230,240,255));
        check("role.GroupPanelFill     == {22,22,25,180}",   eq4(t.GroupPanelFill,     22,22,25,180));
        check("role.GroupTitleBg       == {36,36,40,200}",   eq4(t.GroupTitleBg,       36,36,40,200));
        check("role.DockContentBg      == {14,14,16,255}",   eq4(t.DockContentBg,      14,14,16,255));
        check("role.DockPinIdle        == {50,50,60,255}",   eq4(t.DockPinIdle,        50,50,60,255));
        check("role.DockResizeIdle     == {70,70,80,160}",   eq4(t.DockResizeIdle,     70,70,80,160));
        check("role.DockResizeHot      == {140,140,160,240}",eq4(t.DockResizeHot,      140,140,160,240));
        check("role.DockCloseMark      == {235,235,240,210}",eq4(t.DockCloseMark,      235,235,240,210));
        check("role.DockSplitLine      == {60,60,64,255}",   eq4(t.DockSplitLine,      60,60,64,255));
        check("role.DropArrowBg        == {40,40,46,170}",   eq4(t.DropArrowBg,        40,40,46,170));
        check("role.DropArrowBorder    == {90,90,96,180}",   eq4(t.DropArrowBorder,    90,90,96,180));
        check("role.WindowTitleFill    == {30,30,32,255}",   eq4(t.WindowTitleFill,    30,30,32,255));
        check("role.WindowFrameBorder  == {60,60,65,255}",   eq4(t.WindowFrameBorder,  60,60,65,255));
        check("role.FloatTitleBarBg    == {28,28,32,255}",   eq4(t.FloatTitleBarBg,    28,28,32,255));
        check("role.FloatSeparator     == {50,50,55,255}",   eq4(t.FloatSeparator,     50,50,55,255));
    }

    // ---- JTabWidget (already role-based container): active label = TextPrimary {240,240,245,255} ----
    {
        JSceneGraph g; JTabWidget w(g);
        w.addTab("One", nullptr); w.addTab("Two", nullptr);
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JTabWidget active label text {240,240,245,255}", hasText(buf, 240,240,245,255));
    }

    // ---- JTabBar: active label = ControlText {220,220,228,220}; inactive = TabInactiveText {140,140,148,140} ----
    {
        JSceneGraph g; JTabBar w(g, {"Alpha", "Beta"});
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JTabBar active label text {220,220,228,220}",   hasText(buf, 220,220,228,220));
        check("JTabBar inactive label text {140,140,148,140}", hasText(buf, 140,140,148,140));
    }

    // ---- JListView: item text {220,220,228,220}; scrollbar track {30,30,35,120}, thumb {100,100,110,200} ----
    {
        std::vector<std::string> items;
        for (int i = 0; i < 30; ++i) items.push_back("row " + std::to_string(i));
        JSceneGraph g; JListView w(g, items, 240.f, 200.f);
        w.setSelectedIndex(1);
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JListView item text {220,220,228,220}",  hasText(buf, 220,220,228,220));
        check("JListView track fill {30,30,35,120}",    hasFill(buf, 30,30,35,120));
        check("JListView thumb fill {100,100,110,200}", hasFill(buf, 100,100,110,200));
    }

    // ---- JTreeView: node text {210,210,220,220}; type-icon (kind 1=table) {220,150,60,255} ----
    {
        JTreeViewNode root; root.expanded = true;
        JTreeViewNode c0; c0.label = "child a"; c0.icon = 1;   // table glyph -> TreeIconTable
        JTreeViewNode c1; c1.label = "child b";
        root.children = { c0, c1 };
        JSceneGraph g; JTreeView w(g, 240.f, 300.f);
        w.setRootNode(root);
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JTreeView node text {210,210,220,220}",  hasText(buf, 210,210,220,220));
        check("JTreeView type-icon fill {220,150,60,255}", hasFill(buf, 220,150,60,255));
    }

    // ---- JDataGrid: header text {230,230,240,255}; row text {200,200,210,220}; alt row {34,34,36,120};
    //      header-column line = Border {72,72,76,255}; cell/row grid line {50,50,54,180} ----
    {
        JSceneGraph g; JDataGrid w(g, {"Name", "Value"}, 400.f, 250.f);
        w.setRows({ {"a", "1"}, {"b", "2"}, {"c", "3"} });
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JDataGrid header text {230,230,240,255}", hasText(buf, 230,230,240,255));
        check("JDataGrid row text {200,200,210,220}",    hasText(buf, 200,200,210,220));
        check("JDataGrid alt-row fill {34,34,36,120}",   hasFill(buf, 34,34,36,120));
        check("JDataGrid header line = Border {72,72,76,255}", hasFill(buf, 72,72,76,255));
        check("JDataGrid grid line {50,50,54,180}",      hasFill(buf, 50,50,54,180));
    }

    // ---- JGroupBox: panel {22,22,25,180}; title strip {36,36,40,200}; title text {200,200,210,200} ----
    {
        JSceneGraph g; JGroupBox w(g, "Group");
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JGroupBox panel fill {22,22,25,180}",   hasFill(buf, 22,22,25,180));
        check("JGroupBox title strip fill {36,36,40,200}", hasFill(buf, 36,36,40,200));
        check("JGroupBox title text {200,200,210,200}", hasText(buf, 200,200,210,200));
    }

    // ---- JScrollArea: body fill {20,20,24,255}; border = Border {72,72,76,255} ----
    {
        JSceneGraph g; JScrollArea w(g, 320.f, 200.f);
        JButton child(g, "inner");
        w.addChildWidget(&child);
        JPrimitiveBuffer buf; w.populateRenderPrimitives(buf);
        check("JScrollArea body fill {20,20,24,255}",  hasFill(buf, 20,20,24,255));
        check("JScrollArea border = Border {72,72,76,255}", hasBorder(buf, 72,72,76,255));
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
