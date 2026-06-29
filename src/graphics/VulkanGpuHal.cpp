#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__linux__)
#define VK_USE_PLATFORM_XCB_KHR
#endif

#include <vulkan/vulkan.h>

#if defined(_WIN32)
#include <vulkan/vulkan_win32.h>
#include <windows.h>
#elif defined(__linux__)
#include <vulkan/vulkan_xcb.h>
#include <xcb/xcb.h>
#endif

#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>
#include <j/graphics/ShaderSpirv.h>
#include <j/core/GenesisComponents.h>
#include <j/core/muted_logging_mock.h>
#include "SoftwareGpuHal.h"

#include <iostream>
#include <stdexcept>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <algorithm>

namespace { inline constexpr auto& LogVulkan = jf::Log::Vulkan; }

inline namespace jf {

// ============================================================================
// Helpers
// ============================================================================

static uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t filter,
                               VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((filter & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    throw std::runtime_error("No suitable Vulkan memory type");
}

static void createBuffer(VkDevice dev, VkPhysicalDevice phys,
                         VkDeviceSize size, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags memProps,
                         VkBuffer& outBuf, VkDeviceMemory& outMem) {
    VkBufferCreateInfo ci{};
    ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size        = size;
    ci.usage       = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &ci, nullptr, &outBuf) != VK_SUCCESS)
        throw std::runtime_error("vkCreateBuffer failed");

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, outBuf, &req);

    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = findMemoryType(phys, req.memoryTypeBits, memProps);
    if (vkAllocateMemory(dev, &ai, nullptr, &outMem) != VK_SUCCESS)
        throw std::runtime_error("vkAllocateMemory failed");

    vkBindBufferMemory(dev, outBuf, outMem, 0);
}

static VkCommandBuffer beginOneShot(VkDevice dev, VkCommandPool pool) {
    VkCommandBufferAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandPool        = pool;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

static void endOneShot(VkDevice dev, VkCommandPool pool, VkQueue q, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(q);
    vkFreeCommandBuffers(dev, pool, 1, &cb);
}

static void transitionImage(VkCommandBuffer cb, VkImage img,
                            VkImageLayout from, VkImageLayout to,
                            VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                            VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout           = from;
    barrier.newLayout           = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = img;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask       = srcAccess;
    barrier.dstAccessMask       = dstAccess;
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// ============================================================================
// Per-surface state
// ============================================================================

struct VulkanSurface {
    VkSurfaceKHR    vkSurface   {VK_NULL_HANDLE};
    VkSwapchainKHR  swapchain   {VK_NULL_HANDLE};
    VkCommandBuffer cmdBuf      {VK_NULL_HANDLE};
    VkFence         inFlight    {VK_NULL_HANDLE};   // signaled when GPU work done
    std::vector<VkImage>       images;
    std::vector<VkImageView>   imageViews;
    std::vector<VkFramebuffer> framebuffers;
    VkExtent2D      extent      {};
    uint32_t        imageIndex  {0};
    // Deferred resize: set by resizeSurface(), consumed at top of beginFrame().
    uint32_t        pendingW    {0};
    uint32_t        pendingH    {0};
    bool            needsRebuild{false};
    // Screenshot readback
    std::string     screenshotPath;
    std::string     screenshotSavePath;
    VkBuffer        screenshotBuf  {VK_NULL_HANDLE};
    VkDeviceMemory  screenshotMem  {VK_NULL_HANDLE};
    bool            alive{false};
};

// ============================================================================
// VulkanGpuHal
// ============================================================================

class VulkanGpuHal : public JGpuHal {
public:
    VulkanGpuHal(const JNativeWindowHandle& h) : m_handle(h) {}

    ~VulkanGpuHal() override {
        if (!m_device) return;
        waitIdle();

        for (auto& s : m_surfaces) {
            if (!s.alive && s.vkSurface == VK_NULL_HANDLE) continue;
            if (s.screenshotBuf != VK_NULL_HANDLE) {
                vkDestroyBuffer(m_device, s.screenshotBuf, nullptr);
                vkFreeMemory(m_device, s.screenshotMem, nullptr);
            }
            _destroySwapchain(s);
            _freeSurfaceCmdAndSync(s);
            if (s.vkSurface) vkDestroySurfaceKHR(m_instance, s.vkSurface, nullptr);
        }
        m_surfaces.clear();

        // Text pipeline
        if (m_textVertMapped)  vkUnmapMemory(m_device, m_textVertMemory);
        if (m_textVertBuffer)  vkDestroyBuffer(m_device, m_textVertBuffer, nullptr);
        if (m_textVertMemory)  vkFreeMemory(m_device, m_textVertMemory, nullptr);
        if (m_textPipeline)    vkDestroyPipeline(m_device, m_textPipeline, nullptr);
        if (m_textPipeLayout)  vkDestroyPipelineLayout(m_device, m_textPipeLayout, nullptr);
        if (m_textDescPool)    vkDestroyDescriptorPool(m_device, m_textDescPool, nullptr);
        if (m_textDescSetLayout) vkDestroyDescriptorSetLayout(m_device, m_textDescSetLayout, nullptr);

        // Vector pipeline
        if (m_geomVertMapped) vkUnmapMemory(m_device, m_geomVertMemory);
        if (m_geomVertBuffer) vkDestroyBuffer(m_device, m_geomVertBuffer, nullptr);
        if (m_geomVertMemory) vkFreeMemory(m_device, m_geomVertMemory, nullptr);
        if (m_vectorPipeline)   vkDestroyPipeline(m_device, m_vectorPipeline, nullptr);
        if (m_vectorPipeLayout) vkDestroyPipelineLayout(m_device, m_vectorPipeLayout, nullptr);

        // Image pipeline
        if (m_imagePipeline)  vkDestroyPipeline(m_device, m_imagePipeline, nullptr);
        for (auto& kv : m_textures) {
            if (kv.second.descSet)  { /* freed via pool */ }
            if (kv.second.view)     vkDestroyImageView(m_device, kv.second.view, nullptr);
            if (kv.second.image)    vkDestroyImage(m_device, kv.second.image, nullptr);
            if (kv.second.memory)   vkFreeMemory(m_device, kv.second.memory, nullptr);
            if (kv.second.sampler)  vkDestroySampler(m_device, kv.second.sampler, nullptr);
        }
        if (m_imageDescPool)  vkDestroyDescriptorPool(m_device, m_imageDescPool, nullptr);

        // Atlas texture
        if (m_atlasView)    vkDestroyImageView(m_device, m_atlasView, nullptr);
        if (m_atlasImage)   vkDestroyImage(m_device, m_atlasImage, nullptr);
        if (m_atlasMemory)  vkFreeMemory(m_device, m_atlasMemory, nullptr);
        if (m_atlasSampler) vkDestroySampler(m_device, m_atlasSampler, nullptr);

        // SDF pipeline
        if (m_sdfPipeline)    vkDestroyPipeline(m_device, m_sdfPipeline, nullptr);
        if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        if (m_renderPass)     vkDestroyRenderPass(m_device, m_renderPass, nullptr);

        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        vkDestroyDevice(m_device, nullptr);
        if (m_instance) vkDestroyInstance(m_instance, nullptr);
    }

    bool initialize() override {
        createInstance();
        // Primary surface must exist before pickPhysicalDevice so the driver can
        // check presentation support against the real surface.
        m_surfaces.emplace_back();
        m_surfaces[0].vkSurface = _createVkSurface(m_handle);
        pickPhysicalDevice();
        createLogicalDevice();
        createCommandPool();
        createRenderPass();
        createSdfPipelineLayout();
        createSdfPipeline();
        createTextDescriptorLayout();
        createTextPipeline();
        createTextVertexBuffer();
        createImagePipeline();
        createVectorPipeline();
        createGeomVertexBuffer();
        // Primary surface gets cmd buffer + sync; swapchain built by resizeSurface later.
        _allocSurfaceCmdAndSync(m_surfaces[0]);
        m_surfaces[0].alive = true;
        qCInfo(LogVulkan) << "Vulkan HAL initialized.\n";
        return true;
    }

    void resizeSurface(GpuSurfaceId sid, uint32_t w, uint32_t h) override {
        if (sid >= m_surfaces.size()) return;
        // Non-blocking: just record the desired size. The swapchain is rebuilt
        // at the top of the next beginFrame so no in-flight work is interrupted.
        m_surfaces[sid].pendingW     = w;
        m_surfaces[sid].pendingH     = h;
        m_surfaces[sid].needsRebuild = true;
    }

    GpuSurfaceId createSurface(const JNativeWindowHandle& handle, uint32_t w, uint32_t h) override {
        GpuSurfaceId sid = _allocSurfaceSlot();
        VulkanSurface& s = m_surfaces[sid];
        s.vkSurface = _createVkSurface(handle);
        _allocSurfaceCmdAndSync(s);
        if (w > 0 && h > 0) _buildSwapchain(s, w, h);
        s.alive = true;
        return sid;
    }

    void destroySurface(GpuSurfaceId sid) override {
        if (sid == kPrimarySurface || sid >= m_surfaces.size()) return;
        waitIdle();
        VulkanSurface& s = m_surfaces[sid];
        if (s.screenshotBuf != VK_NULL_HANDLE) {
            if (!s.screenshotSavePath.empty())
                _writeScreenshot(s);
            else {
                vkDestroyBuffer(m_device, s.screenshotBuf, nullptr);
                vkFreeMemory(m_device, s.screenshotMem, nullptr);
                s.screenshotBuf = VK_NULL_HANDLE;
                s.screenshotMem = VK_NULL_HANDLE;
            }
        }
        _destroySwapchain(s);
        _freeSurfaceCmdAndSync(s);
        vkDestroySurfaceKHR(m_instance, s.vkSurface, nullptr);
        s.vkSurface = VK_NULL_HANDLE;
        s.alive = false;
    }

    // ---- Font atlas upload ----
    bool uploadFontAtlas(const uint8_t* pixels, uint32_t w, uint32_t h) override {
        VkDeviceSize sz = static_cast<VkDeviceSize>(w) * h;

        VkBuffer stagBuf; VkDeviceMemory stagMem;
        createBuffer(m_device, m_physicalDevice, sz,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagBuf, stagMem);
        void* ptr; vkMapMemory(m_device, stagMem, 0, sz, 0, &ptr);
        std::memcpy(ptr, pixels, sz);
        vkUnmapMemory(m_device, stagMem);

        VkImageCreateInfo imgCI{};
        imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgCI.imageType     = VK_IMAGE_TYPE_2D;
        imgCI.format        = VK_FORMAT_R8_UNORM;
        imgCI.extent        = {w, h, 1};
        imgCI.mipLevels     = 1;
        imgCI.arrayLayers   = 1;
        imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
        imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imgCI.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(m_device, &imgCI, nullptr, &m_atlasImage);

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(m_device, m_atlasImage, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = findMemoryType(m_physicalDevice, req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(m_device, &ai, nullptr, &m_atlasMemory);
        vkBindImageMemory(m_device, m_atlasImage, m_atlasMemory, 0);

        auto cb = beginOneShot(m_device, m_commandPool);
        transitionImage(cb, m_atlasImage,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, VK_ACCESS_TRANSFER_WRITE_BIT);

        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent      = {w, h, 1};
        vkCmdCopyBufferToImage(cb, stagBuf, m_atlasImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        transitionImage(cb, m_atlasImage,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        endOneShot(m_device, m_commandPool, m_graphicsQueue, cb);

        vkDestroyBuffer(m_device, stagBuf, nullptr);
        vkFreeMemory(m_device, stagMem, nullptr);

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image                       = m_atlasImage;
        vci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        vci.format                      = VK_FORMAT_R8_UNORM;
        vci.subresourceRange            = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &vci, nullptr, &m_atlasView);

        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter  = VK_FILTER_LINEAR;
        sci.minFilter  = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(m_device, &sci, nullptr, &m_atlasSampler);

        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool     = m_textDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &m_textDescSetLayout;
        vkAllocateDescriptorSets(m_device, &dsai, &m_textDescSet);

        VkDescriptorImageInfo dii{};
        dii.sampler     = m_atlasSampler;
        dii.imageView   = m_atlasView;
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet          = m_textDescSet;
        wr.dstBinding      = 0;
        wr.descriptorCount = 1;
        wr.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo      = &dii;
        vkUpdateDescriptorSets(m_device, 1, &wr, 0, nullptr);

        m_atlasUploaded = true;
        qCInfo(LogVulkan) << "Font atlas uploaded " << w << "x" << h << "\n";
        return true;
    }

    JGpuFrameContext beginFrame(GpuSurfaceId sid = kPrimarySurface) override {
        VulkanSurface& s = m_surfaces[sid];
        m_act = &s;
        m_activeSurfaceId = sid;

        // Wait for the PREVIOUS frame's GPU work to finish first.  After this fence the
        // swapchain images/framebuffers are no longer referenced by any in-flight
        // command buffer, so we can rebuild the swapchain WITHOUT a full
        // vkDeviceWaitIdle.  That device-wide stall, run on every resize frame, is the
        // main cause of choppy resizing — the per-frame fence + oldSwapchain handoff in
        // _buildSwapchain give the same safety far more cheaply.
        vkWaitForFences(m_device, 1, &s.inFlight, VK_TRUE, UINT64_MAX);
        vkResetFences(m_device, 1, &s.inFlight);

        // If a screenshot copy was recorded in the previous frame's command buffer,
        // the fence above proves the GPU work (including the copy) is done.
        if (s.screenshotBuf != VK_NULL_HANDLE)
            _writeScreenshot(s);

        // Apply a deferred/pending resize now that the previous frame is retired.
        if (s.needsRebuild)
            _rebuildToCurrentExtent(s);

        // Poll-acquire with zero timeout, no semaphore, no fence.
        // lavapipe submits are CPU-synchronous; the inFlight fence above already
        // guarantees the previous frame's GPU work is done so the image is free.
        // Using a fence or semaphore here causes an XCB-event deadlock on Xvfb.
        VkResult acq;
        do {
            acq = vkAcquireNextImageKHR(m_device, s.swapchain, 0,
                                        VK_NULL_HANDLE, VK_NULL_HANDLE, &s.imageIndex);
            if (acq == VK_ERROR_OUT_OF_DATE_KHR)
                _rebuildToCurrentExtent(s);  // window resized mid-acquire — heal in place
        } while (acq == VK_TIMEOUT || acq == VK_NOT_READY || acq == VK_ERROR_OUT_OF_DATE_KHR);

        vkResetCommandBuffer(s.cmdBuf, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(s.cmdBuf, &bi);

        VkRenderPassBeginInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpi.renderPass        = m_renderPass;
        rpi.framebuffer       = s.framebuffers[s.imageIndex];
        rpi.renderArea.extent = s.extent;
        VkClearValue cv       = {{{0.070f, 0.070f, 0.078f, 1.0f}}};
        rpi.clearValueCount   = 1; rpi.pClearValues = &cv;
        vkCmdBeginRenderPass(s.cmdBuf, &rpi, VK_SUBPASS_CONTENTS_INLINE);

        JGpuFrameContext ctx{};
        ctx.frameIndex          = m_frameCounter++;
        ctx.surfaceId           = sid;
        ctx.commandQueueHandle  = m_graphicsQueue;
        ctx.commandBufferHandle = s.cmdBuf;
        return ctx;
    }

    void drawPrimitives(const JPrimitiveBuffer& buf) override {
        const auto& cmds = buf.getCommands();
        if (cmds.empty()) return;

        uint32_t textCursor = 0;
        uint32_t geomCursor = 0;

        using JKind = JPrimitiveBuffer::JDrawCommand::JKind;
        using Clip = JPrimitiveBuffer::JClipRect;
        auto sameClip = [](const Clip& a, const Clip& b) {
            return a.enabled == b.enabled && a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
        };

        size_t i = 0;
        while (i < cmds.size()) {
            const JKind k    = cmds[i].kind;
            const Clip clip = cmds[i].clip;
            // Batch consecutive commands that share both pipeline kind AND clip rect.
            size_t j = i;
            while (j < cmds.size() && cmds[j].kind == k && sameClip(cmds[j].clip, clip)) ++j;

            _applyScissor(clip);
            if      (k == JKind::JRect)            _drawSdfBatch(cmds, i, j);
            else if (k == JKind::Image)           textCursor = _drawImageBatch(cmds, i, j, textCursor);
            else if (k == JKind::Geometry)        geomCursor = _drawGeometryBatch(cmds, i, j, geomCursor);
            else if (m_atlasUploaded)            textCursor = _drawTextBatch(cmds, i, j, textCursor);
            i = j;
        }
    }

    // Set the dynamic scissor from a clip rect (clamped to the surface), or full window.
    void _applyScissor(const JPrimitiveBuffer::JClipRect& clip) {
        VkRect2D sc;
        if (clip.enabled) {
            float x  = std::max(0.0f, clip.x);
            float y  = std::max(0.0f, clip.y);
            float rr = std::min(static_cast<float>(m_act->extent.width),  clip.x + clip.w);
            float bb = std::min(static_cast<float>(m_act->extent.height), clip.y + clip.h);
            sc.offset = { static_cast<int32_t>(x), static_cast<int32_t>(y) };
            sc.extent = { static_cast<uint32_t>(std::max(0.0f, rr - x)),
                          static_cast<uint32_t>(std::max(0.0f, bb - y)) };
        } else {
            sc.offset = {0, 0};
            sc.extent = m_act->extent;
        }
        vkCmdSetScissor(m_act->cmdBuf, 0, 1, &sc);
    }

    void captureNextFrame(const char* path, GpuSurfaceId sid = kPrimarySurface) override {
        if (sid < m_surfaces.size() && m_surfaces[sid].alive)
            m_surfaces[sid].screenshotPath = path;
    }

    void submitAndPresentFrame(const JGpuFrameContext& ctx) override {
        VulkanSurface& s = m_surfaces[ctx.surfaceId];
        vkCmdEndRenderPass(s.cmdBuf);

        if (!s.screenshotPath.empty()) {
            VkDeviceSize bufSz = (VkDeviceSize)s.extent.width * s.extent.height * 4;
            createBuffer(m_device, m_physicalDevice, bufSz,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         s.screenshotBuf, s.screenshotMem);

            transitionImage(s.cmdBuf, s.images[s.imageIndex],
                            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);

            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent      = {s.extent.width, s.extent.height, 1};
            vkCmdCopyImageToBuffer(s.cmdBuf, s.images[s.imageIndex],
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   s.screenshotBuf, 1, &region);

            transitionImage(s.cmdBuf, s.images[s.imageIndex],
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);

            s.screenshotSavePath = std::move(s.screenshotPath);
        }

        vkEndCommandBuffer(s.cmdBuf);

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &s.cmdBuf;
        vkQueueSubmit(m_graphicsQueue, 1, &si, s.inFlight);

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 0;
        VkSwapchainKHR sc[] = {s.swapchain};
        pi.swapchainCount    = 1; pi.pSwapchains   = sc;
        pi.pImageIndices     = &s.imageIndex;
        VkResult pr = vkQueuePresentKHR(m_graphicsQueue, &pi);
        // Schedule a rebuild for next beginFrame if the swapchain is stale.
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
            s.needsRebuild = true;
    }

    void waitIdle() override { if (m_device) vkDeviceWaitIdle(m_device); }
    JGpuApiType getBackendType() const noexcept override { return JGpuApiType::Vulkan; }

private:
    // ------------------------------------------------------------------ SDF
    using CmdVec = std::vector<JPrimitiveBuffer::JDrawCommand>;

    void _drawSdfBatch(const CmdVec& cmds, size_t from, size_t to) {
        vkCmdBindPipeline(m_act->cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sdfPipeline);

        VkViewport vp{0,0,(float)m_act->extent.width,(float)m_act->extent.height,0,1};
        vkCmdSetViewport(m_act->cmdBuf, 0, 1, &vp);
        // Scissor is set by the caller (drawPrimitives) from the batch's clip rect.

        for (size_t k = from; k < to; ++k) {
            JGpuPrimitiveInstance copy = cmds[k].rect;
            copy.padding[0] = (float)m_act->extent.width;
            copy.padding[1] = (float)m_act->extent.height;
            vkCmdPushConstants(m_act->cmdBuf, m_pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(JGpuPrimitiveInstance), &copy);
            vkCmdDraw(m_act->cmdBuf, 6, 1, 0, 0);
        }
    }

    // ------------------------------------------------------------------ Text
    struct TextPC { float r,g,b,a; float vpW,vpH; float pad[2]; };

    uint32_t _drawTextBatch(const CmdVec& cmds, size_t from, size_t to, uint32_t vertexCursor) {
        std::vector<float> verts;
        struct DrawRange { uint32_t first, count; uint8_t color[4]; };
        std::vector<DrawRange> ranges;

        for (size_t k = from; k < to; ++k) {
            const auto& call = cmds[k].text;
            DrawRange r;
            std::copy(call.color, call.color+4, r.color);
            r.first = static_cast<uint32_t>(verts.size() / 4);
            for (const auto& tv : call.verts) {
                verts.push_back(tv.x); verts.push_back(tv.y);
                verts.push_back(tv.u); verts.push_back(tv.v);
            }
            r.count = static_cast<uint32_t>(verts.size() / 4) - r.first;
            ranges.push_back(r);
        }
        if (verts.empty()) return vertexCursor;

        VkDeviceSize surfaceOffset = (VkDeviceSize)m_activeSurfaceId * MAX_TEXT_VERTS * 4 * sizeof(float);

        VkDeviceSize sliceBytes   = verts.size() * sizeof(float);
        VkDeviceSize bufferOffset = surfaceOffset + (VkDeviceSize)vertexCursor * 4 * sizeof(float);
        VkDeviceSize capacity     = (VkDeviceSize)MAX_TEXT_VERTS * 4 * sizeof(float);
        if (vertexCursor * 4 * sizeof(float) + sliceBytes > capacity) {
            VkDeviceSize remaining = (vertexCursor * 4 * sizeof(float) < capacity) ? capacity - vertexCursor * 4 * sizeof(float) : 0;
            sliceBytes = (sliceBytes > remaining) ? remaining : sliceBytes;
        }
        if (sliceBytes == 0) return vertexCursor;

        std::memcpy(static_cast<char*>(m_textVertMapped) + bufferOffset,
                    verts.data(), sliceBytes);

        vkCmdBindPipeline(m_act->cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_textPipeline);
        VkViewport vp{0,0,(float)m_act->extent.width,(float)m_act->extent.height,0,1};
        vkCmdSetViewport(m_act->cmdBuf, 0, 1, &vp);
        // Scissor is set by the caller (drawPrimitives) from the batch's clip rect.

        vkCmdBindDescriptorSets(m_act->cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_textPipeLayout, 0, 1, &m_textDescSet, 0, nullptr);

        VkDeviceSize bindOffset = bufferOffset;
        vkCmdBindVertexBuffers(m_act->cmdBuf, 0, 1, &m_textVertBuffer, &bindOffset);

        for (auto& r : ranges) {
            if (r.count == 0) continue;
            TextPC pc{};
            pc.r   = r.color[0] / 255.0f; pc.g = r.color[1] / 255.0f;
            pc.b   = r.color[2] / 255.0f; pc.a = r.color[3] / 255.0f;
            pc.vpW = (float)m_act->extent.width; pc.vpH = (float)m_act->extent.height;
            vkCmdPushConstants(m_act->cmdBuf, m_textPipeLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(TextPC), &pc);
            vkCmdDraw(m_act->cmdBuf, r.count, 1, r.first, 0);
        }

        return vertexCursor + static_cast<uint32_t>(verts.size() / 4);
    }

    // ------------------------------------------------------------------ Images

    struct VulkanTexture {
        VkImage        image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkImageView    view{VK_NULL_HANDLE};
        VkSampler      sampler{VK_NULL_HANDLE};
        VkDescriptorSet descSet{VK_NULL_HANDLE};
    };

    TextureHandle uploadTexture(const uint8_t* rgba, uint32_t w, uint32_t h) override {
        VkDeviceSize sz = (VkDeviceSize)w * h * 4;

        VkBuffer stagBuf; VkDeviceMemory stagMem;
        createBuffer(m_device, m_physicalDevice, sz,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagBuf, stagMem);
        void* ptr; vkMapMemory(m_device, stagMem, 0, sz, 0, &ptr);
        std::memcpy(ptr, rgba, sz);
        vkUnmapMemory(m_device, stagMem);

        VulkanTexture tex;

        VkImageCreateInfo imgCI{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imgCI.imageType     = VK_IMAGE_TYPE_2D;
        imgCI.format        = VK_FORMAT_R8G8B8A8_SRGB;
        imgCI.extent        = {w, h, 1};
        imgCI.mipLevels     = 1;
        imgCI.arrayLayers   = 1;
        imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
        imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imgCI.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &imgCI, nullptr, &tex.image) != VK_SUCCESS) {
            vkDestroyBuffer(m_device, stagBuf, nullptr);
            vkFreeMemory(m_device, stagMem, nullptr);
            return kNullTexture;
        }

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(m_device, tex.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = findMemoryType(m_physicalDevice, req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(m_device, &ai, nullptr, &tex.memory);
        vkBindImageMemory(m_device, tex.image, tex.memory, 0);

        auto cb = beginOneShot(m_device, m_commandPool);
        transitionImage(cb, tex.image,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent      = {w, h, 1};
        vkCmdCopyBufferToImage(cb, stagBuf, tex.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transitionImage(cb, tex.image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        endOneShot(m_device, m_commandPool, m_graphicsQueue, cb);

        vkDestroyBuffer(m_device, stagBuf, nullptr);
        vkFreeMemory(m_device, stagMem, nullptr);

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image            = tex.image;
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vci.format           = VK_FORMAT_R8G8B8A8_SRGB;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &vci, nullptr, &tex.view);

        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter    = VK_FILTER_LINEAR;
        sci.minFilter    = VK_FILTER_LINEAR;
        sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(m_device, &sci, nullptr, &tex.sampler);

        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool     = m_imageDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &m_textDescSetLayout; // same layout as text
        vkAllocateDescriptorSets(m_device, &dsai, &tex.descSet);

        VkDescriptorImageInfo dii{};
        dii.sampler     = tex.sampler;
        dii.imageView   = tex.view;
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet          = tex.descSet;
        wr.dstBinding      = 0;
        wr.descriptorCount = 1;
        wr.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo      = &dii;
        vkUpdateDescriptorSets(m_device, 1, &wr, 0, nullptr);

        TextureHandle handle = m_nextTexHandle++;
        m_textures[handle]   = tex;
        return handle;
    }

    void releaseTexture(TextureHandle handle) override {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;
        waitIdle();
        VulkanTexture& tex = it->second;
        if (tex.view)    vkDestroyImageView(m_device, tex.view, nullptr);
        if (tex.image)   vkDestroyImage(m_device, tex.image, nullptr);
        if (tex.memory)  vkFreeMemory(m_device, tex.memory, nullptr);
        if (tex.sampler) vkDestroySampler(m_device, tex.sampler, nullptr);
        // descriptor set freed with pool on shutdown
        m_textures.erase(it);
    }

    // Draw image quads using the image pipeline (RGBA textures).
    uint32_t _drawImageBatch(const CmdVec& cmds, size_t from, size_t to, uint32_t vertexCursor) {
        if (!m_imagePipeline) return vertexCursor;

        std::vector<float> verts;
        struct DrawRange { uint32_t first, count; TextureHandle tex; uint8_t tint[4]; };
        std::vector<DrawRange> ranges;

        for (size_t k = from; k < to; ++k) {
            const auto& img = cmds[k].image;
            auto texIt = m_textures.find(img.tex);
            if (texIt == m_textures.end()) continue;

            DrawRange r;
            r.tex   = img.tex;
            std::copy(img.tint, img.tint+4, r.tint);
            r.first = static_cast<uint32_t>(verts.size() / 4);

            float x0 = img.x, y0 = img.y, x1 = img.x + img.w, y1 = img.y + img.h;
            float u0 = img.u0, v0 = img.v0, u1 = img.u1, v1 = img.v1;
            // Triangle 1: top-left, top-right, bottom-left
            verts.insert(verts.end(), {x0,y0,u0,v0, x1,y0,u1,v0, x0,y1,u0,v1});
            // Triangle 2: top-right, bottom-right, bottom-left
            verts.insert(verts.end(), {x1,y0,u1,v0, x1,y1,u1,v1, x0,y1,u0,v1});
            r.count = static_cast<uint32_t>(verts.size() / 4) - r.first;
            ranges.push_back(r);
        }
        if (verts.empty()) return vertexCursor;

        VkDeviceSize surfaceOffset = (VkDeviceSize)m_activeSurfaceId * MAX_TEXT_VERTS * 4 * sizeof(float);
        VkDeviceSize sliceBytes   = verts.size() * sizeof(float);
        VkDeviceSize bufferOffset = surfaceOffset + (VkDeviceSize)vertexCursor * 4 * sizeof(float);
        VkDeviceSize capacity     = (VkDeviceSize)MAX_TEXT_VERTS * 4 * sizeof(float);
        if (vertexCursor * 4 * sizeof(float) + sliceBytes > capacity) return vertexCursor;

        std::memcpy(static_cast<char*>(m_textVertMapped) + bufferOffset,
                    verts.data(), sliceBytes);

        vkCmdBindPipeline(m_act->cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_imagePipeline);
        VkViewport vp{0,0,(float)m_act->extent.width,(float)m_act->extent.height,0,1};
        vkCmdSetViewport(m_act->cmdBuf, 0, 1, &vp);

        VkDeviceSize bindOffset = bufferOffset;
        vkCmdBindVertexBuffers(m_act->cmdBuf, 0, 1, &m_textVertBuffer, &bindOffset);

        for (auto& r : ranges) {
            if (r.count == 0) continue;
            auto texIt = m_textures.find(r.tex);
            if (texIt == m_textures.end()) continue;

            vkCmdBindDescriptorSets(m_act->cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_textPipeLayout, 0, 1, &texIt->second.descSet, 0, nullptr);

            TextPC pc{};
            pc.r   = r.tint[0] / 255.0f; pc.g = r.tint[1] / 255.0f;
            pc.b   = r.tint[2] / 255.0f; pc.a = r.tint[3] / 255.0f;
            pc.vpW = (float)m_act->extent.width; pc.vpH = (float)m_act->extent.height;
            vkCmdPushConstants(m_act->cmdBuf, m_textPipeLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(TextPC), &pc);
            vkCmdDraw(m_act->cmdBuf, r.count, 1, r.first, 0);
        }
        return vertexCursor + static_cast<uint32_t>(verts.size() / 4);
    }

    // Draw anti-aliased vector geometry (per-vertex colour triangle lists).
    uint32_t _drawGeometryBatch(const CmdVec& cmds, size_t from, size_t to, uint32_t vertexCursor) {
        if (!m_vectorPipeline) return vertexCursor;

        std::vector<JRenderVertex> verts;
        for (size_t k = from; k < to; ++k) {
            const auto& g = cmds[k].geom.verts;
            verts.insert(verts.end(), g.begin(), g.end());
        }
        if (verts.empty()) return vertexCursor;

        VkDeviceSize surfaceOffset = (VkDeviceSize)m_activeSurfaceId * MAX_GEOM_VERTS * sizeof(JRenderVertex);
        VkDeviceSize sliceBytes    = verts.size() * sizeof(JRenderVertex);
        VkDeviceSize bufferOffset  = surfaceOffset + (VkDeviceSize)vertexCursor * sizeof(JRenderVertex);
        VkDeviceSize capacity      = (VkDeviceSize)MAX_GEOM_VERTS * sizeof(JRenderVertex);
        if ((VkDeviceSize)vertexCursor * sizeof(JRenderVertex) + sliceBytes > capacity) return vertexCursor;

        std::memcpy(static_cast<char*>(m_geomVertMapped) + bufferOffset, verts.data(), sliceBytes);

        vkCmdBindPipeline(m_act->cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vectorPipeline);
        VkViewport vp{0, 0, (float)m_act->extent.width, (float)m_act->extent.height, 0, 1};
        vkCmdSetViewport(m_act->cmdBuf, 0, 1, &vp);

        VkDeviceSize bindOffset = bufferOffset;
        vkCmdBindVertexBuffers(m_act->cmdBuf, 0, 1, &m_geomVertBuffer, &bindOffset);

        struct VecPC { float vpW, vpH, p0, p1; } pc{
            (float)m_act->extent.width, (float)m_act->extent.height, 0.f, 0.f };
        vkCmdPushConstants(m_act->cmdBuf, m_vectorPipeLayout,
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(m_act->cmdBuf, static_cast<uint32_t>(verts.size()), 1, 0, 0);
        return vertexCursor + static_cast<uint32_t>(verts.size());
    }

    // ---------------------------------------------------------------- Surface helpers
    GpuSurfaceId _allocSurfaceSlot() {
        for (GpuSurfaceId i = 0; i < static_cast<GpuSurfaceId>(m_surfaces.size()); ++i)
            if (!m_surfaces[i].alive) return i;
        m_surfaces.emplace_back();
        return static_cast<GpuSurfaceId>(m_surfaces.size() - 1);
    }

    VkSurfaceKHR _createVkSurface(const JNativeWindowHandle& h) {
        VkSurfaceKHR surf{};
#if defined(__linux__)
        VkXcbSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR};
        ci.connection = static_cast<xcb_connection_t*>(h.connectionPointer);
        ci.window     = static_cast<xcb_window_t>(reinterpret_cast<uintptr_t>(h.windowPointer));
        if (vkCreateXcbSurfaceKHR(m_instance, &ci, nullptr, &surf) != VK_SUCCESS)
            throw std::runtime_error("vkCreateXcbSurfaceKHR failed");
#elif defined(_WIN32)
        VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        ci.hinstance = static_cast<HINSTANCE>(h.connectionPointer);
        ci.hwnd      = static_cast<HWND>(h.windowPointer);
        if (vkCreateWin32SurfaceKHR(m_instance, &ci, nullptr, &surf) != VK_SUCCESS)
            throw std::runtime_error("vkCreateWin32SurfaceKHR failed");
#endif
        return surf;
    }

    void _allocSurfaceCmdAndSync(VulkanSurface& s) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool        = m_commandPool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_device, &ai, &s.cmdBuf);

        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(m_device, &fi, nullptr, &s.inFlight);

    }

    void _freeSurfaceCmdAndSync(VulkanSurface& s) {
        if (s.cmdBuf)   { vkFreeCommandBuffers(m_device, m_commandPool, 1, &s.cmdBuf); s.cmdBuf = VK_NULL_HANDLE; }
        if (s.inFlight) { vkDestroyFence(m_device, s.inFlight, nullptr); s.inFlight = VK_NULL_HANDLE; }
    }

    // Rebuild the swapchain to the surface's current extent.  The CALLER must already
    // have waited on s.inFlight (so the previous frame's command buffer is retired);
    // we then hand off via oldSwapchain instead of a full vkDeviceWaitIdle.
    void _rebuildToCurrentExtent(VulkanSurface& s) {
        uint32_t w = s.pendingW, h = s.pendingH;
        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                m_physicalDevice, s.vkSurface, &caps) == VK_SUCCESS
            && caps.currentExtent.width != UINT32_MAX) {
            w = caps.currentExtent.width;
            h = caps.currentExtent.height;
        }
        if (w == 0) w = s.extent.width;
        if (h == 0) h = s.extent.height;
        _buildSwapchain(s, w, h);
        s.needsRebuild = false;
        s.pendingW = s.pendingH = 0;
    }

    void _buildSwapchain(VulkanSurface& s, uint32_t w, uint32_t h) {
        // Retire the old views/framebuffers now (safe: the caller waited on the frame
        // fence, so no command buffer still references them), but keep the old
        // swapchain handle alive to pass as oldSwapchain — that lets the driver reuse
        // images and present from the old chain until the new one is ready, so the
        // window never blanks during a resize.
        VkSwapchainKHR oldSwapchain = s.swapchain;
        for (auto fb : s.framebuffers) vkDestroyFramebuffer(m_device, fb, nullptr);
        for (auto iv : s.imageViews)   vkDestroyImageView(m_device, iv, nullptr);
        s.framebuffers.clear(); s.imageViews.clear(); s.images.clear();

        s.extent = {w, h};

        VkPresentModeKHR pm = _bestPresentMode(s.vkSurface);

        // Clamp image count to the surface's allowed range (spec requires
        // minImageCount >= caps.minImageCount; a hardcoded 2 violates it on drivers
        // that demand 3, e.g. some real GPUs under a compositor).
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, s.vkSurface, &caps);
        uint32_t imageCount = std::max(2u, caps.minImageCount);
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
            imageCount = caps.maxImageCount;

        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface          = s.vkSurface;
        ci.minImageCount    = imageCount;
        ci.imageFormat      = VK_FORMAT_B8G8R8A8_UNORM;
        ci.imageColorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        ci.imageExtent      = s.extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform     = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode      = pm;
        ci.clipped          = VK_TRUE;
        ci.oldSwapchain     = oldSwapchain;
        VkResult cr = vkCreateSwapchainKHR(m_device, &ci, nullptr, &s.swapchain);
        if (oldSwapchain) vkDestroySwapchainKHR(m_device, oldSwapchain, nullptr);
        if (cr != VK_SUCCESS) {
            std::fprintf(stderr, "[VulkanError] vkCreateSwapchainKHR failed with error code: %d\n", cr);
            std::fflush(stderr);
            throw std::runtime_error("vkCreateSwapchainKHR failed");
        }

        uint32_t n; vkGetSwapchainImagesKHR(m_device, s.swapchain, &n, nullptr);
        s.images.resize(n);
        vkGetSwapchainImagesKHR(m_device, s.swapchain, &n, s.images.data());

        s.imageViews.resize(n); s.framebuffers.resize(n);
        for (size_t i = 0; i < n; ++i) {
            VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            iv.image                       = s.images[i];
            iv.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
            iv.format                      = VK_FORMAT_B8G8R8A8_UNORM;
            iv.subresourceRange            = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCreateImageView(m_device, &iv, nullptr, &s.imageViews[i]);

            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb.renderPass      = m_renderPass;
            fb.attachmentCount = 1;
            fb.pAttachments    = &s.imageViews[i];
            fb.width           = w; fb.height = h; fb.layers = 1;
            vkCreateFramebuffer(m_device, &fb, nullptr, &s.framebuffers[i]);
        }
        qCInfo(LogVulkan) << "Swapchain surface " << (&s - m_surfaces.data())
                          << " " << w << "x" << h << "\n";
    }

    void _destroySwapchain(VulkanSurface& s) {
        for (auto fb : s.framebuffers) vkDestroyFramebuffer(m_device, fb, nullptr);
        for (auto iv : s.imageViews)   vkDestroyImageView(m_device, iv, nullptr);
        if (s.swapchain) vkDestroySwapchainKHR(m_device, s.swapchain, nullptr);
        s.framebuffers.clear(); s.imageViews.clear(); s.images.clear();
        s.swapchain = VK_NULL_HANDLE;
    }

    VkPresentModeKHR _bestPresentMode(VkSurfaceKHR surf) {
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, surf, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, surf, &n, modes.data());
        // Prefer tear-free modes: MAILBOX (vsync + low-latency) then FIFO (always
        // available, vsync). IMMEDIATE tears under continuous rendering and is never
        // chosen by default — an app that wants it can opt in via vsync-off later.
        for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    void _writeScreenshot(VulkanSurface& s) {
        uint32_t w = s.extent.width, h = s.extent.height;
        void* data;
        vkMapMemory(m_device, s.screenshotMem, 0, VK_WHOLE_SIZE, 0, &data);
        const uint8_t* px = static_cast<const uint8_t*>(data);

        // Swapchain format is B8G8R8A8_UNORM — write as RGB PPM
        FILE* f = fopen(s.screenshotSavePath.c_str(), "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            for (uint32_t i = 0; i < w * h; ++i) {
                fwrite(&px[i*4+2], 1, 1, f); // R (was B channel)
                fwrite(&px[i*4+1], 1, 1, f); // G
                fwrite(&px[i*4+0], 1, 1, f); // B (was R channel)
            }
            fclose(f);
            std::cout << "[Vulkan] Screenshot: " << s.screenshotSavePath << "\n" << std::flush;
        }

        vkUnmapMemory(m_device, s.screenshotMem);
        vkDestroyBuffer(m_device, s.screenshotBuf, nullptr);
        vkFreeMemory(m_device, s.screenshotMem, nullptr);
        s.screenshotBuf = VK_NULL_HANDLE;
        s.screenshotMem = VK_NULL_HANDLE;
        s.screenshotSavePath.clear();
    }

    // ---------------------------------------------------------------- Vulkan setup
    VkShaderModule _makeModule(const uint32_t* code, size_t words) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = words * sizeof(uint32_t); ci.pCode = code;
        VkShaderModule m;
        if (vkCreateShaderModule(m_device, &ci, nullptr, &m) != VK_SUCCESS)
            throw std::runtime_error("vkCreateShaderModule failed");
        return m;
    }

    void createInstance() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Genesis UI";
        app.apiVersion       = VK_API_VERSION_1_1;

        const char* exts[] = {"VK_KHR_surface",
#if defined(__linux__)
            "VK_KHR_xcb_surface"
#elif defined(_WIN32)
            "VK_KHR_win32_surface"
#endif
        };
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo        = &app;
        ci.enabledExtensionCount   = 2;
        ci.ppEnabledExtensionNames = exts;
        if (vkCreateInstance(&ci, nullptr, &m_instance) != VK_SUCCESS)
            throw std::runtime_error("vkCreateInstance failed");
    }

    void pickPhysicalDevice() {
        uint32_t n = 0; vkEnumeratePhysicalDevices(m_instance, &n, nullptr);
        if (!n) throw std::runtime_error("No Vulkan GPUs");
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(m_instance, &n, devs.data());
        m_physicalDevice = devs[0];
    }

    void createLogicalDevice() {
        m_queueFamilyIndex = 0;
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = 0; qci.queueCount = 1; qci.pQueuePriorities = &prio;
        const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        ci.pQueueCreateInfos       = &qci; ci.queueCreateInfoCount = 1;
        ci.enabledExtensionCount   = 1;    ci.ppEnabledExtensionNames = dext;
        if (vkCreateDevice(m_physicalDevice, &ci, nullptr, &m_device) != VK_SUCCESS)
            throw std::runtime_error("vkCreateDevice failed");
        vkGetDeviceQueue(m_device, 0, 0, &m_graphicsQueue);
    }

    void createCommandPool() {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = m_queueFamilyIndex;
        vkCreateCommandPool(m_device, &ci, nullptr, &m_commandPool);
    }

    void createRenderPass() {
        VkAttachmentDescription att{};
        att.format         = VK_FORMAT_B8G8R8A8_UNORM;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription  sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpi.attachmentCount = 1; rpi.pAttachments = &att;
        rpi.subpassCount    = 1; rpi.pSubpasses   = &sub;
        vkCreateRenderPass(m_device, &rpi, nullptr, &m_renderPass);
    }

    void createSdfPipelineLayout() {
        static_assert(sizeof(JGpuPrimitiveInstance) % 16 == 0);
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.size       = sizeof(JGpuPrimitiveInstance);
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(m_device, &ci, nullptr, &m_pipelineLayout);
    }

    VkPipeline _buildPipeline(VkShaderModule vert, VkShaderModule frag,
                               VkPipelineLayout layout,
                               const VkVertexInputBindingDescription* bindings, uint32_t bindingCount,
                               const VkVertexInputAttributeDescription* attrs, uint32_t attrCount) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vert; stages[0].pName = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount   = bindingCount;
        vi.pVertexBindingDescriptions      = bindings;
        vi.vertexAttributeDescriptionCount = attrCount;
        vi.pVertexAttributeDescriptions    = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1; vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace   = VK_FRONT_FACE_CLOCKWISE; rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState ba{};
        ba.blendEnable         = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp        = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        ba.alphaBlendOp        = VK_BLEND_OP_ADD;
        ba.colorWriteMask      = 0xF;
        VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cbs.attachmentCount = 1; cbs.pAttachments = &ba;

        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.stageCount          = 2;   pci.pStages             = stages;
        pci.pVertexInputState   = &vi; pci.pInputAssemblyState = &ia;
        pci.pViewportState      = &vps; pci.pRasterizationState = &rs;
        pci.pMultisampleState   = &ms; pci.pColorBlendState    = &cbs;
        pci.pDynamicState       = &ds;
        pci.layout              = layout;
        pci.renderPass          = m_renderPass;

        VkPipeline pipe;
        if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipe) != VK_SUCCESS)
            throw std::runtime_error("vkCreateGraphicsPipelines failed");
        return pipe;
    }

    void createSdfPipeline() {
        auto vert = _makeModule(Shaders::kRectVert, sizeof(Shaders::kRectVert)/4);
        auto frag = _makeModule(Shaders::kRectFrag, sizeof(Shaders::kRectFrag)/4);
        m_sdfPipeline = _buildPipeline(vert, frag, m_pipelineLayout, nullptr, 0, nullptr, 0);
        vkDestroyShaderModule(m_device, vert, nullptr);
        vkDestroyShaderModule(m_device, frag, nullptr);
        qCInfo(LogVulkan) << "SDF rect pipeline created.\n";
    }

    void createTextDescriptorLayout() {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1; ci.pBindings = &b;
        vkCreateDescriptorSetLayout(m_device, &ci, nullptr, &m_textDescSetLayout);

        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.poolSizeCount = 1; pci.pPoolSizes = &ps; pci.maxSets = 1;
        vkCreateDescriptorPool(m_device, &pci, nullptr, &m_textDescPool);
    }

    void createTextPipeline() {
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.size       = 32;
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount         = 1; lci.pSetLayouts         = &m_textDescSetLayout;
        lci.pushConstantRangeCount = 1; lci.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(m_device, &lci, nullptr, &m_textPipeLayout);

        VkVertexInputBindingDescription binding{0, 4 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[2] = {
            {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float)},
        };

        auto vert = _makeModule(Shaders::kTextVert, sizeof(Shaders::kTextVert)/4);
        auto frag = _makeModule(Shaders::kTextFrag, sizeof(Shaders::kTextFrag)/4);
        m_textPipeline = _buildPipeline(vert, frag, m_textPipeLayout, &binding, 1, attrs, 2);
        vkDestroyShaderModule(m_device, vert, nullptr);
        vkDestroyShaderModule(m_device, frag, nullptr);
        qCInfo(LogVulkan) << "Text pipeline created.\n";
    }

    void createImagePipeline() {
        // Allocate descriptor pool for up to 256 image textures.
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.poolSizeCount = 1; pci.pPoolSizes = &ps; pci.maxSets = 256;
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        vkCreateDescriptorPool(m_device, &pci, nullptr, &m_imageDescPool);

        // Image pipeline uses same vert as text but different frag (full RGBA output).
        auto vert = _makeModule(Shaders::kImageVert, sizeof(Shaders::kImageVert)/4);
        auto frag = _makeModule(Shaders::kImageFrag, sizeof(Shaders::kImageFrag)/4);

        VkVertexInputBindingDescription binding{0, 4 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[2] = {
            {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float)},
        };
        // Reuse the text pipeline layout (same descriptor set layout + push constants).
        m_imagePipeline = _buildPipeline(vert, frag, m_textPipeLayout, &binding, 1, attrs, 2);
        vkDestroyShaderModule(m_device, vert, nullptr);
        vkDestroyShaderModule(m_device, frag, nullptr);
        qCInfo(LogVulkan) << "Image pipeline created.\n";
    }

    void createTextVertexBuffer() {
        VkDeviceSize sz = (VkDeviceSize)16 * MAX_TEXT_VERTS * 4 * sizeof(float);
        createBuffer(m_device, m_physicalDevice, sz,
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     m_textVertBuffer, m_textVertMemory);
        vkMapMemory(m_device, m_textVertMemory, 0, sz, 0, &m_textVertMapped);
    }

    // Vector geometry pipeline: per-vertex position/uv/colour, no descriptor set,
    // single push constant carrying the viewport size (pixels → NDC).
    void createVectorPipeline() {
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pcr.offset     = 0;
        pcr.size       = 16;   // float vpW, vpH, pad0, pad1
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount         = 0; lci.pSetLayouts         = nullptr;
        lci.pushConstantRangeCount = 1; lci.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(m_device, &lci, nullptr, &m_vectorPipeLayout);

        // JRenderVertex: float position[2] @0, float texCoord[2] @8, uint8 color[4] @16, stride 20.
        VkVertexInputBindingDescription binding{0, sizeof(JRenderVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[3] = {
            {0, 0, VK_FORMAT_R32G32_SFLOAT,  0},
            {1, 0, VK_FORMAT_R32G32_SFLOAT,  8},
            {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16},
        };
        auto vert = _makeModule(Shaders::kVectorVert, sizeof(Shaders::kVectorVert)/4);
        auto frag = _makeModule(Shaders::kVectorFrag, sizeof(Shaders::kVectorFrag)/4);
        m_vectorPipeline = _buildPipeline(vert, frag, m_vectorPipeLayout, &binding, 1, attrs, 3);
        vkDestroyShaderModule(m_device, vert, nullptr);
        vkDestroyShaderModule(m_device, frag, nullptr);
        qCInfo(LogVulkan) << "Vector pipeline created.\n";
    }

    void createGeomVertexBuffer() {
        VkDeviceSize sz = (VkDeviceSize)16 * MAX_GEOM_VERTS * sizeof(JRenderVertex);
        createBuffer(m_device, m_physicalDevice, sz,
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     m_geomVertBuffer, m_geomVertMemory);
        vkMapMemory(m_device, m_geomVertMemory, 0, sz, 0, &m_geomVertMapped);
    }

    // ---------------------------------------------------------------- Members
    JNativeWindowHandle m_handle;
    VkInstance         m_instance{VK_NULL_HANDLE};
    VkPhysicalDevice   m_physicalDevice{VK_NULL_HANDLE};
    VkDevice           m_device{VK_NULL_HANDLE};
    uint32_t           m_queueFamilyIndex{0};
    VkQueue            m_graphicsQueue{VK_NULL_HANDLE};
    VkRenderPass       m_renderPass{VK_NULL_HANDLE};
    VkCommandPool      m_commandPool{VK_NULL_HANDLE};

    std::vector<VulkanSurface> m_surfaces;
    VulkanSurface*             m_act{nullptr};  // active surface, set in beginFrame
    GpuSurfaceId               m_activeSurfaceId{kPrimarySurface};

    // SDF rect pipeline
    VkPipelineLayout   m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline         m_sdfPipeline{VK_NULL_HANDLE};

    // Font atlas
    VkImage            m_atlasImage{VK_NULL_HANDLE};
    VkDeviceMemory     m_atlasMemory{VK_NULL_HANDLE};
    VkImageView        m_atlasView{VK_NULL_HANDLE};
    VkSampler          m_atlasSampler{VK_NULL_HANDLE};
    bool               m_atlasUploaded{false};

    // Text pipeline
    VkDescriptorSetLayout m_textDescSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_textDescPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_textDescSet{VK_NULL_HANDLE};
    VkPipelineLayout      m_textPipeLayout{VK_NULL_HANDLE};
    VkPipeline            m_textPipeline{VK_NULL_HANDLE};
    static constexpr uint32_t MAX_TEXT_VERTS = 65536;
    VkBuffer              m_textVertBuffer{VK_NULL_HANDLE};
    VkDeviceMemory        m_textVertMemory{VK_NULL_HANDLE};
    void*                 m_textVertMapped{nullptr};

    uint32_t m_frameCounter{0};

    // Image pipeline
    VkPipeline                                    m_imagePipeline{VK_NULL_HANDLE};
    VkDescriptorPool                              m_imageDescPool{VK_NULL_HANDLE};
    std::unordered_map<TextureHandle, VulkanTexture> m_textures;
    TextureHandle                                 m_nextTexHandle{1};

    // Vector geometry pipeline (anti-aliased 2D paths, per-vertex colour)
    VkPipelineLayout      m_vectorPipeLayout{VK_NULL_HANDLE};
    VkPipeline            m_vectorPipeline{VK_NULL_HANDLE};
    static constexpr uint32_t MAX_GEOM_VERTS = 1u << 16;  // per surface
    VkBuffer              m_geomVertBuffer{VK_NULL_HANDLE};
    VkDeviceMemory        m_geomVertMemory{VK_NULL_HANDLE};
    void*                 m_geomVertMapped{nullptr};
};


std::unique_ptr<JGpuHal> JGpuHal::create(JGpuApiType api, const JNativeWindowHandle& h) {
    if (api == JGpuApiType::Vulkan) {
        try {
            auto hal = std::make_unique<VulkanGpuHal>(h);
            if (hal->initialize()) {
                return hal;
            }
        } catch (const std::exception& e) {
            std::cerr << "[GENESIS] Vulkan hardware initialization failed: " << e.what() << ". Falling back to Software API.\n";
        }
    }
    
    // Software fallback
    auto softwareHal = std::make_unique<SoftwareGpuHal>(h);
    if (softwareHal->initialize()) {
        return softwareHal;
    }
    return nullptr;
}

} // inline namespace jf
