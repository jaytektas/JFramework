// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JAiBusAbi — the shared-memory ABI for the JFramework AI bus. POD only (no widget/toolkit deps), so
// an external client (jf-busctl) and the in-process server (JAiBus) agree on the exact byte layout.
// The server publishes a snapshot of the live widget tree (from the accessibility data every widget
// already exposes) and services one pending action per frame. Bump kAiBusVersion on any layout change.

#include <atomic>
#include <cstdint>
#include <cstring>

inline namespace jf {

inline constexpr uint32_t kAiBusMagic     = 0x4A414942;   // "JAIB"
inline constexpr uint32_t kAiBusVersion   = 9;            // ABI version (bump on layout change)
// Room for every visible widget of a real application. 2048 was not: a tuning studio with its docks,
// menus and a page open publishes more than that, and the rest — the dialogs, which are added last —
// silently did not exist for a client. ~176 bytes a node, so 16384 is under 3 MB of address space.
inline constexpr uint32_t kAiBusMaxNodes  = 16384;
// A request can carry data (a whole table's cells to write), and a reply can carry a lot more (every
// setting an app has, with its units and range). The segment is mapped, not copied, so room that a
// request does not use costs nothing but address space.
inline constexpr uint32_t kAiBusActionMax = 64 * 1024;
inline constexpr uint32_t kAiBusReplyMax  = 8 * 1024 * 1024;
#if defined(_WIN32)
inline constexpr char     kAiBusDefaultName[] = "Local\\jframework_ai_bus";   // a named file mapping
#else
inline constexpr char     kAiBusDefaultName[] = "/jframework_ai_bus";          // a POSIX shm segment
#endif

// One widget, mirrored from its JA11yNode (role/name/value/state) + geometry. Blit-safe POD.
struct alignas(16) JAiBusNode {
    uint32_t id{0xFFFFFFFFu};
    uint32_t stateFlags{0};      // JA11yState bits (focused/selected/…)
    float    x{0}, y{0}, w{0}, h{0};
    uint32_t hasRange{0};        // curValue/minValue/maxValue meaningful?
    float    curValue{0}, minValue{0}, maxValue{0};
    char     role[24]{0};        // "JButton", "JSlider", …
    char     name[64]{0};        // label / accessible name
    char     value[48]{0};       // "0.50", "checked", text, …
};

// Single-slot request/ack RPC (client -> server), addressed by node id, never pixels. The client writes
// targetId + action then bumps requestSeq; the server handles it ON THE MAIN THREAD, writes resultCode,
// then sets ackSeq = requestSeq. Each field has exactly one writer, so no locking is needed.
struct alignas(16) JAiBusAction {
    std::atomic<uint32_t> requestSeq{0};   // client bumps to submit
    std::atomic<uint32_t> ackSeq{0};       // server sets = requestSeq when handled
    uint32_t targetId{0xFFFFFFFFu};
    int32_t  resultCode{0};                // 1 handled, 2 handled but refused (the reply says why), 0 not handled, -1 bad target
    // Long enough for an action that carries DATA, not just a verb: "click", "set_value:0.5",
    // "select_node:A/B/C" — and an app's own commands, which can carry a whole table to write. (It was 64
    // bytes, then 256; each time the overflow was silently truncated and the action went somewhere that
    // did not exist.) A client refuses an action that does not fit rather than truncating it.
    char     action[kAiBusActionMax]{0};
    // THE ANSWER. A result code says whether an action was handled; it cannot say what a setting IS. The
    // server writes replyLen bytes of text here (JSON, by the app's convention) before it acks, and clears
    // it before it handles the next action, so a reply always belongs to the action just acked.
    uint32_t replyLen{0};
    char     reply[kAiBusReplyMax]{0};
};

// The whole segment. `seq` is a seqlock: the server makes it ODD while writing the node array and EVEN
// when done; a reader samples seq, reads, re-samples, and retries if it changed or was odd — so a client
// never consumes a half-written frame without any lock.
struct JAiBusShared {
    uint32_t              magic{kAiBusMagic};
    uint32_t              version{kAiBusVersion};
    std::atomic<uint64_t> seq{0};
    uint32_t              nodeCount{0};
    // Widgets that were visible but did not fit in nodes[] this frame. Never silently: a client that
    // sees this non-zero knows the tree it is reading is not the whole window.
    uint32_t              droppedNodes{0};
    // The process publishing it. A segment outlives the app that made it when that app dies without
    // removing it, and its last frame then reads as live: a client checks this process still runs.
    int32_t               ownerPid{0};
    JAiBusAction          action;
    JAiBusNode            nodes[kAiBusMaxNodes];
};

// Copy a std::string-ish into a fixed NUL-terminated buffer.
// Cut on a CHARACTER boundary, never inside one. A 64-byte name field cut mid-sequence left half an
// "…" or "↔" at the end, and a client then could not find the widget by the name it was showing ("Check
// for ECU Firmware Update" + a stray lead byte), and a strict decoder refused the whole dump. When the
// first byte left out is a UTF-8 continuation byte, the character it belongs to goes with it.
inline void aiBusCopy(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    size_t n = 0;
    if (src) {
        n = std::strlen(src);
        if (n > cap - 1) {
            n = cap - 1;
            while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0u) == 0x80u) --n;
        }
        std::memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

} // inline namespace jf
