#pragma once

#include <string>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <vector>
#include <functional>
#include "Signal.h"
#include "Variant.h"
#include "SceneGraph.h"
#include "TranslationEngine.h"
#include "KeyEvent.h"
#include "DragDrop.h"           // cross-widget drag payloads (JDragDrop) — drop-on-release routing
#include "Style.h"              // JTabBarEdge / JTabFill (folded into JTheme)
#include "../graphics/RenderPrimitive.h"
#include "../graphics/VectorGraphics.h"   // JVectorCanvas — anti-aliased triangle for the tree expand arrow
#include "../platform/Clipboard.h"
#include "../graphics/FontEngine.h"

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
// JWidget — base for every element in the UI tree
// ============================================================================

class JMenu;

class JWidget : public jf::JSlotTracker {
public:
    inline static std::vector<JWidget*> s_activeWidgets;
    // Live keyboard-modifier state, refreshed by the runner each frame so handleMousePress (which
    // carries no modifier args) can honour Ctrl/Shift — e.g. additive/toggle multi-select.
    inline static bool s_ctrlDown = false;
    inline static bool s_shiftDown = false;

    // Focus-request hook — installed by the framework's JFocusManager. A control claims keyboard
    // focus by calling requestFocus() from its own handleMousePress: because the press is only
    // routed to the control it actually landed on, this is authoritative where the runner's global
    // hit-test (focusAt) can be fooled by an occluded/tabbed-behind widget still flagged visible.
    // Focusing a clicked control is the framework's job — an app never wires this.
    inline static std::function<void(JWidget*)> s_focusHook;
    void requestFocus() { if (s_focusHook) s_focusHook(this); }

    // Screen-origin hook — the framework installs a callback returning the top-left of this UI's
    // host window in global (desktop) coordinates, so mapToGlobal/mapFromGlobal can translate a
    // widget-local screen-space point to/from the desktop. Unset → identity ({0,0}).
    inline static std::function<std::pair<float,float>()> s_screenOrigin;

    // Framework-level text clipboard hook. The platform layer installs s_clipboardSet/Get to bridge the OS
    // clipboard (e.g. JClipboard); when unset (headless / unit tests) both fall back to ONE in-process static
    // string, so copy/cut/paste work with no platform present. Text widgets call clipboardGet()/clipboardSet()
    // — never the OS directly — so a headless test drives the exact same paths as the real app.
    inline static std::function<std::string()>            s_clipboardGet;
    inline static std::function<void(const std::string&)> s_clipboardSet;
    static std::string  clipboardGet()                     { return s_clipboardGet ? s_clipboardGet() : _clipboardFallback(); }
    static void         clipboardSet(const std::string& s) { if (s_clipboardSet) s_clipboardSet(s); else _clipboardFallback() = s; }
    static std::string& _clipboardFallback()               { static std::string s; return s; }

    JWidget(JSceneGraph& graph, const std::string& debugName = "")
        : m_graph(graph), m_state(JWidgetState::Normal), m_debugName(debugName)
    {
        m_nodeId = m_graph.createNode(debugName);
        s_activeWidgets.push_back(this);
    }

    virtual ~JWidget() {
        auto it = std::find(s_activeWidgets.begin(), s_activeWidgets.end(), this);
        if (it != s_activeWidgets.end()) {
            s_activeWidgets.erase(it);
        }
    }
    JWidget(const JWidget&)            = delete;
    JWidget& operator=(const JWidget&) = delete;

    std::string m_tooltipText;
    JMenu*       m_contextMenu{nullptr};
    void setTooltip(const std::string& text) { m_tooltipText = text; }
    const std::string& tooltip() const noexcept { return m_tooltipText; }

    // Context menu — shown on right-click. The pointer is non-owning.
    void  setContextMenu(JMenu* menu) { m_contextMenu = menu; }
    JMenu* contextMenu() const        { return m_contextMenu; }
    // Called by the runner at the click point (widget-local) just before the context menu opens, so a
    // widget can update state the menu depends on — e.g. select the element under the cursor / refresh
    // which items are enabled. Default no-op.
    virtual void prepareContextMenu(float /*mx*/, float /*my*/) {}

    static void renderTooltips(JPrimitiveBuffer& buf, float mouseX, float mouseY);

    NodeId      getNodeId()  const noexcept { return m_nodeId; }
    JWidgetState getState()   const noexcept { return m_state;  }
    bool        isVisible()  const noexcept { return m_visible; }
    bool        isEnabled()  const noexcept { return m_state != JWidgetState::Disabled; }
    bool        isFocused()  const noexcept { return m_focused; }

    struct JBBox { float x, y, width, height; };
    JBBox getBoundingBox() const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        return {b.x, b.y, b.width, b.height};
    }

    // The scene graph this widget lives in. Lets the framework resolve the widget's host window (for popup
    // anchoring) so a control in any window — main or modal dialog — opens its dropdown in the right place.
    JSceneGraph&       sceneGraph()       { return m_graph; }
    const JSceneGraph& sceneGraph() const { return m_graph; }

    void setVisible(bool v) { m_visible = v; }

    // Position/size this widget's layout box directly — for host-driven placement (e.g. a dock
    // laying its content widget into the leaf's content rect). Marks the node dirty so the
    // subtree re-lays-out; container widgets (JScrollArea/JGroupBox/…) arrange children from it.
    void setBounds(const JRect& r) {
        JRect c = r;
        c.width  = _clampW(c.width);
        c.height = _clampH(c.height);
        m_graph.getLayout(m_nodeId).boundingBox = c;
    }
    JRect bounds() const { return m_graph.getLayoutConst(m_nodeId).boundingBox; }

    // ------------------------------------------------------------------
    // Geometry — screen-space float coords, JRect {x,y,width,height}.
    // setGeometry/geometry are Qt-familiar aliases over setBounds/bounds and
    // therefore honour the size constraints below.
    // ------------------------------------------------------------------
    void  setGeometry(const JRect& r) { setBounds(r); }
    JRect geometry() const            { return bounds(); }

    void setPos(float x, float y) {
        auto& bb = m_graph.getLayout(m_nodeId).boundingBox;
        bb.x = x; bb.y = y;
    }
    void setSize(float w, float h) {
        auto& bb = m_graph.getLayout(m_nodeId).boundingBox;
        bb.width  = _clampW(w);
        bb.height = _clampH(h);
    }

    // Alias of hitTest(): true when the screen-space point falls inside this widget's box.
    bool contains(float x, float y) const { return isPointInside(x, y); }

    // Map a widget-local screen-space point to/from global (desktop) coordinates, offsetting
    // by the framework's screen-origin hook (identity when unset).
    std::pair<float,float> mapToGlobal(float x, float y) const {
        auto [ox, oy] = s_screenOrigin ? s_screenOrigin() : std::pair<float,float>{0.f, 0.f};
        return { x + ox, y + oy };
    }
    std::pair<float,float> mapFromGlobal(float x, float y) const {
        auto [ox, oy] = s_screenOrigin ? s_screenOrigin() : std::pair<float,float>{0.f, 0.f};
        return { x - ox, y - oy };
    }

    // ------------------------------------------------------------------
    // Size constraints — clamp every future setSize/setBounds/setGeometry.
    // The minimums also feed the layout engine (mirrored into the layout's
    // minWidth/minHeight); maximums are enforced here on assignment.
    // ------------------------------------------------------------------
    void setMinimumSize(float w, float h) {
        m_minW = w; m_minH = h;
        auto& l = m_graph.getLayout(m_nodeId);
        l.minWidth = w; l.minHeight = h;
        setSize(l.boundingBox.width, l.boundingBox.height);   // re-clamp current box
    }
    void setMaximumSize(float w, float h) {
        m_maxW = w; m_maxH = h;
        const auto& bb = m_graph.getLayoutConst(m_nodeId).boundingBox;
        setSize(bb.width, bb.height);
    }
    std::pair<float,float> minimumSize() const { return { m_minW, m_minH }; }
    std::pair<float,float> maximumSize() const { return { m_maxW, m_maxH }; }

    // Pin the widget to an exact size: min == max == the given size, and clamp now.
    void setFixedSize(float w, float h) {
        m_minW = m_maxW = w;
        m_minH = m_maxH = h;
        auto& l = m_graph.getLayout(m_nodeId);
        l.minWidth = w; l.minHeight = h;
        setSize(w, h);
    }

    // Preferred/natural size — defaults to the current box. Override in a subclass
    // that can compute a content-driven hint (e.g. text extent). sizeHint() is an alias.
    virtual JRect preferredSize() const { return bounds(); }
    JRect sizeHint() const { return preferredSize(); }

    // ------------------------------------------------------------------
    // Focus policy — governs Tab traversal and click-to-focus.
    // ------------------------------------------------------------------
    void         setFocusPolicy(JFocusPolicy p) { m_focusPolicy = p; }
    JFocusPolicy focusPolicy() const noexcept   { return m_focusPolicy; }
    // Tab-reachable — this is what the FocusManager's tab ring honours.
    virtual bool isFocusable() const { return m_focusPolicy & JFocusPolicy::TabFocus; }
    // Takes focus on a mouse press.
    bool acceptsClickFocus() const { return m_focusPolicy & JFocusPolicy::ClickFocus; }

    // ------------------------------------------------------------------
    // Visibility nuance + opacity.
    // ------------------------------------------------------------------
    // setHidden(true) == setVisible(false); provided for Qt API familiarity.
    void setHidden(bool h) { setVisible(!h); }
    bool isHidden() const noexcept { return !m_visible; }

    // Opacity in [0,1]. Stored on the widget; the render layer may honour it when
    // compositing (base widgets paint opaque — this is advisory for renderers/subclasses).
    void  setOpacity(float o) { m_opacity = std::clamp(o, 0.f, 1.f); m_graph.invalidateNode(m_nodeId, DirtySelf); }
    float opacity() const noexcept { return m_opacity; }

    // ------------------------------------------------------------------
    // Z-order within s_activeWidgets (paint order: later = on top).
    // raise() → move to the back of the vector (painted last = topmost).
    // lower() → move to the front (painted first = bottommost).
    // O(n), safe no-op if this widget is not currently registered.
    // ------------------------------------------------------------------
    void raise() {
        auto it = std::find(s_activeWidgets.begin(), s_activeWidgets.end(), this);
        if (it == s_activeWidgets.end()) return;
        s_activeWidgets.erase(it);
        s_activeWidgets.push_back(this);
    }
    void lower() {
        auto it = std::find(s_activeWidgets.begin(), s_activeWidgets.end(), this);
        if (it == s_activeWidgets.end()) return;
        s_activeWidgets.erase(it);
        s_activeWidgets.insert(s_activeWidgets.begin(), this);
    }

    void setEnabled(bool e) {
        setState(e ? JWidgetState::Normal : JWidgetState::Disabled);
    }
    void setFocused(bool f) {
        if (m_focused != f) {
            m_focused = f;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        }
    }

    // Schedule a repaint for this widget.
    void invalidate() { m_graph.invalidateNode(m_nodeId, DirtySelf); }

    virtual void setState(JWidgetState s) {
        if (m_state != s) {
            m_state = s;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        }
    }

    bool hitTest(float mx, float my) const { return isPointInside(mx, my); }

    // Render primitive emission — subclasses paint into the shared buffer
    virtual void populateRenderPrimitives(JPrimitiveBuffer& buf) = 0;

    // Input routing — virtual no-ops so the render loop can call these on any JWidget
    virtual void handleMouseMove(float, float)    {}
    virtual void handleMousePress(float, float)   {}
    virtual void handleMouseRelease(float, float) {}
    virtual bool handleKeyEvent(const JKeyEvent&) { return false; }
    virtual bool handleScroll(float /*mx*/, float /*my*/, float /*wheel*/) { return false; }

    // Typed, named read access to a control's own state — the reference-resolution
    // seam (e.g. "value", "checked", "cursorRow"). Returns a Null JVariant for an
    // unknown key. Keys are the element's NATIVE member names: no translation layer,
    // so a renamed member renames its reference. A Map/List return lets a resolver
    // descend further (uid.axis.sigid, uid.bins[3]) — see ReferenceResolver.h.
    //
    // Common native keys every widget exposes. Typed widgets override to add their
    // own members (and to return a real number for "value"). Geometry mirrors the
    // layout box.
    virtual JVariant getRef(const std::string& key) const {
        if (key == "id")      return static_cast<int64_t>(m_nodeId);
        if (key == "label" || key == "name") return m_debugName;
        if (key == "enabled") return isEnabled();
        if (key == "visible") return isVisible();
        if (key == "focused") return isFocused();
        if (key == "x" || key == "y" || key == "width" || key == "height") {
            const JBBox b = getBoundingBox();
            if (key == "x")     return b.x;
            if (key == "y")     return b.y;
            if (key == "width") return b.width;
            return b.height;
        }
        return JVariant{};
    }

protected:
    bool isPointInside(float mx, float my) const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        return mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height;
    }

    // Clamp a requested width/height into [min,max].
    float _clampW(float w) const { return std::clamp(w, m_minW, m_maxW); }
    float _clampH(float h) const { return std::clamp(h, m_minH, m_maxH); }

    // Call at the end of populateRenderPrimitives to draw a keyboard-focus ring.
    // Defined out-of-line (after JTheme) — see below.
    void drawFocusRing(JPrimitiveBuffer& buf) const;

    JSceneGraph& m_graph;
    NodeId      m_nodeId;
    JWidgetState m_state;
    std::string m_debugName;
    bool        m_visible{true};
    bool        m_focused{false};

    // Core-API state added for Qt/GTK-class completeness.
    JFocusPolicy m_focusPolicy{JFocusPolicy::NoFocus};  // plain JWidget: not focusable
    float        m_opacity{1.0f};                       // [0,1]; advisory to the render layer
    float        m_minW{0.0f},   m_minH{0.0f};          // size constraints (mirror layout minW/minH)
    float        m_maxW{1.0e6f}, m_maxH{1.0e6f};
};

// ============================================================================
// JControl — interactive widget with hover/press/click signals
// ============================================================================

// The scheme's default interior text padding (defined after JTheme, below). Forward-declared here so
// JControl — which precedes JTheme in this header — can fall back to it in textPadding().
inline float _jStyleFieldPadding();

class JControl : public JWidget {
public:
    jf::JSignal<>     onHoverEntered;
    jf::JSignal<>     onHoverExited;
    jf::JSignal<>     onClicked;
    jf::JSignal<bool> onFocusChanged;

    JControl(JSceneGraph& graph, const std::string& name) : JWidget(graph, name) {
        // Interactive controls are click- and tab-focusable by default (Qt StrongFocus).
        m_focusPolicy = JFocusPolicy::StrongFocus;
    }

    void handleMouseMove(float mx, float my) override {
        if (m_state == JWidgetState::Disabled) return;
        bool inside = isPointInside(mx, my);
        if (inside && m_state == JWidgetState::Normal)   { setState(JWidgetState::Hovered); onHoverEntered.emit(); }
        else if (!inside && m_state == JWidgetState::Hovered) { setState(JWidgetState::Normal);  onHoverExited.emit();  }
    }

    void handleMousePress(float mx, float my) override {
        if (m_state == JWidgetState::Disabled) return;
        if (isPointInside(mx, my)) { setState(JWidgetState::Pressed); onClicked.emit(); }
    }

    void handleMouseRelease(float mx, float my) override {
        if (m_state == JWidgetState::Disabled) return;
        if (m_state == JWidgetState::Pressed)
            setState(isPointInside(mx, my) ? JWidgetState::Hovered : JWidgetState::Normal);
    }


    // Interior text padding for input controls (JLineEdit/JSpinBox/JComboBox…). Falls back to the
    // scheme's JTheme::fieldPadding; set per-instance for granular control (a negative value restores
    // the scheme default).
    void  setTextPadding(float p) { m_textPad = p; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    float textPadding() const { return m_textPad >= 0.f ? m_textPad : _jStyleFieldPadding(); }

protected:
    float m_textPad = -1.f;   // -1 = inherit the scheme's fieldPadding
};

// ============================================================================
// JColor helpers
// ============================================================================
// Colors now lives AFTER JTheme (below) — each role is a live pointer into the
// runtime stylesheet (JTheme::current()), so existing Colors:: call sites read the
// active theme with no churn.

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
    return t;
}
inline JTheme& JTheme::current() { static JTheme inst; return inst; }
inline void   JTheme::apply(JTheme t) { current() = std::move(t); }

// The one stylesheet accessor — read by the whole framework each frame.
inline JTheme& style() { return JTheme::current(); }

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
    inline constexpr uint8_t    Transparent[4] = {0, 0, 0, 0};  // truly constant, not themed
}

// ============================================================================
// JAction — shareable command object. JBind to menu items, toolbar buttons,
// and shortcuts. Changing enabled/checked propagates to all bound UI.
// ============================================================================
struct JAction {
    std::string label;
    std::string shortcutText;   // display string e.g. "Ctrl+S" (informational)
    bool enabled  {true};
    bool checkable{false};
    bool checked  {false};

    jf::JSignal<>     onTriggered;
    jf::JSignal<bool> onEnabledChanged;
    jf::JSignal<bool> onCheckedChanged;

    void trigger()          { if (!enabled) return;
                              if (checkable) setChecked(!checked);
                              onTriggered.emit(); }
    void setEnabled(bool e) { enabled = e;  onEnabledChanged.emit(e); }
    void setChecked(bool c) { checked = c;  onCheckedChanged.emit(c); }
};

// ============================================================================
// JTextHelper — global font atlas + text layout for widgets
// ============================================================================

/**
 * Single shared JFontAtlas used by all widgets.
 * Set once at app startup: JTextHelper::setAtlas(fontEngine.buildAtlas(14.f));
 * After that, every populateRenderPrimitives call can emit real text glyphs.
 */
class JTextHelper {
public:
    static void setAtlas(JFontAtlas atlas) { get() = std::move(atlas); }
    static const JFontAtlas& atlas()       { return get(); }
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

    /** Push 6 verts (2 triangles) per glyph into a JTextCall and add to buf. */
    static void pushText(JPrimitiveBuffer& buf,
                         float x, float y,
                         const std::string& text,
                         const uint8_t color[4],
                         float maxWidth = 0.0f)
    {
        const auto& atl = get();
        if (!atl.valid || text.empty()) return;

        JPrimitiveBuffer::JTextCall call;
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
            const JGlyphInfo& g = it->second;

            if (maxWidth > 0.0f && (penX - x + g.advanceX) > maxWidth) break;

            penX += g.advanceX;

            // Whitespace or zero-size glyphs: advance pen only, no geometry
            if (g.pixelW < 0.5f || g.pixelH < 0.5f) continue;

            // Pixel-snap the glyph quad to the display grid. The atlas is packed and sampled 1:1, but a
            // fractional pen position (fractional advances + fractional ascent) makes the LINEAR sampler
            // smear each glyph across two texel columns/rows — the whole run looks soft/blurry. Snapping the
            // quad's top-left to integers makes every glyph land on the pixel grid (crisp), while penX stays
            // fractional above so letter spacing is unchanged.
            float gx = std::floor(penX - g.advanceX + g.bearingX + 0.5f);
            float gy = std::floor(baseline + g.bearingY + 0.5f);
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

    /** Push text scaled by `scale` about the atlas' native size. Advances/bearings/glyph quads all
     *  multiply by scale, so the single shared atlas can render large readouts (e.g. a value gauge)
     *  or fine print. Glyphs are sampled up/down by the GPU. maxWidth is measured in final (scaled) px. */
    static void pushTextScaled(JPrimitiveBuffer& buf,
                               float x, float y,
                               const std::string& text,
                               const uint8_t color[4],
                               float scale,
                               float maxWidth = 0.0f)
    {
        const auto& atl = get();
        if (!atl.valid || text.empty() || scale <= 0.0f) return;
        if (scale == 1.0f) { pushText(buf, x, y, text, color, maxWidth); return; }

        JPrimitiveBuffer::JTextCall call;
        std::copy(color, color + 4, call.color);

        float penX     = x;
        float baseline = y + atl.ascent * scale;

        size_t i = 0;
        while (i < text.size()) {
            uint32_t cp = _decodeUtf8(text, i);
            if (cp == 0) continue;
            auto it = atl.glyphs.find(cp);
            if (it == atl.glyphs.end()) {
                cp = _substitute(cp);
                it = atl.glyphs.find(cp);
                if (it == atl.glyphs.end()) { penX += atl.ascent * 0.35f * scale; continue; }
            }
            const JGlyphInfo& g = it->second;

            if (maxWidth > 0.0f && (penX - x + g.advanceX * scale) > maxWidth) break;
            penX += g.advanceX * scale;
            if (g.pixelW < 0.5f || g.pixelH < 0.5f) continue;

            float gx = penX - g.advanceX * scale + g.bearingX * scale;
            float gy = baseline + g.bearingY * scale;
            float gw = g.pixelW * scale, gh = g.pixelH * scale;

            call.verts.push_back({gx,      gy,      g.u0, g.v0});
            call.verts.push_back({gx + gw, gy,      g.u1, g.v0});
            call.verts.push_back({gx + gw, gy + gh, g.u1, g.v1});
            call.verts.push_back({gx + gw, gy + gh, g.u1, g.v1});
            call.verts.push_back({gx,      gy + gh, g.u0, g.v1});
            call.verts.push_back({gx,      gy,      g.u0, g.v0});
        }
        if (!call.verts.empty()) buf.pushTextCall(std::move(call));
    }

    /** Rendered width of `text` at `scale` (advances scale linearly). */
    static float measureWidthScaled(const std::string& text, float scale) {
        return measureWidth(text) * scale;
    }

    // Text rotated 90° (for vertical tab bars). cw=true reads top->bottom (tilt head right);
    // cw=false reads bottom->top. (x,y) is the rotation pivot; the run advances along the
    // screen Y axis. maxLen caps the run length (in the unrotated text width).
    static void pushTextVertical(JPrimitiveBuffer& buf,
                                 float x, float y,
                                 const std::string& text,
                                 const uint8_t color[4],
                                 float maxLen = 0.0f,
                                 bool  cw = true)
    {
        const auto& atl = get();
        if (!atl.valid || text.empty()) return;

        JPrimitiveBuffer::JTextCall call;
        std::copy(color, color + 4, call.color);

        // Lay out in local (lx along the run, ly across the baseline), then rotate to screen.
        auto rot = [&](float lx, float ly, float& sx, float& sy) {
            if (cw) { sx = x - ly; sy = y + lx; }   // CW 90°
            else    { sx = x + ly; sy = y - lx; }   // CCW 90°
        };

        float pen = 0.f;
        float baseline = atl.ascent;
        size_t i = 0;
        while (i < text.size()) {
            uint32_t cp = _decodeUtf8(text, i);
            if (cp == 0) continue;
            auto it = atl.glyphs.find(cp);
            if (it == atl.glyphs.end()) {
                cp = _substitute(cp);
                it = atl.glyphs.find(cp);
                if (it == atl.glyphs.end()) { pen += atl.ascent * 0.35f; continue; }
            }
            const JGlyphInfo& g = it->second;
            if (maxLen > 0.0f && (pen + g.advanceX) > maxLen) break;
            pen += g.advanceX;
            if (g.pixelW < 0.5f || g.pixelH < 0.5f) continue;

            float lx = pen - g.advanceX + g.bearingX;
            float ly = baseline + g.bearingY;
            float gw = g.pixelW, gh = g.pixelH;

            float ax, ay, bx, by, cx, cy, dx, dy;
            rot(lx,      ly,      ax, ay);
            rot(lx + gw, ly,      bx, by);
            rot(lx + gw, ly + gh, cx, cy);
            rot(lx,      ly + gh, dx, dy);
            call.verts.push_back({ax, ay, g.u0, g.v0});
            call.verts.push_back({bx, by, g.u1, g.v0});
            call.verts.push_back({cx, cy, g.u1, g.v1});
            call.verts.push_back({cx, cy, g.u1, g.v1});
            call.verts.push_back({dx, dy, g.u0, g.v1});
            call.verts.push_back({ax, ay, g.u0, g.v0});
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
    static JFontAtlas& get() { static JFontAtlas s; return s; }
};

inline void JWidget::renderTooltips(JPrimitiveBuffer& buf, float mouseX, float mouseY) {
    static JWidget* lastHovered = nullptr;
    static auto hoverStart = std::chrono::steady_clock::now();

    JWidget* hovered = nullptr;
    for (auto it = s_activeWidgets.rbegin(); it != s_activeWidgets.rend(); ++it) {
        JWidget* w = *it;
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
        float textW = JTextHelper::measureWidth(text);
        float textH = JTextHelper::lineHeight();
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
        JTextHelper::pushText(buf, x + padX, y + padY, text, textColor);
    }
}

inline void JWidget::drawFocusRing(JPrimitiveBuffer& buf) const {
    if (m_state != JWidgetState::Focused) return;
    const auto& bb  = m_graph.getLayoutConst(m_nodeId).boundingBox;
    const auto& th  = JTheme::current();
    float p = th.focusRingWidth * 0.5f + 1.f;
    uint8_t ring[4] = {th.Accent[0], th.Accent[1], th.Accent[2], 210};
    uint8_t none[4] = {0, 0, 0, 0};
    buf.pushRectangle(bb.x - p, bb.y - p, bb.width + p * 2.f, bb.height + p * 2.f,
                      none, th.cornerRadius + 1.f, th.focusRingWidth, ring);
}

// ============================================================================
// JSeparator
// ============================================================================

class JSeparator : public JWidget {
public:
    enum class JOrientation { Horizontal, Vertical };

    JSeparator(JSceneGraph& graph, JOrientation orient = JOrientation::Horizontal,
              float size = 280.0f)
        : JWidget(graph, "JSeparator"), m_orient(orient)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        if (orient == JOrientation::Horizontal) { l.boundingBox.width = size; l.boundingBox.height = 1.0f; }
        else                                   { l.boundingBox.width = 1.0f; l.boundingBox.height = size; }
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Border);
    }


private:
    JOrientation m_orient;
};

// ============================================================================
// JLabel
// ============================================================================

class JLabel : public JWidget {
public:
    JLabel(JSceneGraph& graph, const std::string& text, float w = 240.0f, float h = 0.0f)
        : JWidget(graph, "JLabel: " + text), m_text(text)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().labelHeight;
        l.minWidth = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(m_text) : w;
        l.minHeight = h;
    }

    void setText(const std::string& t) { m_text = t; m_graph.invalidateNode(m_nodeId, DirtySelf); }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (JTextHelper::hasAtlas()) {
            uint8_t c[4] = {200, 200, 210, 200};
            float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            JTextHelper::pushText(buf, b.x, ty, tr(m_text), c, b.width);
        } else {
            // Fallback placeholder bars
            float cy = b.y + b.height * 0.5f - 3.0f;
            uint8_t c[4] = {200, 200, 210, 140};
            buf.pushRectangle(b.x, cy, b.width * 0.55f, 6.0f, c, 2.0f);
        }
    }


private:
    std::string m_text;
};

// ============================================================================
class JButton : public JControl {
public:
    JButton(JSceneGraph& graph, const std::string& label,
           float w = 160.0f, float h = 0.0f)
        : JControl(graph, "JButton"), m_label(label)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().buttonHeight;
        l.minWidth = JTextHelper::hasAtlas() ? (JTextHelper::measureWidth(m_label) + 24.f) : w;
        l.minHeight = h;
    }

    void setLabel(const std::string& label) { m_label = label; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    const std::string& label() const { return m_label; }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const uint8_t* fill = Colors::Surface2;
        if (m_state == JWidgetState::Hovered) fill = Colors::Surface3;
        if (m_state == JWidgetState::Pressed) fill = Colors::Accent;
        bool focused = isFocused();
        drawBackground(buf, b, fill, focused);
        drawLabel(buf, b);
    }


protected:
    virtual void drawBackground(JPrimitiveBuffer& buf, const JRect& b, const uint8_t* fill, bool focused) {
        buf.pushRectangle(b.x, b.y, b.width, b.height, fill, 6.0f,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);
    }
    virtual void drawLabel(JPrimitiveBuffer& buf, const JRect& b) {
        if (JTextHelper::hasAtlas()) {
            std::string txt = tr(m_label);
            const float pad   = 6.f;
            const float avail = b.width - 2.f * pad;                 // interior width for the label
            const float tw    = JTextHelper::measureWidth(txt);
            // Centre when it fits; left-align and clip (maxWidth) when the label is too long, so a long
            // label is truncated inside the button instead of spilling past its edges.
            const float tx = (tw <= avail) ? b.x + (b.width - tw) * 0.5f : b.x + pad;
            const float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            uint8_t tc[4] = {220, 220, 228, 230};
            JTextHelper::pushText(buf, tx, ty, txt, tc, avail);
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
// JToggleButton
// ============================================================================

class JToggleButton : public JControl {
public:
    jf::JSignal<bool> onToggled;

    JToggleButton(JSceneGraph& graph, const std::string& label,
                 float w = 160.0f, float h = 0.0f)
        : JControl(graph, "JToggleButton"), m_label(label)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().buttonHeight;
        l.minWidth = JTextHelper::hasAtlas() ? (JTextHelper::measureWidth(m_label) + 24.f) : w;
        l.minHeight = h;
    }

    void setToggled(bool v) {
        if (m_toggled != v) {
            m_toggled = v;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onToggled.emit(v);
        }
    }
    bool isToggled() const { return m_toggled; }
    void setLabel(const std::string& l) { m_label = l; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    const std::string& label() const { return m_label; }

    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) setState(JWidgetState::Pressed);
    }
    void handleMouseRelease(float mx, float my) override {
        if (m_state == JWidgetState::Pressed && isPointInside(mx, my)) setToggled(!m_toggled);
        JControl::handleMouseRelease(mx, my);
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const uint8_t* fill = m_toggled ? Colors::Accent : Colors::Surface2;
        uint8_t hover[4];
        if (m_state == JWidgetState::Hovered) {
            std::copy(fill, fill+4, hover);
            for (int i=0;i<3;i++) hover[i] = static_cast<uint8_t>(std::min(255, hover[i]+20));
            fill = hover;
        }
        bool focused = isFocused();
        drawBackground(buf, b, fill, focused);
        drawLabel(buf, b);
    }


protected:
    virtual void drawBackground(JPrimitiveBuffer& buf, const JRect& b, const uint8_t* fill, bool focused) {
        buf.pushRectangle(b.x, b.y, b.width, b.height, fill, 6.0f,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);
    }
    virtual void drawLabel(JPrimitiveBuffer& buf, const JRect& b) {
        if (JTextHelper::hasAtlas()) {
            std::string txt = tr(m_label);
            const float pad   = 6.f;
            const float avail = b.width - 2.f * pad;
            const float tw    = JTextHelper::measureWidth(txt);
            const float tx    = (tw <= avail) ? b.x + (b.width - tw) * 0.5f : b.x + pad;   // centre or left-align+clip
            uint8_t tc[4] = {220, 220, 228, 230};
            JTextHelper::pushText(buf, tx, b.y + (b.height-JTextHelper::lineHeight())*0.5f, txt, tc, avail);
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
// JCheckBox
// ============================================================================

class JCheckBox : public JControl {
public:
    jf::JSignal<bool> onStateChanged;

    JCheckBox(JSceneGraph& graph, const std::string& label, float w = 200.0f, float h = 0.0f)
        : JControl(graph, "JCheckBox"), m_label(label)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().checkHeight;
        l.minWidth = JTextHelper::hasAtlas() ? (JTextHelper::measureWidth(m_label) + 28.f) : w;
        l.minHeight = h;
    }

    void setChecked(bool v) {
        if (m_checked != v) {
            m_checked = v;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onStateChanged.emit(v);
        }
    }
    bool isChecked() const { return m_checked; }

    JVariant getRef(const std::string& key) const override {
        if (key == "checked" || key == "value") return m_checked;
        return JWidget::getRef(key);
    }

    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) setChecked(!m_checked);
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float boxSz = b.height;
        const uint8_t* fill = m_checked ? Colors::Accent : Colors::Surface1;
        bool focused = isFocused();
        drawBox(buf, b, boxSz, fill, focused);
        drawLabel(buf, b, boxSz);
    }


protected:
    virtual void drawBox(JPrimitiveBuffer& buf, const JRect& b, float boxSz, const uint8_t* fill, bool focused) {
        buf.pushRectangle(b.x, b.y, boxSz, boxSz, fill, 4.0f,
                          focused ? 2.0f : 1.5f,
                          focused ? Colors::Accent : Colors::Border);
        if (m_checked) {
            uint8_t white[4] = {255, 255, 255, 220};
            buf.pushRectangle(b.x + 3.0f, b.y + boxSz*0.5f - 1.5f, boxSz - 6.0f, 3.0f, white, 1.5f);
            buf.pushRectangle(b.x + boxSz*0.5f - 1.5f, b.y + 3.0f, 3.0f, boxSz - 6.0f, white, 1.5f);
        }
    }
    virtual void drawLabel(JPrimitiveBuffer& buf, const JRect& b, float boxSz) {
        if (JTextHelper::hasAtlas()) {
            uint8_t lc[4] = {200, 200, 210, 200};
            JTextHelper::pushText(buf, b.x + boxSz + 8.0f,
                                 b.y + (b.height - JTextHelper::lineHeight()) * 0.5f,
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
// JRadioButton
// ============================================================================

class JRadioButton : public JControl {
public:
    jf::JSignal<bool> onSelected;

    JRadioButton(JSceneGraph& graph, const std::string& label,
                float w = 200.0f, float h = 0.0f)
        : JControl(graph, "JRadioButton"), m_label(label)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().checkHeight;
        l.minWidth = JTextHelper::hasAtlas() ? (JTextHelper::measureWidth(m_label) + 28.f) : w;
        l.minHeight = h;
    }

    void setSelected(bool v) {
        if (m_selected != v) { m_selected = v; m_graph.invalidateNode(m_nodeId, DirtySelf); onSelected.emit(v); }
    }
    bool isSelected() const { return m_selected; }

    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) setSelected(true);
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float r = b.height;
        const uint8_t* ring = m_selected ? Colors::Accent : Colors::Surface1;
        bool focused = isFocused();
        drawCircle(buf, b, r, ring, focused);
        drawLabel(buf, b, r);
    }


protected:
    virtual void drawCircle(JPrimitiveBuffer& buf, const JRect& b, float r, const uint8_t* ring, bool focused) {
        float borderW = 1.5f;
        buf.pushRectangle(b.x, b.y, r, r, ring, r * 0.5f,
                          focused ? 2.0f : borderW,
                          focused ? Colors::Accent : Colors::Border);
        if (m_selected) {
            float dot = r * 0.42f, offset = (r - dot) * 0.5f;
            buf.pushRectangle(b.x + offset, b.y + offset, dot, dot, Colors::TextPrimary, dot * 0.5f);
        }
    }
    virtual void drawLabel(JPrimitiveBuffer& buf, const JRect& b, float r) {
        if (JTextHelper::hasAtlas()) {
            uint8_t lc[4] = {200, 200, 210, 200};
            JTextHelper::pushText(buf, b.x + r + 8.0f,
                                 b.y + (b.height - JTextHelper::lineHeight()) * 0.5f,
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
// JSlider
// ============================================================================

class JSlider : public JControl {
public:
    jf::JSignal<float> onValueChanged;

    JSlider(JSceneGraph& graph, float w = 280.0f, float h = 0.0f)
        : JControl(graph, "JSlider"), m_value(0.5f)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().sliderHeight;
        l.minWidth = 50.0f;
        l.minHeight = h;
    }

    void setValue(float v) {
        float c = std::clamp(v, 0.0f, 1.0f);
        if (m_value != c) { m_value = c; m_graph.invalidateNode(m_nodeId, DirtySelf); onValueChanged.emit(c); }
    }
    float getValue() const { return m_value; }

    JVariant getRef(const std::string& key) const override {
        if (key == "value") return static_cast<double>(m_value);
        return JWidget::getRef(key);
    }

    void handleMouseMove(float mx, float my) override {
        JControl::handleMouseMove(mx, my);
        if (m_state == JWidgetState::Pressed) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            setValue((mx - b.x) / b.width);
        }
    }
    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) {
            setState(JWidgetState::Pressed);
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            setValue((mx - b.x) / b.width);
        }
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float trackH = 4.0f, trackY = b.y + (b.height - trackH) * 0.5f;
        float fillW  = b.width * m_value;
        drawTrack(buf, b, trackY, trackH, fillW);

        float thumbW = 16.0f, thumbH = b.height;
        float thumbX = b.x + fillW - thumbW * 0.5f;
        // Guard the hi bound: when the slider is narrower than the thumb (e.g. a dock
        // collapsing to ~0 width during a drag), b.x + b.width - thumbW < b.x, which would
        // make std::clamp's lo > hi and abort under libstdc++ hardening.
        thumbX = std::clamp(thumbX, b.x, std::max(b.x, b.x + b.width - thumbW));
        const uint8_t* tc = (m_state == JWidgetState::Pressed) ? Colors::AccentPress : Colors::TextPrimary;
        bool focused = isFocused();
        drawThumb(buf, b, thumbX, thumbW, thumbH, tc, focused);
    }


protected:
    virtual void drawTrack(JPrimitiveBuffer& buf, const JRect& b, float trackY, float trackH, float fillW) {
        buf.pushRectangle(b.x, trackY, b.width, trackH, Colors::Surface3, 2.0f);
        if (fillW > 0.5f)
            buf.pushRectangle(b.x, trackY, fillW, trackH, Colors::Accent, 2.0f);
    }
    virtual void drawThumb(JPrimitiveBuffer& buf, const JRect& b, float thumbX, float thumbW, float thumbH, const uint8_t* tc, bool focused) {
        buf.pushRectangle(thumbX, b.y, thumbW, thumbH, tc, thumbW * 0.5f,
                          focused ? 1.5f : 0.0f,
                          focused ? Colors::Accent : Colors::Border);
    }

private:
    float m_value;
};

// ============================================================================
// JProgressBar
// ============================================================================

class JProgressBar : public JWidget {
public:
    JProgressBar(JSceneGraph& graph, float w = 280.0f, float h = 12.0f)
        : JWidget(graph, "JProgressBar"), m_progress(0.0f)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().menuItemHeight;
        l.minWidth = 50.0f;
        l.minHeight = h;
    }

    void setProgress(float p) { m_progress = std::clamp(p, 0.0f, 1.0f); m_graph.invalidateNode(m_nodeId, DirtySelf); }
    float getProgress() const { return m_progress; }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        drawTrack(buf, b);
        drawProgressFill(buf, b, m_progress);
    }


protected:
    virtual void drawTrack(JPrimitiveBuffer& buf, const JRect& b) {
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface2, 6.0f);
    }
    virtual void drawProgressFill(JPrimitiveBuffer& buf, const JRect& b, float progress) {
        if (progress > 0.005f)
            buf.pushRectangle(b.x, b.y, b.width * progress, b.height, Colors::Success, 6.0f);
    }

private:
    float m_progress;
};

// ============================================================================================
// JScrollBar
// ============================================================================

class JScrollBar : public JControl {
public:
    jf::JSignal<float> onScrolled; // emits 0..1 position

    JScrollBar(JSceneGraph& graph, float w = 280.0f, float h = 14.0f,
              float thumbRatio = 0.3f)
        : JControl(graph, "JScrollBar"), m_thumbRatio(std::clamp(thumbRatio, 0.05f, 1.0f))
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().menuItemHeight;
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
        setState(JWidgetState::Pressed);
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        setScrollPosition((mx - b.x) / b.width - m_thumbRatio * 0.5f);
    }
    void handleMouseMove(float mx, float my) override {
        JControl::handleMouseMove(mx, my);
        if (m_state == JWidgetState::Pressed) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            setScrollPosition((mx - b.x) / b.width - m_thumbRatio * 0.5f);
        }
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface1, 7.0f);
        float tw = b.width * m_thumbRatio;
        float tx = b.x + m_position * (b.width - tw);
        const uint8_t* tc = (m_state == JWidgetState::Pressed) ? Colors::AccentPress
                           : (m_state == JWidgetState::Hovered) ? Colors::Surface3
                                                                : Colors::Border;
        buf.pushRectangle(tx, b.y + 1.0f, tw, b.height - 2.0f, tc, 6.0f);
    }


private:
    float m_position{0.0f};
    float m_thumbRatio;
};

// ============================================================================
// JLineEdit
// ============================================================================

class JLineEdit : public JControl {
public:
    jf::JSignal<std::string> onTextChanged;
    jf::JSignal<>            onReturnPressed;

    JLineEdit(JSceneGraph& graph, const std::string& placeholder = "",
             float w = 280.0f, float h = 0.0f)
        : JControl(graph, "JLineEdit"), m_placeholder(placeholder)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().controlHeight;
        l.minWidth = 60.0f;
        l.minHeight = h;
    }

    void setText(const std::string& t) {
        if (m_text != t) {
            m_text = t;
            m_caret = m_anchor = m_text.size();               // caret to end + collapse selection on set
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onTextChanged.emit(t);
        }
    }
    const std::string& text()        const { return m_text; }
    const std::string& placeholder() const { return m_placeholder; }

    // ---- Selection model -------------------------------------------------------------------------------
    // A selection is the byte range [anchor, caret) (either end may be the larger). anchor==caret ⇒ none.
    bool        hasSelection() const { return m_anchor != m_caret; }
    size_t      selectionStart() const { return std::min(m_anchor, m_caret); }
    size_t      selectionEnd()   const { return std::max(m_anchor, m_caret); }
    std::string selectedText()   const {
        return hasSelection() ? m_text.substr(selectionStart(), selectionEnd() - selectionStart()) : std::string();
    }
    void selectAll() { m_anchor = 0; m_caret = m_text.size(); m_graph.invalidateNode(m_nodeId, DirtySelf); }
    void clearSelection() { m_anchor = m_caret; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    size_t caret() const { return m_caret; }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) return;
        requestFocus();   // clicking the field focuses it, so typed characters route here (framework focus)
        onClicked.emit();
        // Multi-click detection (single → caret + drag-select, double → word, triple → all). Two presses count
        // as a double-click when they land close together in space and within 400 ms — the same test path the
        // platform's real click stream drives.
        const auto now  = std::chrono::steady_clock::now();
        const bool near = std::abs(mx - m_lastClickX) < 4.0f;
        const bool quick = m_clickCount > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastClick).count() < 400;
        m_clickCount = (quick && near) ? m_clickCount + 1 : 1;
        m_lastClick  = now;
        m_lastClickX = mx;

        const size_t idx = _caretFromX(mx);
        if (m_clickCount >= 3) {            // triple-click → select the whole field
            m_anchor = 0; m_caret = m_text.size(); m_selecting = false;
        } else if (m_clickCount == 2) {     // double-click → select the word (or whitespace run) under the cursor
            _selectWordAt(idx); m_selecting = false;
        } else {                            // single-click → place caret, begin a drag-select
            m_caret = idx; m_anchor = idx; m_selecting = true;
        }
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    void handleMouseMove(float mx, float my) override {
        JControl::handleMouseMove(mx, my);
        if (m_selecting) { m_caret = _caretFromX(mx); m_graph.invalidateNode(m_nodeId, DirtySelf); }   // extend, keep anchor
    }
    void handleMouseRelease(float mx, float my) override {
        m_selecting = false;
        JControl::handleMouseRelease(mx, my);
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed) return false;
        using K = JKeyEvent::JKey;
        if (m_caret  > m_text.size()) m_caret  = m_text.size();
        if (m_anchor > m_text.size()) m_anchor = m_text.size();
        auto changed = [&]{
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onTextChanged.emit(m_text);
        };
        auto moved = [&]{ m_graph.invalidateNode(m_nodeId, DirtySelf); };

        // ---- Clipboard + select-all (Ctrl). Match on both the JKey and the folded printable, since a
        //      Ctrl-chord may arrive as a named key or as a control-code utf8 byte depending on platform. ----
        if (ke.ctrl && !ke.alt) {
            const char lc = static_cast<char>(ke.utf8[0] | 0x20);   // case-fold the printable, if any
            if (ke.key == K::A || lc == 'a') { selectAll(); return true; }
            if (ke.key == K::C || lc == 'c') { if (hasSelection()) clipboardSet(selectedText()); return true; }
            if (ke.key == K::X || lc == 'x') {
                if (hasSelection()) { clipboardSet(selectedText()); _deleteSelection(); changed(); }
                return true;
            }
            if (ke.key == K::V || lc == 'v') {
                std::string clip = clipboardGet();
                clip.erase(std::remove(clip.begin(), clip.end(), '\n'), clip.end());   // single-line field: drop newlines
                clip.erase(std::remove(clip.begin(), clip.end(), '\r'), clip.end());
                if (!clip.empty()) {
                    _deleteSelection();                              // paste replaces the active selection
                    m_text.insert(m_caret, clip);
                    m_caret += clip.size();
                    m_anchor = m_caret;
                    changed();
                }
                return true;
            }
        }

        switch (ke.key) {
            case K::Backspace:
                if (hasSelection())      { _deleteSelection(); changed(); }
                else if (ke.ctrl)        { const size_t p = _prevWord(m_caret); if (p < m_caret) { m_text.erase(p, m_caret - p); m_caret = m_anchor = p; changed(); } }
                else if (m_caret > 0)    { const size_t p = _prevCharStart(m_caret); m_text.erase(p, m_caret - p); m_caret = m_anchor = p; changed(); }
                return true;
            case K::Delete:
                if (hasSelection())              { _deleteSelection(); changed(); }
                else if (ke.ctrl)                { const size_t e = _nextWord(m_caret); if (e > m_caret) { m_text.erase(m_caret, e - m_caret); m_anchor = m_caret; changed(); } }
                else if (m_caret < m_text.size()){ m_text.erase(m_caret, _nextCharStart(m_caret) - m_caret); m_anchor = m_caret; changed(); }
                return true;
            case K::Left: {
                size_t pos = ke.ctrl ? _prevWord(m_caret) : _prevCharStart(m_caret);
                if (!ke.shift && !ke.ctrl && hasSelection()) pos = selectionStart();   // plain arrow collapses to edge
                m_caret = pos; if (!ke.shift) m_anchor = pos; moved(); return true;
            }
            case K::Right: {
                size_t pos = ke.ctrl ? _nextWord(m_caret) : _nextCharStart(m_caret);
                if (!ke.shift && !ke.ctrl && hasSelection()) pos = selectionEnd();
                m_caret = pos; if (!ke.shift) m_anchor = pos; moved(); return true;
            }
            case K::Home: m_caret = 0;             if (!ke.shift) m_anchor = m_caret; moved(); return true;
            case K::End:  m_caret = m_text.size(); if (!ke.shift) m_anchor = m_caret; moved(); return true;
            case K::Return: onReturnPressed.emit(); return true;
            default: break;
        }
        if (!ke.ctrl && !ke.alt && ke.utf8[0] != '\0' && static_cast<uint8_t>(ke.utf8[0]) >= 32) {   // printable → replace selection + insert
            const std::string ch = ke.utf8;
            _deleteSelection();
            m_text.insert(m_caret, ch);
            m_caret += ch.size();
            m_anchor = m_caret;
            changed();
            return true;
        }
        return false;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        bool focused = isFocused();

        // Background + border (accent when focused)
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface1, 6.0f,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);

        const float pad = textPadding();
        float innerX = b.x + pad;
        float innerW = b.width - 2.0f * pad;
        float midY   = b.y + (b.height - 7.0f) * 0.5f;

        // Selection highlight — a translucent rectangle behind the selected glyph run (drawn before the text).
        if (hasSelection() && JTextHelper::hasAtlas() && !m_text.empty()) {
            float xLo = innerX + JTextHelper::measureWidth(m_text.substr(0, selectionStart()));
            float xHi = innerX + JTextHelper::measureWidth(m_text.substr(0, selectionEnd()));
            if (xHi > innerX + innerW) xHi = innerX + innerW;
            if (xLo < innerX) xLo = innerX;
            uint8_t sc[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 90};
            buf.pushRectangle(xLo, b.y + 4.0f, std::max(1.0f, xHi - xLo), b.height - 8.0f, sc, 2.0f);
        }

        if (JTextHelper::hasAtlas()) {
            float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            if (m_text.empty()) {
                uint8_t pc[4] = {100, 100, 110, 160};
                JTextHelper::pushText(buf, innerX, ty, m_placeholder, pc, innerW);
            } else {
                uint8_t tc[4] = {220, 220, 228, 220};
                JTextHelper::pushText(buf, innerX, ty, m_text, tc, innerW);
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

        // Caret at the actual insertion point (measured up to the caret index), not a fixed fraction.
        if (focused) {
            const size_t caret = m_caret > m_text.size() ? m_text.size() : m_caret;
            float cx = innerX;
            if (caret > 0)
                cx = innerX + (JTextHelper::hasAtlas() ? JTextHelper::measureWidth(m_text.substr(0, caret))
                                                       : innerW * 0.65f * (float)caret / (float)std::max<size_t>(1, m_text.size()));
            if (cx > innerX + innerW) cx = innerX + innerW;   // clamp inside the field
            buf.pushRectangle(cx, b.y + 6.0f, 1.5f, b.height - 12.0f, Colors::Accent);
        }
    }


private:
    // UTF-8 char-boundary navigation: skip continuation bytes (0b10xxxxxx) so the caret lands on whole chars.
    size_t _nextCharStart(size_t i) const {
        if (i >= m_text.size()) return m_text.size();
        ++i;
        while (i < m_text.size() && (static_cast<uint8_t>(m_text[i]) & 0xC0) == 0x80) ++i;
        return i;
    }
    size_t _prevCharStart(size_t i) const {
        if (i == 0) return 0;
        --i;
        while (i > 0 && (static_cast<uint8_t>(m_text[i]) & 0xC0) == 0x80) --i;
        return i;
    }

    // A "word" character for navigation: ASCII alphanumerics, '_' and any UTF-8 lead/continuation byte
    // (so multibyte glyphs count as word content). Everything else (space, punctuation) is a separator.
    static bool _isWordChar(unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80;
    }
    // Start of the next word: skip the current word run, then the following separators (Qt/GTK semantics).
    size_t _nextWord(size_t i) const {
        const size_t n = m_text.size();
        while (i < n &&  _isWordChar(static_cast<unsigned char>(m_text[i]))) ++i;
        while (i < n && !_isWordChar(static_cast<unsigned char>(m_text[i]))) ++i;
        return i;
    }
    // Start of the current/previous word: skip separators to the left, then the word run.
    size_t _prevWord(size_t i) const {
        while (i > 0 && !_isWordChar(static_cast<unsigned char>(m_text[i - 1]))) --i;
        while (i > 0 &&  _isWordChar(static_cast<unsigned char>(m_text[i - 1]))) --i;
        return i;
    }
    // Select the contiguous run (word OR whitespace/punct) of the same class as the char at/under `i`.
    void _selectWordAt(size_t i) {
        const size_t n = m_text.size();
        if (n == 0) { m_anchor = m_caret = 0; return; }
        if (i >= n) i = n - 1;
        const bool w = _isWordChar(static_cast<unsigned char>(m_text[i]));
        size_t s = i, e = i;
        while (s > 0 && _isWordChar(static_cast<unsigned char>(m_text[s - 1])) == w) --s;
        while (e < n && _isWordChar(static_cast<unsigned char>(m_text[e]))     == w) ++e;
        m_anchor = s; m_caret = e;
    }
    // Nearest character boundary to a screen x — used by click and drag-select.
    size_t _caretFromX(float mx) const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const float innerX = b.x + textPadding();
        if (!JTextHelper::hasAtlas() || m_text.empty()) return m_text.size();
        const float target = mx - innerX;
        if (target <= 0.f) return 0;
        float prevW = 0.f;
        for (size_t i = 0; i < m_text.size(); ) {
            const size_t nxt = _nextCharStart(i);
            const float w = JTextHelper::measureWidth(m_text.substr(0, nxt));
            if (target < (prevW + w) * 0.5f) return i;
            prevW = w; i = nxt;
        }
        return m_text.size();
    }
    void _deleteSelection() {
        if (!hasSelection()) return;
        const size_t lo = selectionStart(), hi = selectionEnd();
        m_text.erase(lo, hi - lo);
        m_caret = m_anchor = lo;
    }

    std::string m_text;
    std::string m_placeholder;
    size_t      m_caret  = 0;   // insertion index into m_text (bytes; on a UTF-8 char boundary)
    size_t      m_anchor = 0;   // selection anchor (== m_caret ⇒ no selection)
    bool        m_selecting = false;                                 // mouse drag-select in progress
    int         m_clickCount = 0;                                    // 1/2/3 = single/double/triple within the window
    std::chrono::steady_clock::time_point m_lastClick{};
    float       m_lastClickX = 0.f;
};

// ============================================================================
// JKeySequenceEdit — a keyboard-shortcut capture box. Shows the current binding
// (e.g. "Shift+Up"); click to
// arm ("Press a key..."), and the next non-modifier keystroke is captured and
// reported via onCaptured with the raw event. The owner turns that event into
// whatever binding representation it uses and calls setText() to show the label
// — the framework owns the capture UX + rendering; the app owns the semantics.
// Sized from the scheme (controlHeight) like every other control.
// ============================================================================

class JKeySequenceEdit : public JControl {
public:
    jf::JSignal<JKeyEvent> onCaptured;   // emitted once when a keystroke is captured (capture auto-disarms)

    JKeySequenceEdit(JSceneGraph& graph, const std::string& text = "",
                     float w = 160.0f, float h = 0.0f)
        : JControl(graph, "JKeySequenceEdit"), m_text(text)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().controlHeight;
        l.minWidth = 60.0f;
        l.minHeight = h;
    }

    void setText(const std::string& t) {
        if (m_text != t) { m_text = t; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    }
    const std::string& text() const { return m_text; }

    bool capturing() const { return m_capturing; }
    void setCapturing(bool c) {
        if (m_capturing != c) { m_capturing = c; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    }

    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) { setCapturing(true); onClicked.emit(); }
        else                        setCapturing(false);
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!m_capturing || !ke.pressed) return false;
        using K = JKeyEvent::JKey;
        // A modifier-only press carries no base key and no text — stay armed and wait for the real key.
        const bool hasKey = (ke.key != K::Unknown) || (ke.utf8[0] != '\0');
        if (!hasKey) return true;
        if (ke.key == K::Escape) { setCapturing(false); return true; }   // cancel, keep the old binding
        m_capturing = false;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        onCaptured.emit(ke);
        return true;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface1, 6.0f,
                          m_capturing ? 1.5f : 1.0f,
                          m_capturing ? Colors::Accent : Colors::Border);
        const float pad = textPadding();
        const float innerX = b.x + pad, innerW = b.width - 2.0f * pad;
        if (JTextHelper::hasAtlas()) {
            const float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            if (m_capturing) {
                uint8_t pc[4] = {150, 190, 255, 210};
                JTextHelper::pushText(buf, innerX, ty, "Press a key...", pc, innerW);
            } else if (m_text.empty()) {
                uint8_t pc[4] = {100, 100, 110, 160};
                JTextHelper::pushText(buf, innerX, ty, "None", pc, innerW);
            } else {
                uint8_t tc[4] = {220, 220, 228, 220};
                JTextHelper::pushText(buf, innerX, ty, m_text, tc, innerW);
            }
        }
    }


private:
    std::string m_text;
    bool        m_capturing{false};
};

// ============================================================================
// JTextArea
// ============================================================================

class JTextArea : public JControl {
public:
    jf::JSignal<std::string> onTextChanged;

    JTextArea(JSceneGraph& graph, const std::string& placeholder = "",
             float w = 340.0f, float h = 120.0f)
        : JControl(graph, "JTextArea"), m_placeholder(placeholder)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().controlHeight;
        l.minWidth = 100.0f;
        l.minHeight = 40.0f;
    }

    void setText(const std::string& t) {
        const std::string v = (m_maxLen && t.size() > m_maxLen) ? t.substr(0, m_maxLen) : t;
        if (m_text != v) {
            m_text = v;
            m_cursorPos = m_text.size();
            m_ensureCaret = true;              // scroll to the caret on the next render (cursor moved)
            m_layoutDirty = true;              // text changed → reflow the cached layout
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onTextChanged.emit(m_text);        // clamped value (not the raw argument)
        }
    }
    const std::string& text()        const { return m_text; }
    const std::string& placeholder() const { return m_placeholder; }

    // Hard cap on the character count (0 = unlimited). Typing / Enter / paste that would exceed it are
    // rejected (a paste is truncated to fit); setText clamps too. Use it to bind an editor to a fixed-size
    // backing field so the user can't author more than the field holds.
    void   setMaxLength(size_t n) {
        m_maxLen = n;
        if (m_maxLen && m_text.size() > m_maxLen) {          // clamp any existing over-length text
            m_text.resize(m_maxLen);
            if (m_cursorPos > m_maxLen) m_cursorPos = m_maxLen;
            m_layoutDirty = true; m_graph.invalidateNode(m_nodeId, DirtySelf);
        }
    }
    size_t maxLength() const { return m_maxLen; }
    bool   _full() const { return m_maxLen && m_text.size() >= m_maxLen; }

    std::string selectedText() const {
        if (!m_selActive || m_selStart == m_selEnd) return {};
        size_t lo = std::min(m_selStart, m_selEnd);
        size_t hi = std::max(m_selStart, m_selEnd);
        return m_text.substr(lo, hi - lo);
    }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) return;
        onClicked.emit();
        // Scrollbar takes precedence over caret placement: grab the thumb, or page the view to a track click.
        if (m_hasScrollBar && mx >= m_sbX && mx <= m_sbX + m_sbW) {
            if (my >= m_sbThumbY && my <= m_sbThumbY + m_sbThumbH) { m_sbDragging = true; m_sbGrabDY = my - m_sbThumbY; }
            else _scrollThumbTo(my - m_sbThumbH * 0.5f);
            return;
        }
        m_ensureCaret = true;                  // a click positions the caret → keep it in view
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float innerX = b.x + 8.0f;
        float innerY = b.y + 8.0f;
        float lh = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() : 12.0f;

        float relY = my - innerY + m_scrollOffset;
        auto lines = getLines();
        size_t clickLine = static_cast<size_t>(std::max(0.0f, relY / lh));
        if (clickLine >= lines.size()) clickLine = lines.empty() ? 0 : lines.size() - 1;

        float relX = mx - innerX;
        size_t clickCol = 0;
        if (JTextHelper::hasAtlas() && clickLine < lines.size()) {
            const std::string& ln = lines[clickLine];
            float cx = 0;
            for (size_t i = 0; i < ln.size(); ++i) {
                float cw = JTextHelper::measureWidth(ln.substr(i, 1));
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

    void handleMouseMove(float mx, float my) override {
        JControl::handleMouseMove(mx, my);
        if (m_sbDragging) _scrollThumbTo(my - m_sbGrabDY);
    }
    void handleMouseRelease(float mx, float my) override { m_sbDragging = false; JControl::handleMouseRelease(mx, my); }

    // Position the view so the scroll thumb's top lands at `thumbTopY` (screen). Used by thumb-drag + track-click.
    void _scrollThumbTo(float thumbTopY) {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const float lh = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() : 12.0f;
        const float innerH = b.height - 16.0f;
        const float maxScroll = std::max(0.0f, static_cast<float>(getLines().size()) * lh - innerH);
        const float range = m_sbTrackH - m_sbThumbH;
        const float t = range > 0.0f ? std::clamp((thumbTopY - m_sbY) / range, 0.0f, 1.0f) : 0.0f;
        m_scrollOffset = t * maxScroll;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    // ---- Visual rows (soft word-wrap) --------------------------------------------------------------
    // Every geometry op (render / cursor / click / scroll) works on VISUAL rows so long lines wrap inside
    // the widget instead of spilling out. A row is a byte range [start, start+len) of m_text with NO '\n';
    // rows come from splitting logical lines (on '\n') AND wrapping any line wider than the text area at a
    // space boundary (falling back to mid-word for a single over-long token).
    struct VRow { size_t start; size_t len; };

    float _wrapWidth() const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        return std::max(40.0f, b.width - 16.0f - 10.0f);   // inner width minus the scrollbar gutter
    }
    // Recompute the wrapped rows + syntax colours — ONLY when the text or width changed (marked by
    // m_layoutDirty). Idle frames and cursor navigation reuse the cache; the render just draws + culls it.
    // Wrapping uses a per-ASCII advance table (no per-char measure / substr), so a reflow is one cheap O(n)
    // pass, not O(n) expensive calls per frame.
    void _ensureLayout() const {
        const float w = _wrapWidth();
        if (!m_layoutDirty && w == m_layoutW) return;
        m_layoutW = w; m_layoutDirty = false;

        const bool atlas = JTextHelper::hasAtlas();
        float adv[128];
        if (atlas) { const auto& atl = JTextHelper::atlas();
            for (int i = 0; i < 128; ++i) { auto it = atl.glyphs.find(static_cast<uint32_t>(i));
                adv[i] = (it != atl.glyphs.end()) ? it->second.advanceX : atl.ascent * 0.35f; } }
        auto cw = [&](unsigned char c) -> float { return atlas ? (c < 128 ? adv[c] : 8.0f) : 6.0f; };

        m_rows.clear();
        size_t lineStart = 0;
        for (size_t i = 0; i <= m_text.size(); ++i) {
            if (i != m_text.size() && m_text[i] != '\n') continue;
            if (lineStart >= i) { m_rows.push_back({lineStart, 0}); }
            else {
                size_t rowStart = lineStart;
                while (rowStart < i) {                                     // wrap the logical line [lineStart, i)
                    float acc = 0.f; size_t j = rowStart, lastSpace = std::string::npos, brk = i;
                    while (j < i) {
                        const float a = cw(static_cast<unsigned char>(m_text[j]));
                        if (acc + a > w && j > rowStart) {
                            brk = (lastSpace != std::string::npos && lastSpace + 1 > rowStart) ? lastSpace + 1 : j;
                            break;
                        }
                        if (m_text[j] == ' ' || m_text[j] == '\t') lastSpace = j;
                        acc += a; ++j; brk = j;
                    }
                    m_rows.push_back({rowStart, brk - rowStart});
                    rowStart = brk;
                }
            }
            lineStart = i + 1;
            if (i == m_text.size()) break;
        }
        if (m_rows.empty()) m_rows.push_back({0, 0});

        m_hcols.clear();
        if (m_highlighter && !m_text.empty()) m_highlighter(m_text, m_hcols);   // syntax colours: once per change
    }
    const std::vector<VRow>& visualRows() const { _ensureLayout(); return m_rows; }
    std::string _rowText(const VRow& r) const { return m_text.substr(r.start, r.len); }

    std::vector<std::string> getLines() const {
        std::vector<std::string> lines;
        for (const VRow& r : visualRows()) lines.push_back(_rowText(r));
        return lines;
    }

    void getCursorLineCol(size_t& outLine, size_t& outCol) const {
        const auto& rows = visualRows();
        outLine = 0; outCol = 0;
        for (size_t r = 0; r < rows.size(); ++r) {
            const size_t rowEnd = rows[r].start + rows[r].len;
            const bool last = (r + 1 == rows.size());
            if (m_cursorPos <= rowEnd || last) {                           // caret sits on this visual row
                if (!last && m_cursorPos == rowEnd + 1) continue;          // exactly on the '\n' → next row
                outLine = r;
                outCol  = m_cursorPos >= rows[r].start ? m_cursorPos - rows[r].start : 0;
                if (outCol > rows[r].len) outCol = rows[r].len;
                return;
            }
        }
    }

    size_t getPosFromLineCol(size_t line, size_t col) const {
        const auto& rows = visualRows();
        if (rows.empty()) return 0;
        if (line >= rows.size()) line = rows.size() - 1;
        if (col > rows[line].len) col = rows[line].len;
        return rows[line].start + col;
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed) return false;
        using K = JKeyEvent::JKey;
        m_ensureCaret = true;                  // typing / navigating moves the caret → keep it in view

        size_t line = 0, col = 0;
        getCursorLineCol(line, col);

        // ---- Ctrl shortcuts ----
        if (ke.ctrl) {
            if (ke.key == K::C || (ke.utf8[0]=='c'||ke.utf8[0]=='C')) {
                std::string sel = selectedText();
                if (!sel.empty()) clipboardSet(sel);
                return true;
            }
            if (ke.key == K::X || (ke.utf8[0]=='x'||ke.utf8[0]=='X')) {
                std::string sel = selectedText();
                if (!sel.empty()) {
                    clipboardSet(sel);
                    _deleteSelection();
                }
                return true;
            }
            if (ke.key == K::V || (ke.utf8[0]=='v'||ke.utf8[0]=='V')) {
                std::string clip = clipboardGet();
                if (!clip.empty()) {
                    _deleteSelection();
                    if (m_maxLen && m_text.size() + clip.size() > m_maxLen)      // truncate the paste to fit the cap
                        clip.resize(m_maxLen > m_text.size() ? m_maxLen - m_text.size() : 0);
                    if (!clip.empty()) {
                    m_text.insert(m_cursorPos, clip);
                    m_cursorPos += clip.size();
                    m_layoutDirty = true;
                    m_graph.invalidateNode(m_nodeId, DirtySelf);
                    onTextChanged.emit(m_text);
                    }
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
            } else if (ke.ctrl && m_cursorPos > 0) {           // Ctrl+Backspace → delete the word to the left
                const size_t p = _prevWord(m_cursorPos);
                if (p < m_cursorPos) {
                    m_text.erase(p, m_cursorPos - p);
                    m_cursorPos = p;
                    m_layoutDirty = true;
                    m_graph.invalidateNode(m_nodeId, DirtySelf);
                    onTextChanged.emit(m_text);
                }
            } else if (m_cursorPos > 0 && !m_text.empty()) {
                m_text.erase(m_cursorPos - 1, 1);
                m_cursorPos--;
                m_layoutDirty = true;
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                onTextChanged.emit(m_text);
            }
            return true;
        } else if (ke.key == K::Delete) {
            if (m_selActive && m_selStart != m_selEnd) {
                _deleteSelection();
            } else if (ke.ctrl && m_cursorPos < m_text.size()) {   // Ctrl+Delete → delete the word to the right
                const size_t e = _nextWord(m_cursorPos);
                if (e > m_cursorPos) {
                    m_text.erase(m_cursorPos, e - m_cursorPos);
                    m_layoutDirty = true;
                    m_graph.invalidateNode(m_nodeId, DirtySelf);
                    onTextChanged.emit(m_text);
                }
            } else if (m_cursorPos < m_text.size()) {
                m_text.erase(m_cursorPos, 1);
                m_layoutDirty = true;
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                onTextChanged.emit(m_text);
            }
            return true;
        } else if (ke.key == K::Return) {
            _deleteSelection();
            if (_full()) return true;                          // at the cap → reject
            m_text.insert(m_cursorPos, "\n");
            m_cursorPos++;
            m_layoutDirty = true;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onTextChanged.emit(m_text);
            return true;
        } else if (ke.key == K::Left) {
            if (ke.ctrl) {                                     // Ctrl(+Shift)+Left → move/select by word
                moveCursor(_prevWord(m_cursorPos));
            } else if (!ke.shift && m_selActive && m_selStart != m_selEnd) {
                m_cursorPos = std::min(m_selStart, m_selEnd);
                m_selActive = false;
                m_graph.invalidateNode(m_nodeId, DirtySelf);
            } else {
                moveCursor(m_cursorPos > 0 ? m_cursorPos - 1 : 0);
            }
            return true;
        } else if (ke.key == K::Right) {
            if (ke.ctrl) {                                     // Ctrl(+Shift)+Right → move/select by word
                moveCursor(_nextWord(m_cursorPos));
            } else if (!ke.shift && m_selActive && m_selStart != m_selEnd) {
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
                if (m_maxLen && m_text.size() + std::strlen(ke.utf8) > m_maxLen) return true;   // at the cap → reject
                m_text.insert(m_cursorPos, ke.utf8);
                m_cursorPos += std::strlen(ke.utf8);
                m_layoutDirty = true;
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                onTextChanged.emit(m_text);
                return true;
            }
        }
        return false;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
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

        float lh = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() : 12.0f;

        size_t cursorLine = 0, cursorCol = 0;
        getCursorLineCol(cursorLine, cursorCol);

        const auto& rows = visualRows();

        // Scroll to keep the caret visible — ONLY when the caret just moved (typing / arrows / click), so the
        // mouse wheel can freely scroll elsewhere without the view snapping back to the caret every frame.
        if (m_ensureCaret) {
            float cursorYRel = cursorLine * lh;
            if (cursorYRel < m_scrollOffset)                 m_scrollOffset = cursorYRel;
            else if (cursorYRel + lh > m_scrollOffset + innerH) m_scrollOffset = cursorYRel + lh - innerH;
            m_ensureCaret = false;
        }
        // Always clamp to the content extent (handles text shrinking / resize).
        const float maxScroll = std::max(0.0f, static_cast<float>(rows.size()) * lh - innerH);
        m_scrollOffset = std::clamp(m_scrollOffset, 0.0f, maxScroll);

        buf.pushClip(innerX, innerY, innerW, innerH);   // wrapped rows fit, but clip so nothing ever spills out

        auto rowX = [&](const std::string& t, size_t nchars) -> float {
            return innerX + (JTextHelper::hasAtlas() ? JTextHelper::measureWidth(t.substr(0, nchars))
                                                     : static_cast<float>(nchars) * 6.0f);
        };

        // Selection highlight — per visual row, the intersection of the row's byte range with [selLo, selHi).
        if (m_selActive && m_selStart != m_selEnd) {
            const size_t selLo = std::min(m_selStart, m_selEnd), selHi = std::max(m_selStart, m_selEnd);
            const uint8_t selColor[4] = {65, 105, 225, 100};
            for (size_t r = 0; r < rows.size(); ++r) {
                const float lineY = innerY + r * lh - m_scrollOffset;
                if (lineY + lh < innerY || lineY > innerY + innerH) continue;
                const size_t rs = rows[r].start, re = rs + rows[r].len;
                const size_t a = std::max(selLo, rs), bb = std::min(selHi, re);
                if (bb <= a) continue;
                const std::string rowT = _rowText(rows[r]);
                const float sx = rowX(rowT, a - rs), ex = rowX(rowT, bb - rs);
                if (ex > sx) buf.pushRectangle(sx, lineY, ex - sx, lh, selColor);
            }
        }

        if (JTextHelper::hasAtlas()) {
            const std::vector<uint8_t>& cols = m_hcols;   // syntax colours from the cached layout (not per frame)
            const uint8_t tc[4] = {220, 220, 228, 220};
            if (m_text.empty() && !m_placeholder.empty()) {
                uint8_t pc[4] = {100, 100, 110, 160};
                JTextHelper::pushText(buf, innerX, innerY, m_placeholder, pc, innerW);
            } else {
                for (size_t i = 0; i < rows.size(); ++i) {
                    const float lineY = innerY + i * lh - m_scrollOffset;
                    if (lineY + lh < innerY || lineY > innerY + innerH) continue;
                    const std::string ln = _rowText(rows[i]);
                    if (cols.empty()) { JTextHelper::pushText(buf, innerX, lineY, ln, tc, innerW); continue; }
                    const size_t off = rows[i].start;                    // ABSOLUTE byte offset of this row's start
                    auto colAt = [&](size_t c, uint8_t out[4]) {
                        const size_t ci = (off + c) * 4;
                        if (ci + 3 < cols.size()) { out[0]=cols[ci]; out[1]=cols[ci+1]; out[2]=cols[ci+2]; out[3]=cols[ci+3]; }
                        else { out[0]=tc[0]; out[1]=tc[1]; out[2]=tc[2]; out[3]=tc[3]; }
                    };
                    float x = innerX;
                    for (size_t j = 0; j < ln.size(); ) {
                        uint8_t rc[4]; colAt(j, rc);
                        size_t k = j + 1;
                        for (; k < ln.size(); ++k) { uint8_t kc[4]; colAt(k, kc); if (kc[0]!=rc[0]||kc[1]!=rc[1]||kc[2]!=rc[2]||kc[3]!=rc[3]) break; }
                        const std::string run = ln.substr(j, k - j);
                        JTextHelper::pushText(buf, x, lineY, run, rc, innerW);
                        x += JTextHelper::measureWidth(run);
                        j = k;
                    }
                }
            }
        } else {
            if (m_text.empty()) {
                uint8_t pc[4] = {100, 100, 110, 120};
                buf.pushRectangle(innerX, innerY + (lh - 7.0f) * 0.5f, innerW * 0.55f, 7.0f, pc, 2.0f);
            } else {
                uint8_t tc[4] = {220, 220, 228, 200};
                for (size_t i = 0; i < rows.size(); ++i) {
                    float lineY = innerY + i * lh - m_scrollOffset;
                    if (lineY + lh < innerY || lineY > innerY + innerH) continue;
                    float lw = std::min(innerW, 20.0f + static_cast<float>(rows[i].len * 6));
                    buf.pushRectangle(innerX, lineY + (lh - 7.0f) * 0.5f, lw, 7.0f, tc, 2.0f);
                }
            }
        }

        // Caret at the cursor's visual row/column.
        if (focused) {
            float cx = innerX;
            if (!m_text.empty() && cursorLine < rows.size())
                cx = rowX(_rowText(rows[cursorLine]), std::min(cursorCol, rows[cursorLine].len));
            float cy = innerY + cursorLine * lh - m_scrollOffset;
            if (cy + lh >= innerY && cy <= innerY + innerH)
                buf.pushRectangle(cx, cy + 2.0f, 1.5f, lh - 4.0f, Colors::Accent);
        }

        buf.popClip();

        // Vertical scrollbar — shown only when the content overflows. Track down the right inner edge, thumb
        // sized/positioned by the visible fraction. Drag it with dragScrollThumb via handleMousePress/Move.
        const float contentH = static_cast<float>(rows.size()) * lh;
        m_hasScrollBar = contentH > innerH + 1.0f;
        if (m_hasScrollBar) {
            const float sbW = 8.0f;
            m_sbX = b.x + b.width - sbW - 3.0f; m_sbY = b.y + 4.0f; m_sbW = sbW; m_sbTrackH = b.height - 8.0f;
            buf.pushRectangle(m_sbX, m_sbY, sbW, m_sbTrackH, Colors::Surface0, sbW * 0.5f);
            const float maxScroll = contentH - innerH;
            m_sbThumbH = std::max(24.0f, m_sbTrackH * (innerH / contentH));
            const float frac = maxScroll > 0.0f ? (m_scrollOffset / maxScroll) : 0.0f;
            m_sbThumbY = m_sbY + frac * (m_sbTrackH - m_sbThumbH);
            buf.pushRectangle(m_sbX, m_sbThumbY, sbW, m_sbThumbH, Colors::Surface3, sbW * 0.5f);
        }
    }

    // Mouse wheel scrolls the view (independently of the caret).
    bool handleScroll(float mx, float my, float wheel) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx < b.x || mx > b.x + b.width || my < b.y || my > b.y + b.height) return false;
        const float lh = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() : 12.0f;
        const float innerH = b.height - 16.0f;
        const float maxScroll = std::max(0.0f, static_cast<float>(getLines().size()) * lh - innerH);
        if (maxScroll <= 0.0f) return false;
        m_scrollOffset = std::clamp(m_scrollOffset - wheel * lh * 3.0f, 0.0f, maxScroll);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        return true;
    }


    // Optional syntax highlighter: fills `out` with 4 bytes (RGBA) per character of the text; the render then
    // draws each line as runs of equal colour. Null (default) → the whole text draws in one colour (no change
    // for any existing JTextArea). Used by the studio's Lua editor.
    void setHighlighter(std::function<void(const std::string&, std::vector<uint8_t>&)> h) { m_highlighter = std::move(h); m_graph.invalidateNode(m_nodeId, DirtySelf); }

private:
    std::function<void(const std::string&, std::vector<uint8_t>&)> m_highlighter;   // null = plain single-colour text

    // Word-boundary navigation (Ctrl+Left/Right, Ctrl+Backspace/Delete). A word char is ASCII alnum, '_' or
    // any UTF-8 byte ≥0x80; everything else is a separator. Semantics match JLineEdit / Qt / GTK.
    static bool _isWordChar(unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80;
    }
    size_t _nextWord(size_t i) const {
        const size_t n = m_text.size();
        while (i < n &&  _isWordChar(static_cast<unsigned char>(m_text[i]))) ++i;
        while (i < n && !_isWordChar(static_cast<unsigned char>(m_text[i]))) ++i;
        return i;
    }
    size_t _prevWord(size_t i) const {
        while (i > 0 && !_isWordChar(static_cast<unsigned char>(m_text[i - 1]))) --i;
        while (i > 0 &&  _isWordChar(static_cast<unsigned char>(m_text[i - 1]))) --i;
        return i;
    }

    void _deleteSelection() {
        if (!m_selActive || m_selStart == m_selEnd) return;
        size_t lo = std::min(m_selStart, m_selEnd);
        size_t hi = std::max(m_selStart, m_selEnd);
        m_text.erase(lo, hi - lo);
        m_cursorPos = lo;
        m_selActive = false;
        m_layoutDirty = true;
        onTextChanged.emit(m_text);
    }

    std::string m_text;
    std::string m_placeholder;
    size_t      m_cursorPos{0};
    float       m_scrollOffset{0.0f};
    size_t      m_selStart{0};
    size_t      m_selEnd{0};
    bool        m_selActive{false};
    bool        m_ensureCaret{true};   // scroll to the caret next render (set on caret-moving actions)
    // Vertical scrollbar geometry, recomputed each render; used for wheel + thumb-drag hit-testing.
    bool        m_hasScrollBar{false};
    float       m_sbX{0}, m_sbY{0}, m_sbW{0}, m_sbTrackH{0}, m_sbThumbY{0}, m_sbThumbH{0};
    bool        m_sbDragging{false};
    float       m_sbGrabDY{0};
    // Cached line layout (wrapped rows + syntax colours) — see _ensureLayout(). Recomputed only on a text or
    // width change (m_layoutDirtY); the render/cursor/click just read it. `mutable` so const geometry ops
    // (called from the const render) can lazily refresh it.
    mutable std::vector<VRow>   m_rows;
    mutable std::vector<uint8_t> m_hcols;         // per-char RGBA syntax colours (empty = no highlighter)
    mutable bool                m_layoutDirty{true};
    mutable float               m_layoutW{-1.0f};
    size_t                      m_maxLen{0};          // hard character cap (0 = unlimited)
};

// ============================================================================
// JSpinBox
// ============================================================================

class JSpinBox : public JControl {
public:
    jf::JSignal<int> onValueChanged;

    JSpinBox(JSceneGraph& graph, int minVal = 0, int maxVal = 100,
            float w = 140.0f, float h = 0.0f)
        : JControl(graph, "JSpinBox"), m_min(minVal), m_max(maxVal), m_value(minVal)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().controlHeight;
        l.minWidth = 60.0f;
        l.minHeight = h;
    }

    void setValue(int v) {
        int c = std::clamp(v, m_min, m_max);
        if (m_value != c) { m_value = c; m_graph.invalidateNode(m_nodeId, DirtySelf); onValueChanged.emit(c); }
    }
    int  value() const { return m_value; }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) { _commitEdit(); return; }
        requestFocus();   // clicking the box focuses it, so typed digits route here (framework focus)
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float btnW = b.height * 0.7f;
        if (mx >= b.x + b.width - btnW) {
            _commitEdit();
            setValue(m_value + (my < b.y + b.height * 0.5f ? 1 : -1));
        } else {
            setState(JWidgetState::Pressed);
            _beginEdit();
        }
    }

    bool handleScroll(float mx, float my, float wheel) override {
        if (!isPointInside(mx, my)) return false;
        _commitEdit();
        setValue(m_value + (wheel > 0.0f ? 1 : -1));
        return true;
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed) return false;
        using K = JKeyEvent::JKey;
        if (ke.key == K::Up)   { _commitEdit(); setValue(m_value + 1); return true; }
        if (ke.key == K::Down) { _commitEdit(); setValue(m_value - 1); return true; }
        if (ke.key == K::Return) { _commitEdit(); return true; }
        if (ke.key == K::Escape) { if (m_editing) { m_editing = false; m_selectAll = false; invalidate(); return true; } return false; }
        if (ke.key == K::Backspace) {
            if (!m_editing) return false;
            if (m_selectAll) { m_editBuf.clear(); m_selectAll = false; }   // delete the selection
            else if (!m_editBuf.empty()) m_editBuf.pop_back();
            invalidate();
            return true;
        }
        const char c = ke.utf8[0];
        if (c != '\0') {
            if (m_editing && m_selectAll) {                 // click-then-type replaces the whole value
                const std::string prev = m_editBuf; m_editBuf.clear();
                if (_acceptChar(c)) { m_selectAll = false; m_editBuf += c; invalidate(); return true; }
                m_editBuf = prev; return true;              // reject but consume (selection stays)
            }
            if (_acceptChar(c)) {
                if (!m_editing) { m_editing = true; m_editBuf.clear(); }
                m_editBuf += c;
                invalidate();
                return true;
            }
        }
        return false;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (m_editing && !isFocused()) _commitEdit();
        float btnW = b.height * 0.7f;
        float fieldW = b.width - btnW;

        bool focused = isFocused();
        // Value field
        buf.pushRectangle(b.x, b.y, fieldW, b.height, Colors::Surface1, 6.0f,
                          (focused || m_editing) ? 1.5f : 1.0f,
                          (focused || m_editing) ? Colors::Accent : Colors::Border);
        // Value text
        const std::string txt = m_editing ? m_editBuf : std::to_string(m_value);
        if (JTextHelper::hasAtlas()) {
            uint8_t vc[4] = {210, 210, 220, 220};
            float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            if (m_editing && m_selectAll && !txt.empty()) {   // selection highlight behind the value
                uint8_t sel[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 90};
                buf.pushRectangle(b.x + textPadding() - 1.0f, b.y + 4.0f,
                                  std::min(fieldW - textPadding(), JTextHelper::measureWidth(txt) + 2.0f), b.height - 8.0f, sel, 2.0f);
            }
            JTextHelper::pushText(buf, b.x + textPadding(), ty, txt, vc, fieldW - textPadding());
            if (m_editing && !m_selectAll) {
                float cx = b.x + textPadding() + JTextHelper::measureWidth(txt) + 1.0f;
                buf.pushRectangle(cx, b.y + 6.0f, 1.5f, b.height - 12.0f, Colors::Accent);
            }
        } else {
            uint8_t vc[4] = {200, 200, 210, 180};
            buf.pushRectangle(b.x + textPadding(), b.y + (b.height-7.0f)*0.5f, fieldW * 0.6f, 7.0f, vc, 2.0f);
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


private:
    // Click-to-edit pre-fills the current value AND selects it, so the first typed digit replaces it
    // (standard entry-field behaviour); backspace/edit keys drop into in-place editing instead.
    void _beginEdit() { m_editBuf = std::to_string(m_value); m_editing = true; m_selectAll = true; invalidate(); }
    void _commitEdit() {
        if (!m_editing) return;
        m_editing = false; m_selectAll = false;
        try { setValue(std::stoi(m_editBuf)); } catch (...) {}
        invalidate();
    }
    bool _acceptChar(char c) const {
        if (c >= '0' && c <= '9') return true;
        if (c == '-') return m_min < 0 && m_editBuf.find('-') == std::string::npos;
        return false;
    }

    int m_min, m_max, m_value;
    bool m_editing{false};
    bool m_selectAll{false};
    std::string m_editBuf;
};

// ============================================================================
// JDoubleSpinBox — floating-point spin box.
//
// Mirrors JSpinBox exactly, but stores a `double` with configurable step,
// decimal places and an optional textual suffix.  Full precision is kept
// internally; only the rendered text is rounded to `decimals`.
// ============================================================================

class JDoubleSpinBox : public JControl {
public:
    jf::JSignal<double> onValueChanged;

    JDoubleSpinBox(JSceneGraph& graph, double min, double max,
                   double step = 1.0, int decimals = 2,
                   float w = 120.0f, float h = 0.0f)
        : JControl(graph, "JDoubleSpinBox"),
          m_value(min), m_min(min), m_max(max), m_step(step), m_decimals(decimals)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().controlHeight;
        l.minWidth = 60.0f;
        l.minHeight = h;
    }

    void setValue(double v) {
        double c = std::clamp(v, m_min, m_max);
        if (m_value != c) { m_value = c; m_graph.invalidateNode(m_nodeId, DirtySelf); onValueChanged.emit(c); }
    }
    double value() const { return m_value; }

    void setRange(double min, double max) { m_min = min; m_max = max; setValue(m_value); }
    void setStep(double step)             { m_step = step; }
    void setDecimals(int decimals)        { m_decimals = decimals; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    void setSuffix(const std::string& s)  { m_suffix = s; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    const std::string& suffix() const     { return m_suffix; }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) { _commitEdit(); return; }
        requestFocus();   // clicking the box focuses it, so typed digits route here (framework focus)
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float btnW = b.height * 0.7f;
        if (mx >= b.x + b.width - btnW) {   // an arrow — commit any edit, then step
            _commitEdit();
            setValue(m_value + (my < b.y + b.height * 0.5f ? m_step : -m_step));
        } else {                            // the value field — begin direct text entry
            setState(JWidgetState::Pressed);
            _beginEdit();
        }
    }

    bool handleScroll(float mx, float my, float wheel) override {
        if (!isPointInside(mx, my)) return false;
        _commitEdit();
        setValue(m_value + (wheel > 0.0f ? m_step : -m_step));
        return true;
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed) return false;
        using K = JKeyEvent::JKey;
        if (ke.key == K::Up)   { _commitEdit(); setValue(m_value + m_step); return true; }
        if (ke.key == K::Down) { _commitEdit(); setValue(m_value - m_step); return true; }
        if (ke.key == K::Return) { _commitEdit(); return true; }
        if (ke.key == K::Escape) { if (m_editing) { m_editing = false; m_selectAll = false; invalidate(); return true; } return false; }
        if (ke.key == K::Backspace) {
            if (!m_editing) return false;
            if (m_selectAll) { m_editBuf.clear(); m_selectAll = false; }   // delete the selection
            else if (!m_editBuf.empty()) m_editBuf.pop_back();
            invalidate();
            return true;
        }
        // Printable characters: accept only what can form a number.
        const char c = ke.utf8[0];
        if (c != '\0') {
            if (m_editing && m_selectAll) {                 // click-then-type replaces the whole value
                const std::string prev = m_editBuf; m_editBuf.clear();
                if (_acceptChar(c)) { m_selectAll = false; m_editBuf += c; invalidate(); return true; }
                m_editBuf = prev; return true;              // reject but consume (selection stays)
            }
            if (_acceptChar(c)) {
                if (!m_editing) { m_editing = true; m_editBuf.clear(); }
                m_editBuf += c;
                invalidate();
                return true;
            }
        }
        return false;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        // Clicking away drops keyboard focus — commit the pending edit so it isn't lost.
        if (m_editing && !isFocused()) _commitEdit();
        float btnW = b.height * 0.7f;
        float fieldW = b.width - btnW;

        bool focused = isFocused();
        // Value field
        buf.pushRectangle(b.x, b.y, fieldW, b.height, Colors::Surface1, 6.0f,
                          (focused || m_editing) ? 1.5f : 1.0f,
                          (focused || m_editing) ? Colors::Accent : Colors::Border);
        // Value text (the live edit buffer while typing, otherwise the formatted value)
        const std::string txt = m_editing ? m_editBuf : _formatValue();
        if (JTextHelper::hasAtlas()) {
            uint8_t vc[4] = {210, 210, 220, 220};
            float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            if (m_editing && m_selectAll && !txt.empty()) {   // selection highlight behind the value
                uint8_t sel[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 90};
                buf.pushRectangle(b.x + textPadding() - 1.0f, b.y + 4.0f,
                                  std::min(fieldW - textPadding(), JTextHelper::measureWidth(txt) + 2.0f), b.height - 8.0f, sel, 2.0f);
            }
            JTextHelper::pushText(buf, b.x + textPadding(), ty, txt, vc, fieldW - textPadding());
            if (m_editing && !m_selectAll) {   // caret at the end of the typed text
                float cx = b.x + textPadding() + JTextHelper::measureWidth(txt) + 1.0f;
                buf.pushRectangle(cx, b.y + 6.0f, 1.5f, b.height - 12.0f, Colors::Accent);
            }
        } else {
            uint8_t vc[4] = {200, 200, 210, 180};
            buf.pushRectangle(b.x + textPadding(), b.y + (b.height-7.0f)*0.5f, fieldW * 0.6f, 7.0f, vc, 2.0f);
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


private:
    std::string _formatValue() const {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", m_decimals, m_value);
        return std::string(buf) + m_suffix;
    }
    // Seed the edit buffer with the current numeric value (no suffix) AND select it, so the first typed
    // digit replaces it (standard entry-field behaviour); backspace/edit keys edit in place instead.
    void _beginEdit() {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", m_decimals, m_value);
        m_editBuf = buf; m_editing = true; m_selectAll = true; invalidate();
    }
    void _commitEdit() {
        if (!m_editing) return;
        m_editing = false; m_selectAll = false;
        try { setValue(std::stod(m_editBuf)); } catch (...) {}
        invalidate();
    }
    // Which typed characters can build a number for this box.
    bool _acceptChar(char c) const {
        if (c >= '0' && c <= '9') return true;
        if (c == '-') return m_min < 0.0 && m_editBuf.find('-') == std::string::npos;
        if (c == '.') return m_decimals > 0 && m_editBuf.find('.') == std::string::npos;
        return false;
    }

    double      m_value, m_min, m_max, m_step;
    int         m_decimals;
    std::string m_suffix;
    bool        m_editing{false};
    bool        m_selectAll{false};
    std::string m_editBuf;
};

// ============================================================================
// JPopupItem — a flat, borderless, full-width clickable text row.
//
// The standard building block for popup list content (combo-boxes, menus,
// tree-pickers, etc.).  It has no border and no background of its own;
// the containing JPopupWindow supplies the backdrop.  On hover it shows a
// subtle tint so the user knows it is interactive.
//
// onActivated fires on mouse-release while the cursor is inside.
// ============================================================================

class JPopupItem : public JControl {
public:
    jf::JSignal<> onActivated;

    JPopupItem(JSceneGraph& graph, const std::string& label,
              float width = 200.f, float itemHeight = 28.f)
        : JControl(graph, "JPopupItem"), m_label(label)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width  = width;
        l.boundingBox.height = itemHeight;
        l.minWidth  = JTextHelper::hasAtlas()
                        ? (JTextHelper::measureWidth(label) + 24.f)
                        : width;
        l.minHeight = itemHeight;
    }

    void setLabel(const std::string& s) {
        m_label = s;
        auto& l = m_graph.getLayout(m_nodeId);
        l.minWidth = JTextHelper::hasAtlas()
                       ? (JTextHelper::measureWidth(s) + 24.f)
                       : l.boundingBox.width;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    const std::string& label() const { return m_label; }

    void handleMouseRelease(float mx, float my) override {
        if (m_state == JWidgetState::Pressed && isPointInside(mx, my)) {
            onClicked.emit();
            onActivated.emit();
        }
        JControl::handleMouseRelease(mx, my);
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;

        // Hover highlight — subtle tint only; no border, no solid fill at rest.
        if (m_state == JWidgetState::Hovered || m_state == JWidgetState::Pressed) {
            uint8_t hi[4] = {255, 255, 255, 18};
            buf.pushRectangle(b.x, b.y, b.width, b.height, hi, 3.0f);
        }

        // JLabel text
        if (JTextHelper::hasAtlas()) {
            uint8_t tc[4] = {220, 220, 228, 230};
            float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            JTextHelper::pushText(buf, b.x + 10.f, ty, tr(m_label), tc,
                                 b.width - 16.f);
        } else {
            // Fallback: coloured bar representing the text
            uint8_t tc[4] = {200, 200, 210, 180};
            float bw = JTextHelper::hasAtlas()
                       ? JTextHelper::measureWidth(m_label)
                       : b.width * 0.6f;
            buf.pushRectangle(b.x + 10.f, b.y + (b.height - 6.f) * 0.5f,
                              bw, 6.f, tc, 2.f);
        }
    }


private:
    std::string m_label;
};

// ============================================================================
// JComboBox
// ============================================================================

enum class JComboBoxMode {
    Cycling,
    Popup
};

class JComboBox : public JControl {
public:
    jf::JSignal<int>         onIndexChanged;
    jf::JSignal<std::string> onTextChanged;
    jf::JSignal<JComboBox*>   onPopupRequested;
    // Framework hook: the app runner installs this to OWN the dropdown — it creates, polls,
    // dismisses the popup window and sets the selected index. When set, it fires on a
    // Popup-mode click so the app needn't wire or service anything. (The per-instance
    // onPopupRequested still fires, for apps not on the runner.)
    static inline std::function<void(JComboBox*)> onOpenPopupHook;

    JComboBox(JSceneGraph& graph, std::vector<std::string> items = {},
             float w = 200.0f, float h = 0.0f)   // h <= 0 → theme control height, so every combo matches
        : JControl(graph, "JComboBox"), m_items(std::move(items))
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().controlHeight;
        _updateMinSize();
    }

    void addItem(const std::string& item) {
        m_items.push_back(item);
        _updateMinSize();
    }
    // Replace the whole item list (dynamic combos). Resets the selection; does not emit onIndexChanged
    // (the caller sets the desired index afterwards).
    void setItems(std::vector<std::string> items) {
        m_items = std::move(items);
        m_currentIndex = m_items.empty() ? -1 : std::clamp(m_currentIndex, 0, (int)m_items.size() - 1);
        _updateMinSize();
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    void clearItems() { setItems({}); }
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

    JComboBoxMode mode() const { return m_mode; }
    void setMode(JComboBoxMode mode) { m_mode = mode; }

    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) {
            onClicked.emit();
            if (!m_items.empty()) {
                if (m_mode == JComboBoxMode::Cycling) {
                    setCurrentIndex((m_currentIndex + 1) % (int)m_items.size());
                } else if (m_mode == JComboBoxMode::Popup) {
                    if (onOpenPopupHook) onOpenPopupHook(this);
                    onPopupRequested.emit(this);
                }
            }
        }
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float arrowW = b.height * 0.75f;
        const uint8_t* fill = (m_state == JWidgetState::Hovered) ? Colors::Surface3 : Colors::Surface2;
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
        if (JTextHelper::hasAtlas() && !currentText().empty()) {
            uint8_t tc[4] = {210, 210, 220, 220};
            float ty = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
            JTextHelper::pushText(buf, b.x + textPadding(), ty, tr(currentText()), tc, b.width - arrowW - textPadding() - 6.0f);
        } else {
            uint8_t tc[4] = {200, 200, 210, 180};
            buf.pushRectangle(b.x + textPadding(), b.y + (b.height-7.0f)*0.5f,
                              b.width - arrowW - textPadding() - 6.0f, 7.0f, tc, 2.0f);
        }
    }


private:
    void _updateMinSize() {
        auto& l = m_graph.getLayout(m_nodeId);
        float maxItemW = 0.f;
        for (const auto& item : m_items) {
            float lw = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(tr(item)) : 40.f;
            if (lw > maxItemW) maxItemW = lw;
        }
        l.minWidth = maxItemW + 36.0f;
        l.minHeight = m_graph.getLayoutConst(m_nodeId).boundingBox.height;
    }

    std::vector<std::string> m_items;
    int m_currentIndex{-1};
    JComboBoxMode m_mode{JComboBoxMode::Popup};   // dropdown list by default (the app wires the popup hook)
};

// ============================================================================
// JColorButton — the property-inspector colour field. The whole control IS the swatch: it fills with
// the chosen colour and prints its hex in a contrast-picked text colour (black on light, white on dark).
// Unset ("") means "inherit the scheme" and shows "(scheme)" on a plain surface. Click opens a picker
// (the app wires onOpenPickerHook → a JColorPicker popup); when inheritable, an inline ✕ clears to
// scheme without opening the picker. Emits onColorChanged("#rrggbb" | "") whenever the value changes.
// Dumb-by-design, exactly like JComboBox: it holds the value + requests the popup, the app owns it.
// ============================================================================

class JColorButton : public JControl {
public:
    static inline std::function<void(JColorButton*)> onOpenPickerHook;
    jf::JSignal<std::string> onColorChanged;

    JColorButton(JSceneGraph& graph, float w = 200.0f, float h = 0.0f) : JControl(graph, "JColorButton") {
        auto& l = m_graph.getLayout(m_nodeId); l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().controlHeight;
    }
    void setInheritable(bool v) { m_inheritable = v; }
    bool inheritable() const    { return m_inheritable; }

    void setColorHex(const std::string& hex) { if (m_hex != hex) { m_hex = hex; m_graph.invalidateNode(m_nodeId, DirtySelf); } }
    const std::string& colorHex() const { return m_hex; }
    // Programmatic set that fires the change signal (used by the picker + clear button).
    void pick(const std::string& hex) { if (m_hex != hex) { m_hex = hex; m_graph.invalidateNode(m_nodeId, DirtySelf); onColorChanged.emit(hex); } }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) return;
        onClicked.emit();
        if (m_inheritable && !m_hex.empty()) {   // inline ✕ hit region on the right clears to scheme
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            if (mx >= b.x + b.width - b.height) { pick(""); return; }
        }
        if (onOpenPickerHook) onOpenPickerHook(this);
    }
    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const bool focused = isFocused();
        uint8_t c[4];
        if (_parse(m_hex, c)) {
            buf.pushRectangle(b.x, b.y, b.width, b.height, c, 4.0f, focused ? 1.5f : 1.0f, focused ? Colors::Accent : Colors::Border);
            if (JTextHelper::hasAtlas()) {
                uint8_t tc[4] = {0, 0, 0, 230};
                if (_luma(c) < 0.5f) { tc[0] = tc[1] = tc[2] = 240; }   // white text on dark fills
                JTextHelper::pushText(buf, b.x + 8.0f, b.y + (b.height - JTextHelper::lineHeight()) * 0.5f, m_hex, tc, b.width - 16.0f);
            }
        } else {
            buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface2, 4.0f, focused ? 1.5f : 1.0f, focused ? Colors::Accent : Colors::Border);
            if (JTextHelper::hasAtlas()) {
                uint8_t tc[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, tc);
                JTextHelper::pushText(buf, b.x + 8.0f, b.y + (b.height - JTextHelper::lineHeight()) * 0.5f, "(scheme)", tc, b.width - 16.0f);
            }
        }
        // Inline clear affordance (only when inheritable + a colour is set).
        if (m_inheritable && !m_hex.empty() && JTextHelper::hasAtlas()) {
            uint8_t xc[4] = {0, 0, 0, 200};
            if (_parse(m_hex, c) && _luma(c) < 0.5f) { xc[0] = xc[1] = xc[2] = 235; }
            JTextHelper::pushText(buf, b.x + b.width - b.height + 6.0f, b.y + (b.height - JTextHelper::lineHeight()) * 0.5f, "x", xc, b.height);
        }
    }

private:
    static bool _parse(const std::string& s, uint8_t o[4]) {
        if (s.size() != 7 || s[0] != '#') return false;
        auto hx = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; };
        int v[6]; for (int i = 0; i < 6; ++i) { v[i] = hx(s[i + 1]); if (v[i] < 0) return false; }
        o[0] = uint8_t(v[0] * 16 + v[1]); o[1] = uint8_t(v[2] * 16 + v[3]); o[2] = uint8_t(v[4] * 16 + v[5]); o[3] = 255; return true;
    }
    static float _luma(const uint8_t c[4]) { return (0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]) / 255.0f; }
    std::string m_hex;
    bool        m_inheritable{false};
};

// ============================================================================
// JFontButton — the property-inspector font field (mirrors JColorButton). Shows the current font
// spec ("Family 12 Bold") or "(scheme)" when unset; click opens a picker via onOpenPickerHook (the app
// wires it to a font dialog). Inheritable → inline ✕ clears to scheme. Emits onFontChanged(spec).
// The spec is a compact string "family|size|b|i" (b/i = 1/0); label() renders it human-readably.
// ============================================================================
class JFontButton : public JControl {
public:
    static inline std::function<void(JFontButton*)> onOpenPickerHook;
    jf::JSignal<std::string> onFontChanged;

    JFontButton(JSceneGraph& graph, float w = 200.0f, float h = 0.0f) : JControl(graph, "JFontButton") {
        auto& l = m_graph.getLayout(m_nodeId); l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().controlHeight;
    }
    void setInheritable(bool v) { m_inheritable = v; }
    void setFontSpec(const std::string& s) { if (m_spec != s) { m_spec = s; m_graph.invalidateNode(m_nodeId, DirtySelf); } }
    const std::string& fontSpec() const { return m_spec; }
    void pick(const std::string& s) { if (m_spec != s) { m_spec = s; m_graph.invalidateNode(m_nodeId, DirtySelf); onFontChanged.emit(s); } }

    // Human-readable label from the "family|size|b|i" spec.
    static std::string prettify(const std::string& spec) {
        if (spec.empty()) return "(scheme)";
        std::string out; size_t p = 0; int field = 0; std::string fam, sz; bool b = false, i = false;
        while (p <= spec.size()) { const size_t c = spec.find('|', p); const std::string t = spec.substr(p, c == std::string::npos ? std::string::npos : c - p);
            if (field == 0) fam = t; else if (field == 1) sz = t; else if (field == 2) b = (t == "1"); else if (field == 3) i = (t == "1");
            ++field; if (c == std::string::npos) break; p = c + 1; }
        out = fam.empty() ? "Default" : fam; if (!sz.empty()) out += " " + sz; if (b) out += " Bold"; if (i) out += " Italic";
        return out;
    }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) return;
        onClicked.emit();
        if (m_inheritable && !m_spec.empty()) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            if (mx >= b.x + b.width - b.height) { pick(""); return; }
        }
        if (onOpenPickerHook) onOpenPickerHook(this);
    }
    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const bool focused = isFocused();
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface2, 4.0f, focused ? 1.5f : 1.0f, focused ? Colors::Accent : Colors::Border);
        if (JTextHelper::hasAtlas()) {
            uint8_t tc[4]; std::copy(m_spec.empty() ? Colors::TextSecondary : Colors::TextPrimary, (m_spec.empty() ? Colors::TextSecondary : Colors::TextPrimary) + 4, tc);
            JTextHelper::pushText(buf, b.x + 8.0f, b.y + (b.height - JTextHelper::lineHeight()) * 0.5f, prettify(m_spec), tc, b.width - 16.0f);
            if (m_inheritable && !m_spec.empty()) {
                uint8_t xc[4] = {200, 200, 210, 200};
                JTextHelper::pushText(buf, b.x + b.width - b.height + 6.0f, b.y + (b.height - JTextHelper::lineHeight()) * 0.5f, "x", xc, b.height);
            }
        }
    }

private:
    std::string m_spec;
    bool        m_inheritable{false};
};

// ============================================================================
// JTornTabState — provenance carried by a JDockWidget born from a tear-off.
// Value type: no vtable, cheap to move.
// ============================================================================

struct JTornTabState {
    std::string title;
    int         originIndex{-1};
    NodeId      contentNode{InvalidNodeId};
    // Called when the JDockWidget is dragged back over a valid DockZone.
    // Args: (originIndex, contentNode) — the bar re-inserts the tab.
    std::function<void(int, NodeId)> reattach;
};

// ============================================================================
// JTabBar
// ============================================================================

class JTabBar : public JControl {
public:
    static constexpr float TEAR_THRESHOLD = 8.0f; // px of movement to initiate a tear

    jf::JSignal<int>          onTabChanged;
    jf::JSignal<int, NodeId>  onTabTorn;   // (tabIndex, contentNode) — app should create a JDockWidget

    JTabBar(JSceneGraph& graph, std::vector<std::string> tabs = {},
           float w = 400.0f, float h = 0.0f)
        : JControl(graph, "JTabBar"), m_tabs(std::move(tabs))
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().menuItemHeight;
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

    // Associate a JSceneGraph content subtree with a tab (required for tear-off)
    void setTabContentNode(int index, NodeId node) {
        if (index >= 0 && index < (int)m_contentNodes.size())
            m_contentNodes[index] = node;
    }

    // Peek whether a tab was torn this frame — consumed by the application
    bool hasTornTab() const { return m_pendingTorn.has_value(); }
    JTornTabState consumeTornTab() {
        auto s = std::move(*m_pendingTorn);
        m_pendingTorn.reset();
        return s;
    }

    // Last drag cursor position — used to place the new JDockWidget on creation
    float lastDragX() const { return m_drag.curX; }
    float lastDragY() const { return m_drag.curY; }

    // Called by the app when a JDockWidget is re-docked onto this bar
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
    void populateDragGhost(JPrimitiveBuffer& buf) const {
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
        JControl::handleMouseMove(mx, my);
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
        JControl::handleMouseRelease(mx, my);
    }

    // Programmatically tear off a tab — useful for keyboard shortcuts and testing.
    void forceTear(int idx) { _emitTear(idx); }

    // Close a tab outright (dynamic tab sets: open on demand, close when done). Unlike a tear this
    // doesn't spawn a float — the tab is gone. Emits onTabChanged with the new active index (-1 when
    // the bar empties) so the host can swap the shown content.
    void removeTab(int idx) {
        if (idx < 0 || idx >= (int)m_tabs.size()) return;
        m_tabs.erase(m_tabs.begin() + idx);
        if (idx < (int)m_contentNodes.size()) m_contentNodes.erase(m_contentNodes.begin() + idx);
        if (m_tabs.empty())                        m_activeIndex = -1;
        else if (m_activeIndex >= (int)m_tabs.size()) m_activeIndex = (int)m_tabs.size() - 1;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        _updateMinSize();
        onTabChanged.emit(m_activeIndex);
    }
    int         tabCount() const { return (int)m_tabs.size(); }
    std::string tabLabel(int idx) const { return (idx >= 0 && idx < (int)m_tabs.size()) ? m_tabs[idx] : std::string(); }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
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

            if (JTextHelper::hasAtlas()) {
                std::string label = tr(m_tabs[i]);
                float lw = JTextHelper::measureWidth(label);
                uint8_t lc[4] = {active ? (uint8_t)220 : (uint8_t)140,
                                  active ? (uint8_t)220 : (uint8_t)140,
                                  active ? (uint8_t)228 : (uint8_t)148,
                                  active ? (uint8_t)220 : (uint8_t)140};
                float ly = b.y + (b.height - JTextHelper::lineHeight()) * 0.5f;
                JTextHelper::pushText(buf, tx + (tabW - lw) * 0.5f, ly, label, lc);
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


private:
    void _updateMinSize() {
        auto& l = m_graph.getLayout(m_nodeId);
        float totalTextW = 0.0f;
        for (const auto& tab : m_tabs) {
            float lw = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(tr(tab)) : 50.0f;
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

        // Build JTornTabState with a re-attach closure bound to this JTabBar
        JTornTabState state;
        state.title       = title;
        state.originIndex = idx;
        state.contentNode = content;
        state.reattach    = [this, title](int origIdx, NodeId node) {
            reinsertTab(origIdx, node, title);
        };

        m_pendingTorn = std::move(state);
        onTabTorn.emit(idx, content);
    }

    struct JDragState {
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
    JDragState                m_drag{};
    std::optional<JTornTabState> m_pendingTorn;
};

// ============================================================================
// JTabWidget — content-hosting tabs with close. A dynamic set of tabs, each holding a custom JWidget
// as its content (non-owning; the app owns the widgets). The bar draws label + a × close affordance
// per closable tab; the active tab's content fills the area below and receives input. This is the
// "tabs that come and go, each showing a custom view" primitive (e.g. a studio's surface editor).
// JTabBar (above) is the bar-only, tear-off-into-a-dock variant; this one owns its content stack.
// ============================================================================

class JTabWidget : public JWidget {
public:
    jf::JSignal<int> onTabChanged;   // (active index, or -1 when empty)
    jf::JSignal<int> onTabClosed;    // (index that was closed) — fired after removal

    JTabWidget(JSceneGraph& graph, float w = 640.0f, float h = 400.0f)
        : JWidget(graph, "JTabWidget")
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().menuItemHeight;
    }

    // content is non-owning; caller keeps it alive while the tab exists. Returns the new tab index.
    // closable and draggable are opt-in per tab: only a closable tab gets a ×; only a draggable tab
    // can be dragged to rearrange. A permanent home tab leaves both off — it stays put and stays open.
    int addTab(const std::string& label, JWidget* content, bool closable = false, bool draggable = false) {
        m_tabs.push_back({ label, content, closable, draggable });
        const int idx = (int)m_tabs.size() - 1;
        if (m_active < 0) { m_active = idx; onTabChanged.emit(m_active); }
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        return idx;
    }
    void removeTab(int idx) {
        if (idx < 0 || idx >= (int)m_tabs.size()) return;
        m_tabs.erase(m_tabs.begin() + idx);
        if (m_tabs.empty())            m_active = -1;
        else if (m_active > idx)       --m_active;
        else if (m_active >= (int)m_tabs.size()) m_active = (int)m_tabs.size() - 1;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        onTabClosed.emit(idx);
        onTabChanged.emit(m_active);
    }
    void setActiveTab(int i) {
        if (i < 0 || i >= (int)m_tabs.size() || i == m_active) return;
        m_active = i;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        onTabChanged.emit(i);
    }
    int      activeTab() const { return m_active; }
    int      tabCount()  const { return (int)m_tabs.size(); }
    JWidget* content(int i) const { return (i >= 0 && i < (int)m_tabs.size()) ? m_tabs[i].content : nullptr; }
    JWidget* activeContent() const { return content(m_active); }
    void     setTabLabel(int i, const std::string& s) { if (i >= 0 && i < (int)m_tabs.size()) { m_tabs[i].label = s; m_graph.invalidateNode(m_nodeId, DirtySelf); } }

    // Pro layout config (opt-in; defaults = a Top strip, tabs at natural width).
    //   edge — which side the strip sits on (Top / Bottom / Left / Right; Left/Right run vertical).
    //   fill — how tabs occupy the strip: Fill = equal share, Left = natural width, Compress = shrink to fit.
    void setTabEdge(JTabBarEdge e) { if (m_edge != e) { m_edge = e; m_graph.invalidateNode(m_nodeId, DirtySelf); } }
    JTabBarEdge tabEdge() const { return m_edge; }
    void setTabFill(JTabFill f)   { if (m_fill != f) { m_fill = f; m_graph.invalidateNode(m_nodeId, DirtySelf); } }
    JTabFill tabFill() const { return m_fill; }
    void  setStripThickness(float t) { m_thick = t; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    float stripThickness() const { return m_thick; }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const JRect strip = _stripRect(b);
        buf.pushRectangle(strip.x, strip.y, strip.width, strip.height, Colors::Surface1);   // strip backdrop
        _layoutTabs(b);
        const bool horiz = _horizontal();
        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            const JRect& r = m_tabRect[i];
            const bool active = (i == m_active);
            buf.pushRectangle(r.x + 1.f, r.y + 1.f, r.width - 2.f, r.height - 2.f,
                              active ? Colors::Surface3 : Colors::Surface1, 5.f);
            _drawActiveEdge(buf, r, active);
            if (JTextHelper::hasAtlas()) {
                uint8_t lc[4]; const uint8_t* base = active ? Colors::TextPrimary : Colors::TextSecondary;
                std::copy(base, base + 4, lc);
                uint8_t cc[4]; const uint8_t* cb = (i == m_hotClose) ? Colors::Danger : Colors::TextSecondary;
                std::copy(cb, cb + 4, cc);
                const float lh = JTextHelper::lineHeight();
                if (horiz) {
                    JTextHelper::pushText(buf, r.x + kPadX, r.y + (r.height - lh) * 0.5f, tr(m_tabs[i].label), lc);
                    if (m_tabs[i].closable)
                        JTextHelper::pushText(buf, r.x + r.width - kCloseW + 3.f, r.y + (r.height - lh) * 0.5f, "\xC3\x97", cc);
                } else {
                    const float px = r.x + (r.width + lh) * 0.5f;   // centre the run across the strip thickness
                    JTextHelper::pushTextVertical(buf, px, r.y + kPadX, tr(m_tabs[i].label), lc, 0.f, m_edge == JTabBarEdge::Right);
                    if (m_tabs[i].closable)
                        JTextHelper::pushTextVertical(buf, px, r.y + r.height - kCloseW + 3.f, "\xC3\x97", cc, 0.f, m_edge == JTabBarEdge::Right);
                }
            }
        }
        if (JWidget* c = activeContent()) {
            c->setBounds(_contentRect(b));
            c->populateRenderPrimitives(buf);
        }
    }

    void handleMousePress(float mx, float my) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (_pointIn(_stripRect(b), mx, my)) {   // in the strip: close / select / begin a reorder drag
            _layoutTabs(b);
            for (int i = 0; i < (int)m_tabs.size(); ++i) {
                if (!_pointIn(m_tabRect[i], mx, my)) continue;
                if (m_tabs[i].closable && _inCloseBox(m_tabRect[i], mx, my)) { removeTab(i); return; }
                setActiveTab(i);
                if (m_tabs[i].draggable) { m_dragIdx = i; m_dragActive = false; m_dragPress = _along(mx, my); }
                return;
            }
            return;
        }
        m_capContent = true;
        if (JWidget* c = activeContent()) c->handleMousePress(mx, my);
    }
    void handleMouseMove(float mx, float my) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (m_dragIdx >= 0) {                     // reordering a draggable tab
            const float a = _along(mx, my);
            if (!m_dragActive && std::abs(a - m_dragPress) > kDragThresh) m_dragActive = true;
            if (m_dragActive) {
                const int target = _tabIndexAt(b, a);
                if (target >= 0 && target != m_dragIdx) { _moveTab(m_dragIdx, target); m_dragIdx = target; }
            }
            return;
        }
        int hot = -1;
        if (_pointIn(_stripRect(b), mx, my)) {
            _layoutTabs(b);
            for (int i = 0; i < (int)m_tabs.size(); ++i)
                if (m_tabs[i].closable && _inCloseBox(m_tabRect[i], mx, my)) { hot = i; break; }
        }
        if (hot != m_hotClose) { m_hotClose = hot; m_graph.invalidateNode(m_nodeId, DirtySelf); }
        if (JWidget* c = activeContent()) c->handleMouseMove(mx, my);
    }
    void handleMouseRelease(float mx, float my) override {
        if (m_dragIdx >= 0) { m_dragIdx = -1; m_dragActive = false; return; }
        if (m_capContent) { if (JWidget* c = activeContent()) c->handleMouseRelease(mx, my); m_capContent = false; return; }
        // A cross-widget drag (e.g. a Dictionary binding dropped onto the canvas) ends with a release that
        // had no preceding press here — so nothing captured. Still deliver it to the active content when the
        // cursor is over the content area, so the drop resolves on release instead of hanging until a click.
        if (JDragDrop::isDragging()) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            if (_pointIn(_contentRect(b), mx, my)) if (JWidget* c = activeContent()) c->handleMouseRelease(mx, my);
        }
    }
    bool handleScroll(float mx, float my, float wheel) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (_pointIn(_contentRect(b), mx, my)) if (JWidget* c = activeContent()) return c->handleScroll(mx, my, wheel);
        return false;
    }
    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (JWidget* c = activeContent()) return c->handleKeyEvent(ke);
        return false;
    }


private:
    struct Tab { std::string label; JWidget* content; bool closable; bool draggable; };

    bool  _horizontal() const { return m_edge == JTabBarEdge::Top || m_edge == JTabBarEdge::Bottom; }
    float _along(float mx, float my) const { return _horizontal() ? mx : my; }
    static bool _pointIn(const JRect& r, float x, float y) {
        return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
    }
    JRect _stripRect(const JRect& b) const {
        switch (m_edge) {
            case JTabBarEdge::Bottom: return { b.x, b.y + b.height - m_thick, b.width, m_thick };
            case JTabBarEdge::Left:   return { b.x, b.y, m_thick, b.height };
            case JTabBarEdge::Right:  return { b.x + b.width - m_thick, b.y, m_thick, b.height };
            default:                  return { b.x, b.y, b.width, m_thick };   // Top
        }
    }
    JRect _contentRect(const JRect& b) const {
        switch (m_edge) {
            case JTabBarEdge::Bottom: return { b.x, b.y, b.width, b.height - m_thick };
            case JTabBarEdge::Left:   return { b.x + m_thick, b.y, b.width - m_thick, b.height };
            case JTabBarEdge::Right:  return { b.x, b.y, b.width - m_thick, b.height };
            default:                  return { b.x, b.y + m_thick, b.width, b.height - m_thick };   // Top
        }
    }
    // Close box sits at the far (trailing) end of a tab along the strip axis.
    bool _inCloseBox(const JRect& r, float mx, float my) const {
        return _horizontal() ? (mx >= r.x + r.width - kCloseW && _pointIn(r, mx, my))
                             : (my >= r.y + r.height - kCloseW && _pointIn(r, mx, my));
    }
    void _layoutTabs(const JRect& b) {
        m_tabRect.assign(m_tabs.size(), JRect{});
        if (m_tabs.empty()) return;
        const bool horiz = _horizontal();
        const float L = (horiz ? b.width : b.height) - 8.f;   // usable strip length (4px pad each end)
        std::vector<float> nat(m_tabs.size());
        float sum = 0.f;
        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            const float lw = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(tr(m_tabs[i].label)) : 60.f;
            nat[i] = kPadX * 2.f + lw + (m_tabs[i].closable ? kCloseW : 0.f);
            sum += nat[i];
        }
        std::vector<float> sz(m_tabs.size());
        if (m_fill == JTabFill::Fill) {
            const float each = L / (float)m_tabs.size();
            for (auto& s : sz) s = each;
        } else if (m_fill == JTabFill::Compress) {
            const float k = (sum > L && sum > 0.f) ? L / sum : 1.f;   // shrink to fit, never past natural
            for (int i = 0; i < (int)m_tabs.size(); ++i) sz[i] = nat[i] * k;
        } else {                                                     // Left: natural width
            sz = nat;
        }
        float along = 4.f;
        const float T = m_thick;
        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            const float s = sz[i];
            switch (m_edge) {
                case JTabBarEdge::Bottom: m_tabRect[i] = { b.x + along, b.y + b.height - T, s, T }; break;
                case JTabBarEdge::Left:   m_tabRect[i] = { b.x, b.y + along, T, s }; break;
                case JTabBarEdge::Right:  m_tabRect[i] = { b.x + b.width - T, b.y + along, T, s }; break;
                default:                  m_tabRect[i] = { b.x + along, b.y, s, T }; break;   // Top
            }
            along += s + 2.f;
        }
    }
    int _tabIndexAt(const JRect& b, float along) {
        _layoutTabs(b);
        if (m_tabs.empty()) return -1;
        const bool horiz = _horizontal();
        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            const float lo = horiz ? m_tabRect[i].x : m_tabRect[i].y;
            const float hi = lo + (horiz ? m_tabRect[i].width : m_tabRect[i].height);
            if (along >= lo && along < hi) return i;
        }
        const float firstLo = horiz ? m_tabRect[0].x : m_tabRect[0].y;
        return (along < firstLo) ? 0 : (int)m_tabs.size() - 1;
    }
    void _moveTab(int from, int to) {
        if (from == to || from < 0 || to < 0 || from >= (int)m_tabs.size() || to >= (int)m_tabs.size()) return;
        Tab t = m_tabs[from];
        m_tabs.erase(m_tabs.begin() + from);
        m_tabs.insert(m_tabs.begin() + to, t);
        if (m_active == from)                 m_active = to;
        else if (from < m_active && m_active <= to) --m_active;
        else if (to <= m_active && m_active < from) ++m_active;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    void _drawActiveEdge(JPrimitiveBuffer& buf, const JRect& r, bool active) const {
        if (!active) return;
        switch (m_edge) {   // an accent line on the content-facing edge of the active tab
            case JTabBarEdge::Bottom: buf.pushRectangle(r.x + 4.f, r.y, r.width - 8.f, 2.f, Colors::Accent, 1.f); break;
            case JTabBarEdge::Left:   buf.pushRectangle(r.x + r.width - 2.5f, r.y + 4.f, 2.f, r.height - 8.f, Colors::Accent, 1.f); break;
            case JTabBarEdge::Right:  buf.pushRectangle(r.x, r.y + 4.f, 2.f, r.height - 8.f, Colors::Accent, 1.f); break;
            default:                  buf.pushRectangle(r.x + 4.f, r.y + r.height - 2.5f, r.width - 8.f, 2.f, Colors::Accent, 1.f); break;
        }
    }

    std::vector<Tab>   m_tabs;
    std::vector<JRect> m_tabRect;
    int         m_active   = -1;
    int         m_hotClose = -1;
    bool        m_capContent = false;
    int         m_dragIdx  = -1;
    bool        m_dragActive = false;
    float       m_dragPress = 0.f;
    JTabBarEdge m_edge = JTabBarEdge::Top;
    JTabFill    m_fill = JTabFill::Left;
    float       m_thick = 34.0f;
    static constexpr float kPadX = 12.0f, kCloseW = 18.0f, kDragThresh = 6.0f;
};

// ============================================================================
// JGroupBox  (labelled container panel)
// ============================================================================

class JGroupBox : public JWidget {
public:
    JGroupBox(JSceneGraph& graph, const std::string& title,
             float w = 320.0f, float h = 120.0f)
        : JWidget(graph, "JGroupBox: " + title), m_title(title)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width  = w;
        l.boundingBox.height = h;
        l.padding = 12.0f;
        l.gap     = 8.0f;
        l.direction = JFlexDirection::Column;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        // Panel body
        uint8_t panelFill[4] = {22, 22, 25, 180};
        buf.pushRectangle(b.x, b.y, b.width, b.height, panelFill, 8.0f, 1.0f, Colors::Border);
        // Title bar strip at top
        uint8_t titleBg[4] = {36, 36, 40, 200};
        buf.pushRectangle(b.x + 1.0f, b.y + 1.0f, b.width - 2.0f, 24.0f, titleBg, 7.0f);
        // Title text
        if (JTextHelper::hasAtlas()) {
            uint8_t tc[4] = {200, 200, 210, 200};
            float ty = b.y + (24.0f - JTextHelper::lineHeight()) * 0.5f;
            JTextHelper::pushText(buf, b.x + 10.0f, ty, tr(m_title), tc, b.width - 20.0f);
        } else {
            uint8_t tc[4] = {200, 200, 210, 180};
            buf.pushRectangle(b.x + 10.0f, b.y + 9.0f, b.width * 0.4f, 7.0f, tc, 2.0f);
        }
    }


private:
    std::string m_title;
};

// ============================================================================
// JContainer — a plain container widget: holds a child widget tree, arranges it with the
// scene-graph layout engine (Flex / Grid / Form via its JLayoutComponent), renders it, and
// routes input to it. No chrome of its own. The reusable building block for panels, property
// forms, toolbars, and dock content (JDockWidget::setContent) — the app composes it instead of
// hand-drawing. Non-owning children (the app owns the widgets, as it owns the container).
// ============================================================================
class JContainer : public JWidget {
public:
    JContainer(JSceneGraph& graph, float w = 200.0f, float h = 100.0f)
        : JWidget(graph, "JContainer")
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width  = w;
        l.boundingBox.height = h;
    }

    // Add a child to the tree; registers it with the layout engine so it's arranged/measured.
    JContainer* add(JWidget* w) {
        if (!w) return this;
        m_children.push_back(w);
        m_graph.addChild(m_nodeId, w->getNodeId());
        return this;
    }
    const std::vector<JWidget*>& children() const { return m_children; }

    // Layout configuration — thin pass-throughs to this node's layout component (chainable).
    JContainer* setLayoutMode(JLayoutMode m)   { m_graph.getLayout(m_nodeId).mode = m; return this; }
    JContainer* setColumns(int n)              { m_graph.getLayout(m_nodeId).columns = n; return this; }
    JContainer* setDirection(JFlexDirection d) { m_graph.getLayout(m_nodeId).direction = d; return this; }
    JContainer* setGap(float g)                { m_graph.getLayout(m_nodeId).gap = g; return this; }
    JContainer* setPadding(JEdges p)           { m_graph.getLayout(m_nodeId).padding = p; return this; }
    JContainer* setAlignItems(JAlignItems a)   { m_graph.getLayout(m_nodeId).alignItems = a; return this; }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        // Arrange the subtree into our current box (set by the host via setBounds), then paint
        // each child. Fixed constraints pin the container to its box; children flow within it.
        const JRect b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        m_graph.computeLayout(m_nodeId, JConstraints{b.width, b.width, b.height, b.height});
        for (JWidget* w : m_children) if (w->isVisible()) w->populateRenderPrimitives(buf);
    }

    void handleMouseMove(float mx, float my) override {
        for (JWidget* w : m_children) if (w->isVisible()) w->handleMouseMove(mx, my);
    }
    void handleMousePress(float mx, float my) override {
        for (JWidget* w : m_children) if (w->isVisible()) w->handleMousePress(mx, my);
    }
    void handleMouseRelease(float mx, float my) override {
        for (JWidget* w : m_children) if (w->isVisible()) w->handleMouseRelease(mx, my);
    }
    bool handleScroll(float mx, float my, float wheel) override {
        bool consumed = false;
        for (JWidget* w : m_children) if (w->isVisible()) consumed |= w->handleScroll(mx, my, wheel);
        return consumed;
    }


private:
    std::vector<JWidget*> m_children;   // non-owning
};

// ============================================================================
// JScrollArea
// ============================================================================

class JScrollArea : public JWidget {
public:
    JScrollArea(JSceneGraph& graph, float w = 320.0f, float h = 200.0f)
        : JWidget(graph, "JScrollArea")
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width  = w;
        l.boundingBox.height = h;
    }

    void addChildWidget(JWidget* w) {
        m_children.push_back(w);
    }
    void clearChildren() { m_children.clear(); m_scrollY = 0.0f; }   // for rebuildable content (e.g. a per-selection form)

    const std::vector<JWidget*>& children() const { return m_children; }

    void handleMouseMove(float mx, float my) override {
        if (m_draggingScroll) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            float totalH = 12.0f;
            for (JWidget* w : m_children)
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
            for (JWidget* w : m_children) {
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
                for (JWidget* w : m_children) {
                    if (w->isVisible()) w->handleMousePress(mx, my);
                }
            }
        }
    }

    void handleMouseRelease(float mx, float my) override {
        m_draggingScroll = false;
        for (JWidget* w : m_children) {
            if (w->isVisible()) w->handleMouseRelease(mx, my);
        }
    }

    bool handleScroll(float mx, float my, float wheel) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            bool consumed = false;
            for (JWidget* w : m_children) {
                if (w->isVisible()) {
                    if (w->handleScroll(mx, my, wheel)) {
                        consumed = true;
                    }
                }
            }
            if (consumed) return true;

            float totalH = 12.0f;
            for (JWidget* w : m_children) {
                totalH += m_graph.getLayoutConst(w->getNodeId()).boundingBox.height + 6.0f;
            }
            float maxScrollY = std::max(0.0f, totalH - b.height);
            m_scrollY = std::clamp(m_scrollY - wheel * 40.0f, 0.0f, maxScrollY);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        }
        return false;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        
        // Background
        uint8_t fill[4] = {20, 20, 24, 255};
        buf.pushRectangle(b.x, b.y, b.width, b.height, fill, 6.0f, 1.0f, Colors::Border);

        if (m_children.empty()) return;

        // Perform Layout
        float curY = b.y + 6.0f - m_scrollY;
        float innerW = b.width - 16.0f;
        float totalH = 12.0f;

        for (JWidget* w : m_children) {
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
        for (JWidget* w : m_children) {
            auto& wl = m_graph.getLayout(w->getNodeId());
            wl.boundingBox.y = curY;
            curY += wl.boundingBox.height + 6.0f;
        }

        // Render with clip scissor
        buf.pushClip(b.x + 1.0f, b.y + 1.0f, b.width - 13.0f, b.height - 2.0f);
        for (JWidget* w : m_children) {
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


private:
    std::vector<JWidget*> m_children;
    float   m_scrollY{0.0f};
    bool    m_hovered{false};
    bool    m_draggingScroll{false};
    float   m_dragStartY{0.0f};
    float   m_dragStartScrollY{0.0f};
};

// ============================================================================
// JListView
// ============================================================================

class JListView : public JControl {
public:
    jf::JSignal<int> onSelectionChanged;
    jf::JSignal<int> onItemActivated;

    JListView(JSceneGraph& graph, std::vector<std::string> items = {},
             float w = 240.0f, float h = 200.0f)
        : JControl(graph, "JListView"), m_items(std::move(items))
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().menuItemHeight;
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
            float itemH = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() + 8.0f : 20.0f;
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
        JControl::handleMouseMove(mx, my);
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
                float itemH = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() + 8.0f : 20.0f;
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
        JControl::handleMouseRelease(mx, my);
    }

    bool handleScroll(float mx, float my, float wheel) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height) {
            float itemH = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() + 8.0f : 20.0f;
            float totalH = m_items.size() * itemH + 8.0f;
            float maxScrollY = std::max(0.0f, totalH - b.height);
            m_scrollY = std::clamp(m_scrollY - wheel * 30.0f, 0.0f, maxScrollY);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        }
        return false;
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed || m_items.empty()) return false;
        using K = JKeyEvent::JKey;

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

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        bool focused = isFocused();

        // Background
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface1, 6.0f,
                          focused ? 1.5f : 1.0f,
                          focused ? Colors::Accent : Colors::Border);

        if (m_items.empty()) return;

        float itemH = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() + 8.0f : 20.0f;
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

            if (JTextHelper::hasAtlas()) {
                uint8_t tc[4] = {220, 220, 228, 220};
                if (i == m_selectedIndex) {
                    tc[0] = 255; tc[1] = 255; tc[2] = 255;
                }
                JTextHelper::pushText(buf, b.x + 8.0f, itemY + 4.0f, tr(m_items[i]), tc, itemW - 12.0f);
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



private:
    void _ensureIndexVisible(int index) {
        if (index < 0 || index >= (int)m_items.size()) return;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float itemH = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() + 8.0f : 20.0f;
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

struct JTreeViewNode {
    std::string label;
    bool expanded{false};
    bool selected{false};
    std::vector<JTreeViewNode> children;
    std::string userData;   // opaque app payload (e.g. a binding path) carried by a node but not displayed
    int         icon{0};    // small type glyph drawn before the label (0 = none; app-defined kinds)
    bool        hidden{false};   // transient: run-mode visibility filter hides the row + its subtree (not persisted)
};

class JTreeView : public JControl {
public:
    jf::JSignal<JTreeViewNode*> onSelectionChanged;
    jf::JSignal<JTreeViewNode*> onNodeActivated;
    jf::JSignal<JTreeViewNode*> onNodeRenamed;    // fired after an in-place label edit commits
    jf::JSignal<JTreeViewNode*> onNodeDragStarted; // press-and-drag on a node past a threshold (app starts a JDragDrop)
    jf::JSignal<> onDeleteKey;   // Delete/Backspace pressed with a node selected (app removes it)
    jf::JSignal<> onEnterKey;    // Return pressed with a node selected and not renaming (app adds a sibling)
    // Internal drag-reorder: fires on drop with (moved node, new parent, insert index). Enable with
    // setInternalReorder(true); then a node drag reorders IN the tree instead of firing onNodeDragStarted.
    jf::JSignal<JTreeViewNode*, JTreeViewNode*, int> onNodeMoved;
    void setInternalReorder(bool on) { m_internalReorder = on; }

    // Begin an in-place rename of the selected node (context-menu "Rename"). Enter commits (fires
    // onNodeRenamed), Escape cancels; the row draws an edit field with a caret while active.
    void beginRename() {
        if (!m_selectedNode) return;
        m_editNode = m_selectedNode;
        m_editBuf  = m_selectedNode->label;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    bool isEditing() const { return m_editNode != nullptr; }

    // Commit an in-place rename (Enter, click elsewhere, or focus loss): apply the buffer + fire
    // onNodeRenamed. Cancel (Escape) just clears m_editNode without applying.
    void commitRename() {
        if (!m_editNode) return;
        m_editNode->label = m_editBuf;
        JTreeViewNode* n = m_editNode;
        m_editNode = nullptr;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        onNodeRenamed.emit(n);
    }

    // Whether F2 / double-activate may start an in-place rename (default on). Apps with read-only
    // trees (e.g. a fixed config navigator) can disable it.
    void setEditable(bool e) { m_editable = e; }
    bool isEditable() const { return m_editable; }

    JTreeView(JSceneGraph& graph, float w = 240.0f, float h = 300.0f)
        : JControl(graph, "JTreeView"), m_root{"Root", true, false, {}}
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().menuItemHeight;
        l.minWidth = 80.0f;
        l.minHeight = 40.0f;
    }

    JTreeViewNode& root() { return m_root; }
    const JTreeViewNode& root() const { return m_root; }

    void setRootNode(JTreeViewNode rootNode) {
        m_root = std::move(rootNode);
        m_selectedNode = nullptr;
        m_scrollY = 0.0f;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    // Expand / collapse the whole tree. The (synthetic) root stays expanded so its top-level rows
    // remain visible; every descendant is set accordingly.
    // Filter rows to those whose label — or a descendant's — contains `f` (case-insensitive). Empty
    // clears the filter. Matching subtrees are auto-revealed. Drives the dock search boxes.
    void setFilter(const std::string& f) {
        std::string lo = f; for (char& c : lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lo != m_filter) { m_filter = std::move(lo); m_scrollY = 0.f; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    }

    void expandAll()   { for (auto& c : m_root.children) _setExpandedRec(c, true);  m_root.expanded = true; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    void collapseAll() { for (auto& c : m_root.children) _setExpandedRec(c, false); m_root.expanded = true; m_graph.invalidateNode(m_nodeId, DirtySelf); }

    // Run-mode condition filtering (mirrors the original EditTree::applyConditions): hide every node whose
    // predicate returns false — and its whole subtree — in place, so selection + scroll survive. Re-run each
    // telemetry frame; only invalidates when the visible set actually changes (no per-frame flicker/rebuild).
    void applyVisibility(const std::function<bool(const JTreeViewNode&)>& visible) {
        bool changed = false;
        for (auto& c : m_root.children) _applyVis(c, visible, changed);
        if (changed) { if (m_selectedNode && _isHidden(m_root, m_selectedNode)) _selectNode(nullptr); m_graph.invalidateNode(m_nodeId, DirtySelf); }
    }
    void clearVisibility() {
        bool changed = false;
        for (auto& c : m_root.children) _clearVis(c, changed);
        if (changed) m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    float rowHeight() const { return m_rowHeight; }
    void setRowHeight(float h) { m_rowHeight = h; m_graph.invalidateNode(m_nodeId, DirtySelf); }

    float getItemHeight() const {
        return m_rowHeight > 0.0f ? m_rowHeight : (JTextHelper::hasAtlas() ? JTextHelper::lineHeight() + 8.0f : 22.0f);
    }

    struct JFlatNode {
        JTreeViewNode* node;
        int depth;
        size_t flatIndex;
    };

    std::vector<JFlatNode> getFlatNodes() {
        std::vector<JFlatNode> flat;
        for (auto& child : m_root.children) {
            _flatten(child, 0, flat);
        }
        return flat;
    }

    void handleMousePress(float mx, float my) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        // A press anywhere commits an in-place rename in progress (clicking another node, the same node,
        // or empty tree space all end editing) before the click is handled.
        const bool wasEditing = (m_editNode != nullptr);
        if (wasEditing) commitRename();
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
                        m_pressNode = flat.node; m_pressX = mx; m_pressY = my;   // arm a potential drag
                        // A DOUBLE-click (same node twice within 400 ms) begins an in-place rename;
                        // a single click selects + activates.
                        const auto nowT = std::chrono::steady_clock::now();
                        const bool dbl = m_editable && !wasEditing && flat.node == m_lastClickNode &&
                                         std::chrono::duration_cast<std::chrono::milliseconds>(nowT - m_lastClickTime).count() < 400;
                        m_lastClickNode = flat.node; m_lastClickTime = nowT;
                        _selectNode(flat.node);
                        if (dbl) beginRename();
                        else     onNodeActivated.emit(flat.node);
                    }
                }
            }
        }
    }

    void handleMouseRelease(float mx, float my) override {
        m_draggingScroll = false;
        if (m_dragging) {
            JTreeViewNode* moved = m_pressNode;
            _computeDrop(mx, my);
            JTreeViewNode* target = m_dropTarget; int mode = m_dropMode;
            m_dragging = false; m_dropTarget = nullptr; m_pressNode = nullptr;
            bool didMove = false;
            if (moved && target && moved != target && !_isAncestor(moved, target)) {
                std::vector<int> srcPath = _pathOf(moved);
                std::vector<int> destParentPath; int destIdx = 0;
                if (mode == 1) {                       // drop as last child of target
                    destParentPath = _pathOf(target);
                    destIdx = (int)target->children.size();
                } else {                               // insert before/after target (as its sibling)
                    std::vector<int> tp = _pathOf(target);
                    if (!tp.empty()) { destIdx = tp.back() + (mode == 2 ? 1 : 0); tp.pop_back(); destParentPath = tp; }
                }
                didMove = _moveNode(srcPath, destParentPath, destIdx);
            }
            m_selectedNode = nullptr;   // vector reallocation invalidated node pointers
            m_lastClickNode = nullptr;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            if (didMove) onNodeMoved.emit(nullptr, nullptr, 0);   // app re-reads root() + persists
            return;
        }
        m_pressNode = nullptr;
        JControl::handleMouseRelease(mx, my);
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
        // An internal-reorder drag in progress: track the drop target under the cursor each move.
        if (m_dragging) {
            // If the drag leaves the tree's bounds, hand the node off to the app as a drag payload
            // (onNodeDragStarted) so a drop target elsewhere — e.g. a surface placing the node as a
            // viewport — can accept it. The internal reorder is cancelled. Mirrors the original studio
            // tree, whose drag carried BOTH an internal-move payload and a node-path mime at once.
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            const bool outside = mx < b.x || mx > b.x + b.width || my < b.y || my > b.y + b.height;
            if (outside && m_pressNode) {
                JTreeViewNode* n = m_pressNode;
                m_dragging = false; m_dropTarget = nullptr; m_pressNode = nullptr;
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                onNodeDragStarted.emit(n);
                return;
            }
            _computeDrop(mx, my); m_graph.invalidateNode(m_nodeId, DirtySelf); return;
        }
        // Press-and-drag on a node past a small threshold starts a drag. With internal reorder enabled
        // it becomes an in-tree move (drop indicator + onNodeMoved); otherwise the app turns it into a
        // JDragDrop of the node payload (one-shot: clear the armed node so it fires once).
        if (m_pressNode && !m_editNode) {
            const float ddx = mx - m_pressX, ddy = my - m_pressY;
            if (ddx * ddx + ddy * ddy > 25.0f) {
                if (m_internalReorder) { m_dragging = true; _computeDrop(mx, my); m_graph.invalidateNode(m_nodeId, DirtySelf); }
                else { JTreeViewNode* n = m_pressNode; m_pressNode = nullptr; onNodeDragStarted.emit(n); }
            }
        }
        JControl::handleMouseMove(mx, my);
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

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed) return false;
        using EK = JKeyEvent::JKey;

        // In-place rename: the tree owns keyboard while editing a label.
        if (m_editNode) {
            if (ke.key == EK::Return) { commitRename(); return true; }
            if (ke.key == EK::Escape) { m_editNode = nullptr; m_graph.invalidateNode(m_nodeId, DirtySelf); return true; }
            if (ke.key == EK::Backspace) { if (!m_editBuf.empty()) m_editBuf.pop_back(); m_graph.invalidateNode(m_nodeId, DirtySelf); return true; }
            if (static_cast<unsigned char>(ke.utf8[0]) >= 32) { m_editBuf += ke.utf8; m_graph.invalidateNode(m_nodeId, DirtySelf); return true; }
            return true;   // swallow other keys while editing
        }

        // F2 begins an in-place rename of the selected node (standard rename shortcut).
        if (m_editable && ke.key == EK::F2 && m_selectedNode) { beginRename(); return true; }

        // Structural shortcuts (mirror the original studio's EditTree): Delete/Backspace remove the
        // selected node, Return adds a sibling. The app connects these and applies its own edit-mode gate.
        if ((ke.key == EK::Delete || ke.key == EK::Backspace) && m_selectedNode) { onDeleteKey.emit(); return true; }
        if (ke.key == EK::Return && m_selectedNode) { onEnterKey.emit(); return true; }

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

        using K = JKeyEvent::JKey;
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
                    JTreeViewNode* parent = _findParent(&m_root, n);
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

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        bool focused = isFocused();
        if (m_editNode && !focused) commitRename();   // focus moved elsewhere (another widget) — end the edit

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
                // While filtering, subtrees are force-opened (_flatten), so show ▼ then too — not a stale ▶.
                drawNodeChevron(buf, flat.node, ax, ay, 10.0f, flat.node->expanded || !m_filter.empty());
            }

            float textX = b.x + indent + 16.0f;
            float ty = itemY + (itemH - (JTextHelper::hasAtlas() ? JTextHelper::lineHeight() : 8.0f)) * 0.5f;
            if (flat.node == m_editNode) {
                // In-place edit field: boxed buffer + caret over the row.
                const float ex = textX - 3.0f, ew = b.x + b.width - 16.0f - ex;
                buf.pushRectangle(ex, itemY + 2.0f, ew, itemH - 4.0f, Colors::Surface0, 3.0f, 1.5f, Colors::Accent);
                if (JTextHelper::hasAtlas()) {
                    uint8_t tc[4] = {225, 225, 232, 230};
                    JTextHelper::pushText(buf, textX, ty, m_editBuf, tc, ew - 10.0f);
                    buf.pushRectangle(textX + JTextHelper::measureWidth(m_editBuf) + 1.0f, itemY + 4.0f, 1.5f, itemH - 8.0f, Colors::Accent);
                }
            } else {
                float tx = textX;
                if (flat.node->icon != 0) { _drawTreeIcon(buf, textX + 1.0f, itemY + itemH * 0.5f, flat.node->icon); tx += 15.0f; }
                drawNodeText(buf, flat.node, tx, ty, b.width - (tx - b.x) - 14.0f);
            }
        }

        // Drop indicator during an internal-reorder drag: a boxed row for "into", else a caret line
        // at the top (before) or bottom (after) of the hovered row.
        if (m_dragging && m_dropTarget) {
            int di = -1;
            for (int i = 0; i < (int)flatNodes.size(); ++i) if (flatNodes[i].node == m_dropTarget) { di = i; break; }
            if (di >= 0) {
                float ry = b.y + 4.0f + di * itemH - m_scrollY;
                uint8_t ind[4] = {10, 132, 255, 255};
                if (m_dropMode == 1) {
                    buf.pushRectangle(b.x + 3.0f, ry, b.width - 16.0f, itemH, Colors::Transparent, 4.0f, 2.0f, ind);
                } else {
                    float ly = (m_dropMode == 2) ? ry + itemH - 1.0f : ry - 1.0f;
                    buf.pushRectangle(b.x + 6.0f, ly, b.width - 20.0f, 2.5f, ind, 1.0f);
                }
            }
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



    JTreeViewNode* selectedNode() const { return m_selectedNode; }

protected:
    virtual void drawNodeBackground(JPrimitiveBuffer& buf, JTreeViewNode* node, const JRect& bounds) {
        uint8_t selBg[4] = {10, 132, 255, 60};
        buf.pushRectangle(bounds.x, bounds.y, bounds.width, bounds.height, selBg, 4.0f);
    }

    // A small ~10px type glyph before a leaf label (app-assigned JTreeViewNode::icon). Shapes/colours
    // distinguish kinds without needing font symbol glyphs: 1 grid, 2 rounded, 3 bar, 4 pill, 5 filled,
    // else a hollow outline. Kinds are app-defined; unknown ones fall through to the hollow default.
    void _drawTreeIcon(JPrimitiveBuffer& buf, float x, float cy, int kind) {
        const float s = 10.0f, y = cy - s * 0.5f, h = s * 0.45f;
        static const uint8_t orange[4] = {220, 150, 60, 255}, blue[4] = {90, 150, 230, 255},
                             green[4] = {90, 200, 130, 255}, purple[4] = {175, 130, 225, 255}, cyan[4] = {90, 200, 220, 255};
        switch (kind) {
            case 1:  // table — 2×2 grid
                buf.pushRectangle(x, y, h, h, orange);           buf.pushRectangle(x + h + 1.f, y, h, h, orange);
                buf.pushRectangle(x, y + h + 1.f, h, h, orange); buf.pushRectangle(x + h + 1.f, y + h + 1.f, h, h, orange);
                break;
            case 2:  buf.pushRectangle(x, y, s, s, cyan, 3.0f); break;                 // curve
            case 3:  buf.pushRectangle(x, y + s * 0.28f, s, s * 0.44f, purple, 2.0f); break;   // enum — bar
            case 4:  buf.pushRectangle(x, y + s * 0.15f, s, s * 0.7f, green, s * 0.35f); break; // toggle — pill
            case 5:  buf.pushRectangle(x, y, s, s, blue, 2.0f); break;                 // config — filled
            default: buf.pushRectangle(x, y, s, s, Colors::Transparent, 2.0f, 1.5f, green); break;   // value/channel — hollow
        }
    }

    virtual void drawNodeChevron(JPrimitiveBuffer& buf, JTreeViewNode* /*node*/, float ax, float ay, float /*size*/, bool expanded) {
        // A filled triangle disclosure arrow (▶ collapsed / ▼ expanded), matching the original studio's tree
        // branch indicators — not the old crude split-bar (which read as vertical dots).
        const JColor col = jf::rgba(180, 180, 190, 230);
        const float s = 4.0f;
        JVectorCanvas vg; vg.setAntiAlias(1.2f);
        if (expanded) vg.fillConvex({ {ax - s, ay - s * 0.55f}, {ax + s, ay - s * 0.55f}, {ax, ay + s * 0.85f} }, JPaint::solid(col));   // ▼
        else          vg.fillConvex({ {ax - s * 0.55f, ay - s}, {ax + s * 0.85f, ay}, {ax - s * 0.55f, ay + s} }, JPaint::solid(col));   // ▶
        vg.flush(buf);
    }

    virtual void drawNodeText(JPrimitiveBuffer& buf, JTreeViewNode* node, float tx, float ty, float maxW) {
        if (JTextHelper::hasAtlas()) {
            uint8_t tc[4] = {210, 210, 220, 220};
            JTextHelper::pushText(buf, tx, ty, tr(node->label), tc, maxW);
        } else {
            uint8_t tc[4] = {200, 200, 210, 180};
            buf.pushRectangle(tx, ty + 2.0f, 60.0f, 8.0f, tc, 2.0f);
        }
    }

private:
    void _flatten(JTreeViewNode& node, int depth, std::vector<JFlatNode>& result) {
        if (node.hidden) return;                                // run-mode condition filter (self + descendants)
        if (!m_filter.empty() && !_nodeMatches(node)) return;   // filtered out (self + descendants)
        result.push_back({&node, depth, result.size()});
        // While filtering, force subtrees open so matches deep in the tree are revealed.
        if (node.expanded || !m_filter.empty()) {
            for (auto& child : node.children) {
                _flatten(child, depth + 1, result);
            }
        }
    }
    // A node is shown when its label — or any descendant's — contains the (lower-cased) filter.
    bool _nodeMatches(const JTreeViewNode& n) const {
        std::string l = n.label; for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (l.find(m_filter) != std::string::npos) return true;
        for (const auto& c : n.children) if (_nodeMatches(c)) return true;
        return false;
    }

    void _selectNode(JTreeViewNode* node) {
        if (m_selectedNode != node) {
            if (m_selectedNode) m_selectedNode->selected = false;
            m_selectedNode = node;
            if (m_selectedNode) m_selectedNode->selected = true;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onSelectionChanged.emit(node);
        }
    }

    JTreeViewNode* _findParent(JTreeViewNode* current, JTreeViewNode* target) {
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

    static void _setExpandedRec(JTreeViewNode& n, bool e) { n.expanded = e; for (auto& c : n.children) _setExpandedRec(c, e); }

    // Run-mode condition filter helpers (applyVisibility / clearVisibility). A node is hidden when its own
    // predicate is false; a hidden node hides its whole subtree (children not evaluated). `changed` tracks
    // whether the visible set moved, so the caller only invalidates on a real transition.
    static void _applyVis(JTreeViewNode& n, const std::function<bool(const JTreeViewNode&)>& visible, bool& changed) {
        const bool hide = !visible(n);
        if (n.hidden != hide) { n.hidden = hide; changed = true; }
        if (hide) { _clearVis(n, changed, /*childrenOnly*/ true); return; }   // subtree follows a hidden ancestor
        for (auto& c : n.children) _applyVis(c, visible, changed);
    }
    static void _clearVis(JTreeViewNode& n, bool& changed, bool childrenOnly = false) {
        if (!childrenOnly && n.hidden) { n.hidden = false; changed = true; }
        for (auto& c : n.children) _clearVis(c, changed);
    }
    // True if `target` lies anywhere in `node`'s subtree AND that path passes through a hidden node — i.e. the
    // node is currently filtered out. Used to drop a selection that a filter just hid.
    static bool _isHidden(JTreeViewNode& node, JTreeViewNode* target) {
        for (auto& c : node.children) {
            if (&c == target) return c.hidden;
            if (c.hidden) { if (_contains(c, target)) return true; }   // hidden ancestor hides the target
            else if (_isHidden(c, target)) return true;
        }
        return false;
    }
    static bool _contains(JTreeViewNode& node, JTreeViewNode* target) {
        for (auto& c : node.children) { if (&c == target) return true; if (_contains(c, target)) return true; }
        return false;
    }

    // --- internal drag-reorder ---
    // Decide the drop target + mode (0 before / 1 into / 2 after) for the cursor at (mx,my).
    void _computeDrop(float mx, float my) {
        (void)mx;
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        auto flat = getFlatNodes();
        m_dropTarget = nullptr; m_dropMode = 0;
        if (flat.empty()) return;
        float itemH = getItemHeight();
        float rel = my - b.y + m_scrollY - 4.0f;
        int idx = std::clamp((int)(rel / itemH), 0, (int)flat.size() - 1);
        m_dropTarget = flat[idx].node;
        float frac = (rel - idx * itemH) / itemH;
        m_dropMode = (frac < 0.25f) ? 0 : (frac > 0.75f) ? 2 : 1;
    }
    bool _isAncestor(JTreeViewNode* a, JTreeViewNode* b) {
        if (a == b) return true;
        for (auto& c : a->children) if (_isAncestor(&c, b)) return true;
        return false;
    }
    std::vector<int> _pathOf(JTreeViewNode* target) {
        std::vector<int> path;
        std::function<bool(JTreeViewNode&)> rec = [&](JTreeViewNode& n) -> bool {
            for (int i = 0; i < (int)n.children.size(); ++i) {
                path.push_back(i);
                if (&n.children[i] == target) return true;
                if (rec(n.children[i])) return true;
                path.pop_back();
            }
            return false;
        };
        if (rec(m_root)) return path;
        return {};
    }
    JTreeViewNode* _atPath(const std::vector<int>& p) {
        JTreeViewNode* n = &m_root;
        for (int i : p) { if (i < 0 || i >= (int)n->children.size()) return nullptr; n = &n->children[i]; }
        return n;
    }
    // Move the node at srcPath to be child #destIdx of the node at destParentPath, correcting for the
    // index shifts that removing the source introduces (same parent, or a dest branch past the source).
    bool _moveNode(std::vector<int> srcPath, std::vector<int> destParentPath, int destIdx) {
        if (srcPath.empty()) return false;
        int srcIdx = srcPath.back();
        std::vector<int> srcParentPath(srcPath.begin(), srcPath.end() - 1);
        JTreeViewNode* srcParent = _atPath(srcParentPath);
        if (!srcParent || srcIdx < 0 || srcIdx >= (int)srcParent->children.size()) return false;
        JTreeViewNode moved = std::move(srcParent->children[srcIdx]);
        srcParent->children.erase(srcParent->children.begin() + srcIdx);
        // If the destination path descends through srcParent past the removed index, shift it down one.
        if (destParentPath.size() > srcParentPath.size() &&
            std::equal(srcParentPath.begin(), srcParentPath.end(), destParentPath.begin())) {
            int& branch = destParentPath[srcParentPath.size()];
            if (branch > srcIdx) branch -= 1;
        }
        JTreeViewNode* destParent = _atPath(destParentPath);
        if (!destParent) {   // shouldn't happen; put it back where it came from
            srcParent->children.insert(srcParent->children.begin() + std::min(srcIdx, (int)srcParent->children.size()), std::move(moved));
            return false;
        }
        if (destParent == srcParent && destIdx > srcIdx) destIdx -= 1;
        destIdx = std::clamp(destIdx, 0, (int)destParent->children.size());
        destParent->children.insert(destParent->children.begin() + destIdx, std::move(moved));
        return true;
    }

    JTreeViewNode  m_root;
    JTreeViewNode* m_selectedNode{nullptr};
    JTreeViewNode* m_editNode{nullptr};   // node whose label is being edited in place
    std::string    m_editBuf;             // working text during an in-place rename
    bool           m_editable{true};      // F2 / click-selected may start an in-place rename
    std::string    m_filter;              // lower-cased row filter ("" = show all)
    float         m_scrollY{0.0f};
    float         m_rowHeight{-1.0f};
    bool          m_draggingScroll{false};
    float         m_dragStartY{0.0f};
    float         m_dragStartScrollY{0.0f};
    JTreeViewNode* m_pressNode{nullptr};   // node pressed (candidate for a drag), cleared once the drag fires/ends
    float          m_pressX{0.0f}, m_pressY{0.0f};
    bool           m_internalReorder{false};   // drag = in-tree move (vs. onNodeDragStarted external DnD)
    bool           m_dragging{false};          // an internal-reorder drag is live
    JTreeViewNode* m_dropTarget{nullptr};      // row under the cursor during a drag
    int            m_dropMode{0};              // 0 = insert before, 1 = drop as child, 2 = insert after
    JTreeViewNode* m_lastClickNode{nullptr};                 // double-click-to-rename tracking
    std::chrono::steady_clock::time_point m_lastClickTime{};
};

class JDataGrid : public JControl {
public:
    jf::JSignal<int> onSelectionChanged;
    jf::JSignal<int> onRowActivated;

    JDataGrid(JSceneGraph& graph, std::vector<std::string> headers = {}, float w = 400.0f, float h = 250.0f)
        : JControl(graph, "JDataGrid"), m_headers(std::move(headers))
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().menuItemHeight;
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
        JControl::handleMouseRelease(mx, my);
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
        JControl::handleMouseMove(mx, my);
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

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed || m_rows.empty()) return false;
        using K = JKeyEvent::JKey;

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

    bool hasVScroll(const JRect& b) const {
        float headerH = m_headerHeight;
        float rowH = m_rowHeight;
        float totalH = m_rows.size() * rowH + headerH;
        return totalH > b.height;
    }

    bool hasHScroll(const JRect& b) const {
        float totalColW = 0.0f;
        for (int i = 0; i < (int)m_headers.size(); ++i) {
            totalColW += columnWidth(i, b.width);
        }
        return totalColW > b.width;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
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



protected:
    virtual void drawHeaderCell(JPrimitiveBuffer& buf, int colIdx, const JRect& bounds, const std::string& title) {
        if (JTextHelper::hasAtlas()) {
            uint8_t tc[4] = {230, 230, 240, 255};
            float ty = bounds.y + (bounds.height - JTextHelper::lineHeight()) * 0.5f;
            JTextHelper::pushText(buf, bounds.x + m_cellPadding, ty, tr(title), tc, bounds.width - m_cellPadding * 2.0f);
        }
    }

    virtual void drawRowCell(JPrimitiveBuffer& buf, int rowIdx, int colIdx, const JRect& bounds, const std::string& val, bool selected) {
        if (JTextHelper::hasAtlas()) {
            uint8_t tc[4] = {200, 200, 210, 220};
            float ty = bounds.y + (bounds.height - JTextHelper::lineHeight()) * 0.5f;
            JTextHelper::pushText(buf, bounds.x + m_cellPadding, ty, tr(val), tc, bounds.width - m_cellPadding * 2.0f);
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

} // inline namespace jf
