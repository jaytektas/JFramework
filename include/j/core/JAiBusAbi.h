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
inline constexpr uint32_t kAiBusVersion   = 5;            // ABI version (bump on layout change)
inline constexpr uint32_t kAiBusMaxNodes  = 2048;
inline constexpr char     kAiBusDefaultName[] = "/jframework_ai_bus";

// One widget, mirrored from its JA11yNode (role/name/value/state) + geometry. Blit-safe POD.
struct alignas(16) JAiBusNode {
    uint32_t id{0xFFFFFFFFu};
    uint32_t stateFlags{0};      // JA11yState bits (focused/selected/…)
    float    x{0}, y{0}, w{0}, h{0};
    uint32_t hasRange{0};        // curValue/minValue/maxValue meaningful?
    float    curValue{0}, minValue{0}, maxValue{0};
    char     role[24]{0};        // "JButton", "JSlider", …
    char     name[32]{0};        // label / accessible name
    char     value[24]{0};       // "0.50", "checked", text, …
};

// Single-slot request/ack RPC (client -> server), addressed by node id, never pixels. The client writes
// targetId + action then bumps requestSeq; the server handles it ON THE MAIN THREAD, writes resultCode,
// then sets ackSeq = requestSeq. Each field has exactly one writer, so no locking is needed.
struct alignas(16) JAiBusAction {
    std::atomic<uint32_t> requestSeq{0};   // client bumps to submit
    std::atomic<uint32_t> ackSeq{0};       // server sets = requestSeq when handled
    uint32_t targetId{0xFFFFFFFFu};
    int32_t  resultCode{0};                // 1 handled, 0 not handled, -1 bad target
    char     action[64]{0};                // "click", "focus", "set_value:0.5", "select:1080p", …
};

// The whole segment. `seq` is a seqlock: the server makes it ODD while writing the node array and EVEN
// when done; a reader samples seq, reads, re-samples, and retries if it changed or was odd — so a client
// never consumes a half-written frame without any lock.
struct JAiBusShared {
    uint32_t              magic{kAiBusMagic};
    uint32_t              version{kAiBusVersion};
    std::atomic<uint64_t> seq{0};
    uint32_t              nodeCount{0};
    uint32_t              _pad{0};
    JAiBusAction          action;
    JAiBusNode            nodes[kAiBusMaxNodes];
};

// Copy a std::string-ish into a fixed NUL-terminated buffer.
inline void aiBusCopy(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    size_t n = 0; if (src) { n = std::strlen(src); if (n > cap - 1) n = cap - 1; std::memcpy(dst, src, n); }
    dst[n] = '\0';
}

} // inline namespace jf
