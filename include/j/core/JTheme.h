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
        /*HighlightedText*/ JColor{255, 255, 255, 255},
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
    inline constexpr uint8_t    Transparent[4] = {0, 0, 0, 0};  // truly constant, not themed
}

} // inline namespace jf
