# 04. Controls & Widget Catalog

Genesis replaces Qt widgets with a zero-dependency, data-oriented alternative.
Every widget is a C++ class that writes into a `PrimitiveBuffer`; there is no
retained-mode scene, no virtual DOM, and no MOC.  All widgets inherit the AI
semantic interface automatically.

---

## Status key
| Symbol | Meaning |
|--------|---------|
| ✅ | Implemented with SDF fallback rendering |
| 🔜 | Planned — spec complete |
| 📋 | On roadmap |

---

## Base Classes

| Class | Status | Notes |
|-------|--------|-------|
| `Widget` | ✅ | Root base. Owns `NodeId`, `WidgetState`, virtual mouse handlers, `populateRenderPrimitives` |
| `Control` | ✅ | Adds hover/press/click signals, `isPointInside`, enabled/disabled routing |
| `IAIState` | ✅ | Forces `getSemanticNode()` + `executeSemanticAction()` on every widget |

## Layout Primitives

| Class | Status | Notes |
|-------|--------|-------|
| `SceneGraph` | ✅ | Flat array + invalidation bitmask. `computeLayout` respects `padding` and `gap` |
| `FlexDirection` | ✅ | `Row` / `Column` on every `LayoutComponent` |
| `Constraints` | ✅ | `{minW, maxW, minH, maxH}` propagated down the tree |

## Structural

| Class | Status | Notes |
|-------|--------|-------|
| `Separator` | ✅ | Horizontal or vertical 1px rule, configurable length |
| `GroupBox` | ✅ | Labelled container panel with title bar; children laid out via SceneGraph |

## Basic Controls

| Class | Status | Notes |
|-------|--------|-------|
| `Label` | ✅ | Faux glyph bars until `FontEngine` lands; size configurable |
| `Button` | ✅ | Hover / press state, accent colour on press, AI `click` action |
| `ToggleButton` | ✅ | Latches on/off, emits `onToggled(bool)` |
| `CheckBox` | ✅ | Blue fill + cross-tick mark when checked |
| `RadioButton` | ✅ | SDF circle + dot; use `setSelected(true)` from group logic |

## Input Controls

| Class | Status | Notes |
|-------|--------|-------|
| `LineEdit` | ✅ | Focused border (accent), cursor line, placeholder bars |
| `SpinBox` | ✅ | Value field + up/down chrome; `setValue`, `increment`, `decrement` AI actions |
| `ComboBox` | ✅ | Cycles items on click; `select:<label>` AI action |
| `TextArea` | ✅ | Multi-line text input with custom SDF cursor, scroll offsets, key navigation, and text actions |

## Range & Feedback

| Class | Status | Notes |
|-------|--------|-------|
| `Slider` | ✅ | Filled track + draggable thumb; emits `onValueChanged(float)` |
| `ProgressBar` | ✅ | Deterministic fill; animated in catalog demo |
| `ScrollBar` | ✅ | Track + proportional thumb; `setScrollPosition(0..1)` |
| `Dial/Knob` | 📋 | Radial SDF rendering planned |

## Navigation

| Class | Status | Notes |
|-------|--------|-------|
| `TabBar` | ✅ | N equal-width tabs, accent underline on active |
| `TreeView` | 🔜 | Collapse/expand, virtualised |
| `ListView` | ✅ | Virtualised vertical scrolling list with selection, active item signals, and arrow navigation |

## Future

- `Canvas` — immediate-mode drawing surface
- `Image` — bitmap decode + display (PNG/QOI target)
- `TableView` — sortable, virtualized 2D grid
- `ScrollArea` — ✅ Wraps a collection of widgets with a scrollable viewport and scissor clipping
- `Tooltip` — hover-triggered overlay
- `ContextMenu` — right-click overlay list

---

## AI Semantic Interface

Every widget implements:

```cpp
struct AISemanticNode {
    std::string role;         // "Button", "Slider", "CheckBox", ...
    std::string label;        // human-readable identifier
    std::string value;        // current value as string
    bool        interactable;
};

virtual AISemanticNode getSemanticNode() const = 0;
virtual bool executeSemanticAction(const std::string& action) = 0;
```

Standard action strings:

| Widget | Action strings |
|--------|---------------|
| Button | `"click"` |
| ToggleButton | `"toggle"` |
| CheckBox | `"check"`, `"uncheck"` |
| RadioButton | `"select"` |
| Slider | `"set_value:0.75"` |
| SpinBox | `"set_value:42"`, `"increment"`, `"decrement"` |
| ComboBox | `"select:1440p"` |
| TabBar | `"select_tab:2"` |
| LineEdit | `"set_text:hello"` |

The AI Control Bus polls this interface every frame and exposes the full
semantic tree to any connected AI agent without any extra instrumentation.
