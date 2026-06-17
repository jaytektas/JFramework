# Genesis UI Toolkit (Internal Codename)

## Overview
This repository contains the completely bespoke, cross-platform C++ GUI and application toolkit. Designed from the absolute ground up, this toolkit bridges the raw interface of operating system GPU drivers (Vulkan/Metal/DX/DRM) all the way to a highly abstract, AI-driven application layer. 

**Zero code is copied from existing toolkits (Qt, GTK, wxWidgets, etc.).** The codebase relies solely on OS-level system calls and native driver APIs.

## Core Philosophy

1. **Absolute Ownership:** We own the stack. From drawing a line on a pixel buffer to managing multi-monitor DPI scaling, every algorithm is written in-house.
2. **Layered & Intuitive:** The architecture is strictly stratified. Higher layers (Controls, Layout) cannot bypass their immediate dependencies to access lower layers (GPU HAL), ensuring a clean, intuitive, and easily understandable codebase.
3. **AI-First (The "Inside-Out" Model):** Traditional GUIs are designed for humans first, with AI relying on clunky accessibility APIs or OCR. This toolkit integrates a high-speed **AI Control Bus**. AI agents have direct memory access to the UI AST (Abstract Syntax Tree), layout constraints, and event queues, allowing them to read, drive, and mutate the GUI at computational speeds.
4. **Modern & High Performance:** Built using modern C++ standards (C++20/C++23) emphasizing lock-free data structures, Data-Oriented Design (DOD), and SIMD-optimized math paths. All rendering is heavily multi-threaded and GPU-accelerated.
5. **Cross-Platform:** Native abstractions for Windows, Linux, and macOS, without compromising the unified core.

## Documentation Index
- [01. Layered Design & Architecture](architecture/01_LAYERED_DESIGN.md)
- [02. AI Control Bus Integration](architecture/02_AI_CONTROL_BUS.md)
- [03. Graphics & Rendering Primitives](architecture/03_GRAPHICS_PRIMITIVES.md)
- [04. Controls & Widget Catalog](architecture/04_CONTROLS_CATALOG.md)
