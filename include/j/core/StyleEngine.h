#pragma once

#include <unordered_map>
#include <any>
#include <memory>
#include <optional>
#include <cstdint>
#include <iostream>
#include "SceneGraph.h"
#include "../graphics/VectorGraphics.h"   // canonical JColor (+ rgb/rgba/lerp helpers)
inline namespace jf {

// JColor is the framework's RGBA8 primitive from VectorGraphics.h. `lerp(a,b,t)` blends
// two colours — the workhorse for derived (disabled/inactive) roles below.

// ============================================================================
// JPalette — semantic COLOUR ROLES resolved per widget-state GROUP.
//   role  : what a pixel MEANS (Window background, Text, Highlight…), not a literal shade
//   group : the widget's coarse state family { Active, Inactive, Disabled }
// A widget asks `palette.color(role, group)` and never names a raw colour. The default
// light() / dark() palettes are self-contained; JTheme::palette() (BaseWidgets.h) rebuilds
// one from the live named theme colours so existing `Colors::` sites stay pixel-identical.
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

// ============================================================================
// JStyleState — Qt-style widget state bit flags carried by a JStyleOption.
// ============================================================================
enum JStyleState : uint32_t {
    State_None     = 0,
    State_Enabled  = 1u << 0,
    State_Hovered  = 1u << 1,
    State_Pressed  = 1u << 2,
    State_Focused  = 1u << 3,
    State_Selected = 1u << 4,
    State_On       = 1u << 5,   // checked / toggled on
    State_Off      = 1u << 6,   // explicitly off
    State_Inactive = 1u << 7,   // owning top-level is not active
};

// ============================================================================
// JStyleOption — the packet a widget hands the styler: its state + geometry.
// ============================================================================
struct JStyleOption {
    uint32_t state = State_Enabled;
    JRect    rect{};

    bool has(uint32_t flag) const { return (state & flag) != 0; }
    void set(uint32_t flag, bool on) { on ? (state |= flag) : (state &= ~flag); }

    JColorGroup group() const {
        if (!(state & State_Enabled)) return JColorGroup::Disabled;
        if (state & State_Inactive)   return JColorGroup::Inactive;
        return JColorGroup::Active;
    }
};

/**
 * @brief JType-safe style property key.
...
 * Encapsulates a unique ID and the expected value type.
 */
template<typename T>
struct JStyleKey {
    uint32_t id;
};

/**
 * @brief Sparse storage for visual properties at a specific node.
 */
class JStyleStore {
public:
    template<typename T>
    void set(JStyleKey<T> key, T value) {
        m_properties[key.id] = std::make_any<T>(value);
    }

    template<typename T>
    std::optional<T> get(JStyleKey<T> key) const {
        auto it = m_properties.find(key.id);
        if (it != m_properties.end()) {
            return std::any_cast<T>(it->second);
        }
        return std::nullopt;
    }

    bool has(uint32_t keyId) const {
        return m_properties.count(keyId) > 0;
    }

private:
    std::unordered_map<uint32_t, std::any> m_properties;
};

/**
 * @brief The JStyle Engine manages the cascading lookup logic across the JSceneGraph.
 */
class JStyleEngine {
public:
    JStyleEngine(const JSceneGraph& graph) : m_graph(graph) {}

    /**
     * @brief Looks up a property, traversing up the parent hierarchy if not found locally.
     */
    template<typename T>
    T lookup(NodeId id, JStyleKey<T> key, T defaultValue) const {
        NodeId current = id;
        while (current != InvalidNodeId) {
            auto it = m_nodeStyles.find(current);
            if (it != m_nodeStyles.end()) {
                auto val = it->second.get(key);
                if (val.has_value()) {
                    return val.value();
                }
            }
            // Traverse up the hierarchy
            // Note: We need a way to get parent from JSceneGraph, 
            // but our JSceneGraph currently stores hierarchy privately.
            // I will assume for now we can access it or we update JSceneGraph.
            current = getParentId(current);
        }
        return defaultValue;
    }

    template<typename T>
    void setLocal(NodeId id, JStyleKey<T> key, T value) {
        m_nodeStyles[id].set(key, value);
    }

private:
    // This requires exposing parentId in JSceneGraph or keeping a mirror here.
    // For this architectural demo, I'll mirror it or assume a helper.
    NodeId getParentId(NodeId id) const {
        return m_graph.getParent(id);
    }

    const JSceneGraph& m_graph;
    std::unordered_map<NodeId, JStyleStore> m_nodeStyles;
};

/**
 * @brief Standard JStyle Keys for the Toolkit.
 */
namespace JStyle {
    constexpr JStyleKey<uint32_t[4]> BackgroundColor{ 0x01 }; // RGBA8
    constexpr JStyleKey<float> CornerRadius{ 0x02 };
    constexpr JStyleKey<float> BorderWidth{ 0x03 };
    constexpr JStyleKey<float> RowHeight{ 0x50 };
    constexpr JStyleKey<float> HeaderHeight{ 0x51 };
    constexpr JStyleKey<float> CellPadding{ 0x52 };

    // ------------------------------------------------------------------------
    // Primitive draw-DECISION hooks. Given a control's state + a palette, resolve
    // which semantic colour a widget should paint — so widgets stop hardcoding
    // shades. These decide roles only; the widget still issues the draw call.
    // ------------------------------------------------------------------------

    // Background fill for a generic control (button/checkbox/…): a checked/selected
    // or pressed control reads Highlight, everything else its Base surface.
    inline JColor controlFill(const JStyleOption& o, const JPalette& p) {
        const JColorGroup g = o.group();
        if (o.has(State_On | State_Selected | State_Pressed))
            return p.color(JColorRole::Highlight, g);
        return p.color(JColorRole::Base, g);
    }

    // Outline colour: a focused control takes the Accent ring, else the Border role.
    inline JColor borderColor(const JStyleOption& o, const JPalette& p) {
        const JColorGroup g = o.group();
        if (o.has(State_Focused)) return p.color(JColorRole::Accent, g);
        return p.color(JColorRole::Border, g);
    }

    // Foreground text colour for the control's current group.
    inline JColor textColor(const JStyleOption& o, const JPalette& p) {
        if (o.has(State_On | State_Selected))
            return p.color(JColorRole::HighlightedText, o.group());
        return p.color(JColorRole::Text, o.group());
    }
}

// ============================================================================
// JStyleHint — keyed metrics a widget queries instead of embedding magic
// numbers. Resolved by JTheme::hint() (BaseWidgets.h) against the live theme.
// ============================================================================
enum class JStyleHint : uint16_t {
    FocusRingWidth,   // outline width of the focus ring
    ControlRadius,    // default corner radius of controls
    BorderWidth,      // default outline width
    ControlHeight,    // default interactive-field height
    ItemPadding,      // interior padding
    Spacing,          // gap between items
};

} // inline namespace jf
