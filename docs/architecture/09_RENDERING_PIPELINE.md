# 09. Rendering Pipeline

Genesis uses a **CPU-side SDF primitive buffer → GPU draw calls** model.
There are no retained scene graphs on the GPU, no material systems, and no
pre-built mesh libraries.  Each frame is built from scratch.

---

## Data flow

```
Widget tree
    │  populateRenderPrimitives(PrimitiveBuffer&)
    ▼
PrimitiveBuffer               ← CPU, rebuilt every frame
    │  instances: [GpuPrimitiveInstance × N]
    │
    ▼  GpuHal::drawPrimitives(buffer)
VulkanGpuHal
    ├─ vkCmdBindPipeline        (SDF rect pipeline, once per frame)
    ├─ vkCmdSetViewport/Scissor (dynamic state)
    └─ for each instance:
           vkCmdPushConstants   (48 bytes: bounds, colours, radius, border, viewport)
           vkCmdDraw(6, 1, …)   (two triangles, no vertex buffer)
```

---

## GpuPrimitiveInstance (push constant layout, 48 bytes, std430)

| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0 | `rectBounds` | `vec4` | x, y, w, h in screen-space pixels |
| 16 | `colorPacked` | `uint` | RGBA8 little-endian fill colour |
| 20 | `borderColorPacked` | `uint` | RGBA8 little-endian border colour |
| 24 | `borderRadius` | `float` | Corner radius in pixels |
| 28 | `borderWidth` | `float` | Inset stroke width |
| 32 | `primitiveType` | `uint` | `PrimitiveType` enum |
| 36 | `padding[0]` | `float` | Viewport width (baked in at draw time) |
| 40 | `padding[1]` | `float` | Viewport height (baked in at draw time) |
| 44 | `padding[2]` | `float` | Reserved |

The Vulkan spec guarantees ≥ 128 bytes of push constant space; we use 48.

---

## Shaders (`src/shaders/`)

### rect.vert
- Generates a two-triangle quad from `gl_VertexIndex` (0-5) — **no vertex buffer**.
- Reads `rectBounds` and viewport dimensions from push constants.
- Outputs `fragLocalPos` (pixel offset within the rect) for the SDF.

### rect.frag
- Evaluates `sdRoundedBox(center, halfExtent, radius)`.
- Anti-aliases the shape edge with `smoothstep(-0.5, 0.5, d)`.
- Blends an inset border when `borderWidth > 0`.
- Discards fully-outside fragments (`d > 0.5`).
- Alpha output → standard `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` pipeline blend.

### Shader build pipeline
Shaders are compiled by the `genesis_shaders` CMake target:

```
src/shaders/rect.vert  ─glslc→  build/shaders/rect.vert.spv
src/shaders/rect.frag  ─glslc→  build/shaders/rect.frag.spv
                                        │
                               cmake/embed_spv.py
                                        │
                         include/genesis/graphics/ShaderSpirv.h
                              (constexpr uint32_t arrays)
```

`genesis_platform` depends on `genesis_shaders`, so shaders are always
recompiled before the library when sources change.

---

## Pipeline state (fixed per session)

| State | Value |
|-------|-------|
| Topology | `TRIANGLE_LIST` |
| Vertex input | None (procedural) |
| Cull mode | `NONE` |
| Depth test | Disabled |
| Blend | `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` |
| Viewport/Scissor | Dynamic |
| Format | `VK_FORMAT_B8G8R8A8_UNORM` |
| Present mode | `FIFO` (V-Sync) |

---

## Platform surface

| Platform | API | Extension |
|----------|-----|-----------|
| Linux | XCB | `VK_KHR_xcb_surface` |
| Windows | Win32 | `VK_KHR_win32_surface` |

The `LinuxPlatformWindow` header (`include/genesis/platforms/linux/`) is
the concrete implementation for Linux.  It exposes `nativeConnection()` /
`nativeWindow()` for Vulkan surface creation, plus consume-once mouse state
accessors for the render loop.

---

## Roadmap

- **Vertex + index buffer upload** — needed when primitive count exceeds
  push constant limits or when GPU instancing is added.
- **Font rendering** — glyph rasterisation into an atlas texture, with
  SDF text draw calls using a separate text pipeline.
- **Image / texture pipeline** — VkImage upload + descriptor set for
  bitmap widgets.
- **Off-screen render targets** — for blur, shadow, and compositing effects.
