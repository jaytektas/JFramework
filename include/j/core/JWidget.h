#pragma once

// JWidget — base for every element in the UI tree. Extracted verbatim from BaseWidgets.h.

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
#include "JPropertyModel.h"
#include "AccessibilityBridge.h"
#include "SceneGraph.h"
#include "TranslationEngine.h"
#include "KeyEvent.h"
#include "DragDrop.h"
#include "JStyle.h"          // JWidgetState / JFocusPolicy / JStyle / Colors (pulls Style/JStyleEngine/VectorGraphics)
#include "../graphics/RenderPrimitive.h"
#include "../platform/Clipboard.h"

inline namespace jf {

// JWidgetState / JFocusPolicy moved to JStyle.h (included above).

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

    // Accessibility change-notification hook — installed by the framework (e.g. the
    // JAccessibilityBridge) to hear when a widget's value/state/focus changes so an AT
    // client (Orca) is told to re-read it. Widgets call notifyAccessibility() at their
    // change sites; unset (headless / no AT running) → a no-op. Mirrors s_focusHook.
    inline static std::function<void(JWidget*)> s_a11yNotifyHook;
    void notifyAccessibility() { if (s_a11yNotifyHook) s_a11yNotifyHook(this); }
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

    // ------------------------------------------------------------------
    // Base widget signals (the QObject/QWidget analog). Signals live on the BASE widget, so EVERY widget —
    // not just JControl — can emit change notifications, and JSignal routes them to any receiver widget
    // (JWidget is a JSlotTracker). Fired by the mutators below; onModified is the generic CanvasElement
    // modified() analog a subclass raises when one of its own properties changes (call emitModified()).
    // ------------------------------------------------------------------
    jf::JSignal<>     onModified;           // a property/state changed
    jf::JSignal<>     onGeometryChanged;    // bounds / pos / size changed
    jf::JSignal<bool> onVisibilityChanged;  // shown (true) / hidden (false)
    jf::JSignal<bool> onFocusChanged;       // focus gained (true) / lost (false)
    jf::JSignal<bool> onEnabledChanged;     // enabled (true) / disabled (false)
    void emitModified() { onModified.emit(); }

    void setVisible(bool v) { if (m_visible == v) return; m_visible = v; onVisibilityChanged.emit(v); }

    // Position/size this widget's layout box directly — for host-driven placement (e.g. a dock
    // laying its content widget into the leaf's content rect). Marks the node dirty so the
    // subtree re-lays-out; container widgets (JScrollArea/JGroupBox/…) arrange children from it.
    void setBounds(const JRect& r) {
        JRect c = r;
        c.width  = _clampW(c.width);
        c.height = _clampH(c.height);
        m_graph.getLayout(m_nodeId).boundingBox = c;
        onGeometryChanged.emit();
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
        onGeometryChanged.emit();
    }
    void setSize(float w, float h) {
        auto& bb = m_graph.getLayout(m_nodeId).boundingBox;
        bb.width  = _clampW(w);
        bb.height = _clampH(h);
        onGeometryChanged.emit();
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
        auto& l = m_graph.getLayout(m_nodeId);
        l.maxWidth = w; l.maxHeight = h;                 // let the layout pass honour the ceiling too
        setSize(l.boundingBox.width, l.boundingBox.height);
    }
    std::pair<float,float> minimumSize() const { return { m_minW, m_minH }; }
    std::pair<float,float> maximumSize() const { return { m_maxW, m_maxH }; }

    // ------------------------------------------------------------------
    // Size policy — how this widget negotiates for space along each axis when its parent's
    // flex/box pass has slack or a deficit to hand out. Default is Preferred/0 (original
    // behaviour). Pass stretch < 0 to leave the existing stretch factor untouched.
    // ------------------------------------------------------------------
    void setHSizePolicy(JSizePolicyMode m, int stretch = -1) {
        auto& l = m_graph.getLayout(m_nodeId);
        l.hPolicy.mode = m; if (stretch >= 0) l.hPolicy.stretch = stretch;
    }
    void setVSizePolicy(JSizePolicyMode m, int stretch = -1) {
        auto& l = m_graph.getLayout(m_nodeId);
        l.vPolicy.mode = m; if (stretch >= 0) l.vPolicy.stretch = stretch;
    }
    void setSizePolicy(JSizePolicyMode h, JSizePolicyMode v) { setHSizePolicy(h); setVSizePolicy(v); }
    // Set the stretch factor on both axes (weights how expanding space is split against siblings).
    void setStretch(int factor) {
        auto& l = m_graph.getLayout(m_nodeId);
        l.hPolicy.stretch = factor; l.vPolicy.stretch = factor;
    }
    JSizePolicy hSizePolicy() const { return m_graph.getLayoutConst(m_nodeId).hPolicy; }
    JSizePolicy vSizePolicy() const { return m_graph.getLayoutConst(m_nodeId).vPolicy; }

    // Placement inside an over-sized layout cell (grid column / opted-out box child).
    void       setCellAlign(JCellAlign a) { m_graph.getLayout(m_nodeId).cellAlign = a; }
    JCellAlign cellAlign() const          { return m_graph.getLayoutConst(m_nodeId).cellAlign; }

    // Pin the widget to an exact size: min == max == the given size, and clamp now.
    void setFixedSize(float w, float h) {
        m_minW = m_maxW = w;
        m_minH = m_maxH = h;
        auto& l = m_graph.getLayout(m_nodeId);
        l.minWidth = w; l.minHeight = h;
        l.maxWidth = w; l.maxHeight = h;            // pin the layout pass too
        l.hPolicy.mode = JSizePolicyMode::Fixed;
        l.vPolicy.mode = JSizePolicyMode::Fixed;
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
        const bool was = (m_state != JWidgetState::Disabled);
        setState(e ? JWidgetState::Normal : JWidgetState::Disabled);
        if (was != e) onEnabledChanged.emit(e);
    }
    void setFocused(bool f) {
        if (m_focused != f) {
            m_focused = f;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            notifyAccessibility();
            onFocusEvent(f);       // deterministic blur/focus hook — e.g. spin boxes commit their edit here
            onFocusChanged.emit(f); // routed signal — external listeners (any receiver widget) observe focus
        }
    }

    // Called the instant focus is gained (true) or lost (false), synchronously from setFocused —
    // before any repaint or panel rebuild. Widgets that buffer edits (spin boxes, line edits)
    // override this to commit on blur so a value typed then Tab/click-away is never dropped.
    // (This is the virtual HOOK; the onFocusChanged JSignal above is the external notification — both fire.)
    virtual void onFocusEvent(bool focused) { (void)focused; }

    // Schedule a repaint for this widget.
    void invalidate() { m_graph.invalidateNode(m_nodeId, DirtySelf); }

    virtual void setState(JWidgetState s) {
        if (m_state != s) {
            m_state = s;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            notifyAccessibility();
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

    // --- General drag & drop hooks (DragDrop.h; jDragTick routes to these) ---
    // Default: accept nothing / no-op. A drop target overrides canDrop() to vet
    // the payload's formats and onDrop() to consume it. Only entered when
    // canDrop()==true, so a plain widget never sees onDragEnter/onDrop.
    virtual bool canDrop(const JMimeData&) const { return false; }
    virtual void onDragEnter(JDragSession&) {}
    virtual void onDragMove (JDragSession&) {}
    virtual void onDragLeave(JDragSession&) {}
    virtual bool onDrop     (JDragSession&) { return false; }
    // Begin dragging FROM this widget with the given payload and permitted action.
    virtual void startDrag(JMimeData mime, JDropAction supported) {
        jf::jBeginDrag(this, std::move(mime), supported);
    }

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

    // ------------------------------------------------------------------
    // Introspective property model — the QWidget/Q_PROPERTY analog (minus moc).
    // A flat, editable set of named JVariant properties a generic property editor
    // enumerates and writes back through. Built lazily on first access and cached;
    // each subclass overrides collectProperties(), chains to its base, and registers
    // its own members. Distinct from getRef() above (which resolves reference PATHS
    // for bindings/the AI bus) and from JPropertyBag (opt-in app-level dynamic
    // runtime properties). See JPropertyModel.h.
    // ------------------------------------------------------------------
    JPropertyModel& properties() {
        if (!m_propertiesBuilt) { collectProperties(m_propertyModel); m_propertiesBuilt = true; }
        return m_propertyModel;
    }

    // ------------------------------------------------------------------
    // Accessibility — the semantic snapshot an AT client reads. Every
    // interactive widget overrides a11yNode() to report its role, accessible
    // name, current value and live state. The default derives a generic
    // "widget" role and takes its name from the debug name.
    // ------------------------------------------------------------------
    virtual JA11yNode a11yNode() const {
        JA11yNode n;
        _a11yFillCommon(n, JA11yRole::Widget, m_debugName, "");
        return n;
    }

protected:
    // Register this widget's editable properties into the model. The base
    // contributes the universal, most-edited set — identity, behaviour, and
    // geometry. A subclass overrides this, calls JWidget::collectProperties(m)
    // first, then adds its own typed members. Called once, lazily, by properties().
    virtual void collectProperties(JPropertyModel& m) {
        m.add("name",    this, &JWidget::m_debugName, JPropertyMeta{.category = "General"});
        m.add("enabled", this, &JWidget::isEnabled, &JWidget::setEnabled, JPropertyMeta{.category = "General"});
        m.add("visible", this, &JWidget::isVisible, &JWidget::setVisible, JPropertyMeta{.category = "General"});
        m.add("tooltip", this, &JWidget::tooltip,   &JWidget::setTooltip, JPropertyMeta{.category = "General"});
        m.add("opacity", this, &JWidget::opacity,   &JWidget::setOpacity,
              JPropertyMeta{.category = "General", .min = 0.0, .max = 1.0, .step = 0.05});

        // Geometry — the caller/layout owns these coordinates (edits route through
        // setBounds, which honours the size constraints), so exposing them here does
        // not violate the "widgets never position themselves" rule.
        m.add(JProperty{ .name = "x",
            .get = [this] { return JVariant(bounds().x); },
            .set = [this](const JVariant& v) { JRect b = bounds(); b.x = static_cast<float>(v.toDouble(b.x)); setBounds(b); return true; },
            .meta = JPropertyMeta{.category = "Geometry"} });
        m.add(JProperty{ .name = "y",
            .get = [this] { return JVariant(bounds().y); },
            .set = [this](const JVariant& v) { JRect b = bounds(); b.y = static_cast<float>(v.toDouble(b.y)); setBounds(b); return true; },
            .meta = JPropertyMeta{.category = "Geometry"} });
        m.add(JProperty{ .name = "width",
            .get = [this] { return JVariant(bounds().width); },
            .set = [this](const JVariant& v) { JRect b = bounds(); b.width = static_cast<float>(v.toDouble(b.width)); setBounds(b); return true; },
            .meta = JPropertyMeta{.category = "Geometry", .min = 0.0} });
        m.add(JProperty{ .name = "height",
            .get = [this] { return JVariant(bounds().height); },
            .set = [this](const JVariant& v) { JRect b = bounds(); b.height = static_cast<float>(v.toDouble(b.height)); setBounds(b); return true; },
            .meta = JPropertyMeta{.category = "Geometry", .min = 0.0} });
    }

    // Populate id / role / name / value / bounds and the universal state bits
    // (focusable, focused, disabled, pressed) shared by every widget. Overrides
    // call this, then OR in role-specific bits (Checked/Selected/Editable/...).
    void _a11yFillCommon(JA11yNode& n, JA11yRole role,
                         const std::string& name, const std::string& value) const {
        n.id     = static_cast<uint32_t>(m_nodeId);
        n.roleId = role;
        jA11yCopyStr(n.role,  sizeof(n.role),  jA11yRoleName(role));
        jA11yCopyStr(n.name,  sizeof(n.name),  name);
        jA11yCopyStr(n.value, sizeof(n.value), value);
        const JBBox b = getBoundingBox();
        n.x = b.x; n.y = b.y; n.width = b.width; n.height = b.height;
        uint32_t s = 0;
        if (isFocusable())               s |= JA11yFocusable;
        if (isFocused())                 s |= JA11yFocused;
        if (!isEnabled())                s |= JA11yDisabled;
        if (m_state == JWidgetState::Pressed) s |= JA11yPressed;
        n.stateFlags = s;
    }

    bool isPointInside(float mx, float my) const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        return mx >= b.x && mx <= b.x + b.width && my >= b.y && my <= b.y + b.height;
    }

    // Clamp a requested width/height into [min,max].
    float _clampW(float w) const { return std::clamp(w, m_minW, m_maxW); }
    float _clampH(float h) const { return std::clamp(h, m_minH, m_maxH); }

    // Call at the end of populateRenderPrimitives to draw a keyboard-focus ring.
    // Defined out-of-line (after JStyle) — see below.
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

    JPropertyModel m_propertyModel;                     // introspective/editable property surface (lazy)
    bool           m_propertiesBuilt{false};            // collectProperties() run yet?
};

} // inline namespace jf
