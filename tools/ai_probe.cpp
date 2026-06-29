// genesis-ai-probe — an external process that attaches to a running Genesis app's
// shared-memory AI bus and reads its live semantic tree.  No screenshots, no X, no
// pixels: it sees every control by identity, role, label, value and state, at memory
// speed.  This is the "AI on the other side of the GUI" read channel.
//
//   usage: genesis_ai_probe [shm_name] [--frames N]
//
#include <genesis/core/AiControlBus.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <thread>
#include <chrono>

#if defined(__linux__) || defined(__APPLE__)
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <unistd.h>
#endif

using namespace Genesis;

int main(int argc, char** argv) {
    const char* name   = DefaultBusName;
    int         frames = 8;                 // distinct snapshots to print, then exit
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (argv[i][0] != '-')                                name   = argv[i];
    }

#if !defined(__linux__) && !defined(__APPLE__)
    std::fprintf(stderr, "ai-probe: POSIX shared memory required\n");
    return 1;
#else
    // Wait (up to ~10s) for the app to create the segment.
    int fd = -1;
    for (int i = 0; i < 100 && fd < 0; ++i) {
        fd = shm_open(name, O_RDONLY, 0600);
        if (fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (fd < 0) {
        std::fprintf(stderr, "ai-probe: could not open '%s' (is a Genesis app running?)\n", name);
        return 1;
    }
    void* p = mmap(nullptr, sizeof(SharedBusMemory), PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) { std::fprintf(stderr, "ai-probe: mmap failed\n"); return 1; }

    const SharedBusMemory* bus = reinterpret_cast<const SharedBusMemory*>(p);
    std::printf("ai-probe: attached to '%s'  (magic=%#x version=%u, %zu bytes)\n",
                name, bus->magicCookie, bus->version, sizeof(SharedBusMemory));

    std::vector<AiNodeDescriptor> nodes;
    uint64_t lastFrame = ~0ull;
    int printed = 0;
    for (int iter = 0; iter < 600 && printed < frames; ++iter) {
        uint64_t frame = 0;
        if (JAiControlBus::snapshot(bus, nodes, &frame) && frame != lastFrame) {
            lastFrame = frame;
            ++printed;
            std::printf("\n=== AI view  (publish #%llu, %zu controls) ===\n",
                        (unsigned long long)frame, nodes.size());
            for (const auto& n : nodes) {
                char flags[8];
                std::snprintf(flags, sizeof(flags), "%c%c%c%c%c",
                    (n.stateFlags & AiEnabled)      ? 'E' : '-',
                    (n.stateFlags & AiVisible)      ? 'V' : '-',
                    (n.stateFlags & AiInteractable) ? 'I' : '-',
                    (n.stateFlags & AiFocused)      ? 'F' : '-',
                    (n.stateFlags & AiPressed)      ? 'P' : '-');
                std::printf("  #%-3u %-10s '%-18s' val='%s'  [%s]  @(%.0f,%.0f  %.0fx%.0f)\n",
                            n.id, n.role, n.name, n.value, flags,
                            n.x, n.y, n.width, n.height);
            }
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    munmap(p, sizeof(SharedBusMemory));
    return 0;
#endif
}
