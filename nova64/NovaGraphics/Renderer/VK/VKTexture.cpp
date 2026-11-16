/**
 * @file VKTexture.cpp
 */
// SPDX-License-Identifier: MIT
#include <map>
#include <array>
#include <mutex>
#include "VKTexture.hpp"
#include "VKBuffer.hpp" // staging buffer

#include <cstring>

namespace Nova64 {
namespace Video {
namespace VK {

// --- Format mapping ---

VkFormat ToVkFormat(TextureFormat f) {
    switch (f) {
    case TextureFormat::RGBA8:           return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::RGB8:            return VK_FORMAT_R8G8B8_UNORM;
    case TextureFormat::RGBA4:           return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case TextureFormat::RGB5A1:          return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case TextureFormat::R8:              return VK_FORMAT_R8_UNORM;
    case TextureFormat::RG8:             return VK_FORMAT_R8G8_UNORM;
    case TextureFormat::RGBA16F:         return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TextureFormat::RGBA32F:         return VK_FORMAT_R32G32B32A32_SFLOAT;
    case TextureFormat::Depth16:         return VK_FORMAT_D16_UNORM;
    case TextureFormat::Depth24:         return VK_FORMAT_X8_D24_UNORM_PACK32;
    case TextureFormat::Depth32F:        return VK_FORMAT_D32_SFLOAT;
    case TextureFormat::Depth24Stencil8: return VK_FORMAT_D24_UNORM_S8_UINT;
    // N64 native — best-effort
    case TextureFormat::IA16: return VK_FORMAT_R8G8_UNORM;
    case TextureFormat::I8:   return VK_FORMAT_R8_UNORM;
    case TextureFormat::CI4:  return VK_FORMAT_R8_UNORM;
    case TextureFormat::CI8:  return VK_FORMAT_R8_UNORM;
    default:                  Logger::Warn("VK: unsupported TextureFormat={}", static_cast<int>(f)); return VK_FORMAT_UNDEFINED;
    }
}

uint32_t BytesPerPixel(TextureFormat f) {
    switch (f) {
    case TextureFormat::RGBA8:           return 4;
    case TextureFormat::RGB8:            return 3;
    case TextureFormat::RGBA4:           return 2;
    case TextureFormat::RGB5A1:          return 2;
    case TextureFormat::R8:              return 1;
    case TextureFormat::RG8:             return 2;
    case TextureFormat::RGBA16F:         return 8;
    case TextureFormat::RGBA32F:         return 16;
    case TextureFormat::Depth16:         return 2;
    case TextureFormat::Depth24:         return 4;
    case TextureFormat::Depth32F:        return 4;
    case TextureFormat::Depth24Stencil8: return 4;
    case TextureFormat::IA16:            return 2;
    case TextureFormat::I8:              return 1;
    case TextureFormat::CI4:             return 1;
    case TextureFormat::CI8:             return 1;
    default:                             return 0;
    }
}

// --- Helpers ---

namespace {

bool IsDepthFormat(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D32_SFLOAT ||
           f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

bool HasStencil(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }

VkImageAspectFlags AspectFor(VkFormat f) {
    if (!IsDepthFormat(f)) return VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageAspectFlags a = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (HasStencil(f)) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return a;
}

VkFilter ToVkFilter(SamplerFilter f) {
    switch (f) {
    case SamplerFilter::Nearest:
    case SamplerFilter::NearestMipNearest:
    case SamplerFilter::NearestMipLinear:  return VK_FILTER_NEAREST;
    default:                               return VK_FILTER_LINEAR;
    }
}

VkSamplerMipmapMode ToVkMipMode(SamplerFilter f) {
    switch (f) {
    case SamplerFilter::NearestMipLinear:
    case SamplerFilter::LinearMipLinear:  return VK_SAMPLER_MIPMAP_MODE_LINEAR;
    default:                              return VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }
}

VkSamplerAddressMode ToVkAddress(TextureWrap w) {
    switch (w) {
    case TextureWrap::Repeat:            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case TextureWrap::MirroredRepeat:    return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case TextureWrap::ClampToEdge:       return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case TextureWrap::ClampToBorder:     return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case TextureWrap::MirrorClampToEdge: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

void TransitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, uint32_t mipLevels,
                           VkImageLayout oldL, VkImageLayout newL, VkAccessFlags srcA, VkAccessFlags dstA,
                           VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = mipLevels;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = srcA;
    b.dstAccessMask = dstA;
    vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace

// --- VKTextureImpl ---

VKTextureImpl::VKTextureImpl(const VKContext* ctx, const TextureDesc& desc) :
    m_ctx(ctx), m_w(desc.width), m_h(desc.height), m_mipLevels(desc.mipLevels ? desc.mipLevels : 1),
    m_format(desc.format) {
    m_vkFormat = ToVkFormat(desc.format);
    if (m_vkFormat == VK_FORMAT_UNDEFINED || m_w == 0 || m_h == 0) {
        Logger::Error("VKTexture: invalid desc ({}x{} fmt={})", m_w, m_h, static_cast<int>(desc.format));
        return;
    }
    if (!CreateImageAndMemory()) return;
    CreateDefaultSampler();
}

VKTextureImpl::~VKTextureImpl() {
    if (!m_ctx || !m_ctx->device) return;
    if (m_view) vkDestroyImageView(m_ctx->device, m_view, nullptr);
    if (m_image) vkDestroyImage(m_ctx->device, m_image, nullptr);
    if (m_memory) vkFreeMemory(m_ctx->device, m_memory, nullptr);
}

bool VKTextureImpl::CreateImageAndMemory() {
    const bool isDepth = m_ctx ? IsDepthFormat(m_vkFormat) : false;

    VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // for blit-mip
    if (isDepth) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = m_vkFormat;
    ii.extent = {m_w, m_h, 1};
    ii.mipLevels = m_mipLevels;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_ctx->device, &ii, nullptr, &m_image) != VK_SUCCESS) {
        Logger::Error("VKTexture: vkCreateImage failed");
        return false;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_ctx->device, m_image, &req);
    uint32_t typeIdx = FindMemoryType(m_ctx->physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (typeIdx == UINT32_MAX) return false;

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = typeIdx;
    if (vkAllocateMemory(m_ctx->device, &ai, nullptr, &m_memory) != VK_SUCCESS) {
        Logger::Error("VKTexture: vkAllocateMemory failed");
        return false;
    }
    if (vkBindImageMemory(m_ctx->device, m_image, m_memory, 0) != VK_SUCCESS) {
        Logger::Error("VKTexture: vkBindImageMemory failed");
        return false;
    }

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = m_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = m_vkFormat;
    vi.subresourceRange.aspectMask = AspectFor(m_vkFormat);
    vi.subresourceRange.baseMipLevel = 0;
    vi.subresourceRange.levelCount = m_mipLevels;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(m_ctx->device, &vi, nullptr, &m_view) != VK_SUCCESS) {
        Logger::Error("VKTexture: vkCreateImageView failed");
        return false;
    }

    m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void VKTextureImpl::CreateDefaultSampler() {
    SamplerDesc d{};
    SetSampler(d);
}

namespace {
std::mutex g_samplerMutex;
std::map<std::array<uint32_t, 10>, VkSampler> g_samplerCache;

inline uint32_t FloatBits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
} // namespace

void VKTextureImpl::DestroySamplerCache(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_samplerMutex);
    for (auto& entry : g_samplerCache)
        if (device && entry.second) vkDestroySampler(device, entry.second, nullptr);
    g_samplerCache.clear();
}

void VKTextureImpl::SetSampler(const SamplerDesc& desc) {
    if (!m_ctx || !m_ctx->device) return;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = ToVkFilter(desc.magFilter);
    si.minFilter = ToVkFilter(desc.minFilter);
    si.mipmapMode = ToVkMipMode(desc.minFilter);
    si.addressModeU = ToVkAddress(desc.wrapS);
    si.addressModeV = ToVkAddress(desc.wrapT);
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.mipLodBias = desc.lodBias;
    si.anisotropyEnable = desc.maxAnisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    si.maxAnisotropy = desc.maxAnisotropy;
    si.compareEnable = VK_FALSE;
    si.minLod = 0.0f;
    si.maxLod = static_cast<float>(m_mipLevels);
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    si.unnormalizedCoordinates = VK_FALSE;

    // One sampler per distinct set of settings, not one per texture.
    const std::array<uint32_t, 10> key = {static_cast<uint32_t>(si.magFilter),
                                          static_cast<uint32_t>(si.minFilter),
                                          static_cast<uint32_t>(si.mipmapMode),
                                          static_cast<uint32_t>(si.addressModeU),
                                          static_cast<uint32_t>(si.addressModeV),
                                          static_cast<uint32_t>(si.addressModeW),
                                          FloatBits(si.mipLodBias),
                                          static_cast<uint32_t>(si.anisotropyEnable),
                                          FloatBits(si.maxAnisotropy),
                                          FloatBits(si.maxLod)};
    std::lock_guard<std::mutex> lock(g_samplerMutex);
    auto it = g_samplerCache.find(key);
    if (it == g_samplerCache.end()) {
        VkSampler sampler = VK_NULL_HANDLE;
        if (vkCreateSampler(m_ctx->device, &si, nullptr, &sampler) != VK_SUCCESS) {
            Logger::Error("VKTexture: vkCreateSampler failed");
            return;
        }
        it = g_samplerCache.emplace(key, sampler).first;
    }
    m_sampler = it->second;
}

void VKTextureImpl::Upload(const void* data, size_t sizeBytes, uint32_t mipLevel) {
    UploadSubRegion(0, 0, m_w >> mipLevel, m_h >> mipLevel, data, sizeBytes, mipLevel);
}

void VKTextureImpl::UploadSubRegion(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* data, size_t sizeBytes,
                                    uint32_t mipLevel) {
    if (!m_image || !data || sizeBytes == 0) return;
    if (mipLevel >= m_mipLevels) {
        Logger::Error("VKTexture: mipLevel {} >= {}", mipLevel, m_mipLevels);
        return;
    }

    // Staging buffer
    VKBufferStorage staging{};
    if (!CreateBufferStorage(*m_ctx, sizeBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)) {
        return;
    }
    std::memcpy(staging.mapped, data, sizeBytes);

    VkCommandBuffer cmd = BeginOneShot(m_ctx->device, m_ctx->transientPool);
    if (!cmd) {
        DestroyBufferStorage(*m_ctx, staging);
        return;
    }

    const VkImageAspectFlags aspect = AspectFor(m_vkFormat);

    // Transition (current layout) → TRANSFER_DST
    VkImageLayout oldL = m_layout;
    TransitionImageLayout(cmd, m_image, aspect, m_mipLevels, oldL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = aspect;
    region.imageSubresource.mipLevel = mipLevel;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition → SHADER_READ_ONLY for sampling
    TransitionImageLayout(cmd, m_image, aspect, m_mipLevels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    m_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    EndOneShotAndWait(*m_ctx, cmd);
    DestroyBufferStorage(*m_ctx, staging);
}

void VKTextureImpl::GenerateMipmaps() {
    if (!m_image || m_mipLevels <= 1) return;

    // Verify format supports linear blit (sane for RGBA8/UNORM; warn otherwise).
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(m_ctx->physical, m_vkFormat, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        Logger::Warn("VKTexture: format does not support linear blit; skipping mip gen");
        return;
    }

    VkCommandBuffer cmd = BeginOneShot(m_ctx->device, m_ctx->transientPool);
    if (!cmd) return;

    int32_t mipW = static_cast<int32_t>(m_w);
    int32_t mipH = static_cast<int32_t>(m_h);

    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image = m_image;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    b.subresourceRange.levelCount = 1;

    for (uint32_t i = 1; i < m_mipLevels; ++i) {
        // Transition mip i-1 to TRANSFER_SRC.
        // First iteration: mip 0 is in SHADER_READ_ONLY_OPTIMAL (after upload).
        // Later iterations: mip i-1 was just blitted into, so it is in TRANSFER_DST_OPTIMAL.
        b.subresourceRange.baseMipLevel = i - 1;
        b.oldLayout = (i == 1) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = (i == 1) ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, (i == 1) ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

        // Transition mip i to TRANSFER_DST so we can blit into it.
        // UploadSubRegion transitions ALL mip levels to SHADER_READ_ONLY_OPTIMAL,
        // so mips 1..n-1 are in SHADER_READ_ONLY even though they have no data yet.
        b.subresourceRange.baseMipLevel = i;
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);

        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mipW, mipH, 1};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.layerCount = 1;
        int32_t nextW = mipW > 1 ? mipW / 2 : 1;
        int32_t nextH = mipH > 1 ? mipH / 2 : 1;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {nextW, nextH, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.layerCount = 1;
        vkCmdBlitImage(cmd, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        // Transition mip i-1 back to SHADER_READ_ONLY.
        b.subresourceRange.baseMipLevel = i - 1;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
        mipW = nextW;
        mipH = nextH;
    }

    // Transition the last mip from TRANSFER_DST (just blitted into) to SHADER_READ_ONLY.
    b.subresourceRange.baseMipLevel = m_mipLevels - 1;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);

    EndOneShotAndWait(*m_ctx, cmd);
    m_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

} // namespace VK
} // namespace Video
} // namespace Nova64
