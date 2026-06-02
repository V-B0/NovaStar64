/**
 * @file VKBackend.cpp
 * @brief Vulkan IRenderer implementation â€” full geometry, textures and pipeline.
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
//   * All resource creation (CreateVertexBuffer, CreateTexture, â€¦) returns
//     valid no-op objects (callers won't crash on null deref).
//   * Draw / DrawArrays / DrawIndexed do nothing â€” no real geometry yet.
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
#include "VKRTPipeline.hpp"
#include "../../RT/RTSceneManager.hpp"
#include "../../Common/Logger.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Nova64 { namespace Video { namespace VK {

static bool ForensicEnabled() {
    static const bool enabled = []() {
#pragma warning(push)
#pragma warning(disable: 4996)
        const char* v = std::getenv("NOVA64_FORENSIC");
#pragma warning(pop)
        return v && v[0] != 0;
    }();
    return enabled;
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//  DynPipelineKey â€” shader + blend factors + depth state
//  Used to cache VkPipelines that differ only in blend/depth mode.
//  blendColor (the dynamic blend constant) is NOT included â€” it is set via
//  vkCmdSetBlendConstants every draw so it doesn't affect the pipeline.
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
struct DynPipelineKey {
    void*   shaderPtr       = nullptr;
    uint8_t blendEnabled    = 0;
    uint8_t srcColor        = 0; // BlendFactor enum value
    uint8_t dstColor        = 0;
    uint8_t colorOp         = 0; // BlendOp enum value
    uint8_t srcAlpha        = 0;
    uint8_t dstAlpha        = 0;
    uint8_t alphaOp         = 0;
    uint8_t colorWriteMask  = 0x0F;
    // depthTestEnable, depthWriteEnable, depthFunc are NOT part of the key:
    // they are set dynamically per-draw via vkCmdSetDepthTestEnable /
    // vkCmdSetDepthWriteEnable / vkCmdSetDepthCompareOp (VK 1.3 core).
    // Pipelines that differ only in depth state share one VkPipeline object;
    // depth state is emitted in PrepareDraw for every draw call.
    uint8_t _pad[8]         = {};

    bool operator==(const DynPipelineKey& o) const {
        return std::memcmp(this, &o, sizeof(*this)) == 0;
    }
};
struct DynPipelineKeyHash {
    size_t operator()(const DynPipelineKey& k) const {
        // FNV-1a over the raw bytes
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
        size_t h = 14695981039346656037ull;
        for (size_t i = 0; i < sizeof(k); ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return h;
    }
};

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//  IsVulkanAvailable â€” real probe via volk
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

bool IsVulkanAvailable() {
    static int cached = -1; // -1 unknown, 0 no, 1 yes
    if (cached != -1) return cached == 1;

    if (volkInitialize() != VK_SUCCESS) {
        cached = 0;
        return false;
    }
    if (!vkEnumerateInstanceVersion) {
        // Vulkan 1.0 loader only â€” we require 1.1+ for vkEnumerateInstanceVersion.
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

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//  Stub resources (Stage 2+ will replace these with real implementations)
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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
    VkSemaphore     midFrameDone   = VK_NULL_HANDLE;
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

    // â”€â”€â”€ Lifecycle â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

        // â”€â”€ instance extensions required by SDL â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

// Validation layers add significant CPU/GPU overhead that can trigger Intel
// TDR (Timeout Detection & Recovery) during shader compilation bursts on
// Iris Xe hardware.  Keep them off by default in all builds; re-enable
// manually when diagnosing Vulkan API errors.
        const bool wantValidation = false;
        if (!m_instance.Create(extNames, wantValidation)) {
            Logger::Error("VulkanRenderer: instance creation failed");
            return false;
        }

        // â”€â”€ surface (requires instance) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (!SDL_Vulkan_CreateSurface(m_sdlWindow, m_instance.Get(), &m_surface)) {
            Logger::Error("VulkanRenderer: SDL_Vulkan_CreateSurface failed: {}",
                          SDL_GetError());
            Cleanup();
            return false;
        }

        // â”€â”€ physical device â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        uint32_t gfxFam = 0, presFam = 0;
        if (!m_instance.PickPhysicalDevice(m_surface, gfxFam, presFam)) {
            Cleanup();
            return false;
        }

        // â”€â”€ logical device â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (!m_device.Create(m_instance.GetPhysical(), gfxFam, presFam)) {
            Cleanup();
            return false;
        }

        // â”€â”€ swapchain â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (!m_swapchain.Create(m_instance.GetPhysical(), m_device.Get(), m_surface,
                                gfxFam, presFam, m_width, m_height, /*vsync=*/false)) {
            Cleanup();
            return false;
        }

        // â”€â”€ per-frame command pool + sync â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (!CreateFrameResources()) {
            Cleanup();
            return false;
        }

        // â”€â”€ transient pool for one-shot uploads (Stage 2) â”€â”€â”€
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

        // Shared pipeline cache â€” persisted to ShaderCache/vk_pipeline_cache.bin
        // so that Intel Iris Xe (deferred GPU shader compilation) can reuse
        // pre-compiled pipelines on subsequent runs, avoiding TDR during the
        // first frame that exercises a new pipeline combination.
        {
            std::vector<uint8_t> cacheData;
            std::ifstream cacheFile("ShaderCache/vk_pipeline_cache.bin", std::ios::binary);
            if (cacheFile) {
                cacheFile.seekg(0, std::ios::end);
                auto sz = cacheFile.tellg();
                cacheFile.seekg(0, std::ios::beg);
                if (sz > 0) {
                    cacheData.resize(static_cast<size_t>(sz));
                    cacheFile.read(reinterpret_cast<char*>(cacheData.data()), sz);
                    // ifstream::read does not set failbit on partial binary reads;
                    // verify gcount() matches and discard a truncated payload so
                    // vkCreatePipelineCache builds an empty cache rather than load
                    // corrupt data.
                    if (cacheFile.gcount() != sz) {
                        Logger::Warn("VulkanRenderer: pipeline cache truncated ({} of {} bytes) - discarding",
                                     static_cast<size_t>(cacheFile.gcount()),
                                     static_cast<size_t>(sz));
                        cacheData.clear();
                    } else {
                        Logger::Info("VulkanRenderer: loaded pipeline cache ({} bytes)", static_cast<size_t>(sz));
                    }
                }
            }
            VkPipelineCacheCreateInfo pcci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
            if (!cacheData.empty()) {
                pcci.initialDataSize = cacheData.size();
                pcci.pInitialData    = cacheData.data();
            }
            if (vkCreatePipelineCache(m_device.Get(), &pcci, nullptr, &m_pipelineCache) != VK_SUCCESS) {
                Logger::Warn("VulkanRenderer: vkCreatePipelineCache failed (non-fatal)");
                m_pipelineCache = VK_NULL_HANDLE;
            }
        }
        m_ctx.pipelineCache = m_pipelineCache;

        // â”€â”€ standard descriptor set layout + per-frame pools (Stage 4) â”€
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
        m_uboAlignment = static_cast<uint32_t>(
            std::max(uint64_t(4), props.limits.minUniformBufferOffsetAlignment));

        // RT capabilities (populated after VKDevice::Create() which probes extensions)
        m_caps.supportsRayTracing = m_device.SupportsRayTracing();

        // Detect Intel GPU for workaround paths shared with GL backend.
        m_caps.isIntelGPU = (props.vendorID == 0x8086); // Intel PCI vendor ID

        Logger::Info("VulkanRenderer: initialized (full pipeline, textures, geometry)");
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
        if (m_ctx.deviceLost.load(std::memory_order_relaxed)) {
            m_skipPresent = true;
            return;
        }

        PerFrame& f = m_frames[m_currentFrame];
        // Use a finite timeout so that a GPU hang (TDR) doesn't lock the
        // emulator thread forever.  5 seconds is generous for any legitimate
        // frame, but short enough to detect a hung GPU quickly.
        auto fenceT0 = std::chrono::steady_clock::now();
        VkResult fw = vkWaitForFences(m_device.Get(), 1, &f.inFlightFence, VK_TRUE,
                                      5'000'000'000ULL); // 5 s in nanoseconds
        auto fenceMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - fenceT0).count());
        if (fenceMs > 50) { // log fence waits that indicate GPU pressure
            Logger::Warn("VKBackend: fence wait {}ms frame={} caps.isIntel={}",
                         fenceMs, m_currentFrame, m_caps.isIntelGPU);
        }
        if (fw == VK_TIMEOUT) {
            Logger::Warn("VulkanRenderer: vkWaitForFences timed out (5s) â€” GPU may be hung, skipping frame");
            m_skipPresent = true;
            return;
        }
        if (fw == VK_ERROR_DEVICE_LOST) {
            Logger::Warn("VulkanRenderer: vkWaitForFences: VK_ERROR_DEVICE_LOST â€” attempting swapchain recreation");
            RecreateSwapchain();
            m_skipPresent = true;
            return;
        }

        VkResult r = vkAcquireNextImageKHR(m_device.Get(), m_swapchain.Get(),
                                           UINT64_MAX, f.imageAvailable,
                                           VK_NULL_HANDLE, &m_currentImage);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            RecreateSwapchain();
            m_skipPresent = true;
            return;
        }
        if (r == VK_ERROR_DEVICE_LOST) {
            // On Intel Iris Xe the driver sometimes returns VK_ERROR_DEVICE_LOST
            // instead of VK_ERROR_OUT_OF_DATE_KHR when the swapchain surface is
            // temporarily invalid (e.g. during DWM updates on first present).
            // Attempt swapchain recreation before treating this as a fatal error.
            Logger::Warn("VulkanRenderer: vkAcquireNextImageKHR: VK_ERROR_DEVICE_LOST â€” attempting swapchain recreation");
            RecreateSwapchain();
            // Retry once with the new swapchain.
            r = vkAcquireNextImageKHR(m_device.Get(), m_swapchain.Get(),
                                      UINT64_MAX, f.imageAvailable,
                                      VK_NULL_HANDLE, &m_currentImage);
            if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
                Logger::Error("VulkanRenderer: vkAcquireNextImageKHR still failing after swapchain recreate: {} â€” device lost",
                              VkResultToString(r));
                m_ctx.deviceLost.store(true, std::memory_order_relaxed);
                m_skipPresent = true;
                return;
            }
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
        VkResult bcb = vkBeginCommandBuffer(f.cmd, &bi);
        if (bcb != VK_SUCCESS) {
            Logger::Error("VulkanRenderer: vkBeginCommandBuffer failed: {}",
                          VkResultToString(bcb));
            m_skipPresent = true;
            return;
        }
        m_skipPresent = false;
        m_midFrameReadbackDone = false;

        // Reset per-frame descriptor pool (frees all sets allocated last time
        // this frame index was active).
        if (m_descPools[m_currentFrame]) {
            vkResetDescriptorPool(m_device.Get(), m_descPools[m_currentFrame], 0);
        }

        // Reset per-frame draw state cache.
        m_curPipeline    = nullptr;
        m_curDynPipeline = nullptr;
        m_curShader      = nullptr;
        m_curVB         = VK_NULL_HANDLE;
        m_curIB         = VK_NULL_HANDLE;
        m_curIndexType  = VK_INDEX_TYPE_UINT16;
        m_curUBO        = VK_NULL_HANDLE;
        m_curUBOSize    = 0;
        for (auto& t : m_curTextures) t = nullptr;
        m_pipelineDirty = true; // force pipeline (re)bind at first draw
        m_uboRingOffset[m_currentFrame] = 0; // reset ring write position
        // Force depth state re-emission on the first draw of each frame.
        m_lastDepthTest  = -1;
        m_lastDepthWrite = -1;
        m_lastDepthFunc  = -1;

        // Reset per-frame GPU stats so each frame gets an accurate count.
        m_stats.Reset();

        // Begin the swapchain render pass (or pending custom RT) with
        // LOAD_OP_CLEAR using the latest m_clearColor. Draws happen between
        // Begin/EndFrame.
        BeginPass(m_pendingRT);
        m_pendingRT = nullptr;
    }

    void EndFrame() override {
        if (!m_initialized || m_skipPresent) return;
        if (m_ctx.deviceLost.load(std::memory_order_relaxed)) return;
        PerFrame& f = m_frames[m_currentFrame];

        if (m_passActive) {
            vkCmdEndRenderPass(f.cmd);
            m_passActive = false;
        }
        vkEndCommandBuffer(f.cmd);

        // VK draw call diagnostic: log per-frame GPU draw count (first 120 frames, then every 60).
        {
            static uint32_t s_vkFrame = 0;
            ++s_vkFrame;
            if (s_vkFrame <= 120 || (s_vkFrame % 60) == 0) {
                Logger::Info("VKEndFrame[{}]: drawCalls={} tris={} pipeBinds={}",
                             s_vkFrame, m_stats.drawCalls, m_stats.triangles, m_stats.pipelineBinds);
            }
        }

        // 4. Submit
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        VkSemaphore waitSem = m_midFrameReadbackDone ? f.midFrameDone : f.imageAvailable;
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &waitSem;
        si.pWaitDstStageMask    = &waitStage;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &f.cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &f.renderFinished;
        VkResult sr = vkQueueSubmit(m_device.GetGraphicsQueue(), 1, &si, f.inFlightFence);
        m_midFrameReadbackDone = false;
        if (sr != VK_SUCCESS) {
            Logger::Error("VulkanRenderer: vkQueueSubmit failed: {}", VkResultToString(sr));
            if (sr == VK_ERROR_DEVICE_LOST)
                m_ctx.deviceLost.store(true, std::memory_order_relaxed);
        }
    }

    void Present() override {
        if (!m_initialized || m_skipPresent) {
            m_currentFrame = (m_currentFrame + 1) % kFramesInFlight;
            return;
        }
        if (m_ctx.deviceLost.load(std::memory_order_relaxed)) {
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
        } else if (r == VK_ERROR_DEVICE_LOST) {
            Logger::Error("VulkanRenderer: vkQueuePresentKHR: VK_ERROR_DEVICE_LOST");
            m_ctx.deviceLost.store(true, std::memory_order_relaxed);
        } else if (r != VK_SUCCESS) {
            Logger::Error("VulkanRenderer: vkQueuePresentKHR failed: {}", VkResultToString(r));
        }
        m_currentFrame = (m_currentFrame + 1) % kFramesInFlight;
    }

    // â”€â”€â”€ Resource Creation (Stage 2: real buffers + textures) â”€
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
        } else if (sh && sh->VS() && sh->FS()) {
            // Shader already available â€” build immediately.
            if (!p->Build(m_ctx, d, m_swapchain.GetRenderPass(), m_setLayout)) {
                Logger::Error("VulkanRenderer: pipeline build failed");
            }
        } else {
            // No shader yet (GL-style deferred binding).  Store the desc so
            // BindShader can build the real VkPipeline on first use.
            m_deferredDesc        = d;
            m_deferredRenderPass  = m_swapchain.GetRenderPass();
            m_hasDeferredDesc     = true;
        }
        return p;
    }

    // â”€â”€â”€ State Management â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    void SetViewport(const RenderViewport& v) override {
        if (!m_passActive) return;
        VkExtent2D ext = m_swapchain.GetExtent();
        // Remember FBO size so SetScissor can undo the OpenGL Y-flip
        m_vpFBOW = (v.width  > 0.0f) ? v.width  : static_cast<float>(ext.width);
        m_vpFBOH = (v.height > 0.0f) ? v.height : static_cast<float>(ext.height);
        float scaleX = static_cast<float>(ext.width)  / m_vpFBOW;
        float scaleY = static_cast<float>(ext.height) / m_vpFBOH;
        // Map N64 FBO viewport to full swapchain with Vulkan Y-flip (negative height).
        // This fixes two bugs at once:
        //   1. Tiny rendering: game calls SetViewport(0,0,320,240) which we scale to swapchain.
        //   2. Upside-down: negative height makes NDC Y=+1 map to pixel row 0 (top), matching OpenGL.
        float scaledW = v.width  * scaleX;
        float scaledH = v.height * scaleY;
        VkViewport vp{};
        vp.x        = v.x * scaleX;
        vp.width    = scaledW;
        vp.height   = -scaledH;               // negative = Y-flip
        vp.y        = v.y * scaleY + scaledH; // bottom edge in Vulkan window coords
        vp.minDepth = v.minDepth;
        vp.maxDepth = v.maxDepth;
        vkCmdSetViewport(m_frames[m_currentFrame].cmd, 0, 1, &vp);
    }
    void SetScissor(const RenderScissorRect& r) override {
        if (!m_passActive) return;
        VkExtent2D ext = m_swapchain.GetExtent();
        // r uses OpenGL scissor convention: y=0 at bottom of FBO.
        // Vulkan scissor: y=0 at top of window.
        // Step 1: undo GL Y-flip â†’ get top edge in FBO coords
        float yTopFBO = m_vpFBOH - static_cast<float>(r.y + r.height);
        // Step 2: scale from FBO to swapchain
        float scaleX = static_cast<float>(ext.width)  / m_vpFBOW;
        float scaleY = static_cast<float>(ext.height) / m_vpFBOH;
        int32_t sx = static_cast<int32_t>(r.x    * scaleX);
        int32_t sy = static_cast<int32_t>(yTopFBO * scaleY);
        int32_t sw = r.width  > 0 ? static_cast<int32_t>(r.width  * scaleX) : 0;
        int32_t sh = r.height > 0 ? static_cast<int32_t>(r.height * scaleY) : 0;
        // Clamp to swapchain extent
        sx = std::max(0, sx);
        sy = std::max(0, sy);
        sw = std::max(0, std::min(sw, static_cast<int32_t>(ext.width)  - sx));
        sh = std::max(0, std::min(sh, static_cast<int32_t>(ext.height) - sy));
        VkRect2D sc{};
        sc.offset = { sx, sy };
        sc.extent = { static_cast<uint32_t>(sw), static_cast<uint32_t>(sh) };
        vkCmdSetScissor(m_frames[m_currentFrame].cmd, 0, 1, &sc);
    }
    void SetBlendState(const BlendState& s) override {
        m_pendingBlend = s;
        m_pipelineDirty = true;
        // Update dynamic blend constants immediately if a pass is active.
        if (m_passActive) {
            vkCmdSetBlendConstants(m_frames[m_currentFrame].cmd, s.blendColor);
        }
    }
    void SetDepthStencilState(const DepthStencilState& s) override {
        m_pendingDepth = s;
        m_pipelineDirty = true;
    }
    void SetRasterizerState(const RasterizerState& state) override {
        if (m_passActive) {
            vkCmdSetDepthBias(m_frames[m_currentFrame].cmd, state.depthBias, 0.0f, state.depthBiasSlope);
        }
    }
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

    // â”€â”€â”€ Binding â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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
        if (ForensicEnabled()) {
            auto* vktex = static_cast<VKTextureImpl*>(tex);
            Logger::Info("[FORENSIC] BIND unit={} view={}",
                         unit, static_cast<void*>(vktex ? vktex->GetView() : VK_NULL_HANDLE));
        }
    }
    void BindShader(IShaderProgram* shader) override {
        VKShaderProgramImpl* newShader = static_cast<VKShaderProgramImpl*>(shader);
        if (m_curShader != newShader) {
            m_curShader = newShader;
            m_pipelineDirty = true;
        }
    }
    void BindPipeline(IPipeline* pipeline) override {
        m_curPipeline    = static_cast<VKPipelineImpl*>(pipeline);
        m_curDynPipeline = nullptr;
        m_pipelineDirty  = true; // need to re-resolve dyn pipeline with current state
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

    // â”€â”€â”€ Image RW / draw / compute / readback (Stage 5+) â”€â”€â”€â”€â”€
    void BindImageForRW(uint32_t unit, uint64_t handle, ImageBindFormat fmt) override {
        // Store the binding; applied lazily when descriptor sets are written
        // for compute pipelines that use storage images.
        if (unit < kVKMaxImageBindings) {
            m_imageBindings[unit] = { reinterpret_cast<VkImageView>(handle), fmt };
        }
    }
    void UnbindAllImages() override {
        for (auto& b : m_imageBindings) b = {};
    }
    void SetColorWriteMask(bool r, bool g, bool b, bool a) override {
        // Stored and applied the next time a graphics pipeline is built.
        m_colorWriteMask =
            (r ? VK_COLOR_COMPONENT_R_BIT : 0u) |
            (g ? VK_COLOR_COMPONENT_G_BIT : 0u) |
            (b ? VK_COLOR_COMPONENT_B_BIT : 0u) |
            (a ? VK_COLOR_COMPONENT_A_BIT : 0u);
    }
    void ImageMemoryBarrier() override {
        if (!m_initialized) return;
        // Full execution + memory barrier â€” flushes all in-flight image writes
        // before subsequent reads (equivalent to glMemoryBarrier(GL_ALL_BARRIER_BITS)).
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        vkCmdPipelineBarrier(
            m_frames[m_currentFrame].cmd,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 1, &mb, 0, nullptr, 0, nullptr);
    }

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
        uint32_t dynOffset = 0;
        VkDescriptorSet ds = AllocateAndWriteDescriptorSet(dynOffset);
        if (ds) {
            vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    m_curPipeline->GetLayout(), 0, 1, &ds, 1, &dynOffset);
            vkCmdDispatch(f.cmd, gx, gy, gz);
        }
        if (wasActive) BeginPass(m_currentRT);
    }

    void DispatchRayTracing(void* tlas, void* outputImage,
                            uint32_t w, uint32_t h) override {
        if (!m_initialized || !m_device.SupportsRayTracing()) return;
        if (!tlas || !outputImage || w == 0 || h == 0) return;

        // Lazily build the RT pipeline on first call.
        if (!m_rtPipeline) {
            m_rtPipeline = std::make_unique<VKRTPipeline>();
            if (!m_rtPipeline->Build(m_ctx,
                    kRTRgenSrc, kRTRmissSrc, kRTRchitSrc)) {
                Logger::Error("VulkanRenderer: RT pipeline build failed â€” RT disabled");
                m_rtPipeline.reset();
                return;
            }
        }

        PerFrame& f = m_frames[m_currentFrame];
        // RT dispatch must run outside a render pass.
        bool wasActive = m_passActive;
        if (wasActive) { vkCmdEndRenderPass(f.cmd); m_passActive = false; }

        m_rtPipeline->Dispatch(f.cmd, tlas, outputImage, w, h);

        if (wasActive) BeginPass(m_currentRT);
    }
    void ExecuteRayTracing(RT::RTSceneManager* scene,
                           uint32_t w, uint32_t h) override {
        if (!m_initialized || !m_device.SupportsRayTracing()) return;
        if (!scene || scene->GetTriangleCount() == 0 || w == 0 || h == 0) return;

        PerFrame& f = m_frames[m_currentFrame];
        // Build BLAS + TLAS from this frame's accumulated triangles.
        // BuildAS records commands into the current command buffer; the geometry
        // is available to TraceRays in the same submission.
        if (!scene->BuildAS(m_ctx, f.cmd)) {
            Logger::Warn("VulkanRenderer::ExecuteRayTracing: BuildAS failed");
            return;
        }
        void* tlas = scene->GetTLAS();
        if (!tlas) return;

        // Use the current swapchain image view as the RT output surface.
        void* outputView = reinterpret_cast<void*>(
            m_swapchain.GetImageView(m_currentImage));
        DispatchRayTracing(tlas, outputView, w, h);
    }

    void ReadPixels(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    TextureFormat, void* outData) override {
        if (!m_initialized || !outData || w == 0 || h == 0) return;

        // End the render pass before operating on the swapchain image.
        PerFrame& f = m_frames[m_currentFrame];
        bool wasActive = m_passActive;
        if (wasActive) { vkCmdEndRenderPass(f.cmd); m_passActive = false; }

        VkImage srcImage = m_swapchain.GetImage(m_currentImage);
        const VkDeviceSize bytesPerPixel = 4; // RGBA8
        const VkDeviceSize stagingSize   = static_cast<VkDeviceSize>(w) * h * bytesPerPixel;

        // Create a host-visible staging buffer.
        VKBufferStorage staging{};
        if (!CreateBufferStorage(m_ctx, stagingSize,
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 staging)) {
            Logger::Error("VulkanRenderer::ReadPixels: staging buffer alloc failed");
            if (wasActive) BeginPass(m_currentRT);
            return;
        }

        // Transition swapchain image to TRANSFER_SRC.
        TransitionImage(f.cmd, srcImage,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkBufferImageCopy region{};
        region.bufferOffset      = 0;
        region.bufferRowLength   = 0; // tightly packed
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel       = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount     = 1;
        region.imageOffset = { static_cast<int32_t>(x), static_cast<int32_t>(y), 0 };
        region.imageExtent = { w, h, 1 };
        vkCmdCopyImageToBuffer(f.cmd, srcImage,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging.buffer, 1, &region);

        // Transition back to PRESENT_SRC.
        TransitionImage(f.cmd, srcImage,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_ACCESS_TRANSFER_READ_BIT,
                        VK_ACCESS_MEMORY_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

        
        vkEndCommandBuffer(f.cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &f.cmd;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        VkResult fr = vkCreateFence(m_device.Get(), &fci, nullptr, &fence);
        if (fr != VK_SUCCESS) {
            Logger::Error("VulkanRenderer::ReadPixels: vkCreateFence failed: {}",
                          VkResultToString(fr));
            DestroyBufferStorage(m_ctx, staging);
            m_skipPresent = true;
            return;
        }
        VkResult qr = vkQueueSubmit(m_device.GetGraphicsQueue(), 1, &si, fence);
        if (qr != VK_SUCCESS) {
            Logger::Error("VulkanRenderer::ReadPixels: vkQueueSubmit failed: {}",
                          VkResultToString(qr));
            vkDestroyFence(m_device.Get(), fence, nullptr);
            DestroyBufferStorage(m_ctx, staging);
            if (qr == VK_ERROR_DEVICE_LOST)
                m_ctx.deviceLost.store(true, std::memory_order_relaxed);
            m_skipPresent = true;
            return;
        }
        vkWaitForFences(m_device.Get(), 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(m_device.Get(), fence, nullptr);

        // Copy from staging to caller buffer.
        std::memcpy(outData, staging.mapped, static_cast<size_t>(stagingSize));
        DestroyBufferStorage(m_ctx, staging);

        // Restart the command buffer for the rest of the frame.
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(f.cmd, 0);
        VkResult rbb = vkBeginCommandBuffer(f.cmd, &bi);
        if (rbb != VK_SUCCESS) {
            Logger::Error("VulkanRenderer::ReadPixels: restart vkBeginCommandBuffer failed: {}",
                          VkResultToString(rbb));
            m_skipPresent = true;
            return;
        }
        m_skipPresent = true; 

        if (wasActive) BeginPass(m_currentRT);
    }

    // GPU-side blit: copy the current swapchain image into dst using vkCmdCopyImage
    // recorded directly into the in-flight command buffer.  No CPU readback, no
    // staging buffer, no synchronous submit, no m_skipPresent side-effect.
    void BlitToTexture(ITexture* dst) override {
        if (!dst || !m_initialized) return;
        PerFrame& f = m_frames[m_currentFrame];

        
        bool wasActive = m_passActive;
        if (wasActive) { vkCmdEndRenderPass(f.cmd); m_passActive = false; }

        VkImage  srcImage = m_swapchain.GetImage(m_currentImage);
        VkImage  dstImage = reinterpret_cast<VkImage>(dst->GetNativeHandle());
        if (!srcImage || !dstImage) { if (wasActive) BeginPass(m_currentRT); return; }
        uint32_t w = std::min(dst->GetWidth(),  m_swapchain.GetExtent().width);
        uint32_t h = std::min(dst->GetHeight(), m_swapchain.GetExtent().height);

        // Render-pass finalLayout leaves swapchain in PRESENT_SRC_KHR.
        TransitionImage(f.cmd, srcImage,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);

        // UNDEFINED as oldLayout = "discard previous contents" - valid per spec,
        TransitionImage(f.cmd, dstImage,
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        0,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);

        // Use vkCmdBlitImage rather than vkCmdCopyImage so the BGRA8 swapchain
        // can be sampled from an RGBA8 snapshot texture; Blit performs the
        // channel-order conversion automatically, whereas Copy requires the
        // two images to be format-compatible (Vulkan spec 11.6).
        VkImageBlit region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = region.srcSubresource;
        region.srcOffsets[0]  = { 0, 0, 0 };
        region.srcOffsets[1]  = { static_cast<int32_t>(w), static_cast<int32_t>(h), 1 };
        region.dstOffsets[0]  = { 0, 0, 0 };
        region.dstOffsets[1]  = { static_cast<int32_t>(w), static_cast<int32_t>(h), 1 };
        vkCmdBlitImage(f.cmd,
                       srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region, VK_FILTER_NEAREST);

        // Leave snapshot in SHADER_READ_ONLY so FlushBatch can bind it directly.
        TransitionImage(f.cmd, dstImage,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        // Restore swapchain to PRESENT_SRC_KHR; BeginPass uses initialLayout=UNDEFINED
        // so re-entry is safe from any actual layout.
        TransitionImage(f.cmd, srcImage,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_ACCESS_TRANSFER_READ_BIT,
                        0,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        // Resume the render pass - inventory draws continue in the same cmd buffer.
        // EndFrame will submit everything normally (m_skipPresent is NOT set).
        if (wasActive) BeginPass(m_currentRT);
    }

    // GPU-side snapshot without clearing: copy the swapchain into dst, then
    // restart the render pass with LOAD_OP_LOAD to preserve existing content.
    // Use this when a mid-frame SetColorImage switches away from a rendered FB
    // and we want to keep drawing on top of it afterwards.
    void SnapshotToTexture(ITexture* dst) override {
        if (!dst || !m_initialized) return;
        PerFrame& f = m_frames[m_currentFrame];

        bool wasActive = m_passActive;
        if (wasActive) { vkCmdEndRenderPass(f.cmd); m_passActive = false; }

        VkImage srcImage = m_swapchain.GetImage(m_currentImage);
        VkImage dstImage = reinterpret_cast<VkImage>(dst->GetNativeHandle());
        if (!srcImage || !dstImage) { if (wasActive) BeginPassLoad(); return; }
        uint32_t w = std::min(dst->GetWidth(),  m_swapchain.GetExtent().width);
        uint32_t h = std::min(dst->GetHeight(), m_swapchain.GetExtent().height);

        TransitionImage(f.cmd, srcImage,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);

        TransitionImage(f.cmd, dstImage,
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        0,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);

        // See BlitToTexture for rationale on vkCmdBlitImage vs vkCmdCopyImage.
        VkImageBlit region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = region.srcSubresource;
        region.srcOffsets[0]  = { 0, 0, 0 };
        region.srcOffsets[1]  = { static_cast<int32_t>(w), static_cast<int32_t>(h), 1 };
        region.dstOffsets[0]  = { 0, 0, 0 };
        region.dstOffsets[1]  = { static_cast<int32_t>(w), static_cast<int32_t>(h), 1 };
        vkCmdBlitImage(f.cmd,
                       srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region, VK_FILTER_NEAREST);

        TransitionImage(f.cmd, dstImage,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        // Restore swapchain to PRESENT_SRC_KHR -- required by the load render
        // pass (initialLayout = PRESENT_SRC_KHR).
        TransitionImage(f.cmd, srcImage,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_ACCESS_TRANSFER_READ_BIT,
                        0,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        // Resume rendering without clearing -- keeps previously drawn content.
        if (wasActive) BeginPassLoad();
    }

    void CopyFramebufferToRDRAM(uint32_t fbAddress, uint32_t fbW, uint32_t fbH,
                                bool isRGBA16, uint8_t* rdram) override {
        if (!m_initialized || !rdram || fbW == 0 || fbH == 0 || m_skipPresent ||
            m_midFrameReadbackDone) {
            return;
        }

        PerFrame& f = m_frames[m_currentFrame];
        bool wasActive = m_passActive;
        if (wasActive) {
            vkCmdEndRenderPass(f.cmd);
            m_passActive = false;
        }

        VkImage srcImage = m_swapchain.GetImage(m_currentImage);
        if (!srcImage) {
            if (wasActive) BeginPassLoad();
            return;
        }

        const VkExtent2D extent = m_swapchain.GetExtent();
        const VkDeviceSize stagingSize =
            static_cast<VkDeviceSize>(extent.width) * extent.height * 4;

        VKBufferStorage staging{};
        if (!CreateBufferStorage(m_ctx, stagingSize,
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 staging)) {
            Logger::Error("VulkanRenderer::CopyFramebufferToRDRAM: staging alloc failed");
            if (wasActive) BeginPassLoad();
            return;
        }

        TransitionImage(f.cmd, srcImage,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(f.cmd, srcImage,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging.buffer, 1, &region);

        TransitionImage(f.cmd, srcImage,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_ACCESS_TRANSFER_READ_BIT,
                        VK_ACCESS_MEMORY_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

        vkEndCommandBuffer(f.cmd);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.imageAvailable;
        si.pWaitDstStageMask = &waitStage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &f.midFrameDone;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;

        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence readbackFence = VK_NULL_HANDLE;
        if (vkCreateFence(m_device.Get(), &fci, nullptr, &readbackFence) != VK_SUCCESS) {
            Logger::Error("VulkanRenderer::CopyFramebufferToRDRAM: fence create failed");
            DestroyBufferStorage(m_ctx, staging);
            if (wasActive) {
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkResetCommandBuffer(f.cmd, 0);
                VkResult rb = vkBeginCommandBuffer(f.cmd, &bi);
                if (rb != VK_SUCCESS) {
                    Logger::Error("VulkanRenderer::CopyFramebufferToRDRAM: recovery vkBeginCommandBuffer failed: {}",
                                  VkResultToString(rb));
                    m_skipPresent = true;
                    return;
                }
                BeginPassLoad();
            }
            return;
        }

        VkResult sr = vkQueueSubmit(m_device.GetGraphicsQueue(), 1, &si, readbackFence);
        if (sr != VK_SUCCESS) {
            Logger::Error("VulkanRenderer::CopyFramebufferToRDRAM: submit failed: {}",
                          VkResultToString(sr));
            vkDestroyFence(m_device.Get(), readbackFence, nullptr);
            DestroyBufferStorage(m_ctx, staging);
            if (sr == VK_ERROR_DEVICE_LOST) {
                m_ctx.deviceLost.store(true, std::memory_order_relaxed);
            }
            return;
        }

        vkWaitForFences(m_device.Get(), 1, &readbackFence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(m_device.Get(), readbackFence, nullptr);

        const uint8_t* pixels = static_cast<const uint8_t*>(staging.mapped);
        const uint32_t pixelCount = fbW * fbH;
        uint32_t scaleX = (fbW > 0) ? (extent.width / fbW) : 1;
        uint32_t scaleY = (fbH > 0) ? (extent.height / fbH) : 1;
        if (scaleX == 0) scaleX = 1;
        if (scaleY == 0) scaleY = 1;

        if (isRGBA16) {
            uint16_t* dst = reinterpret_cast<uint16_t*>(rdram + fbAddress);
            for (uint32_t i = 0; i < pixelCount; ++i) {
                uint32_t fy = i / fbW;
                uint32_t fx = i % fbW;
                uint32_t sx = std::min(fx * scaleX, extent.width - 1);
                uint32_t sy = std::min(fy * scaleY, extent.height - 1);
                uint32_t off = (sy * extent.width + sx) * 4;
                uint8_t b = pixels[off + 0];
                uint8_t g = pixels[off + 1];
                uint8_t r = pixels[off + 2];
                uint8_t a = pixels[off + 3];
                uint16_t rgba16 = static_cast<uint16_t>(((r >> 3) << 11) |
                                                        ((g >> 3) << 6) |
                                                        ((b >> 3) << 1) |
                                                        (a >> 7));
                dst[i ^ 1] = rgba16;
            }
        } else {
            uint32_t* dst = reinterpret_cast<uint32_t*>(rdram + fbAddress);
            for (uint32_t i = 0; i < pixelCount; ++i) {
                uint32_t fy = i / fbW;
                uint32_t fx = i % fbW;
                uint32_t sx = std::min(fx * scaleX, extent.width - 1);
                uint32_t sy = std::min(fy * scaleY, extent.height - 1);
                uint32_t off = (sy * extent.width + sx) * 4;
                uint8_t b = pixels[off + 0];
                uint8_t g = pixels[off + 1];
                uint8_t r = pixels[off + 2];
                uint8_t a = pixels[off + 3];
                dst[i] = (static_cast<uint32_t>(r) << 24) |
                         (static_cast<uint32_t>(g) << 16) |
                         (static_cast<uint32_t>(b) << 8) |
                         static_cast<uint32_t>(a);
            }
        }

        DestroyBufferStorage(m_ctx, staging);

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(f.cmd, 0);
        VkResult rrb = vkBeginCommandBuffer(f.cmd, &bi);
        if (rrb != VK_SUCCESS) {
            Logger::Error("VulkanRenderer::CopyFramebufferToRDRAM: restart vkBeginCommandBuffer failed: {}",
                          VkResultToString(rrb));
            m_midFrameReadbackDone = false;
            m_skipPresent = true;
            return;
        }

        m_midFrameReadbackDone = true;

        if (wasActive) BeginPassLoad();
    }

    // â”€â”€â”€ Debug â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // VK_EXT_debug_utils is enabled with validation; functions are loaded by
    // volk (null when extension absent â€” guard).
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

    // â”€â”€â”€ Queries â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    BackendType GetBackendType() const override { return BackendType::Vulkan; }
    const RendererCaps& GetCapabilities() const override { return m_caps; }
    const RendererStats& GetStats() const override { return m_stats; }
    void ResetStats() override { m_stats.Reset(); }

    // â”€â”€â”€ Vulkan UBO helper â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    void* GetMappedUBOPtr() override {
        auto& s = m_scratchUBO[m_currentFrame];
        return s.mapped ? s.mapped : nullptr;
    }

    // â”€â”€â”€ Window â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    void OnResize(uint32_t width, uint32_t height) override {
        if (width == m_width && height == m_height) return;
        m_width = width; m_height = height;
        if (m_initialized) RecreateSwapchain();
    }
    uint32_t GetWidth()  const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }

    // Clear the depth buffer mid-frame via vkCmdClearAttachments.
    // Called by RDPProcessor when the N64 game issues a FillRect targeting
    // the Z-buffer address, signalling an explicit depth clear. Keeping the
    // depth buffer clean between the 3D scene and the HUD prevents 3D HUD
    // geometry from failing depth tests against stale scene depth values.
    void ClearDepthBuffer(float depth) override {
        if (!m_passActive) return;
        VkClearAttachment ca{};
        ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        ca.clearValue.depthStencil = {depth, 0};
        VkExtent2D ext = m_swapchain.GetExtent();
        VkClearRect cr{};
        cr.rect           = {{0, 0}, {ext.width, ext.height}};
        cr.baseArrayLayer = 0;
        cr.layerCount     = 1;
        vkCmdClearAttachments(m_frames[m_currentFrame].cmd, 1, &ca, 1, &cr);
    }

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
            VK_CHECK(vkCreateSemaphore(m_device.Get(), &sci, nullptr, &m_frames[i].midFrameDone));
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
        m_ctx.Reset();
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
            if (m_frames[i].midFrameDone) {
                vkDestroySemaphore(dev, m_frames[i].midFrameDone, nullptr);
                m_frames[i].midFrameDone = VK_NULL_HANDLE;
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
            m_rtPipeline.reset(); // destroy RT pipeline before device
            // Destroy dynamically-built per-shader pipelines
            m_dynPipelines.clear();
            m_curDynPipeline = nullptr;
            m_whiteTex.reset();
            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                if (m_descPools[i]) {
                    vkDestroyDescriptorPool(m_device.Get(), m_descPools[i], nullptr);
                    m_descPools[i] = VK_NULL_HANDLE;
                }
                if (m_scratchUBO[i].buffer) {
                    DestroyBufferStorage(m_ctx, m_scratchUBO[i]);
                }
                if (m_uboRing[i].buffer) {
                    DestroyBufferStorage(m_ctx, m_uboRing[i]);
                }
            }
            if (m_setLayout) {
                vkDestroyDescriptorSetLayout(m_device.Get(), m_setLayout, nullptr);
                m_setLayout = VK_NULL_HANDLE;
            }
            if (m_pipelineCache) {
                // Persist the compiled pipeline cache so subsequent runs avoid
                // re-compiling shaders on Intel Iris Xe (deferred GPU compilation
                // on first use causes TDR otherwise).
                size_t cacheSize = 0;
                if (vkGetPipelineCacheData(m_device.Get(), m_pipelineCache, &cacheSize, nullptr) == VK_SUCCESS && cacheSize > 0) {
                    std::vector<uint8_t> cacheData(cacheSize);
                    if (vkGetPipelineCacheData(m_device.Get(), m_pipelineCache, &cacheSize, cacheData.data()) == VK_SUCCESS) {
                        std::ofstream f("ShaderCache/vk_pipeline_cache.bin", std::ios::binary | std::ios::trunc);
                        if (f) {
                            f.write(reinterpret_cast<const char*>(cacheData.data()), static_cast<std::streamsize>(cacheSize));
                            Logger::Info("VulkanRenderer: saved pipeline cache ({} bytes)", cacheSize);
                        }
                    }
                }
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
        // Drain all in-flight frames before destroying the old swapchain.
        // Without this, in-flight command buffers can still reference the old
        // swapchain images / VkRenderPass after Recreate() destroys them, which
        // manifests as crashes or validation errors on scene<->menu transitions
        // (where SetColorImage format changes can trigger a recreate mid-stream).
        vkDeviceWaitIdle(m_device.Get());
        m_swapchain.Recreate(m_instance.GetPhysical(), m_device.Get(), m_surface,
                             m_device.GetGraphicsFamily(), m_device.GetPresentFamily(),
                             m_width, m_height, /*vsync=*/true);
        // The swapchain recreated a new VkRenderPass handle.  Update
        // m_deferredRenderPass so new dynamic pipelines are built against
        // the live render pass rather than the now-destroyed old handle.
        // Also flush m_dynPipelines so any in-flight pipelines referencing
        // the old render pass are discarded (they remain Vulkan-compatible
        // due to render-pass compatibility rules, but rebuilding is cleaner).
        if (m_hasDeferredDesc) {
            m_deferredRenderPass = m_swapchain.GetRenderPass();
            // Do NOT clear m_dynPipelines here: once a VkPipeline is created the
            // VkRenderPass handle it was built against no longer needs to live — the
            // compiled pipeline object is self-contained.  New and old render passes
            // are render-pass-compatible (same attachment formats / sample counts),
            // so all existing pipelines continue to work after swapchain recreation.
            // Clearing them forces every pipeline to be re-created and re-ISA-compiled
            // on first use, which can trigger multiple simultaneous GPU ISA compiles
            // (non-preemptable on Intel Iris Xe) and cause a Windows TDR crash.
        }
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

    // â”€â”€ Stage 4: descriptor pools, default UBO + white texture, PrepareDraw â”€â”€
    bool CreateDescriptorPools() {
        std::array<VkDescriptorPoolSize, 2> sizes{};
        sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        sizes[0].descriptorCount = kMaxDescSetsPerFrame;
        sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = kMaxDescSetsPerFrame * kVKMaxTexturesPerDraw;

        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets       = kMaxDescSetsPerFrame;
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

        // Per-frame ring UBO: kMaxSetsPerFrame slots of alignedSlotSize bytes.
        // Each draw call gets a unique slot so the GPU reads its own uniforms.
        {
            uint32_t alignedSlot = (static_cast<uint32_t>(kScratchUBOSize) + m_uboAlignment - 1)
                                    & ~(m_uboAlignment - 1);
            uint32_t ringSize = kMaxSetsPerFrame * alignedSlot;
            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                if (!CreateBufferStorage(m_ctx, ringSize,
                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                         m_uboRing[i])) {
                    Logger::Error("VulkanRenderer: UBO ring alloc failed");
                    return false;
                }
                std::memset(m_uboRing[i].mapped, 0, ringSize);
                m_uboRingOffset[i] = 0;
            }
        }

        // 1Ă—1 white default texture (used for any unbound texture slot so the
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
        if (!m_curShader || !m_hasDeferredDesc) return false;
        if (!m_curShader->VS() || !m_curShader->FS()) return false;

        // Rebuild/find pipeline if shader or blend/depth state changed.
        if (m_pipelineDirty) {
            DynPipelineKey key{};
            key.shaderPtr       = m_curShader;
            key.blendEnabled    = m_pendingBlend.enabled ? 1u : 0u;
            key.srcColor        = static_cast<uint8_t>(m_pendingBlend.srcColor);
            key.dstColor        = static_cast<uint8_t>(m_pendingBlend.dstColor);
            key.colorOp         = static_cast<uint8_t>(m_pendingBlend.colorOp);
            key.srcAlpha        = static_cast<uint8_t>(m_pendingBlend.srcAlpha);
            key.dstAlpha        = static_cast<uint8_t>(m_pendingBlend.dstAlpha);
            key.alphaOp         = static_cast<uint8_t>(m_pendingBlend.alphaOp);
            key.colorWriteMask  = static_cast<uint8_t>(m_colorWriteMask & 0x0F);
            // depth test/write/compare are NOT part of the key; they are
            // emitted dynamically per draw via vkCmdSetDepthTestEnable etc.

            auto it = m_dynPipelines.find(key);
            if (it == m_dynPipelines.end()) {
                PipelineDesc d  = m_deferredDesc;
                d.shader        = m_curShader;
                d.blend         = m_pendingBlend;
                d.depthStencil  = m_pendingDepth;
                // Apply per-channel write mask from last SetColorWriteMask call.
                d.blend.colorWriteMask = static_cast<uint8_t>(m_colorWriteMask & 0x0F);

                auto dp = std::make_unique<VKPipelineImpl>();
                if (!dp->Build(m_ctx, d, m_deferredRenderPass, m_setLayout)) {
                    Logger::Warn("VulkanRenderer: dynamic pipeline build failed "
                                 "shader={:p} blendEn={} src={} dst={}",
                                 static_cast<void*>(m_curShader),
                                 key.blendEnabled, key.srcColor, key.dstColor);
                    return false;
                }
                it = m_dynPipelines.emplace(key, std::move(dp)).first;
                // Persist the pipeline cache to disk immediately after each new
                // pipeline is compiled.  Doing this eagerly (not just at Cleanup)
                // ensures the cache is available even if the GPU TDRs later in the
                // same session, so subsequent runs reuse the compiled shaders.
                if (m_pipelineCache) {
                    size_t cacheSize = 0;
                    if (vkGetPipelineCacheData(m_device.Get(), m_pipelineCache, &cacheSize, nullptr) == VK_SUCCESS && cacheSize > 0) {
                        std::vector<uint8_t> cacheData(cacheSize);
                        if (vkGetPipelineCacheData(m_device.Get(), m_pipelineCache, &cacheSize, cacheData.data()) == VK_SUCCESS) {
                            // Ensure ShaderCache/ exists - silent ofstream open failure
                            // would otherwise mean the cache never persists across runs.
                            std::error_code ec;
                            std::filesystem::create_directories("ShaderCache", ec);
                            std::ofstream f("ShaderCache/vk_pipeline_cache.bin", std::ios::binary | std::ios::trunc);
                            if (f) {
                                f.write(reinterpret_cast<const char*>(cacheData.data()),
                                        static_cast<std::streamsize>(cacheSize));
                                if (!f.good()) {
                                    static bool s_warnedWrite = false;
                                    if (!s_warnedWrite) {
                                        Logger::Warn("VulkanRenderer: pipeline cache write failed (ofstream bad)");
                                        s_warnedWrite = true;
                                    }
                                }
                            } else {
                                static bool s_warnedOpen = false;
                                if (!s_warnedOpen) {
                                    Logger::Warn("VulkanRenderer: pipeline cache open-for-write failed");
                                    s_warnedOpen = true;
                                }
                            }
                        }
                    }
                }
            }
            m_curDynPipeline = it->second.get();
            if (m_curDynPipeline->Get()) {
                vkCmdBindPipeline(m_frames[m_currentFrame].cmd,
                                  VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  m_curDynPipeline->Get());
                ++m_stats.pipelineBinds;
            }
            m_pipelineDirty = false;
        }

        VKPipelineImpl* effectivePipeline = m_curDynPipeline;
        if (!effectivePipeline || !effectivePipeline->Get()) {
            static int s_nullPipeWarn = 0;
            if (s_nullPipeWarn < 5) {
                Logger::Warn("PrepareDraw: null effective pipeline (shader={:p} dirty={}) â€” draw skipped",
                             static_cast<void*>(m_curShader), m_pipelineDirty);
                ++s_nullPipeWarn;
            }
            return false;
        }

        PerFrame& f = m_frames[m_currentFrame];
        uint32_t dynOffset = 0;
        VkDescriptorSet ds = AllocateAndWriteDescriptorSet(dynOffset);
        if (!ds) {
            return false;
        }
        // Emit dynamic depth state only when it changes — avoids expensive
        // per-draw pipeline state flushes on Intel Iris Xe.
        const bool depthTestOn  = m_pendingDepth.depthTestEnabled;
        const bool depthWriteOn = m_pendingDepth.depthWriteEnabled;
        const int  depthFuncI   = static_cast<int>(m_pendingDepth.depthFunc);
        const int  depthTestI   = depthTestOn  ? 1 : 0;
        const int  depthWriteI  = depthWriteOn ? 1 : 0;
        if (vkCmdSetDepthTestEnableEXT && vkCmdSetDepthWriteEnableEXT && vkCmdSetDepthCompareOpEXT &&
            (depthTestI != m_lastDepthTest || depthWriteI != m_lastDepthWrite || depthFuncI != m_lastDepthFunc)) {
            vkCmdSetDepthTestEnableEXT (f.cmd, depthTestOn  ? VK_TRUE : VK_FALSE);
            vkCmdSetDepthWriteEnableEXT(f.cmd, depthWriteOn ? VK_TRUE : VK_FALSE);
            auto toVkCmp = [](CompareFunc cf) -> VkCompareOp {
                switch (cf) {
                    case CompareFunc::Never:        return VK_COMPARE_OP_NEVER;
                    case CompareFunc::Less:         return VK_COMPARE_OP_LESS;
                    case CompareFunc::Equal:        return VK_COMPARE_OP_EQUAL;
                    case CompareFunc::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
                    case CompareFunc::Greater:      return VK_COMPARE_OP_GREATER;
                    case CompareFunc::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
                    case CompareFunc::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
                    case CompareFunc::Always:       return VK_COMPARE_OP_ALWAYS;
                    default:                        return VK_COMPARE_OP_ALWAYS;
                }
            };
            vkCmdSetDepthCompareOpEXT(f.cmd, toVkCmp(m_pendingDepth.depthFunc));
            m_lastDepthTest  = depthTestI;
            m_lastDepthWrite = depthWriteI;
            m_lastDepthFunc  = depthFuncI;
        }
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                effectivePipeline->GetLayout(), 0, 1, &ds, 1, &dynOffset);
        if (m_curVB != VK_NULL_HANDLE) {
            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(f.cmd, 0, 1, &m_curVB, &off);
        }
        if (m_curIB != VK_NULL_HANDLE) {
            vkCmdBindIndexBuffer(f.cmd, m_curIB, 0, m_curIndexType);
        }
        return true;
    }

    VkDescriptorSet AllocateAndWriteDescriptorSet(uint32_t& outDynamicOffset) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool     = m_descPools[m_currentFrame];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &m_setLayout;
        VkDescriptorSet ds = VK_NULL_HANDLE;
        VkResult r = vkAllocateDescriptorSets(m_device.Get(), &ai, &ds);
        if (r != VK_SUCCESS) {
            if (r == VK_ERROR_OUT_OF_POOL_MEMORY) {
                // Pool exhausted for this frame â€” degrade gracefully (skip draw).
                // Increase kMaxDescSetsPerFrame if this fires frequently.
                static uint32_t s_poolExhaustedCount = 0;
                if ((++s_poolExhaustedCount & 0x3FF) == 1) { // log 1st, then every 1024th
                    Logger::Warn("VulkanRenderer: descriptor pool full ({} so far this session) â€” skipping draw",
                                 s_poolExhaustedCount);
                }
            } else {
                Logger::Error("VulkanRenderer: vkAllocateDescriptorSets failed: {}",
                              VkResultToString(r));
            }
            return VK_NULL_HANDLE;
        }

        // Copy current scratch UBO data into a unique ring slot so the GPU
        // reads this draw's uniforms even after the CPU advances the scratch.
        uint32_t alignedSlot = (static_cast<uint32_t>(kScratchUBOSize) + m_uboAlignment - 1)
                                & ~(m_uboAlignment - 1);
        uint32_t ringSize = kMaxSetsPerFrame * alignedSlot;
        uint32_t dynOffset = m_uboRingOffset[m_currentFrame];
        if (dynOffset + alignedSlot > ringSize) {
            // Ring and pool should exhaust simultaneously (both = kMaxSetsPerFrame).
            // If we somehow reach here, skip the draw rather than wrapping and
            // corrupting UBO data for in-flight descriptor sets.
            static uint32_t s_ringOverflowCount = 0;
            if ((++s_ringOverflowCount & 0xFF) == 1) {
                Logger::Warn("VulkanRenderer: UBO ring overflow ({} so far) â€” skipping draw to avoid corruption",
                             s_ringOverflowCount);
            }
            return VK_NULL_HANDLE;
        }
        if (m_scratchUBO[m_currentFrame].mapped && m_uboRing[m_currentFrame].mapped) {
            std::memcpy(static_cast<uint8_t*>(m_uboRing[m_currentFrame].mapped) + dynOffset,
                        m_scratchUBO[m_currentFrame].mapped,
                        kScratchUBOSize);
        }
        m_uboRingOffset[m_currentFrame] = dynOffset + alignedSlot;
        outDynamicOffset = dynOffset;

        // Binding 0: ring buffer at base offset 0; dynamic offset applied at bind time.
        VkDescriptorBufferInfo bi{};
        bi.buffer = m_uboRing[m_currentFrame].buffer;
        bi.offset = 0;
        bi.range  = kScratchUBOSize;

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
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
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
    // Restart the swapchain render pass with LOAD_OP_LOAD (preserve content).
    // Called by SnapshotToTexture after the GPU copy.
    void BeginPassLoad() {
        PerFrame& f = m_frames[m_currentFrame];
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass      = m_swapchain.GetRenderPassLoad();
        rpbi.framebuffer     = m_swapchain.GetFramebuffer(m_currentImage);
        rpbi.renderArea      = { {0,0}, m_swapchain.GetExtent() };
        rpbi.clearValueCount = 0;
        rpbi.pClearValues    = nullptr;
        vkCmdBeginRenderPass(f.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        m_passActive = true;
        m_currentRT  = nullptr;
    }

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
            clears[1].depthStencil.depth   = 1.0f;
            clears[1].depthStencil.stencil = 0u;
            rpbi.renderPass      = m_swapchain.GetRenderPass();
            rpbi.framebuffer     = m_swapchain.GetFramebuffer(m_currentImage);
            rpbi.clearValueCount = 2;
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

        // Default Y-flipped full-swapchain viewport. Game code calls SetViewport
        // shortly after which applies the FBOâ†’swapchain scale too.
        float vpW = static_cast<float>(extent.width);
        float vpH = static_cast<float>(extent.height);
        m_vpFBOW = vpW;  // initialize FBO scale; SetViewport will update these
        m_vpFBOH = vpH;
        VkViewport vp{};
        vp.x        = 0.0f;
        vp.width    = vpW;
        vp.height   = -vpH;  // negative = Y-flip: NDC Y=+1 maps to pixel row 0 (top)
        vp.y        = vpH;   // bottom edge of viewport in Vulkan window coords
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
    bool          m_midFrameReadbackDone = false;
    bool          m_initialized  = false;
    bool          m_passActive   = false;

    // Stage 4: descriptor management + per-draw state cache
    // kMaxSetsPerFrame MUST equal kMaxDescSetsPerFrame.
    // The UBO ring holds kMaxSetsPerFrame * alignedSlot bytes.  If the ring
    // limit is smaller than the pool limit, the ring wraps mid-frame and
    // overwrites UBO slots still referenced by in-flight descriptor sets â€”
    // causing data corruption and a visually frozen frame.  Keeping them equal
    // ensures the pool exhausts at the same draw as the ring would wrap, so
    // excess draws are simply skipped (no corruption).
    static constexpr uint32_t     kMaxSetsPerFrame     = 262144; // UBO ring slots â€” must equal kMaxDescSetsPerFrame
    static constexpr uint32_t     kMaxDescSetsPerFrame = 262144; // descriptor pool capacity per frame
    static constexpr VkDeviceSize kScratchUBOSize  = 624; // exact N64 UBO size (aligned to 768 at runtime)
    VkDescriptorSetLayout                            m_setLayout = VK_NULL_HANDLE;
    VkPipelineCache                                  m_pipelineCache = VK_NULL_HANDLE;
    std::array<VkDescriptorPool, kFramesInFlight>    m_descPools{};
    std::array<VKBufferStorage, kFramesInFlight>     m_scratchUBO{};
    std::array<VKBufferStorage, kFramesInFlight>     m_uboRing{};          // per-frame ring UBO
    std::array<uint32_t, kFramesInFlight>            m_uboRingOffset{};    // next free byte in ring
    uint32_t                                          m_uboAlignment = 256; // device minUniformBufferOffsetAlignment
    std::unique_ptr<VKTextureImpl>                   m_whiteTex;

    VKPipelineImpl*                                  m_curPipeline    = nullptr;
    VKPipelineImpl*                                  m_curDynPipeline = nullptr; // resolved dyn pipeline
    VKShaderProgramImpl*                             m_curShader      = nullptr;
    VKRenderTargetImpl*                              m_currentRT   = nullptr;
    // Cached last-emitted dynamic depth state so we only call vkCmdSetDepth*
    // when state actually changes.  Initialized to -1 to force emission on first draw.
    int                                              m_lastDepthTest  = -1;
    int                                              m_lastDepthWrite = -1;
    int                                              m_lastDepthFunc  = -1;
    VKRenderTargetImpl*                              m_pendingRT   = nullptr;
    VkBuffer                                         m_curVB       = VK_NULL_HANDLE;
    VkBuffer                                         m_curIB       = VK_NULL_HANDLE;
    VkIndexType                                      m_curIndexType = VK_INDEX_TYPE_UINT16;
    VkBuffer                                         m_curUBO      = VK_NULL_HANDLE;
    VkDeviceSize                                     m_curUBOSize  = 0;
    std::array<VKTextureImpl*, kVKMaxTexturesPerDraw> m_curTextures{};

    float         m_clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    // FBO dimensions from the last SetViewport call; used by SetScissor to
    // undo the OpenGL-style Y-flip that RDPProcessor bakes into scissor.y.
    float         m_vpFBOW = 320.0f;
    float         m_vpFBOH = 240.0f;

    // Image RW bindings (used by MLE blender / compute passes).
    struct ImageBinding { VkImageView view = VK_NULL_HANDLE; ImageBindFormat fmt = {}; };
    static constexpr uint32_t kVKMaxImageBindings = 8;
    std::array<ImageBinding, kVKMaxImageBindings> m_imageBindings{};

    // Per-channel color write mask for the next pipeline build (default: all channels).
    VkColorComponentFlags m_colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    // Lazy shader pipeline cache: pipelines are keyed by (shader, blend, depth)
    // so that each unique draw-state combination gets its own VkPipeline.
    bool                 m_hasDeferredDesc    = false;
    PipelineDesc         m_deferredDesc;
    VkRenderPass         m_deferredRenderPass = VK_NULL_HANDLE;
    std::unordered_map<DynPipelineKey, std::unique_ptr<VKPipelineImpl>, DynPipelineKeyHash> m_dynPipelines;

    // Pending draw state â€” set by SetBlendState / SetDepthStencilState.
    // Incorporated into the pipeline key in PrepareDraw().
    BlendState           m_pendingBlend{};
    DepthStencilState    m_pendingDepth{};
    bool                 m_pipelineDirty = false; // true when shader/blend/depth changed

    RendererCaps  m_caps{};
    RendererStats m_stats{};

    // â”€â”€ RT (optional, activated when device supports RT extensions) â”€â”€â”€â”€â”€â”€
    std::unique_ptr<VKRTPipeline> m_rtPipeline;

    static constexpr const char* kRTRgenSrc = R"GLSL(
// n64rt.rgen
#version 460
#extension GL_EXT_ray_tracing : require

layout(set = 0, binding = 0) uniform accelerationStructureEXT u_tlas;
layout(set = 0, binding = 1, rgba8) uniform image2D u_output;

struct RayPayload { vec4 color; bool hit; };
layout(location = 0) rayPayloadEXT RayPayload g_payload;

void main() {
    const ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
    const ivec2 size  = ivec2(gl_LaunchSizeEXT.xy);
    const vec2 uv  = (vec2(pixel) + 0.5) / vec2(size);
    const vec2 ndc = uv * 2.0 - 1.0;
    const vec3 origin    = vec3(ndc.x, -ndc.y, 1.0);
    const vec3 direction = vec3(0.0, 0.0, -1.0);
    g_payload.color = vec4(0.0, 0.0, 0.0, 1.0);
    g_payload.hit   = false;
    traceRayEXT(u_tlas, gl_RayFlagsOpaqueEXT, 0xFF, 0, 0, 0,
                origin, 0.001, direction, 1000.0, 0);
    imageStore(u_output, pixel, g_payload.color);
}
)GLSL";

    static constexpr const char* kRTRmissSrc = R"GLSL(
// n64rt.rmiss
#version 460
#extension GL_EXT_ray_tracing : require

struct RayPayload { vec4 color; bool hit; };
layout(location = 0) rayPayloadInEXT RayPayload g_payload;

void main() {
    g_payload.color = vec4(0.05, 0.05, 0.1, 1.0);
    g_payload.hit   = false;
}
)GLSL";

    static constexpr const char* kRTRchitSrc = R"GLSL(
// n64rt.rchit
#version 460
#extension GL_EXT_ray_tracing          : require
#extension GL_EXT_nonuniform_qualifier : require

struct RayPayload { vec4 color; bool hit; };
layout(location = 0) rayPayloadInEXT RayPayload g_payload;
hitAttributeEXT vec2 g_barycentrics;

void main() {
    const vec3 bary = vec3(1.0 - g_barycentrics.x - g_barycentrics.y,
                           g_barycentrics.x, g_barycentrics.y);
    const float fogFactor = clamp(1.0 - gl_HitTEXT * 0.01, 0.0, 1.0);
    g_payload.color = vec4(bary * fogFactor, 1.0);
    g_payload.hit   = true;
}
)GLSL";
};

} // anonymous

std::unique_ptr<IRenderer> CreateVulkanRenderer() {
    return std::make_unique<VulkanRenderer>();
}

}}} // namespace Nova64::Video::VK
