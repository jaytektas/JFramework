#pragma once

#include <string>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>
#include <functional>
#include "Signal.h"
#include "SceneGraph.h"
#include "TranslationEngine.h"
#include "AiBusHook.h"          // zero-dependency AI bus bridge
#include "KeyEvent.h"
#include "../graphics/RenderPrimitive.h"
#include "../graphics/FontEngine.h"

namespace Genesis {

// ============================================================================
// Widget states
// ============================================================================

enum class WidgetState : uint32_t {
    Normal,
    Hovered,
    Pressed,
    Disabled,
    Focused
};

// ============================================================================
// AI semantic layer
// ============================================================================

struct AISemanticNode {
    std::string role;
    std::string label;
    std::string value;
    bool interactable{true};
};

class IAIState {
public:
    virtual ~IAIState() = default;
    virtual AISemanticNode getSemanticNode() const = 0;
    virtual bool executeSemanticAction(const std::string& action) = 0;
};

// ============================================================================
// Widget — base for every element in the UI tree
// ============================================================================

class Widget : public Core::SlotTracker, public IAIState {
public:
    Widget(SceneGraph& graph, const std::string& debugName = "")
        : m_graph(graph), m_state(WidgetState::Normal), m_debugName(debugName)
    {
        m_nodeId = m_graph.createNode(debugName);
    }

    virtual ~Widget() = default;
    Widget(const Widget&)            = delete;
    Widget& operator=(const Widget&) = delete;

    NodeId      getNodeId()  const noexcept { return m_nodeId; }
    WidgetState getState()   const noexcept { return m_state;  }
    bool        isVisible()  const noexcept { return m_visible; }
    bool        isEnabled()  const noexcept { return m_state != WidgetState::Disabled; }
    bool        isFocused()  const noexcept { return m_focused; }

    void setVisible(bool v) { m_visible = v; }
    void setEnabled(bool e) {
        setState(e ? WidgetState::Normal : WidgetState::Disabled);
    }
    void setFocused(bool f) {
        if (m_focused != f) {
            m_focused = f;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        }
    }

    virtual void setState(WidgetState s) {
        if (m_state != s) {
            m_state = s;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        }
    }

    virtual bool isFocusable() const { return false; }
    bool hitTest(float mx, float my) const { return isPointInside(mx, my); }

    // Render primitive emission — subclasses paint into the shared buffer
    virtual void populateRenderPrimitives(PrimitiveBuffer& buf) = 0;

    // Input routing — virtual no-ops so the render loop can call these on any Widget
    virtual void handleMouseMove(float, float)    {}
    virtual void handleMousePress(float, float)   {}
    virtual void handleMouseRelease(float, float) {}
    virtual bool handleKeyEvent(const KeyEvent&) { return false; }
    virtual void handleScroll(float /*mx*/, float /*my*/, float /*wheel*/) {}

    // AI interface
    AISemanticNode getSemanticNode() const override {
        return {"Widget", m_debugName, "", true};
    }
    bool executeSemanticAction(const std::string&) override { return false; }

protected:
    bool isPointInside(float mx, float my) const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        return mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height;
    }

    SceneGraph& m_graph;
    NodeId      m_nodeId;
    WidgetState m_state;
    std::string m_debugName;
    bool        m_visible{true};
    bool        m_focused{false};
};

// ============================================================================
// Control — interactive widget with hover/press/click signals
// ============================================================================

class Control : public Widget {
public:
    Core::Signal<>     onHoverEntered;
    Core::Signal<>     onHoverExited;
    Core::Signal<>     onClicked;
    Core::Signal<bool> onFocusChanged;

    Control(SceneGraph& graph, const std::string& name) : Widget(graph, name) {}

    bool isFocusable() const override { return true; }

    void handleMouseMove(float mx, float my) override {
        if (m_state == WidgetState::Disabled) return;
        bool inside = isPointInside(mx, my);
        if (inside && m_state == WidgetState::Normal)   { setState(WidgetState::Hovered); onHoverEntered.emit(); }
        else if (!inside && m_state == WidgetState::Hovered) { setState(WidgetState::Normal);  onHoverExited.emit();  }
    }

    void handleMousePress(float mx, float my) override {
        if (m_state == WidgetState::Disabled) return;
        if (isPointInside(mx, my)) { setState(WidgetState::Pressed); onClicked.emit(); }
    }

    void handleMouseRelease(float mx, float my) override {
        if (m_state == WidgetState::Disabled) return;
        if (m_state == WidgetState::Pressed)
            setState(isPointInside(mx, my) ? WidgetState::Hovered : WidgetState::Normal);
    }

    bool executeSemanticAction(const std::string& a) override {
        if (m_state == WidgetState::Disabled) return false;
        if (a == "click" || a == "activate") {
            onClicked.emit();
            return true;
        }
        return false;
    }
};

// ============================================================================
// Color helpers
// ============================================================================
namespace Colors {
    // Genesis palette (dark theme)
    inline constexpr uint8_t Surface0[4]  = {18,  18,  20,  255}; // #121214
    inline constexpr uint8_t Surface1[4]  = {28,  28,  30,  255}; // #1C1C1E
    inline constexpr uint8_t Surface2[4]  = {40,  40,  42,  255}; // #28282A
    inline constexpr uint8_t Surface3[4]  = {56,  56,  58,  255}; // #38383A
    inline constexpr uint8_t Border[4]    = {72,  72,  76,  255}; // #48484C
    inline constexpr uint8_t TextPrimary[4]  = {240, 240, 245, 255};
    inline constexpr uint8_t TextSecondary[4] = {160, 160, 168, 255};
    inline constexpr uint8_t Accent[4]    = {10,  132, 255, 255}; // iOS-style blue
    inline constexpr uint8_t AccentHover[4] = {50, 160, 255, 255};
    inline constexpr uint8_t AccentPress[4] = {0,  100, 220, 255};
    inline constexpr uint8_t Success[4]   = {48,  209, 88,  255}; // green
    inline constexpr uint8_t Warning[4]   = {255, 159, 10,  255}; // amber
    inline constexpr uint8_t Danger[4]    = {255, 69,  58,  255}; // red
    inline constexpr uint8_t Transparent[4] = {0, 0, 0, 0};
}

// ============================================================================
// TextHelper — global font atlas + text layout for widgets
// ============================================================================

/**
 * Single shared FontAtlas used by all widgets.
 * Set once at app startup: TextHelper::setAtlas(fontEngine.buildAtlas(14.f));
 * After that, every populateRenderPrimitives call can emit real text glyphs.
 */
class TextHelper {
public:
    static void setAtlas(FontAtlas atlas) { get() = std::move(atlas); }
    static const FontAtlas& atlas()       { return get(); }
    static bool hasAtlas()                { return get().valid; }

    /** Decode one UTF-8 codepoint from src[i], advance i, return codepoint. */
    static uint32_t _decodeUtf8(const std::string& s, size_t& i) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        if (b < 0x80)                       { i += 1; return b; }
        if ((b & 0xE0) == 0xC0 && i+1 < s.size()) {
            uint32_t cp = ((b & 0x1F) << 6) | (static_cast<unsigned char>(s[i+1]) & 0x3F);
            i += 2; return cp;
        }
        if ((b & 0xF0) == 0xE0 && i+2 < s.size()) {
            uint32_t cp = ((b & 0x0F) << 12)
                        | ((static_cast<unsigned char>(s[i+1]) & 0x3F) << 6)
                        |  (static_cast<unsigned char>(s[i+2]) & 0x3F);
            i += 3; return cp;
        }
        if ((b & 0xF8) == 0xF0 && i+3 < s.size()) {
            uint32_t cp = ((b & 0x07) << 18)
                        | ((static_cast<unsigned char>(s[i+1]) & 0x3F) << 12)
                        | ((static_cast<unsigned char>(s[i+2]) & 0x3F) << 6)
                        |  (static_cast<unsigned char>(s[i+3]) & 0x3F);
            i += 4; return cp;
        }
        i += 1; return 0; // invalid byte — skip
    }

    /** Map codepoints outside the atlas to something that is in it. */
    static uint32_t _substitute(uint32_t cp) {
        if (cp >= 0x2013 && cp <= 0x2014) return '-';   // en/em dash
        if (cp == 0x2018 || cp == 0x2019) return '\'';  // smart single quotes
        if (cp == 0x201C || cp == 0x201D) return '"';   // smart double quotes
        if (cp == 0x2026)                 return '.';   // ellipsis → period
        if (cp == 0x00A0)                 return ' ';   // non-breaking space
        return '?';
    }

    /** Push 6 verts (2 triangles) per glyph into a TextCall and add to buf. */
    static void pushText(PrimitiveBuffer& buf,
                         float x, float y,
                         const std::string& text,
                         const uint8_t color[4],
                         float maxWidth = 0.0f)
    {
        const auto& atl = get();
        if (!atl.valid || text.empty()) return;

        PrimitiveBuffer::TextCall call;
        std::copy(color, color + 4, call.color);

        float penX    = x;
        float baseline = y + atl.ascent;

        size_t i = 0;
        while (i < text.size()) {
            uint32_t cp = _decodeUtf8(text, i);
            if (cp == 0) continue;

            auto it = atl.glyphs.find(cp);
            if (it == atl.glyphs.end()) {
                cp = _substitute(cp);
                it = atl.glyphs.find(cp);
                if (it == atl.glyphs.end()) { penX += atl.ascent * 0.35f; continue; }
            }
            const GlyphInfo& g = it->second;

            if (maxWidth > 0.0f && (penX - x + g.advanceX) > maxWidth) break;

            penX += g.advanceX;

            // Whitespace or zero-size glyphs: advance pen only, no geometry
            if (g.pixelW < 0.5f || g.pixelH < 0.5f) continue;

            float gx = penX - g.advanceX + g.bearingX;
            float gy = baseline + g.bearingY;
            float gw = g.pixelW, gh = g.pixelH;

            call.verts.push_back({gx,      gy,      g.u0, g.v0});
            call.verts.push_back({gx + gw, gy,      g.u1, g.v0});
            call.verts.push_back({gx + gw, gy + gh, g.u1, g.v1});
            call.verts.push_back({gx + gw, gy + gh, g.u1, g.v1});
            call.verts.push_back({gx,      gy + gh, g.u0, g.v1});
            call.verts.push_back({gx,      gy,      g.u0, g.v0});
        }

        if (!call.verts.empty()) buf.pushTextCall(std::move(call));
    }

    /** Measure rendered width of a UTF-8 string using the shared atlas. */
    static float measureWidth(const std::string& text) {
        const auto& atl = get();
        if (!atl.valid) return static_cast<float>(text.size()) * 8.0f;
        float w = 0.0f;
        size_t i = 0;
        while (i < text.size()) {
            uint32_t cp = _decodeUtf8(text, i);
            if (cp == 0) continue;
            auto it = atl.glyphs.find(cp);
            if (it == atl.glyphs.end()) { cp = _substitute(cp); it = atl.glyphs.find(cp); }
            if (it != atl.glyphs.end()) w += it->second.advanceX;
            else w += atl.ascent * 0.35f;
        }
        return w;
    }

    static float lineHeight() {
        return hasAtlas() ? get().lineHeight : 16.0f;
    }

private:
    static FontAtlas& get() { static FontAtlas s; return s; }
};

// ============================================================================
// Separator
// ============================================================================

class Separator : public Widget {
public:
    enum class Orientation { Horizontal, Vertical };

    Separator(SceneGraph& graph, Orientation orient = Orientation::Horizontal,
              float size = 280.0f)
        : Widget(graph, "Separator"), m_orient(orient)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        if (orient == Orientation::Horizontal) { l.boundingBox.width = size; l.boundingBox.height = 1.0f; }
        else                                   { l.boundingBox.width = 1.0f; l.boundingBox.height = size; }
    }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Border);
    }

    AISemanticNode getSemanticNode() const override { return {"Separator", "", "", false}; }
    bool executeSemanticAction(const std::string&) override { return false; }

private:
    Orientation m_orient;
};

// ============================================================================
// Label
// ============================================================================

class Label : public Widget {
public:
    Label(SceneGraph& graph, const std::string& text, float w = 240.0f, float h = 20.0f)
        : Widget(graph, "Label: " + text), m_text(text)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = h;
        l.minWidth = TextHelper::hasAtlas() ? TextHelper::measureWidth(m_text) : w;
        l.minHeight = h;
    }

    void setText(const std::string& t) { m_text = t; m_graph.invalidateNode(m_nodeId, DirtySelf); }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (TextHelper::hasAtlas()) {
            uint8_t c[4] = {200, 200, 210, 200};
            float ty = b.y + (b.height - TextHelper::lineHeight()) * 0.5f;
            TextHelper::pushText(buf, b.x, ty, tr(m_text), c, b.width);
        } else {
            // Fallback placeholder bars
            float cy = b.y + b.height * 0.5f - 3.0f;
            uint8_t c[4] = {200, 200, 210, 140};
            buf.pushRectangle(b.x, cy, b.width * 0.55f, 6.0f, c, 2.0f);
        }
    }

    AISemanticNode getSemanticNode() const override { return {"Label", m_text, "", false}; }
    bool executeSemanticAction(const std::string&) override { return false; }

private:
    std::string m_text;
};

// ============================================================================
// Button
// ============================================================================

class Button : public Control {
public:
    Button(SceneGraph& graph, const std::string& label,
           float w = 160.0f, float h = 36.0f)
        : Control(graph, "Button"), m_label(label)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = h;
        l.minWidth = TextHelper::hasAtlas() ? (TextHelper::measureWidth(m_label) + 24.f) : w;
        l.minHeight = h;
    }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const uint8_t* fill = Colors::Surface2;
        if (m_state == WidgetState::Hovered) fill = Colors::Surface3;
        if (m_state == WidgetState::Pressed) fill = Colors::Accent;
        bool focused = isFocused();
        buf.pushRectangle(b.x, b.y, b.width, b.height, fill, 6.0f,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);

        if (TextHelper::hasAtlas()) {
            std::string txt = tr(m_label);
            float tw = TextHelper::measureWidth(txt);
            float tx = b.x + (b.width  - tw) * 0.5f;
            float ty = b.y + (b.height - TextHelper::lineHeight()) * 0.5f;
            uint8_t tc[4] = {220, 220, 228, 230};
            TextHelper::pushText(buf, tx, ty, txt, tc);
        } else {
            float tw = b.width * 0.5f;
            float tx = b.x + (b.width - tw) * 0.5f;
            uint8_t tc[4] = {220, 220, 228, 200};
            buf.pushRectangle(tx, b.y + (b.height - 6.0f) * 0.5f, tw, 6.0f, tc, 2.0f);
        }
    }

    AISemanticNode getSemanticNode() const override { return {"Button", m_label, "", true}; }
    bool executeSemanticAction(const std::string& a) override {
        if (a == "click") {
            onClicked.emit();
            if (AiBusHook::emit) AiBusHook::emit(m_nodeId, AiBusHook::kClick, m_label.c_str());
            return true;
        }
        return false;
    }

private:
    std::string m_label;
};

// ============================================================================
// ToggleButton
// ============================================================================

class ToggleButton : public Control {
public:
    Core::Signal<bool> onToggled;

    ToggleButton(SceneGraph& graph, const std::string& label,
                 float w = 160.0f, float h = 36.0f)
        : Control(graph, "ToggleButton"), m_label(label)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = h;
        l.minWidth = TextHelper::hasAtlas() ? (TextHelper::measureWidth(m_label) + 24.f) : w;
        l.minHeight = h;
    }

    void setToggled(bool v) {
        if (m_toggled != v) {
            m_toggled = v;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onToggled.emit(v);
            if (AiBusHook::emit)
                AiBusHook::emit(m_nodeId, AiBusHook::kToggled, m_toggled ? "true" : "false");
        }
    }
    bool isToggled() const { return m_toggled; }

    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) setState(WidgetState::Pressed);
    }
    void handleMouseRelease(float mx, float my) override {
        if (m_state == WidgetState::Pressed && isPointInside(mx, my)) setToggled(!m_toggled);
        Control::handleMouseRelease(mx, my);
    }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const uint8_t* fill = m_toggled ? Colors::Accent : Colors::Surface2;
        uint8_t hover[4];
        if (m_state == WidgetState::Hovered) {
            std::copy(fill, fill+4, hover);
            for (int i=0;i<3;i++) hover[i] = static_cast<uint8_t>(std::min(255, hover[i]+20));
            fill = hover;
        }
        bool focused = isFocused();
        buf.pushRectangle(b.x, b.y, b.width, b.height, fill, 6.0f,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);
        if (TextHelper::hasAtlas()) {
            std::string txt = tr(m_label);
            float tw = TextHelper::measureWidth(txt);
            uint8_t tc[4] = {220, 220, 228, 230};
            TextHelper::pushText(buf, b.x + (b.width-tw)*0.5f,
                                 b.y + (b.height-TextHelper::lineHeight())*0.5f, txt, tc);
        } else {
            float tw = b.width * 0.5f;
            uint8_t tc[4] = {220, 220, 228, 200};
            buf.pushRectangle(b.x + (b.width-tw)*0.5f, b.y + (b.height-6.0f)*0.5f, tw, 6.0f, tc, 2.0f);
        }
    }

    AISemanticNode getSemanticNode() const override {
        return {"ToggleButton", m_label, m_toggled ? "true" : "false", true};
    }
    bool executeSemanticAction(const std::string& a) override {
        if (a == "toggle") { setToggled(!m_toggled); return true; }
        return false;
    }

private:
    std::string m_label;
    bool m_toggled{false};
};

// ============================================================================
// CheckBox
// ============================================================================

class CheckBox : public Control {
public:
    Core::Signal<bool> onStateChanged;

    CheckBox(SceneGraph& graph, const std::string& label, float w = 200.0f, float h = 22.0f)
        : Control(graph, "CheckBox"), m_label(label)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = h;
        l.minWidth = TextHelper::hasAtlas() ? (TextHelper::measureWidth(m_label) + 28.f) : w;
        l.minHeight = h;
    }

    void setChecked(bool v) {
        if (m_checked != v) {
            m_checked = v;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onStateChanged.emit(v);
            if (AiBusHook::emit)
                AiBusHook::emit(m_nodeId,
                                m_checked ? AiBusHook::kChecked : AiBusHook::kUnchecked,
                                m_checked ? "true" : "false");
        }
    }
    bool isChecked() const { return m_checked; }

    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) setChecked(!m_checked);
    }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float boxSz = b.height;
        // Box
        const uint8_t* fill = m_checked ? Colors::Accent : Colors::Surface1;
        bool focused = isFocused();
        buf.pushRectangle(b.x, b.y, boxSz, boxSz, fill, 4.0f,
                          focused ? 2.0f : 1.5f,
                          focused ? Colors::Accent : Colors::Border);
        // Tick mark when checked — two overlapping rects form a checkmark shape
        if (m_checked) {
            uint8_t white[4] = {255, 255, 255, 220};
            buf.pushRectangle(b.x + 3.0f, b.y + boxSz*0.5f - 1.5f, boxSz - 6.0f, 3.0f, white, 1.5f);
            buf.pushRectangle(b.x + boxSz*0.5f - 1.5f, b.y + 3.0f, 3.0f, boxSz - 6.0f, white, 1.5f);
        }
        // Label next to box
        if (TextHelper::hasAtlas()) {
            uint8_t lc[4] = {200, 200, 210, 200};
            TextHelper::pushText(buf, b.x + boxSz + 8.0f,
                                 b.y + (b.height - TextHelper::lineHeight()) * 0.5f,
                                 tr(m_label), lc, b.width - boxSz - 8.0f);
        } else {
            uint8_t lc[4] = {180, 180, 190, 140};
            buf.pushRectangle(b.x + boxSz + 8.0f, b.y + (b.height - 6.0f)*0.5f,
                              b.width - boxSz - 8.0f, 6.0f, lc, 2.0f);
        }
    }

    AISemanticNode getSemanticNode() const override {
        return {"CheckBox", m_label, m_checked ? "checked" : "unchecked", true};
    }
    bool executeSemanticAction(const std::string& a) override {
        if (a == "check")   { setChecked(true);  return true; }
        if (a == "uncheck") { setChecked(false); return true; }
        return false;
    }

private:
    std::string m_label;
    bool m_checked{false};
};

// ============================================================================
// RadioButton
// ============================================================================

class RadioButton : public Control {
public:
    Core::Signal<bool> onSelected;

    RadioButton(SceneGraph& graph, const std::string& label,
                float w = 200.0f, float h = 22.0f)
        : Control(graph, "RadioButton"), m_label(label)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = h;
        l.minWidth = TextHelper::hasAtlas() ? (TextHelper::measureWidth(m_label) + 28.f) : w;
        l.minHeight = h;
    }

    void setSelected(bool v) {
        if (m_selected != v) { m_selected = v; m_graph.invalidateNode(m_nodeId, DirtySelf); onSelected.emit(v); }
    }
    bool isSelected() const { return m_selected; }

    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) setSelected(true);
    }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float r = b.height;           // outer circle diameter == row height
        float borderW = 1.5f;
        // Outer circle (use extreme radius for perfect circle)
        const uint8_t* ring = m_selected ? Colors::Accent : Colors::Surface1;
        bool focused = isFocused();
        buf.pushRectangle(b.x, b.y, r, r, ring, r * 0.5f,
                          focused ? 2.0f : borderW,
                          focused ? Colors::Accent : Colors::Border);
        // Inner fill dot when selected
        if (m_selected) {
            float dot = r * 0.42f, offset = (r - dot) * 0.5f;
            buf.pushRectangle(b.x + offset, b.y + offset, dot, dot, Colors::TextPrimary, dot * 0.5f);
        }
        // Label
        if (TextHelper::hasAtlas()) {
            uint8_t lc[4] = {200, 200, 210, 200};
            TextHelper::pushText(buf, b.x + r + 8.0f,
                                 b.y + (b.height - TextHelper::lineHeight()) * 0.5f,
                                 tr(m_label), lc, b.width - r - 8.0f);
        } else {
            uint8_t lc[4] = {180, 180, 190, 140};
            buf.pushRectangle(b.x + r + 8.0f, b.y + (b.height - 6.0f)*0.5f,
                              b.width - r - 8.0f, 6.0f, lc, 2.0f);
        }
    }

    AISemanticNode getSemanticNode() const override {
        return {"RadioButton", m_label, m_selected ? "selected" : "unselected", true};
    }
    bool executeSemanticAction(const std::string& a) override {
        if (a == "select") { setSelected(true); return true; }
        return false;
    }

private:
    std::string m_label;
    bool m_selected{false};
};

// ============================================================================
// Slider
// ============================================================================

class Slider : public Control {
public:
    Core::Signal<float> onValueChanged;

    Slider(SceneGraph& graph, float w = 280.0f, float h = 24.0f)
        : Control(graph, "Slider"), m_value(0.5f)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = h;
        l.minWidth = 50.0f;
        l.minHeight = h;
    }

    void setValue(float v) {
        float c = std::clamp(v, 0.0f, 1.0f);
        if (m_value != c) { m_value = c; m_graph.invalidateNode(m_nodeId, DirtySelf); onValueChanged.emit(c); }
    }
    float getValue() const { return m_value; }

    void handleMouseMove(float mx, float my) override {
        Control::handleMouseMove(mx, my);
        if (m_state == WidgetState::Pressed) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            setValue((mx - b.x) / b.width);
        }
    }
    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) {
            setState(WidgetState::Pressed);
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            setValue((mx - b.x) / b.width);
        }
    }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float trackH = 4.0f, trackY = b.y + (b.height - trackH) * 0.5f;
        float fillW  = b.width * m_value;

        // Track background
        buf.pushRectangle(b.x, trackY, b.width, trackH, Colors::Surface3, 2.0f);
        // Filled portion
        if (fillW > 0.5f)
            buf.pushRectangle(b.x, trackY, fillW, trackH, Colors::Accent, 2.0f);
        // Thumb
        float thumbW = 16.0f, thumbH = b.height;
        float thumbX = b.x + fillW - thumbW * 0.5f;
        thumbX = std::clamp(thumbX, b.x, b.x + b.width - thumbW);
        const uint8_t* tc = (m_state == WidgetState::Pressed) ? Colors::AccentPress : Colors::TextPrimary;
        bool focused = isFocused();
        buf.pushRectangle(thumbX, b.y, thumbW, thumbH, tc, thumbW * 0.5f,
                          focused ? 1.5f : 0.0f,
                          focused ? Colors::Accent : Colors::Border);
    }

    AISemanticNode getSemanticNode() const override {
        return {"Slider", "Slider", std::to_string(m_value), true};
    }
    bool executeSemanticAction(const std::string& a) override {
        if (a.rfind("set_value:", 0) == 0) {
            try { setValue(std::stof(a.substr(10))); return true; } catch (...) {}
        }
        return false;
    }

private:
    float m_value;
};

// ============================================================================
// ProgressBar
// ============================================================================

class ProgressBar : public Widget {
public:
    ProgressBar(SceneGraph& graph, float w = 280.0f, float h = 12.0f)
        : Widget(graph, "ProgressBar"), m_progress(0.0f)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = h;
        l.minWidth = 50.0f;
        l.minHeight = h;
    }

    void setProgress(float p) { m_progress = std::clamp(p, 0.0f, 1.0f); m_graph.invalidateNode(m_nodeId, DirtySelf); }
    float getProgress() const { return m_progress; }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface2, 6.0f);
        if (m_progress > 0.005f)
            buf.pushRectangle(b.x, b.y, b.width * m_progress, b.height, Colors::Success, 6.0f);
    }

    AISemanticNode getSemanticNode() const override {
        return {"ProgressBar", "", std::to_string(int(m_progress * 100)) + "%", false};
    }
    bool executeSemanticAction(const std::string&) override { return false; }

private:
    float m_progress;
};

// ============================================================================
// ScrollBar
// ============================================================================

class ScrollBar : public Control {
public:
    Core::Signal<float> onScrolled; // emits 0..1 position

    ScrollBar(SceneGraph& graph, float w = 280.0f, float h = 14.0f,
              float thumbRatio = 0.3f)
        : Control(graph, "ScrollBar"), m_thumbRatio(std::clamp(thumbRatio, 0.05f, 1.0f))
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = h;
        l.minWidth = 40.0f;
        l.minHeight = h;
    }

    void setScrollPosition(float p) {
        float c = std::clamp(p, 0.0f, 1.0f);
        if (m_position != c) { m_position = c; m_graph.invalidateNode(m_nodeId, DirtySelf); onScrolled.emit(c); }
    }
    float scrollPosition() const { return m_position; }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) return;
        setState(WidgetState::Pressed);
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        setScrollPosition((mx - b.x) / b.width - m_thumbRatio * 0.5f);
    }
    void handleMouseMove(float mx, float my) override {
        Control::handleMouseMove(mx, my);
        if (m_state == WidgetState::Pressed) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            setScrollPosition((mx - b.x) / b.width - m_thumbRatio * 0.5f);
        }
    }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface1, 7.0f);
        float tw = b.width * m_thumbRatio;
        float tx = b.x + m_position * (b.width - tw);
        const uint8_t* tc = (m_state == WidgetState::Pressed) ? Colors::AccentPress
                           : (m_state == WidgetState::Hovered) ? Colors::Surface3
                                                                : Colors::Border;
        buf.pushRectangle(tx, b.y + 1.0f, tw, b.height - 2.0f, tc, 6.0f);
    }

    AISemanticNode getSemanticNode() const override {
        return {"ScrollBar", "", std::to_string(m_position), true};
    }
    bool executeSemanticAction(const std::string&) override { return false; }

private:
    float m_position{0.0f};
    float m_thumbRatio;
};

// ============================================================================
// LineEdit
// ============================================================================

class LineEdit : public Control {
public:
    Core::Signal<std::string> onTextChanged;
    Core::Signal<>            onReturnPressed;

    LineEdit(SceneGraph& graph, const std::string& placeholder = "",
             float w = 280.0f, float h = 32.0f)
        : Control(graph, "LineEdit"), m_placeholder(placeholder)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = h;
        l.minWidth = 60.0f;
        l.minHeight = h;
    }

    void setText(const std::string& t) {
        if (m_text != t) {
            m_text = t;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onTextChanged.emit(t);
            if (AiBusHook::emit)
                AiBusHook::emit(m_nodeId, AiBusHook::kTextChanged, m_text.c_str());
        }
    }
    const std::string& text()        const { return m_text; }
    const std::string& placeholder() const { return m_placeholder; }

    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) { onClicked.emit(); }
    }

    bool handleKeyEvent(const KeyEvent& ke) override {
        if (!ke.pressed) return false;
        using K = KeyEvent::Key;
        if (ke.key == K::Backspace) {
            if (!m_text.empty()) {
                m_text.pop_back();
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                onTextChanged.emit(m_text);
                if (AiBusHook::emit)
                    AiBusHook::emit(m_nodeId, AiBusHook::kTextChanged, m_text.c_str());
            }
            return true;
        } else if (ke.key == K::Return) {
            onReturnPressed.emit();
            return true;
        } else if (ke.utf8[0] != '\0') {
            // Append character if it's printable (not control characters)
            if (static_cast<uint8_t>(ke.utf8[0]) >= 32) {
                m_text += ke.utf8;
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                onTextChanged.emit(m_text);
                if (AiBusHook::emit)
                    AiBusHook::emit(m_nodeId, AiBusHook::kTextChanged, m_text.c_str());
                return true;
            }
        }
        return false;
    }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        bool focused = isFocused();

        // Background + border (accent when focused)
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface1, 6.0f,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);

        float innerX = b.x + 8.0f;
        float innerW = b.width - 16.0f;
        float midY   = b.y + (b.height - 7.0f) * 0.5f;

        if (TextHelper::hasAtlas()) {
            float ty = b.y + (b.height - TextHelper::lineHeight()) * 0.5f;
            if (m_text.empty()) {
                uint8_t pc[4] = {100, 100, 110, 160};
                TextHelper::pushText(buf, innerX, ty, m_placeholder, pc, innerW);
            } else {
                uint8_t tc[4] = {220, 220, 228, 220};
                TextHelper::pushText(buf, innerX, ty, m_text, tc, innerW);
            }
        } else {
            if (m_text.empty()) {
                uint8_t pc[4] = {100, 100, 110, 120};
                buf.pushRectangle(innerX, midY, innerW * 0.55f, 7.0f, pc, 2.0f);
            } else {
                uint8_t tc[4] = {220, 220, 228, 200};
                buf.pushRectangle(innerX, midY, innerW * 0.65f, 7.0f, tc, 2.0f);
            }
        }

        // Blinking cursor when focused
        if (focused) {
            float cx = m_text.empty() ? innerX : innerX + innerW * 0.65f + 2.0f;
            buf.pushRectangle(cx, b.y + 6.0f, 1.5f, b.height - 12.0f, Colors::Accent);
        }
    }

    AISemanticNode getSemanticNode() const override { return {"LineEdit", m_placeholder, m_text, true}; }
    bool executeSemanticAction(const std::string& a) override {
        if (a.rfind("set_text:", 0) == 0) { setText(a.substr(9)); return true; }
        return false;
    }

private:
    std::string m_text;
    std::string m_placeholder;
};

// ============================================================================
// TextArea
// ============================================================================

class TextArea : public Control {
public:
    Core::Signal<std::string> onTextChanged;

    TextArea(SceneGraph& graph, const std::string& placeholder = "",
             float w = 340.0f, float h = 120.0f)
        : Control(graph, "TextArea"), m_placeholder(placeholder)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = h;
        l.minWidth = 100.0f;
        l.minHeight = 40.0f;
    }

    void setText(const std::string& t) {
        if (m_text != t) {
            m_text = t;
            m_cursorPos = m_text.size();
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onTextChanged.emit(t);
            if (AiBusHook::emit)
                AiBusHook::emit(m_nodeId, AiBusHook::kTextChanged, m_text.c_str());
        }
    }
    const std::string& text()        const { return m_text; }
    const std::string& placeholder() const { return m_placeholder; }

    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) { onClicked.emit(); }
    }

    std::vector<std::string> getLines() const {
        std::vector<std::string> lines;
        std::string current;
        for (char c : m_text) {
            if (c == '\n') {
                lines.push_back(current);
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        lines.push_back(current);
        return lines;
    }

    void getCursorLineCol(size_t& outLine, size_t& outCol) const {
        outLine = 0;
        outCol = 0;
        for (size_t i = 0; i < m_cursorPos && i < m_text.size(); ++i) {
            if (m_text[i] == '\n') {
                outLine++;
                outCol = 0;
            } else {
                outCol++;
            }
        }
    }

    size_t getPosFromLineCol(size_t line, size_t col) const {
        size_t curLine = 0;
        size_t curCol = 0;
        size_t i = 0;
        for (; i < m_text.size(); ++i) {
            if (curLine == line) {
                if (curCol == col || m_text[i] == '\n') {
                    return i;
                }
                curCol++;
            } else {
                if (m_text[i] == '\n') {
                    curLine++;
                    curCol = 0;
                    if (curLine == line && col == 0) {
                        return i + 1;
                    }
                }
            }
        }
        return i;
    }

    bool handleKeyEvent(const KeyEvent& ke) override {
        if (!ke.pressed) return false;
        using K = KeyEvent::Key;
        
        size_t line = 0, col = 0;
        getCursorLineCol(line, col);

        if (ke.key == K::Backspace) {
            if (m_cursorPos > 0 && !m_text.empty()) {
                m_text.erase(m_cursorPos - 1, 1);
                m_cursorPos--;
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                onTextChanged.emit(m_text);
                if (AiBusHook::emit)
                    AiBusHook::emit(m_nodeId, AiBusHook::kTextChanged, m_text.c_str());
            }
            return true;
        } else if (ke.key == K::Delete) {
            if (m_cursorPos < m_text.size()) {
                m_text.erase(m_cursorPos, 1);
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                onTextChanged.emit(m_text);
                if (AiBusHook::emit)
                    AiBusHook::emit(m_nodeId, AiBusHook::kTextChanged, m_text.c_str());
            }
            return true;
        } else if (ke.key == K::Return) {
            m_text.insert(m_cursorPos, "\n");
            m_cursorPos++;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onTextChanged.emit(m_text);
            if (AiBusHook::emit)
                AiBusHook::emit(m_nodeId, AiBusHook::kTextChanged, m_text.c_str());
            return true;
        } else if (ke.key == K::Left) {
            if (m_cursorPos > 0) m_cursorPos--;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        } else if (ke.key == K::Right) {
            if (m_cursorPos < m_text.size()) m_cursorPos++;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        } else if (ke.key == K::Up) {
            if (line > 0) {
                m_cursorPos = getPosFromLineCol(line - 1, col);
                m_graph.invalidateNode(m_nodeId, DirtySelf);
            }
            return true;
        } else if (ke.key == K::Down) {
            auto lines = getLines();
            if (line + 1 < lines.size()) {
                m_cursorPos = getPosFromLineCol(line + 1, col);
                m_graph.invalidateNode(m_nodeId, DirtySelf);
            }
            return true;
        } else if (ke.key == K::Home) {
            m_cursorPos = getPosFromLineCol(line, 0);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        } else if (ke.key == K::End) {
            auto lines = getLines();
            m_cursorPos = getPosFromLineCol(line, lines[line].size());
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        } else if (ke.utf8[0] != '\0') {
            if (static_cast<uint8_t>(ke.utf8[0]) >= 32 || ke.utf8[0] == '\t') {
                m_text.insert(m_cursorPos, ke.utf8);
                m_cursorPos += std::strlen(ke.utf8);
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                onTextChanged.emit(m_text);
                if (AiBusHook::emit)
                    AiBusHook::emit(m_nodeId, AiBusHook::kTextChanged, m_text.c_str());
                return true;
            }
        }
        return false;
    }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        bool focused = isFocused();

        // Background + border (accent when focused)
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface1, 6.0f,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);

        float innerX = b.x + 8.0f;
        float innerY = b.y + 8.0f;
        float innerW = b.width - 16.0f;
        float innerH = b.height - 16.0f;

        float lh = TextHelper::hasAtlas() ? TextHelper::lineHeight() : 12.0f;

        size_t cursorLine = 0, cursorCol = 0;
        getCursorLineCol(cursorLine, cursorCol);

        // Keep cursor visible with scroll offset
        float cursorYRel = cursorLine * lh;
        if (cursorYRel < m_scrollOffset) {
            m_scrollOffset = cursorYRel;
        } else if (cursorYRel + lh > m_scrollOffset + innerH) {
            m_scrollOffset = cursorYRel + lh - innerH;
        }
        m_scrollOffset = std::max(0.0f, m_scrollOffset);

        auto lines = getLines();

        if (TextHelper::hasAtlas()) {
            if (m_text.empty() && !m_placeholder.empty()) {
                uint8_t pc[4] = {100, 100, 110, 160};
                TextHelper::pushText(buf, innerX, innerY, m_placeholder, pc, innerW);
            } else {
                uint8_t tc[4] = {220, 220, 228, 220};
                for (size_t i = 0; i < lines.size(); ++i) {
                    float lineY = innerY + i * lh - m_scrollOffset;
                    if (lineY + lh >= innerY && lineY <= innerY + innerH) {
                        TextHelper::pushText(buf, innerX, lineY, lines[i], tc, innerW);
                    }
                }
            }
        } else {
            if (m_text.empty()) {
                uint8_t pc[4] = {100, 100, 110, 120};
                buf.pushRectangle(innerX, innerY + (lh - 7.0f) * 0.5f, innerW * 0.55f, 7.0f, pc, 2.0f);
            } else {
                uint8_t tc[4] = {220, 220, 228, 200};
                for (size_t i = 0; i < lines.size(); ++i) {
                    float lineY = innerY + i * lh - m_scrollOffset;
                    if (lineY + lh >= innerY && lineY <= innerY + innerH) {
                        float lw = std::min(innerW, 20.0f + static_cast<float>(lines[i].size() * 6));
                        buf.pushRectangle(innerX, lineY + (lh - 7.0f) * 0.5f, lw, 7.0f, tc, 2.0f);
                    }
                }
            }
        }

        // Render Cursor
        if (focused) {
            float cx = innerX;
            if (!m_text.empty() && cursorLine < lines.size()) {
                std::string currentLineText = lines[cursorLine].substr(0, cursorCol);
                cx = innerX + (TextHelper::hasAtlas() ? TextHelper::measureWidth(currentLineText) : cursorCol * 6.0f);
            }
            float cy = innerY + cursorLine * lh - m_scrollOffset;
            if (cy + lh >= innerY && cy <= innerY + innerH) {
                buf.pushRectangle(cx, cy + 2.0f, 1.5f, lh - 4.0f, Colors::Accent);
            }
        }
    }

    AISemanticNode getSemanticNode() const override { return {"TextArea", m_placeholder, m_text, true}; }
    bool executeSemanticAction(const std::string& a) override {
        if (a.rfind("set_text:", 0) == 0) { setText(a.substr(9)); return true; }
        return false;
    }

private:
    std::string m_text;
    std::string m_placeholder;
    size_t      m_cursorPos{0};
    float       m_scrollOffset{0.0f};
};

// ============================================================================
// SpinBox
// ============================================================================

class SpinBox : public Control {
public:
    Core::Signal<int> onValueChanged;

    SpinBox(SceneGraph& graph, int minVal = 0, int maxVal = 100,
            float w = 140.0f, float h = 32.0f)
        : Control(graph, "SpinBox"), m_min(minVal), m_max(maxVal), m_value(minVal)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = h;
        l.minWidth = 60.0f;
        l.minHeight = h;
    }

    void setValue(int v) {
        int c = std::clamp(v, m_min, m_max);
        if (m_value != c) { m_value = c; m_graph.invalidateNode(m_nodeId, DirtySelf); onValueChanged.emit(c); }
    }
    int  value() const { return m_value; }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float btnW = b.height * 0.7f;
        if (mx >= b.x + b.width - btnW)          setValue(m_value + (my < b.y + b.height * 0.5f ? 1 : -1));
        else                                       setState(WidgetState::Pressed);
    }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float btnW = b.height * 0.7f;
        float fieldW = b.width - btnW;

        bool focused = isFocused();
        // Value field
        buf.pushRectangle(b.x, b.y, fieldW, b.height, Colors::Surface1, 6.0f,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);
        // Value text
        if (TextHelper::hasAtlas()) {
            uint8_t vc[4] = {210, 210, 220, 220};
            float ty = b.y + (b.height - TextHelper::lineHeight()) * 0.5f;
            TextHelper::pushText(buf, b.x + 8.0f, ty, std::to_string(m_value), vc, fieldW - 8.0f);
        } else {
            uint8_t vc[4] = {200, 200, 210, 180};
            buf.pushRectangle(b.x + 8.0f, b.y + (b.height-7.0f)*0.5f, fieldW * 0.6f, 7.0f, vc, 2.0f);
        }

        // Up/down button area
        float halfH = b.height * 0.5f;
        buf.pushRectangle(b.x + fieldW, b.y,          btnW, halfH, Colors::Surface2, 0.0f, 1.0f, Colors::Border);
        buf.pushRectangle(b.x + fieldW, b.y + halfH,  btnW, halfH, Colors::Surface2, 0.0f, 1.0f, Colors::Border);
        // Arrow marks (tiny rects)
        float ax = b.x + fieldW + btnW * 0.3f, aw = btnW * 0.4f;
        uint8_t ac[4] = {180, 180, 190, 200};
        buf.pushRectangle(ax, b.y + halfH * 0.35f,        aw, 2.0f, ac);  // up mark
        buf.pushRectangle(ax, b.y + halfH + halfH * 0.55f, aw, 2.0f, ac); // down mark
    }

    AISemanticNode getSemanticNode() const override {
        return {"SpinBox", "", std::to_string(m_value), true};
    }
    bool executeSemanticAction(const std::string& a) override {
        if (a.rfind("set_value:", 0) == 0) { try { setValue(std::stoi(a.substr(10))); return true; } catch (...) {} }
        if (a == "increment") { setValue(m_value + 1); return true; }
        if (a == "decrement") { setValue(m_value - 1); return true; }
        return false;
    }

private:
    int m_min, m_max, m_value;
};

// ============================================================================
// PopupItem — a flat, borderless, full-width clickable text row.
//
// The standard building block for popup list content (combo-boxes, menus,
// tree-pickers, etc.).  It has no border and no background of its own;
// the containing PopupWindow supplies the backdrop.  On hover it shows a
// subtle tint so the user knows it is interactive.
//
// onActivated fires on mouse-release while the cursor is inside.
// ============================================================================

class PopupItem : public Control {
public:
    Core::Signal<> onActivated;

    PopupItem(SceneGraph& graph, const std::string& label,
              float width = 200.f, float itemHeight = 28.f)
        : Control(graph, "PopupItem"), m_label(label)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width  = width;
        l.boundingBox.height = itemHeight;
        l.minWidth  = TextHelper::hasAtlas()
                        ? (TextHelper::measureWidth(label) + 24.f)
                        : width;
        l.minHeight = itemHeight;
    }

    void setLabel(const std::string& s) {
        m_label = s;
        auto& l = m_graph.getLayout(m_nodeId);
        l.minWidth = TextHelper::hasAtlas()
                       ? (TextHelper::measureWidth(s) + 24.f)
                       : l.boundingBox.width;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    const std::string& label() const { return m_label; }

    void handleMouseRelease(float mx, float my) override {
        if (m_state == WidgetState::Pressed && isPointInside(mx, my)) {
            onClicked.emit();
            onActivated.emit();
        }
        Control::handleMouseRelease(mx, my);
    }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;

        // Hover highlight — subtle tint only; no border, no solid fill at rest.
        if (m_state == WidgetState::Hovered || m_state == WidgetState::Pressed) {
            uint8_t hi[4] = {255, 255, 255, 18};
            buf.pushRectangle(b.x, b.y, b.width, b.height, hi, 3.0f);
        }

        // Label text
        if (TextHelper::hasAtlas()) {
            uint8_t tc[4] = {220, 220, 228, 230};
            float ty = b.y + (b.height - TextHelper::lineHeight()) * 0.5f;
            TextHelper::pushText(buf, b.x + 10.f, ty, tr(m_label), tc,
                                 b.width - 16.f);
        } else {
            // Fallback: coloured bar representing the text
            uint8_t tc[4] = {200, 200, 210, 180};
            float bw = TextHelper::hasAtlas()
                       ? TextHelper::measureWidth(m_label)
                       : b.width * 0.6f;
            buf.pushRectangle(b.x + 10.f, b.y + (b.height - 6.f) * 0.5f,
                              bw, 6.f, tc, 2.f);
        }
    }

    AISemanticNode getSemanticNode() const override {
        return {"PopupItem", m_label, "", true};
    }
    bool executeSemanticAction(const std::string& a) override {
        if (a == "click" || a == "activate") {
            onClicked.emit();
            onActivated.emit();
            return true;
        }
        return false;
    }

private:
    std::string m_label;
};

// ============================================================================
// ComboBox
// ============================================================================

enum class ComboBoxMode {
    Cycling,
    Popup
};

class ComboBox : public Control {
public:
    Core::Signal<int>         onIndexChanged;
    Core::Signal<std::string> onTextChanged;
    Core::Signal<ComboBox*>   onPopupRequested;

    ComboBox(SceneGraph& graph, std::vector<std::string> items = {},
             float w = 200.0f, float h = 36.0f)
        : Control(graph, "ComboBox"), m_items(std::move(items))
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = h;
        _updateMinSize();
    }

    void addItem(const std::string& item) {
        m_items.push_back(item);
        _updateMinSize();
    }
    void setCurrentIndex(int i) {
        int c = (m_items.empty()) ? -1 : std::clamp(i, 0, (int)m_items.size()-1);
        if (m_currentIndex != c) {
            m_currentIndex = c;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onIndexChanged.emit(c);
            if (c >= 0) onTextChanged.emit(m_items[c]);
        }
    }
    int         currentIndex() const { return m_currentIndex; }
    std::string currentText()  const {
        return (m_currentIndex >= 0 && m_currentIndex < (int)m_items.size())
               ? m_items[m_currentIndex] : "";
    }
    const std::vector<std::string>& items() const { return m_items; }

    ComboBoxMode mode() const { return m_mode; }
    void setMode(ComboBoxMode mode) { m_mode = mode; }

    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) {
            onClicked.emit();
            if (!m_items.empty()) {
                if (m_mode == ComboBoxMode::Cycling) {
                    setCurrentIndex((m_currentIndex + 1) % (int)m_items.size());
                } else if (m_mode == ComboBoxMode::Popup) {
                    onPopupRequested.emit(this);
                }
            }
        }
    }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float arrowW = b.height * 0.75f;
        const uint8_t* fill = (m_state == WidgetState::Hovered) ? Colors::Surface3 : Colors::Surface2;
        // Main box
        bool focused = isFocused();
        buf.pushRectangle(b.x, b.y, b.width, b.height, fill, 6.0f,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);
        // Arrow area
        buf.pushRectangle(b.x + b.width - arrowW, b.y + 1.0f, arrowW - 1.0f, b.height - 2.0f,
                          Colors::Surface3, 5.0f);
        // Arrow chevron (two rects forming a V)
        float ax = b.x + b.width - arrowW * 0.62f, ay = b.y + b.height * 0.38f;
        uint8_t ac[4] = {180, 180, 190, 220};
        buf.pushRectangle(ax - 4.0f, ay, 5.0f, 2.0f, ac, 1.0f);
        buf.pushRectangle(ax + 1.0f, ay, 5.0f, 2.0f, ac, 1.0f);
        // Selected item text
        if (TextHelper::hasAtlas() && !currentText().empty()) {
            uint8_t tc[4] = {210, 210, 220, 220};
            float ty = b.y + (b.height - TextHelper::lineHeight()) * 0.5f;
            TextHelper::pushText(buf, b.x + 8.0f, ty, tr(currentText()), tc, b.width - arrowW - 14.0f);
        } else {
            uint8_t tc[4] = {200, 200, 210, 180};
            buf.pushRectangle(b.x + 8.0f, b.y + (b.height-7.0f)*0.5f,
                              b.width - arrowW - 14.0f, 7.0f, tc, 2.0f);
        }
    }

    AISemanticNode getSemanticNode() const override {
        return {"ComboBox", "", currentText(), true};
    }
    bool executeSemanticAction(const std::string& a) override {
        if (a.rfind("select:", 0) == 0) {
            auto needle = a.substr(7);
            for (int i = 0; i < (int)m_items.size(); ++i)
                if (m_items[i] == needle) { setCurrentIndex(i); return true; }
        }
        if (a == "click" || a == "activate") {
            onClicked.emit();
            if (!m_items.empty() && m_mode == ComboBoxMode::Popup) {
                onPopupRequested.emit(this);
            }
            return true;
        }
        return Control::executeSemanticAction(a);
    }

private:
    void _updateMinSize() {
        auto& l = m_graph.getLayout(m_nodeId);
        float maxItemW = 0.f;
        for (const auto& item : m_items) {
            float lw = TextHelper::hasAtlas() ? TextHelper::measureWidth(tr(item)) : 40.f;
            if (lw > maxItemW) maxItemW = lw;
        }
        l.minWidth = maxItemW + 36.0f;
        l.minHeight = m_graph.getLayoutConst(m_nodeId).boundingBox.height;
    }

    std::vector<std::string> m_items;
    int m_currentIndex{-1};
    ComboBoxMode m_mode{ComboBoxMode::Cycling};
};

// ============================================================================
// TornTabState — provenance carried by a DockWidget born from a tear-off.
// Value type: no vtable, cheap to move.
// ============================================================================

struct TornTabState {
    std::string title;
    int         originIndex{-1};
    NodeId      contentNode{InvalidNodeId};
    // Called when the DockWidget is dragged back over a valid DockZone.
    // Args: (originIndex, contentNode) — the bar re-inserts the tab.
    std::function<void(int, NodeId)> reattach;
};

// ============================================================================
// TabBar
// ============================================================================

class TabBar : public Control {
public:
    static constexpr float TEAR_THRESHOLD = 8.0f; // px of movement to initiate a tear

    Core::Signal<int>          onTabChanged;
    Core::Signal<int, NodeId>  onTabTorn;   // (tabIndex, contentNode) — app should create a DockWidget

    TabBar(SceneGraph& graph, std::vector<std::string> tabs = {},
           float w = 400.0f, float h = 36.0f)
        : Control(graph, "TabBar"), m_tabs(std::move(tabs))
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = h;
        if (!m_tabs.empty()) m_activeIndex = 0;
        _updateMinSize();
    }

    void addTab(const std::string& label, NodeId contentNode = InvalidNodeId) {
        m_tabs.push_back(label);
        m_contentNodes.push_back(contentNode);
        if (m_activeIndex < 0) m_activeIndex = 0;
        _updateMinSize();
    }

    void setActiveTab(int i) {
        if (i < 0 || i >= (int)m_tabs.size() || i == m_activeIndex) return;
        m_activeIndex = i;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        onTabChanged.emit(i);
    }
    int activeTab() const { return m_activeIndex; }

    // --- Tearable mode ---
    void setTearable(bool t) { m_tearable = t; }
    bool isTearable() const  { return m_tearable; }

    // Associate a SceneGraph content subtree with a tab (required for tear-off)
    void setTabContentNode(int index, NodeId node) {
        if (index >= 0 && index < (int)m_contentNodes.size())
            m_contentNodes[index] = node;
    }

    // Peek whether a tab was torn this frame — consumed by the application
    bool hasTornTab() const { return m_pendingTorn.has_value(); }
    TornTabState consumeTornTab() {
        auto s = std::move(*m_pendingTorn);
        m_pendingTorn.reset();
        return s;
    }

    // Last drag cursor position — used to place the new DockWidget on creation
    float lastDragX() const { return m_drag.curX; }
    float lastDragY() const { return m_drag.curY; }

    // Called by the app when a DockWidget is re-docked onto this bar
    void reinsertTab(int originIndex, NodeId contentNode, const std::string& title) {
        int clampedIdx = std::clamp(originIndex, 0, (int)m_tabs.size());
        m_tabs.insert(m_tabs.begin() + clampedIdx, title);
        m_contentNodes.insert(m_contentNodes.begin() + clampedIdx, contentNode);
        if (m_activeIndex >= clampedIdx) m_activeIndex++;
        setActiveTab(clampedIdx);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        _updateMinSize();
    }

    // Draw a translucent ghost tab at the current drag position (call from render loop)
    void populateDragGhost(PrimitiveBuffer& buf) const {
        if (!m_drag.active || !m_drag.tornOff) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float tabW = m_tabs.empty() ? 120.0f : b.width / static_cast<float>(m_tabs.size() + 1);
        float gx = m_drag.curX - tabW * 0.5f;
        float gy = m_drag.curY - b.height * 0.5f;
        uint8_t ghostFill[4]   = {40, 40, 50, 180};
        uint8_t ghostBorder[4] = {80, 130, 255, 200};
        buf.pushRectangle(gx, gy, tabW, b.height, ghostFill, 6.0f, 1.5f, ghostBorder);
        uint8_t lc[4] = {200, 210, 255, 180};
        buf.pushRectangle(gx + 8.0f, gy + (b.height - 6.0f) * 0.5f, tabW - 16.0f, 6.0f, lc, 2.0f);
    }

    // --- Input handling ---
    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my) || m_tabs.empty()) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float tabW = b.width / static_cast<float>(m_tabs.size());
        int idx = std::clamp(static_cast<int>((mx - b.x) / tabW), 0, (int)m_tabs.size()-1);

        if (m_tearable) {
            m_drag = { idx, mx, my, mx, my, false, false };
        } else {
            setActiveTab(idx);
        }
    }

    void handleMouseMove(float mx, float my) override {
        Control::handleMouseMove(mx, my);
        if (!m_drag.active && m_drag.pressedIndex < 0) return;

        m_drag.curX = mx;
        m_drag.curY = my;

        float dx = mx - m_drag.pressX;
        float dy = my - m_drag.pressY;
        float dist = std::sqrt(dx*dx + dy*dy);

        if (!m_drag.active && dist > TEAR_THRESHOLD) {
            m_drag.active = true;
            setActiveTab(m_drag.pressedIndex);
        }

        if (m_drag.active && !m_drag.tornOff) {
            // Cross the vertical threshold to commit the tear
            if (std::abs(dy) > TEAR_THRESHOLD * 2.0f) {
                m_drag.tornOff = true;
                _emitTear(m_drag.pressedIndex);
            }
        }
    }

    void handleMouseRelease(float mx, float my) override {
        if (m_drag.pressedIndex >= 0 && !m_drag.active) {
            // Short press with no drag — just select the tab
            setActiveTab(m_drag.pressedIndex);
        }
        m_drag = {};
        Control::handleMouseRelease(mx, my);
    }

    // Programmatically tear off a tab — useful for keyboard shortcuts and testing.
    void forceTear(int idx) { _emitTear(idx); }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        if (m_tabs.empty()) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float tabW = b.width / static_cast<float>(m_tabs.size());

        bool focused = isFocused();
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface1, 6.0f,
                          focused ? 1.5f : 0.0f,
                          focused ? Colors::Accent : Colors::Border);

        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            float tx   = b.x + i * tabW;
            bool active = (i == m_activeIndex);
            // Highlight the tab being dragged (pre-tear)
            bool dragging = m_drag.active && !m_drag.tornOff && (i == m_drag.pressedIndex);

            const uint8_t* fill = dragging ? Colors::AccentPress
                                 : active   ? Colors::Surface3
                                            : Colors::Surface1;
            float radius = (i == 0) ? 6.0f : (i == (int)m_tabs.size()-1) ? 6.0f : 0.0f;
            buf.pushRectangle(tx + 1.0f, b.y + 1.0f, tabW - 2.0f, b.height - 2.0f, fill, radius);

            if (active && !dragging)
                buf.pushRectangle(tx + 4.0f, b.y + b.height - 3.0f, tabW - 8.0f, 2.5f, Colors::Accent, 1.0f);

            // Tearable indicator: small dot in top-right of each tab when tearable
            if (m_tearable && !active) {
                uint8_t dot[4] = {80, 80, 100, 120};
                buf.pushRectangle(tx + tabW - 8.0f, b.y + 5.0f, 3.0f, 3.0f, dot, 1.5f);
            }

            if (TextHelper::hasAtlas()) {
                std::string label = tr(m_tabs[i]);
                float lw = TextHelper::measureWidth(label);
                uint8_t lc[4] = {active ? (uint8_t)220 : (uint8_t)140,
                                  active ? (uint8_t)220 : (uint8_t)140,
                                  active ? (uint8_t)228 : (uint8_t)148,
                                  active ? (uint8_t)220 : (uint8_t)140};
                float ly = b.y + (b.height - TextHelper::lineHeight()) * 0.5f;
                TextHelper::pushText(buf, tx + (tabW - lw) * 0.5f, ly, label, lc);
            } else {
                uint8_t lc[4] = {active ? (uint8_t)220 : (uint8_t)140,
                                  active ? (uint8_t)220 : (uint8_t)140,
                                  active ? (uint8_t)228 : (uint8_t)148,
                                  active ? (uint8_t)220 : (uint8_t)140};
                float lw = tabW * 0.5f;
                buf.pushRectangle(tx + (tabW - lw)*0.5f, b.y + (b.height-6.0f)*0.5f, lw, 6.0f, lc, 2.0f);
            }
        }
    }

    AISemanticNode getSemanticNode() const override {
        return {"TabBar", "", std::to_string(m_activeIndex), true};
    }
    bool executeSemanticAction(const std::string& a) override {
        if (a.rfind("select_tab:", 0) == 0) { try { setActiveTab(std::stoi(a.substr(11))); return true; } catch (...) {} }
        return false;
    }

private:
    void _updateMinSize() {
        auto& l = m_graph.getLayout(m_nodeId);
        float totalTextW = 0.0f;
        for (const auto& tab : m_tabs) {
            float lw = TextHelper::hasAtlas() ? TextHelper::measureWidth(tr(tab)) : 50.0f;
            totalTextW += lw + 24.0f;
        }
        l.minWidth = totalTextW + 8.0f;
        l.minHeight = m_graph.getLayoutConst(m_nodeId).boundingBox.height;
    }

    void _emitTear(int idx) {
        if (idx < 0 || idx >= (int)m_tabs.size()) return;

        std::string title     = m_tabs[idx];
        NodeId      content   = (idx < (int)m_contentNodes.size()) ? m_contentNodes[idx] : InvalidNodeId;

        // Remove the tab from the bar
        m_tabs.erase(m_tabs.begin() + idx);
        if (idx < (int)m_contentNodes.size()) m_contentNodes.erase(m_contentNodes.begin() + idx);
        m_activeIndex = m_tabs.empty() ? -1 : std::clamp(m_activeIndex, 0, (int)m_tabs.size()-1);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        _updateMinSize();

        // Build TornTabState with a re-attach closure bound to this TabBar
        TornTabState state;
        state.title       = title;
        state.originIndex = idx;
        state.contentNode = content;
        state.reattach    = [this, title](int origIdx, NodeId node) {
            reinsertTab(origIdx, node, title);
        };

        m_pendingTorn = std::move(state);
        onTabTorn.emit(idx, content);
    }

    struct DragState {
        int   pressedIndex{-1};
        float pressX{0}, pressY{0};
        float curX{0},   curY{0};
        bool  active{false};
        bool  tornOff{false};
    };

    std::vector<std::string> m_tabs;
    std::vector<NodeId>      m_contentNodes;
    int    m_activeIndex{-1};
    bool   m_tearable{false};
    DragState                m_drag{};
    std::optional<TornTabState> m_pendingTorn;
};

// ============================================================================
// GroupBox  (labelled container panel)
// ============================================================================

class GroupBox : public Widget {
public:
    GroupBox(SceneGraph& graph, const std::string& title,
             float w = 320.0f, float h = 120.0f)
        : Widget(graph, "GroupBox: " + title), m_title(title)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width  = w;
        l.boundingBox.height = h;
        l.padding = 12.0f;
        l.gap     = 8.0f;
        l.direction = FlexDirection::Column;
    }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        // Panel body
        uint8_t panelFill[4] = {22, 22, 25, 180};
        buf.pushRectangle(b.x, b.y, b.width, b.height, panelFill, 8.0f, 1.0f, Colors::Border);
        // Title bar strip at top
        uint8_t titleBg[4] = {36, 36, 40, 200};
        buf.pushRectangle(b.x + 1.0f, b.y + 1.0f, b.width - 2.0f, 24.0f, titleBg, 7.0f);
        // Title text
        if (TextHelper::hasAtlas()) {
            uint8_t tc[4] = {200, 200, 210, 200};
            float ty = b.y + (24.0f - TextHelper::lineHeight()) * 0.5f;
            TextHelper::pushText(buf, b.x + 10.0f, ty, tr(m_title), tc, b.width - 20.0f);
        } else {
            uint8_t tc[4] = {200, 200, 210, 180};
            buf.pushRectangle(b.x + 10.0f, b.y + 9.0f, b.width * 0.4f, 7.0f, tc, 2.0f);
        }
    }

    AISemanticNode getSemanticNode() const override { return {"GroupBox", m_title, "", false}; }
    bool executeSemanticAction(const std::string&) override { return false; }

private:
    std::string m_title;
};

// ============================================================================
// ScrollArea
// ============================================================================

class ScrollArea : public Widget {
public:
    ScrollArea(SceneGraph& graph, float w = 320.0f, float h = 200.0f)
        : Widget(graph, "ScrollArea")
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width  = w;
        l.boundingBox.height = h;
    }

    void addChildWidget(Widget* w) {
        m_children.push_back(w);
    }

    const std::vector<Widget*>& children() const { return m_children; }

    void handleMouseMove(float mx, float my) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            m_hovered = true;
            for (Widget* w : m_children) {
                if (w->isVisible()) w->handleMouseMove(mx, my);
            }
        } else {
            m_hovered = false;
        }
    }

    void handleMousePress(float mx, float my) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            float trackW = 10.0f;
            float trackX = b.x + b.width - trackW;
            if (mx >= trackX) {
                m_draggingScroll = true;
            } else {
                for (Widget* w : m_children) {
                    if (w->isVisible()) w->handleMousePress(mx, my);
                }
            }
        }
    }

    void handleMouseRelease(float mx, float my) override {
        m_draggingScroll = false;
        for (Widget* w : m_children) {
            if (w->isVisible()) w->handleMouseRelease(mx, my);
        }
    }

    void handleScroll(float mx, float my, float wheel) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            float totalH = 12.0f;
            for (Widget* w : m_children) {
                totalH += m_graph.getLayoutConst(w->getNodeId()).boundingBox.height + 6.0f;
            }
            float maxScrollY = std::max(0.0f, totalH - b.height);
            m_scrollY = std::clamp(m_scrollY - wheel * 40.0f, 0.0f, maxScrollY);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            
            for (Widget* w : m_children) {
                if (w->isVisible()) w->handleScroll(mx, my, wheel);
            }
        }
    }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        
        // Background
        uint8_t fill[4] = {20, 20, 24, 255};
        buf.pushRectangle(b.x, b.y, b.width, b.height, fill, 6.0f, 1.0f, Colors::Border);

        if (m_children.empty()) return;

        // Perform Layout
        float curY = b.y + 6.0f - m_scrollY;
        float innerW = b.width - 16.0f;
        float totalH = 12.0f;

        for (Widget* w : m_children) {
            auto& wl = m_graph.getLayout(w->getNodeId());
            wl.boundingBox.x = b.x + 8.0f;
            wl.boundingBox.y = curY;
            wl.boundingBox.width = innerW;
            
            curY += wl.boundingBox.height + 6.0f;
            totalH += wl.boundingBox.height + 6.0f;
        }

        float maxScrollY = std::max(0.0f, totalH - b.height);
        m_scrollY = std::clamp(m_scrollY, 0.0f, maxScrollY);

        curY = b.y + 6.0f - m_scrollY;
        for (Widget* w : m_children) {
            auto& wl = m_graph.getLayout(w->getNodeId());
            wl.boundingBox.y = curY;
            curY += wl.boundingBox.height + 6.0f;
        }

        // Render with clip scissor
        buf.pushClip(b.x + 1.0f, b.y + 1.0f, b.width - 13.0f, b.height - 2.0f);
        for (Widget* w : m_children) {
            const auto& wb = m_graph.getLayoutConst(w->getNodeId()).boundingBox;
            if (wb.y + wb.height >= b.y && wb.y <= b.y + b.height) {
                if (w->isVisible()) {
                    w->populateRenderPrimitives(buf);
                }
            }
        }
        buf.popClip();

        // Render Scrollbar
        if (maxScrollY > 0.0f) {
            float trackW = 10.0f;
            float trackH = b.height - 4.0f;
            float trackX = b.x + b.width - trackW - 2.0f;
            float trackY = b.y + 2.0f;

            uint8_t trackColor[4] = {30, 30, 35, 120};
            buf.pushRectangle(trackX, trackY, trackW, trackH, trackColor, 3.0f);

            float visibleRatio = b.height / totalH;
            float thumbH = std::max(20.0f, trackH * visibleRatio);
            float scrollRatio = m_scrollY / maxScrollY;
            float thumbY = trackY + scrollRatio * (trackH - thumbH);

            uint8_t thumbColor[4] = {100, 100, 110, 200};
            if (m_draggingScroll) {
                thumbColor[0] = 130; thumbColor[1] = 130; thumbColor[2] = 140;
            }
            buf.pushRectangle(trackX + 1.0f, thumbY, trackW - 2.0f, thumbH, thumbColor, 3.0f);
        }
    }

    AISemanticNode getSemanticNode() const override { return {"ScrollArea", "", "", true}; }
    bool executeSemanticAction(const std::string&) override { return false; }

private:
    std::vector<Widget*> m_children;
    float   m_scrollY{0.0f};
    bool    m_hovered{false};
    bool    m_draggingScroll{false};
};

} // namespace Genesis
