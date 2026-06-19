# 02. AI Control Bus ("Inside-Out" Integration)

Modern applications must be as native to AI agents as they are to humans. Standard GUI
toolkits bolt on accessibility APIs (like UIAutomation or AT-SPI) which are slow,
string-heavy, and prone to state desync.

This toolkit features the **AI Control Bus (AICB)**, designed from day one — a
bidirectional, zero-copy, lock-free shared-memory channel between the running application
and any external AI agent.

---

## Core Concepts

### 1. The Synchronized State Tree (SST)
Every widget exists in a contiguous scene-graph array.  The AI bus mirrors that array as
a flat `AiNodeDescriptor[]` snapshot — structured schema, no IPC strings, no round-trips.
An agent can parse a complex UI (thousands of controls) in microseconds.

### 2. Semantic Typing
Every control exposes a strongly typed semantic interface:
- Human sees a custom toggle switch.
- AI sees `role="ToggleButton" name="Dark Mode" value="true" flags=EVIF`.
- No pixel access. No screenshots. Identity + meaning, always.

### 3. Bidirectional, Lock-Free Channels

| Channel | Direction | Purpose |
|---|---|---|
| `nodes[AiNodeDescriptor]` | App → Agent | Live semantic tree (READ) |
| `inboundAction` | Agent → App | Semantic act-by-id (WRITE) |
| `outboundSignal` | App → Agent | User-interaction events (LISTEN) |

All cross-process fields are lock-free atomics; reads are wait-free via the seqlock.

### 4. Runtime Co-Development (Live Injection)
An AI agent can **inject** new widgets into a running application with no recompile:
- `inject:button:Console:Run Analysis` — adds a button to the Console panel live.
- `inject:lineedit:Properties:API key` — adds an input field.
- `remove_widget:<nodeId>` — removes any previously-injected control.

When satisfied with the live layout, a developer can compile it in as a permanent feature.

---

## Implementation — authoritative reference

> Source: `include/genesis/core/AiControlBus.h`, version **4**.

### Transport
- **POSIX shared-memory segment** named **`/genesis_ai_bus`** (`shm_open` + `mmap`).
- Created by `GApplication` at startup; unlinked on exit; defensively re-unlinked on relaunch.
- Any language that can `mmap` and read packed atomics can connect (C, C++, Rust, Python ctypes).
- Read-only mapping → observe. Read-write mapping → drive.

### Memory layout (`SharedBusMemory`)
| field | purpose |
|---|---|
| `magicCookie` = `0x47454E53` ("GENS") | readers validate before trusting the segment |
| `version` = **4** | ABI gate — readers must match |
| `generation` (atomic u64) | **seqlock**: odd while publishing, even when stable |
| `telemetryFrameCounter` (atomic u64) | monotonic publish id |
| `nodeCount`, `nodes[AiNodeDescriptor]` | semantic tree (READ channel) |
| `inboundCommand` (`AiVirtualInput`) | legacy pixel/key channel |
| `inboundAction` (`AiActionRequest`) | semantic act-by-id channel (Agent→App) |
| `outboundSignal` (`AiSignalNotification`) | user-event channel (App→Agent) |

### READ — the semantic tree
Each `AiNodeDescriptor` is POD/blittable: `id`, `role`, `name`, `value`, `stateFlags`,
geometry.  Read a **consistent, lock-free snapshot** with the seqlock:

```c
do {
    g1 = atomic_load(bus->generation);      // retry while odd (write in progress)
    if (g1 & 1) continue;
    copy nodeCount and nodes[];
    g2 = atomic_load(bus->generation);
} while (g1 != g2);
```

`AiControlBus::snapshot()` implements this for C++ callers.

### ACT — semantic action, by identity not pixels
Single-slot request/ack RPC via `inboundAction`:
1. Agent: write `targetId` + `action`, bump `requestSeq` (release).
2. App (UI thread): `pollAction()`, dispatch, write `resultCode`, set `ackSeq = requestSeq`.
3. Agent: wait for `ackSeq`, check `resultCode` (`1`=OK, `0`=not-understood, `-1`=no-target),
   re-read tree to verify.

`AiControlBus::submitActionBlocking()` implements steps 1-3 for agent-side C++.

### LISTEN — outbound signal channel (new in v4)
When a **user** interacts with a widget (click, toggle, text-change…) the app automatically
publishes a non-blocking signal via `outboundSignal`:

```c
// App side (automatic — wired in GApplication ctor via AiBusHook):
bus.publishSignal(nodeId, "click", "Primary Action");

// Agent side — poll for new signals:
uint32_t lastSeq = 0;
while (true) {
    uint32_t seq = atomic_load(bus->outboundSignal.signalSeq);
    if (seq != lastSeq) {
        lastSeq = seq;
        // read targetId, signalName, signalValue
    }
    sleep_ms(5);
}
```

Agents can use `genesis_ai_agent --watch-signals` to stream events to stdout.

### SYSTEM — broadcast / inject actions (targetId = `0xFFFFFFFF`)

Addressed to the magic broadcast id `0xFFFFFFFF`, these bypass the widget tree entirely:

| action | effect |
|---|---|
| `inject:<type>:<panel>:<label>` | Dynamically create a widget in the named panel |
| `remove_widget:<nodeId>` | Remove a widget by scene-graph node id |

`<type>` is one of: `button`, `label`, `checkbox`, `lineedit`.  
Panel name must match an existing dock panel title (e.g. `Console`, `Properties`).

### Widget action vocabulary
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
| DockPanel (id ≥ `0x40000000`) | `activate`, `float`, `set_width:<px>`, `set_height:<px>`, `move_to:<pos>[:<target>]` |
| **(system broadcast)** | `inject:<type>:<panel>:<label>`, `remove_widget:<id>` |

### Concurrency & stability
- Reads are wait-free; writes/dispatch happen on the UI thread — widget mutation is always
  thread-correct.
- Versioned ABI, bounds-checked dispatch, fixed-size string fields, result codes — a
  malformed or stale agent cannot corrupt the app.

### Tools (`tools/`)
| binary | purpose |
|---|---|
| `genesis_ai_probe` | Dump live semantic tree |
| `genesis_ai_agent` | Full featured agent: find→act→verify, inject, watch signals |

```sh
build/genesis_ai_probe                                      # snapshot
build/genesis_ai_agent                                      # scripted demo
build/genesis_ai_agent --tree                               # dump semantic tree
build/genesis_ai_agent --role Button --find "Primary" --do click
build/genesis_ai_agent --inject "button:Console:Run Analysis"
build/genesis_ai_agent --remove 42
build/genesis_ai_agent --watch-signals                      # live event stream
```

### Extending
A widget joins the AI surface by implementing:
- `IAIState::getSemanticNode()` — role/label/value/interactable.
- `executeSemanticAction(action)` — dispatch vocab.

Automatic signal emission is provided by `AiBusHook` (set once in `GApplication`'s ctor)
— no widget needs to know about the bus directly.  The app bridges descriptors in
`ControlsCatalog::collectAiNodes()` / dispatches in `dispatchAiAction()`.

See **`AGENTS.md`** at the repo root for the agent quick-start loop.
