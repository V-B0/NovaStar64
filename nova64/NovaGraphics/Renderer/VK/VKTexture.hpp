/**
 * @file VKTexture.hpp
 * @brief Vulkan-backed ITexture (VkImage + VkDeviceMemory + view + sampler).
 *
 * Only 2D textures are supported (no arrays or cubemaps). Mip generation uses
 * vkCmdBlitImage on the linear-filter path (the caller must verify that the
 * chosen format supports linear blit on the device; almost always true for
 * RGBA8).
 *
 * N64 native formats (I8, IA16, CI4, CI8) have no direct Vulkan equivalent.
 * They are mapped to:
 *   I8   → VK_FORMAT_R8_UNORM       (caller swizzles in shader)
 *   IA16 → VK_FORMAT_R8G8_UNORM
 *   CI4  → VK_FORMAT_R8_UNORM       (caller unpacks to RGBA via TLUT)
 *   CI8  → VK_FORMAT_R8_UNORM
 */
// SPDX-License-Identifier: MIT
#pragma once

#include "VKCommon.hpp"
#include "VKMemory.hpp"
#include "../Renderer.hpp"

namespace Nova64 {
namespace Video {
namespace VK {

/// Translate engine TextureFormat → VkFormat.
/// Returns VK_FORMAT_UNDEFINED for unsupported formats (logged).
VkFormat ToVkFormat(TextureFormat f);

/// Bytes per pixel for the chosen format (after Vulkan mapping).
/// Returns 0 for compressed/unknown.
uint32_t BytesPerPixel(TextureFormat f);

class VKTextureImpl final : public ITexture {
public:
    VKTextureImpl(const VKContext* ctx, const TextureDesc& desc);
    ~VKTextureImpl() override;

    // ITexture
    void Upload(const void* data, size_t sizeBytes, uint32_t mipLevel) override;
    void UploadSubRegion(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* data, size_t sizeBytes,
                         uint32_t mipLevel) override;
    void GenerateMipmaps() override;
    void SetSampler(const SamplerDesc& desc) override;
    uint32_t GetWidth() const override { return m_w; }
    uint32_t GetHeight() const override { return m_h; }
    TextureFormat GetFormat() const override { return m_format; }
    uint64_t GetNativeHandle() const override { return reinterpret_cast<uint64_t>(m_image); }

    // VK-side accessors
    VkImage GetImage() const { return m_image; }
    VkImageView GetView() const { return m_view; }
    VkSampler GetSampler() const { return m_sampler; }
    VkImageLayout GetLayout() const { return m_layout; }

    /// Samplers are shared between textures with the same settings (a game has a few
    /// dozen distinct ones, and thousands of textures). Call before the device is destroyed.
    static void DestroySamplerCache(VkDevice device);

private:
    bool CreateImageAndMemory();
    void CreateDefaultSampler();

    const VKContext* m_ctx;
    uint32_t m_w = 0;
    uint32_t m_h = 0;
    uint32_t m_mipLevels = 1;
    TextureFormat m_format = TextureFormat::RGBA8;
    VkFormat m_vkFormat = VK_FORMAT_UNDEFINED;

    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE; // shared, owned by the sampler cache
    VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

} // namespace VK
} // namespace Video
} // namespace Nova64
