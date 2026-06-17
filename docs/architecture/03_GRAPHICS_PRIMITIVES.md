# 03. Graphics Rendering & Primitives

In keeping with the strict mandate of **no copied code**, we do not rely on Skia, Cairo, or even standard hardware wrappers like bgfx. We build the rendering pipeline from scratch.

## The Rendering Philosophy
All rendering is performed via a custom intermediate representation (IR) that sits directly on top of the OS's native GPU interfaces:
*   **Linux:** Vulkan / DRI 
*   **Windows:** DirectX 12 
*   **macOS:** Metal

## Step-by-Step Rendering Pipeline

### 1. Primitive Generation
Instead of immediate mode drawing (`drawLine`, `drawRect`), the UI layers output **Primitive Commands**.
```cpp
struct Primitive {
    uint32_t type; // e.g., Rect, RoundedRect, Bezier, Glyph
    Vec4 color;
    Mat4 transform;
    Vec4 parameters; // e.g., Corner radii, stroke width
};
```
### 2. Vector Tessellation (CPU/GPU)
Complex shapes (paths, text glyphs) must be reduced to triangles.
- **Static geometry** is tessellated on the CPU in background threads using an in-house sweep-line algorithm and uploaded to GPU buffers.
- **Dynamic geometry** (like animating rounded corners or scaling bezier curves) is passed as raw data to GPU Fragment Shaders, utilizing mathematically exact Signed Distance Fields (SDFs).

### 3. Bespoke Font Rendering
We implement a custom OpenType/TrueType parsing engine.
- Reads `cmap`, `glyf`, and `kern` tables manually.
- Extracts bezier contours and computes exact bounding boxes.
- Text is rendered via multi-channel Signed Distance Fields (MSDF) generated at runtime, allowing infinite scaling, sub-pixel positioning, and AI-driven font manipulation without rasterization artifacts.

### 4. Compositing and Z-Ordering
The engine uses a painter's algorithm combined with Z-buffering to allow for complex overlapping UI elements, drop shadows, and acrylic/blur effects.
- Render targets are heavily cached. A static window does not redraw; it only presents its final frame buffer.

### 5. Multi-Threading Matrix
- **Main Thread:** Handles App State and Event Dispatch.
- **Layout Thread:** Computes flex grids and constraints.
- **Render Preparation Thread:** Flattens the Scene Graph into linear Primitive Commands.
- **GPU Thread:** Sole owner of the Vulkan/Metal device. Translates Primitive Commands into Command Buffers and submits to the queue.
