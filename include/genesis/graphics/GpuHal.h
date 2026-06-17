#pragma once

#include <vector>
#include <memory>
#include <cstdint>
#include <iostream>
#include <string>

// --- Custom Logging Integration Mock ---
#ifndef qCCritical
#define qCCritical(category) std::cerr << "[CRITICAL] "
#define qCWarning(category) std::cerr << "[WARNING] "
struct MockCategory {};
inline MockCategory LogGraphicsBackend;
#endif

namespace Genesis {

/**
 * @brief Identifiers for concrete GPU backends supported by the toolkit.
 */
enum class GpuApiType : uint32_t {
    Vulkan,
    Metal
};

/**
 * @brief Opaque abstraction wrapper for native OS surface boundaries.
 * Wraps platform handles (e.g., wl_surface*, HWND, NSView*) safely.
 */
struct NativeWindowHandle {
    GpuApiType apiTarget;
    void* connectionPointer{nullptr}; // xcb_connection_t* or wl_display*
    void* windowPointer{nullptr};     // xcb_window_t or wl_surface*
};

/**
 * @brief Encapsulates multi-buffered synchronization primitives for CPU/GPU parallelism.
 */
struct GpuFrameContext {
    uint32_t frameIndex{0};
    void* commandQueueHandle{nullptr};   // Internal VkQueue or MTLCommandQueue
    void* commandBufferHandle{nullptr}; // Active command recorder reference
};

/**
 * @brief The strict foundational hardware abstraction layer interface.
 */
class GpuHal {
public:
    virtual ~GpuHal() = default;

    // Rigid asset topology; deny shallow buffer duplicates across execution bounds
    GpuHal(const GpuHal&) = delete;
    GpuHal& operator=(const GpuHal&) = delete;
    GpuHal(GpuHal&&) noexcept = default;
    GpuHal& operator=(GpuHal&&) noexcept = default;

    /**
     * @brief Factory method to initialize a hardware rendering subsystem instance.
     */
    static std::unique_ptr<GpuHal> create(GpuApiType api, const NativeWindowHandle& windowHandle);

    /**
     * @brief Bootstraps logical devices, surfaces, and basic swapchain allocations.
     */
    virtual bool initialize() = 0;

    /**
     * @brief Recreates the surface boundaries during native OS resizing cycles.
     */
    virtual void resizeSwapchain(uint32_t width, uint32_t height) = 0;

    /**
     * @brief Acquires the next available frame index boundary from the OS presentation layer.
     */
    virtual GpuFrameContext beginFrame() = 0;

    /**
     * @brief Dispatches recorded operations directly to physical hardware execution paths.
     */
    virtual void submitAndPresentFrame(const GpuFrameContext& context) = 0;

    /**
     * @brief Explicit blocking barrier execution pass ensuring zero active processing workloads remain on the GPU.
     */
    virtual void waitIdle() = 0;

    virtual GpuApiType getBackendType() const noexcept = 0;

protected:
    GpuHal() = default;
};

} // namespace Genesis
