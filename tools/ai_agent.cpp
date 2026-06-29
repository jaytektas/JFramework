// genesis-ai-agent — an external process that drives a running Genesis GUI purely
// through the shared-memory semantic bus: it finds controls by role/label, acts on
// them by node id (never pixels), and verifies the result by reading the state back.
// This is the "AI on the other side of the GUI" act-and-evaluate loop, at memory speed.
//
//   usage:
//     genesis_ai_agent [shm]                         # scripted demo
//     genesis_ai_agent [shm] --role R --find L --do ACTION
//
//   Inject a new widget at runtime (system target 0xFFFFFFFF):
//     genesis_ai_agent --inject button:Console:"My JButton"
//     genesis_ai_agent --inject checkbox:Properties:"Enable thing"
//     genesis_ai_agent --inject lineedit:Console:"Search..."
//     genesis_ai_agent --inject label:Inspector:"Status: OK"
//
//   Remove a widget by node id:
//     genesis_ai_agent --remove 42
//
//   Watch live user-interaction events from the app (Ctrl-C to exit):
//     genesis_ai_agent --watch-signals
//
#include <genesis/core/AiControlBus.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <string>
#include <csignal>
#include <atomic>

#if defined(__linux__) || defined(__APPLE__)
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <unistd.h>
#endif

using namespace jf;

static const AiNodeDescriptor* findNode(const std::vector<AiNodeDescriptor>& v,
                                        const char* role, const char* label) {
    for (const auto& d : v) {
        bool rok = !role  || !*role  || std::strcmp(d.role, role) == 0;
        bool lok = !label || !*label || std::strstr(d.name, label) != nullptr;
        if (rok && lok) return &d;
    }
    return nullptr;
}

static std::atomic<bool> g_stop{false};
static void sigHandler(int) { g_stop = true; }

int main(int argc, char** argv) {
#if !defined(__linux__) && !defined(__APPLE__)
    std::fprintf(stderr, "ai-agent: POSIX shared memory required\n");
    return 1;
#else
    const char* name    = DefaultBusName;
    const char* role    = nullptr;
    const char* label   = nullptr;
    const char* action  = nullptr;
    const char* inject  = nullptr;   // "<type>:<panel>:<label>"
    const char* removeW = nullptr;   // "<nodeId>"
    bool watchSignals   = false;
    bool dumpTree       = false;

    for (int i = 1; i < argc; ++i) {
        if      (!std::strcmp(argv[i], "--role")          && i+1 < argc) role       = argv[++i];
        else if (!std::strcmp(argv[i], "--find")          && i+1 < argc) label      = argv[++i];
        else if (!std::strcmp(argv[i], "--do")            && i+1 < argc) action     = argv[++i];
        else if (!std::strcmp(argv[i], "--inject")        && i+1 < argc) inject     = argv[++i];
        else if (!std::strcmp(argv[i], "--remove")        && i+1 < argc) removeW    = argv[++i];
        else if (!std::strcmp(argv[i], "--watch-signals"))               watchSignals= true;
        else if (!std::strcmp(argv[i], "--tree"))                        dumpTree    = true;
        else if (argv[i][0] != '-')                                      name        = argv[i];
    }

    // ---- Attach to the running app's shared bus ----
    int fd = -1;
    for (int i = 0; i < 100 && fd < 0; ++i) {
        fd = shm_open(name, O_RDWR, 0600);
        if (fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (fd < 0) {
        std::fprintf(stderr, "ai-agent: cannot open '%s' (app running?)\n", name);
        return 1;
    }
    void* p = mmap(nullptr, sizeof(SharedBusMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) { std::fprintf(stderr, "ai-agent: mmap failed\n"); return 1; }
    SharedBusMemory* bus = reinterpret_cast<SharedBusMemory*>(p);
    if (bus->magicCookie != SharedMagicCookie || bus->version != SharedBusVersion) {
        std::fprintf(stderr, "ai-agent: bus version mismatch (expected %u, got %u)\n",
                     SharedBusVersion, bus->version);
        return 1;
    }
    std::printf("ai-agent: attached to '%s'  (version=%u, %zu bytes)\n\n",
                name, bus->version, sizeof(SharedBusMemory));

    // Helper: dump the full semantic tree
    auto dumpSnapshot = [&]() {
        std::vector<AiNodeDescriptor> snap;
        JAiControlBus::snapshot(bus, snap);
        std::printf("  Semantic tree (%zu nodes):\n", snap.size());
        for (const auto& d : snap) {
            std::printf("    #%-4u  %-14s  %-28s  val='%s'  flags=%02x\n",
                        d.id, d.role, d.name, d.value, d.stateFlags);
        }
    };

    // Helper: find -> act -> verify (addressed by identity, never pixels)
    auto step = [&](const char* r, const char* l, const char* act) {
        std::vector<AiNodeDescriptor> snap;
        JAiControlBus::snapshot(bus, snap);
        const AiNodeDescriptor* t = findNode(snap, r, l);
        if (!t) {
            std::printf("  [skip]  no %s matching '%s'\n", r ? r : "*", l ? l : "*");
            return;
        }
        uint32_t id = t->id;
        char before[64];  std::strncpy(before,   t->value, 63); before[63] = '\0';
        char rolebuf[24]; std::strncpy(rolebuf,  t->role,  23); rolebuf[23]= '\0';
        char namebuf[32]; std::strncpy(namebuf,  t->name,  31); namebuf[31]= '\0';

        int res = JAiControlBus::submitActionBlocking(bus, id, act);

        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        std::vector<AiNodeDescriptor> snap2;
        JAiControlBus::snapshot(bus, snap2);
        const AiNodeDescriptor* t2 = findNode(snap2, r, l);
        const char* okstr = res == 1 ? "OK"
                          : res == 0 ? "not-understood"
                          : res == -1? "no-target" : "timeout/err";
        std::printf("  #%-4u %-12s '%-24s'  do:%-20s  '%s' -> '%s'  [%s]\n",
                    id, rolebuf, namebuf, act, before,
                    t2 ? t2->value : "?", okstr);
    };

    // Helper: system-level broadcast action (targetId == 0xFFFFFFFF)
    auto broadcast = [&](const char* act) {
        int res = JAiControlBus::submitActionBlocking(bus, 0xFFFFFFFFu, act);
        const char* okstr = res == 1 ? "OK"
                          : res == 0 ? "not-understood"
                          : res == -1? "no-target" : "timeout/err";
        std::printf("  [system]  %-40s  [%s]\n", act, okstr);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    };

    // ---- Dump tree ----
    if (dumpTree) {
        dumpSnapshot();
        munmap(p, sizeof(SharedBusMemory));
        return 0;
    }

    // ---- Watch outbound signals (user interactions reported by the app) ----
    if (watchSignals) {
        std::signal(SIGINT,  sigHandler);
        std::signal(SIGTERM, sigHandler);
        std::printf("Watching live user-interaction signals (Ctrl-C to stop)...\n");
        uint32_t lastSeq = bus->outboundSignal.signalSeq.load(std::memory_order_acquire);
        while (!g_stop) {
            uint32_t seq = bus->outboundSignal.signalSeq.load(std::memory_order_acquire);
            if (seq != lastSeq) {
                lastSeq = seq;
                char sigName[32], sigVal[64];
                std::strncpy(sigName, bus->outboundSignal.signalName,  31); sigName[31] = '\0';
                std::strncpy(sigVal,  bus->outboundSignal.signalValue, 63); sigVal[63]  = '\0';
                std::printf("  [event]  nodeId=#%-4u  signal=%-16s  value='%s'\n",
                             bus->outboundSignal.targetId, sigName, sigVal);
                std::fflush(stdout);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::printf("\nai-agent: signal watch stopped.\n");
        munmap(p, sizeof(SharedBusMemory));
        return 0;
    }

    // ---- Inject a widget at runtime ----
    if (inject) {
        std::string injectAction = std::string("inject:") + inject;
        broadcast(injectAction.c_str());
        dumpSnapshot();
        munmap(p, sizeof(SharedBusMemory));
        return 0;
    }

    // ---- Remove a widget by id ----
    if (removeW) {
        std::string removeAction = std::string("remove_widget:") + removeW;
        broadcast(removeAction.c_str());
        munmap(p, sizeof(SharedBusMemory));
        return 0;
    }

    // ---- Single explicit command ----
    if (action) {
        step(role, label, action);
        munmap(p, sizeof(SharedBusMemory));
        return 0;
    }

    // ---- Scripted demonstration ----
    std::printf("Driving the GUI by meaning (find -> act -> verify), no pixels:\n");
    step("JCheckBox",     "Show tooltips",  "check");
    step("JToggleButton", "Dark JMode",      "toggle");
    step("JRadioButton",  "Metal backend",  "select");
    step("JSlider",       "",               "set_value:0.90");
    step("JSpinBox",      "",               "increment");
    step("JButton",       "Primary JAction", "click");

    // Demo inject: add a button to the Console panel live
    std::printf("\nDynamic injection demo:\n");
    broadcast("inject:button:Console:AI-Injected JButton");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    broadcast("inject:label:Inspector:Live AI status: OK");

    std::printf("\nai-agent: done.\n");
    munmap(p, sizeof(SharedBusMemory));
    return 0;
#endif
}
