# 08. Genesis Prime: The Agentic Window Manager

Traditional Window Managers (WMs) are static grids. Genesis Prime is a **Context-Aware Spatial Compositor**. It treats the entire desktop as a single, contiguous Genesis `SceneGraph` where applications are nodes, not isolated pixel boxes.

## The Symmetric Workspace (The "Dual-Sided" Model)

Genesis Prime is not a traditional OS where the AI is a "sidebar assistant." Instead, it is a **Symmetric Workspace** where the human and the agent "sit on opposite sides" of the same data structure.

### 1. Dual-Path Input Lanes
The environment processes two high-frequency input streams in parallel:
*   **The Human Lane (Southbound):** Raw hardware events (mouse, keyboard, touch) arriving from the OS HAL. Optimized for intuition and physical precision.
*   **The Agentic Lane (Northbound):** Semantic action commands arriving via the lock-free `AiControlBus`. Optimized for speed and structural logic.

### 2. Dual-Focus Model
Traditional WMs have one "Active Window." Genesis Prime maintains a **Split Focus**:
*   **Human Focus (`H-Focus`):** The window currently responding to the user's physical input.
*   **Agentic Focus (`A-Focus`):** The window currently being processed, reorganized, or driven by the AI agent.
*   *Outcome:* An agent can be cleaning up data in your Spreadsheet window while you are typing an email in the Message window—zero interference.

### 3. Symmetrical Arbitration
When both "sides" attempt to manipulate the same node (e.g., both trying to resize the same window):
*   The **Symmetric Arbiter** uses a weight-based priority system. 
*   Human intent usually takes precedence for *creative content*, while Agentic intent takes precedence for *structural layout* and *efficiency optimizations*.

### 2. Spatial Compositing (Infinite Canvas)
Windows are no longer restricted to screen boundaries.
*   **SceneGraph Desktop:** The root `NodeId` in Genesis Prime is the "Infinite Workspace." All application windows are children of this node.
*   **Semantic Z-Ordering:** Depth is managed by the `AiControlBus`. High-priority alerts or active task windows are moved forward in the Z-buffer algorithmically, not just by "last click."

### 3. "Inside-Out" Surface Protocol
Instead of standard X11/Wayland atom communication (which is string-heavy and slow), Genesis Prime uses the **Genesis Surface Protocol (GSP)**.
*   **Direct Texture Sharing:** Applications submit their `PrimitiveBuffer` directly to the WM's GPU command queue.
*   **Unified SDF Shading:** The WM and all its apps share the same SDF font atlases and shader state, allowing for global acrylic blurs and sub-pixel text anti-aliasing across window boundaries.

## Architectural Layers

### Layer A: The Global Compositor
*   **Engine:** Genesis GRE (Layer 2).
*   **Duty:** Final frame assembly of all app surfaces.
*   **Input:** Multi-producer `AiControlBus`.

### Layer B: Workspace Virtualization
*   **Engine:** Genesis SceneGraph (Layer 5).
*   **Duty:** Managing thousands of "Off-screen" application nodes that remain in memory for instant spatial navigation.

### Layer C: The Agent Bridge
*   **Engine:** Genesis AiControlBus (Layer 4).
*   **Duty:** Allowing local LLMs or task-agents to "see" every window's internal structure and manipulate the workspace layout without user intervention.

## Comparison: The Superior Edge
| Feature | Traditional WM | Genesis Prime |
| :--- | :--- | :--- |
| **Logic** | Static Rules (JSON/Lua) | Dynamic AI Optimization |
| **Communication** | IPC / Message Passing | Lock-free Shared Memory (AICB) |
| **Rendering** | Independent Pixmap Blitting | Unified SDF Primitive Stream |
| **Structure** | Linked List of Windows | Data-Oriented SceneGraph |
| **Latency** | Frame-Lag (Compositor hop) | Zero-Copy (Direct GPU submission) |
