# 02. AI Control Bus ("Inside-Out" Integration)

Modern applications must be as native to AI agents as they are to humans. Standard GUI toolkits bolt on accessibility APIs (like UIAutomation or AT-SPI) which are slow, string-heavy, and prone to state desync. 

This toolkit features the **AI Control Bus (AICB)**, designed from day one.

## Core Concepts

### 1. The Synchronized State Tree (SST)
The UI Scene Graph is stored using a Data-Oriented Design (DOD) approach (arrays of structs). 
- Every widget and layout node exists in a contiguous block of memory.
- The AI Bus provides a structured schema (akin to an Abstract Syntax Tree) directly mapping to this memory.
- **Speed:** An AI system can parse the entire state of a complex UI (thousands of elements) in microseconds via shared memory, rather than querying elements via IPC strings.

### 2. Semantic Typing
Every Control has a strongly typed Semantic Interface.
- A human sees a custom rendered 3D toggle switch.
- The AI sees `Control::Toggle{id: 402, state: Boolean::True, purpose: "EnableDarkTheme"}`.
- All controls demand a `purpose` string and data-binding upon creation. An application will not compile/run properly in debug mode without these, enforcing AI-readability.

### 3. High-Speed Action Pathways
When an AI drives the application, it bypasses the Human Event Queue (mouse moves, clicks).
- The AI injects commands directly into the **Action Dispatcher**.
- Example: Instead of simulating mouse movement to X:400, Y:200 and sending a `Mouse_Down`, the AI issues `ExecuteAction(ControlID: 402, Action::Toggle)`.
- The GUI engine recognizes AI actions and can (optionally) render a visual confirmation (e.g., a ripple effect) to inform a human observer what the AI just did.

### 4. Layout Constraints Modification
AI isn't just a user; it's a co-creator.
- The AI Control Bus allows AI agents to stream structural UI modifications on the fly.
- An AI can append new `Views`, alter `LayoutConstraints`, or bind new data streams to the application without a re-compile.

## Architectural Implementation Details
The AICB is implemented via a lock-free Ring Buffer shared between the UI thread and a dedicated AI integration thread. External agents (running in separate processes, Python, or local LLMs) connect via a high-performance Named Pipe or Shared Memory segment defined in `Layer 4`.

---

## Implementation (current) — authoritative reference

> This section documents what is actually built today and supersedes the speculative
> notes above where they differ. Source: `include/genesis/core/AiControlBus.h`.

### Transport
- A real **POSIX shared-memory segment** named **`/genesis_ai_bus`** (`shm_open` + `mmap`),
  created by `GApplication` at startup. If shm is unavailable it falls back to an
  in-process pool (tests/headless stay green). The segment is unlinked on clean exit and
  defensively re-unlinked on launch (no leaks).
- An external agent in any language maps the segment read-only (to observe) or read-write
  (to drive). No sockets, no X, no screenshots.

### Memory layout (`SharedBusMemory`)
| field | purpose |
|---|---|
| `magicCookie` = `0x47454E53` ("GENS") | readers validate before trusting the segment |
| `version` (currently **3**) | ABI gate — readers must match |
| `generation` (atomic u64) | **seqlock**: odd while the app is publishing, even when stable |
| `telemetryFrameCounter` (atomic u64) | monotonic publish id |
| `nodeCount`, `nodes[AiNodeDescriptor]` | the semantic tree (READ channel) |
| `inboundCommand` (`AiVirtualInput`) | legacy pixel/key channel |
| `inboundAction` (`AiActionRequest`) | **semantic act-by-id channel** |

### READ — the semantic tree
Each `AiNodeDescriptor` is POD/blittable: `id`, `role` (e.g. `"Button"`, `"Slider"`,
`"DockPanel"`), `name` (label), `value` (live: `"checked"`, `"0.90"`, `"1440p"`…),
`stateFlags` (`AiEnabled|AiVisible|AiInteractable|AiFocused|AiPressed`), and geometry.

Read a **consistent, lock-free snapshot** with the seqlock: load `generation` (retry if
odd), copy `nodeCount`+`nodes`, re-load `generation`; if unchanged the snapshot is torn-free.
`AiControlBus::snapshot()` implements this for C++ callers. The app re-publishes whenever the
UI changes (event-driven) plus a ~1 s idle heartbeat.

### ACT — semantic action, by identity not pixels
`AiActionRequest` is a **single-slot request/ack RPC**:
1. Agent writes `targetId` + `action` (a string), then bumps `requestSeq` (release).
2. The app, **on the UI thread**, `pollAction()`s, dispatches to the widget via
   `executeSemanticAction(action)`, writes `resultCode`, and sets `ackSeq = requestSeq`.
3. Agent waits for `ackSeq`, reads `resultCode` (`1` = handled, `0` = action not understood,
   `-1` = no such target), then re-reads the tree to verify the new state.

The action vocabulary **is the widgets' own** — no separate command schema:

| role | actions |
|---|---|
| Button | `click` |
| ToggleButton | `toggle` |
| CheckBox | `check`, `uncheck` |
| RadioButton | `select` |
| Slider / ScrollBar | `set_value:<float>` |
| SpinBox | `increment`, `decrement` |
| LineEdit | `set_text:<string>` |
| ComboBox | `select:<item-label>` |
| TabBar | `select_tab:<index>` |
| DockPanel | `activate` |

Node ids at/above `0x40000000` are **docked panels** (synthetic ids offset past scene-graph
node ids); below that they are widget scene-graph node ids.

### Concurrency & stability
- All cross-process fields are lock-free atomics; reads are wait-free via the seqlock and are
  safe from any thread or process. **Writes (publish) and command dispatch happen on the UI
  thread**, keeping widget mutation thread-correct under the app's worker-thread model.
- Versioned ABI, bounds-checked dispatch, fixed-size string fields, result codes — a
  malformed or stale reader/agent cannot corrupt the app.

### Tools (`tools/`)
- **`genesis_ai_probe`** — dumps the live semantic tree (read channel).
- **`genesis_ai_agent`** — finds controls by role/label, acts by id, verifies by read-back.
  Scripted demo, or `--role R --find L --do ACTION`.

### Extending
A widget joins the AI surface by implementing `IAIState::getSemanticNode()`
(role/label/value/interactable) and `executeSemanticAction()`. The app bridges those into
descriptors in `ControlsCatalog::collectAiNodes()` / dispatches in `dispatchAiAction()`.
See **`AGENTS.md`** at the repo root for the agent-author quick start.
