# CLAUDE.md

## Project

A cross-platform GUI toolkit written from scratch. No Qt, no wxWidgets, no
GTK — none of it. Everything is ours. The `J` prefix is the toolkit namespace
marker (`JAppWindow`, `JButton`, `JTheme`).

## The two rules everything else derives from

**1. One class, one responsibility, one file.**
`JMenu` lives in `JMenu.h` / `JMenu.cpp`. Nothing else does. If a file
contains two classes, one of them is in the wrong place.

**2. Nothing is hardcoded. Everything comes from the theme.**
If you find yourself writing `height = 32`, stop. That is a theme value.
Title bar heights, padding, corner radii, font sizes, colors, borders,
scrollbar widths, focus ring thickness, animation durations — all of it
resolves through `JTheme` / `JStyle`. There are no exceptions to this.

Magic numbers are a bug, even when they look right.

## Composition, not duplication

Widgets are built from other widgets. If two things render the same visual
element, they use the same class to do it.

- `JAppWindow` renders its title bar via `JTitleBar`.
- `JDock` renders its title bar via `JTitleBar`. Same class. Same code path.
- `JToolBar` holds `JButton`s. It does not have its own button drawing code.
- `JMenu` items are widgets, not a bespoke item list.

Before writing any rendering code, search the tree for something that already
draws that element. Extend it if it needs to flex. Do not fork it.

If a shared widget needs to behave differently in two contexts, that
difference is a *style variant* on the theme, not a copy of the class.

## Layout

Widgets never position themselves. A layout manager positions them.

Layout code asks widgets for size hints (min / preferred / max) and assigns
geometry. Widgets expose their hints and paint into whatever rect they are
given. A widget that computes its own screen position is broken.

The rule is about *who owns the geometry*, not about whether positions are
computed. `JFixedLayout` is a legitimate layout manager: it assigns
explicit rects to its children. The distinction is that the coordinates live
in the layout, set by the caller — not baked into the widget's own paint or
resize code.

So this is fine:

    layout->place(button, {x, y, w, h});

And this is not, ever:

    void JButton::onResize() { m_rect.x = 10; }

Nested layouts must resolve without cyclic size queries. Sizing is one pass
down, one pass back.

Note that `JFixedLayout` opts out of theme-driven sizing by design — the
caller supplies the numbers. That is the one place raw dimensions are
allowed, because they are *data passed in*, not constants compiled into a
widget. Everything a widget draws for itself still comes from `JTheme`.
## Platform boundary

Platform-specific code lives behind an interface and stays there. Public
headers never leak `windows.h`, `X11`, `Cocoa`, or any platform type.

The core toolkit is written once. The backends implement a small surface:
window creation, event pump, input, and a paint target. If a platform detail
is bleeding into a widget file, that is the bug — fix the boundary, not the
widget.

## Subsystems

- `JTheme` / `JStyle` — the source of every visual constant. Widgets read
  from it. They never carry defaults of their own.
- `JLogging` — category-based logging. Every log call names a category. No
  raw `printf`, no `std::cout`, no ad-hoc streams.
- `JSerial` — serial communications. Independent of the widget tree; does not
  know GUI exists.

Subsystems are independent. `JSerial` must not include a widget header.
`JLogging` must not depend on the theme.

## Naming

- Classes: `JPascalCase`, always `J`-prefixed.
- Files: match the class exactly. `JToolBar` → `JToolBar.h`, `JToolBar.cpp`.
- One public class per header.

## What to do when adding a widget

1. Check whether an existing widget already does part of it. Reuse it.
2. Create `JThing.h` and `JThing.cpp`. Nothing else in those files.
3. Every visual constant it needs → add it to the theme. Read it from there.
4. Give it size hints. Let a layout manager place it.
5. Log through `JLogging` with a category.

## What will get a change rejected

- A literal dimension, color, or duration anywhere outside the theme.
- A second copy of drawing code that already exists elsewhere.
- Two public classes in one header.
- A platform type in a public header.
- A widget setting its own absolute position.
- A subsystem reaching into another subsystem it has no business knowing.

## Build

CMake + Ninja. The toolkit is header-only under `include/j/`; the platform
backend (`libj_platform.a`) and the tests/examples are compiled targets.

Configure once (only if `build/` is missing):

    cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=$HOME/jframework-sdk

Build the platform lib (the usual inner-loop target):

    cmake --build build --target j_platform

Build everything (lib + tests + examples):

    cmake --build build

Tests live in `src/tests/` and build as their own targets (`test_widgets`,
`test_style`, …). Run one directly from `build/`, e.g. `./build/test_widgets`,
or run the suite via `ctest --test-dir build`.

Because the widgets are self-contained headers, the fastest check for a single
header is a standalone syntax probe — it proves the header carries its own
includes and doesn't lean on a neighbour:

    printf '#include <j/core/JButton.h>\nint main(){}\n' > /tmp/p.cpp
    g++ -std=c++20 -Iinclude -Ithird_party -fsyntax-only /tmp/p.cpp

`-Ithird_party` is required (stb, pulled in transitively by `FontEngine.h`).

SDK install (what downstream apps compile against): headers are copied to
`$HOME/jframework-sdk/include/j/…` and `libj_platform.a` to
`$HOME/jframework-sdk/lib/`. Changing a public header means copying it to the
SDK; adding/removing a base-class virtual shifts the vtable, so rebuild
`j_platform` and copy the lib too before relinking a downstream app.

No codegen steps.
