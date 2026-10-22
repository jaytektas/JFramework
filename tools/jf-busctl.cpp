// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// jf-busctl — client for the JFramework AI bus (an app started with JF_AI_BUS=1).
//   jf-busctl dump [--json]             every visible widget node (id role "name" = "value" @rect)
//   jf-busctl find <substr> [--json]    the nodes whose line contains substr
//   jf-busctl act <id|name> <action>    click, focus, type:<text>, key:Return, … ; waits for the ack
//   jf-busctl call <action>             an action addressed at the APP, not a widget (its own commands)
//   jf-busctl get <name>                one node's value, nothing else — for scripts
//   jf-busctl wait <substr> [secs]      block until a node matches (default 10s); exit 0 found, 4 timeout
//
// An action of "-" is read from stdin, for one too long for a command line (a table's cells to write).
// Whatever the app replies is printed to stdout; the exit code says whether it was handled:
//   0 handled · 1 no bus · 2 usage · 3 no ack · 4 not found / not handled / refused · 5 ambiguous ·
//   6 bad target
// JF_BUSCTL_TIMEOUT (seconds, default 10) is how long `act` / `call` wait for the ack.
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

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/mman.h>
#  include <unistd.h>
#endif
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

using namespace jf;

static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static void sleepUs(int us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }

static const char* kUsage =
    "usage: jf-busctl dump [--json] | find <s> [--json] | get <name> | wait <s> [secs]\n"
    "                 | act <id|name> <action|-> | call <action|->\n";

// Map the app's segment. nullptr (and a message) when there is no bus, or it belongs to nobody.
static JAiBusShared* openBus() {
#if defined(_WIN32)
    HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, kAiBusDefaultName);
    if (!map) { std::fprintf(stderr, "no bus (%s) — is the app running with JF_AI_BUS=1?\n", kAiBusDefaultName); return nullptr; }
    void* mem = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(JAiBusShared));
    if (!mem) { std::fprintf(stderr, "cannot map the bus (error %lu)\n", GetLastError()); return nullptr; }
#else
    int fd = shm_open(kAiBusDefaultName, O_RDWR, 0600);
    if (fd < 0) { std::fprintf(stderr, "no bus (%s) — is the app running with JF_AI_BUS=1?\n", kAiBusDefaultName); return nullptr; }
    void* mem = mmap(nullptr, sizeof(JAiBusShared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (mem == MAP_FAILED) { std::perror("mmap"); return nullptr; }
#endif
    auto* bus = static_cast<JAiBusShared*>(mem);
    if (bus->magic != kAiBusMagic)     { std::fprintf(stderr, "bad magic 0x%08x\n", bus->magic); return nullptr; }
    if (bus->version != kAiBusVersion) { std::fprintf(stderr, "ABI mismatch: bus v%u, client v%u\n", bus->version, kAiBusVersion); return nullptr; }

    // A segment whose app has gone (it died without removing it) still holds that app's last frame: read
    // as live, it shows widgets and dialogs that are no longer there, and every action waits for an ack
    // that never comes. (A Windows mapping goes with its last handle, so only POSIX can be stale.)
#if !defined(_WIN32)
    if (bus->ownerPid > 0 && kill(bus->ownerPid, 0) != 0 && errno == ESRCH) {
        std::fprintf(stderr, "stale bus (%s): the app that published it (pid %d) is no longer running — "
                             "is the app running with JF_AI_BUS=1?\n", kAiBusDefaultName, bus->ownerPid);
        return nullptr;
    }
#endif
    return bus;
}

// JSON string escaping for the --json dump: a name with a quote in it is still one field.
static std::string jsonEsc(const char* s) {
    std::string o;
    for (; *s; ++s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fputs(kUsage, stderr); return 2; }
    JAiBusShared* bus = openBus();
    if (!bus) return 1;

    const std::string cmd = argv[1];

    // One seqlock read of the node array — sample, copy, re-sample, retry while odd or changed.
    static JAiBusNode local[kAiBusMaxNodes];
    auto snapshot = [&](uint32_t& count) {
        count = 0;
        for (int tries = 0; tries < 200; ++tries) {
            const uint64_t s1 = bus->seq.load(std::memory_order_acquire);
            if (s1 & 1u) { sleepUs(200); continue; }
            count = bus->nodeCount; if (count > kAiBusMaxNodes) count = kAiBusMaxNodes;
            std::memcpy(local, bus->nodes, sizeof(JAiBusNode) * count);
            const uint64_t s2 = bus->seq.load(std::memory_order_acquire);
            if (s1 == s2) return;
            sleepUs(200);
        }
    };
    auto lineOf = [](const JAiBusNode& d) {
        char line[320];
        std::snprintf(line, sizeof(line), "%u  [%s]  \"%s\" = \"%s\"  @(%.0f,%.0f %gx%g) flags=%u",
                      d.id, d.role, d.name, d.value, d.x, d.y, d.w, d.h, d.stateFlags);
        return std::string(line);
    };
    auto jsonOf = [](const JAiBusNode& d) {
        char geo[160];
        std::snprintf(geo, sizeof(geo), "\"x\":%.0f,\"y\":%.0f,\"w\":%g,\"h\":%g,\"flags\":%u",
                      d.x, d.y, d.w, d.h, d.stateFlags);
        return "{\"id\":" + std::to_string(d.id) + ",\"role\":\"" + jsonEsc(d.role) + "\",\"name\":\""
             + jsonEsc(d.name) + "\",\"value\":\"" + jsonEsc(d.value) + "\"," + geo + "}";
    };
    const bool asJson = [&] {
        for (int i = 2; i < argc; ++i) if (std::string(argv[i]) == "--json") return true;
        return false;
    }();

    // Submit one action and wait for its ack; print the reply. "-" reads the action from stdin.
    auto submit = [&](uint32_t tid, std::string action) -> int {
        if (action == "-") {
            action.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
            while (!action.empty() && (action.back() == '\n' || action.back() == '\r')) action.pop_back();
        }
        if (action.size() >= kAiBusActionMax) {   // refused, never truncated: a cut action does the wrong thing
            std::fprintf(stderr, "action is %zu bytes; the bus takes at most %u\n", action.size(), kAiBusActionMax - 1);
            return 2;
        }
        bus->action.targetId = tid;
        bus->action.resultCode = 0;
        std::memcpy(bus->action.action, action.data(), action.size());
        bus->action.action[action.size()] = '\0';
        const uint32_t req = bus->action.requestSeq.load() + 1;
        bus->action.requestSeq.store(req, std::memory_order_release);   // submit LAST
        const char* t = std::getenv("JF_BUSCTL_TIMEOUT");
        const double secs = (t && *t) ? std::atof(t) : 10.0;
        bool acked = false;
        for (int i = 0; i < static_cast<int>(secs * 200); ++i) {
            if (bus->action.ackSeq.load(std::memory_order_acquire) == req) { acked = true; break; }
            sleepMs(5);
        }
        if (!acked) { std::fprintf(stderr, "no ack within %.1fs — is the app's window running?\n", secs); return 3; }
        const int rc = bus->action.resultCode;
        const uint32_t n = bus->action.replyLen < kAiBusReplyMax ? bus->action.replyLen : kAiBusReplyMax;
        if (n) { std::fwrite(bus->action.reply, 1, n, stdout); if (bus->action.reply[n - 1] != '\n') std::fputc('\n', stdout); }
        else   std::fprintf(stderr, "resultCode=%d\n", rc);
        return rc == 1 ? 0 : rc == -1 ? 6 : 4;   // 2 (refused) and 0 (not handled) are both 4
    };

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
            sleepMs(50);
        }
        std::fprintf(stderr, "timeout: nothing matching '%s' within %.1fs\n", needle.c_str(), secs);
        return 4;
    }

    if (cmd == "dump" || cmd == "find") {
        const std::string needle = (cmd == "find" && argc > 2 && std::string(argv[2]) != "--json")
                                 ? argv[2] : "";
        uint32_t count = 0; snapshot(count);
        if (asJson) std::printf("[");
        else std::printf("nodeCount=%u dropped=%u seq=%llu\n", count, bus->droppedNodes,
                         (unsigned long long)bus->seq.load());
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

    // "call" — the app's own commands. No widget is the target (id 0 means "nothing"), so the generic
    // dispatch passes and the app's handler answers.
    if (cmd == "call" && argc >= 3) return submit(0, argv[2]);

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
        return submit(tid, argv[3]);
    }

    std::fputs(kUsage, stderr);
    return 2;
}
