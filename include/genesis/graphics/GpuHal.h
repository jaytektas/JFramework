#pragma once

#include <vector>
#include <memory>
#include <cstdint>
#include <iostream>
#include <string>
#include <genesis/graphics/RenderPrimitive.h>

namespace { inline constexpr auto& LogGraphicsBackend = Genesis::Log::Vulkan; }

namespace Genesis {

/**
 * @brief Identifiers for concrete GPU backends supported by the toolkit.
 */
enum class GpuApiType : uint32_t {
    Vulkan,
    Metal,
    Software
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

using GpuSurfaceId = uint32_t;
static constexpr GpuSurfaceId kPrimarySurface = 0;

/**
 * @brief Encapsulates multi-buffered synchronization primitives for CPU/GPU parallelism.
 */
struct GpuFrameContext {
    uint32_t     frameIndex{0};
    GpuSurfaceId surfaceId{kPrimarySurface};
    void*        commandQueueHandle{nullptr};
    void*        commandBufferHandle{nullptr};
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
    virtual void resizeSurface(GpuSurfaceId sid, uint32_t width, uint32_t height) = 0;

    // Compat shim — keeps existing call sites unchanged.
    void resizeSwapchain(uint32_t w, uint32_t h) { resizeSurface(kPrimarySurface, w, h); }

    /**
     * @brief Acquires the next available frame index boundary from the OS presentation layer.
     */
    virtual GpuFrameContext beginFrame(GpuSurfaceId sid = kPrimarySurface) = 0;

    /**
     * @brief Upload a greyscale R8 font atlas bitmap as a GPU texture.
     * Call once after FontEngine::buildAtlas() and before the first frame.
     */
    virtual bool uploadFontAtlas(const uint8_t* pixels, uint32_t w, uint32_t h) = 0;

    /**
     * @brief Records draw calls for all primitives (SDF rects + text glyphs)
     * into the active command buffer.
     * Must be called between beginFrame() and submitAndPresentFrame().
     * Viewport dimensions are taken from the current swapchain extent.
     */
    virtual void drawPrimitives(const PrimitiveBuffer& buffer) = 0;

    /**
     * @brief Dispatches recorded operations directly to physical hardware execution paths.
     */
    virtual void submitAndPresentFrame(const GpuFrameContext& context) = 0;

    /**
     * @brief Explicit blocking barrier execution pass ensuring zero active processing workloads remain on the GPU.
     */
    virtual void waitIdle() = 0;

    virtual GpuApiType getBackendType() const noexcept = 0;

    // Schedule a framebuffer readback on the next submitAndPresentFrame call.
    // Writes a raw PPM file to `path`.  sid defaults to the primary surface.
    virtual void captureNextFrame(const char* /*path*/, GpuSurfaceId /*sid*/ = kPrimarySurface) {}

    // Create a new rendering surface for a native window. Returns a GpuSurfaceId.
    // w/h are the initial pixel dimensions of the window.
    virtual GpuSurfaceId createSurface(const NativeWindowHandle& window, uint32_t w, uint32_t h) = 0;

    // Destroy a surface created with createSurface. Do not call on kPrimarySurface.
    virtual void destroySurface(GpuSurfaceId sid) = 0;

protected:
    GpuHal() = default;
};

} // namespace Genesis
