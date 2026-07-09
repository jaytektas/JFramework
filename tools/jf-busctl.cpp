// jf-busctl — client for the JFramework AI bus (/jframework_ai_bus).
//   jf-busctl dump                    list every visible widget node (id role "name" = "value" ...)
//   jf-busctl find <substr>           nodes whose line contains substr
//   jf-busctl act <id> <action>       send an action (click, focus, set_value:0.5, select:foo); waits for ack
//
// Reads the node array under the server's seqlock so it never consumes a half-written frame. Build:
//   g++ -std=c++20 -I$HOME/jframework-sdk/include jf-busctl.cpp -o jf-busctl -lrt

#include <j/core/JAiBusAbi.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

using namespace jf;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: jf-busctl dump | find <s> | act <id> <action>\n"); return 2; }

    int fd = shm_open(kAiBusDefaultName, O_RDWR, 0600);
    if (fd < 0) { std::fprintf(stderr, "no bus (%s) — is the app running with JF_AI_BUS=1?\n", kAiBusDefaultName); return 1; }
    void* mem = mmap(nullptr, sizeof(JAiBusShared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (mem == MAP_FAILED) { std::perror("mmap"); return 1; }
    auto* bus = static_cast<JAiBusShared*>(mem);
    if (bus->magic != kAiBusMagic)       { std::fprintf(stderr, "bad magic 0x%08x\n", bus->magic); return 1; }
    if (bus->version != kAiBusVersion)   { std::fprintf(stderr, "ABI mismatch: bus v%u, client v%u\n", bus->version, kAiBusVersion); return 1; }

    const std::string cmd = argv[1];

    if (cmd == "dump" || cmd == "find") {
        const std::string needle = (cmd == "find" && argc > 2) ? argv[2] : "";
        // Seqlock read: sample, copy, re-sample; retry while odd (write in progress) or changed.
        static JAiBusNode local[kAiBusMaxNodes];
        uint32_t count = 0;
        for (int tries = 0; tries < 200; ++tries) {
            const uint64_t s1 = bus->seq.load(std::memory_order_acquire);
            if (s1 & 1u) { usleep(200); continue; }
            count = bus->nodeCount; if (count > kAiBusMaxNodes) count = kAiBusMaxNodes;
            std::memcpy(local, bus->nodes, sizeof(JAiBusNode) * count);
            const uint64_t s2 = bus->seq.load(std::memory_order_acquire);
            if (s1 == s2) break;
            usleep(200);
        }
        std::printf("nodeCount=%u seq=%llu\n", count, (unsigned long long)bus->seq.load());
        for (uint32_t i = 0; i < count; ++i) {
            const JAiBusNode& d = local[i];
            char line[256];
            std::snprintf(line, sizeof(line), "%u  [%s]  \"%s\" = \"%s\"  @(%.0f,%.0f %gx%g) flags=%u",
                          d.id, d.role, d.name, d.value, d.x, d.y, d.w, d.h, d.stateFlags);
            if (needle.empty() || std::string(line).find(needle) != std::string::npos)
                std::printf("%s\n", line);
        }
        return 0;
    }

    if (cmd == "act" && argc >= 4) {
        const uint32_t tid = static_cast<uint32_t>(strtoul(argv[2], nullptr, 10));
        bus->action.targetId = tid;
        bus->action.resultCode = 0;
        aiBusCopy(bus->action.action, sizeof(bus->action.action), argv[3]);
        const uint32_t req = bus->action.requestSeq.load() + 1;
        bus->action.requestSeq.store(req, std::memory_order_release);   // submit LAST
        bool acked = false;
        for (int i = 0; i < 400; ++i) { if (bus->action.ackSeq.load(std::memory_order_acquire) == req) { acked = true; break; } usleep(5000); }
        std::printf("act id=%u action='%s' acked=%d resultCode=%d\n", tid, argv[3], (int)acked, bus->action.resultCode);
        return acked ? 0 : 3;
    }

    std::fprintf(stderr, "bad args\n");
    return 2;
}
