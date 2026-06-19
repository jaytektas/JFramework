# 11. Layout Engine (Flexbox)

`SceneGraph::computeLayout` is a small flexbox-style solver over the flat, array-backed
scene graph. It replaces Qt's `QLayout` hierarchy with a data-oriented, invalidation-driven
pass — no per-widget layout objects, no virtual dispatch.

## Model

Every node carries a `LayoutComponent`:

| field | meaning |
|---|---|
| `direction` | `Row` or `Column` (main axis) |
| `justifyContent` | main-axis distribution: `FlexStart` / `Center` / `FlexEnd` / `SpaceBetween` / `SpaceAround` |
| `alignItems` | cross-axis: `Start` / `Center` / `End` / `Stretch` |
| `alignSelf` | per-child override of the parent's `alignItems` (`-1` = inherit) |
| `flexGrow` | share of leftover main-axis space this child claims |
| `padding`, `margin` | `Edges` (per-side; assigning a single `float` means "all sides") |
| `gap` | space inserted between consecutive children |
| `boundingBox` | the computed output rect |

## Two-phase pass

`computeLayout(node, constraints)` runs:

1. **Measure** (bottom-up) — size every node from its children. A child is measured at its
   intrinsic size clamped to the available space, then **flexGrow** distributes any leftover
   main-axis space, and **Stretch** fills the cross axis. Grown/stretched containers are
   re-measured so their own children reflow.
2. **Arrange** (top-down) — position each node from its parent's final origin, applying
   `justifyContent` (main-axis offset/spacing), `alignItems`/`alignSelf` (cross-axis), and
   per-side margins.

Splitting measure from arrange is what makes **nested** layouts position correctly (a
grandchild inherits all ancestor paddings/offsets). Sizes use a non-asserting `clampF` so a
degenerate (zero/negative) container can never abort the app.

## Invalidation

`invalidateNode` marks a node `DirtySelf` and walks `DirtyChildren` up to the root, so
`computeLayout` skips clean subtrees. `getLayout()` invalidates on access; `getLayoutConst()`
does not.

## Global defaults with local override

`SceneGraph::defaultLayout()` returns the `LayoutComponent` every **new** node is cloned from
— set app-wide spacing/alignment once at startup, then override any individual node locally
via `getLayout(id)`. (Global-with-local-override, the project's standard.)

## Clipping & scrolling

The render side complements layout: `PrimitiveBuffer::pushClip/popClip` scope draws to a
rectangle (nested clips intersect), emitted as `vkCmdSetScissor` per batch in the HAL. A
container taller than its viewport is shown clipped and scrolled by offsetting its laid-out
origin — this is how the controls catalog turns each dock panel into a scrollable view (see
`ControlsCatalog::updateDockContent`).

## Example

```cpp
auto& root = graph.getLayout(panelRoot);
root.direction  = FlexDirection::Column;
root.padding    = 14.0f;          // Edges{14,14,14,14}
root.gap        = 8.0f;
root.alignItems = AlignItems::Stretch;   // children fill the panel width
// a child that should fill leftover vertical space:
graph.getLayout(spacerNode).flexGrow = 1.0f;
graph.computeLayout(panelRoot, Constraints{areaW, areaW, 0.f, 100000.f});
```
