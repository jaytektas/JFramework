#pragma once

#include "../graphics/VectorGraphics.h"   // canonical JColor (+ rgb/rgba/lerp helpers)
#include <cstddef>

inline namespace jf {

// JColor is the framework's RGBA8 primitive from VectorGraphics.h. `lerp(a,b,t)` blends
// two colours — the workhorse for derived (disabled/inactive) roles below.

// ============================================================================
// JPalette — semantic COLOUR ROLES resolved per widget-state GROUP.
//   role  : what a pixel MEANS (Window background, Text, Highlight…), not a literal shade
//   group : the widget's coarse state family { Active, Inactive, Disabled }
// A widget asks `palette.color(role, group)` and never names a raw colour. The default
// light() / dark() palettes are self-contained; JStyle::palette() rebuilds one from the
// live named theme colours so existing `Colors::` sites stay pixel-identical.
// ============================================================================
enum class JColorRole : uint8_t {
    Window,          // general widget background
    WindowText,      // text on Window
    Base,            // input-field / control background (checkbox, line edit…)
    Text,            // text on Base
    Button,          // push-button background
    ButtonText,      // text on Button
    Highlight,       // selection / checked / pressed fill
    HighlightedText, // text on Highlight
    Border,          // control outlines / separators
    Accent,          // focus ring + emphasis (may equal Highlight)
    ToolTipBase,     // tooltip background
    ToolTipText,     // tooltip text
    PlaceholderText, // empty-field hint text
    Link,            // hyperlink text
    _Count
};

enum class JColorGroup : uint8_t {
    Active,     // widget in a focused/active top-level
    Inactive,   // widget in a background top-level
    Disabled,   // widget greyed out
    _Count
};

class JPalette {
public:
    JColor color(JColorRole role, JColorGroup group = JColorGroup::Active) const {
        return m_c[static_cast<size_t>(group)][static_cast<size_t>(role)];
    }
    void setColor(JColorRole role, JColorGroup group, JColor c) {
        m_c[static_cast<size_t>(group)][static_cast<size_t>(role)] = c;
    }
    // Set the role across all three groups at once (Active = value, Inactive = value,
    // Disabled = value blended toward `window` so disabled reads dimmer automatically).
    void setRole(JColorRole role, JColor active, JColor window) {
        setColor(role, JColorGroup::Active,   active);
        setColor(role, JColorGroup::Inactive, active);
        setColor(role, JColorGroup::Disabled, lerp(active, window, 0.55f));
    }

    static JPalette dark();
    static JPalette light();

private:
    JColor m_c[static_cast<size_t>(JColorGroup::_Count)]
              [static_cast<size_t>(JColorRole::_Count)]{};
};

namespace palette_detail {
    // Assemble a palette from a flat role list; Disabled auto-derived by blending each
    // role toward Window (except Window/Border themselves, which dim more gently).
    inline JPalette build(JColor window, JColor windowText, JColor base, JColor text,
                          JColor button, JColor buttonText, JColor highlight,
                          JColor highlightedText, JColor border, JColor accent,
                          JColor tipBase, JColor tipText, JColor placeholder, JColor link) {
        JPalette p;
        p.setRole(JColorRole::Window,          window,          window);
        p.setRole(JColorRole::WindowText,      windowText,      window);
        p.setRole(JColorRole::Base,            base,            window);
        p.setRole(JColorRole::Text,            text,            window);
        p.setRole(JColorRole::Button,          button,          window);
        p.setRole(JColorRole::ButtonText,      buttonText,      window);
        p.setRole(JColorRole::Highlight,       highlight,       window);
        p.setRole(JColorRole::HighlightedText, highlightedText, window);
        p.setRole(JColorRole::Border,          border,          window);
        p.setRole(JColorRole::Accent,          accent,          window);
        p.setRole(JColorRole::ToolTipBase,     tipBase,         window);
        p.setRole(JColorRole::ToolTipText,     tipText,         window);
        p.setRole(JColorRole::PlaceholderText, placeholder,     window);
        p.setRole(JColorRole::Link,            link,            window);
        return p;
    }
}

inline JPalette JPalette::dark() {
    return palette_detail::build(
        /*Window*/          {28, 28, 30, 255},
        /*WindowText*/      {240, 240, 245, 255},
        /*Base*/            {28, 28, 30, 255},
        /*Text*/            {240, 240, 245, 255},
        /*Button*/          {40, 40, 42, 255},
        /*ButtonText*/      {240, 240, 245, 255},
        /*Highlight*/       {10, 132, 255, 255},
        /*HighlightedText*/ {255, 255, 255, 255},
        /*Border*/          {72, 72, 76, 255},
        /*Accent*/          {10, 132, 255, 255},
        /*ToolTipBase*/     {56, 56, 58, 255},
        /*ToolTipText*/     {240, 240, 245, 255},
        /*PlaceholderText*/ {160, 160, 168, 255},
        /*Link*/            {10, 132, 255, 255});
}

inline JPalette JPalette::light() {
    return palette_detail::build(
        /*Window*/          {238, 238, 242, 255},
        /*WindowText*/      {15, 15, 22, 255},
        /*Base*/            {238, 238, 242, 255},
        /*Text*/            {15, 15, 22, 255},
        /*Button*/          {222, 222, 228, 255},
        /*ButtonText*/      {15, 15, 22, 255},
        /*Highlight*/       {10, 132, 255, 255},
        /*HighlightedText*/ {255, 255, 255, 255},
        /*Border*/          {168, 168, 178, 255},
        /*Accent*/          {10, 132, 255, 255},
        /*ToolTipBase*/     {200, 200, 208, 255},
        /*ToolTipText*/     {15, 15, 22, 255},
        /*PlaceholderText*/ {90, 90, 100, 255},
        /*Link*/            {10, 132, 255, 255});
}

} // inline namespace jf
