#pragma once

#include <vector>
#include <optional>
#include <algorithm>
#include <cstdint>

namespace Genesis {

class DockHost;

// ============================================================================
// DockRegistry — global list of all live DockHost instances and their current
// screen rectangles.
//
// FloatingDockWindow queries this every frame during a drag to find which host
// is under the cursor, then calls updateDrag() on it directly.  No hardcoded
// window bounds needed in the application loop.
//
// Usage:
//   // At startup (after the host's layout is computed):
//   DockRegistry::instance().registerHost(host, win.screenX(), win.screenY(), W, H);
//
//   // Each frame (window may have been moved by the WM):
//   DockRegistry::instance().updateBounds(host, win.screenX(), win.screenY(), W, H);
//
//   // At shutdown:
//   DockRegistry::instance().unregisterHost(host);
// ============================================================================
struct DockOptions {
    std::optional<float> handleHoverPad;
    std::optional<bool>  enforceMinSizes;
    std::optional<bool>  showResizeCursors;
};

class DockRegistry {
public:
    static DockRegistry& instance() {
        static DockRegistry reg;
        return reg;
    }

    DockOptions& defaultOptions() { return m_defaultOptions; }
    const DockOptions& defaultOptions() const { return m_defaultOptions; }

    struct Entry {
        DockHost* host{nullptr};
        int       screenX{0}, screenY{0};
        uint32_t  width{0},   height{0};
    };

    void registerHost(DockHost& host, int sx, int sy, uint32_t w, uint32_t h) {
        for (auto& e : m_entries) {
            if (e.host == &host) { e.screenX = sx; e.screenY = sy; e.width = w; e.height = h; return; }
        }
        m_entries.push_back({&host, sx, sy, w, h});
    }

    void unregisterHost(DockHost& host) {
        m_entries.erase(
            std::remove_if(m_entries.begin(), m_entries.end(),
                [&](const Entry& e){ return e.host == &host; }),
            m_entries.end());
    }

    // Upsert: registers if not present, updates bounds otherwise.
    void updateBounds(DockHost& host, int sx, int sy, uint32_t w, uint32_t h) {
        registerHost(host, sx, sy, w, h);
    }

    struct HitResult {
        DockHost* host;
        float     localX, localY;
    };

    // Returns the first registered host whose screen rect contains (gx, gy),
    // plus the cursor position in that host's local coordinate space.
    std::optional<HitResult> hitTest(int globalX, int globalY) const {
        for (const auto& e : m_entries) {
            if (globalX >= e.screenX && globalX < e.screenX + static_cast<int>(e.width) &&
                globalY >= e.screenY && globalY < e.screenY + static_cast<int>(e.height))
            {
                return HitResult{
                    e.host,
                    static_cast<float>(globalX - e.screenX),
                    static_cast<float>(globalY - e.screenY)
                };
            }
        }
        return std::nullopt;
    }

    const std::vector<Entry>& entries() const { return m_entries; }

private:
    DockRegistry() {
        m_defaultOptions.handleHoverPad = 4.0f;
        m_defaultOptions.enforceMinSizes = true;
        m_defaultOptions.showResizeCursors = true;
    }
    DockOptions m_defaultOptions;
    std::vector<Entry> m_entries;
};

} // namespace Genesis
