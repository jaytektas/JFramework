# 07. Cascading Style Engine

The Genesis Styling Engine provides a unified way to manage visual properties (Colors, Padding, Borders, Fonts) across the UI tree, supporting global themes with granular local overrides.

## Data Structure: Sparse Property Storage
Unlike traditional toolkits that store every possible property in every widget (bloating memory), Genesis uses a **Sparse Property Store**.
- Each `Control` maintains a small, sorted `std::vector` or a highly-optimized hash map of `PropertyOverride` objects.
- If a property (e.g., `AccentColor`) is not found locally, the engine traverses the Scene Graph parentage.

## The Cascading Context Tree
The "Cascade" behaves similarly to CSS but is optimized for binary execution and type safety.

1. **Global Context (Root):** Defines the base theme (e.g., "Deep Sea Dark").
2. **Sub-Tree Context:** A container (like a `Panel`) can inject a new `StyleContext`, changing the theme for all its children (e.g., a "Light Mode" preview pane inside a "Dark Mode" app).
3. **Local Override:** A specific `Button` can override `BorderWidth` directly.

## Change Propagation: The Invalidation Vector
To avoid O(N) re-evaluation of the entire tree when a theme changes:
- **Dirty Bitmask:** Each `Control` has a `StyleDirty` flag.
- **Dependency Tracking:** When a property is looked up, the node registers a dependency on that property key.
- **Propagation:** When `GlobalContext.AccentColor` changes, it broadcasts an invalidation signal. Only nodes that have registered a dependency on `AccentColor` or its ancestors are marked for re-evaluation in the next layout/paint pass.

## Type-Safe Style Keys
Instead of string lookups (`style["color"]`), Genesis uses compile-time constants:

```cpp
namespace Style {
    constexpr PropertyKey<Color> BackgroundColor{ 0x01 };
    constexpr PropertyKey<float> CornerRadius{ 0x02 };
}

// Usage
myButton.SetStyle(Style::BackgroundColor, Color::Red);
Color c = myButton.GetStyle(Style::BackgroundColor); // Returns Red (local) or Parent (cascade)
```

## Architectural Diagram
```
[Global Style Context] ──► [Pointer to Base Theme Data]
        │
        ▼
[Scene Graph Node] ─────► [Local Overrides Map] ──┐
        │                                         │
        ├─ Lookup(Key) ◄──────────────────────────┘
        │    (If not found, Recurse Parent)
        ▼
[Parent Node] ...
```

## AI Integration
The AI Control Bus can inject temporary `StyleContexts` to highlight elements or "hallucinate" UI modifications for the user, effectively allowing the AI to re-skin the application in real-time based on context.
