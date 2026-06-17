# 01. Layered Design & Architecture

To achieve an intuitive, bug-free, and scalable toolkit, the architecture is strictly divided into distinct logical layers. Each layer provides a clean, well-documented C++ API for the layer directly above it.

## The Architecture Stack

### Layer 0: System Abstraction Layer (SAL) & Driver Bindings
- **Responsibilities:** Direct interface with OS kernels (Linux syscalls, Win32, Mach/Darwin) and GPU Driver interfaces (Direct Rendering Manager (DRM) on Linux, DXGI on Windows). 
- **Threading:** Thread-spawning, atomic primitives, and thread-local storage mapping.
- **Memory:** Custom highly-aligned memory allocators (Arena, Pool, and Stack allocators) bypassing standard `malloc` for critical paths.

### Layer 1: Core Primitives & Math Engine
- **Responsibilities:** The foundational data types.
- **Components:** 
  - SIMD-accelerated Vectors (`Vec2`, `Vec3`, `Vec4`), Matrices (`Mat3`, `Mat4`).
  - Colorspaces (RGBA, linear RGB, sRGB, Oklab for perceptually uniform blending).
  - Geometry (Lines, Quads, Cubic/Quadratic Bezier curves, Polygons).
  - String manipulation (UTF-8 native, completely custom parsing).

### Layer 2: Graphics Rendering Engine (GRE)
- **Responsibilities:** Translating geometric primitives into GPU commands.
- **Components:**
  - **Command Builder:** Batches draw calls to minimize state changes.
  - **Shader Generator:** Compiles toolkit-specific shading logic into target representations (SPIR-V, DXIL, MSL).
  - **Text Renderer:** A bespoke vector-based font engine. Parses OpenType/TrueType binaries directly and tessellates glyphs onto the GPU. No FreeType dependency.

### Layer 3: Application & Event Loop
- **Responsibilities:** Window management, input aggregation, and timing.
- **Components:**
  - **Window Manager:** Connects to X11/Wayland/Win32/Cocoa to create application frames and handle resize/DPI.
  - **Event Queue:** Lock-free, multi-producer single-consumer (MPSC) queue handling hardware interrupts (mouse, keyboard, touch, stylus).

### Layer 4: AI Control Bus (Data & State Plane)
- **Responsibilities:** Exposes the entire UI state as an immediate, readable, and writable data structure for AI integration. *(See [02_AI_CONTROL_BUS.md](02_AI_CONTROL_BUS.md) for details)*.

### Layer 5: Base View & Composition Layer
- **Responsibilities:** The building blocks of the UI.
- **Components:**
  - `View`: The base node of the UI tree. Contains bounds, transform matrix, and styling data.
  - **Scene Graph:** A Data-Oriented tree structure representing the UI layout, built for high-speed cache-friendly traversal during the rendering and layout passes.

### Layer 6: Constraint & Layout Engine
- **Responsibilities:** Positioning Views relative to each other dynamically.
- **Components:**
  - A bespoke constraint solver, supporting Flex-like box models, Grids, and absolute anchoring, evaluated multi-threaded.

### Layer 7: Control Foundation (Widgets)
- **Responsibilities:** Interactive elements tailored for both human user and AI interaction.
- **Components:** Buttons, TextInputs, Sliders, Dropdowns. State-managed elements with defined hover, active, and focus behaviors. *(See [04_CONTROLS_CATALOG.md](04_CONTROLS_CATALOG.md))*.

### Layer 8: Complex Application Components
- **Responsibilities:** High-level abstractions for rapid application development.
- **Components:** Docking systems, MDI (Multiple Document Interface), Data Models, State Binding (MVVM/MVC structures).
