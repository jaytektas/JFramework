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
#include "../platform/Clipboard.h"
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

class Menu;

class Widget : public Core::SlotTracker, public IAIState {
public:
    inline static std::vector<Widget*> s_activeWidgets;

    Widget(SceneGraph& graph, const std::string& debugName = "")
        : m_graph(graph), m_state(WidgetState::Normal), m_debugName(debugName)
    {
        m_nodeId = m_graph.createNode(debugName);
        s_activeWidgets.push_back(this);
    }

    virtual ~Widget() {
        auto it = std::find(s_activeWidgets.begin(), s_activeWidgets.end(), this);
        if (it != s_activeWidgets.end()) {
            s_activeWidgets.erase(it);
        }
    }
    Widget(const Widget&)            = delete;
    Widget& operator=(const Widget&) = delete;

    std::string m_tooltipText;
    Menu*       m_contextMenu{nullptr};
    void setTooltip(const std::string& text) { m_tooltipText = text; }
    const std::string& tooltip() const noexcept { return m_tooltipText; }

    // Context menu — shown on right-click. The pointer is non-owning.
    void  setContextMenu(Menu* menu) { m_contextMenu = menu; }
    Menu* contextMenu() const        { return m_contextMenu; }

    static void renderTooltips(PrimitiveBuffer& buf, float mouseX, float mouseY);

    NodeId      getNodeId()  const noexcept { return m_nodeId; }
    WidgetState getState()   const noexcept { return m_state;  }
    bool        isVisible()  const noexcept { return m_visible; }
    bool        isEnabled()  const noexcept { return m_state != WidgetState::Disabled; }
    bool        isFocused()  const noexcept { return m_focused; }

    struct BBox { float x, y, width, height; };
    BBox getBoundingBox() const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        return {b.x, b.y, b.width, b.height};
    }

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

    // Schedule a repaint for this widget.
    void invalidate() { m_graph.invalidateNode(m_nodeId, DirtySelf); }

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
    virtual bool handleScroll(float /*mx*/, float /*my*/, float /*wheel*/) { return false; }

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

    // Call at the end of populateRenderPrimitives to draw a keyboard-focus ring.
    // Defined out-of-line (after Theme) — see below.
    void drawFocusRing(PrimitiveBuffer& buf) const;

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
    // Close-button shared style — used by DockWidget and floating popup windows
    inline constexpr uint8_t CloseBtn[4]      = {60,  40,  44,  160};
    inline constexpr uint8_t CloseBtnHover[4] = {220, 50,  50,  255};
    inline constexpr uint8_t CloseBtnMark[4]  = {255, 255, 255, 200};
}

// ============================================================================
// Theme — runtime-configurable style. Switch the whole app with Theme::apply().
// All palette entries mirror the Colors namespace; widgets can use either, but
// new code should prefer Theme::current() for runtime-swappability.
// ============================================================================
struct Theme {
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

    // Style dimensions
    float cornerRadius   = 6.f;
    float menuItemHeight = 28.f;
    float itemPadding    = 8.f;
    float spacing        = 4.f;
    float borderWidth    = 1.f;
    float titleBarHeight = 30.f;
    float focusRingWidth = 1.5f;
    float animSpeed      = 1.0f;

    static Theme  dark();
    static Theme  light();
    static Theme& current();
    static void   apply(Theme t);
};

inline Theme Theme::dark()  { return Theme{}; }
inline Theme Theme::light() {
    Theme t;
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
    return t;
}
inline Theme& Theme::current() { static Theme inst; return inst; }
inline void   Theme::apply(Theme t) { current() = std::move(t); }

// ============================================================================
// Action — shareable command object. Bind to menu items, toolbar buttons,
// and shortcuts. Changing enabled/checked propagates to all bound UI.
// ============================================================================
struct Action {
    std::string label;
    std::string shortcutText;   // display string e.g. "Ctrl+S" (informational)
    bool enabled  {true};
    bool checkable{false};
    bool checked  {false};

    Core::Signal<>     onTriggered;
    Core::Signal<bool> onEnabledChanged;
    Core::Signal<bool> onCheckedChanged;

    void trigger()          { if (!enabled) return;
                              if (checkable) setChecked(!checked);
                              onTriggered.emit(); }
    void setEnabled(bool e) { enabled = e;  onEnabledChanged.emit(e); }
    void setChecked(bool c) { checked = c;  onCheckedChanged.emit(c); }
};

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

inline void Widget::renderTooltips(PrimitiveBuffer& buf, float mouseX, float mouseY) {
    static Widget* lastHovered = nullptr;
    static auto hoverStart = std::chrono::steady_clock::now();

    Widget* hovered = nullptr;
    for (auto it = s_activeWidgets.rbegin(); it != s_activeWidgets.rend(); ++it) {
        Widget* w = *it;
        if (w && w->isVisible() && !w->tooltip().empty() && w->hitTest(mouseX, mouseY)) {
            hovered = w;
            break;
        }
    }

    if (hovered != lastHovered) {
        lastHovered = hovered;
        hoverStart = std::chrono::steady_clock::now();
    }

    if (hovered) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - hoverStart).count();
        if (elapsed < 500) {
            return; // 500ms delay
        }

        float padX = 8.0f;
        float padY = 6.0f;
        std::string text = hovered->tooltip();
        float textW = TextHelper::measureWidth(text);
        float textH = TextHelper::lineHeight();
        float tooltipW = textW + padX * 2.0f;
        float tooltipH = textH + padY * 2.0f;
        float x = mouseX + 12.0f;
        float y = mouseY + 12.0f;

        uint8_t shadow[4] = {0, 0, 0, 80};
        buf.pushRectangle(x + 2.f, y + 2.f, tooltipW, tooltipH, shadow, 4.0f);

        uint8_t fill[4] = {30, 30, 34, 250};
        uint8_t border[4] = {80, 80, 85, 255};
        buf.pushRectangle(x, y, tooltipW, tooltipH, fill, 4.0f, 1.0f, border);

        uint8_t textColor[4] = {240, 240, 245, 255};
        TextHelper::pushText(buf, x + padX, y + padY, text, textColor);
    }
}

inline void Widget::drawFocusRing(PrimitiveBuffer& buf) const {
    if (m_state != WidgetState::Focused) return;
    const auto& bb  = m_graph.getLayoutConst(m_nodeId).boundingBox;
    const auto& th  = Theme::current();
    float p = th.focusRingWidth * 0.5f + 1.f;
    uint8_t ring[4] = {th.Accent[0], th.Accent[1], th.Accent[2], 210};
    uint8_t none[4] = {0, 0, 0, 0};
    buf.pushRectangle(bb.x - p, bb.y - p, bb.width + p * 2.f, bb.height + p * 2.f,
                      none, th.cornerRadius + 1.f, th.focusRingWidth, ring);
}

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
        drawBackground(buf, b, fill, focused);
        drawLabel(buf, b);
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

protected:
    virtual void drawBackground(PrimitiveBuffer& buf, const Rect& b, const uint8_t* fill, bool focused) {
        buf.pushRectangle(b.x, b.y, b.width, b.height, fill, 6.0f,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);
    }
    virtual void drawLabel(PrimitiveBuffer& buf, const Rect& b) {
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
        drawBackground(buf, b, fill, focused);
        drawLabel(buf, b);
    }

    AISemanticNode getSemanticNode() const override {
        return {"ToggleButton", m_label, m_toggled ? "true" : "false", true};
    }
    bool executeSemanticAction(const std::string& a) override {
        if (a == "toggle") { setToggled(!m_toggled); return true; }
        return false;
    }

protected:
    virtual void drawBackground(PrimitiveBuffer& buf, const Rect& b, const uint8_t* fill, bool focused) {
        buf.pushRectangle(b.x, b.y, b.width, b.height, fill, 6.0f,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);
    }
    virtual void drawLabel(PrimitiveBuffer& buf, const Rect& b) {
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
        const uint8_t* fill = m_checked ? Colors::Accent : Colors::Surface1;
        bool focused = isFocused();
        drawBox(buf, b, boxSz, fill, focused);
        drawLabel(buf, b, boxSz);
    }

    AISemanticNode getSemanticNode() const override {
        return {"CheckBox", m_label, m_checked ? "checked" : "unchecked", true};
    }
    bool executeSemanticAction(const std::string& a) override {
        if (a == "check")   { setChecked(true);  return true; }
        if (a == "uncheck") { setChecked(false); return true; }
        return false;
    }

protected:
    virtual void drawBox(PrimitiveBuffer& buf, const Rect& b, float boxSz, const uint8_t* fill, bool focused) {
        buf.pushRectangle(b.x, b.y, boxSz, boxSz, fill, 4.0f,
                          focused ? 2.0f : 1.5f,
                          focused ? Colors::Accent : Colors::Border);
        if (m_checked) {
            uint8_t white[4] = {255, 255, 255, 220};
            buf.pushRectangle(b.x + 3.0f, b.y + boxSz*0.5f - 1.5f, boxSz - 6.0f, 3.0f, white, 1.5f);
            buf.pushRectangle(b.x + boxSz*0.5f - 1.5f, b.y + 3.0f, 3.0f, boxSz - 6.0f, white, 1.5f);
        }
    }
    virtual void drawLabel(PrimitiveBuffer& buf, const Rect& b, float boxSz) {
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
        float r = b.height;
        const uint8_t* ring = m_selected ? Colors::Accent : Colors::Surface1;
        bool focused = isFocused();
        drawCircle(buf, b, r, ring, focused);
        drawLabel(buf, b, r);
    }

    AISemanticNode getSemanticNode() const override {
        return {"RadioButton", m_label, m_selected ? "selected" : "unselected", true};
    }
    bool executeSemanticAction(const std::string& a) override {
        if (a == "select") { setSelected(true); return true; }
        return false;
    }

protected:
    virtual void drawCircle(PrimitiveBuffer& buf, const Rect& b, float r, const uint8_t* ring, bool focused) {
        float borderW = 1.5f;
        buf.pushRectangle(b.x, b.y, r, r, ring, r * 0.5f,
                          focused ? 2.0f : borderW,
                          focused ? Colors::Accent : Colors::Border);
        if (m_selected) {
            float dot = r * 0.42f, offset = (r - dot) * 0.5f;
            buf.pushRectangle(b.x + offset, b.y + offset, dot, dot, Colors::TextPrimary, dot * 0.5f);
        }
    }
    virtual void drawLabel(PrimitiveBuffer& buf, const Rect& b, float r) {
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
        drawTrack(buf, b, trackY, trackH, fillW);

        float thumbW = 16.0f, thumbH = b.height;
        float thumbX = b.x + fillW - thumbW * 0.5f;
        thumbX = std::clamp(thumbX, b.x, b.x + b.width - thumbW);
        const uint8_t* tc = (m_state == WidgetState::Pressed) ? Colors::AccentPress : Colors::TextPrimary;
        bool focused = isFocused();
        drawThumb(buf, b, thumbX, thumbW, thumbH, tc, focused);
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

protected:
    virtual void drawTrack(PrimitiveBuffer& buf, const Rect& b, float trackY, float trackH, float fillW) {
        buf.pushRectangle(b.x, trackY, b.width, trackH, Colors::Surface3, 2.0f);
        if (fillW > 0.5f)
            buf.pushRectangle(b.x, trackY, fillW, trackH, Colors::Accent, 2.0f);
    }
    virtual void drawThumb(PrimitiveBuffer& buf, const Rect& b, float thumbX, float thumbW, float thumbH, const uint8_t* tc, bool focused) {
        buf.pushRectangle(thumbX, b.y, thumbW, thumbH, tc, thumbW * 0.5f,
                          focused ? 1.5f : 0.0f,
                          focused ? Colors::Accent : Colors::Border);
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
        drawTrack(buf, b);
        drawProgressFill(buf, b, m_progress);
    }

    AISemanticNode getSemanticNode() const override {
        return {"ProgressBar", "", std::to_string(int(m_progress * 100)) + "%", false};
    }
    bool executeSemanticAction(const std::string& a) override {
        if (a.rfind("set_value:", 0) == 0) {
            try { setProgress(std::stof(a.substr(10))); return true; } catch (...) {}
        }
        return false;
    }

protected:
    virtual void drawTrack(PrimitiveBuffer& buf, const Rect& b) {
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface2, 6.0f);
    }
    virtual void drawProgressFill(PrimitiveBuffer& buf, const Rect& b, float progress) {
        if (progress > 0.005f)
            buf.pushRectangle(b.x, b.y, b.width * progress, b.height, Colors::Success, 6.0f);
    }

private:
    float m_progress;
};

// ============================================================================================
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
    bool executeSemanticAction(const std::string& a) override {
        if (a.rfind("set_value:", 0) == 0 || a.rfind("scroll_to:", 0) == 0) {
            size_t colon = a.find(':');
            try { setScrollPosition(std::stof(a.substr(colon + 1))); return true; } catch (...) {}
        }
        return false;
    }

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

    std::string selectedText() const {
        if (!m_selActive || m_selStart == m_selEnd) return {};
        size_t lo = std::min(m_selStart, m_selEnd);
        size_t hi = std::max(m_selStart, m_selEnd);
        return m_text.substr(lo, hi - lo);
    }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) return;
        onClicked.emit();
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float innerX = b.x + 8.0f;
        float innerY = b.y + 8.0f;
        float lh = TextHelper::hasAtlas() ? TextHelper::lineHeight() : 12.0f;

        float relY = my - innerY + m_scrollOffset;
        auto lines = getLines();
        size_t clickLine = static_cast<size_t>(std::max(0.0f, relY / lh));
        if (clickLine >= lines.size()) clickLine = lines.empty() ? 0 : lines.size() - 1;

        float relX = mx - innerX;
        size_t clickCol = 0;
        if (TextHelper::hasAtlas() && clickLine < lines.size()) {
            const std::string& ln = lines[clickLine];
            float cx = 0;
            for (size_t i = 0; i < ln.size(); ++i) {
                float cw = TextHelper::measureWidth(ln.substr(i, 1));
                if (cx + cw * 0.5f > relX) { clickCol = i; goto done_click; }
                cx += cw;
            }
            clickCol = ln.size();
        } else {
            if (clickLine < lines.size()) {
                clickCol = static_cast<size_t>(std::max(0.0f, relX / 6.0f));
                if (clickCol > lines[clickLine].size()) clickCol = lines[clickLine].size();
            }
        }
        done_click:
        m_cursorPos = getPosFromLineCol(clickLine, clickCol);
        m_selActive = false;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
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

        // ---- Ctrl shortcuts ----
        if (ke.ctrl) {
            if (ke.key == K::C || (ke.utf8[0]=='c'||ke.utf8[0]=='C')) {
                std::string sel = selectedText();
                if (!sel.empty()) Clipboard::setText(sel);
                return true;
            }
            if (ke.key == K::X || (ke.utf8[0]=='x'||ke.utf8[0]=='X')) {
                std::string sel = selectedText();
                if (!sel.empty()) {
                    Clipboard::setText(sel);
                    _deleteSelection();
                }
                return true;
            }
            if (ke.key == K::V || (ke.utf8[0]=='v'||ke.utf8[0]=='V')) {
                std::string clip = Clipboard::getText();
                if (!clip.empty()) {
                    _deleteSelection();
                    m_text.insert(m_cursorPos, clip);
                    m_cursorPos += clip.size();
                    m_graph.invalidateNode(m_nodeId, DirtySelf);
                    onTextChanged.emit(m_text);
                    if (AiBusHook::emit)
                        AiBusHook::emit(m_nodeId, AiBusHook::kTextChanged, m_text.c_str());
                }
                return true;
            }
            if (ke.key == K::A || (ke.utf8[0]=='a'||ke.utf8[0]=='A')) {
                m_selStart  = 0;
                m_selEnd    = m_text.size();
                m_selActive = true;
                m_cursorPos = m_selEnd;
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                return true;
            }
        }

        // ---- Movement / selection with optional Shift ----
        auto moveCursor = [&](size_t newPos) {
            if (ke.shift) {
                if (!m_selActive) { m_selStart = m_cursorPos; m_selActive = true; }
                m_selEnd    = newPos;
            } else {
                m_selActive = false;
            }
            m_cursorPos = newPos;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        };

        if (ke.key == K::Backspace) {
            if (m_selActive && m_selStart != m_selEnd) {
                _deleteSelection();
            } else if (m_cursorPos > 0 && !m_text.empty()) {
                m_text.erase(m_cursorPos - 1, 1);
                m_cursorPos--;
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                onTextChanged.emit(m_text);
                if (AiBusHook::emit)
                    AiBusHook::emit(m_nodeId, AiBusHook::kTextChanged, m_text.c_str());
            }
            return true;
        } else if (ke.key == K::Delete) {
            if (m_selActive && m_selStart != m_selEnd) {
                _deleteSelection();
            } else if (m_cursorPos < m_text.size()) {
                m_text.erase(m_cursorPos, 1);
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                onTextChanged.emit(m_text);
                if (AiBusHook::emit)
                    AiBusHook::emit(m_nodeId, AiBusHook::kTextChanged, m_text.c_str());
            }
            return true;
        } else if (ke.key == K::Return) {
            _deleteSelection();
            m_text.insert(m_cursorPos, "\n");
            m_cursorPos++;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onTextChanged.emit(m_text);
            if (AiBusHook::emit)
                AiBusHook::emit(m_nodeId, AiBusHook::kTextChanged, m_text.c_str());
            return true;
        } else if (ke.key == K::Left) {
            if (!ke.shift && m_selActive && m_selStart != m_selEnd) {
                m_cursorPos = std::min(m_selStart, m_selEnd);
                m_selActive = false;
                m_graph.invalidateNode(m_nodeId, DirtySelf);
            } else {
                moveCursor(m_cursorPos > 0 ? m_cursorPos - 1 : 0);
            }
            return true;
        } else if (ke.key == K::Right) {
            if (!ke.shift && m_selActive && m_selStart != m_selEnd) {
                m_cursorPos = std::max(m_selStart, m_selEnd);
                m_selActive = false;
                m_graph.invalidateNode(m_nodeId, DirtySelf);
            } else {
                moveCursor(m_cursorPos < m_text.size() ? m_cursorPos + 1 : m_text.size());
            }
            return true;
        } else if (ke.key == K::Up) {
            if (line > 0) moveCursor(getPosFromLineCol(line - 1, col));
            return true;
        } else if (ke.key == K::Down) {
            auto lines = getLines();
            if (line + 1 < lines.size()) moveCursor(getPosFromLineCol(line + 1, col));
            return true;
        } else if (ke.key == K::Home) {
            moveCursor(getPosFromLineCol(line, 0));
            return true;
        } else if (ke.key == K::End) {
            auto lines = getLines();
            moveCursor(getPosFromLineCol(line, lines[line].size()));
            return true;
        } else if (ke.utf8[0] != '\0' && !ke.ctrl) {
            if (static_cast<uint8_t>(ke.utf8[0]) >= 32 || ke.utf8[0] == '\t') {
                _deleteSelection();
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

        // Draw selection highlights before text
        if (m_selActive && m_selStart != m_selEnd) {
            size_t selLo = std::min(m_selStart, m_selEnd);
            size_t selHi = std::max(m_selStart, m_selEnd);
            size_t loLine = 0, loCol = 0, hiLine = 0, hiCol = 0;
            // Walk text to find line/col of selLo and selHi
            size_t l = 0, c = 0;
            for (size_t i = 0; i <= m_text.size(); ++i) {
                if (i == selLo) { loLine = l; loCol = c; }
                if (i == selHi) { hiLine = l; hiCol = c; break; }
                if (i < m_text.size()) {
                    if (m_text[i] == '\n') { ++l; c = 0; } else { ++c; }
                }
            }
            uint8_t selColor[4] = {65, 105, 225, 100}; // semi-transparent blue
            for (size_t sl = loLine; sl <= hiLine && sl < lines.size(); ++sl) {
                float lineY = innerY + sl * lh - m_scrollOffset;
                if (lineY + lh < innerY || lineY > innerY + innerH) continue;
                float startX = innerX;
                float endX   = innerX + (TextHelper::hasAtlas()
                    ? TextHelper::measureWidth(lines[sl])
                    : static_cast<float>(lines[sl].size() * 6));
                if (sl == loLine) {
                    std::string prefix = lines[sl].substr(0, loCol);
                    startX = innerX + (TextHelper::hasAtlas()
                        ? TextHelper::measureWidth(prefix)
                        : static_cast<float>(loCol * 6));
                }
                if (sl == hiLine) {
                    std::string prefix = lines[sl].substr(0, hiCol);
                    endX = innerX + (TextHelper::hasAtlas()
                        ? TextHelper::measureWidth(prefix)
                        : static_cast<float>(hiCol * 6));
                }
                float rw = endX - startX;
                if (rw > 0) buf.pushRectangle(startX, lineY, rw, lh, selColor);
            }
        }

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
    void _deleteSelection() {
        if (!m_selActive || m_selStart == m_selEnd) return;
        size_t lo = std::min(m_selStart, m_selEnd);
        size_t hi = std::max(m_selStart, m_selEnd);
        m_text.erase(lo, hi - lo);
        m_cursorPos = lo;
        m_selActive = false;
        onTextChanged.emit(m_text);
        if (AiBusHook::emit)
            AiBusHook::emit(m_nodeId, AiBusHook::kTextChanged, m_text.c_str());
    }

    std::string m_text;
    std::string m_placeholder;
    size_t      m_cursorPos{0};
    float       m_scrollOffset{0.0f};
    size_t      m_selStart{0};
    size_t      m_selEnd{0};
    bool        m_selActive{false};
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
        if (m_draggingScroll) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            float totalH = 12.0f;
            for (Widget* w : m_children)
                totalH += m_graph.getLayoutConst(w->getNodeId()).boundingBox.height + 6.0f;
            float maxScrollY = std::max(0.0f, totalH - b.height);
            float trackH = b.height - 4.0f;
            float thumbH = std::max(20.0f, trackH * (b.height / totalH));
            float thumbRange = trackH - thumbH;
            if (thumbRange > 0.0f) {
                m_scrollY = std::clamp(m_dragStartScrollY + (my - m_dragStartY) * maxScrollY / thumbRange, 0.0f, maxScrollY);
                m_graph.invalidateNode(m_nodeId, DirtySelf);
            }
            return;
        }
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
            float trackX = b.x + b.width - trackW - 6.0f;
            if (mx >= trackX) {
                m_draggingScroll = true;
                m_dragStartY = my;
                m_dragStartScrollY = m_scrollY;
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

    bool handleScroll(float mx, float my, float wheel) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            bool consumed = false;
            for (Widget* w : m_children) {
                if (w->isVisible()) {
                    if (w->handleScroll(mx, my, wheel)) {
                        consumed = true;
                    }
                }
            }
            if (consumed) return true;

            float totalH = 12.0f;
            for (Widget* w : m_children) {
                totalH += m_graph.getLayoutConst(w->getNodeId()).boundingBox.height + 6.0f;
            }
            float maxScrollY = std::max(0.0f, totalH - b.height);
            m_scrollY = std::clamp(m_scrollY - wheel * 40.0f, 0.0f, maxScrollY);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        }
        return false;
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
    bool executeSemanticAction(const std::string& a) override {
        if (a.rfind("scroll_to:", 0) == 0) {
            try {
                m_scrollY = std::max(0.0f, std::stof(a.substr(10)));
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                return true;
            } catch (...) {}
        }
        return false;
    }

private:
    std::vector<Widget*> m_children;
    float   m_scrollY{0.0f};
    bool    m_hovered{false};
    bool    m_draggingScroll{false};
    float   m_dragStartY{0.0f};
    float   m_dragStartScrollY{0.0f};
};

// ============================================================================
// ListView
// ============================================================================

class ListView : public Control {
public:
    Core::Signal<int> onSelectionChanged;
    Core::Signal<int> onItemActivated;

    ListView(SceneGraph& graph, std::vector<std::string> items = {},
             float w = 240.0f, float h = 200.0f)
        : Control(graph, "ListView"), m_items(std::move(items))
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = h;
        l.minWidth = 80.0f;
        l.minHeight = 40.0f;
    }

    void setItems(const std::vector<std::string>& items) {
        m_items = items;
        m_selectedIndex = (m_items.empty()) ? -1 : std::clamp(m_selectedIndex, 0, (int)m_items.size()-1);
        m_scrollY = 0.0f;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    const std::vector<std::string>& items() const { return m_items; }

    void setSelectedIndex(int index) {
        int nextIdx = (m_items.empty()) ? -1 : std::clamp(index, 0, (int)m_items.size()-1);
        if (m_selectedIndex != nextIdx) {
            m_selectedIndex = nextIdx;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onSelectionChanged.emit(nextIdx);
        }
    }
    int selectedIndex() const { return m_selectedIndex; }

    void handleMouseMove(float mx, float my) override {
        if (m_draggingScroll) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            float itemH = TextHelper::hasAtlas() ? TextHelper::lineHeight() + 8.0f : 20.0f;
            float totalH = m_items.size() * itemH + 8.0f;
            float maxScrollY = std::max(0.0f, totalH - b.height);
            float trackH = b.height - 4.0f;
            float thumbH = std::max(20.0f, trackH * (b.height / totalH));
            float thumbRange = trackH - thumbH;
            if (thumbRange > 0.0f) {
                m_scrollY = std::clamp(m_dragStartScrollY + (my - m_dragStartY) * maxScrollY / thumbRange, 0.0f, maxScrollY);
                m_graph.invalidateNode(m_nodeId, DirtySelf);
            }
            return;
        }
        Control::handleMouseMove(mx, my);
    }

    void handleMousePress(float mx, float my) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            onClicked.emit();
            float trackW = 10.0f;
            float trackX = b.x + b.width - trackW - 6.0f;
            if (mx >= trackX) {
                m_draggingScroll = true;
                m_dragStartY = my;
                m_dragStartScrollY = m_scrollY;
            } else {
                float itemH = TextHelper::hasAtlas() ? TextHelper::lineHeight() + 8.0f : 20.0f;
                float relativeY = my - b.y + m_scrollY - 4.0f;
                int clickedIndex = static_cast<int>(relativeY / itemH);
                if (clickedIndex >= 0 && clickedIndex < (int)m_items.size()) {
                    setSelectedIndex(clickedIndex);
                    onItemActivated.emit(clickedIndex);
                }
            }
        }
    }

    void handleMouseRelease(float mx, float my) override {
        m_draggingScroll = false;
        Control::handleMouseRelease(mx, my);
    }

    bool handleScroll(float mx, float my, float wheel) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            float itemH = TextHelper::hasAtlas() ? TextHelper::lineHeight() + 8.0f : 20.0f;
            float totalH = m_items.size() * itemH + 8.0f;
            float maxScrollY = std::max(0.0f, totalH - b.height);
            m_scrollY = std::clamp(m_scrollY - wheel * 30.0f, 0.0f, maxScrollY);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        }
        return false;
    }

    bool handleKeyEvent(const KeyEvent& ke) override {
        if (!ke.pressed || m_items.empty()) return false;
        using K = KeyEvent::Key;

        if (ke.key == K::Down) {
            setSelectedIndex(m_selectedIndex + 1);
            _ensureIndexVisible(m_selectedIndex);
            return true;
        } else if (ke.key == K::Up) {
            setSelectedIndex(m_selectedIndex - 1);
            _ensureIndexVisible(m_selectedIndex);
            return true;
        } else if (ke.key == K::Return || ke.key == K::Space) {
            if (m_selectedIndex >= 0 && m_selectedIndex < (int)m_items.size()) {
                onItemActivated.emit(m_selectedIndex);
            }
            return true;
        }
        return false;
    }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        bool focused = isFocused();

        // Background
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface1, 6.0f,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);

        if (m_items.empty()) return;

        float itemH = TextHelper::hasAtlas() ? TextHelper::lineHeight() + 8.0f : 20.0f;
        float totalH = m_items.size() * itemH + 8.0f;
        float maxScrollY = std::max(0.0f, totalH - b.height);
        m_scrollY = std::clamp(m_scrollY, 0.0f, maxScrollY);

        int startIdx = static_cast<int>(m_scrollY / itemH);
        int endIdx = static_cast<int>((m_scrollY + b.height) / itemH) + 1;
        startIdx = std::clamp(startIdx, 0, (int)m_items.size() - 1);
        endIdx = std::clamp(endIdx, 0, (int)m_items.size() - 1);

        buf.pushClip(b.x + 1.0f, b.y + 1.0f, b.width - 13.0f, b.height - 2.0f);

        for (int i = startIdx; i <= endIdx; ++i) {
            float itemY = b.y + 4.0f + i * itemH - m_scrollY;
            float itemW = b.width - 14.0f;

            if (i == m_selectedIndex) {
                buf.pushRectangle(b.x + 4.0f, itemY, itemW - 4.0f, itemH - 2.0f, Colors::Accent, 3.0f);
            }

            if (TextHelper::hasAtlas()) {
                uint8_t tc[4] = {220, 220, 228, 220};
                if (i == m_selectedIndex) {
                    tc[0] = 255; tc[1] = 255; tc[2] = 255;
                }
                TextHelper::pushText(buf, b.x + 8.0f, itemY + 4.0f, tr(m_items[i]), tc, itemW - 12.0f);
            } else {
                uint8_t tc[4] = {200, 200, 210, 180};
                if (i == m_selectedIndex) {
                    tc[0] = 255; tc[1] = 255; tc[2] = 255;
                }
                buf.pushRectangle(b.x + 8.0f, itemY + (itemH - 6.0f) * 0.5f, itemW * 0.7f, 6.0f, tc, 2.0f);
            }
        }
        buf.popClip();

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

    AISemanticNode getSemanticNode() const override {
        std::string selectedText = (m_selectedIndex >= 0 && m_selectedIndex < (int)m_items.size()) ? m_items[m_selectedIndex] : "";
        return {"ListView", std::to_string(m_items.size()) + " items", selectedText, true};
    }

    bool executeSemanticAction(const std::string& a) override {
        if (a.rfind("select_index:", 0) == 0) {
            try {
                int idx = std::stoi(a.substr(13));
                setSelectedIndex(idx);
                _ensureIndexVisible(idx);
                return true;
            } catch (...) {}
        }
        if (a.rfind("select:", 0) == 0) {
            std::string text = a.substr(7);
            for (int i = 0; i < (int)m_items.size(); ++i) {
                if (m_items[i] == text) {
                    setSelectedIndex(i);
                    _ensureIndexVisible(i);
                    return true;
                }
            }
        }
        return false;
    }

private:
    void _ensureIndexVisible(int index) {
        if (index < 0 || index >= (int)m_items.size()) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float itemH = TextHelper::hasAtlas() ? TextHelper::lineHeight() + 8.0f : 20.0f;
        float itemY = index * itemH;
        if (itemY < m_scrollY) {
            m_scrollY = itemY;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        } else if (itemY + itemH > m_scrollY + b.height) {
            m_scrollY = itemY + itemH - b.height;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        }
    }

    std::vector<std::string> m_items;
    int                      m_selectedIndex{-1};
    float                    m_scrollY{0.0f};
    bool                     m_draggingScroll{false};
    float                    m_dragStartY{0.0f};
    float                    m_dragStartScrollY{0.0f};
};

struct TreeViewNode {
    std::string label;
    bool expanded{false};
    bool selected{false};
    std::vector<TreeViewNode> children;
};

class TreeView : public Control {
public:
    Core::Signal<TreeViewNode*> onSelectionChanged;
    Core::Signal<TreeViewNode*> onNodeActivated;

    TreeView(SceneGraph& graph, float w = 240.0f, float h = 300.0f)
        : Control(graph, "TreeView"), m_root{"Root", true, false, {}}
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = h;
        l.minWidth = 80.0f;
        l.minHeight = 40.0f;
    }

    TreeViewNode& root() { return m_root; }
    const TreeViewNode& root() const { return m_root; }

    void setRootNode(TreeViewNode rootNode) {
        m_root = std::move(rootNode);
        m_selectedNode = nullptr;
        m_scrollY = 0.0f;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    float rowHeight() const { return m_rowHeight; }
    void setRowHeight(float h) { m_rowHeight = h; m_graph.invalidateNode(m_nodeId, DirtySelf); }

    float getItemHeight() const {
        return m_rowHeight > 0.0f ? m_rowHeight : (TextHelper::hasAtlas() ? TextHelper::lineHeight() + 8.0f : 22.0f);
    }

    struct FlatNode {
        TreeViewNode* node;
        int depth;
        size_t flatIndex;
    };

    std::vector<FlatNode> getFlatNodes() {
        std::vector<FlatNode> flat;
        for (auto& child : m_root.children) {
            _flatten(child, 0, flat);
        }
        return flat;
    }

    void handleMousePress(float mx, float my) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            onClicked.emit();
            float trackW = 10.0f;
            float trackX = b.x + b.width - trackW;
            if (mx >= trackX) {
                m_draggingScroll = true;
                m_dragStartY = my;
                m_dragStartScrollY = m_scrollY;
            } else {
                auto flatNodes = getFlatNodes();
                float itemH = getItemHeight();
                float relativeY = my - b.y + m_scrollY - 4.0f;
                int clickedIndex = static_cast<int>(relativeY / itemH);
                if (clickedIndex >= 0 && clickedIndex < (int)flatNodes.size()) {
                    auto& flat = flatNodes[clickedIndex];
                    float indent = flat.depth * 16.0f + 6.0f;
                    float arrowW = 16.0f;
                    if (!flat.node->children.empty() && mx >= b.x + indent && mx <= b.x + indent + arrowW) {
                        flat.node->expanded = !flat.node->expanded;
                        m_graph.invalidateNode(m_nodeId, DirtySelf);
                    } else {
                        _selectNode(flat.node);
                        onNodeActivated.emit(flat.node);
                    }
                }
            }
        }
    }

    void handleMouseRelease(float mx, float my) override {
        m_draggingScroll = false;
        Control::handleMouseRelease(mx, my);
    }

    void handleMouseMove(float mx, float my) override {
        if (m_draggingScroll) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            auto flatNodes = getFlatNodes();
            float itemH = getItemHeight();
            float totalH = flatNodes.size() * itemH + 8.0f;
            float maxScrollY = std::max(0.0f, totalH - b.height);
            float trackH = b.height - 8.0f;
            float handleH = std::max(20.0f, (b.height / totalH) * trackH);
            float thumbRange = trackH - handleH;
            if (thumbRange > 0.0f) {
                m_scrollY = std::clamp(m_dragStartScrollY + (my - m_dragStartY) * maxScrollY / thumbRange, 0.0f, maxScrollY);
                m_graph.invalidateNode(m_nodeId, DirtySelf);
            }
            return;
        }
        Control::handleMouseMove(mx, my);
    }

    bool handleScroll(float mx, float my, float wheel) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            auto flatNodes = getFlatNodes();
            float itemH = getItemHeight();
            float totalH = flatNodes.size() * itemH + 8.0f;
            float maxScrollY = std::max(0.0f, totalH - b.height);
            m_scrollY = std::clamp(m_scrollY - wheel * 30.0f, 0.0f, maxScrollY);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        }
        return false;
    }

    bool handleKeyEvent(const KeyEvent& ke) override {
        if (!ke.pressed) return false;
        auto flatNodes = getFlatNodes();
        if (flatNodes.empty()) return false;

        int selIdx = -1;
        if (m_selectedNode) {
            for (int i = 0; i < (int)flatNodes.size(); ++i) {
                if (flatNodes[i].node == m_selectedNode) {
                    selIdx = i;
                    break;
                }
            }
        }

        using K = KeyEvent::Key;
        if (ke.key == K::Down) {
            int nextIdx = (selIdx == -1) ? 0 : std::clamp(selIdx + 1, 0, (int)flatNodes.size() - 1);
            _selectNode(flatNodes[nextIdx].node);
            _ensureIndexVisible(nextIdx);
            return true;
        } else if (ke.key == K::Up) {
            int nextIdx = (selIdx == -1) ? 0 : std::clamp(selIdx - 1, 0, (int)flatNodes.size() - 1);
            _selectNode(flatNodes[nextIdx].node);
            _ensureIndexVisible(nextIdx);
            return true;
        } else if (ke.key == K::Right) {
            if (selIdx != -1) {
                auto* n = flatNodes[selIdx].node;
                if (!n->children.empty()) {
                    if (!n->expanded) {
                        n->expanded = true;
                        m_graph.invalidateNode(m_nodeId, DirtySelf);
                    } else {
                        int nextIdx = std::clamp(selIdx + 1, 0, (int)flatNodes.size() - 1);
                        _selectNode(flatNodes[nextIdx].node);
                        _ensureIndexVisible(nextIdx);
                    }
                    return true;
                }
            }
        } else if (ke.key == K::Left) {
            if (selIdx != -1) {
                auto* n = flatNodes[selIdx].node;
                if (!n->children.empty() && n->expanded) {
                    n->expanded = false;
                    m_graph.invalidateNode(m_nodeId, DirtySelf);
                    return true;
                } else {
                    TreeViewNode* parent = _findParent(&m_root, n);
                    if (parent && parent != &m_root) {
                        _selectNode(parent);
                        for (int i = 0; i < (int)flatNodes.size(); ++i) {
                            if (flatNodes[i].node == parent) {
                                _ensureIndexVisible(i);
                                break;
                            }
                        }
                        return true;
                    }
                }
            }
        } else if (ke.key == K::Return || ke.key == K::Space) {
            if (selIdx != -1) {
                onNodeActivated.emit(flatNodes[selIdx].node);
                return true;
            }
        }

        return false;
    }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        bool focused = isFocused();

        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface1, 6.0f,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);

        auto flatNodes = getFlatNodes();
        if (flatNodes.empty()) return;

        float itemH = getItemHeight();
        float totalH = flatNodes.size() * itemH + 8.0f;
        float maxScrollY = std::max(0.0f, totalH - b.height);
        m_scrollY = std::clamp(m_scrollY, 0.0f, maxScrollY);

        int startIdx = static_cast<int>(m_scrollY / itemH);
        int endIdx = static_cast<int>((m_scrollY + b.height) / itemH) + 1;
        startIdx = std::clamp(startIdx, 0, (int)flatNodes.size() - 1);
        endIdx = std::clamp(endIdx, 0, (int)flatNodes.size() - 1);

        buf.pushClip(b.x + 1.0f, b.y + 1.0f, b.width - 13.0f, b.height - 2.0f);

        for (int i = startIdx; i <= endIdx; ++i) {
            auto& flat = flatNodes[i];
            float itemY = b.y + 4.0f + i * itemH - m_scrollY;
            float indent = flat.depth * 16.0f + 6.0f;

            if (flat.node == m_selectedNode) {
                drawNodeBackground(buf, flat.node, {b.x + 4.0f, itemY, b.width - 18.0f, itemH});
            }

            if (!flat.node->children.empty()) {
                float ax = b.x + indent + 8.0f;
                float ay = itemY + itemH * 0.5f;
                drawNodeChevron(buf, flat.node, ax, ay, 10.0f, flat.node->expanded);
            }

            float textX = b.x + indent + 16.0f;
            float ty = itemY + (itemH - (TextHelper::hasAtlas() ? TextHelper::lineHeight() : 8.0f)) * 0.5f;
            drawNodeText(buf, flat.node, textX, ty, b.width - indent - 30.0f);
        }

        buf.popClip();

        if (totalH > b.height) {
            float trackW = 8.0f;
            float trackX = b.x + b.width - trackW - 2.0f;
            buf.pushRectangle(trackX, b.y + 2.0f, trackW, b.height - 4.0f, Colors::Surface2, 4.0f);

            float handleH = std::max(20.0f, (b.height / totalH) * (b.height - 8.0f));
            float handleY = b.y + 4.0f + (m_scrollY / maxScrollY) * (b.height - 8.0f - handleH);
            buf.pushRectangle(trackX + 1.0f, handleY, trackW - 2.0f, handleH, Colors::Surface3, 3.0f);
        }
    }

    AISemanticNode getSemanticNode() const override {
        std::string val = m_selectedNode ? m_selectedNode->label : "";
        return {"TreeView", "", val, true};
    }

    bool executeSemanticAction(const std::string& a) override {
        if (a.rfind("select:", 0) == 0) {
            std::string label = a.substr(7);
            auto flatNodes = getFlatNodes();
            for (auto& flat : flatNodes) {
                if (flat.node->label == label) {
                    _selectNode(flat.node);
                    return true;
                }
            }
        }
        return false;
    }

    TreeViewNode* selectedNode() const { return m_selectedNode; }

protected:
    virtual void drawNodeBackground(PrimitiveBuffer& buf, TreeViewNode* node, const Rect& bounds) {
        uint8_t selBg[4] = {10, 132, 255, 60};
        buf.pushRectangle(bounds.x, bounds.y, bounds.width, bounds.height, selBg, 4.0f);
    }

    virtual void drawNodeChevron(PrimitiveBuffer& buf, TreeViewNode* node, float ax, float ay, float size, bool expanded) {
        uint8_t arrowColor[4] = {180, 180, 190, 220};
        if (expanded) {
            buf.pushRectangle(ax - 4.0f, ay - 2.0f, 5.0f, 2.0f, arrowColor, 1.0f);
            buf.pushRectangle(ax + 1.0f, ay - 2.0f, 5.0f, 2.0f, arrowColor, 1.0f);
        } else {
            buf.pushRectangle(ax - 2.0f, ay - 4.0f, 2.0f, 5.0f, arrowColor, 1.0f);
            buf.pushRectangle(ax - 2.0f, ay + 1.0f, 2.0f, 5.0f, arrowColor, 1.0f);
        }
    }

    virtual void drawNodeText(PrimitiveBuffer& buf, TreeViewNode* node, float tx, float ty, float maxW) {
        if (TextHelper::hasAtlas()) {
            uint8_t tc[4] = {210, 210, 220, 220};
            TextHelper::pushText(buf, tx, ty, tr(node->label), tc, maxW);
        } else {
            uint8_t tc[4] = {200, 200, 210, 180};
            buf.pushRectangle(tx, ty + 2.0f, 60.0f, 8.0f, tc, 2.0f);
        }
    }

private:
    void _flatten(TreeViewNode& node, int depth, std::vector<FlatNode>& result) {
        result.push_back({&node, depth, result.size()});
        if (node.expanded) {
            for (auto& child : node.children) {
                _flatten(child, depth + 1, result);
            }
        }
    }

    void _selectNode(TreeViewNode* node) {
        if (m_selectedNode != node) {
            if (m_selectedNode) m_selectedNode->selected = false;
            m_selectedNode = node;
            if (m_selectedNode) m_selectedNode->selected = true;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onSelectionChanged.emit(node);
        }
    }

    TreeViewNode* _findParent(TreeViewNode* current, TreeViewNode* target) {
        for (auto& child : current->children) {
            if (&child == target) return current;
            auto* p = _findParent(&child, target);
            if (p) return p;
        }
        return nullptr;
    }

    void _ensureIndexVisible(int index) {
        auto flatNodes = getFlatNodes();
        if (index < 0 || index >= (int)flatNodes.size()) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float itemH = getItemHeight();
        float itemY = index * itemH;
        if (itemY < m_scrollY) {
            m_scrollY = itemY;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        } else if (itemY + itemH > m_scrollY + b.height) {
            m_scrollY = itemY + itemH - b.height;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        }
    }

    TreeViewNode  m_root;
    TreeViewNode* m_selectedNode{nullptr};
    float         m_scrollY{0.0f};
    float         m_rowHeight{-1.0f};
    bool          m_draggingScroll{false};
    float         m_dragStartY{0.0f};
    float         m_dragStartScrollY{0.0f};
};

class DataGrid : public Control {
public:
    Core::Signal<int> onSelectionChanged;
    Core::Signal<int> onRowActivated;

    DataGrid(SceneGraph& graph, std::vector<std::string> headers = {}, float w = 400.0f, float h = 250.0f)
        : Control(graph, "DataGrid"), m_headers(std::move(headers))
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = h;
        l.minWidth = 100.0f;
        l.minHeight = 80.0f;
    }

    void setHeaders(const std::vector<std::string>& headers) {
        m_headers = headers;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    void setColumnWidths(const std::vector<float>& widths) {
        m_columnWidths = widths;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    void setRows(const std::vector<std::vector<std::string>>& rows) {
        m_rows = rows;
        if (m_selectedIndex != -1) {
            m_selectedIndex = m_rows.empty() ? -1 : std::clamp(m_selectedIndex, 0, (int)m_rows.size()-1);
        }
        m_scrollY = 0.0f;
        m_scrollX = 0.0f;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    const std::vector<std::string>& headers() const { return m_headers; }
    const std::vector<std::vector<std::string>>& rows() const { return m_rows; }

    void setSelectedIndex(int index) {
        int nextIdx = (m_rows.empty()) ? -1 : std::clamp(index, 0, (int)m_rows.size()-1);
        if (m_selectedIndex != nextIdx) {
            m_selectedIndex = nextIdx;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onSelectionChanged.emit(nextIdx);
        }
    }
    int selectedIndex() const { return m_selectedIndex; }

    float rowHeight() const { return m_rowHeight; }
    void setRowHeight(float h) { m_rowHeight = h; m_graph.invalidateNode(m_nodeId, DirtySelf); }

    float headerHeight() const { return m_headerHeight; }
    void setHeaderHeight(float h) { m_headerHeight = h; m_graph.invalidateNode(m_nodeId, DirtySelf); }

    float cellPadding() const { return m_cellPadding; }
    void setCellPadding(float p) { m_cellPadding = p; m_graph.invalidateNode(m_nodeId, DirtySelf); }

    float columnWidth(int colIdx, float totalW) const {
        if (colIdx >= 0 && colIdx < (int)m_columnWidths.size()) {
            return m_columnWidths[colIdx];
        }
        if (m_headers.empty()) return totalW;
        return totalW / m_headers.size();
    }

    void handleMousePress(float mx, float my) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            onClicked.emit();
            float scrollBarW = 10.0f;
            float headerH = m_headerHeight;
            float rowH = m_rowHeight;

            float trackX = b.x + b.width - scrollBarW;
            if (mx >= trackX && my >= b.y + headerH) {
                m_draggingVScroll = true;
                m_dragStartY = my;
                m_dragStartScrollY = m_scrollY;
                return;
            }

            float trackY = b.y + b.height - scrollBarW;
            if (my >= trackY && mx < trackX) {
                m_draggingHScroll = true;
                m_dragStartX = mx;
                m_dragStartScrollX = m_scrollX;
                return;
            }

            if (my >= b.y + headerH && my < b.y + b.height - (hasHScroll(b) ? scrollBarW : 0.0f)) {
                float relativeY = my - (b.y + headerH) + m_scrollY;
                int clickedIndex = static_cast<int>(relativeY / rowH);
                if (clickedIndex >= 0 && clickedIndex < (int)m_rows.size()) {
                    setSelectedIndex(clickedIndex);
                    onRowActivated.emit(clickedIndex);
                }
            }
        }
    }

    void handleMouseRelease(float mx, float my) override {
        m_draggingVScroll = false;
        m_draggingHScroll = false;
        Control::handleMouseRelease(mx, my);
    }

    void handleMouseMove(float mx, float my) override {
        if (m_draggingVScroll) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            bool hasH = hasHScroll(b);
            float scrollBarW = 10.0f;
            float trackH = b.height - m_headerHeight - (hasH ? scrollBarW : 0.0f);
            float totalH = m_rows.size() * m_rowHeight + m_headerHeight + (hasH ? scrollBarW : 0.0f);
            float maxScrollY = std::max(0.0f, totalH - b.height);
            float handleH = std::max(20.0f, (trackH / totalH) * trackH);
            float thumbRange = trackH - handleH;
            if (thumbRange > 0.0f) {
                m_scrollY = std::clamp(m_dragStartScrollY + (my - m_dragStartY) * maxScrollY / thumbRange, 0.0f, maxScrollY);
                m_graph.invalidateNode(m_nodeId, DirtySelf);
            }
            return;
        }
        if (m_draggingHScroll) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            bool hasV = hasVScroll(b);
            float scrollBarW = 10.0f;
            float trackW = b.width - (hasV ? scrollBarW : 0.0f);
            float totalColW = 0.0f;
            for (int i = 0; i < (int)m_headers.size(); ++i)
                totalColW += columnWidth(i, b.width);
            float maxScrollX = std::max(0.0f, totalColW - trackW);
            float handleW = std::max(20.0f, (trackW / totalColW) * trackW);
            float thumbRange = trackW - handleW;
            if (thumbRange > 0.0f) {
                m_scrollX = std::clamp(m_dragStartScrollX + (mx - m_dragStartX) * maxScrollX / thumbRange, 0.0f, maxScrollX);
                m_graph.invalidateNode(m_nodeId, DirtySelf);
            }
            return;
        }
        Control::handleMouseMove(mx, my);
    }

    bool handleScroll(float mx, float my, float wheel) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            float headerH = m_headerHeight;
            float rowH = m_rowHeight;
            float hScrollH = hasHScroll(b) ? 10.0f : 0.0f;
            float totalH = m_rows.size() * rowH + headerH + hScrollH;
            float maxScrollY = std::max(0.0f, totalH - b.height);
            m_scrollY = std::clamp(m_scrollY - wheel * 30.0f, 0.0f, maxScrollY);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        }
        return false;
    }

    bool handleKeyEvent(const KeyEvent& ke) override {
        if (!ke.pressed || m_rows.empty()) return false;
        using K = KeyEvent::Key;

        if (ke.key == K::Down) {
            setSelectedIndex(m_selectedIndex + 1);
            _ensureRowVisible(m_selectedIndex);
            return true;
        } else if (ke.key == K::Up) {
            setSelectedIndex(m_selectedIndex - 1);
            _ensureRowVisible(m_selectedIndex);
            return true;
        } else if (ke.key == K::Return || ke.key == K::Space) {
            if (m_selectedIndex >= 0 && m_selectedIndex < (int)m_rows.size()) {
                onRowActivated.emit(m_selectedIndex);
            }
            return true;
        } else if (ke.key == K::Right) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            float totalColW = 0.0f;
            for (int i = 0; i < (int)m_headers.size(); ++i) {
                totalColW += columnWidth(i, b.width);
            }
            float maxScrollX = std::max(0.0f, totalColW - b.width + (hasVScroll(b) ? 10.0f : 0.0f));
            m_scrollX = std::clamp(m_scrollX + 20.0f, 0.0f, maxScrollX);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        } else if (ke.key == K::Left) {
            m_scrollX = std::clamp(m_scrollX - 20.0f, 0.0f, m_scrollX);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        }
        return false;
    }

    bool hasVScroll(const Rect& b) const {
        float headerH = m_headerHeight;
        float rowH = m_rowHeight;
        float totalH = m_rows.size() * rowH + headerH;
        return totalH > b.height;
    }

    bool hasHScroll(const Rect& b) const {
        float totalColW = 0.0f;
        for (int i = 0; i < (int)m_headers.size(); ++i) {
            totalColW += columnWidth(i, b.width);
        }
        return totalColW > b.width;
    }

    void populateRenderPrimitives(PrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        bool focused = isFocused();

        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface1, 6.0f,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);

        float headerH = m_headerHeight;
        float rowH = m_rowHeight;
        float scrollBarW = 10.0f;

        bool hasV = hasVScroll(b);
        bool hasH = hasHScroll(b);
        float visibleW = b.width - (hasV ? scrollBarW : 0.0f) - 2.0f;
        float visibleH = b.height - (hasH ? scrollBarW : 0.0f) - 2.0f;

        buf.pushRectangle(b.x + 1.0f, b.y + 1.0f, visibleW, headerH - 1.0f, Colors::Surface3, 4.0f);

        buf.pushClip(b.x + 1.0f, b.y + 1.0f, visibleW, visibleH);

        std::vector<float> colStartX;
        float currentX = 0.0f;
        for (int i = 0; i < (int)m_headers.size(); ++i) {
            colStartX.push_back(currentX);
            currentX += columnWidth(i, b.width);
        }
        float totalColW = currentX;

        float maxScrollY = std::max(0.0f, m_rows.size() * rowH + headerH + (hasH ? scrollBarW : 0.0f) - b.height);
        m_scrollY = std::clamp(m_scrollY, 0.0f, maxScrollY);

        float maxScrollX = std::max(0.0f, totalColW - visibleW);
        m_scrollX = std::clamp(m_scrollX, 0.0f, maxScrollX);

        for (int i = 0; i < (int)m_headers.size(); ++i) {
            float colW = columnWidth(i, b.width);
            float startX = b.x + 1.0f + colStartX[i] - m_scrollX;

            drawHeaderCell(buf, i, {startX, b.y + 1.0f, colW, headerH - 1.0f}, m_headers[i]);

            if (i > 0) {
                uint8_t gridC[4] = {72, 72, 76, 255};
                buf.pushRectangle(startX, b.y + 1.0f, 1.0f, headerH - 1.0f, gridC);
            }
        }

        int startIdx = static_cast<int>(m_scrollY / rowH);
        int endIdx = static_cast<int>((m_scrollY + visibleH - headerH) / rowH) + 1;
        startIdx = std::clamp(startIdx, 0, (int)m_rows.size() - 1);
        endIdx = std::clamp(endIdx, 0, (int)m_rows.size() - 1);

        for (int r = startIdx; r <= endIdx; ++r) {
            float rowY = b.y + headerH + r * rowH - m_scrollY;

            if (r == m_selectedIndex) {
                uint8_t selBg[4] = {10, 132, 255, 60};
                buf.pushRectangle(b.x + 1.0f, rowY, visibleW, rowH, selBg);
            } else if (r % 2 == 1) {
                uint8_t altBg[4] = {34, 34, 36, 120};
                buf.pushRectangle(b.x + 1.0f, rowY, visibleW, rowH, altBg);
            }

            for (int c = 0; c < (int)m_headers.size() && c < (int)m_rows[r].size(); ++c) {
                float colW = columnWidth(c, b.width);
                float cellX = b.x + 1.0f + colStartX[c] - m_scrollX;

                drawRowCell(buf, r, c, {cellX, rowY, colW, rowH}, m_rows[r][c], r == m_selectedIndex);

                if (c > 0) {
                    uint8_t gridC[4] = {50, 50, 54, 180};
                    buf.pushRectangle(cellX, rowY, 1.0f, rowH, gridC);
                }
            }

            uint8_t gridH[4] = {50, 50, 54, 180};
            buf.pushRectangle(b.x + 1.0f, rowY + rowH - 1.0f, visibleW, 1.0f, gridH);
        }

        buf.popClip();

        if (hasV) {
            float trackX = b.x + b.width - scrollBarW - 1.0f;
            buf.pushRectangle(trackX, b.y + headerH, scrollBarW, b.height - headerH - (hasH ? scrollBarW : 0.0f), Colors::Surface2, 3.0f);
            
            float totalH = m_rows.size() * rowH + headerH + (hasH ? scrollBarW : 0.0f);
            float trackH = b.height - headerH - (hasH ? scrollBarW : 0.0f);
            float handleH = std::max(20.0f, (trackH / totalH) * trackH);
            float handleY = b.y + headerH + (m_scrollY / maxScrollY) * (trackH - handleH);
            buf.pushRectangle(trackX + 1.0f, handleY, scrollBarW - 2.0f, handleH, Colors::Surface3, 2.0f);
        }

        if (hasH) {
            float trackY = b.y + b.height - scrollBarW - 1.0f;
            buf.pushRectangle(b.x, trackY, b.width - (hasV ? scrollBarW : 0.0f), scrollBarW, Colors::Surface2, 3.0f);

            float trackW = b.width - (hasV ? scrollBarW : 0.0f);
            float handleW = std::max(20.0f, (trackW / totalColW) * trackW);
            float handleX = b.x + (m_scrollX / maxScrollX) * (trackW - handleW);
            buf.pushRectangle(handleX, trackY + 1.0f, handleW, scrollBarW - 2.0f, Colors::Surface3, 2.0f);
        }
    }

    AISemanticNode getSemanticNode() const override {
        std::string val = "";
        if (m_selectedIndex >= 0 && m_selectedIndex < (int)m_rows.size()) {
            for (const auto& cell : m_rows[m_selectedIndex]) {
                if (!val.empty()) val += " | ";
                val += cell;
            }
        }
        return {"DataGrid", "", val, true};
    }

    bool executeSemanticAction(const std::string& a) override {
        if (a.rfind("select_row:", 0) == 0) {
            try {
                int row = std::stoi(a.substr(11));
                setSelectedIndex(row);
                _ensureRowVisible(row);
                return true;
            } catch (...) {}
        }
        return false;
    }

protected:
    virtual void drawHeaderCell(PrimitiveBuffer& buf, int colIdx, const Rect& bounds, const std::string& title) {
        if (TextHelper::hasAtlas()) {
            uint8_t tc[4] = {230, 230, 240, 255};
            float ty = bounds.y + (bounds.height - TextHelper::lineHeight()) * 0.5f;
            TextHelper::pushText(buf, bounds.x + m_cellPadding, ty, tr(title), tc, bounds.width - m_cellPadding * 2.0f);
        }
    }

    virtual void drawRowCell(PrimitiveBuffer& buf, int rowIdx, int colIdx, const Rect& bounds, const std::string& val, bool selected) {
        if (TextHelper::hasAtlas()) {
            uint8_t tc[4] = {200, 200, 210, 220};
            float ty = bounds.y + (bounds.height - TextHelper::lineHeight()) * 0.5f;
            TextHelper::pushText(buf, bounds.x + m_cellPadding, ty, tr(val), tc, bounds.width - m_cellPadding * 2.0f);
        }
    }

private:
    void _ensureRowVisible(int index) {
        if (index < 0 || index >= (int)m_rows.size()) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float headerH = m_headerHeight;
        float rowH = m_rowHeight;
        float hScrollH = hasHScroll(b) ? 10.0f : 0.0f;
        float visibleAreaH = b.height - headerH - hScrollH;

        float rowY = index * rowH;
        if (rowY < m_scrollY) {
            m_scrollY = rowY;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        } else if (rowY + rowH > m_scrollY + visibleAreaH) {
            m_scrollY = rowY + rowH - visibleAreaH;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        }
    }

    std::vector<std::string>              m_headers;
    std::vector<float>                    m_columnWidths;
    std::vector<std::vector<std::string>> m_rows;
    int                                   m_selectedIndex{-1};
    float                                 m_scrollY{0.0f};
    float                                 m_scrollX{0.0f};
    float                                 m_rowHeight{24.0f};
    float                                 m_headerHeight{28.0f};
    float                                 m_cellPadding{8.0f};
    bool                                  m_draggingVScroll{false};
    bool                                  m_draggingHScroll{false};
    float                                 m_dragStartY{0.0f};
    float                                 m_dragStartScrollY{0.0f};
    float                                 m_dragStartX{0.0f};
    float                                 m_dragStartScrollX{0.0f};
};

} // namespace Genesis
