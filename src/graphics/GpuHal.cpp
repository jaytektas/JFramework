#include <genesis/graphics/GpuHal.h>

namespace Genesis {

// Base implementation for the factory method.
// Specific implementations (VulkanGpuHal, MetalGpuHal) would be instantiated here.
std::unique_ptr<GpuHal> GpuHal::create(GpuApiType api, const NativeWindowHandle& windowHandle) {
    (void)api;
    (void)windowHandle;
    // return std::make_unique<VulkanGpuHal>(windowHandle);
    return nullptr; 
}

} // namespace Genesis
