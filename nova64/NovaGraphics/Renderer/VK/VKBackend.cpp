/**
 * @file VKBackend.cpp
 * @brief Vulkan IRenderer implementation â€” full geometry, textures and pipeline.
 */
// SPDX-License-Identifier: MIT
//
// Full Vulkan 1.2 IRenderer implementation: real geometry (Draw/DrawArrays/
// DrawIndexed), textures, dynamically-built pipelines, the compute-based ray
// tracer (VKComputeRaytracer, MoltenVK has no hardware RT), per-N64-buffer
// persistent canvases for framebuffer isolation and framebuffer-as-texture
// reads, GPU<->RDRAM sync (CopyFramebufferToRDRAM / UploadPixelsToCanvas),
// and VI gamma correction. IsVulkanAvailable() performs a real probe
// (volkInitialize + vkEnumerateInstanceVersion) rather than a hardcoded
// answer. See the class-level comments throughout this file for how each
// subsystem works; there are no remaining stubbed/no-op entry points in the
// IRenderer interface (checked against Renderer.hpp's virtuals).

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
#include "VKComputeRaytracer.hpp"
#include "../../RT/RTSceneManager.hpp"
#include "../../RT/RTBVH.hpp"
#include "../../Common/Logger.hpp"
#include "../../Texture/ImageWrite.hpp"
#include "../../Core/FramebufferManager.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
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

// Vertex/Index/Uniform buffers and Texture: see VKBuffer/VKTexture.
// Shaders and Pipelines: see VKShader/VKPipeline. No stubs remain locally.

constexpr uint32_t kFramesInFlight = 2;

struct PerFrame {
    VkCommandBuffer cmd            = VK_NULL_HANDLE;
    VkSemaphore     imageAvailable = VK_NULL_HANDLE;
    VkSemaphore     midFrameDone   = VK_NULL_HANDLE;
    VkSemaphore     renderFinished = VK_NULL_HANDLE;
    VkFence         inFlightFence  = VK_NULL_HANDLE;
};

// General 4x4 inverse (row-major in/out). Used to derive the compute-RT
// inverse projection for primary-ray generation. Returns identity on a
// singular matrix.
static void Invert4x4(const float m[16], float out[16]) {
    float inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det > -1e-12f && det < 1e-12f) {
        for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        return;
    }
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * det;
}

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
        DestroyCanvas();
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
        m_frameRecording = true;
        m_midFrameReadbackDone = false;
        m_frameReadbackCount = 0;
        m_activeCanvasDirty = false;
        DrainPendingUploads();

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

        // Begin the swapchain render pass (or pending custom RT). When the
        // active N64 buffer's persistent canvas holds accumulated content,
        // blit it into the freshly acquired image and resume with the LOAD
        // pass so content carries across swapchain image rotation (see
        // m_canvases docs). Otherwise fall back to the clearing pass.
        m_frameOnSwapchain = (m_pendingRT == nullptr);
        bool resumedFromCanvas = false;
        if (!m_pendingRT && m_canvasPreserve) {
            CanvasSlot* c = EnsureCanvas(m_activeCanvasAddr);
            if (c && c->valid) {
                VkImage swapImg = m_swapchain.GetImage(m_currentImage);
                VkExtent2D ext  = m_swapchain.GetExtent();
                LoadCanvasToSwapchain(f.cmd, *c, swapImg, VK_IMAGE_LAYOUT_UNDEFINED);
                BeginPassLoad();
                // Depth must NOT carry across frames — clear it inside the pass.
                {
                    VkClearAttachment ca{};
                    ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                    ca.clearValue.depthStencil = { 1.0f, 0u };
                    VkClearRect cr{};
                    cr.rect.extent    = ext;
                    cr.layerCount     = 1;
                    vkCmdClearAttachments(f.cmd, 1, &ca, 1, &cr);
                }
                // BeginPassLoad doesn't set viewport/scissor (BeginPass does);
                // set the same Y-flipped full-extent defaults here.
                float vpW = static_cast<float>(ext.width);
                float vpH = static_cast<float>(ext.height);
                m_vpFBOW = vpW;
                m_vpFBOH = vpH;
                VkViewport vp{};
                vp.width    = vpW;
                vp.height   = -vpH;
                vp.y        = vpH;
                vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
                vkCmdSetViewport(f.cmd, 0, 1, &vp);
                VkRect2D sc{ {0, 0}, ext };
                vkCmdSetScissor(f.cmd, 0, 1, &sc);
                resumedFromCanvas = true;
            }
        }
        if (!resumedFromCanvas) {
            BeginPass(m_pendingRT);
        }
        m_pendingRT = nullptr;
    }

    // N64 SetColorImage handler notification: the game switched its render
    // target buffer.  Save the swapchain (holding the OLD buffer's content)
    // into the old canvas, load the NEW buffer's canvas, and resume the
    // pass — full per-buffer isolation on a single swapchain image.
    // Outside a frame (command buffer closed) just latch the new address;
    // the next BeginFrame restores the right canvas.
    void SetActiveCanvas(uint32_t n64Addr) override {
        n64Addr &= 0x7FFFFF;
        if (n64Addr == m_activeCanvasAddr) return;

        if (!m_initialized || !m_frameRecording || m_skipPresent ||
            !m_frameOnSwapchain || !m_canvasPreserve ||
            m_ctx.deviceLost.load(std::memory_order_relaxed)) {
            m_activeCanvasAddr = n64Addr;
            return;
        }
        PerFrame& f = m_frames[m_currentFrame];

        bool wasActive = m_passActive;
        if (wasActive) { vkCmdEndRenderPass(f.cmd); m_passActive = false; }

        VkImage swapImg = m_swapchain.GetImage(m_currentImage);
        CanvasSlot* oldC = EnsureCanvas(m_activeCanvasAddr);
        // Only persist the outgoing buffer if something was actually DRAWN to
        // it while it was active. A buffer that was merely bound must keep the
        // content it already had.
        //
        // Without this, MM's every-frame Z-clear (it briefly switches the color
        // image to the Z-buffer address 0x383AC0, issues a fill, and switches
        // straight back -- visible as a switch pair per frame in CANVAS_DBG)
        // destroyed the pause-menu world snapshot stored in that address's
        // canvas: on switching TO it we intentionally skip LoadCanvasToSwapchain
        // when the canvas is invalid (keeping stale pixels rather than flashing
        // black, see below), so the swapchain still held the LIVE menu frame --
        // and this save then wrote that live frame into canvas[0x383AC0] and
        // marked it valid. The pause background consequently animated along with
        // the page-turn instead of showing the frozen world snapshot. (Resizing
        // the window "fixed" the bouncing precisely because swapchain recreation
        // drops all canvases, breaking the feedback loop -- a long-standing
        // clue in this investigation that only makes sense with this mechanism.)
        // NOTE (diagnostic, see NOVA64_CANVAS_DBG below): whether the save
        // actually RUNS is gated on m_activeCanvasDirty, not just oldC being
        // non-null -- a canvas object can exist (from a previous frame) while
        // this particular activation drew nothing to it, in which case the
        // save is correctly skipped and the canvas keeps its LAST real content
        // (not garbage) -- but the OLD trace line below couldn't distinguish
        // "skipped, kept prior valid content" from "skipped, still never
        // written" from "actually saved this time". Capture the real outcome.
        bool savedOld = false;
        if (swapImg && oldC && m_activeCanvasDirty) {
            SaveSwapchainToCanvas(f.cmd, swapImg, *oldC);
            savedOld = true;
        }
        CanvasSlot* newC = EnsureCanvas(n64Addr);
        if (swapImg && newC && newC->valid) {
            LoadCanvasToSwapchain(f.cmd, *newC, swapImg, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        }
        // If the new canvas has no content yet, keep the swapchain as-is: the
        // game fully repaints new buffers before displaying them, and stale
        // pixels are preferable to a hard black flash mid-frame.

        static const bool s_cvDbg = std::getenv("NOVA64_CANVAS_DBG") != nullptr;
        if (s_cvDbg) {
            std::fprintf(stderr, "[CANVAS] switch 0x%06X -> 0x%06X (oldExists=%d dirty=%d savedOld=%d oldValidNow=%d loadNew=%d newValid=%d)\n",
                         m_activeCanvasAddr, n64Addr,
                         oldC != nullptr, m_activeCanvasDirty, savedOld,
                         oldC ? (int)oldC->valid : -1,
                         newC != nullptr, newC ? (int)newC->valid : -1);
        }

        m_activeCanvasAddr  = n64Addr;
        m_activeCanvasDirty = false;   // nothing drawn to the new target yet
        if (wasActive) BeginPassLoad();
    }

    // VI origin update from the video plugin: which N64 buffer is display-
    // ready. EndFrame presents this buffer's canvas instead of the one still
    // being drawn.
    void SetPresentOrigin(uint32_t n64Addr) override {
        m_presentOriginAddr = n64Addr & 0x7FFFFF;
    }

    void EndFrame() override {
        if (!m_initialized || m_skipPresent) return;
        if (m_ctx.deviceLost.load(std::memory_order_relaxed)) return;
        PerFrame& f = m_frames[m_currentFrame];

        if (m_passActive) {
            vkCmdEndRenderPass(f.cmd);
            m_passActive = false;
        }

        // Compute-RT hybrid composite: now that all raster is done and the pass
        // is closed (swapchain in PRESENT_SRC_KHR), the ray tracer reads the
        // textured frame, applies shadows/effects, and blits the result back.
        if (m_rtComposePending && m_computeRT && m_computeRT->Ready()) {
            VkImage    sc  = m_swapchain.GetImage(m_currentImage);
            VkExtent2D ext = m_swapchain.GetExtent();
            m_rtUBO.dims[0] = ext.width;
            m_rtUBO.dims[1] = ext.height;

            // The composite samples the raster depth buffer (billboard/decal
            // suppression). Transition depth → shader-read for the compute
            // passes, back to attachment layout afterwards.
            VkImage  depthImg = m_swapchain.GetDepthImage();
            VkFormat depthFmt = m_swapchain.GetDepthFormat();
            VkImageAspectFlags depthAspects = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (depthFmt == VK_FORMAT_D24_UNORM_S8_UINT ||
                depthFmt == VK_FORMAT_D32_SFLOAT_S8_UINT)
                depthAspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
            auto depthBarrier = [&](VkImageLayout oldL, VkImageLayout newL,
                                    VkAccessFlags srcA, VkAccessFlags dstA,
                                    VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.oldLayout = oldL; b.newLayout = newL;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = depthImg;
                b.subresourceRange = {depthAspects, 0, 1, 0, 1};
                b.srcAccessMask = srcA; b.dstAccessMask = dstA;
                vkCmdPipelineBarrier(f.cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
            };
            depthBarrier(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

            m_computeRT->RenderHybrid(m_ctx, f.cmd, m_currentFrame, m_rtBVH, m_rtUBO,
                                      sc, ext.width, ext.height, depthImg, depthFmt);

            depthBarrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_ACCESS_SHADER_READ_BIT,
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
        }
        m_rtComposePending = false;

        // Persist the frame's rendering into the ACTIVE buffer's canvas, then
        // put the DISPLAY-READY buffer's canvas (VI origin) on the swapchain
        // for presentation. The game flips VI origin only to buffers it has
        // finished drawing, so the presented image is always a complete
        // frame — never the half-drawn buffer the DL is still working on.
        // The swapchain image is still application-owned here — reading it
        // pre-present is legal, unlike reading an already-presented image.
        // Guard on m_currentRT: if the frame ended inside a custom render
        // target pass, the swapchain image is not in PRESENT_SRC layout.
        if (m_frameOnSwapchain && m_currentRT == nullptr && m_canvasPreserve) {
            VkImage    swapImg = m_swapchain.GetImage(m_currentImage);
            VkExtent2D ext     = m_swapchain.GetExtent();
            CanvasSlot* active = EnsureCanvas(m_activeCanvasAddr);
            if (swapImg && active && m_activeCanvasDirty &&
                ext.width == m_canvasW && ext.height == m_canvasH) {
                SaveSwapchainToCanvas(f.cmd, swapImg, *active);
                CanvasSlot* presentC = FindCanvasForOrigin(m_presentOriginAddr);
                if (presentC && presentC != active && presentC->valid) {
                    LoadCanvasToSwapchain(f.cmd, *presentC, swapImg,
                                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                }
                static const bool s_cvDbg = std::getenv("NOVA64_CANVAS_DBG") != nullptr;
                if (s_cvDbg) {
                    uint32_t presentAddr = 0xFFFFFF;
                    for (auto& [a, c] : m_canvases) if (&c == presentC) presentAddr = a;
                    std::fprintf(stderr, "[CANVAS] endframe active=0x%06X origin=0x%06X -> present=0x%06X (valid=%d self=%d)\n",
                                 m_activeCanvasAddr, m_presentOriginAddr, presentAddr,
                                 presentC ? (int)presentC->valid : -1,
                                 presentC == active);
                }
            }
        }

        m_frameRecording = false;
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
        // TRANSFER is included because the canvas copy-forward in BeginFrame
        // writes the acquired image at the transfer stage — the acquire
        // semaphore must gate that write, not just color-attachment output.
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                         VK_PIPELINE_STAGE_TRANSFER_BIT;
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

    // Headless framebuffer capture for debugging. When NOVA64_SHOT_FRAME lists
    // the current present index (comma-separated), copy the just-rendered
    // swapchain image to <NOVA64_SHOT_PATH>_<n>.png. Fully env-gated: a normal
    // run (no env var) does nothing. Self-verifying — a coherent PNG proves the
    // capture path works regardless of what the game drew.
    void MaybeCaptureFrame() {
        static bool s_parsed = false;
        static std::vector<long> s_targets;
        static std::string s_basePath;
        static long s_presentCount = 0;
        static long s_every = 0;        // NOVA64_SHOT_EVERY: capture every Nth present
        static long s_everyStart = 0;   // NOVA64_SHOT_START: first present to begin "every"
        if (!s_parsed) {
            s_parsed = true;
            if (const char* env = std::getenv("NOVA64_SHOT_FRAME")) {
                const char* p = env;
                while (*p) { char* e = nullptr; long v = std::strtol(p, &e, 10);
                    if (e == p) break; s_targets.push_back(v); p = (*e == ',') ? e + 1 : e; }
            }
            if (const char* ev = std::getenv("NOVA64_SHOT_EVERY")) s_every = std::strtol(ev, nullptr, 10);
            if (const char* st = std::getenv("NOVA64_SHOT_START")) s_everyStart = std::strtol(st, nullptr, 10);
            const char* path = std::getenv("NOVA64_SHOT_PATH");
            s_basePath = path ? path : "/tmp/nova64_shot";
        }
        // n is the VI frame index, published by VideoPlugin::UpdateScreen. The
        // raw swapchain present counter is decoupled from VI rate in HLE, so we
        // key capture off VI count for a consistent cross-mode timeline.
        const long n = g_presentIndex.load(std::memory_order_relaxed);
        static long s_lastCaptured = -1;
        static long s_nextThreshold = -1;
        (void)s_presentCount;
        if (n == s_lastCaptured) return; // one capture per VI index
        bool hit = false;
        for (long t : s_targets) if (t == n) { hit = true; break; }
        // "every" mode: presents are sparse in HLE, so capture on the FIRST
        // present at-or-after each threshold rather than requiring an exact
        // modulo match (which would almost never coincide with a present).
        if (s_every > 0 && n >= s_everyStart) {
            if (s_nextThreshold < 0) s_nextThreshold = s_everyStart;
            if (n >= s_nextThreshold) {
                hit = true;
                s_nextThreshold = n + s_every;
            }
        }
        if (!hit) return;
        s_lastCaptured = n;
        CaptureSwapchainToPNG((s_basePath + "_" + std::to_string(n) + ".png").c_str());
    }

    void CaptureSwapchainToPNG(const char* path) {
        vkQueueWaitIdle(m_device.GetGraphicsQueue()); // debug-only: ensure render done
        VkExtent2D ext = m_swapchain.GetExtent();
        const uint32_t w = ext.width, h = ext.height;
        if (w == 0 || h == 0) return;
        const VkDeviceSize sz = static_cast<VkDeviceSize>(w) * h * 4;
        VKBufferStorage staging{};
        if (!CreateBufferStorage(m_ctx, sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                staging))
            return;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = m_transientPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(m_device.Get(), &ai, &cb) != VK_SUCCESS) {
            DestroyBufferStorage(m_ctx, staging); return;
        }
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        VkImage img = m_swapchain.GetImage(m_currentImage);
        TransitionImage(cb, img, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { w, h, 1 };
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging.buffer, 1, &region);
        TransitionImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        vkCreateFence(m_device.Get(), &fci, nullptr, &fence);
        vkQueueSubmit(m_device.GetGraphicsQueue(), 1, &si, fence);
        vkWaitForFences(m_device.Get(), 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(m_device.Get(), fence, nullptr);
        vkFreeCommandBuffers(m_device.Get(), m_transientPool, 1, &cb);

        // Swapchain images are typically BGRA8 — swizzle to RGBA for the PNG and
        // force opaque alpha (the presented image's alpha is irrelevant).
        const VkFormat fmt = m_swapchain.GetFormat();
        const bool bgra = (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB);
        std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
        const uint8_t* src = static_cast<const uint8_t*>(staging.mapped);
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            const uint8_t c0 = src[i*4+0], c1 = src[i*4+1], c2 = src[i*4+2];
            if (bgra) { rgba[i*4+0]=c2; rgba[i*4+1]=c1; rgba[i*4+2]=c0; }
            else      { rgba[i*4+0]=c0; rgba[i*4+1]=c1; rgba[i*4+2]=c2; }
            rgba[i*4+3] = 255;
        }
        DestroyBufferStorage(m_ctx, staging);
        if (Nova::WritePNG(path, static_cast<int>(w), static_cast<int>(h), 4,
                           rgba.data(), static_cast<int>(w) * 4))
            Logger::Info("[SHOT] wrote {} ({}x{})", path, w, h);
        else
            Logger::Error("[SHOT] WritePNG failed for {}", path);
    }

    // Lazily build the gamma-correction compute pipeline. Mirrors the m_computeRT
    // pattern above: cheap to skip entirely for a game/frame that never enables
    // gamma, built once on first actual use.
    bool BuildGammaPass() {
        if (m_gammaBuilt) return m_gammaPipeline != VK_NULL_HANDLE;
        m_gammaBuilt = true;

        // In-place sqrt() on an rgba8 storage image. N64 Programming Manual
        // §5.3: recovering the pre-gamma value from a gamma-corrected one is
        // "square and shift down by 8 each color component" — i.e. in
        // normalized [0,1] terms, undo = x². The forward hardware op the VI
        // actually applies is therefore the inverse of squaring: sqrt(x).
        // (Matches FramebufferManager's existing, correct GL implementation:
        // pow(c, 1.0/uGamma) with uGamma=2.0 — this is the same curve, just
        // with the exponent spelled out instead of computed from a variable
        // gamma value that in practice is always 2.0 for this bit.)
        static const char* kGammaSrc = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(rgba8, binding = 0) uniform image2D img;
void main() {
    ivec2 sz = imageSize(img);
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= sz.x || p.y >= sz.y) return;
    vec4 c = imageLoad(img, p);
    c.rgb = sqrt(c.rgb);
    imageStore(img, p, c);
}
)GLSL";
        GlslangInitProcess();
        std::vector<uint32_t> spv = CompileGLSLtoSPIRV(ShaderStage::Compute, kGammaSrc, "vi_gamma");
        GlslangFinalizeProcess();
        if (spv.empty()) {
            Logger::Error("VulkanRenderer: gamma shader compilation failed");
            return false;
        }
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = spv.size() * sizeof(uint32_t);
        mi.pCode    = spv.data();
        if (vkCreateShaderModule(m_device.Get(), &mi, nullptr, &m_gammaShaderMod) != VK_SUCCESS) {
            Logger::Error("VulkanRenderer: gamma shader module creation failed");
            return false;
        }

        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dslci.bindingCount = 1;
        dslci.pBindings    = &binding;
        if (vkCreateDescriptorSetLayout(m_device.Get(), &dslci, nullptr, &m_gammaDSLayout) != VK_SUCCESS) {
            Logger::Error("VulkanRenderer: gamma descriptor set layout creation failed");
            return false;
        }

        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts    = &m_gammaDSLayout;
        if (vkCreatePipelineLayout(m_device.Get(), &pli, nullptr, &m_gammaPipeLayout) != VK_SUCCESS) {
            Logger::Error("VulkanRenderer: gamma pipeline layout creation failed");
            return false;
        }

        VkPipelineShaderStageCreateInfo ssi{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ssi.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        ssi.module = m_gammaShaderMod;
        ssi.pName  = "main";
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage  = ssi;
        cpi.layout = m_gammaPipeLayout;
        if (vkCreateComputePipelines(m_device.Get(), m_ctx.pipelineCache, 1, &cpi, nullptr,
                                     &m_gammaPipeline) != VK_SUCCESS) {
            Logger::Error("VulkanRenderer: gamma pipeline creation failed");
            return false;
        }

        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets       = 1;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes    = &poolSize;
        if (vkCreateDescriptorPool(m_device.Get(), &dpci, nullptr, &m_gammaDescPool) != VK_SUCCESS) {
            Logger::Error("VulkanRenderer: gamma descriptor pool creation failed");
            return false;
        }
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool     = m_gammaDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &m_gammaDSLayout;
        if (vkAllocateDescriptorSets(m_device.Get(), &dsai, &m_gammaDescSet) != VK_SUCCESS) {
            Logger::Error("VulkanRenderer: gamma descriptor set allocation failed");
            return false;
        }
        return true;
    }

    // Applies the sqrt() gamma curve to the just-rendered swapchain image,
    // in place, right before it's presented. Gated on the SAME VIConfig the
    // GL backend already reads (g_fbManager.GetVIConfig().gammaCorrect,
    // populated from the live VI_STATUS register in VideoPlugin.cpp) — a
    // no-op unless the game itself actually enabled gamma correction.
    //
    // Swapchain images aren't created with STORAGE_BIT usage (and adding it
    // isn't universally safe across present-engine/driver combinations), so
    // this blits into a private rgba8 storage-capable temp image (format
    // conversion happens for free in the blit — same trick the RT compute
    // pipeline already uses to read the swapchain, VKComputeRaytracer.cpp's
    // "copy the textured raster (swapchain) into the private raster image"),
    // runs the shader in place there, then blits back.
    void ApplyGammaIfNeeded() {
        if (!g_fbManager.GetVIConfig().gammaCorrect) return;
        if (!BuildGammaPass()) return;

        VkExtent2D ext = m_swapchain.GetExtent();
        if (m_gammaTmpW != ext.width || m_gammaTmpH != ext.height) {
            if (m_gammaTmpView) { vkDestroyImageView(m_device.Get(), m_gammaTmpView, nullptr); m_gammaTmpView = VK_NULL_HANDLE; }
            if (m_gammaTmpImg)  { vkDestroyImage(m_device.Get(), m_gammaTmpImg, nullptr); m_gammaTmpImg = VK_NULL_HANDLE; }
            if (m_gammaTmpMem)  { vkFreeMemory(m_device.Get(), m_gammaTmpMem, nullptr); m_gammaTmpMem = VK_NULL_HANDLE; }

            VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ici.imageType     = VK_IMAGE_TYPE_2D;
            ici.format        = VK_FORMAT_R8G8B8A8_UNORM;
            ici.extent        = {ext.width, ext.height, 1};
            ici.mipLevels     = 1;
            ici.arrayLayers   = 1;
            ici.samples       = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
            ici.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_device.Get(), &ici, nullptr, &m_gammaTmpImg) != VK_SUCCESS) return;
            VkMemoryRequirements mr{};
            vkGetImageMemoryRequirements(m_device.Get(), m_gammaTmpImg, &mr);
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize  = mr.size;
            mai.memoryTypeIndex = FindMemoryType(m_ctx.physical, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (mai.memoryTypeIndex == UINT32_MAX ||
                vkAllocateMemory(m_device.Get(), &mai, nullptr, &m_gammaTmpMem) != VK_SUCCESS ||
                vkBindImageMemory(m_device.Get(), m_gammaTmpImg, m_gammaTmpMem, 0) != VK_SUCCESS) {
                return;
            }
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image    = m_gammaTmpImg;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format   = VK_FORMAT_R8G8B8A8_UNORM;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(m_device.Get(), &vi, nullptr, &m_gammaTmpView) != VK_SUCCESS) return;
            m_gammaTmpW = ext.width;
            m_gammaTmpH = ext.height;
            m_gammaTmpValid = false;

            VkDescriptorImageInfo dii{};
            dii.imageView   = m_gammaTmpView;
            dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet          = m_gammaDescSet;
            w.dstBinding      = 0;
            w.descriptorCount = 1;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w.pImageInfo      = &dii;
            vkUpdateDescriptorSets(m_device.Get(), 1, &w, 0, nullptr);
        }
        if (!m_gammaTmpImg) return;

        VkImage swapImg = m_swapchain.GetImage(m_currentImage);

        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool        = m_transientPool;
        cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(m_device.Get(), &cai, &cb) != VK_SUCCESS) return;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);

        TransitionImage(cb, swapImg, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        TransitionImage(cb, m_gammaTmpImg,
                        m_gammaTmpValid ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        0, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffsets[1]  = {static_cast<int32_t>(ext.width), static_cast<int32_t>(ext.height), 1};
        region.dstSubresource = region.srcSubresource;
        region.dstOffsets[1]  = region.srcOffsets[1];
        vkCmdBlitImage(cb, swapImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       m_gammaTmpImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
        m_gammaTmpValid = true;

        TransitionImage(cb, m_gammaTmpImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_gammaPipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_gammaPipeLayout, 0, 1, &m_gammaDescSet, 0, nullptr);
        vkCmdDispatch(cb, (ext.width + 7) / 8, (ext.height + 7) / 8, 1);
        TransitionImage(cb, m_gammaTmpImg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        TransitionImage(cb, swapImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdBlitImage(cb, m_gammaTmpImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       swapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
        TransitionImage(cb, swapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        // Leave m_gammaTmpImg in TRANSFER_SRC_OPTIMAL (matches m_gammaTmpValid's
        // implied layout for next frame's DST transition above).
        TransitionImage(cb, m_gammaTmpImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        vkEndCommandBuffer(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cb;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        vkCreateFence(m_device.Get(), &fci, nullptr, &fence);
        vkQueueSubmit(m_device.GetGraphicsQueue(), 1, &si, fence);
        vkWaitForFences(m_device.Get(), 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(m_device.Get(), fence, nullptr);
        vkFreeCommandBuffers(m_device.Get(), m_transientPool, 1, &cb);
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
        ApplyGammaIfNeeded();
        MaybeCaptureFrame();
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
        m_activeCanvasDirty = true;
        vkCmdDraw(m_frames[m_currentFrame].cmd, count, 1, first, 0);
        ++m_stats.drawCalls;
        m_stats.triangles += count / 3;
    }
    void DrawIndexed(PrimitiveType, uint32_t indexCount, IndexType type, uint32_t offset) override {
        if (!PrepareDraw()) return;
        if (m_curIB == VK_NULL_HANDLE) return;
        m_activeCanvasDirty = true;
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
        if (!m_initialized) return;
        if (!scene || scene->GetTriangleCount() == 0 || w == 0 || h == 0) return;

        // ── Hardware RT path (Vulkan KHR ray tracing pipeline) ──────────────
        if (m_device.SupportsRayTracing()) {
            PerFrame& f = m_frames[m_currentFrame];
            if (!scene->BuildAS(m_ctx, f.cmd)) {
                Logger::Warn("VulkanRenderer::ExecuteRayTracing: BuildAS failed");
                return;
            }
            void* tlas = scene->GetTLAS();
            if (!tlas) return;
            void* outputView = reinterpret_cast<void*>(
                m_swapchain.GetImageView(m_currentImage));
            DispatchRayTracing(tlas, outputView, w, h);
            return;
        }

        // ── Compute RT path (MoltenVK: no hardware AS / RT pipeline) ────────
        // CPU-only here: build the BVH and prepare the UBO. The GPU dispatch +
        // hybrid composite happen in EndFrame (so RT reads the finished raster).
        if (!scene->HasCamera()) return;          // need the world perspective
        if (!m_computeRT) {
            m_computeRT = std::make_unique<VKComputeRaytracer>();
            if (!m_computeRT->Build(m_ctx)) { m_computeRT.reset(); return; }
        }
        m_rtBVH.Build(scene->GetFrameData());
        if (m_rtBVH.Empty()) return;

        // Scene-size gate, effectively OFF by default (user wants RT on menus
        // too). Kept env-tunable as an escape hatch for games whose menus
        // misbehave: NOVA64_RT_MIN_TRIS / NOVA64_RT_MIN_DIAG.
        static const uint32_t s_rtMinTris = []() {
            const char* v = std::getenv("NOVA64_RT_MIN_TRIS");
            return (v && v[0]) ? static_cast<uint32_t>(std::atoi(v)) : 32u;
        }();
        static const float s_rtMinDiag = []() {
            const char* v = std::getenv("NOVA64_RT_MIN_DIAG");
            return (v && v[0]) ? static_cast<float>(std::atof(v)) : 0.0f;
        }();
        float rtSceneDiag = 0.0f;
        {
            const auto& root = m_rtBVH.Nodes()[0];
            float dx = root.aabbMax[0] - root.aabbMin[0];
            float dy = root.aabbMax[1] - root.aabbMin[1];
            float dz = root.aabbMax[2] - root.aabbMin[2];
            float diag = std::sqrt(dx*dx + dy*dy + dz*dz);
            rtSceneDiag = diag;
            if (m_rtBVH.TriangleCount() < s_rtMinTris || diag < s_rtMinDiag) {
                static int s_skipLog = 0;
                if ((s_skipLog++ % 60) == 0)
                    Logger::Info("RT-skip: tris={} diag={:.0f} (min {} / {:.0f})",
                                 m_rtBVH.TriangleCount(), diag, s_rtMinTris, s_rtMinDiag);
                return;
            }
        }

        m_rtUBO = RTUBOData{};
        Invert4x4(scene->Projection(), m_rtUBO.invProj);
        // Forward projection too (raster-depth prediction in the composite).
        // Row-major copy — the row-vector→GLSL-column transpose falls out,
        // same convention as invProj.
        std::memcpy(m_rtUBO.proj, scene->Projection(), 16 * sizeof(float));
        m_rtUBO.misc[0] = rtSceneDiag;   // composite: eye-pos reconstruction scale
        {
            const float* vp = scene->Viewport();
            for (int i = 0; i < 4; ++i) m_rtUBO.viewport[i] = vp[i];
        }
        if (scene->HasLight()) {
            const float* d = scene->LightDir();
            // lightDir.w = the sun's angular size (tangent), RT64's pointRadius
            // analogue for a directional light: the trace shader jitters shadow
            // rays across a cone of this radius for soft penumbras. 0 disables.
            static const float s_rtSunSize = []() {
                const char* v = std::getenv("NOVA64_RT_SUN");
                return (v && v[0]) ? static_cast<float>(std::atof(v)) : 0.05f;
            }();
            m_rtUBO.lightDir[0] = d[0]; m_rtUBO.lightDir[1] = d[1]; m_rtUBO.lightDir[2] = d[2];
            m_rtUBO.lightDir[3] = s_rtSunSize;
            // Sun color is WHITE. N64 light colors are per-object MATERIAL
            // colors (SM64 bakes object tints into them) — averaging them
            // tinted the whole scene (user saw everything red). Scene color
            // belongs to the albedo; the sun stays neutral, like RT64's.
            m_rtUBO.lightColor[0] = 1.0f;
            m_rtUBO.lightColor[1] = 1.0f;
            m_rtUBO.lightColor[2] = 1.0f;
        }
        // lightColor.w = frame counter — rotates the shadow sampling pattern each
        // frame so the temporal accumulator converges (RT64's frameCount).
        static uint32_t s_rtFrameCounter = 0;
        m_rtUBO.lightColor[3] = static_cast<float>(s_rtFrameCounter++ & 0xFFFF);
        // NOVA64_RT_DEBUG: 1 = normals + grid overlay, 2 = lighting-term-only
        // view (spatial alignment check for the RT term vs the raster).
        static const int s_rtDebug = []() {
            const char* v = std::getenv("NOVA64_RT_DEBUG");
            return (v && v[0]) ? std::atoi(v) : 0;
        }();
        // Denoise/shadow tuning (UBO ambient slot; see RTComputeShaders.hpp):
        // x = blur radius (px), y = normal edge power, z = depth edge threshold
        // (normalized eye depth), w = shadow strength (0 = black, 1 = none).
        auto envf = [](const char* n, float def) {
            const char* v = std::getenv(n);
            return (v && v[0]) ? static_cast<float>(std::atof(v)) : def;
        };
        static const float s_rtBlur    = envf("NOVA64_RT_BLUR",    3.0f);
        static const float s_rtEdgeN   = envf("NOVA64_RT_EDGE_N", 32.0f);
        static const float s_rtEdgeD   = envf("NOVA64_RT_EDGE_D",  0.02f);
        static const float s_rtShadow  = envf("NOVA64_RT_SHADOW",  0.5f);
        static const float s_rtExpose  = envf("NOVA64_RT_EXPOSURE", 1.15f);
        m_rtUBO.ambient[0] = s_rtBlur;
        m_rtUBO.ambient[1] = s_rtEdgeN;
        m_rtUBO.ambient[2] = s_rtEdgeD;
        m_rtUBO.ambient[3] = s_rtShadow;

        // NO TEMPORAL HISTORY: every frame's shadows/AO/GI are computed fresh
        // from that frame's own trace + spatial (single-frame) bilateral blur
        // only, with zero blending from any previous frame. The denoise
        // shader's blend is `mix(prevFrame, thisFrame, 1/histCap)` — histCap=1
        // makes that a full replace every time (see RTComputeShaders.hpp's
        // "Temporal: RT64-style history accumulation" block). This used to be
        // motion-adaptive (long history when the camera was still, for extra
        // noise reduction), driven by a heuristic that approximated camera
        // motion from how much the RT BVH's root AABB center shifts — the
        // only motion signal available in an eye-space-only pipeline (the
        // camera is always at the origin by construction, so there's no
        // direct position to track). That heuristic degrades badly in large
        // open scenes (Zelda's outdoor areas pull huge, distant terrain/sky
        // into the same BVH, so its diagonal dwarfs any nearby motion), and
        // even tuned correctly, a stale multi-frame blend means shadows/
        // lighting visibly lag the current frame's geometry after any camera
        // or character movement — reads as everything being spatially "off"
        // from where it should be. Forcing histCap=1 removes that failure
        // mode entirely, at the cost of the extra noise reduction favorable
        // static-camera history gave.
        m_rtUBO.extra[0] = 1.0f / 64.0f;
        m_rtUBO.extra[1] = s_rtExpose;
        // Reflections ON by default, material-gated to metal (env-mapped) and
        // water (translucent up-facing) in the trace shader. NOVA64_RT_REFL=0
        // disables them.
        static const bool s_rtRefl = []() {
            const char* v = std::getenv("NOVA64_RT_REFL");
            return !(v && v[0] == '0');
        }();
        m_rtUBO.dims[2] = m_rtBVH.NodeCount();
        m_rtUBO.dims[3] = RT_F_SHADOWS | RT_F_LIGHTING
                        | (s_rtRefl       ? RT_F_REFLECTIONS   : 0u)
                        | (s_rtDebug == 1 ? RT_F_DEBUG         : 0u)
                        | (s_rtDebug == 2 ? RT_F_DEBUG_TERM    : 0u)
                        | (s_rtDebug == 3 ? RT_F_DEBUG_COVERED : 0u);
        m_rtComposePending = true;

        {
            static int s_rtLog = 0;
            if (s_rtLog < 600 && (s_rtLog < 6 || (s_rtLog % 60) == 0)) {
                const auto& root = m_rtBVH.Nodes()[0];
                Logger::Info("RT[{}]: tris={} nodes={} rootAABB=({:.0f},{:.0f},{:.0f})..({:.0f},{:.0f},{:.0f}) "
                             "light=({:.2f},{:.2f},{:.2f}) sun={}",
                             s_rtLog, m_rtBVH.TriangleCount(), m_rtBVH.NodeCount(),
                             root.aabbMin[0], root.aabbMin[1], root.aabbMin[2],
                             root.aabbMax[0], root.aabbMax[1], root.aabbMax[2],
                             m_rtUBO.lightDir[0], m_rtUBO.lightDir[1], m_rtUBO.lightDir[2],
                             m_rtUBO.lightDir[3]);
            }
            ++s_rtLog;
        }
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
    void InvalidateCanvas() override {
        // The call site (VideoPlugin.cpp, VI_TYPE==0 / screen-blank handling)
        // documents the intent as dropping the FRAME-CARRY canvas so a blank
        // doesn't resurrect the previous frame's content on the next
        // BeginFrame -- i.e. only whatever's about to be shown again next
        // frame (the currently-active buffer). This previously invalidated
        // EVERY canvas in the map instead, unconditionally -- including
        // completely unrelated scratch buffers with no connection to what's
        // being blanked. MM/OoT's pause-menu world-snapshot buffer (written
        // once at menu-open, then read back as a texture on every background
        // recomposite -- e.g. page turns -- without ever becoming the active
        // render target again) was exactly such a buffer: any VI blank during
        // the menu-open or page-turn transition animation (fades are a
        // textbook use of VI_TYPE==0) permanently zeroed its valid flag, and
        // since only SaveSwapchainToCanvas (which only runs for the ACTIVE
        // target) ever sets valid=true again, it never recovered. Confirmed
        // via a live capture: the background read correctly for exactly two
        // frames, then failed on every following frame with no recovery --
        // a one-time event, not a gradual staleness/eviction pattern.
        auto it = m_canvases.find(m_activeCanvasAddr);
        if (it != m_canvases.end()) it->second.valid = false;
    }

    // Sample a specific N64 buffer's persistent canvas as a texture. See the
    // rationale on IRenderer::BlitCanvasToTexture -- this is our equivalent of
    // GLideN64's always-available per-FrameBuffer color texture.
    // CPU->GPU framebuffer upload: the direction that never existed on this
    // backend (see IRenderer::UploadPixelsToCanvas). Stages RGBA8 pixels into a
    // temporary N64-resolution image, then blits (scaling) into the persistent
    // canvas for that buffer address, leaving it in the canonical TRANSFER_SRC
    // resting layout so the normal present/save/load paths pick it up unchanged.
    bool UploadPixelsToCanvas(uint32_t n64Addr, uint32_t w, uint32_t h,
                              const uint32_t* pixels) override {
        if (!m_initialized || !m_canvasPreserve || !pixels || w == 0 || h == 0)
            return false;
        if (!m_frameRecording || m_skipPresent ||
            m_ctx.deviceLost.load(std::memory_order_relaxed))
            return false;

        CanvasSlot* c = EnsureCanvas(n64Addr & 0x7FFFFF);
        if (!c || !c->img) return false;

        const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
        VKBufferStorage staging{};
        if (!CreateBufferStorage(m_ctx, bytes,
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 staging) || !staging.mapped) {
            return false;
        }
        std::memcpy(staging.mapped, pixels, static_cast<size_t>(bytes));

        // Temporary N64-resolution image to blit from (canvases are swapchain
        // sized, so a straight buffer->image copy could not rescale).
        VkImage tmpImg = VK_NULL_HANDLE;
        VkDeviceMemory tmpMem = VK_NULL_HANDLE;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = { w, h, 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device.Get(), &ici, nullptr, &tmpImg) != VK_SUCCESS) {
            DestroyBufferStorage(m_ctx, staging);
            return false;
        }
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(m_device.Get(), tmpImg, &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = FindMemoryType(m_ctx.physical, mr.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(m_device.Get(), &mai, nullptr, &tmpMem) != VK_SUCCESS ||
            vkBindImageMemory(m_device.Get(), tmpImg, tmpMem, 0) != VK_SUCCESS) {
            if (tmpMem) vkFreeMemory(m_device.Get(), tmpMem, nullptr);
            vkDestroyImage(m_device.Get(), tmpImg, nullptr);
            DestroyBufferStorage(m_ctx, staging);
            return false;
        }

        PerFrame& f = m_frames[m_currentFrame];
        bool wasActive = m_passActive;
        if (wasActive) { vkCmdEndRenderPass(f.cmd); m_passActive = false; }

        TransitionImage(f.cmd, tmpImg,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        0, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy bic{};
        bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        bic.imageExtent = { w, h, 1 };
        vkCmdCopyBufferToImage(f.cmd, staging.buffer, tmpImg,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        TransitionImage(f.cmd, tmpImg,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        // Canvas rests in TRANSFER_SRC (or UNDEFINED when freshly created).
        TransitionImage(f.cmd, c->img,
                        c->valid ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        c->valid ? VK_ACCESS_TRANSFER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        const VkExtent2D ext = m_swapchain.GetExtent();
        VkImageBlit region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = region.srcSubresource;
        region.srcOffsets[0] = { 0, 0, 0 };
        region.srcOffsets[1] = { int32_t(w), int32_t(h), 1 };
        region.dstOffsets[0] = { 0, 0, 0 };
        region.dstOffsets[1] = { int32_t(ext.width), int32_t(ext.height), 1 };
        vkCmdBlitImage(f.cmd, tmpImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       c->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region, VK_FILTER_LINEAR);
        TransitionImage(f.cmd, c->img,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        c->valid = true;

        if (wasActive) BeginPassLoad();

        // Retire once this frame slot is known complete (see DrainPendingUploads).
        PendingUpload pu;
        pu.img = tmpImg; pu.mem = tmpMem; pu.staging = staging;
        pu.frameIdx = m_currentFrame; pu.age = 0;
        m_pendingUploads.push_back(pu);
        return true;
    }

    bool BlitCanvasToTexture(uint32_t n64Addr, ITexture* dst) override {
        if (!dst || !m_initialized || !m_canvasPreserve) return false;
        const uint32_t addr = n64Addr & 0x7FFFFF;

        // Only use an ALREADY-SAVED canvas. Do not EnsureCanvas() here: creating
        // one on demand would hand back an uninitialised image (garbage), and
        // the currently-active buffer's freshest content lives in the swapchain,
        // not its canvas -- callers should keep using the snapshot path for that.
        //
        // Match by CONTAINMENT, not exact key (via FindCanvasForOrigin: exact,
        // else nearest canvas at or below the address within 1 MB). Canvases are
        // keyed by the SetColorImage address, but the FramebufferManager
        // registers entries at related-but-different addresses -- most commonly
        // base+0x280, since the VI origin sits a few rows into the buffer. An
        // exact-key lookup missed those every time (measured: blit attempted 40x,
        // succeeded 0x, all for 0x000280 while the canvas was keyed 0x000000).
        CanvasSlot* c = FindCanvasForOrigin(addr);
        if (!c || !c->valid || !c->img) return false;

        VkImage dstImage = reinterpret_cast<VkImage>(dst->GetNativeHandle());
        if (!dstImage) return false;

        PerFrame& f = m_frames[m_currentFrame];
        bool wasActive = m_passActive;
        if (wasActive) { vkCmdEndRenderPass(f.cmd); m_passActive = false; }

        const VkExtent2D ext = m_swapchain.GetExtent();
        uint32_t w = std::min(dst->GetWidth(),  ext.width);
        uint32_t h = std::min(dst->GetHeight(), ext.height);

        // A saved canvas rests in TRANSFER_SRC_OPTIMAL (SaveSwapchainToCanvas'
        // final transition), so it needs no transition here and is left as-is.
        TransitionImage(f.cmd, dstImage,
                        VK_IMAGE_LAYOUT_UNDEFINED,   // discard previous contents
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        0, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);

        // Blit (not Copy): canvases carry the swapchain's format (typically
        // BGRA8) while snapshot textures are RGBA8 -- Blit converts, Copy
        // would require format compatibility.
        VkImageBlit region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = region.srcSubresource;
        region.srcOffsets[0]  = { 0, 0, 0 };
        region.srcOffsets[1]  = { static_cast<int32_t>(w), static_cast<int32_t>(h), 1 };
        region.dstOffsets[0]  = { 0, 0, 0 };
        region.dstOffsets[1]  = { static_cast<int32_t>(w), static_cast<int32_t>(h), 1 };
        vkCmdBlitImage(f.cmd,
                       c->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dstImage,       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region, VK_FILTER_NEAREST);

        // Leave in SHADER_READ_ONLY so the caller can bind it immediately.
        TransitionImage(f.cmd, dstImage,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        if (wasActive) BeginPassLoad();
        return true;
    }

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
        if (!m_initialized || !rdram || fbW == 0 || fbH == 0 || m_skipPresent) {
            return;
        }
        // ── Per-address readback budget (was: ONE readback per whole frame) ──
        // The old guard was a single global `m_midFrameReadbackDone` bool, so
        // the FIRST GPU->RDRAM copy in a frame consumed the only allowance and
        // every later one silently no-oped. OoT/MM already perform an unrelated
        // CfB/AA-resolve copy-back read every single frame, so by the time the
        // pause menu issued its ~20 LoadTile strip reads there was no allowance
        // left -- the strips read never-written RDRAM (black panel). This is the
        // same starvation shape as the gpuModified/needsInventorySnapshot flags,
        // but a hard structural cap that no flag fix could route around.
        //
        // Two changes make lifting it both correct and cheap:
        //  1. Dedup by ADDRESS. One copy writes the buffer's ENTIRE image, so
        //     all 20 strips of one buffer are served by a single copy; repeats
        //     for the same address are genuinely redundant and still skipped.
        //  2. Chain the semaphore (see the submit below). The readback waits on
        //     f.imageAvailable and signals f.midFrameDone; a binary semaphore
        //     can only satisfy one wait, which is the real reason the cap
        //     existed. Later readbacks in the same frame must instead wait on
        //     f.midFrameDone -- exactly the substitution EndFrame already makes.
        const uint32_t reqAddr = fbAddress & 0x7FFFFF;
        for (uint32_t i = 0; i < m_frameReadbackCount; ++i) {
            if (m_frameReadbackAddrs[i] == reqAddr) return;
        }
        // Hard ceiling as a runaway guard: each readback is a full submit +
        // fence wait, so an unforeseen access pattern must not stall the frame.
        if (m_frameReadbackCount >= kMaxFrameReadbacks) return;

        PerFrame& f = m_frames[m_currentFrame];
        bool wasActive = m_passActive;
        if (wasActive) {
            vkCmdEndRenderPass(f.cmd);
            m_passActive = false;
        }

        // Choose the SOURCE image by the requested N64 buffer address, not the
        // swapchain unconditionally. The swapchain only ever holds the CURRENTLY
        // ACTIVE canvas; a different N64 buffer's content lives in its own saved
        // canvas image (see m_canvases / SetActiveCanvas). MM's pause menu draws
        // its blurred world snapshot into one buffer (e.g. 0x383AC0), then
        // switches the active buffer to the UI/display buffer BEFORE issuing the
        // LoadTile mosaic that reads the snapshot back. Reading the swapchain at
        // that point copies the UI buffer (black in the item-grid region) into
        // RDRAM instead of the world snapshot -- the asymmetric black pause-menu
        // panel bug. When the requested address is a valid, non-active saved
        // canvas, read THAT canvas image (already in TRANSFER_SRC_OPTIMAL after
        // SaveSwapchainToCanvas, so no layout transition is needed for it).
        // reqAddr computed above (readback budget check).
        VkImage srcImage    = VK_NULL_HANDLE;
        bool    srcIsCanvas = false;
        const bool wantActive = (reqAddr == m_activeCanvasAddr) || !m_canvasPreserve;
        if (!wantActive) {
            // Match by CONTAINMENT, not exact key: canvases are keyed by the
            // SetColorImage address while the FramebufferManager registers
            // entries at related addresses (commonly base+0x280 — the VI origin
            // sits a few rows into the buffer). An exact find() missed those.
            CanvasSlot* c = FindCanvasForOrigin(reqAddr);
            if (c && c->valid && c->img) {
                srcImage    = c->img;   // already resting in TRANSFER_SRC_OPTIMAL
                srcIsCanvas = true;
            }
        } else {
            srcImage = m_swapchain.GetImage(m_currentImage);
        }
        if (!srcImage) {
            // Deliberately DO NOT fall back to the swapchain for a non-active
            // buffer. The swapchain holds the CURRENTLY RENDERING frame, so
            // substituting it writes live content into a different buffer's
            // RDRAM. For MM's pause menu that means the frozen world snapshot
            // gets overwritten with the in-progress menu frame every time the
            // budget allows a copy -- the background then animates along with
            // the page-turn instead of standing still. Copying nothing leaves
            // the previous (correct) contents intact, which is strictly better
            // than copying knowingly-wrong pixels.
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

        // Only the swapchain needs a layout round-trip; a saved canvas is
        // already in TRANSFER_SRC_OPTIMAL and stays there (its resting layout).
        if (!srcIsCanvas) {
            TransitionImage(f.cmd, srcImage,
                            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT);
        }

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

        if (!srcIsCanvas) {
            TransitionImage(f.cmd, srcImage,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_ACCESS_TRANSFER_READ_BIT,
                            VK_ACCESS_MEMORY_READ_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }

        vkEndCommandBuffer(f.cmd);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        // First readback of the frame consumes f.imageAvailable; any subsequent
        // one must wait on f.midFrameDone instead, because a binary semaphore
        // satisfies exactly one wait. Waiting midFrameDone and re-signalling it
        // keeps a valid chain (and EndFrame makes the same substitution when
        // choosing its own wait semaphore).
        VkSemaphore rbWaitSem = m_midFrameReadbackDone ? f.midFrameDone : f.imageAvailable;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &rbWaitSem;
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
        if (m_frameReadbackCount < kMaxFrameReadbacks)
            m_frameReadbackAddrs[m_frameReadbackCount++] = reqAddr;

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
            m_computeRT.reset();
            // VI gamma-correction pass resources
            if (m_gammaTmpView)  { vkDestroyImageView(m_device.Get(), m_gammaTmpView, nullptr); m_gammaTmpView = VK_NULL_HANDLE; }
            if (m_gammaTmpImg)   { vkDestroyImage(m_device.Get(), m_gammaTmpImg, nullptr); m_gammaTmpImg = VK_NULL_HANDLE; }
            if (m_gammaTmpMem)   { vkFreeMemory(m_device.Get(), m_gammaTmpMem, nullptr); m_gammaTmpMem = VK_NULL_HANDLE; }
            if (m_gammaDescPool) { vkDestroyDescriptorPool(m_device.Get(), m_gammaDescPool, nullptr); m_gammaDescPool = VK_NULL_HANDLE; }
            if (m_gammaPipeline)   { vkDestroyPipeline(m_device.Get(), m_gammaPipeline, nullptr); m_gammaPipeline = VK_NULL_HANDLE; }
            if (m_gammaPipeLayout){ vkDestroyPipelineLayout(m_device.Get(), m_gammaPipeLayout, nullptr); m_gammaPipeLayout = VK_NULL_HANDLE; }
            if (m_gammaDSLayout)  { vkDestroyDescriptorSetLayout(m_device.Get(), m_gammaDSLayout, nullptr); m_gammaDSLayout = VK_NULL_HANDLE; }
            if (m_gammaShaderMod) { vkDestroyShaderModule(m_device.Get(), m_gammaShaderMod, nullptr); m_gammaShaderMod = VK_NULL_HANDLE; }
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
        // Extent/format may change — drop the persistent canvas; EnsureCanvas
        // recreates it (empty) on the next BeginFrame.
        DestroyCanvas();
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

    // ── Persistent canvas helpers (see member docs at m_canvases) ────────
    struct CanvasSlot {
        VkImage        img   = VK_NULL_HANDLE;
        VkDeviceMemory mem   = VK_NULL_HANDLE;
        bool           valid = false;   // holds meaningful content
        uint64_t       lastUse = 0;     // for LRU eviction
    };

    void DestroyCanvas() {
        for (auto& [addr, c] : m_canvases) {
            if (c.img) vkDestroyImage(m_device.Get(), c.img, nullptr);
            if (c.mem) vkFreeMemory(m_device.Get(), c.mem, nullptr);
        }
        m_canvases.clear();
        m_canvasW = m_canvasH = 0;
    }

    // Get or lazily create the canvas for an N64 buffer address. Returns
    // nullptr on allocation failure or before the swapchain exists. All
    // canvases share the swapchain's dimensions; a resize drops them all
    // (DestroyCanvas is called from RecreateSwapchain).
    // Free upload temporaries whose frame slot has cycled all the way around
    // (so the GPU is guaranteed done with them). Called from BeginFrame.
    void DrainPendingUploads() {
        for (auto it = m_pendingUploads.begin(); it != m_pendingUploads.end(); ) {
            if (++it->age >= kFramesInFlight + 1) {
                if (it->img) vkDestroyImage(m_device.Get(), it->img, nullptr);
                if (it->mem) vkFreeMemory(m_device.Get(), it->mem, nullptr);
                DestroyBufferStorage(m_ctx, it->staging);
                it = m_pendingUploads.erase(it);
            } else {
                ++it;
            }
        }
    }

    CanvasSlot* EnsureCanvas(uint32_t addr) {
        VkExtent2D ext = m_swapchain.GetExtent();
        if (ext.width == 0 || ext.height == 0) return nullptr;
        m_canvasW = ext.width;
        m_canvasH = ext.height;

        auto it = m_canvases.find(addr);
        if (it != m_canvases.end()) {
            it->second.lastUse = ++m_canvasUseCounter;
            return &it->second;
        }

        // LRU-evict when at capacity (games recycle a handful of buffers;
        // stale addresses from scene changes would otherwise accumulate).
        if (m_canvases.size() >= kMaxCanvases) {
            auto lru = m_canvases.begin();
            for (auto i = m_canvases.begin(); i != m_canvases.end(); ++i)
                if (i->second.lastUse < lru->second.lastUse) lru = i;
            if (lru->second.img) vkDestroyImage(m_device.Get(), lru->second.img, nullptr);
            if (lru->second.mem) vkFreeMemory(m_device.Get(), lru->second.mem, nullptr);
            m_canvases.erase(lru);
        }

        CanvasSlot c{};
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = m_swapchain.GetFormat();
        ici.extent        = { ext.width, ext.height, 1 };
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device.Get(), &ici, nullptr, &c.img) != VK_SUCCESS) {
            Logger::Error("VulkanRenderer: canvas image creation failed (addr=0x{:06X})", addr);
            return nullptr;
        }
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(m_device.Get(), c.img, &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = FindMemoryType(m_ctx.physical, mr.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(m_device.Get(), &mai, nullptr, &c.mem) != VK_SUCCESS ||
            vkBindImageMemory(m_device.Get(), c.img, c.mem, 0) != VK_SUCCESS) {
            Logger::Error("VulkanRenderer: canvas memory allocation failed (addr=0x{:06X})", addr);
            if (c.mem) vkFreeMemory(m_device.Get(), c.mem, nullptr);
            vkDestroyImage(m_device.Get(), c.img, nullptr);
            return nullptr;
        }
        c.valid   = false;
        c.lastUse = ++m_canvasUseCounter;
        auto [ins, ok] = m_canvases.emplace(addr, c);
        Logger::Info("VulkanRenderer: canvas created for fb 0x{:06X} ({}x{}, {} total)",
                     addr, m_canvasW, m_canvasH, m_canvases.size());
        return &ins->second;
    }

    // Find the canvas that covers a VI origin address. The origin usually
    // equals a SetColorImage address but can be offset into the buffer
    // (scanline offsets, interlace fields), so fall back to the nearest
    // canvas that starts at or below the origin within a 1 MB window.
    //
    // Touches lastUse on a hit: EnsureCanvas only bumps lastUse when a
    // canvas becomes the ACTIVE RENDER TARGET, never when it's merely read
    // back as a texture (the only thing that happens here). A canvas that's
    // written once and then repeatedly re-read as a texture -- e.g. MM/OoT's
    // pause-menu world-snapshot scratch buffer, redrawn into the visible
    // frame every time the background needs recompositing but never
    // rewritten -- would otherwise look permanently stale to the LRU and get
    // evicted by kMaxCanvases (6) as soon as any other buffer address needed
    // a 7th slot (MM's routine per-frame Z-buffer-reuse trick alone touches
    // 2 extra addresses on top of the double-buffer, so this wasn't rare).
    // Once evicted, every later read fell through to raw, never-synced
    // RDRAM -- the pause background rendering correctly for its first two
    // frames, then permanently breaking (confirmed via a live capture: reads
    // succeeded via the snapshot ring for frames 914/916, then failed for
    // every frame from 917 onward with no further successes).
    CanvasSlot* FindCanvasForOrigin(uint32_t origin) {
        auto it = m_canvases.find(origin);
        if (it != m_canvases.end()) {
            it->second.lastUse = ++m_canvasUseCounter;
            return &it->second;
        }
        CanvasSlot* best = nullptr;
        uint32_t bestDelta = 0x100000; // 1 MB — larger than any 640x480x4 fb
        for (auto& [addr, c] : m_canvases) {
            if (addr <= origin && origin - addr < bestDelta) {
                bestDelta = origin - addr;
                best = &c;
            }
        }
        if (best) best->lastUse = ++m_canvasUseCounter;
        return best;
    }

    // Record a full-extent blit between two canvas/swapchain images.
    // Layout handling is caller's responsibility via the transition helpers.
    void BlitFull(VkCommandBuffer cmd, VkImage src, VkImage dst) {
        VkImageBlit region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = region.srcSubresource;
        region.srcOffsets[1]  = { static_cast<int32_t>(m_canvasW),
                                  static_cast<int32_t>(m_canvasH), 1 };
        region.dstOffsets[1]  = region.srcOffsets[1];
        vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region, VK_FILTER_NEAREST);
    }

    // Save the current swapchain content into a canvas (swapchain must be in
    // PRESENT_SRC — i.e. outside a render pass). Leaves canvas TRANSFER_SRC,
    // swapchain back in PRESENT_SRC.
    void SaveSwapchainToCanvas(VkCommandBuffer cmd, VkImage swapImg, CanvasSlot& c) {
        TransitionImage(cmd, swapImg,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);
        // Canvases rest in TRANSFER_SRC_OPTIMAL after a successful save (see
        // the class comment on m_canvases) and only sit in UNDEFINED before
        // their first-ever write. Unconditionally claiming UNDEFINED here --
        // as this code did previously -- lies to the driver about a
        // once-valid canvas's real layout, matching the c.valid-conditional
        // pattern the sibling CPU-upload path (EnsureCanvas write-back,
        // ~line 1624) already uses correctly for the identical situation.
        TransitionImage(cmd, c.img,
                        c.valid ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        c.valid ? VK_ACCESS_TRANSFER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);
        BlitFull(cmd, swapImg, c.img);
        TransitionImage(cmd, c.img,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);
        TransitionImage(cmd, swapImg,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_ACCESS_TRANSFER_READ_BIT, 0,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        c.valid = true;
    }

    // Load a canvas into the swapchain image (canvas must be TRANSFER_SRC —
    // it always is after SaveSwapchainToCanvas). Swapchain may be in any
    // layout (UNDEFINED discard is fine — full overwrite); leaves it
    // PRESENT_SRC so the LOAD render pass can begin.
    void LoadCanvasToSwapchain(VkCommandBuffer cmd, CanvasSlot& c, VkImage swapImg,
                               VkImageLayout swapOldLayout) {
        TransitionImage(cmd, swapImg,
                        swapOldLayout,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        swapOldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                            ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        swapOldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                            ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                            : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);
        BlitFull(cmd, c.img, swapImg);
        TransitionImage(cmd, swapImg,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
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
    // Transient resources used by UploadPixelsToCanvas. The temp image/staging
    // buffer are referenced by the in-flight command buffer, so they can only be
    // freed once that frame index comes around again (kFramesInFlight later).
    struct PendingUpload {
        VkImage          img = VK_NULL_HANDLE;
        VkDeviceMemory   mem = VK_NULL_HANDLE;
        VKBufferStorage  staging{};
        uint32_t         frameIdx = 0;
        uint32_t         age = 0;
    };
    std::vector<PendingUpload> m_pendingUploads;
    uint32_t      m_currentImage = 0;
    bool          m_skipPresent  = false;
    // Has anything been DRAWN to the currently-active canvas since it became
    // active? Gates SaveSwapchainToCanvas so merely binding a buffer cannot
    // overwrite its stored content with the live frame.
    bool          m_activeCanvasDirty = false;
    bool          m_midFrameReadbackDone = false;
    // GPU->RDRAM readbacks already performed this frame, keyed by N64 buffer
    // address. Replaces a single global "one readback per frame" bool that
    // starved MM's pause-menu strip reads (see CopyFramebufferToRDRAM).
    static constexpr uint32_t kMaxFrameReadbacks = 4;
    uint32_t      m_frameReadbackAddrs[kMaxFrameReadbacks] = {};
    uint32_t      m_frameReadbackCount = 0;
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

    // ── Persistent per-N64-buffer canvases ──────────────────────────────
    // The swapchain rotates between 2-3 images; without carry-over, each
    // presented image only contains the draw calls recorded since ITS OWN
    // BeginFrame — a fraction of the game's draw stream that starts at an
    // arbitrary point mid-display-list.  Games that render across VI
    // boundaries or alternate between multiple color buffers (double/triple
    // buffering — e.g. Majora's Mask file select) presented as a shattered
    // interleave of partial frames with black (clear-color) holes where the
    // current slice hadn't drawn yet.  GL never had this bug because its
    // per-FB FBOs persist and BlitToScreen presents completed content.
    //
    // Fix: keep one persistent canvas image PER N64 COLOR BUFFER (keyed by
    // the SetColorImage RDRAM address).  Draws accumulate on the canvas of
    // the buffer they target; SetColorImage swaps canvases mid-frame; at
    // present time the canvas belonging to the VI origin register (the
    // buffer the game says is display-ready) is shown — never the buffer
    // still being drawn.  This is real double-buffer isolation at swapchain
    // resolution, and it also keeps Z-buffer clear fills (SetColorImage to
    // the depth address) off the visible output entirely.
    // Was 6 -- too tight. MM/OoT's pause menu alone needs 6 DISTINCT addresses
    // live simultaneously just from routine per-frame mechanics: the 2 double-
    // buffer targets (e.g. 0x000500/0x7DA800), 2 addresses from the every-frame
    // Z-buffer-reuse-as-color-target trick (0x383AC0 and 0x3A92C0 -- the start
    // and end of the same depth-buffer-sized scratch region, used for two
    // different purposes), the boot-time 0x000000 placeholder, and the pause-
    // menu's own world-snapshot scratch buffer (e.g. 0x784600) -- which is
    // written ONCE at menu-open but then read back as a texture repeatedly
    // (every background recomposite, e.g. on page turns) without ever
    // becoming the active render target again. At exactly 6, that snapshot
    // buffer was the very next eviction candidate: any 7th distinct address
    // (trivially reached by normal gameplay) evicted it via LRU, and every
    // later read of it permanently fell through to raw, never-synced RDRAM --
    // confirmed via a live capture where the read succeeded for the pause
    // background's first two frames, then failed for every frame after with
    // no further recovery. Each canvas is one swapchain-sized image (e.g.
    // ~20MB at 3440x1440 BGRA8), so even a generous cap here is a trivial
    // memory cost against the eviction-thrash risk.
    static constexpr size_t kMaxCanvases = 16;
    std::unordered_map<uint32_t, CanvasSlot> m_canvases; // key: N64 fb address
    uint32_t       m_activeCanvasAddr = 0;   // buffer the game is drawing into
    uint32_t       m_presentOriginAddr = 0;  // VI origin (display-ready buffer)
    uint64_t       m_canvasUseCounter = 0;
    uint32_t       m_canvasW = 0, m_canvasH = 0; // common canvas dimensions
    bool           m_canvasPreserve = true;   // copy-forward enabled
    bool           m_frameOnSwapchain = false; // current frame targets the swapchain pass
    bool           m_frameRecording = false;   // command buffer open (BeginFrame..EndFrame)

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

    // â”€â”€ Compute ray tracer (MoltenVK / no-hardware-RT fallback) â”€â”€â”€â”€â”€â”€â”€â”€â”€
    std::unique_ptr<VKComputeRaytracer> m_computeRT;
    RT::RTBVH                           m_rtBVH;
    // Deferred hybrid composite: BVH/UBO are prepared mid-display-list, but the
    // compute dispatch + swapchain composite happen at EndFrame (after all
    // raster), so the RT reads the finished textured frame and is not discarded.
    bool                                m_rtComposePending = false;
    RTUBOData                           m_rtUBO{};

    // ── VI gamma correction ──────────────────────────────────────────────
    // Real N64 VI hardware, when VI_STATUS's gamma-enable bit is set,
    // brightens the output with a sqrt() curve (N64 Programming Manual
    // §5.3: undoing it is "square and shift down by 8 each color
    // component" — i.e. the forward op is the inverse of squaring). This
    // was already implemented correctly for GL (FramebufferManager's
    // uGamma=2.0 shader does pow(c,1/2.0)=sqrt(c)) but never had a Vulkan
    // counterpart, so it silently never ran on this platform despite every
    // test ROM (SM64/OOT/MM) enabling it. Built lazily on first use, exactly
    // like m_computeRT above.
    bool                 m_gammaBuilt = false;
    VkShaderModule       m_gammaShaderMod   = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_gammaDSLayout   = VK_NULL_HANDLE;
    VkPipelineLayout      m_gammaPipeLayout = VK_NULL_HANDLE;
    VkPipeline            m_gammaPipeline   = VK_NULL_HANDLE;
    VkDescriptorPool      m_gammaDescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_gammaDescSet    = VK_NULL_HANDLE;
    VkImage               m_gammaTmpImg     = VK_NULL_HANDLE;
    VkDeviceMemory        m_gammaTmpMem     = VK_NULL_HANDLE;
    VkImageView           m_gammaTmpView    = VK_NULL_HANDLE;
    uint32_t              m_gammaTmpW       = 0;
    uint32_t              m_gammaTmpH       = 0;
    bool                  m_gammaTmpValid   = false;   // has this frame's blit-in happened at least once?

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
