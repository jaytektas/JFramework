# 04. Controls & Widget Catalog

While no code is borrowed, the taxonomy of GUI controls follows industry standards (inspired by Qt, GTK, and web frameworks) to ensure intuitive usage for both developers and AI.

## Base Classes
*   `Control`: The abstract base class. Handles visibility, padding, margins, and standard event callbacks (`onHover`, `onClick`, `onFocus`).
*   `Container`: Inherits `Control`. Maintains a list of child controls and applies Layout Engines (Grid, Flex, Absolute).

## Core Interactables
*   `Button`: Standard push button. Supports text, icons, and state transitions (idle, hover, pressed, disabled).
*   `ToggleButton`: Boolean state switch.
*   `CheckBox` / `RadioButton`: Standard grouped and isolated boolean selections.
*   `Slider`: 1D value selection. Handles fractional interpolation and stepping.
*   `Dial` / `Knob`: Radial 1D value selection.
*   `ProgressBar`: Visual feedback for indeterminate or determinate asynchronous tasks.

## Text & Data Entry
*   `Label`: Non-interactive text display. Supports rich text (bold, italic, color runs) via custom text engine.
*   `TextInput`: Single-line text entry. Supports custom cursors, text selection, and localized IME (Input Method Editor) hookups.
*   `TextArea`: Multi-line text entry with virtualization and fast scrolling.
*   `ComboBox` / `DropDown`: Collapsed list for singular selection.

## Advanced Data Displays
*   `ListView`: Highly optimized, virtualized vertical list for displaying massive datasets. Only renders items currently visible in the viewport.
*   `TreeView`: Hierarchical data display with expand/collapse logic.
*   `TableView` / `DataGrid`: 2D virtualized scrolling, sortable columns, and editable cells.
*   `ScrollView`: Wraps any content providing custom physics-based scrollbars.

## Layout & Structure
*   `FlexBox`: Directional (Row/Column) layout engine prioritizing distribution of available space.
*   `GridBox`: 2D coordinate-based placement.
*   `Splitter`: Draggable divider between two or more layout regions.
*   `TabControl`: Overlapping page navigation.
*   `Window`: The top-level OS integration unit. Handles title bars (client-side decorations), minimize/maximize, and OS dragging.

## Multimedia & Custom
*   `Canvas`: An immediate-mode drawing surface exposed to the application developer for custom visualizations.
*   `Image`: Decodes (via custom written decoders for PNG/JPG) and displays bitmap data.

## AI Control Specifics
Every widget in this catalog inherits the `AIState` interface:
```cpp
virtual AITreeNode GetSemanticNode() const = 0;
virtual bool ExecuteSemanticAction(AIAction action) = 0;
```
This forces all future widget implementations to define exactly how an AI understands them and interacts with them, ensuring the AI bus is never an afterthought.
