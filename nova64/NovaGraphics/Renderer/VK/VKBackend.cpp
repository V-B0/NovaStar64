/**
 * @file VKBackend.cpp
 * @brief Vulkan IRenderer implementation — Stage 1 (init + clear).
 */
// SPDX-License-Identifier: MIT
//
// What works in this stage:
//   * volk loader init, instance + debug messenger, device, surface, swapchain.
//   * BeginFrame acquires the next swapchain image.
//   * EndFrame records a command buffer that transitions the image to
//     TRANSFER_DST, vkCmdClearColorImage with the latest clear color, then
//     transitions to PRESENT_SRC and submits with the standard
//     image-available / render-finished semaphores + per-frame fence.
//   * Present queue-presents the image.
//   * Window resize (OnResize) recreates the swapchain.
//
// What is intentionally still a no-op (Stage 2+):
//   * All resource creation (CreateVertexBuffer, CreateTexture, …) returns
//     valid no-op objects (callers won't crash on null deref).
//   * Draw / DrawArrays / DrawIndexed do nothing — no real geometry yet.
//   * BindShader / BindPipeline / BindRenderTarget / state setters are no-ops.
//   * BindImageForRW, DispatchCompute, ReadPixels are no-ops.
//
// IsVulkanAvailable() now performs a real probe: it tries to volkInitialize()
// and call vkEnumerateInstanceVersion. DetectBestBackend() still does NOT
// auto-pick Vulkan (you must explicitly request it) until draw-call support
// lands in Stage 4.

#include "VKBackend.hpp"
#include "VKCommon.hpp"
#include "VKInstance.hpp"
#include "VKDevice.hpp"
#include "VKSwapchain.hpp"
#include "VKMemory.hpp"
#include "VKBuffer.hpp"
#include "VKTexture.hpp"
#include "VKShader.hpp"
#include "VKPipeline.hpp"
#include "VKRenderTarget.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include <array>
#include <cstring>
#include <memory>
#include <vector>

namespace Nova64 { namespace Video { namespace VK {

// ─────────────────────────────────────────────────────────────────────────────
//  IsVulkanAvailable — real probe via volk
// ─────────────────────────────────────────────────────────────────────────────

bool IsVulkanAvailable() {
    static int cached = -1; // -1 unknown, 0 no, 1 yes
    if (cached != -1) return cached == 1;

    if (volkInitialize() != VK_SUCCESS) {
        cached = 0;
        return false;
    }
    if (!vkEnumerateInstanceVersion) {
        // Vulkan 1.0 loader only — we require 1.1+ for vkEnumerateInstanceVersion.
        cached = 0;
        return false;
    }
    uint32_t apiVersion = 0;
    if (vkEnumerateInstanceVersion(&apiVersion) != VK_SUCCESS) {
        cached = 0;
        return false;
    }
    cached = (apiVersion >= VK_API_VERSION_1_1) ? 1 : 0;
    return cached == 1;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Stub resources (Stage 2+ will replace these with real implementations)
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Vertex/Index/Uniform buffers and Texture are now real (see VKBuffer/VKTexture).
// Shaders and Pipelines are real (see VKShader/VKPipeline).
// Only resources that are still no-ops live here.

// Vertex/Index/Uniform buffers, Texture, Shader, Pipeline, RenderTarget are all
// real now. No local stubs remain in Stage 5.

constexpr uint32_t kFramesInFlight = 2;

struct PerFrame {
    VkCommandBuffer cmd            = VK_NULL_HANDLE;
    VkSemaphore     imageAvailable = VK_NULL_HANDLE;
    VkSemaphore     renderFinished = VK_NULL_HANDLE;
    VkFence         inFlightFence  = VK_NULL_HANDLE;
};

class VulkanRenderer final : public IRenderer {
public:
    VulkanRenderer() {
        m_caps.backend       = BackendType::Vulkan;
        m_caps.deviceName    = "Vulkan (uninitialized)";
        m_caps.driverVersion = "0.0.0";
        m_caps.apiVersion    = "Vulkan";
    }
    ~VulkanRenderer() override { Shutdown(); }

    // ─── Lifecycle ────────────────────────────────────────────
    bool Initialize(void* windowHandle, uint32_t width, uint32_t height) override {
        m_sdlWindow = static_cast<SDL_Window*>(windowHandle);
        m_width  = width;
        m_height = height;

        if (!m_sdlWindow) {
            Logger::Error("VulkanRenderer: null SDL_Window passed to Initialize");
            return false;
        }
        if (!IsVulkanAvailable()) {
            Logger::Warn("VulkanRenderer: Vulkan loader not available");
            return false;
        }

        // ── instance extensions required by SDL ─────────────
        unsigned int extCount = 0;
        if (!SDL_Vulkan_GetInstanceExtensions(m_sdlWindow, &extCount, nullptr)) {
            Logger::Error("VulkanRenderer: SDL_Vulkan_GetInstanceExtensions(count) failed: {}",
                          SDL_GetError());
            return false;
        }
        std::vector<const char*> extNames(extCount);
        if (extCount && !SDL_Vulkan_GetInstanceExtensions(m_sdlWindow, &extCount, extNames.data())) {
            Logger::Error("VulkanRenderer: SDL_Vulkan_GetInstanceExtensions(names) failed: {}",
                          SDL_GetError());
            return false;
        }

#ifdef NDEBUG
        const bool wantValidation = false;
#else
        const bool wantValidation = true;
#endif
        if (!m_instance.Create(extNames, wantValidation)) {
            Logger::Error("VulkanRenderer: instance creation failed");
            return false;
        }

        // ── surface (requires instance) ─────────────────────
        if (!SDL_Vulkan_CreateSurface(m_sdlWindow, m_instance.Get(), &m_surface)) {
            Logger::Error("VulkanRenderer: SDL_Vulkan_CreateSurface failed: {}",
                          SDL_GetError());
            Cleanup();
            return false;
        }

        // ── physical device ─────────────────────────────────
        uint32_t gfxFam = 0, presFam = 0;
        if (!m_instance.PickPhysicalDevice(m_surface, gfxFam, presFam)) {
            Cleanup();
            return false;
        }

        // ── logical device ──────────────────────────────────
        if (!m_device.Create(m_instance.GetPhysical(), gfxFam, presFam)) {
            Cleanup();
            return false;
        }

        // ── swapchain ───────────────────────────────────────
        if (!m_swapchain.Create(m_instance.GetPhysical(), m_device.Get(), m_surface,
                                gfxFam, presFam, m_width, m_height, /*vsync=*/true)) {
            Cleanup();
            return false;
        }

        // ── per-frame command pool + sync ───────────────────
        if (!CreateFrameResources()) {
            Cleanup();
            return false;
        }

        // ── transient pool for one-shot uploads (Stage 2) ───
        {
            VkCommandPoolCreateInfo tpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            tpci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            tpci.queueFamilyIndex = m_device.GetGraphicsFamily();
            if (vkCreateCommandPool(m_device.Get(), &tpci, nullptr, &m_transientPool) != VK_SUCCESS) {
                Logger::Error("VulkanRenderer: vkCreateCommandPool(transient) failed");
                Cleanup();
                return false;
            }
        }
        m_ctx.physical      = m_instance.GetPhysical();
        m_ctx.device        = m_device.Get();
        m_ctx.transferQueue = m_device.GetGraphicsQueue();
        m_ctx.transientPool = m_transientPool;

        // Shared pipeline cache (empty initial data; could be persisted to
        // disk later for faster cold starts).
        VkPipelineCacheCreateInfo pcci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        if (vkCreatePipelineCache(m_device.Get(), &pcci, nullptr, &m_pipelineCache) != VK_SUCCESS) {
            Logger::Warn("VulkanRenderer: vkCreatePipelineCache failed (non-fatal)");
            m_pipelineCache = VK_NULL_HANDLE;
        }
        m_ctx.pipelineCache = m_pipelineCache;

        // ── standard descriptor set layout + per-frame pools (Stage 4) ─
        m_setLayout = CreateStandardSetLayout(m_device.Get());
        if (!m_setLayout) { Cleanup(); return false; }
        if (!CreateDescriptorPools()) { Cleanup(); return false; }

        // Update caps with real device info
        const auto& props = m_instance.GetProps();
        m_caps.deviceName    = props.deviceName;
        m_caps.driverVersion = std::to_string(props.driverVersion);
        m_caps.apiVersion    = "Vulkan "
                             + std::to_string(VK_VERSION_MAJOR(props.apiVersion)) + "."
                             + std::to_string(VK_VERSION_MINOR(props.apiVersion)) + "."
                             + std::to_string(VK_VERSION_PATCH(props.apiVersion));
        m_caps.maxTextureSize = props.limits.maxImageDimension2D;
        m_caps.maxTextureUnits = 16;
        m_caps.supportsCompute = true;
        m_caps.supportsAnisotropicFiltering = true;
        m_caps.maxAnisotropy = props.limits.maxSamplerAnisotropy;
        m_caps.supportsDebugMarkers = m_instance.IsValidationEnabled();

        Logger::Info("VulkanRenderer: initialized (Stage 1: clear-only)");
        return true;
    }

    void Shutdown() override {
        if (!m_initialized && !m_device.Get()) return;
        if (m_device.Get()) m_device.WaitIdle();
        DestroyTransient();
        DestroyFrameResources();
        m_swapchain.Destroy(m_device.Get());
        m_device.Destroy();
        if (m_surface) {
            vkDestroySurfaceKHR(m_instance.Get(), m_surface, nullptr);
            m_surface = VK_NULL_HANDLE;
        }
        m_instance.Destroy();
        m_initialized = false;
    }

    void BeginFrame() override {
        if (!m_initialized) return;

        PerFrame& f = m_frames[m_currentFrame];
        vkWaitForFences(m_device.Get(), 1, &f.inFlightFence, VK_TRUE, UINT64_MAX);

        VkResult r = vkAcquireNextImageKHR(m_device.Get(), m_swapchain.Get(),
                                           UINT64_MAX, f.imageAvailable,
                                           VK_NULL_HANDLE, &m_currentImage);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            RecreateSwapchain();
            m_skipPresent = true;
            return;
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            Logger::Error("VulkanRenderer: vkAcquireNextImageKHR failed: {}",
                          VkResultToString(r));
            m_skipPresent = true;
            return;
        }

        vkResetFences(m_device.Get(), 1, &f.inFlightFence);
        vkResetCommandBuffer(f.cmd, 0);

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.cmd, &bi);
        m_skipPresent = false;

        // Reset per-frame descriptor pool (frees all sets allocated last time
        // this frame index was active).
        if (m_descPools[m_currentFrame]) {
            vkResetDescriptorPool(m_device.Get(), m_descPools[m_currentFrame], 0);
        }

        // Reset per-frame draw state cache.
        m_curPipeline   = nullptr;
        m_curShader     = nullptr;
        m_curVB         = VK_NULL_HANDLE;
        m_curIB         = VK_NULL_HANDLE;
        m_curIndexType  = VK_INDEX_TYPE_UINT16;
        m_curUBO        = VK_NULL_HANDLE;
        m_curUBOSize    = 0;
        for (auto& t : m_curTextures) t = nullptr;

        // Begin the swapchain render pass (or pending custom RT) with
        // LOAD_OP_CLEAR using the latest m_clearColor. Draws happen between
        // Begin/EndFrame.
        BeginPass(m_pendingRT);
        m_pendingRT = nullptr;
    }

    void EndFrame() override {
        if (!m_initialized || m_skipPresent) return;
        PerFrame& f = m_frames[m_currentFrame];

        if (m_passActive) {
            vkCmdEndRenderPass(f.cmd);
            m_passActive = false;
        }
        vkEndCommandBuffer(f.cmd);

        // 4. Submit
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &f.imageAvailable;
        si.pWaitDstStageMask    = &waitStage;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &f.cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &f.renderFinished;
        VkResult sr = vkQueueSubmit(m_device.GetGraphicsQueue(), 1, &si, f.inFlightFence);
        if (sr != VK_SUCCESS) {
            Logger::Error("VulkanRenderer: vkQueueSubmit failed: {}", VkResultToString(sr));
        }
    }

    void Present() override {
        if (!m_initialized || m_skipPresent) {
            m_currentFrame = (m_currentFrame + 1) % kFramesInFlight;
            return;
        }
        PerFrame& f = m_frames[m_currentFrame];
        VkSwapchainKHR sc = m_swapchain.Get();

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &f.renderFinished;
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &sc;
        pi.pImageIndices      = &m_currentImage;

        VkResult r = vkQueuePresentKHR(m_device.GetPresentQueue(), &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            RecreateSwapchain();
        } else if (r != VK_SUCCESS) {
            Logger::Error("VulkanRenderer: vkQueuePresentKHR failed: {}", VkResultToString(r));
        }
        m_currentFrame = (m_currentFrame + 1) % kFramesInFlight;
    }

    // ─── Resource Creation (Stage 2: real buffers + textures) ─
    std::unique_ptr<IVertexBuffer> CreateVertexBuffer(size_t s, BufferUsage u) override {
        return std::make_unique<VKVertexBufferImpl>(&m_ctx, s, u);
    }
    std::unique_ptr<IIndexBuffer> CreateIndexBuffer(size_t s, BufferUsage u) override {
        return std::make_unique<VKIndexBufferImpl>(&m_ctx, s, u);
    }
    std::unique_ptr<IUniformBuffer> CreateUniformBuffer(size_t s) override {
        return std::make_unique<VKUniformBufferImpl>(&m_ctx, s);
    }
    std::unique_ptr<ITexture> CreateTexture(const TextureDesc& d) override {
        return std::make_unique<VKTextureImpl>(&m_ctx, d);
    }
    std::unique_ptr<IShaderProgram> CreateShaderProgram() override {
        return std::make_unique<VKShaderProgramImpl>(&m_ctx);
    }
    std::unique_ptr<IRenderTarget> CreateRenderTarget(const RenderTargetDesc& d) override {
        return std::make_unique<VKRenderTargetImpl>(&m_ctx, d);
    }
    std::unique_ptr<IPipeline> CreatePipeline(const PipelineDesc& d) override {
        auto p = std::make_unique<VKPipelineImpl>();
        // If the attached shader is compute-only, build a compute pipeline.
        auto* sh = static_cast<VKShaderProgramImpl*>(d.shader);
        if (sh && sh->IsCompute()) {
            if (!p->BuildCompute(m_ctx, sh->CS(), m_setLayout)) {
                Logger::Error("VulkanRenderer: compute pipeline build failed");
            }
        } else if (!p->Build(m_ctx, d, m_swapchain.GetRenderPass(), m_setLayout)) {
            Logger::Error("VulkanRenderer: pipeline build failed");
        }
        return p;
    }

    // ─── State Management ─────────────────────────────────
    void SetViewport(const RenderViewport& v) override {
        if (!m_passActive) return;
        VkViewport vp{};
        vp.x = v.x; vp.y = v.y;
        vp.width = v.width; vp.height = v.height;
        vp.minDepth = v.minDepth; vp.maxDepth = v.maxDepth;
        vkCmdSetViewport(m_frames[m_currentFrame].cmd, 0, 1, &vp);
    }
    void SetScissor(const RenderScissorRect& r) override {
        if (!m_passActive) return;
        VkRect2D sc{};
        sc.offset = { r.x, r.y };
        sc.extent = { static_cast<uint32_t>(r.width > 0 ? r.width : 0),
                      static_cast<uint32_t>(r.height > 0 ? r.height : 0) };
        vkCmdSetScissor(m_frames[m_currentFrame].cmd, 0, 1, &sc);
    }
    void SetBlendState(const BlendState&) override {}      // baked into pipeline
    void SetDepthStencilState(const DepthStencilState&) override {}
    void SetRasterizerState(const RasterizerState&) override {}
    void SetBlendColor(float r, float g, float b, float a) override {
        // Real blend constants via dynamic state; also kept as the swapchain
        // clear color so callers without an explicit clear can still see
        // their state reflected.
        const float c[4] = { r, g, b, a };
        m_clearColor[0] = r; m_clearColor[1] = g;
        m_clearColor[2] = b; m_clearColor[3] = a;
        if (m_passActive) {
            vkCmdSetBlendConstants(m_frames[m_currentFrame].cmd, c);
        }
    }

    // ─── Binding ──────────────────────────────────────────────
    void BindVertexBuffer(IVertexBuffer* vb, const VertexLayout&) override {
        m_curVB = vb ? static_cast<VKVertexBufferImpl*>(vb)->GetVk() : VK_NULL_HANDLE;
    }
    void BindIndexBuffer(IIndexBuffer* ib) override {
        if (ib) {
            auto* vib = static_cast<VKIndexBufferImpl*>(ib);
            m_curIB        = vib->GetVk();
            m_curIndexType = (vib->GetIndexType() == IndexType::UInt16)
                           ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        } else {
            m_curIB = VK_NULL_HANDLE;
        }
    }
    void BindTexture(uint32_t unit, ITexture* tex) override {
        if (unit >= kVKMaxTexturesPerDraw) return;
        m_curTextures[unit] = static_cast<VKTextureImpl*>(tex);
    }
    void BindShader(IShaderProgram* shader) override {
        m_curShader = static_cast<VKShaderProgramImpl*>(shader);
        // Heuristic: if a shader uses the renderer's CreateUniformBuffer + BindSlot
        // pattern, the uniform buffer is bound separately via that path. We track
        // it here so descriptor writes can find it.
    }
    void BindPipeline(IPipeline* pipeline) override {
        m_curPipeline = static_cast<VKPipelineImpl*>(pipeline);
        if (m_passActive && m_curPipeline && m_curPipeline->Get()) {
            vkCmdBindPipeline(m_frames[m_currentFrame].cmd,
                              VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_curPipeline->Get());
            ++m_stats.pipelineBinds;
        }
    }
    void BindRenderTarget(IRenderTarget* rt) override {
        if (!m_passActive) {
            m_pendingRT = static_cast<VKRenderTargetImpl*>(rt);
            return;
        }
        // End the current pass and switch.
        PerFrame& f = m_frames[m_currentFrame];
        vkCmdEndRenderPass(f.cmd);
        m_passActive = false;
        BeginPass(static_cast<VKRenderTargetImpl*>(rt));
        // Re-bind cached pipeline if any (it may not be compatible; user is
        // expected to bind a pipeline built for the target's pass).
        if (m_curPipeline && m_curPipeline->Get()) {
            vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_curPipeline->Get());
        }
    }

    // ─── Image RW / draw / compute / readback (Stage 5+) ─────
    void BindImageForRW(uint32_t, uint64_t, ImageBindFormat) override {}
    void UnbindAllImages() override {}
    void SetColorWriteMask(bool, bool, bool, bool) override {}
    void ImageMemoryBarrier() override {}

    void Draw(const DrawCommand& cmd) override {
        if (cmd.indexed) {
            DrawIndexed(cmd.primitive, cmd.indexCount, IndexType::UInt16, cmd.indexOffset);
        } else {
            DrawArrays(cmd.primitive, cmd.vertexOffset, cmd.vertexCount);
        }
    }
    void DrawArrays(PrimitiveType, uint32_t first, uint32_t count) override {
        if (!PrepareDraw()) return;
        vkCmdDraw(m_frames[m_currentFrame].cmd, count, 1, first, 0);
        ++m_stats.drawCalls;
        m_stats.triangles += count / 3;
    }
    void DrawIndexed(PrimitiveType, uint32_t indexCount, IndexType type, uint32_t offset) override {
        if (!PrepareDraw()) return;
        if (m_curIB == VK_NULL_HANDLE) return;
        VkIndexType it = (type == IndexType::UInt16) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        // Re-bind in case the cached state's index type differs from this call.
        vkCmdBindIndexBuffer(m_frames[m_currentFrame].cmd, m_curIB, 0, it);
        vkCmdDrawIndexed(m_frames[m_currentFrame].cmd, indexCount, 1, offset, 0, 0);
        ++m_stats.drawCalls;
        m_stats.triangles += indexCount / 3;
    }
    void DispatchCompute(uint32_t gx, uint32_t gy, uint32_t gz) override {
        if (!m_initialized || !m_curPipeline || !m_curPipeline->Get()) return;
        if (!m_curPipeline->IsCompute()) {
            Logger::Warn("VulkanRenderer: DispatchCompute on non-compute pipeline");
            return;
        }
        PerFrame& f = m_frames[m_currentFrame];
        // Compute must run outside a render pass.
        bool wasActive = m_passActive;
        if (wasActive) { vkCmdEndRenderPass(f.cmd); m_passActive = false; }

        vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          m_curPipeline->Get());
        // Allocate + write descriptor set (UBO + textures, same as draw).
        VkDescriptorSet ds = AllocateAndWriteDescriptorSet();
        if (ds) {
            vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    m_curPipeline->GetLayout(), 0, 1, &ds, 0, nullptr);
            vkCmdDispatch(f.cmd, gx, gy, gz);
        }
        if (wasActive) BeginPass(m_currentRT);
    }
    void ReadPixels(uint32_t, uint32_t, uint32_t, uint32_t,
                    TextureFormat, void*) override {}

    // ─── Debug ────────────────────────────────────────────────
    // VK_EXT_debug_utils is enabled with validation; functions are loaded by
    // volk (null when extension absent — guard).
    void PushDebugGroup(const char* name) override {
        if (!m_passActive && !m_initialized) return;
        if (!vkCmdBeginDebugUtilsLabelEXT) return;
        VkDebugUtilsLabelEXT lbl{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        lbl.pLabelName = name ? name : "";
        vkCmdBeginDebugUtilsLabelEXT(m_frames[m_currentFrame].cmd, &lbl);
    }
    void PopDebugGroup() override {
        if (!vkCmdEndDebugUtilsLabelEXT) return;
        vkCmdEndDebugUtilsLabelEXT(m_frames[m_currentFrame].cmd);
    }
    void InsertDebugMarker(const char* name) override {
        if (!vkCmdInsertDebugUtilsLabelEXT) return;
        VkDebugUtilsLabelEXT lbl{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        lbl.pLabelName = name ? name : "";
        vkCmdInsertDebugUtilsLabelEXT(m_frames[m_currentFrame].cmd, &lbl);
    }

    // ─── Queries ──────────────────────────────────────────────
    BackendType GetBackendType() const override { return BackendType::Vulkan; }
    const RendererCaps& GetCapabilities() const override { return m_caps; }
    const RendererStats& GetStats() const override { return m_stats; }
    void ResetStats() override { m_stats.Reset(); }

    // ─── Window ───────────────────────────────────────────────
    void OnResize(uint32_t width, uint32_t height) override {
        if (width == m_width && height == m_height) return;
        m_width = width; m_height = height;
        if (m_initialized) RecreateSwapchain();
    }
    uint32_t GetWidth()  const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }

private:
    bool CreateFrameResources() {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = m_device.GetGraphicsFamily();
        VK_CHECK(vkCreateCommandPool(m_device.Get(), &pci, nullptr, &m_cmdPool));

        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool        = m_cmdPool;
        cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;

        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VK_CHECK(vkAllocateCommandBuffers(m_device.Get(), &cai, &m_frames[i].cmd));
            VK_CHECK(vkCreateSemaphore(m_device.Get(), &sci, nullptr, &m_frames[i].imageAvailable));
            VK_CHECK(vkCreateSemaphore(m_device.Get(), &sci, nullptr, &m_frames[i].renderFinished));
            VK_CHECK(vkCreateFence(m_device.Get(), &fci, nullptr, &m_frames[i].inFlightFence));
        }
        m_initialized = true;
        return true;
    }

    void DestroyTransient() {
        if (m_transientPool && m_device.Get()) {
            vkDestroyCommandPool(m_device.Get(), m_transientPool, nullptr);
            m_transientPool = VK_NULL_HANDLE;
        }
        m_ctx = {};
    }

    void DestroyFrameResources() {
        VkDevice dev = m_device.Get();
        if (!dev) return;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (m_frames[i].imageAvailable) {
                vkDestroySemaphore(dev, m_frames[i].imageAvailable, nullptr);
                m_frames[i].imageAvailable = VK_NULL_HANDLE;
            }
            if (m_frames[i].renderFinished) {
                vkDestroySemaphore(dev, m_frames[i].renderFinished, nullptr);
                m_frames[i].renderFinished = VK_NULL_HANDLE;
            }
            if (m_frames[i].inFlightFence) {
                vkDestroyFence(dev, m_frames[i].inFlightFence, nullptr);
                m_frames[i].inFlightFence = VK_NULL_HANDLE;
            }
            m_frames[i].cmd = VK_NULL_HANDLE; // freed with pool
        }
        if (m_cmdPool) {
            vkDestroyCommandPool(dev, m_cmdPool, nullptr);
            m_cmdPool = VK_NULL_HANDLE;
        }
    }

    void Cleanup() {
        if (m_device.Get()) m_device.WaitIdle();
        // Destroy Stage-4 owned objects first (they reference the device).
        if (m_device.Get()) {
            m_whiteTex.reset();
            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                if (m_descPools[i]) {
                    vkDestroyDescriptorPool(m_device.Get(), m_descPools[i], nullptr);
                    m_descPools[i] = VK_NULL_HANDLE;
                }
                if (m_scratchUBO[i].buffer) {
                    DestroyBufferStorage(m_ctx, m_scratchUBO[i]);
                }
            }
            if (m_setLayout) {
                vkDestroyDescriptorSetLayout(m_device.Get(), m_setLayout, nullptr);
                m_setLayout = VK_NULL_HANDLE;
            }
            if (m_pipelineCache) {
                vkDestroyPipelineCache(m_device.Get(), m_pipelineCache, nullptr);
                m_pipelineCache = VK_NULL_HANDLE;
                m_ctx.pipelineCache = VK_NULL_HANDLE;
            }
        }
        DestroyTransient();
        DestroyFrameResources();
        m_swapchain.Destroy(m_device.Get());
        m_device.Destroy();
        if (m_surface) {
            vkDestroySurfaceKHR(m_instance.Get(), m_surface, nullptr);
            m_surface = VK_NULL_HANDLE;
        }
        m_instance.Destroy();
        m_initialized = false;
    }

    void RecreateSwapchain() {
        if (m_width == 0 || m_height == 0) return;
        m_swapchain.Recreate(m_instance.GetPhysical(), m_device.Get(), m_surface,
                             m_device.GetGraphicsFamily(), m_device.GetPresentFamily(),
                             m_width, m_height, /*vsync=*/true);
    }

    static void TransitionImage(VkCommandBuffer cmd, VkImage image,
                                VkImageLayout oldLayout, VkImageLayout newLayout,
                                VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                                VkPipelineStageFlags srcStage,
                                VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout           = oldLayout;
        b.newLayout           = newLayout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // ── Stage 4: descriptor pools, default UBO + white texture, PrepareDraw ──
    bool CreateDescriptorPools() {
        std::array<VkDescriptorPoolSize, 2> sizes{};
        sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = kMaxSetsPerFrame;
        sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = kMaxSetsPerFrame * kVKMaxTexturesPerDraw;

        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets       = kMaxSetsPerFrame;
        pi.poolSizeCount = static_cast<uint32_t>(sizes.size());
        pi.pPoolSizes    = sizes.data();

        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (vkCreateDescriptorPool(m_device.Get(), &pi, nullptr, &m_descPools[i]) != VK_SUCCESS) {
                Logger::Error("VulkanRenderer: vkCreateDescriptorPool failed");
                return false;
            }
            // Per-frame scratch UBO (host-visible, persistently mapped).
            if (!CreateBufferStorage(m_ctx, kScratchUBOSize,
                                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                     m_scratchUBO[i])) {
                Logger::Error("VulkanRenderer: scratch UBO alloc failed");
                return false;
            }
            if (m_scratchUBO[i].mapped) {
                std::memset(m_scratchUBO[i].mapped, 0, kScratchUBOSize);
            }
        }

        // 1×1 white default texture (used for any unbound texture slot so the
        // standard descriptor set always has a complete set of valid images).
        TextureDesc td{};
        td.width = 1; td.height = 1; td.mipLevels = 1;
        td.format = TextureFormat::RGBA8;
        td.usage  = BufferUsage::Static;
        td.debugName = "VK_white_default";
        m_whiteTex = std::make_unique<VKTextureImpl>(&m_ctx, td);
        const uint8_t white[4] = {255, 255, 255, 255};
        m_whiteTex->Upload(white, sizeof(white), /*mipLevel=*/0);
        return true;
    }

    bool PrepareDraw() {
        if (!m_passActive) return false;
        if (!m_curPipeline || !m_curPipeline->Get()) return false;
        PerFrame& f = m_frames[m_currentFrame];
        VkDescriptorSet ds = AllocateAndWriteDescriptorSet();
        if (!ds) return false;
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_curPipeline->GetLayout(), 0, 1, &ds, 0, nullptr);
        if (m_curVB != VK_NULL_HANDLE) {
            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(f.cmd, 0, 1, &m_curVB, &off);
        }
        if (m_curIB != VK_NULL_HANDLE) {
            vkCmdBindIndexBuffer(f.cmd, m_curIB, 0, m_curIndexType);
        }
        return true;
    }

    VkDescriptorSet AllocateAndWriteDescriptorSet() {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool     = m_descPools[m_currentFrame];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &m_setLayout;
        VkDescriptorSet ds = VK_NULL_HANDLE;
        VkResult r = vkAllocateDescriptorSets(m_device.Get(), &ai, &ds);
        if (r != VK_SUCCESS) {
            Logger::Error("VulkanRenderer: vkAllocateDescriptorSets failed: {}",
                          VkResultToString(r));
            return VK_NULL_HANDLE;
        }

        // UBO at binding 0: prefer the most recently BindSlot(0)'d uniform buffer.
        VkBuffer     uboBuf  = g_boundUBOs[0].buffer;
        VkDeviceSize uboSize = g_boundUBOs[0].size;
        if (uboBuf == VK_NULL_HANDLE) {
            uboBuf  = m_scratchUBO[m_currentFrame].buffer;
            uboSize = kScratchUBOSize;
        }
        VkDescriptorBufferInfo bi{};
        bi.buffer = uboBuf;
        bi.offset = 0;
        bi.range  = uboSize ? uboSize : VK_WHOLE_SIZE;

        std::array<VkDescriptorImageInfo, kVKMaxTexturesPerDraw> imgs{};
        for (uint32_t i = 0; i < kVKMaxTexturesPerDraw; ++i) {
            VKTextureImpl* tx = m_curTextures[i] ? m_curTextures[i] : m_whiteTex.get();
            imgs[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imgs[i].imageView   = tx->GetView();
            imgs[i].sampler     = tx->GetSampler();
        }

        std::array<VkWriteDescriptorSet, 1 + kVKMaxTexturesPerDraw> writes{};
        writes[0] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet          = ds;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo     = &bi;
        for (uint32_t i = 0; i < kVKMaxTexturesPerDraw; ++i) {
            writes[1 + i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[1 + i].dstSet          = ds;
            writes[1 + i].dstBinding      = 1 + i;
            writes[1 + i].descriptorCount = 1;
            writes[1 + i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1 + i].pImageInfo      = &imgs[i];
        }
        vkUpdateDescriptorSets(m_device.Get(),
                               static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
        return ds;
    }

    // Begin a render pass on either the swapchain (rt==nullptr) or a custom
    // render target. Sets default full-extent dynamic viewport+scissor.
    void BeginPass(VKRenderTargetImpl* rt) {
        PerFrame& f = m_frames[m_currentFrame];
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        VkClearValue          clears[2]{};
        VkExtent2D            extent;

        if (rt && rt->GetRenderPass()) {
            extent.width  = rt->GetWidth();
            extent.height = rt->GetHeight();
            const float* cc = rt->GetClearColor();
            clears[0].color.float32[0] = cc[0];
            clears[0].color.float32[1] = cc[1];
            clears[0].color.float32[2] = cc[2];
            clears[0].color.float32[3] = cc[3];
            clears[1].depthStencil.depth   = rt->GetClearDepth();
            clears[1].depthStencil.stencil = rt->GetClearStencil();
            rpbi.renderPass  = rt->GetRenderPass();
            rpbi.framebuffer = rt->GetFramebuffer();
            // The custom RT pass uses LOAD_OP_LOAD so we don't actually clear
            // here unless the caller explicitly emits Clear* + a follow-up
            // ClearAttachments. (Future: respect a per-target "needsClear".)
            rpbi.clearValueCount = 0;
            rpbi.pClearValues    = nullptr;
            (void)clears;
        } else {
            extent = m_swapchain.GetExtent();
            clears[0].color.float32[0] = m_clearColor[0];
            clears[0].color.float32[1] = m_clearColor[1];
            clears[0].color.float32[2] = m_clearColor[2];
            clears[0].color.float32[3] = m_clearColor[3];
            rpbi.renderPass      = m_swapchain.GetRenderPass();
            rpbi.framebuffer     = m_swapchain.GetFramebuffer(m_currentImage);
            rpbi.clearValueCount = 1;
            rpbi.pClearValues    = clears;
        }
        rpbi.renderArea.offset = {0, 0};
        rpbi.renderArea.extent = extent;
        vkCmdBeginRenderPass(f.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        m_passActive = true;
        m_currentRT  = rt;

        // For custom RTs (LOAD_OP_LOAD), satisfy any pending Clear* calls via
        // vkCmdClearAttachments inside the active pass.
        if (rt) {
            auto pc = rt->ConsumePendingClear();
            std::array<VkClearAttachment, 2> ca{};
            uint32_t n = 0;
            if (pc.color) {
                ca[n].aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
                ca[n].colorAttachment = 0;
                const float* cc = rt->GetClearColor();
                ca[n].clearValue.color.float32[0] = cc[0];
                ca[n].clearValue.color.float32[1] = cc[1];
                ca[n].clearValue.color.float32[2] = cc[2];
                ca[n].clearValue.color.float32[3] = cc[3];
                ++n;
            }
            if (pc.depth) {
                ca[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                ca[n].clearValue.depthStencil.depth   = rt->GetClearDepth();
                ca[n].clearValue.depthStencil.stencil = rt->GetClearStencil();
                ++n;
            }
            if (n > 0) {
                VkClearRect cr{};
                cr.rect.offset = {0, 0};
                cr.rect.extent = extent;
                cr.baseArrayLayer = 0;
                cr.layerCount     = 1;
                vkCmdClearAttachments(f.cmd, n, ca.data(), 1, &cr);
            }
        }

        VkViewport vp{};
        vp.x = 0.0f; vp.y = 0.0f;
        vp.width  = static_cast<float>(extent.width);
        vp.height = static_cast<float>(extent.height);
        vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
        vkCmdSetViewport(f.cmd, 0, 1, &vp);
        VkRect2D sc{};
        sc.offset = {0, 0};
        sc.extent = extent;
        vkCmdSetScissor(f.cmd, 0, 1, &sc);
    }

    // members
    SDL_Window*   m_sdlWindow = nullptr;
    uint32_t      m_width  = 0;
    uint32_t      m_height = 0;

    VKInstance    m_instance;
    VkSurfaceKHR  m_surface = VK_NULL_HANDLE;
    VKDevice      m_device;
    VKSwapchain   m_swapchain;

    VkCommandPool                 m_cmdPool       = VK_NULL_HANDLE;
    VkCommandPool                 m_transientPool = VK_NULL_HANDLE;
    VKContext                     m_ctx{};
    std::array<PerFrame, kFramesInFlight> m_frames{};
    uint32_t      m_currentFrame = 0;
    uint32_t      m_currentImage = 0;
    bool          m_skipPresent  = false;
    bool          m_initialized  = false;
    bool          m_passActive   = false;

    // Stage 4: descriptor management + per-draw state cache
    static constexpr uint32_t     kMaxSetsPerFrame = 1024;
    static constexpr VkDeviceSize kScratchUBOSize  = 256;
    VkDescriptorSetLayout                            m_setLayout = VK_NULL_HANDLE;
    VkPipelineCache                                  m_pipelineCache = VK_NULL_HANDLE;
    std::array<VkDescriptorPool, kFramesInFlight>    m_descPools{};
    std::array<VKBufferStorage, kFramesInFlight>     m_scratchUBO{};
    std::unique_ptr<VKTextureImpl>                   m_whiteTex;

    VKPipelineImpl*                                  m_curPipeline = nullptr;
    VKShaderProgramImpl*                             m_curShader   = nullptr;
    VKRenderTargetImpl*                              m_currentRT   = nullptr;
    VKRenderTargetImpl*                              m_pendingRT   = nullptr;
    VkBuffer                                         m_curVB       = VK_NULL_HANDLE;
    VkBuffer                                         m_curIB       = VK_NULL_HANDLE;
    VkIndexType                                      m_curIndexType = VK_INDEX_TYPE_UINT16;
    VkBuffer                                         m_curUBO      = VK_NULL_HANDLE;
    VkDeviceSize                                     m_curUBOSize  = 0;
    std::array<VKTextureImpl*, kVKMaxTexturesPerDraw> m_curTextures{};

    float         m_clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    RendererCaps  m_caps{};
    RendererStats m_stats{};
};

} // anonymous

std::unique_ptr<IRenderer> CreateVulkanRenderer() {
    return std::make_unique<VulkanRenderer>();
}

}}} // namespace Nova64::Video::VK
