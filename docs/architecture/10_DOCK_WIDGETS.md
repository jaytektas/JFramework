# 10. Dock Widgets & Tear-Off Tabs

Genesis dock widgets replace Qt's `QDockWidget` + `QTabWidget::setTabsClosable` +
`QTabBar::setMovable` with a single, composition-based design.

---

## Design principles

- **No inheritance between `DockWidget` and `TabBar`** — they are orthogonal concepts
  connected only through the `TornTabState` value type.
- **`DockWidget` is not a `Widget`** — it floats at absolute screen coordinates and is
  independent of the SceneGraph layout engine.  This avoids position conflicts between the
  layout tree and a freely draggable panel.
- **`TornTabState` is a plain value type** — cheap to move, carries a `std::function`
  reattach closure so the DockZone can re-insert a tab without knowing anything about the
  bar's internal structure.
- **Composition over subclassing** — a DockWidget *optionally* carries `TornTabState`
  (via `std::optional`).  A dock created fresh has none; a torn-off tab carries one.
  This keeps the class small and the re-dock path explicit.

---

## File layout

```
include/genesis/core/
    BaseWidgets.h   — TornTabState struct + tearable TabBar extensions
    DockWidget.h    — DockZone + DockWidget
```

---

## Data model

```
TornTabState (value type)
    std::string  title
    int          originIndex     // insertion slot on re-dock
    NodeId       contentNode     // SceneGraph subtree to move
    function<void(int,NodeId)> reattach   // closure bound to origin TabBar
```

```
DockWidget
    float x, y, w, h            // absolute screen position
    std::string title
    std::optional<TornTabState>  // present only for torn-off tabs
    bool pinned / closeRequested / dragging
    titleBar drag → update x, y
    close btn  → closeRequested = true
    pin btn    → pinned = !pinned
```

```
DockZone
    Rect bounds                  // typically covers the origin TabBar
    function<void(TornTabState&&)> onDrop
    updateDrag(curX, curY, isDragging)   // highlight when cursor is inside
    tryDrop(releaseX, releaseY, state)   // fires onDrop, returns true if hit
```

---

## Tear-off lifecycle

```
1. TabBar::setTearable(true)            (opt in per bar)
   TabBar::setTabContentNode(i, nodeId) (associate SceneGraph content)

2. User presses a tab and drags vertically > 16 px
   → TabBar::handleMouseMove detects threshold
   → _emitTear(): removes tab from bar, populates m_pendingTorn

3. App render loop calls:
   if (tabBar.hasTornTab()) {
       auto state = tabBar.consumeTornTab();
       m_docks.emplace_back(std::move(state), dragX - 160, dragY - 15);
   }
   tabBar.populateDragGhost(buf);   // ghost tab follows cursor

4. DockWidget appears at cursor position
   User drags title bar to position it

5a. User closes dock   → dock.closeRequested() == true → erase from list
5b. User re-docks:
       DockZone::updateDrag(mx, my, isDragging)
       on release: DockZone::tryDrop(mx, my, dock.tornState())
           → calls state.reattach(originIndex, contentNode)
           → TabBar::reinsertTab inserts tab at original slot
           → DockWidget erased from list
```

---

## Rendering order (each frame)

```
1. Layout widgets           (SceneGraph-positioned content)
2. tabBar.populateDragGhost (semi-transparent tab following cursor during drag)
3. dockZone.populateRenderPrimitives  (highlight when drop is possible)
4. for each dock: dock.populateRenderPrimitives   (floating, always on top)
```

DockWidgets render last so they always appear above all layout content.

---

## DockWidget chrome (SDF fallback rendering)

| Layer | Shape | Notes |
|-------|-------|-------|
| Shadow | offset dark rect | 4px offset, radius 8 |
| Body | dark rect, 8px radius, 1px border | #16161A background |
| Title bar | Surface2 rect (pinned: AccentPress) | 30px tall |
| Title hint | two text-bar rects | mimics label text |
| Pin button | 16×16 rect + vertical needle | blue when pinned |
| Close button | 16×16 rect + cross (two perp. rects) | red on hover |
| Content area | near-black rect | content widget renders here |
| Re-dock badge | small accent rect at bottom | visible only for torn tabs |
| Resize handle | 8×8 accent rect, bottom-right | resize not yet implemented |

---

## Snap to window edges

`DockWidget::snapToWindow(windowW, windowH)` — call on mouse-release.
Snaps the dock flush to any window edge within `SNAP_DIST` (14 px).

---

## Panel content

A dock panel is just a frame; its *content* is supplied by the application. In the controls
catalog each panel owns a flex-column container of widgets (`ControlsCatalog::Panel`). Every
frame, `updateDockContent` places that container at the panel's content area, lays it out at
the panel width (controls **stretch**), **clips** it to the panel, and applies wheel
**scroll** for overflow — see [11_LAYOUT_ENGINE.md](11_LAYOUT_ENGINE.md). When a panel is
floated, the same content is rendered and driven inside the floating window via
`FloatingDockWindow`'s content render/input callbacks, so it stays fully functional detached.

Per-dock constraints (`setFloatable`/`setTabifiable`, `setAllowedDrops`, leaf
`DockAffinityRule`, accept/reject leaf labels, min/max size) are enforced by the host on every
drop. The demo keeps them permissive for free-form docking, but the API is unchanged.

## Roadmap

- **Floating dock hosts (nesting)** — make every `FloatingDockWindow` a full `DockHost`
  registered with `DockRegistry`, so docks can be dragged *into* a float to tabify/split
  (honoring each dock's options) and `return-to-dock-on-close` falls out naturally. This
  unifies main-window and floating docking into one symmetric model.
- **Resize handles** — drag bottom/right/corner to resize the dock
- **Persistence** — serialize dock positions/sizes to/from JSON layout file
