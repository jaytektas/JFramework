// genesis-ai-agent — an external process that drives a running Genesis GUI purely
// through the shared-memory semantic bus: it finds controls by role/label, acts on
// them by node id (never pixels), and verifies the result by reading the state back.
// This is the "AI on the other side of the GUI" act-and-evaluate loop, at memory speed.
//
//   usage: genesis_ai_agent [shm]                       # scripted demo
//          genesis_ai_agent [shm] --role R --find L --do ACTION
//
#include <genesis/core/AiControlBus.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>

#if defined(__linux__) || defined(__APPLE__)
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <unistd.h>
#endif

using namespace Genesis;

static const AiNodeDescriptor* find(const std::vector<AiNodeDescriptor>& v,
                                    const char* role, const char* label) {
    for (const auto& d : v) {
        bool rok = !role  || !*role  || std::strcmp(d.role, role) == 0;
        bool lok = !label || !*label || std::strstr(d.name, label) != nullptr;
        if (rok && lok) return &d;
    }
    return nullptr;
}

int main(int argc, char** argv) {
#if !defined(__linux__) && !defined(__APPLE__)
    std::fprintf(stderr, "ai-agent: POSIX shared memory required\n");
    return 1;
#else
    const char* name = DefaultBusName;
    const char* role = nullptr, *label = nullptr, *action = nullptr;
    for (int i = 1; i < argc; ++i) {
        if      (!std::strcmp(argv[i], "--role") && i+1 < argc) role   = argv[++i];
        else if (!std::strcmp(argv[i], "--find") && i+1 < argc) label  = argv[++i];
        else if (!std::strcmp(argv[i], "--do")   && i+1 < argc) action = argv[++i];
        else if (argv[i][0] != '-')                              name   = argv[i];
    }

    int fd = -1;
    for (int i = 0; i < 100 && fd < 0; ++i) {
        fd = shm_open(name, O_RDWR, 0600);
        if (fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (fd < 0) { std::fprintf(stderr, "ai-agent: cannot open '%s' (app running?)\n", name); return 1; }
    void* p = mmap(nullptr, sizeof(SharedBusMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) { std::fprintf(stderr, "ai-agent: mmap failed\n"); return 1; }
    SharedBusMemory* bus = reinterpret_cast<SharedBusMemory*>(p);
    if (bus->magicCookie != SharedMagicCookie || bus->version != SharedBusVersion) {
        std::fprintf(stderr, "ai-agent: bus version mismatch\n"); return 1;
    }
    std::printf("ai-agent: attached to '%s'\n\n", name);

    // find -> act -> verify, all by identity & meaning
    auto step = [&](const char* r, const char* l, const char* act) {
        std::vector<AiNodeDescriptor> snap;
        AiControlBus::snapshot(bus, snap);
        const AiNodeDescriptor* t = find(snap, r, l);
        if (!t) { std::printf("  [skip]  no %s matching '%s'\n", r ? r : "*", l ? l : "*"); return; }
        uint32_t id = t->id;
        char before[24]; std::strncpy(before, t->value, sizeof(before)); before[23] = '\0';
        char rolebuf[24]; std::strncpy(rolebuf, t->role, sizeof(rolebuf)); rolebuf[23] = '\0';
        char namebuf[32]; std::strncpy(namebuf, t->name, sizeof(namebuf)); namebuf[31] = '\0';

        int res = AiControlBus::submitActionBlocking(bus, id, act);

        std::this_thread::sleep_for(std::chrono::milliseconds(60));  // let the app republish
        std::vector<AiNodeDescriptor> snap2;
        AiControlBus::snapshot(bus, snap2);
        const AiNodeDescriptor* t2 = find(snap2, r, l);
        const char* okstr = res == 1 ? "OK" : res == 0 ? "not-understood"
                          : res == -1 ? "no-target" : "timeout/err";
        std::printf("  #%-3u %-12s '%-18s'  do:%-16s  '%s' -> '%s'   [%s]\n",
                    id, rolebuf, namebuf, act, before,
                    t2 ? t2->value : "?", okstr);
    };

    if (action) {                       // single explicit command
        step(role, label, action);
    } else {                            // scripted demonstration
        std::printf("Driving the GUI by meaning (find -> act -> verify), no pixels:\n");
        step("CheckBox",     "Show tooltips",  "check");
        step("ToggleButton", "Dark Mode",      "toggle");
        step("RadioButton",  "Metal backend",  "select");
        step("Slider",       "",               "set_value:0.90");
        step("SpinBox",      "",               "increment");
        step("Button",       "Primary Action", "click");
    }
    std::printf("\nai-agent: done.\n");

    munmap(p, sizeof(SharedBusMemory));
    return 0;
#endif
}
