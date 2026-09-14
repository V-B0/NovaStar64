/**
 * @file VKComputeRaytracer.hpp
 * @brief Two-pass compute-shader ray tracer for MoltenVK / Apple Silicon.
 *
 * MoltenVK does not expose VK_KHR_ray_tracing_pipeline, so this traces a
 * CPU-built BVH (RTBVH) in plain Vulkan compute (see RTComputeShaders.hpp).
 *
 * Pass 1 (trace, HALF res):   primary ray + soft-shadow rays + stochastic AO,
 *                             written to intermediate images (lit/occ/G-buffer).
 * Pass 2 (denoise, HALF res): bilateral blur + RT64-style temporal accumulation
 *                             of the shadow/AO term (history image).
 * Pass 3 (composite, FULL res): joint bilateral upsample of the RT term +
 *                             directional wrap diffuse, over the full-res raster.
 * Half-res tracing is the performance core: ¼ of the BVH rays and filter taps.
 *
 * Self-contained: owns descriptor set layouts, both pipelines, descriptor pool,
 * and PER-FRAME-IN-FLIGHT copies of all buffers/images (so the CPU never
 * overwrites a buffer the GPU is still reading). The caller composites onto the
 * swapchain via RenderHybrid().
 */
// SPDX-License-Identifier: MIT
#pragma once

#include "VKMemory.hpp"
#include "VKBuffer.hpp"
#include "../../RT/RTBVH.hpp"

#include <cstdint>

namespace Nova64 { namespace Video { namespace VK {

// Mirrors the std140 UBO in the RT compute shaders.
struct RTUBOData {
    float    invProj[16];     // clip -> eye, pre-transposed for column-vector GLSL
    float    lightDir[4];     // xyz eye-space dir TO light, w = sun angular size (0=off)
    float    lightColor[4];   // rgb, w = frame counter (temporal sampling rotation)
    float    ambient[4];      // denoise params: x=blur radius(px), y=normal edge
                              // power, z=depth edge threshold, w=ambient level
    float    viewport[4];     // N64 viewport {scaleX, transX, scaleY, transY}
                              // (fb-pixel units; rays must invert this mapping)
    float    extra[4];        // x = temporal history cap /64 (camera motion),
                              // y = exposure, z/w = reserved
    float    proj[16];        // eye -> clip (FORWARD; row-major flatten like
                              // invProj — used to predict raster depth values)
    float    misc[4];         // x = scene AABB diagonal, y/z/w = reserved
    uint32_t dims[4];         // x=width y=height z=nodeCount w=featureFlags
};

// Feature flags for RTUBOData::dims.w (must match RTComputeShaders.hpp).
enum RTFeatureFlags : uint32_t {
    RT_F_SHADOWS     = 1u << 0,
    RT_F_LIGHTING    = 1u << 1,
    RT_F_REFLECTIONS = 1u << 2,
    RT_F_DEBUG       = 1u << 3,   // normal-as-color + grid (pass/wiring check)
    RT_F_DEBUG_TERM  = 1u << 4,   // lighting-term-only view (alignment check)
    RT_F_DEBUG_COVERED = 1u << 5, // raster-depth-guard view: red=suppressed, green=RT applied, black=no hit
};

class VKComputeRaytracer {
public:
    // Must match VKBackend's kFramesInFlight.
    static constexpr uint32_t kFrames = 2;

    /// One-time setup: compile both compute shaders, create both pipelines,
    /// descriptor layouts/pool and per-frame descriptor sets/UBOs.
    bool Build(const VKContext& ctx);

    /// Release all GPU resources.
    void Destroy(const VKContext& ctx);

    /// Hybrid composite for frame `frameIdx`, recorded entirely into `cmd`:
    ///   1. copy the swapchain (textured raster, in PRESENT_SRC) into a private
    ///      copy the compute shaders can read,
    ///   2. dispatch pass 1 (trace) writing litColor/occ/G-buffer,
    ///   3. dispatch pass 2 (denoise+composite) writing the output image,
    ///   4. blit the output back onto the swapchain.
    /// The swapchain is left in PRESENT_SRC_KHR. No-op (false) if not built or
    /// the BVH is empty. `w`/`h` must equal the swapchain extent.
    /// `depthImg`/`depthFormat`: the raster's depth attachment (already in
    /// SHADER_READ_ONLY_OPTIMAL — the caller transitions it) — sampled by the
    /// composite to suppress RT terms where the raster drew a closer surface
    /// (ray-invisible billboards).
    bool RenderHybrid(const VKContext& ctx, VkCommandBuffer cmd, uint32_t frameIdx,
                      const RT::RTBVH& bvh, const RTUBOData& ubo,
                      VkImage swapchainImg, uint32_t w, uint32_t h,
                      VkImage depthImg, VkFormat depthFormat);

    bool Ready() const {
        return m_tracePipeline != VK_NULL_HANDLE
            && m_denoisePipeline != VK_NULL_HANDLE
            && m_compositePipeline != VK_NULL_HANDLE;
    }

private:
    // One storage image + memory + view.
    struct Img {
        VkImage        image = VK_NULL_HANDLE;
        VkDeviceMemory mem   = VK_NULL_HANDLE;
        VkImageView    view  = VK_NULL_HANDLE;
    };

    struct FrameRes {
        VKBufferStorage ubo{};
        VKBufferStorage nodeBuf{};
        VKBufferStorage triBuf{};
        VkDeviceSize    nodeCap = 0;
        VkDeviceSize    triCap  = 0;
        Img             out{};       // FULL res: final composite (blit to swapchain)
        Img             ras{};       // FULL res: copy of the raster render
        Img             lit{};       // HALF res: additive reflection extras
        Img             shd{};       // HALF res: baked vertex shade + lit flag
        Img             occ{};       // HALF res: r=shadow g=ao b=reflMix a=hit
        Img             gbuf{};      // HALF res: rgb=smooth normal, a=depth (rgba16f)
        Img             hist{};      // HALF res: temporal accum (r=shadow,g=ao,a=len/64)
        Img             gi{};        // HALF res: raw one-bounce irradiance
        Img             giHist{};    // HALF res: accumulated irradiance
        uint32_t        w = 0, h = 0;
        bool            outInGeneral = false;
        bool            imagesFresh  = false;  // just (re)created → need init/clear
        bool            descDirty    = true;
        VkDescriptorSet traceSet     = VK_NULL_HANDLE;
        VkDescriptorSet denoiseSet   = VK_NULL_HANDLE;
        VkDescriptorSet compositeSet = VK_NULL_HANDLE;
    };

    bool EnsureBuffers(const VKContext& ctx, FrameRes& fr,
                       VkDeviceSize nodeBytes, VkDeviceSize triBytes);
    bool EnsureImages(const VKContext& ctx, FrameRes& fr, uint32_t w, uint32_t h);
    // `prev` = the other frame-in-flight; its hist/gbuf feed the temporal
    // accumulator (bindings 5/6 of the denoise set).
    void WriteDescriptors(const VKContext& ctx, FrameRes& fr, const FrameRes& prev);
    void DestroyImg(const VKContext& ctx, Img& im);
    // Depth-only sampled view over the raster's depth attachment (recreated
    // when the swapchain depth image changes).
    bool EnsureDepthView(const VKContext& ctx, VkImage depthImg, VkFormat fmt);

    VkSampler   m_depthSampler  = VK_NULL_HANDLE;
    VkImage     m_cachedDepth   = VK_NULL_HANDLE;
    VkImageView m_depthView     = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_traceDsLayout     = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_denoiseDsLayout   = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_compositeDsLayout = VK_NULL_HANDLE;
    VkPipelineLayout      m_tracePipeLayout     = VK_NULL_HANDLE;
    VkPipelineLayout      m_denoisePipeLayout   = VK_NULL_HANDLE;
    VkPipelineLayout      m_compositePipeLayout = VK_NULL_HANDLE;
    VkPipeline            m_tracePipeline     = VK_NULL_HANDLE;
    VkPipeline            m_denoisePipeline   = VK_NULL_HANDLE;
    VkPipeline            m_compositePipeline = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool              = VK_NULL_HANDLE;

    FrameRes m_frame[kFrames];
};

}}} // namespace Nova64::Video::VK
