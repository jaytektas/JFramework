# Driving a Genesis app as an AI agent

Genesis is **AI-native**: a running Genesis application exposes its entire UI — every
control's identity, role, label, live value and state — through a shared-memory bus, and
accepts **semantic commands addressed by node id**. As an agent you should **never**
screenshot the window or synthesize pixel clicks. Read the tree, find the control by
meaning, act on it by id, verify by reading back. It is deterministic and runs at memory
speed.

> Full reference: [`docs/architecture/02_AI_CONTROL_BUS.md`](docs/architecture/02_AI_CONTROL_BUS.md).
> Working examples: [`tools/ai_probe.cpp`](tools/ai_probe.cpp) (read) and
> [`tools/ai_agent.cpp`](tools/ai_agent.cpp) (read + act).

## The loop

```
open /genesis_ai_bus  ->  read semantic tree  ->  find control by role/label
        ->  submit {targetId, action}  ->  wait for ack  ->  read tree again to verify
```

## 1. Attach

The app creates a POSIX shared segment named **`/genesis_ai_bus`**. Map it (read-only to
observe, read-write to drive):

```c
int fd = shm_open("/genesis_ai_bus", O_RDWR, 0600);
SharedBusMemory* bus = mmap(NULL, sizeof(SharedBusMemory),
                            PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
// validate before trusting:  bus->magicCookie == 0x47454E53 && bus->version == 3
```

The layout is `struct SharedBusMemory` in `include/genesis/core/AiControlBus.h` — that
header IS the ABI. Any language that can mmap and read the struct works (C/C++/Rust/Python via
ctypes…).

## 2. Read the tree (consistently)

Use the **seqlock** so you never read a half-written frame:

```
do {
    g1 = atomic_load(bus->generation);      // retry while odd (publish in progress)
    if (g1 & 1) continue;
    copy bus->nodeCount and bus->nodes[...];
    g2 = atomic_load(bus->generation);
} while (g1 != g2);
```

Each `AiNodeDescriptor`: `id`, `role`, `name` (label), `value` (live), `stateFlags`
(`E`nabled/`V`isible/`I`nteractable/`F`ocused/`P`ressed), and geometry. Find your target by
matching `role` and/or a substring of `name`.

## 3. Act (by id, never pixels)

Single-slot request/ack RPC in `bus->inboundAction`:

```c
strncpy(bus->inboundAction.action, "set_value:0.9", 63);
bus->inboundAction.targetId = id;                 // from the descriptor you found
uint32_t seq = bus->inboundAction.requestSeq + 1;
atomic_store(bus->inboundAction.requestSeq, seq); // submit (release)
// wait for the app (UI thread) to handle it:
while (atomic_load(bus->inboundAction.ackSeq) != seq) sleep_ms(1);
int result = bus->inboundAction.resultCode;       // 1=ok, 0=action not understood, -1=bad id
```

Then re-read the tree (step 2) to confirm the new `value`/`stateFlags`. Issue the next command
only after the ack (the slot holds one command at a time).

### Action vocabulary (it's the widgets' own language)

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
| DockPanel (id ≥ `0x40000000`) | `activate` |

## Try it now

With a Genesis app running (e.g. `build/genesis_controls_catalog`):

```sh
build/genesis_ai_probe                              # watch the live semantic tree
build/genesis_ai_agent                              # scripted: check / toggle / select / set
build/genesis_ai_agent --role Slider --do "set_value:0.25"
build/genesis_ai_agent --role DockPanel --find "Console" --do "activate"
```

## Rules of engagement

- **Never** screenshot or send pixel clicks — there is no need, and it is fragile.
- Address controls by **id**, found via **role + label**; ids below `0x40000000` are widgets,
  ids at/above are dock panels.
- Reads are wait-free and safe from any thread/process. The app applies your command on its
  **UI thread**, so a command takes effect within one frame (~16 ms) and is then visible in
  the tree.
- Respect the handshake: one command per `requestSeq`; wait for `ackSeq` before the next.
