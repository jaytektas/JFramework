#pragma once

// JTheme — THE stylesheet: runtime-mutable colours + dimensions + dock-tab style, the semantic
// palette bridge, the jstyle helpers, and the legacy Colors:: aliases. Extracted from BaseWidgets.h
// so the theme/schema is one file, editable without touching the widget classes. Also carries the two
// small foundational enums (JWidgetState / JFocusPolicy) the theme's jstyle helpers consume.

#include <cstdint>
#include <optional>
#include <algorithm>
#include <utility>

#include "Style.h"          // JTabBarEdge / JTabFill
#include "JStyleEngine.h"    // JPalette / JColorRole / JColorGroup / JStyleOption / JStyle / JStyleHint / State_* / palette_detail
#include "../graphics/VectorGraphics.h"   // canonical JColor

inline namespace jf {

// ============================================================================
// JWidget states
// ============================================================================

enum class JWidgetState : uint32_t {
    Normal,
    Hovered,
    Pressed,
    Disabled,
    Focused
};

// ============================================================================
// JFocusPolicy — how a widget can acquire keyboard focus (bit flags)
// ============================================================================
// Tab   → reachable by Tab/Shift+Tab traversal (drives isFocusable()).
// Click → takes focus on a mouse press (drives acceptsClickFocus()).
// Strong = Click | Tab (the usual interactive default).
enum class JFocusPolicy : uint32_t {
    NoFocus    = 0,
    ClickFocus = 1,
    TabFocus   = 2,
    StrongFocus = ClickFocus | TabFocus
};

inline bool operator&(JFocusPolicy a, JFocusPolicy b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}
inline JFocusPolicy operator|(JFocusPolicy a, JFocusPolicy b) {
    return static_cast<JFocusPolicy>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

// ============================================================================
// JTheme — THE stylesheet (unified). Runtime-mutable colours + dimensions + dock
// tab style; swap the whole app with JTheme::apply(). Read it via style() (alias
// for current()); legacy Colors:: now points into it.
// ============================================================================
struct JTheme {
    // Palette
    uint8_t Surface0[4]      = {18,  18,  20,  255};
    uint8_t Surface1[4]      = {28,  28,  30,  255};
    uint8_t Surface2[4]      = {40,  40,  42,  255};
    uint8_t Surface3[4]      = {56,  56,  58,  255};
    uint8_t Border[4]        = {72,  72,  76,  255};
    uint8_t TextPrimary[4]   = {240, 240, 245, 255};
    uint8_t TextSecondary[4] = {160, 160, 168, 255};
    uint8_t Accent[4]        = {10,  132, 255, 255};
    uint8_t AccentHover[4]   = {50,  160, 255, 255};
    uint8_t AccentPress[4]   = {0,   100, 220, 255};
    uint8_t Success[4]       = {48,  209, 88,  255};
    uint8_t Warning[4]       = {255, 159, 10,  255};
    uint8_t Danger[4]        = {255, 69,  58,  255};
    uint8_t CloseBtn[4]      = {60,  40,  44,  160};
    uint8_t CloseBtnHover[4] = {220, 50,  50,  255};
    uint8_t CloseBtnMark[4]  = {255, 255, 255, 200};
    uint8_t TitleBar[4]      = {22,  22,  28,  255};   // window/dialog/popup title-bar fill (semantic role)
    uint8_t TitleBarText[4]  = {200, 200, 210, 230};   // title-bar caption

    // Widget text/mark roles — de-hardcoded from the standard widgets. Each default is the EXACT byte
    // value the widget painted before migration (the site's own alpha is applied at the call site), so a
    // default-theme render is pixel-identical while a theme swap now restyles the whole widget set.
    uint8_t ControlText[4]      = {220, 220, 228, 255};  // caption/value text on filled controls (buttons, inputs, popup rows)
    uint8_t FieldText[4]        = {210, 210, 220, 255};  // value text in spin/combo entry fields
    uint8_t LabelText[4]        = {200, 200, 210, 255};  // widget labels / secondary captions
    uint8_t MutedText[4]        = {180, 180, 190, 255};  // arrow & chevron marks, dimmed fallback labels
    uint8_t FieldPlaceholder[4] = {100, 100, 110, 255};  // placeholder text inside input fields
    uint8_t CaptureHint[4]      = {150, 190, 255, 255};  // key-capture "Press a key…" arming prompt
    uint8_t SelectionFill[4]    = {65,  105, 225, 255};  // text-area selection highlight band
    uint8_t HighlightedText[4]  = {255, 255, 255, 255};  // marks/glyphs drawn on a highlight fill (check ticks)
    // Dialog / popup chrome (de-hardcoded from Dialog/StandardDialogs/Native/Colour/Font pickers + popups).
    uint8_t DialogBg[4]        = {26,  26,  32,  255};   // dialog / native-dialog body fill (shared)
    uint8_t DialogTitleBg[4]   = {38,  38,  48,  255};   // dialog title-bar strip (shared)
    uint8_t OverlayScrim[4]    = {0,   0,   0,   160};   // modal backdrop dim
    uint8_t DialogShadow[4]    = {0,   0,   0,   60};    // dialog drop-shadow halo
    uint8_t InputFieldBg[4]    = {18,  18,  28,  255};   // dialog text-input field fill
    uint8_t CancelBtnBg[4]     = {55,  55,  65,  255};   // secondary/cancel button fill (alpha varies at site)
    uint8_t CancelBtnBorder[4] = {100, 100, 110, 255};   // secondary/cancel button outline
    uint8_t DialogCloseHover[4]= {180, 50,  50,  200};   // dialog close-× hover fill
    uint8_t PopupBg[4]         = {22,  22,  26,  255};   // bordered popup / popup-list body fill
    uint8_t PopupInnerBg[4]    = {18,  18,  22,  250};   // borderless popup body fill
    uint8_t PopupItemText[4]   = {220, 220, 228, 255};   // popup-list item text
    uint8_t PreviewBg[4]       = {20,  20,  26,  255};   // font-picker preview panel fill

    // Chart / data-viz surface (self-contained palette in graphics/Chart.h).
    uint8_t ChartBg[4]          = {22,  24,  31,  255};
    uint8_t ChartTitleText[4]   = {220, 222, 230, 235};
    uint8_t ChartAxisText[4]    = {150, 154, 165, 220};
    uint8_t ChartAxis2Text[4]   = {160, 150, 140, 220};
    uint8_t ChartLegendText[4]  = {200, 204, 214, 230};
    uint8_t ChartTooltipText[4] = {210, 214, 224, 235};
    uint8_t ChartTooltipBg[4]   = {20,  22,  30,  235};
    uint8_t ChartTooltipBorder[4]={90,  96,  110, 255};
    uint8_t ChartCrosshair[4]   = {255, 255, 255, 70};

    // Container / chrome roles — de-hardcoded from the tab / list / tree / grid / group-box / dock / scroll
    // / window widgets. Each default is the EXACT byte value the widget painted before migration (a site's
    // own alpha is applied at the call site where it differs), so a default-theme render is pixel-identical
    // while a theme swap now restyles the whole container/chrome set from here.
    uint8_t ScrollTrack[4]        = {30,  30,  35,  120};  // list / scroll-area scrollbar track
    uint8_t ScrollThumb[4]        = {100, 100, 110, 200};  // scrollbar thumb (idle)
    uint8_t ScrollThumbActive[4]  = {130, 130, 140, 200};  // scrollbar thumb (dragging)
    uint8_t ScrollAreaBg[4]       = {20,  20,  24,  255};  // scroll-area body fill
    uint8_t TabGhostFill[4]       = {40,  40,  50,  180};  // torn-tab drag ghost body
    uint8_t TabGhostBorder[4]     = {80,  130, 255, 200};  // torn-tab drag ghost outline
    uint8_t TabGhostBar[4]        = {200, 210, 255, 180};  // torn-tab drag ghost label bar
    uint8_t TabTearDot[4]         = {80,  80,  100, 120};  // tearable-tab corner dot
    uint8_t TabInactiveText[4]    = {140, 140, 148, 255};  // inactive tab label (active = ControlText)
    uint8_t DockTabInactiveText[4]= {150, 150, 158, 255};  // inactive dock-tab label (active = ControlText)
    uint8_t TreeEditText[4]       = {225, 225, 232, 255};  // tree in-place edit buffer text
    uint8_t TreeIconTable[4]      = {220, 150, 60,  255};  // tree node-kind glyph: table
    uint8_t TreeIconConfig[4]     = {90,  150, 230, 255};  // tree node-kind glyph: config (filled)
    uint8_t TreeIconToggle[4]     = {90,  200, 130, 255};  // tree node-kind glyph: toggle / value outline
    uint8_t TreeIconEnum[4]       = {175, 130, 225, 255};  // tree node-kind glyph: enum
    uint8_t TreeIconCurve[4]      = {90,  200, 220, 255};  // tree node-kind glyph: curve
    uint8_t RowAltBg[4]           = {34,  34,  36,  120};  // data-grid alternating row stripe
    uint8_t GridLine[4]           = {50,  50,  54,  180};  // data-grid cell / row grid line
    uint8_t GridHeaderText[4]     = {230, 230, 240, 255};  // data-grid header caption
    uint8_t GroupPanelFill[4]     = {22,  22,  25,  180};  // group-box panel body
    uint8_t GroupTitleBg[4]       = {36,  36,  40,  200};  // group-box title strip
    uint8_t DockContentBg[4]      = {14,  14,  16,  255};  // dock / leaf content-area fill (alpha varies at site)
    uint8_t DockPinIdle[4]        = {50,  50,  60,  255};  // dock pin badge, unpinned / flexible (alpha varies)
    uint8_t DockResizeIdle[4]     = {70,  70,  80,  160};  // dock resize grip (idle)
    uint8_t DockResizeHot[4]      = {140, 140, 160, 240};  // dock resize grip (hover)
    uint8_t DockCloseMark[4]      = {235, 235, 240, 210};  // dock close-× glyph
    uint8_t DockSplitLine[4]      = {60,  60,  64,  255};  // dock split-handle divider line
    uint8_t DropArrowBg[4]        = {40,  40,  46,  170};  // dock drop-target arrow bg (idle)
    uint8_t DropArrowBorder[4]    = {90,  90,  96,  180};  // dock drop-target arrow outline (idle)
    uint8_t WindowTitleFill[4]    = {30,  30,  32,  255};  // fallback-skin window title-bar fill
    uint8_t WindowFrameBorder[4]  = {60,  60,  65,  255};  // fallback-skin window frame border
    uint8_t FloatTitleBarBg[4]    = {28,  28,  32,  255};  // floating-window global title bar
    uint8_t FloatSeparator[4]     = {50,  50,  55,  255};  // floating-window title separator line

    // Dimensions
    float cornerRadius   = 6.f;
    float menuItemHeight = 28.f;
    // Per-control-type default heights — each STANDARD control derives its OWN appropriate height from the
    // scheme (not one squished value). The app passes no size; a local size arg is a per-instance override.
    // Authorable in the stylesheet (controlHeight / buttonHeight / labelHeight / checkHeight / sliderHeight).
    float controlHeight  = 30.f;   // interactive fields: combo / spin box / line edit / colour+font button
    float buttonHeight   = 32.f;   // push buttons
    float labelHeight    = 20.f;   // form labels (text)
    float checkHeight    = 22.f;   // check box / radio
    float sliderHeight   = 24.f;   // slider track + thumb
    float itemPadding    = 8.f;
    float fieldPadding   = 8.f;   // interior text padding for input fields (JLineEdit/JSpinBox/JComboBox…)
    float spacing        = 4.f;
    float borderWidth    = 1.f;
    float titleBarHeight = 30.f;
    float focusRingWidth = 1.5f;
    float animSpeed      = 1.0f;

    // Dock tabs (formerly JStyle — unified here as the one stylesheet).
    JTabBarEdge tabEdge{JTabBarEdge::Top};
    JTabFill    tabFill{JTabFill::Fill};
    float       tabBarSize = 28.f;

    static JTheme  dark();
    static JTheme  light();
    static JTheme& current();
    static void   apply(JTheme t);

    // Optional fully-custom palette. When set, palette() returns it verbatim instead of
    // deriving one from the named colours — so a caller can install JPalette::light()/
    // dark()/bespoke and have every migrated widget follow it. Left empty by default so
    // the derived (named-colour) palette drives the standard themes unchanged.
    std::optional<JPalette> paletteOverride;
    JTheme& setPalette(JPalette p) { paletteOverride = std::move(p); return *this; }

    // Semantic palette derived from THIS theme's named colours — the bridge that lets
    // role-based widgets read the live theme with no visual change (see mapping below).
    JPalette palette() const;
    // Keyed metric lookup — widgets query hint(JStyleHint::…) instead of magic numbers.
    float    hint(JStyleHint h) const;
};

inline float _jStyleFieldPadding() { return JTheme::current().fieldPadding; }

inline JTheme JTheme::dark()  { return JTheme{}; }
inline JTheme JTheme::light() {
    JTheme t;
    auto s = [](uint8_t* d, uint8_t a, uint8_t b, uint8_t c, uint8_t e)
              { d[0]=a; d[1]=b; d[2]=c; d[3]=e; };
    s(t.Surface0,      248, 248, 250, 255);
    s(t.Surface1,      238, 238, 242, 255);
    s(t.Surface2,      222, 222, 228, 255);
    s(t.Surface3,      200, 200, 208, 255);
    s(t.Border,        168, 168, 178, 255);
    s(t.TextPrimary,    15,  15,  22, 255);
    s(t.TextSecondary,  90,  90, 100, 255);
    s(t.CloseBtn,      200, 188, 188, 160);
    s(t.TitleBar,      222, 222, 228, 255);
    s(t.TitleBarText,   15,  15,  22, 255);
    // Widget text roles in the light scheme resolve to dark ink (HighlightedText stays white — it is
    // drawn on the accent fill). Not covered by the default-theme byte-exact gate; sensible light values.
    s(t.ControlText,     30,  30,  36, 255);
    s(t.FieldText,       40,  40,  48, 255);
    s(t.LabelText,       60,  60,  70, 255);
    s(t.MutedText,      110, 110, 120, 255);
    s(t.FieldPlaceholder,150, 150, 160, 255);
    s(t.CaptureHint,     20,  90, 200, 255);
    s(t.SelectionFill,  120, 160, 235, 255);
    // Dialog / popup chrome — light-theme values (provisional; reconciled at merge).
    s(t.DialogBg,         238, 238, 242, 255);
    s(t.DialogTitleBg,    222, 222, 228, 255);
    s(t.OverlayScrim,       0,   0,   0,  80);
    s(t.DialogShadow,       0,   0,   0,  40);
    s(t.InputFieldBg,     255, 255, 255, 255);
    s(t.CancelBtnBg,      224, 224, 230, 255);
    s(t.CancelBtnBorder,  168, 168, 178, 255);
    s(t.DialogCloseHover, 220,  90,  90, 200);
    s(t.PopupBg,          248, 248, 250, 255);
    s(t.PopupInnerBg,     248, 248, 250, 250);
    s(t.PopupItemText,     30,  30,  36, 255);
    s(t.PreviewBg,        242, 242, 246, 255);
    // Chart surface — light-theme values (provisional).
    s(t.ChartBg,          248, 248, 250, 255);
    s(t.ChartTitleText,    30,  30,  40, 235);
    s(t.ChartAxisText,     90,  94, 105, 220);
    s(t.ChartAxis2Text,   110,  95,  80, 220);
    s(t.ChartLegendText,   50,  54,  64, 230);
    s(t.ChartTooltipText,  30,  34,  44, 235);
    s(t.ChartTooltipBg,   245, 246, 250, 235);
    s(t.ChartTooltipBorder,160,166, 178, 255);
    s(t.ChartCrosshair,     0,   0,   0,  70);
    // Container / chrome roles — light-theme values (provisional; not covered by the byte-exact gate).
    s(t.ScrollTrack,        0,   0,   0,  30);
    s(t.ScrollThumb,      160, 160, 170, 200);
    s(t.ScrollThumbActive,130, 130, 140, 220);
    s(t.ScrollAreaBg,     248, 248, 250, 255);
    s(t.TabGhostFill,     225, 225, 235, 180);
    s(t.TabGhostBorder,    80, 130, 255, 200);
    s(t.TabGhostBar,      120, 150, 220, 180);
    s(t.TabTearDot,       150, 150, 170, 140);
    s(t.TabInactiveText,  120, 120, 130, 255);
    s(t.DockTabInactiveText,120,120,130, 255);
    s(t.TreeEditText,      30,  30,  36, 255);
    s(t.TreeIconTable,    200, 130,  40, 255);
    s(t.TreeIconConfig,    60, 120, 210, 255);
    s(t.TreeIconToggle,    50, 170, 100, 255);
    s(t.TreeIconEnum,     150, 100, 205, 255);
    s(t.TreeIconCurve,     50, 170, 190, 255);
    s(t.RowAltBg,           0,   0,   0,  14);
    s(t.GridLine,           0,   0,   0,  40);
    s(t.GridHeaderText,    30,  30,  40, 255);
    s(t.GroupPanelFill,   255, 255, 255, 180);
    s(t.GroupTitleBg,     228, 228, 234, 200);
    s(t.DockContentBg,    252, 252, 254, 255);
    s(t.DockPinIdle,      170, 170, 180, 255);
    s(t.DockResizeIdle,   150, 150, 160, 160);
    s(t.DockResizeHot,     90,  90, 110, 240);
    s(t.DockCloseMark,     40,  40,  48, 210);
    s(t.DockSplitLine,    190, 190, 198, 255);
    s(t.DropArrowBg,      220, 220, 226, 170);
    s(t.DropArrowBorder,  150, 150, 160, 180);
    s(t.WindowTitleFill,  228, 228, 234, 255);
    s(t.WindowFrameBorder,170, 170, 180, 255);
    s(t.FloatTitleBarBg,  235, 235, 240, 255);
    s(t.FloatSeparator,   200, 200, 208, 255);
    return t;
}
inline JTheme& JTheme::current() { static JTheme inst; return inst; }
inline void   JTheme::apply(JTheme t) { current() = std::move(t); }

// Map the legacy named colours onto semantic roles. This is the ONE source of truth for
// the running theme: every role resolves to a live theme field, so a widget that switches
// to palette().color(role) paints exactly what the old Colors:: constant produced.
//   Window/Base  <- Surface1     Button      <- Surface2     ToolTipBase <- Surface3
//   *Text        <- TextPrimary  Placeholder <- TextSecondary
//   Highlight/Accent/Link <- Accent          Border      <- Border
inline JPalette JTheme::palette() const {
    if (paletteOverride) return *paletteOverride;   // caller-installed custom palette wins
    return palette_detail::build(
        /*Window*/          JColor::fromArray(Surface1),
        /*WindowText*/      JColor::fromArray(TextPrimary),
        /*Base*/            JColor::fromArray(Surface1),
        /*Text*/            JColor::fromArray(TextPrimary),
        /*Button*/          JColor::fromArray(Surface2),
        /*ButtonText*/      JColor::fromArray(TextPrimary),
        /*Highlight*/       JColor::fromArray(Accent),
        /*HighlightedText*/ JColor::fromArray(HighlightedText),
        /*Border*/          JColor::fromArray(Border),
        /*Accent*/          JColor::fromArray(Accent),
        /*ToolTipBase*/     JColor::fromArray(Surface3),
        /*ToolTipText*/     JColor::fromArray(TextPrimary),
        /*PlaceholderText*/ JColor::fromArray(TextSecondary),
        /*Link*/            JColor::fromArray(Accent));
}

inline float JTheme::hint(JStyleHint h) const {
    switch (h) {
        case JStyleHint::FocusRingWidth: return focusRingWidth;
        case JStyleHint::ControlRadius:  return cornerRadius;
        case JStyleHint::BorderWidth:    return borderWidth;
        case JStyleHint::ControlHeight:  return controlHeight;
        case JStyleHint::ItemPadding:    return itemPadding;
        case JStyleHint::Spacing:        return spacing;
    }
    return 0.f;
}

// The one stylesheet accessor — read by the whole framework each frame.
inline JTheme& style() { return JTheme::current(); }

// ============================================================================
// jstyle — palette-routed styling helpers shared by the standard widgets.
// A control builds a JStyleOption from its live JWidgetState, then pulls
// fill / border / text from the semantic palette (JTheme::palette()) via the
// JStyle decision hooks instead of naming raw shades. Every ROLE picked below
// resolves — under the default theme — to the EXACT shade the widget painted
// before migration (Base=Surface1, Button=Surface2, ToolTipBase=Surface3,
// Border=Border, Highlight/Accent=Accent, Text/WindowText=TextPrimary,
// PlaceholderText=TextSecondary; see JTheme::palette). So a default-theme
// render is pixel-for-pixel unchanged, while a theme/palette swap now restyles
// the whole set from one place and hover/press/focus/on/selected resolve
// consistently. A few status shades (AccentPress/Success/Danger) have no
// palette role; those stay on their themed JTheme field (still follow a theme
// swap) and are called out at each site.
// ============================================================================
namespace jstyle {

// Compose a style option from a widget's coarse state. Like the migrated
// JCheckBox we resolve colour in the Active/Enabled group: the pre-migration
// widgets did NOT dim on disable (their render code never consulted the state),
// so keeping the Active group makes a disabled render pixel-identical (no
// regression) while still exposing hover/press/focus/on/selected to the hooks.
inline JStyleOption option(JWidgetState st, bool focused,
                           bool on = false, bool selected = false) {
    JStyleOption o;                              // state defaults to State_Enabled
    if (focused)                     o.set(State_Focused, true);
    if (st == JWidgetState::Hovered) o.set(State_Hovered, true);
    if (st == JWidgetState::Pressed) o.set(State_Pressed, true);
    o.set(State_On, on);
    o.set(State_Selected, selected);
    return o;
}

inline JPalette pal() { return JTheme::current().palette(); }

// Resolve a bare role in the option's group.
inline JColor role(JColorRole r, const JStyleOption& o) { return pal().color(r, o.group()); }

// Outline: Accent ring when focused, else Border (JStyle::borderColor).
inline JColor border(const JStyleOption& o) { return JStyle::borderColor(o, pal()); }

// Standard focus-aware outline width: FocusRingWidth when focused, else BorderWidth.
inline float borderW(bool focused) {
    const JTheme& t = JTheme::current();
    return focused ? t.hint(JStyleHint::FocusRingWidth) : t.hint(JStyleHint::BorderWidth);
}

// Input-field / list / strip background = the Base surface role.
inline JColor fieldFill(const JStyleOption& o) { return role(JColorRole::Base, o); }

// Push-button-style background across states: pressed => Highlight, hovered =>
// the lifted ToolTipBase surface (old Surface3 hover), else the Button role.
inline JColor buttonFill(const JStyleOption& o) {
    const JColorGroup g = o.group();
    const JPalette p = pal();
    if (o.has(State_Pressed)) return p.color(JColorRole::Highlight, g);
    if (o.has(State_Hovered)) return p.color(JColorRole::ToolTipBase, g);
    return p.color(JColorRole::Button, g);
}

} // namespace jstyle

// Legacy palette access: each role is a live pointer into the runtime stylesheet, so
// every existing `Colors::Role` site now reads JTheme::current() (mutate it / apply() a
// new theme and the whole GUI follows). apply() copies into the same singleton object,
// so these pointers stay valid.
namespace Colors {
    inline const uint8_t* const Surface0      = JTheme::current().Surface0;
    inline const uint8_t* const Surface1      = JTheme::current().Surface1;
    inline const uint8_t* const Surface2      = JTheme::current().Surface2;
    inline const uint8_t* const Surface3      = JTheme::current().Surface3;
    inline const uint8_t* const Border        = JTheme::current().Border;
    inline const uint8_t* const TextPrimary   = JTheme::current().TextPrimary;
    inline const uint8_t* const TextSecondary = JTheme::current().TextSecondary;
    inline const uint8_t* const Accent        = JTheme::current().Accent;
    inline const uint8_t* const AccentHover   = JTheme::current().AccentHover;
    inline const uint8_t* const AccentPress   = JTheme::current().AccentPress;
    inline const uint8_t* const Success       = JTheme::current().Success;
    inline const uint8_t* const Warning       = JTheme::current().Warning;
    inline const uint8_t* const Danger        = JTheme::current().Danger;
    inline const uint8_t* const CloseBtn      = JTheme::current().CloseBtn;
    inline const uint8_t* const CloseBtnHover = JTheme::current().CloseBtnHover;
    inline const uint8_t* const CloseBtnMark  = JTheme::current().CloseBtnMark;
    inline const uint8_t* const TitleBar      = JTheme::current().TitleBar;
    inline const uint8_t* const TitleBarText  = JTheme::current().TitleBarText;
    inline const uint8_t* const ControlText      = JTheme::current().ControlText;
    inline const uint8_t* const FieldText        = JTheme::current().FieldText;
    inline const uint8_t* const LabelText        = JTheme::current().LabelText;
    inline const uint8_t* const MutedText        = JTheme::current().MutedText;
    inline const uint8_t* const FieldPlaceholder = JTheme::current().FieldPlaceholder;
    inline const uint8_t* const CaptureHint      = JTheme::current().CaptureHint;
    inline const uint8_t* const SelectionFill    = JTheme::current().SelectionFill;
    inline const uint8_t* const HighlightedText  = JTheme::current().HighlightedText;
    inline const uint8_t* const DialogBg          = JTheme::current().DialogBg;
    inline const uint8_t* const DialogTitleBg     = JTheme::current().DialogTitleBg;
    inline const uint8_t* const OverlayScrim      = JTheme::current().OverlayScrim;
    inline const uint8_t* const DialogShadow      = JTheme::current().DialogShadow;
    inline const uint8_t* const InputFieldBg      = JTheme::current().InputFieldBg;
    inline const uint8_t* const CancelBtnBg       = JTheme::current().CancelBtnBg;
    inline const uint8_t* const CancelBtnBorder   = JTheme::current().CancelBtnBorder;
    inline const uint8_t* const DialogCloseHover  = JTheme::current().DialogCloseHover;
    inline const uint8_t* const PopupBg           = JTheme::current().PopupBg;
    inline const uint8_t* const PopupInnerBg      = JTheme::current().PopupInnerBg;
    inline const uint8_t* const PopupItemText     = JTheme::current().PopupItemText;
    inline const uint8_t* const PreviewBg         = JTheme::current().PreviewBg;
    inline const uint8_t* const ChartBg           = JTheme::current().ChartBg;
    inline const uint8_t* const ChartTitleText    = JTheme::current().ChartTitleText;
    inline const uint8_t* const ChartAxisText     = JTheme::current().ChartAxisText;
    inline const uint8_t* const ChartAxis2Text    = JTheme::current().ChartAxis2Text;
    inline const uint8_t* const ChartLegendText   = JTheme::current().ChartLegendText;
    inline const uint8_t* const ChartTooltipText  = JTheme::current().ChartTooltipText;
    inline const uint8_t* const ChartTooltipBg    = JTheme::current().ChartTooltipBg;
    inline const uint8_t* const ChartTooltipBorder= JTheme::current().ChartTooltipBorder;
    inline const uint8_t* const ChartCrosshair    = JTheme::current().ChartCrosshair;
    inline const uint8_t* const ScrollTrack        = JTheme::current().ScrollTrack;
    inline const uint8_t* const ScrollThumb        = JTheme::current().ScrollThumb;
    inline const uint8_t* const ScrollThumbActive  = JTheme::current().ScrollThumbActive;
    inline const uint8_t* const ScrollAreaBg       = JTheme::current().ScrollAreaBg;
    inline const uint8_t* const TabGhostFill       = JTheme::current().TabGhostFill;
    inline const uint8_t* const TabGhostBorder     = JTheme::current().TabGhostBorder;
    inline const uint8_t* const TabGhostBar        = JTheme::current().TabGhostBar;
    inline const uint8_t* const TabTearDot         = JTheme::current().TabTearDot;
    inline const uint8_t* const TabInactiveText    = JTheme::current().TabInactiveText;
    inline const uint8_t* const DockTabInactiveText= JTheme::current().DockTabInactiveText;
    inline const uint8_t* const TreeEditText       = JTheme::current().TreeEditText;
    inline const uint8_t* const TreeIconTable      = JTheme::current().TreeIconTable;
    inline const uint8_t* const TreeIconConfig     = JTheme::current().TreeIconConfig;
    inline const uint8_t* const TreeIconToggle     = JTheme::current().TreeIconToggle;
    inline const uint8_t* const TreeIconEnum       = JTheme::current().TreeIconEnum;
    inline const uint8_t* const TreeIconCurve      = JTheme::current().TreeIconCurve;
    inline const uint8_t* const RowAltBg           = JTheme::current().RowAltBg;
    inline const uint8_t* const GridLine           = JTheme::current().GridLine;
    inline const uint8_t* const GridHeaderText     = JTheme::current().GridHeaderText;
    inline const uint8_t* const GroupPanelFill     = JTheme::current().GroupPanelFill;
    inline const uint8_t* const GroupTitleBg       = JTheme::current().GroupTitleBg;
    inline const uint8_t* const DockContentBg      = JTheme::current().DockContentBg;
    inline const uint8_t* const DockPinIdle        = JTheme::current().DockPinIdle;
    inline const uint8_t* const DockResizeIdle     = JTheme::current().DockResizeIdle;
    inline const uint8_t* const DockResizeHot      = JTheme::current().DockResizeHot;
    inline const uint8_t* const DockCloseMark      = JTheme::current().DockCloseMark;
    inline const uint8_t* const DockSplitLine      = JTheme::current().DockSplitLine;
    inline const uint8_t* const DropArrowBg        = JTheme::current().DropArrowBg;
    inline const uint8_t* const DropArrowBorder    = JTheme::current().DropArrowBorder;
    inline const uint8_t* const WindowTitleFill    = JTheme::current().WindowTitleFill;
    inline const uint8_t* const WindowFrameBorder  = JTheme::current().WindowFrameBorder;
    inline const uint8_t* const FloatTitleBarBg    = JTheme::current().FloatTitleBarBg;
    inline const uint8_t* const FloatSeparator     = JTheme::current().FloatSeparator;
    inline constexpr uint8_t    Transparent[4] = {0, 0, 0, 0};  // truly constant, not themed
    inline constexpr uint8_t    White[4]        = {255, 255, 255, 255};  // truly constant neutral white (overlay tints)
}

} // inline namespace jf
