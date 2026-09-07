// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// jf-busctl — client for the JFramework AI bus (/jframework_ai_bus).
//   jf-busctl dump [--json]             every visible widget node (id role "name" = "value" @rect)
//   jf-busctl find <substr> [--json]    the nodes whose line contains substr
//   jf-busctl act <id|name> <action>    click, focus, set_value:0.5, select:foo, … ; waits for the ack
//   jf-busctl get <name>                one node's value, nothing else — for scripts
//   jf-busctl wait <substr> [secs]      block until a node matches (default 10s); exit 0 found, 4 timeout
//
// ADDRESSING BY NAME is why `act` takes either: ids are stable while a widget lives but mean nothing
// across a restart, and a script that hard-codes one breaks the first time a dock opens in a different
// order. A name is what the thing IS. An ambiguous name is an error rather than a guess — acting on the
// wrong widget is worse than not acting.
//
// WAIT exists because the alternative is sleeping and hoping. The UI settles when it settles: a page
// arrives when its document loads, a value when the ECU answers. Polling for the thing you are waiting
// for takes as long as it takes and no longer.
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
#include <vector>

using namespace jf;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: jf-busctl dump [--json] | find <s> [--json] | get <name>\n"
                             "                  | wait <s> [secs] | act <id|name> <action>\n");
        return 2;
    }

    int fd = shm_open(kAiBusDefaultName, O_RDWR, 0600);
    if (fd < 0) { std::fprintf(stderr, "no bus (%s) — is the app running with JF_AI_BUS=1?\n", kAiBusDefaultName); return 1; }
    void* mem = mmap(nullptr, sizeof(JAiBusShared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (mem == MAP_FAILED) { std::perror("mmap"); return 1; }
    auto* bus = static_cast<JAiBusShared*>(mem);
    if (bus->magic != kAiBusMagic)       { std::fprintf(stderr, "bad magic 0x%08x\n", bus->magic); return 1; }
    if (bus->version != kAiBusVersion)   { std::fprintf(stderr, "ABI mismatch: bus v%u, client v%u\n", bus->version, kAiBusVersion); return 1; }

    const std::string cmd = argv[1];

    // One seqlock read of the node array — sample, copy, re-sample, retry while odd or changed.
    static JAiBusNode local[kAiBusMaxNodes];
    auto snapshot = [&](uint32_t& count) {
        count = 0;
        for (int tries = 0; tries < 200; ++tries) {
            const uint64_t s1 = bus->seq.load(std::memory_order_acquire);
            if (s1 & 1u) { usleep(200); continue; }
            count = bus->nodeCount; if (count > kAiBusMaxNodes) count = kAiBusMaxNodes;
            std::memcpy(local, bus->nodes, sizeof(JAiBusNode) * count);
            const uint64_t s2 = bus->seq.load(std::memory_order_acquire);
            if (s1 == s2) return;
            usleep(200);
        }
    };
    auto lineOf = [](const JAiBusNode& d) {
        char line[256];
        std::snprintf(line, sizeof(line), "%u  [%s]  \"%s\" = \"%s\"  @(%.0f,%.0f %gx%g) flags=%u",
                      d.id, d.role, d.name, d.value, d.x, d.y, d.w, d.h, d.stateFlags);
        return std::string(line);
    };
    auto jsonOf = [](const JAiBusNode& d) {
        char line[420];
        std::snprintf(line, sizeof(line),
                      "{\"id\":%u,\"role\":\"%s\",\"name\":\"%s\",\"value\":\"%s\","
                      "\"x\":%.0f,\"y\":%.0f,\"w\":%g,\"h\":%g,\"flags\":%u}",
                      d.id, d.role, d.name, d.value, d.x, d.y, d.w, d.h, d.stateFlags);
        return std::string(line);
    };
    const bool asJson = [&] {
        for (int i = 2; i < argc; ++i) if (std::string(argv[i]) == "--json") return true;
        return false;
    }();

    // "get" — one node's value and nothing else, so a script can read a reading without parsing a dump.
    if (cmd == "get" && argc >= 3) {
        uint32_t count = 0; snapshot(count);
        const std::string needle = argv[2];
        for (uint32_t i = 0; i < count; ++i)
            if (std::string(local[i].name) == needle) { std::printf("%s\n", local[i].value); return 0; }
        for (uint32_t i = 0; i < count; ++i)
            if (std::string(local[i].name).find(needle) != std::string::npos) {
                std::printf("%s\n", local[i].value); return 0;
            }
        std::fprintf(stderr, "no node named '%s'\n", needle.c_str());
        return 4;
    }

    // "wait" — poll until something matches, then print it. The point is not to sleep a fixed guess.
    if (cmd == "wait" && argc >= 3) {
        const std::string needle = argv[2];
        const double secs = (argc > 3) ? atof(argv[3]) : 10.0;
        for (int i = 0; i < static_cast<int>(secs * 20); ++i) {
            uint32_t count = 0; snapshot(count);
            for (uint32_t k = 0; k < count; ++k) {
                const std::string l = lineOf(local[k]);
                if (l.find(needle) != std::string::npos) { std::printf("%s\n", l.c_str()); return 0; }
            }
            usleep(50000);
        }
        std::fprintf(stderr, "timeout: nothing matching '%s' within %.1fs\n", needle.c_str(), secs);
        return 4;
    }

    if (cmd == "dump" || cmd == "find") {
        const std::string needle = (cmd == "find" && argc > 2 && std::string(argv[2]) != "--json")
                                 ? argv[2] : "";
        uint32_t count = 0; snapshot(count);
        if (asJson) std::printf("[");
        else std::printf("nodeCount=%u seq=%llu\n", count, (unsigned long long)bus->seq.load());
        bool first = true;
        for (uint32_t i = 0; i < count; ++i) {
            const std::string line = lineOf(local[i]);
            if (!needle.empty() && line.find(needle) == std::string::npos) continue;
            if (asJson) { std::printf("%s%s", first ? "" : ",", jsonOf(local[i]).c_str()); first = false; }
            else        std::printf("%s\n", line.c_str());
        }
        if (asJson) std::printf("]\n");
        return 0;
    }

    if (cmd == "act" && argc >= 4) {
        // An id, or a name. A name that matches more than one node is refused: acting on the wrong widget
        // is worse than not acting, and "the first one" is not an answer anybody wants from a tool.
        const std::string target = argv[2];
        uint32_t tid = 0;
        if (!target.empty() && target.find_first_not_of("0123456789") == std::string::npos) {
            tid = static_cast<uint32_t>(strtoul(target.c_str(), nullptr, 10));
        } else {
            uint32_t count = 0; snapshot(count);
            std::vector<uint32_t> exact, partial;
            for (uint32_t i = 0; i < count; ++i) {
                const std::string n = local[i].name;
                if (n == target) exact.push_back(local[i].id);
                else if (n.find(target) != std::string::npos) partial.push_back(local[i].id);
            }
            const std::vector<uint32_t>& hits = exact.empty() ? partial : exact;
            if (hits.empty()) { std::fprintf(stderr, "no node named '%s'\n", target.c_str()); return 4; }
            if (hits.size() > 1) {
                std::fprintf(stderr, "'%s' matches %zu nodes — name it exactly, or use an id:\n",
                             target.c_str(), hits.size());
                for (uint32_t i = 0; i < count; ++i)
                    for (uint32_t h : hits)
                        if (local[i].id == h) std::fprintf(stderr, "   %s\n", lineOf(local[i]).c_str());
                return 5;
            }
            tid = hits.front();
        }
        bus->action.targetId = tid;
        bus->action.resultCode = 0;
        aiBusCopy(bus->action.action, sizeof(bus->action.action), argv[3]);
        const uint32_t req = bus->action.requestSeq.load() + 1;
        bus->action.requestSeq.store(req, std::memory_order_release);   // submit LAST
        bool acked = false;
        for (int i = 0; i < 400; ++i) { if (bus->action.ackSeq.load(std::memory_order_acquire) == req) { acked = true; break; } usleep(5000); }
        std::printf("act id=%u action='%s' acked=%d resultCode=%d\n", tid, argv[3], (int)acked,
                    bus->action.resultCode);
        return acked ? 0 : 3;
    }

    std::fprintf(stderr, "usage: jf-busctl dump [--json] | find <s> [--json] | get <name>\n"
                         "                  | wait <s> [secs] | act <id|name> <action>\n");
    return 2;
}
