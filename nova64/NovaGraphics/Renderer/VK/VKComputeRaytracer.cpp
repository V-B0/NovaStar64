/**
 * @file VKComputeRaytracer.cpp
 * @brief Two-pass compute BVH ray tracer implementation (MoltenVK-compatible).
 *
 * Hybrid: reads the textured raster render, traces shadows/AO/reflections into
 * intermediate images (pass 1), edge-aware-denoises and composites (pass 2),
 * blits the result back to the swapchain.
 */
// SPDX-License-Identifier: MIT
#include "VKComputeRaytracer.hpp"
#include "VKShader.hpp"
#include "../../RT/RTComputeShaders.hpp"
#include "../../Common/Logger.hpp"

#include <array>
#include <cstring>
#include <algorithm>
#include <vector>

namespace Nova64 { namespace Video { namespace VK {

namespace {
constexpr VkDeviceSize kMinBuf = 256;

void ImgBarrier(VkCommandBuffer cmd, VkImage img,
                VkImageLayout oldL, VkImageLayout newL,
                VkAccessFlags srcA, VkAccessFlags dstA,
                VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout           = oldL;
    b.newLayout           = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = img;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask       = srcA;
    b.dstAccessMask       = dstA;
    vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool CreateStorageImage(const VKContext& ctx, uint32_t w, uint32_t h,
                        VkFormat format, VkImageUsageFlags extraUsage,
                        VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = format;
    ii.extent      = {w, h, 1};
    ii.mipLevels   = 1;
    ii.arrayLayers = 1;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.usage       = VK_IMAGE_USAGE_STORAGE_BIT | extraUsage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx.device, &ii, nullptr, &img) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(ctx.device, img, &req);
    uint32_t typeIdx = FindMemoryType(ctx.physical, req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (typeIdx == UINT32_MAX) return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = typeIdx;
    if (vkAllocateMemory(ctx.device, &ai, nullptr, &mem) != VK_SUCCESS) return false;
    vkBindImageMemory(ctx.device, img, mem, 0);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(ctx.device, &vi, nullptr, &view) == VK_SUCCESS;
}

void FullBlit(VkCommandBuffer cmd, VkImage src, VkImageLayout srcL,
              VkImage dst, VkImageLayout dstL, uint32_t w, uint32_t h) {
    VkImageBlit b{};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1]  = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstOffsets[1]  = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
    vkCmdBlitImage(cmd, src, srcL, dst, dstL, 1, &b, VK_FILTER_NEAREST);
}
} // namespace

bool VKComputeRaytracer::Build(const VKContext& ctx) {
    if (Ready()) return true;

    // ── Compile the three compute shaders ────────────────────────────────────
    GlslangInitProcess();
    std::vector<uint32_t> traceSpv =
        CompileGLSLtoSPIRV(ShaderStage::Compute, RT::kRTTraceSrc, "rt_trace");
    std::vector<uint32_t> denoiseSpv =
        CompileGLSLtoSPIRV(ShaderStage::Compute, RT::kRTDenoiseSrc, "rt_denoise");
    std::vector<uint32_t> compositeSpv =
        CompileGLSLtoSPIRV(ShaderStage::Compute, RT::kRTCompositeSrc, "rt_composite");
    GlslangFinalizeProcess();
    if (traceSpv.empty() || denoiseSpv.empty() || compositeSpv.empty()) {
        Logger::Error("VKComputeRaytracer: shader compilation failed (trace={} denoise={} composite={})",
                      traceSpv.size(), denoiseSpv.size(), compositeSpv.size());
        return false;
    }

    auto makeModule = [&](const std::vector<uint32_t>& spv) -> VkShaderModule {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = spv.size() * sizeof(uint32_t);
        mi.pCode    = spv.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(ctx.device, &mi, nullptr, &m);
        return m;
    };
    VkShaderModule traceMod     = makeModule(traceSpv);
    VkShaderModule denoiseMod   = makeModule(denoiseSpv);
    VkShaderModule compositeMod = makeModule(compositeSpv);
    auto cleanupModules = [&]() {
        if (traceMod)     vkDestroyShaderModule(ctx.device, traceMod, nullptr);
        if (denoiseMod)   vkDestroyShaderModule(ctx.device, denoiseMod, nullptr);
        if (compositeMod) vkDestroyShaderModule(ctx.device, compositeMod, nullptr);
    };
    if (traceMod == VK_NULL_HANDLE || denoiseMod == VK_NULL_HANDLE ||
        compositeMod == VK_NULL_HANDLE) {
        Logger::Error("VKComputeRaytracer: vkCreateShaderModule failed");
        cleanupModules();
        return false;
    }

    // ── Descriptor set layouts ───────────────────────────────────────────────
    // Pass 1 (trace, half):     0 UBO, 1 nodes SSBO, 2 tris SSBO,
    //                           3 lit(w), 4 occ(w), 5 gbuf(w).
    // Pass 2 (denoise, half):   0 UBO, 1 occ(r), 2 gbuf(r),
    //                           3 prevHist(r), 4 prevGbuf(r), 5 hist(w).
    // Pass 3 (composite, full): 0 UBO, 1 ras(r), 2 lit(r), 3 occ(r),
    //                           4 hist(r), 5 gbuf(r), 6 out(w).
    auto makeLayout = [&](const std::vector<VkDescriptorType>& types)
            -> VkDescriptorSetLayout {
        std::vector<VkDescriptorSetLayoutBinding> bind(types.size());
        for (uint32_t i = 0; i < types.size(); ++i) {
            bind[i].binding         = i;
            bind[i].descriptorType  = types[i];
            bind[i].descriptorCount = 1;
            bind[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dsli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dsli.bindingCount = static_cast<uint32_t>(bind.size());
        dsli.pBindings    = bind.data();
        VkDescriptorSetLayout l = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(ctx.device, &dsli, nullptr, &l);
        return l;
    };
    m_traceDsLayout = makeLayout({
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  // 1 nodes
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  // 2 tris
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 3 lit
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 4 occ
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 5 gbuf
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 6 shd
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 7 gi
    });
    m_denoiseDsLayout = makeLayout({
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 1 occ
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 2 gbuf
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 3 prev hist
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 4 prev gbuf
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 5 hist
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 6 gi
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 7 prev giHist
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 8 giHist
    });
    m_compositeDsLayout = makeLayout({
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 1 ras
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 2 lit
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 3 occ
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 4 hist
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 5 gbuf
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 6 shd
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 7 giHist
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   // 8 out
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // 9 raster depth
    });
    if (m_traceDsLayout == VK_NULL_HANDLE || m_denoiseDsLayout == VK_NULL_HANDLE ||
        m_compositeDsLayout == VK_NULL_HANDLE) {
        Logger::Error("VKComputeRaytracer: descriptor set layout failed");
        cleanupModules();
        return false;
    }

    // ── Pipeline layouts + pipelines ─────────────────────────────────────────
    auto makePipeline = [&](VkDescriptorSetLayout dsl, VkShaderModule mod,
                            VkPipelineLayout& layoutOut) -> VkPipeline {
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts    = &dsl;
        if (vkCreatePipelineLayout(ctx.device, &pli, nullptr, &layoutOut) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        VkPipelineShaderStageCreateInfo ss{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ss.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        ss.module = mod;
        ss.pName  = "main";
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage  = ss;
        cpi.layout = layoutOut;
        VkPipeline p = VK_NULL_HANDLE;
        vkCreateComputePipelines(ctx.device, ctx.pipelineCache, 1, &cpi, nullptr, &p);
        return p;
    };
    m_tracePipeline     = makePipeline(m_traceDsLayout,     traceMod,     m_tracePipeLayout);
    m_denoisePipeline   = makePipeline(m_denoiseDsLayout,   denoiseMod,   m_denoisePipeLayout);
    m_compositePipeline = makePipeline(m_compositeDsLayout, compositeMod, m_compositePipeLayout);
    cleanupModules();
    if (m_tracePipeline == VK_NULL_HANDLE || m_denoisePipeline == VK_NULL_HANDLE ||
        m_compositePipeline == VK_NULL_HANDLE) {
        Logger::Error("VKComputeRaytracer: vkCreateComputePipelines failed");
        return false;
    }

    // ── Descriptor pool + per-frame sets/UBOs ────────────────────────────────
    // Per frame: trace(1 UBO, 2 SSBO, 5 img) + denoise(1 UBO, 8 img)
    //          + composite(1 UBO, 8 img) = 3 UBO, 2 SSBO, 21 images.
    std::array<VkDescriptorPoolSize, 4> ps{{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         3 * kFrames},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         2 * kFrames},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,         24 * kFrames},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 * kFrames},
    }};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets       = 3 * kFrames;
    dpi.poolSizeCount = static_cast<uint32_t>(ps.size());
    dpi.pPoolSizes    = ps.data();
    if (vkCreateDescriptorPool(ctx.device, &dpi, nullptr, &m_pool) != VK_SUCCESS) {
        Logger::Error("VKComputeRaytracer: descriptor pool failed");
        return false;
    }
    for (uint32_t i = 0; i < kFrames; ++i) {
        std::array<VkDescriptorSetLayout, 3> layouts{
            m_traceDsLayout, m_denoiseDsLayout, m_compositeDsLayout};
        std::array<VkDescriptorSet, 3> sets{};
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool     = m_pool;
        dsai.descriptorSetCount = 3;
        dsai.pSetLayouts        = layouts.data();
        if (vkAllocateDescriptorSets(ctx.device, &dsai, sets.data()) != VK_SUCCESS) {
            Logger::Error("VKComputeRaytracer: descriptor set alloc failed");
            return false;
        }
        m_frame[i].traceSet     = sets[0];
        m_frame[i].denoiseSet   = sets[1];
        m_frame[i].compositeSet = sets[2];
        if (!CreateBufferStorage(ctx, sizeof(RTUBOData),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                m_frame[i].ubo)) {
            Logger::Error("VKComputeRaytracer: UBO buffer alloc failed");
            return false;
        }
    }

    // Nearest sampler for the raster-depth comparison (no filtering wanted).
    {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter    = VK_FILTER_NEAREST;
        si.minFilter    = VK_FILTER_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(ctx.device, &si, nullptr, &m_depthSampler) != VK_SUCCESS) {
            Logger::Error("VKComputeRaytracer: depth sampler creation failed");
            return false;
        }
    }

    Logger::Info("VKComputeRaytracer: built (half-res trace+denoise + full-res composite, {} frames)", kFrames);
    return true;
}

bool VKComputeRaytracer::EnsureDepthView(const VKContext& ctx, VkImage depthImg, VkFormat fmt) {
    if (depthImg == m_cachedDepth && m_depthView != VK_NULL_HANDLE) return true;
    if (m_depthView) { vkDestroyImageView(ctx.device, m_depthView, nullptr); m_depthView = VK_NULL_HANDLE; }
    m_cachedDepth = depthImg;
    // Depth-only aspect: sampling a D24S8/D32 image requires a single aspect.
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = depthImg;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(ctx.device, &vi, nullptr, &m_depthView) != VK_SUCCESS) {
        Logger::Error("VKComputeRaytracer: depth view creation failed");
        m_cachedDepth = VK_NULL_HANDLE;
        return false;
    }
    m_frame[0].descDirty = true;
    m_frame[1].descDirty = true;
    return true;
}

bool VKComputeRaytracer::EnsureBuffers(const VKContext& ctx, FrameRes& fr,
                                       VkDeviceSize nodeBytes, VkDeviceSize triBytes) {
    nodeBytes = std::max(nodeBytes, kMinBuf);
    triBytes  = std::max(triBytes,  kMinBuf);
    auto grow = [&](VKBufferStorage& buf, VkDeviceSize& cap, VkDeviceSize need) -> bool {
        if (buf.buffer != VK_NULL_HANDLE && cap >= need) return true;
        if (buf.buffer != VK_NULL_HANDLE) DestroyBufferStorage(ctx, buf);
        VkDeviceSize newCap = std::max<VkDeviceSize>(need, cap ? cap * 2 : need);
        if (!CreateBufferStorage(ctx, newCap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                buf)) {
            Logger::Error("VKComputeRaytracer: storage buffer alloc failed");
            return false;
        }
        cap = newCap;
        fr.descDirty = true;
        return true;
    };
    return grow(fr.nodeBuf, fr.nodeCap, nodeBytes) && grow(fr.triBuf, fr.triCap, triBytes);
}

void VKComputeRaytracer::DestroyImg(const VKContext& ctx, Img& im) {
    if (im.view)  vkDestroyImageView(ctx.device, im.view, nullptr);
    if (im.image) vkDestroyImage(ctx.device, im.image, nullptr);
    if (im.mem)   vkFreeMemory(ctx.device, im.mem, nullptr);
    im = Img{};
}

bool VKComputeRaytracer::EnsureImages(const VKContext& ctx, FrameRes& fr,
                                      uint32_t w, uint32_t h) {
    if (fr.out.image != VK_NULL_HANDLE && fr.w == w && fr.h == h) return true;
    DestroyImg(ctx, fr.out);
    DestroyImg(ctx, fr.ras);
    DestroyImg(ctx, fr.lit);
    DestroyImg(ctx, fr.shd);
    DestroyImg(ctx, fr.occ);
    DestroyImg(ctx, fr.gbuf);
    DestroyImg(ctx, fr.hist);
    DestroyImg(ctx, fr.gi);
    DestroyImg(ctx, fr.giHist);

    // RT intermediates run at HALF resolution (¼ of the rays/filter taps);
    // out/ras stay full so the game's raster image is never downscaled.
    const uint32_t rw = (w + 1) / 2;
    const uint32_t rh = (h + 1) / 2;

    // Output: storage (composite writes) + transfer-src (blit to swapchain).
    if (!CreateStorageImage(ctx, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            fr.out.image, fr.out.mem, fr.out.view)) return false;
    // Raster copy: storage (composite reads) + transfer-dst (blit from swapchain).
    if (!CreateStorageImage(ctx, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            fr.ras.image, fr.ras.mem, fr.ras.view)) return false;
    // Pass-1 intermediates (half res, pure storage).
    if (!CreateStorageImage(ctx, rw, rh, VK_FORMAT_R8G8B8A8_UNORM, 0,
                            fr.lit.image, fr.lit.mem, fr.lit.view)) return false;
    if (!CreateStorageImage(ctx, rw, rh, VK_FORMAT_R8G8B8A8_UNORM, 0,
                            fr.shd.image, fr.shd.mem, fr.shd.view)) return false;
    if (!CreateStorageImage(ctx, rw, rh, VK_FORMAT_R8G8B8A8_UNORM, 0,
                            fr.occ.image, fr.occ.mem, fr.occ.view)) return false;
    // G-buffer needs signed normals + real depth → rgba16f (core-mandated
    // storage-image format, safe on MoltenVK).
    if (!CreateStorageImage(ctx, rw, rh, VK_FORMAT_R16G16B16A16_SFLOAT, 0,
                            fr.gbuf.image, fr.gbuf.mem, fr.gbuf.view)) return false;
    // Temporal history (persists across frames; TRANSFER_DST for the init clear).
    if (!CreateStorageImage(ctx, rw, rh, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            fr.hist.image, fr.hist.mem, fr.hist.view)) return false;
    // One-bounce GI: raw irradiance (transient) + its own history accumulator.
    if (!CreateStorageImage(ctx, rw, rh, VK_FORMAT_R8G8B8A8_UNORM, 0,
                            fr.gi.image, fr.gi.mem, fr.gi.view)) return false;
    if (!CreateStorageImage(ctx, rw, rh, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            fr.giHist.image, fr.giHist.mem, fr.giHist.view)) return false;

    fr.w = w; fr.h = h;
    fr.outInGeneral = false;
    fr.imagesFresh  = true;
    fr.descDirty = true;
    return true;
}

void VKComputeRaytracer::WriteDescriptors(const VKContext& ctx, FrameRes& fr,
                                          const FrameRes& prev) {
    VkDescriptorBufferInfo ubo{fr.ubo.buffer,     0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo nb {fr.nodeBuf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo tb {fr.triBuf.buffer,  0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo  out  {VK_NULL_HANDLE, fr.out.view,    VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo  ras  {VK_NULL_HANDLE, fr.ras.view,    VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo  lit  {VK_NULL_HANDLE, fr.lit.view,    VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo  shd  {VK_NULL_HANDLE, fr.shd.view,    VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo  occ  {VK_NULL_HANDLE, fr.occ.view,    VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo  gbuf {VK_NULL_HANDLE, fr.gbuf.view,   VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo  hist {VK_NULL_HANDLE, fr.hist.view,   VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo  pHist{VK_NULL_HANDLE, prev.hist.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo  pGbuf{VK_NULL_HANDLE, prev.gbuf.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo  gi    {VK_NULL_HANDLE, fr.gi.view,       VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo  giH   {VK_NULL_HANDLE, fr.giHist.view,   VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo  pGiH  {VK_NULL_HANDLE, prev.giHist.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo  dep   {m_depthSampler, m_depthView,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    std::array<VkWriteDescriptorSet, 27> w{};
    uint32_t n = 0;
    auto W = [&](VkDescriptorSet set, uint32_t binding, VkDescriptorType t,
                 VkDescriptorBufferInfo* bi, VkDescriptorImageInfo* ii) {
        w[n] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[n].dstSet          = set;
        w[n].dstBinding      = binding;
        w[n].descriptorCount = 1;
        w[n].descriptorType  = t;
        w[n].pBufferInfo     = bi;
        w[n].pImageInfo      = ii;
        ++n;
    };
    // Pass 1 (trace, half res).
    W(fr.traceSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &ubo, nullptr);
    W(fr.traceSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &nb,  nullptr);
    W(fr.traceSet, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &tb,  nullptr);
    W(fr.traceSet, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &lit);
    W(fr.traceSet, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &occ);
    W(fr.traceSet, 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &gbuf);
    W(fr.traceSet, 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &shd);
    W(fr.traceSet, 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &gi);
    // Pass 2 (denoise, half res). 3/4/7 reference the OTHER frame-in-flight's
    // hist/gbuf/giHist — that ping-pong is the temporal history storage.
    W(fr.denoiseSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &ubo, nullptr);
    W(fr.denoiseSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &occ);
    W(fr.denoiseSet, 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &gbuf);
    W(fr.denoiseSet, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &pHist);
    W(fr.denoiseSet, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &pGbuf);
    W(fr.denoiseSet, 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &hist);
    W(fr.denoiseSet, 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &gi);
    W(fr.denoiseSet, 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &pGiH);
    W(fr.denoiseSet, 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &giH);
    // Pass 3 (composite, full res).
    W(fr.compositeSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &ubo, nullptr);
    W(fr.compositeSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &ras);
    W(fr.compositeSet, 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &lit);
    W(fr.compositeSet, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &occ);
    W(fr.compositeSet, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &hist);
    W(fr.compositeSet, 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &gbuf);
    W(fr.compositeSet, 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &shd);
    W(fr.compositeSet, 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &giH);
    W(fr.compositeSet, 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  nullptr, &out);
    W(fr.compositeSet, 9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &dep);
    vkUpdateDescriptorSets(ctx.device, n, w.data(), 0, nullptr);
}

bool VKComputeRaytracer::RenderHybrid(const VKContext& ctx, VkCommandBuffer cmd, uint32_t frameIdx,
                                      const RT::RTBVH& bvh, const RTUBOData& uboIn,
                                      VkImage swapchainImg, uint32_t w, uint32_t h,
                                      VkImage depthImg, VkFormat depthFormat) {
    if (!Ready() || bvh.Empty() || w == 0 || h == 0) return false;
    if (!EnsureDepthView(ctx, depthImg, depthFormat)) return false;
    FrameRes& fr   = m_frame[frameIdx % kFrames];
    FrameRes& prev = m_frame[(frameIdx + 1) % kFrames];

    const VkDeviceSize nodeBytes = static_cast<VkDeviceSize>(bvh.Nodes().size())     * sizeof(RT::GpuBVHNode);
    const VkDeviceSize triBytes  = static_cast<VkDeviceSize>(bvh.Triangles().size()) * sizeof(RT::GpuRTTri);
    if (!EnsureBuffers(ctx, fr, nodeBytes, triBytes)) return false;
    // Both frames' images must exist (the denoise pass reads the OTHER frame's
    // hist/gbuf as temporal history) and stay the same size.
    if (!EnsureImages(ctx, fr, w, h))   return false;
    if (!EnsureImages(ctx, prev, w, h)) return false;
    if (fr.imagesFresh || prev.imagesFresh) { fr.descDirty = true; prev.descDirty = true; }

    std::memcpy(fr.nodeBuf.mapped, bvh.Nodes().data(),     static_cast<size_t>(nodeBytes));
    std::memcpy(fr.triBuf.mapped,  bvh.Triangles().data(), static_cast<size_t>(triBytes));
    std::memcpy(fr.ubo.mapped,     &uboIn,                 sizeof(RTUBOData));
    if (fr.descDirty) { WriteDescriptors(ctx, fr, prev); fr.descDirty = false; }

    // Initialise freshly created images: intermediates to GENERAL (contents
    // don't matter — history length 0 rejects them), hist cleared to
    // "fully lit, zero history" so the temporal accumulator starts clean.
    for (FrameRes* p : {&fr, &prev}) {
        if (!p->imagesFresh) continue;
        p->imagesFresh = false;
        for (VkImage im : {p->lit.image, p->shd.image, p->occ.image,
                           p->gbuf.image, p->gi.image}) {
            ImgBarrier(cmd, im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                       0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        // Clear both history accumulators: hist to "fully lit, zero history",
        // giHist to neutral irradiance (0.66 = 1.0 after the ×1.5 decode).
        struct { VkImage img; VkClearColorValue cv; } clears[2] = {
            { p->hist.image,   {{1.0f, 1.0f, 0.0f, 0.0f}} },
            { p->giHist.image, {{0.66f, 0.66f, 0.66f, 0.0f}} },
        };
        VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (auto& c : clears) {
            ImgBarrier(cmd, c.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       0, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdClearColorImage(cmd, c.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c.cv, 1, &rng);
            ImgBarrier(cmd, c.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
    }

    // 1) Copy the textured raster (swapchain) into the private raster image.
    ImgBarrier(cmd, swapchainImg, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    ImgBarrier(cmd, fr.ras.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               0, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    FullBlit(cmd, swapchainImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
             fr.ras.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, w, h);

    // 2) raster -> GENERAL (compute read); pass-1 outputs -> GENERAL (write).
    //    lit/occ/gbuf are transient (fully rewritten each frame), so UNDEFINED
    //    old layout (contents discarded) is correct. srcStage must be COMPUTE:
    //    the PREVIOUS frame's denoise pass reads this frame's gbuf as temporal
    //    history, and the discard write must wait for that read (WAR).
    ImgBarrier(cmd, fr.ras.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    const VkImage pass1Out[5] = {fr.lit.image, fr.shd.image, fr.occ.image,
                                 fr.gbuf.image, fr.gi.image};
    for (VkImage im : pass1Out) {
        ImgBarrier(cmd, im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                   0, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    // 3) Pass 1 — trace (HALF resolution: ¼ of the BVH rays).
    const uint32_t rw = (w + 1) / 2;
    const uint32_t rh = (h + 1) / 2;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_tracePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_tracePipeLayout,
                            0, 1, &fr.traceSet, 0, nullptr);
    vkCmdDispatch(cmd, (rw + 7) / 8, (rh + 7) / 8, 1);

    // 4) Pass-1 outputs write->read; output image -> GENERAL (pass-2 write);
    //    temporal history: prev frame's hist/gbuf writes -> this frame's reads,
    //    and our own hist (read by prev frame as history, WAR) -> writable.
    for (VkImage im : pass1Out) {
        ImgBarrier(cmd, im, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    for (VkImage im : {prev.hist.image, prev.gbuf.image, prev.giHist.image}) {
        ImgBarrier(cmd, im, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    for (VkImage im : {fr.hist.image, fr.giHist.image}) {
        ImgBarrier(cmd, im, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                   0, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    VkImageLayout outOld = fr.outInGeneral ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                           : VK_IMAGE_LAYOUT_UNDEFINED;
    ImgBarrier(cmd, fr.out.image, outOld, VK_IMAGE_LAYOUT_GENERAL,
               0, VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    fr.outInGeneral = true;

    // 5) Pass 2 — denoise (half res): bilateral blur + temporal accumulation.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoisePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoisePipeLayout,
                            0, 1, &fr.denoiseSet, 0, nullptr);
    vkCmdDispatch(cmd, (rw + 7) / 8, (rh + 7) / 8, 1);

    // 5b) hist/giHist writes -> composite reads.
    for (VkImage im : {fr.hist.image, fr.giHist.image}) {
        ImgBarrier(cmd, im, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    // 5c) Pass 3 — composite (FULL res): upsample RT term over the raster.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_compositePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_compositePipeLayout,
                            0, 1, &fr.compositeSet, 0, nullptr);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);

    // 6) output -> TRANSFER_SRC, swapchain -> TRANSFER_DST, blit, swapchain -> PRESENT.
    ImgBarrier(cmd, fr.out.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    ImgBarrier(cmd, swapchainImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    FullBlit(cmd, fr.out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
             swapchainImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, w, h);
    ImgBarrier(cmd, swapchainImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    return true;
}

void VKComputeRaytracer::Destroy(const VKContext& ctx) {
    for (uint32_t i = 0; i < kFrames; ++i) {
        FrameRes& fr = m_frame[i];
        DestroyImg(ctx, fr.out);
        DestroyImg(ctx, fr.ras);
        DestroyImg(ctx, fr.lit);
        DestroyImg(ctx, fr.shd);
        DestroyImg(ctx, fr.occ);
        DestroyImg(ctx, fr.gbuf);
        DestroyImg(ctx, fr.hist);
        DestroyImg(ctx, fr.gi);
        DestroyImg(ctx, fr.giHist);
        if (fr.nodeBuf.buffer) DestroyBufferStorage(ctx, fr.nodeBuf);
        if (fr.triBuf.buffer)  DestroyBufferStorage(ctx, fr.triBuf);
        if (fr.ubo.buffer)     DestroyBufferStorage(ctx, fr.ubo);
        fr = FrameRes{};
    }
    if (m_depthView)    vkDestroyImageView(ctx.device, m_depthView, nullptr);
    if (m_depthSampler) vkDestroySampler(ctx.device, m_depthSampler, nullptr);
    m_depthView = VK_NULL_HANDLE; m_depthSampler = VK_NULL_HANDLE;
    m_cachedDepth = VK_NULL_HANDLE;
    if (m_pool)                vkDestroyDescriptorPool(ctx.device, m_pool, nullptr);
    if (m_tracePipeline)       vkDestroyPipeline(ctx.device, m_tracePipeline, nullptr);
    if (m_denoisePipeline)     vkDestroyPipeline(ctx.device, m_denoisePipeline, nullptr);
    if (m_compositePipeline)   vkDestroyPipeline(ctx.device, m_compositePipeline, nullptr);
    if (m_tracePipeLayout)     vkDestroyPipelineLayout(ctx.device, m_tracePipeLayout, nullptr);
    if (m_denoisePipeLayout)   vkDestroyPipelineLayout(ctx.device, m_denoisePipeLayout, nullptr);
    if (m_compositePipeLayout) vkDestroyPipelineLayout(ctx.device, m_compositePipeLayout, nullptr);
    if (m_traceDsLayout)       vkDestroyDescriptorSetLayout(ctx.device, m_traceDsLayout, nullptr);
    if (m_denoiseDsLayout)     vkDestroyDescriptorSetLayout(ctx.device, m_denoiseDsLayout, nullptr);
    if (m_compositeDsLayout)   vkDestroyDescriptorSetLayout(ctx.device, m_compositeDsLayout, nullptr);
    m_pool = VK_NULL_HANDLE;
    m_tracePipeline = VK_NULL_HANDLE;     m_denoisePipeline = VK_NULL_HANDLE;
    m_compositePipeline = VK_NULL_HANDLE;
    m_tracePipeLayout = VK_NULL_HANDLE;   m_denoisePipeLayout = VK_NULL_HANDLE;
    m_compositePipeLayout = VK_NULL_HANDLE;
    m_traceDsLayout = VK_NULL_HANDLE;     m_denoiseDsLayout = VK_NULL_HANDLE;
    m_compositeDsLayout = VK_NULL_HANDLE;
}

}}} // namespace Nova64::Video::VK
