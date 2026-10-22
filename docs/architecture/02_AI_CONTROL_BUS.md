# 02. The AI bus

An app built on JFramework can be driven by a script or an AI agent through the **AI bus**: a block of
shared memory where the app publishes its live widget tree and services one action at a time. It is off
unless asked for, and costs one null check per frame when off.

Source: `include/j/core/JAiBusAbi.h` (the byte layout, ABI **8**), `include/j/core/JAiBus.h` (the server
inside the app), `tools/jf-busctl.cpp` (the client).

## Turning it on

- Start the app with `JF_AI_BUS=1` in its environment (`JAppWindow` calls `JAiBus::enable()`), or call
  `JAiBus::instance().enable()` yourself — an app with no window does this and calls `tick()` from its own
  loop.
- Linux / macOS: a POSIX shared-memory segment, `/jframework_ai_bus`. Windows: a named file mapping,
  `Local\jframework_ai_bus`. One app per session has the bus; a second one replaces it.

## What is in it

| Part | Direction | What |
|---|---|---|
| `nodes[16384]` | app → client | every visible widget: id, role, name (64), value (48), state flags, rect, range |
| `droppedNodes` | app → client | visible widgets that did not fit this frame (0 = the dump is the whole window; `jf-busctl dump` prints it) |
| `action` | client → app | a target id and an action string of up to 64 KB |
| `reply` | app → client | up to 8 MB of text answering the action (JSON by convention) |

The node array is under a **seqlock** (`seq` odd while writing), so a reader never takes a half-written
frame. The action is a single-slot RPC: the client writes `targetId` and `action`, bumps `requestSeq`; the
app handles it **on its main thread**, writes `resultCode` (1 handled, 0 not, −1 bad target) and the reply,
then sets `ackSeq = requestSeq`. An app handler may also answer 2, *handled but refused*, with the reason
in the reply. Each field has one writer, so nothing is locked. `ownerPid` lets a client
tell a segment left behind by a crashed app from a live one.

## Actions every app answers

Addressed at a widget id from a dump:

- `click`, `click:X,Y` (a point of the widget, window coordinates), `focus`
- `type:<text>` (one key press per character, through the widget's own keyboard path), `key:Return`
  (`Escape`, `Tab`, `Backspace`, `Delete`, `Left`, `Right`, `Up`, `Down`, `Home`, `End`, `Space`)
- `dialog:ok` / `dialog:cancel` — the frontmost native dialog (published as `Dialog` nodes after the widgets)

Anything else goes to the app's own handler, `JAiBus::onAction`, which may answer with
`JAiBus::reply(text)`. An app's commands are usually addressed at no widget at all (id 0).

## jf-busctl

```
jf-busctl dump [--json]           every visible node
jf-busctl find <text> [--json]    the nodes whose line contains it
jf-busctl get <name>              one node's value
jf-busctl wait <text> [secs]      until a node matches (exit 4 on timeout)
jf-busctl act <id|name> <action>  act on a widget (a name must match exactly one node)
jf-busctl call <action>           an app command; prints the reply
```

An action of `-` is read from stdin. Exit codes: 0 handled, 1 no bus, 2 usage, 3 no ack, 4 not found /
not handled / refused, 5 ambiguous name, 6 bad target. `JF_BUSCTL_TIMEOUT` (seconds, default 10) bounds the wait.

The SDK installs the client's source as `share/jframework/tools/jf-busctl.cpp`, so an app can build it
beside itself and ship it — the client then always speaks that app's ABI.

## Rules it keeps

- **Opt-in.** Off, it is a null check.
- **One hook.** `JAppWindow` calls `tick()` once per frame; no widget has AI code of its own. The data is
  the accessibility layer every widget already has (`a11yNode()`).
- **Main thread only.** Actions run in the render loop, never from another thread.
- **Never truncated.** A client refuses an action that does not fit; a reply too large says so instead of
  arriving cut off.
