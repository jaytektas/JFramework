#include <genesis/graphics/GpuHal.h>
#include <cassert>

using namespace Genesis;

class MockGpuHal : public GpuHal {
public:
    bool initialized = false;
    uint32_t currentWidth = 0;
    uint32_t currentHeight = 0;
    uint32_t frameCounter = 0;

    bool initialize() override {
        initialized = true;
        return true;
    }

    void resizeSurface(GpuSurfaceId /*sid*/, uint32_t width, uint32_t height) override {
        currentWidth = width;
        currentHeight = height;
    }

    GpuFrameContext beginFrame(GpuSurfaceId sid = kPrimarySurface) override {
        GpuFrameContext ctx{};
        ctx.frameIndex = frameCounter++;
        ctx.surfaceId  = sid;
        return ctx;
    }

    void submitAndPresentFrame(const GpuFrameContext& /*context*/) override {}

    bool uploadFontAtlas(const uint8_t*, uint32_t, uint32_t) override { return true; }
    void drawPrimitives(const PrimitiveBuffer&) override {}
    void waitIdle() override {}

    GpuApiType getBackendType() const noexcept override { return GpuApiType::Vulkan; }

    GpuSurfaceId createSurface(const NativeWindowHandle& /*window*/,
                               uint32_t /*w*/, uint32_t /*h*/) override {
        return kPrimarySurface + 1; // stub
    }

    void destroySurface(GpuSurfaceId /*sid*/) override {}
};

// Fulfilling the factory method for testing
std::unique_ptr<GpuHal> GpuHal::create(GpuApiType api, const NativeWindowHandle& windowHandle) {
    (void)api; (void)windowHandle;
    return std::make_unique<MockGpuHal>();
}

void test_gpuhal_factory() {
    NativeWindowHandle handle{ GpuApiType::Vulkan, nullptr, nullptr };
    auto hal = GpuHal::create(GpuApiType::Vulkan, handle);

    assert(hal != nullptr);
    assert(hal->initialize() == true);
    assert(hal->getBackendType() == GpuApiType::Vulkan);

    std::cout << "test_gpuhal_factory passed" << std::endl;
}

void test_gpuhal_lifecycle() {
    NativeWindowHandle handle{ GpuApiType::Vulkan, nullptr, nullptr };
    auto hal = GpuHal::create(GpuApiType::Vulkan, handle);

    hal->initialize();
    hal->resizeSwapchain(1920, 1080); // uses compat shim

    auto frame = hal->beginFrame();
    assert(frame.frameIndex == 0);
    assert(frame.surfaceId == kPrimarySurface);

    hal->submitAndPresentFrame(frame);

    auto nextFrame = hal->beginFrame();
    assert(nextFrame.frameIndex == 1);

    hal->waitIdle();

    std::cout << "test_gpuhal_lifecycle passed" << std::endl;
}

int main() {
    test_gpuhal_factory();
    test_gpuhal_lifecycle();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
