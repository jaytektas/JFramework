# 05. Development Roadmap

Building a zero-dependency, cross-platform UI framework from scratch requires strict staging. We cannot build a button until we can render a quad, and we cannot render a quad until we have a window.

## Phase 1: Bare Metal & Primitives
**Goal:** Prove OS abstraction and GPU initialization.
1. **SAL (System Abstraction Layer):** 
   - Write memory allocators.
   - Implement `PlatformWindow` for Linux (XCB/Wayland initially) and Windows (Win32).
   - Hook into raw OS input events.
2. **GPU Bootstrapping:**
   - Initialize Vulkan instance, physical device, and swapchain.
   - Set up the Command Pool and render pass structures.
3. **Math & Math Primitives:**
   - Implement `Vec2`, `Vec3`, `Mat4`.
   - Implement SIMD matrix multiplication.
4. **First Triangle:**
   - Push a static vertex buffer to the GPU and render a colored triangle using a hardcoded SPIR-V shader.

## Phase 2: Vector Graphics & Fonts
**Goal:** Be able to draw complex 2D shapes and text.
1. **The Shape Engine:**
   - Implement CPU-side tessellation for Quads, Circles, and Rounded Rectangles.
   - Upload dynamic vertex buffers.
2. **Bespoke Font Parser:**
   - Parse TrueType `cmap` and `glyf` tables.
   - Implement Bezier curve evaluation.
   - Generate MSDF (Multi-channel Signed Distance Field) textures for font atlases.
3. **Text Rendering:**
   - Write the MSDF fragment shader.
   - Implement basic string layout (kerning, line-breaks).

## Phase 3: The AI Control Bus & Data Models
**Goal:** Define the internal state before building visual widgets.
1. **Scene Graph & DOD:**
   - Implement the Data-Oriented tree structure for UI nodes.
2. **AI Memory Interface:**
   - Setup lock-free ring buffers for the `AICB` (AI Control Bus).
   - Define the `SemanticNode` schema for serialized state reading.
3. **Layout Engine:**
   - Implement a multi-threaded Flexbox-equivalent constraint solver.

## Phase 4: Core Controls & User Interaction
**Goal:** Interactive visual elements.
1. **Event Dispatcher:**
   - Route raw OS inputs to the Scene Graph.
   - Implement hit-testing and Z-order sorting.
2. **Base Widgets:**
   - Implement `Button`, `TextInput`, `Slider`, `CheckBox`.
3. **Focus & Accessibility: [Done]**
   - Implement keyboard tabbing and focus rings (SDF focus borders and FocusManager synchronization are complete).

## Phase 5: Complex Applications & Polish
**Goal:** Capable of building production software.
1. **Advanced Widgets:**
   - `TreeView`, `DataGrid`, `ScrollView`.
2. **Styling & Theming:**
   - Implement a CSS-like or structural styling system.
3. **Animation:**
   - Interpolation system for layout constraints and colors (spring physics).
4. **AI Automation Testing:**
   - Drive the entire application state via the AI Control Bus to prove the fast-path architecture.
