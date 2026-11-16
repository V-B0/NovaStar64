/**
 * @file VKRenderTarget.hpp
 * @brief Real Vulkan IRenderTarget — own VkImage(s) + VkRenderPass + VkFramebuffer.
 *
 * Stage 5: a render target owns a color VKTextureImpl (sampleable) and an
 * optional depth VKTextureImpl, plus a private VkRenderPass + VkFramebuffer
 * sized to the target. VulkanRenderer::BindRenderTarget switches the active
 * render pass between this target and the swapchain's pass.
 */
// SPDX-License-Identifier: MIT
#pragma once

#include "VKCommon.hpp"
#include "VKMemory.hpp"
#include "VKTexture.hpp"
#include "../Renderer.hpp"

#include <memory>

namespace Nova64 {
namespace Video {
namespace VK {

class VKRenderTargetImpl final : public IRenderTarget {
public:
    VKRenderTargetImpl(const VKContext* ctx, const RenderTargetDesc& d);
    ~VKRenderTargetImpl() override;

    // IRenderTarget
    void Bind() override {} // VulkanRenderer drives binding
    void Unbind() override {}
    void Clear(float r, float g, float b, float a, float depth, uint8_t stencil) override {
        m_clearColor[0] = r;
        m_clearColor[1] = g;
        m_clearColor[2] = b;
        m_clearColor[3] = a;
        m_clearDepth = depth;
        m_clearStencil = stencil;
        m_pendingClearColor = true;
        m_pendingClearDepth = HasDepth();
    }
    void ClearColor(float r, float g, float b, float a) override {
        m_clearColor[0] = r;
        m_clearColor[1] = g;
        m_clearColor[2] = b;
        m_clearColor[3] = a;
        m_pendingClearColor = true;
    }
    void ClearDepthStencil(float depth, uint8_t stencil) override {
        m_clearDepth = depth;
        m_clearStencil = stencil;
        m_pendingClearDepth = HasDepth();
    }
    void Resize(uint32_t w, uint32_t h) override;
    ITexture* GetColorTexture() override { return m_color.get(); }
    ITexture* GetDepthTexture() override { return m_depth.get(); }
    uint32_t GetWidth() const override { return m_w; }
    uint32_t GetHeight() const override { return m_h; }
    uint64_t GetNativeHandle() const override { return reinterpret_cast<uint64_t>(m_framebuffer); }

    // VK accessors
    VkRenderPass GetRenderPass() const { return m_renderPass; }
    VkFramebuffer GetFramebuffer() const { return m_framebuffer; }
    bool HasDepth() const { return m_depth != nullptr; }
    const float* GetClearColor() const { return m_clearColor; }
    float GetClearDepth() const { return m_clearDepth; }
    uint8_t GetClearStencil() const { return m_clearStencil; }
    VKTextureImpl* GetColor() const { return m_color.get(); }
    VKTextureImpl* GetDepth() const { return m_depth.get(); }

    /// Returns the pending clear flags and resets them. Called by VulkanRenderer
    /// just after vkCmdBeginRenderPass on this target so a vkCmdClearAttachments
    /// can satisfy any Clear* recorded since the last bind.
    struct PendingClear {
        bool color;
        bool depth;
    };
    PendingClear ConsumePendingClear() {
        PendingClear p{m_pendingClearColor, m_pendingClearDepth};
        m_pendingClearColor = false;
        m_pendingClearDepth = false;
        return p;
    }

private:
    bool BuildResources();
    void DestroyResources();

    const VKContext* m_ctx;
    RenderTargetDesc m_desc;
    uint32_t m_w = 0;
    uint32_t m_h = 0;
    std::unique_ptr<VKTextureImpl> m_color;
    std::unique_ptr<VKTextureImpl> m_depth;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
    float m_clearColor[4] = {0, 0, 0, 1};
    float m_clearDepth = 1.0f;
    uint8_t m_clearStencil = 0;
    bool m_pendingClearColor = false;
    bool m_pendingClearDepth = false;
};

} // namespace VK
} // namespace Video
} // namespace Nova64
