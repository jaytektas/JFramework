#pragma once

// Thread-safety: MAIN THREAD ONLY.
// Dock layout is updated during the render loop on the main thread.

#include <vector>
#include <optional>
#include <algorithm>
#include <cstdint>

inline namespace jf {

class JDockHost;

// ============================================================================
// JDockRegistry — global list of all live JDockHost instances and their current
// screen rectangles.
//
// JFloatingDockWindow queries this every frame during a drag to find which host
// is under the cursor, then calls updateDrag() on it directly.  No hardcoded
// window bounds needed in the application loop.
//
// Usage:
//   // At startup (after the host's layout is computed):
//   JDockRegistry::instance().registerHost(host, win.screenX(), win.screenY(), W, H);
//
//   // Each frame (window may have been moved by the WM):
//   JDockRegistry::instance().updateBounds(host, win.screenX(), win.screenY(), W, H);
//
//   // At shutdown:
//   JDockRegistry::instance().unregisterHost(host);
// ============================================================================
struct JDockOptions {
    std::optional<float> handleHoverPad;
    std::optional<bool>  enforceMinSizes;
    std::optional<bool>  showResizeCursors;
};

class JDockRegistry {
public:
    static JDockRegistry& instance() {
        static JDockRegistry reg;
        return reg;
    }

    JDockOptions& defaultOptions() { return m_defaultOptions; }
    const JDockOptions& defaultOptions() const { return m_defaultOptions; }

    struct JEntry {
        JDockHost* host{nullptr};
        int       screenX{0}, screenY{0};   // hit bounds (where the host visually is)
        uint32_t  width{0},   height{0};
        int       originX{0}, originY{0};   // coord origin (subtracted to get host-local)
    };

    void registerHost(JDockHost& host, int sx, int sy, uint32_t w, uint32_t h) {
        registerHostEx(host, sx, sy, w, h, sx, sy);   // origin == hit bounds
    }

    // Register with a separate coordinate origin: the host occupies the screen rect
    // (hitSx,hitSy,w,h) but its node coordinates are measured from (originSx,originSy).
    // Used by JDockSpace — each area host occupies its area rect but shares the window's
    // coordinate origin, so its nodes stay in window space (no per-area render offset).
    void registerHostEx(JDockHost& host, int hitSx, int hitSy, uint32_t w, uint32_t h,
                        int originSx, int originSy) {
        for (auto& e : m_entries) {
            if (e.host == &host) { e.screenX = hitSx; e.screenY = hitSy; e.width = w; e.height = h;
                                   e.originX = originSx; e.originY = originSy; return; }
        }
        m_entries.push_back({&host, hitSx, hitSy, w, h, originSx, originSy});
    }

    void unregisterHost(JDockHost& host) {
        m_entries.erase(
            std::remove_if(m_entries.begin(), m_entries.end(),
                [&](const JEntry& e){ return e.host == &host; }),
            m_entries.end());
    }

    // Upsert: registers if not present, updates bounds otherwise.
    void updateBounds(JDockHost& host, int sx, int sy, uint32_t w, uint32_t h) {
        registerHost(host, sx, sy, w, h);
    }

    struct JHitResult {
        JDockHost* host;
        float     localX, localY;
    };

    // Returns the first registered host whose screen rect contains (gx, gy),
    // plus the cursor position in that host's local coordinate space.
    std::optional<JHitResult> hitTest(int globalX, int globalY) const {
        for (const auto& e : m_entries) {
            if (globalX >= e.screenX && globalX < e.screenX + static_cast<int>(e.width) &&
                globalY >= e.screenY && globalY < e.screenY + static_cast<int>(e.height))
            {
                return JHitResult{
                    e.host,
                    static_cast<float>(globalX - e.originX),
                    static_cast<float>(globalY - e.originY)
                };
            }
        }
        return std::nullopt;
    }

    const std::vector<JEntry>& entries() const { return m_entries; }

private:
    JDockRegistry() {
        m_defaultOptions.handleHoverPad = 4.0f;
        m_defaultOptions.enforceMinSizes = true;
        m_defaultOptions.showResizeCursors = true;
    }
    JDockOptions m_defaultOptions;
    std::vector<JEntry> m_entries;
};

} // inline namespace jf
