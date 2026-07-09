#pragma once

// StyleSheet — author the unified stylesheet (JStyle) as a text file and load/reload it at
// runtime. Format: `key: value` lines, `#` comments, blank lines ignored.
//   colours   -> "r g b a"  (a optional, defaults 255)   e.g.  accent: 10 132 255
//   floats    -> a number                                 e.g.  cornerRadius: 6
//   tabEdge   -> top|bottom|left|right
//   tabFill   -> fill|left|compress
// Unknown keys are ignored, so a sheet can be partial — everything it omits keeps the
// dark() default. apply() assigns into the live singleton, so a reload reskins the GUI
// (Colors:: points into it; hosts read style().* each frame) — edit the file, reload, presto.

#include "JStyle.h"
#include <string>
#include <sstream>
#include <fstream>
#include <cctype>
#include <cstdint>

inline namespace jf {

namespace style_detail {
    inline std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }
    inline std::string lower(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    inline bool parseColor(const std::string& v, uint8_t* out) {
        std::istringstream in(v);
        int r, g, b, a = 255;
        if (!(in >> r >> g >> b)) return false;
        in >> a;
        out[0] = (uint8_t)r; out[1] = (uint8_t)g; out[2] = (uint8_t)b; out[3] = (uint8_t)a;
        return true;
    }
}

// Parse a stylesheet into a JStyle (starting from dark() so partial sheets are fine).
inline JStyle parseStyleSheet(const std::string& text) {
    using namespace style_detail;
    JStyle t = JStyle::dark();

    struct CRef { const char* n; uint8_t* p; };
    CRef colors[] = {
        {"surface0", t.Surface0}, {"surface1", t.Surface1}, {"surface2", t.Surface2},
        {"surface3", t.Surface3}, {"border", t.Border},
        {"textprimary", t.TextPrimary}, {"textsecondary", t.TextSecondary},
        {"accent", t.Accent}, {"accenthover", t.AccentHover}, {"accentpress", t.AccentPress},
        {"success", t.Success}, {"warning", t.Warning}, {"danger", t.Danger},
        {"closebtn", t.CloseBtn}, {"closebtnhover", t.CloseBtnHover}, {"closebtnmark", t.CloseBtnMark},
        {"titlebar", t.TitleBar}, {"titlebartext", t.TitleBarText},
        {"controltext", t.ControlText}, {"fieldtext", t.FieldText}, {"labeltext", t.LabelText},
        {"mutedtext", t.MutedText}, {"fieldplaceholder", t.FieldPlaceholder},
        {"capturehint", t.CaptureHint}, {"selectionfill", t.SelectionFill},
        {"highlightedtext", t.HighlightedText},
        {"dialogbg", t.DialogBg}, {"dialogtitlebg", t.DialogTitleBg}, {"overlayscrim", t.OverlayScrim},
        {"dialogshadow", t.DialogShadow}, {"inputfieldbg", t.InputFieldBg},
        {"cancelbtnbg", t.CancelBtnBg}, {"cancelbtnborder", t.CancelBtnBorder},
        {"dialogclosehover", t.DialogCloseHover},
        {"popupbg", t.PopupBg}, {"popupinnerbg", t.PopupInnerBg}, {"popupitemtext", t.PopupItemText},
        {"tooltipfill", t.ToolTipFill}, {"tooltipborder", t.ToolTipBorder},
        {"previewbg", t.PreviewBg},
        {"chartbg", t.ChartBg}, {"charttitletext", t.ChartTitleText}, {"chartaxistext", t.ChartAxisText},
        {"chartaxis2text", t.ChartAxis2Text}, {"chartlegendtext", t.ChartLegendText},
        {"charttooltiptext", t.ChartTooltipText}, {"charttooltipbg", t.ChartTooltipBg},
        {"charttooltipborder", t.ChartTooltipBorder}, {"chartcrosshair", t.ChartCrosshair},
        {"scrolltrack", t.ScrollTrack}, {"scrollthumb", t.ScrollThumb},
        {"scrollthumbactive", t.ScrollThumbActive}, {"scrollareabg", t.ScrollAreaBg},
        {"tabghostfill", t.TabGhostFill}, {"tabghostborder", t.TabGhostBorder},
        {"tabghostbar", t.TabGhostBar}, {"tabteardot", t.TabTearDot},
        {"tabinactivetext", t.TabInactiveText}, {"docktabinactivetext", t.DockTabInactiveText},
        {"treeedittext", t.TreeEditText}, {"treeicontable", t.TreeIconTable},
        {"treeiconconfig", t.TreeIconConfig}, {"treeicontoggle", t.TreeIconToggle},
        {"treeiconenum", t.TreeIconEnum}, {"treeiconcurve", t.TreeIconCurve},
        {"rowaltbg", t.RowAltBg}, {"gridline", t.GridLine}, {"gridheadertext", t.GridHeaderText},
        {"grouppanelfill", t.GroupPanelFill}, {"grouptitlebg", t.GroupTitleBg},
        {"dockcontentbg", t.DockContentBg}, {"dockpinidle", t.DockPinIdle},
        {"dockresizeidle", t.DockResizeIdle}, {"dockresizehot", t.DockResizeHot},
        {"dockclosemark", t.DockCloseMark}, {"docksplitline", t.DockSplitLine},
        {"droparrowbg", t.DropArrowBg}, {"droparrowborder", t.DropArrowBorder},
        {"windowtitlefill", t.WindowTitleFill}, {"windowframeborder", t.WindowFrameBorder},
        {"floattitlebarbg", t.FloatTitleBarBg}, {"floatseparator", t.FloatSeparator},
    };
    struct FRef { const char* n; float* p; };
    FRef floats[] = {
        {"cornerradius", &t.cornerRadius}, {"menuitemheight", &t.menuItemHeight},
        {"controlheight", &t.controlHeight}, {"buttonheight", &t.buttonHeight},
        {"labelheight", &t.labelHeight}, {"checkheight", &t.checkHeight}, {"sliderheight", &t.sliderHeight},
        {"itempadding", &t.itemPadding}, {"spacing", &t.spacing}, {"borderwidth", &t.borderWidth},
        {"titlebarheight", &t.titleBarHeight}, {"focusringwidth", &t.focusRingWidth},
        {"animspeed", &t.animSpeed}, {"tabbarsize", &t.tabBarSize},
    };

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        auto h = line.find('#'); if (h != std::string::npos) line.erase(h);
        auto c = line.find(':'); if (c == std::string::npos) continue;
        std::string key = lower(trim(line.substr(0, c)));
        std::string val = trim(line.substr(c + 1));
        if (key.empty() || val.empty()) continue;

        bool done = false;
        for (auto& cr : colors) if (key == cr.n) { parseColor(val, cr.p); done = true; break; }
        if (done) continue;
        for (auto& fr : floats) if (key == fr.n) { try { *fr.p = std::stof(val); } catch (...) {} done = true; break; }
        if (done) continue;

        std::string lv = lower(val);
        if (key == "tabedge")
            t.tabEdge = lv == "bottom" ? JTabBarEdge::Bottom : lv == "left" ? JTabBarEdge::Left
                      : lv == "right"  ? JTabBarEdge::Right  : JTabBarEdge::Top;
        else if (key == "tabfill")
            t.tabFill = lv == "left" ? JTabFill::Left : lv == "compress" ? JTabFill::Compress : JTabFill::Fill;
    }
    return t;
}

// Read + parse a stylesheet file. Returns false if the file can't be opened.
inline bool loadStyleSheet(const std::string& path, JStyle& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    out = parseStyleSheet(ss.str());
    return true;
}

// Load a stylesheet file and make it the live theme (reskins the GUI). False if unreadable.
inline bool applyStyleSheetFile(const std::string& path) {
    JStyle t;
    if (!loadStyleSheet(path, t)) return false;
    JStyle::apply(std::move(t));
    return true;
}

} // inline namespace jf
