#pragma once

// JAiBus — the in-process server for the JFramework AI bus. Publishes a snapshot of the live widget tree
// to shared memory (from the accessibility data EVERY widget already exposes via a11yNode()) and services
// one action per frame, all on the MAIN thread.
//
// Design (why this one won't repeat the old bus's bugs):
//   * OPT-IN — off by default (env JF_AI_BUS or an explicit enable()); disabled = a single null check, zero
//     overhead, zero surface. The old bus was always-on and woven through every widget.
//   * ONE hook — JAppWindow calls tick() once per frame. No JAiBusHook::emit() scattered across Dialog /
//     SerialPort / Settings / PopupWindow / … and no getSemanticNode()/executeSemanticAction() virtuals on
//     every widget. All data comes from the accessibility layer that already exists.
//   * MAIN-THREAD ONLY — publish + action dispatch run in the render loop; widgets are never touched from
//     another thread. The shm frame is a seqlock so a reader can't tear a half-written snapshot.
//   * NON-INVASIVE actions — focus + click go through the widget's existing input methods; anything richer
//     is an OPTIONAL app-installed handler. The framework never grows per-widget AI code.

#include "JAiBusAbi.h"
#include "JWidget.h"
#include "FocusManager.h"

#include <functional>
#include <string>
#include <vector>
#include <new>

#if defined(__linux__) || defined(__APPLE__)
#  include <sys/mman.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

inline namespace jf {

class JAiBus {
public:
    static JAiBus& instance() { static JAiBus b; return b; }

    // Create + map the shared segment. Idempotent; a no-op (leaving the bus disabled) on unsupported
    // platforms or if the shm can't be created. JAppWindow calls this when env JF_AI_BUS is set.
    void enable(const char* name = kAiBusDefaultName) {
#if defined(__linux__) || defined(__APPLE__)
        if (m_shm) return;
        int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
        if (fd < 0) return;
        if (ftruncate(fd, sizeof(JAiBusShared)) != 0) { ::close(fd); return; }
        void* mem = mmap(nullptr, sizeof(JAiBusShared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);   // the mapping keeps the segment alive; the fd isn't needed after mmap
        if (mem == MAP_FAILED) return;
        m_shm = new (mem) JAiBusShared();   // placement-new: sets magic/version, zeroes atomics + nodes
#else
        (void)name;
#endif
    }

    bool enabled() const { return m_shm != nullptr; }

    // Optional richer-action handler (set_value:X, select:foo, …) the generic dispatch can't do without
    // per-widget knowledge. Return 1 handled / 0 not handled / -1 bad id. Runs on the MAIN thread.
    std::function<int(uint32_t id, const std::string& action)> onAction;

    // Once per frame, MAIN thread: service a pending action then publish the snapshot. Returns true if an
    // action was serviced this frame (so the caller can force a repaint). No-op + false when disabled.
    bool tick(const std::vector<JWidget*>& widgets, JFocusManager* focus) {
        if (!m_shm) return false;
        const bool acted = serviceAction(widgets, focus);
        publish(widgets);
        return acted;
    }

private:
    bool serviceAction(const std::vector<JWidget*>& widgets, JFocusManager* focus) {
        const uint32_t req = m_shm->action.requestSeq.load(std::memory_order_acquire);
        if (req == m_lastHandled) return false;
        int rc = dispatchDefault(widgets, focus, m_shm->action.targetId, m_shm->action.action);
        if (rc == 0 && onAction) rc = onAction(m_shm->action.targetId, m_shm->action.action);
        m_shm->action.resultCode = rc;
        m_shm->action.ackSeq.store(req, std::memory_order_release);   // ack last: client waits on this
        m_lastHandled = req;
        return true;
    }

    // Generic actions only — focus + click via the widget's existing input methods (no AI virtuals).
    // Everything else returns 0 so the optional app handler gets a shot.
    int dispatchDefault(const std::vector<JWidget*>& widgets, JFocusManager* focus,
                        uint32_t id, const std::string& action) {
        JWidget* w = nullptr;
        for (JWidget* c : widgets) if (c && static_cast<uint32_t>(c->getNodeId()) == id) { w = c; break; }
        if (!w) return -1;
        if (action == "focus") { if (focus) focus->setFocus(w); return 1; }
        if (action == "click") {
            const JRect b = w->bounds();
            const float cx = b.x + b.width * 0.5f, cy = b.y + b.height * 0.5f;
            w->handleMousePress(cx, cy);
            w->handleMouseRelease(cx, cy);
            return 1;
        }
        return 0;
    }

    void publish(const std::vector<JWidget*>& widgets) {
        m_shm->seq.fetch_add(1, std::memory_order_release);   // odd → write in progress
        uint32_t n = 0;
        for (JWidget* w : widgets) {
            if (!w || !w->isVisible() || n >= kAiBusMaxNodes) continue;
            const JA11yNode a = w->a11yNode();   // fully populated: role/name/value/state/geometry/range
            JAiBusNode& node = m_shm->nodes[n++];
            node.id = a.id;
            node.stateFlags = a.stateFlags;
            node.x = a.x; node.y = a.y; node.w = a.width; node.h = a.height;
            node.hasRange = a.hasRange ? 1u : 0u;
            node.curValue = a.curValue; node.minValue = a.minValue; node.maxValue = a.maxValue;
            aiBusCopy(node.role,  sizeof(node.role),  a.role);
            aiBusCopy(node.name,  sizeof(node.name),  a.name);
            aiBusCopy(node.value, sizeof(node.value), a.value);
        }
        m_shm->nodeCount = n;
        m_shm->seq.fetch_add(1, std::memory_order_release);   // even → frame complete
    }

    JAiBusShared* m_shm = nullptr;
    uint32_t      m_lastHandled = 0;
};

} // inline namespace jf
