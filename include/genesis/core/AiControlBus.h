#pragma once

#include <cstdint>
#include <string_view>
#include <atomic>
#include <iostream>
#include <cstring>
#include <vector>
#include <algorithm>
#include <new>
#include <thread>
#include <chrono>
#include "SceneGraph.h"

#if defined(__linux__) || defined(__APPLE__)
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

#include <genesis/core/muted_logging_mock.h>
namespace { inline constexpr auto& LogAiBus = Genesis::Log::AI; }

namespace Genesis {

constexpr uint32_t MaxBusNodes      = 2048;
constexpr uint32_t SharedMagicCookie = 0x47454E53; // "GENS"
constexpr uint32_t SharedBusVersion  = 4;          // bump on ABI change
constexpr char     DefaultBusName[]  = "/genesis_ai_bus";

/**
 * @brief JWidget state exposed to the AI side as a bitmask.  Lets an agent reason
 *        about a control ("is it enabled? focused? checked?") without pixels.
 */
enum AiStateBits : uint32_t {
    AiEnabled      = 1u << 0,
    AiVisible      = 1u << 1,
    AiFocused      = 1u << 2,
    AiChecked      = 1u << 3,
    AiPressed      = 1u << 4,
    AiInteractable = 1u << 5,
};

/**
 * @brief Blit-safe, POD node record mirroring exactly what the AI agent can "see".
 *        Carries semantics (role/label/value/state), not just geometry, so an agent
 *        addresses controls by identity & meaning rather than coordinates.
 */
struct alignas(16) AiNodeDescriptor {
    uint32_t id{0xFFFFFFFF};
    uint32_t stateFlags{0};
    float    x{0.0f};
    float    y{0.0f};
    float    width{0.0f};
    float    height{0.0f};
    char     role[24]{0};   // "JButton", "JSlider", "JCheckBox", ...
    char     name[32]{0};   // label / accessible name
    char     value[24]{0};  // current value ("0.50", "checked", text, ...)
};

/**
 * @brief Shared Virtual Input Command Buffer (AI -> app).
 */
struct alignas(16) AiVirtualInput {
    enum class JCommandType : uint32_t { None = 0, MouseClick = 1, TextKey = 2 };

    std::atomic<JCommandType> type{JCommandType::None};
    float targetX{0.0f};
    float targetY{0.0f};
    uint32_t keyPayload{0};
    std::atomic<uint32_t> sequenceId{0};
};

/**
 * @brief Semantic action request (AI -> app), addressed by node id — never pixels.
 *
 * Single-slot request/ack RPC over shared memory: the agent writes targetId + action
 * then bumps `requestSeq`; the app handles it on the UI thread (so widget mutation stays
 * thread-correct) and writes `resultCode` + sets `ackSeq = requestSeq`.  The agent waits
 * for the ack, reads the result, and re-reads the telemetry to verify the new state.
 */
struct alignas(16) AiActionRequest {
    std::atomic<uint32_t> requestSeq{0};   // agent bumps to submit
    std::atomic<uint32_t> ackSeq{0};       // app sets = requestSeq when handled
    uint32_t targetId{0xFFFFFFFF};
    int32_t  resultCode{0};                // 1 = handled, 0 = not handled, -1 = bad target
    char     action[64]{0};                // "click", "set_value:0.5", "select:1080p", ...
};

/**
 * @brief Outbound signal notification from the app to the AI agent.
 */
struct alignas(16) AiSignalNotification {
    std::atomic<uint32_t> signalSeq{0};   // app bumps to publish a new signal
    uint32_t targetId{0xFFFFFFFF};
    char     signalName[32]{0};           // e.g. "click", "checked", "text_changed"
    char     signalValue[64]{0};          // e.g. "", "checked"/"unchecked", textbox text
};

/**
 * @brief Complete shared-memory layout representing the physical IPC segment.
 *
 * Concurrency: `generation` is a seqlock — the writer bumps it odd before a publish
 * and even after, so any reader (another thread or an external process) can take a
 * consistent snapshot without locks.  All cross-process fields are lock-free atomics.
 */
struct alignas(64) SharedBusMemory {
    uint32_t magicCookie;
    uint32_t version;
    std::atomic<uint64_t> generation;            // seqlock: odd => publish in progress
    std::atomic<uint64_t> telemetryFrameCounter; // monotonic publish count
    uint32_t nodeCount;
    uint32_t _reserved;
    AiNodeDescriptor nodes[MaxBusNodes];
    AiVirtualInput  inboundCommand;   // legacy pixel/key channel
    AiActionRequest inboundAction;    // semantic act-by-id channel
    AiSignalNotification outboundSignal; // outbound event notification channel
};

/** Safe fixed-buffer string copy for descriptor fields. */
inline void aiSetField(char* dst, size_t cap, std::string_view s) {
    size_t n = std::min(s.size(), cap - 1);
    if (n) std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

/**
 * @brief High-performance, lock-free AI JControl Bus manager (host side).
 */
class JAiControlBus {
public:
    JAiControlBus() = default;
    ~JAiControlBus() { detach(); }

    JAiControlBus(const JAiControlBus&) = delete;
    JAiControlBus& operator=(const JAiControlBus&) = delete;

    /**
     * @brief Create (and own) a named POSIX shared-memory segment for true
     *        cross-process AI access.  Returns false if shm is unavailable, in which
     *        case the caller should fall back to an in-process attach().
     */
    bool createSegment(const char* name = DefaultBusName) {
#if defined(__linux__) || defined(__APPLE__)
        shm_unlink(name);  // clear any stale segment from a previous crash
        int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
        if (fd < 0) return false;
        if (ftruncate(fd, static_cast<off_t>(sizeof(SharedBusMemory))) != 0) {
            ::close(fd); shm_unlink(name); return false;
        }
        void* p = mmap(nullptr, sizeof(SharedBusMemory),
                       PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) { shm_unlink(name); return false; }
        std::memset(p, 0, sizeof(SharedBusMemory));  // zero-fill => valid atomics(0)
        m_mapped = p;
        m_owner  = true;
        std::strncpy(m_name, name, sizeof(m_name) - 1);
        attach(p);
        qCInfo(LogAiBus) << "AI JControl Bus shared segment '" << name << "' created ("
                         << sizeof(SharedBusMemory) << " bytes)." << std::endl;
        return true;
#else
        (void)name;
        return false;
#endif
    }

    /**
     * @brief Hook the bus into an existing memory segment (in-process fallback / tests).
     */
    void attach(void* memorySegment) {
        if (!memorySegment) {
            qCWarning(LogAiBus) << "Attempted to attach AI JControl Bus to a null segment." << std::endl;
            return;
        }
        m_sharedRegion = reinterpret_cast<SharedBusMemory*>(memorySegment);
        m_sharedRegion->magicCookie = SharedMagicCookie;
        m_sharedRegion->version     = SharedBusVersion;
        m_sharedRegion->generation.store(0, std::memory_order_release);
        m_sharedRegion->telemetryFrameCounter.store(0, std::memory_order_release);
        m_sharedRegion->nodeCount = 0;
        m_sharedRegion->outboundSignal.signalSeq.store(0, std::memory_order_release);
        m_sharedRegion->outboundSignal.targetId = 0xFFFFFFFF;
        m_sharedRegion->outboundSignal.signalName[0] = '\0';
        m_sharedRegion->outboundSignal.signalValue[0] = '\0';
        qCInfo(LogAiBus) << "AI JControl Bus attached to memory block successfully." << std::endl;
    }

    void detach() {
#if defined(__linux__) || defined(__APPLE__)
        if (m_mapped) { munmap(m_mapped, sizeof(SharedBusMemory)); m_mapped = nullptr; }
        if (m_owner && m_name[0]) { shm_unlink(m_name); m_name[0] = '\0'; }
        m_owner = false;
#endif
        m_sharedRegion = nullptr;
    }

    /**
     * @brief Publish a full semantic snapshot of the UI (the rich path).  Seqlock-guarded
     *        so external readers never see a torn frame.
     */
    void publishNodes(const AiNodeDescriptor* src, uint32_t count) {
        if (!m_sharedRegion) return;
        SharedBusMemory& r = *m_sharedRegion;
        if (count > MaxBusNodes) count = MaxBusNodes;

        r.generation.fetch_add(1, std::memory_order_acq_rel);   // -> odd: publish begins
        r.nodeCount = count;
        if (count) std::memcpy(r.nodes, src, count * sizeof(AiNodeDescriptor));
        r.telemetryFrameCounter.fetch_add(1, std::memory_order_acq_rel);
        r.generation.fetch_add(1, std::memory_order_acq_rel);   // -> even: publish ends
    }

    /**
     * @brief Geometry-only telemetry from the scene graph (legacy / minimal path).
     */
    void updateTelemetry(const class JSceneGraph& sceneGraph) {
        if (!m_sharedRegion) return;
        SharedBusMemory& r = *m_sharedRegion;
        size_t count = std::min(static_cast<size_t>(MaxBusNodes), sceneGraph.totalNodes());

        r.generation.fetch_add(1, std::memory_order_acq_rel);   // -> odd
        r.nodeCount = static_cast<uint32_t>(count);
        for (uint32_t i = 0; i < r.nodeCount; ++i) {
            auto& d = r.nodes[i];
            const auto& lo = const_cast<JSceneGraph&>(sceneGraph).getLayout(i);
            d = AiNodeDescriptor{};
            d.id     = i;
            d.x      = lo.boundingBox.x;
            d.y      = lo.boundingBox.y;
            d.width  = lo.boundingBox.width;
            d.height = lo.boundingBox.height;
        }
        r.telemetryFrameCounter.fetch_add(1, std::memory_order_acq_rel);
        r.generation.fetch_add(1, std::memory_order_acq_rel);   // -> even
    }

    /**
     * @brief Reader-side lock-free consistent snapshot (used by external agents/probes).
     *        Retries while the writer is mid-publish; returns false only if the segment
     *        is invalid or never settled.
     */
    static bool snapshot(const SharedBusMemory* r,
                         std::vector<AiNodeDescriptor>& out,
                         uint64_t* frameOut = nullptr) {
        if (!r || r->magicCookie != SharedMagicCookie || r->version != SharedBusVersion)
            return false;
        for (int tries = 0; tries < 256; ++tries) {
            uint64_t g1 = r->generation.load(std::memory_order_acquire);
            if (g1 & 1ull) continue;                      // publish in progress
            uint32_t n = r->nodeCount;
            if (n > MaxBusNodes) n = MaxBusNodes;
            out.resize(n);
            if (n) std::memcpy(out.data(), r->nodes, n * sizeof(AiNodeDescriptor));
            uint64_t frame = r->telemetryFrameCounter.load(std::memory_order_relaxed);
            uint64_t g2 = r->generation.load(std::memory_order_acquire);
            if (g1 == g2) { if (frameOut) *frameOut = frame; return true; }
        }
        return false;
    }

    /**
     * @brief Poll for a virtual input generated by the connected AI controller.
     */
    bool pollInboundCommand(AiVirtualInput& outCommand) {
        if (!m_sharedRegion) return false;
        uint32_t sharedSeq = m_sharedRegion->inboundCommand.sequenceId.load(std::memory_order_acquire);
        if (sharedSeq <= m_localSequenceTracker) return false;
        if (m_sharedRegion->inboundCommand.type.load(std::memory_order_relaxed) ==
            AiVirtualInput::JCommandType::None) return false;

        outCommand.type.store(m_sharedRegion->inboundCommand.type.load(std::memory_order_relaxed));
        outCommand.targetX    = m_sharedRegion->inboundCommand.targetX;
        outCommand.targetY    = m_sharedRegion->inboundCommand.targetY;
        outCommand.keyPayload = m_sharedRegion->inboundCommand.keyPayload;
        m_localSequenceTracker = sharedSeq;
        return true;
    }

    /**
     * @brief App side: poll for a pending semantic action.  Call on the UI thread; on
     *        true, dispatch the action to widget `targetId`, then ackAction(seq, result).
     */
    bool pollAction(uint32_t& targetId, char* actionOut, size_t actionCap, uint32_t& seqOut) {
        if (!m_sharedRegion) return false;
        uint32_t req = m_sharedRegion->inboundAction.requestSeq.load(std::memory_order_acquire);
        if (req == m_localActionSeq) return false;
        targetId = m_sharedRegion->inboundAction.targetId;
        if (actionCap) {
            std::strncpy(actionOut, m_sharedRegion->inboundAction.action, actionCap - 1);
            actionOut[actionCap - 1] = '\0';
        }
        seqOut = req;
        m_localActionSeq = req;
        return true;
    }

    /** App side: acknowledge a handled action so the waiting agent unblocks. */
    void ackAction(uint32_t seq, int result) {
        if (!m_sharedRegion) return;
        m_sharedRegion->inboundAction.resultCode = result;
        m_sharedRegion->inboundAction.ackSeq.store(seq, std::memory_order_release);
    }

    /** App side: publish a signal (event notification) onto the AI control bus. */
    void publishSignal(uint32_t targetId, std::string_view name, std::string_view value) {
        if (!m_sharedRegion) return;
        m_sharedRegion->outboundSignal.targetId = targetId;
        aiSetField(m_sharedRegion->outboundSignal.signalName, sizeof(m_sharedRegion->outboundSignal.signalName), name);
        aiSetField(m_sharedRegion->outboundSignal.signalValue, sizeof(m_sharedRegion->outboundSignal.signalValue), value);
        uint32_t seq = m_sharedRegion->outboundSignal.signalSeq.load(std::memory_order_relaxed) + 1;
        m_sharedRegion->outboundSignal.signalSeq.store(seq, std::memory_order_release);
    }

    /**
     * @brief Agent side (separate RDWR mapping): submit a semantic action and block
     *        until the app acknowledges.  Returns the app's resultCode, or -2 on timeout.
     */
    static int submitActionBlocking(SharedBusMemory* r, uint32_t targetId,
                                    const char* action, int timeoutMs = 1000) {
        if (!r) return -3;
        std::strncpy(r->inboundAction.action, action, sizeof(r->inboundAction.action) - 1);
        r->inboundAction.action[sizeof(r->inboundAction.action) - 1] = '\0';
        r->inboundAction.targetId = targetId;
        uint32_t seq = r->inboundAction.requestSeq.load(std::memory_order_relaxed) + 1;
        r->inboundAction.requestSeq.store(seq, std::memory_order_release);
        for (int i = 0; i < timeoutMs; ++i) {
            if (r->inboundAction.ackSeq.load(std::memory_order_acquire) == seq)
                return r->inboundAction.resultCode;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return -2;
    }

    SharedBusMemory* region() noexcept { return m_sharedRegion; }

private:
    SharedBusMemory* m_sharedRegion{nullptr};
    uint32_t         m_localSequenceTracker{0};
    uint32_t         m_localActionSeq{0};
    void*            m_mapped{nullptr};
    bool             m_owner{false};
    char             m_name[64]{0};
};

} // namespace Genesis
