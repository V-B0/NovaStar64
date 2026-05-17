/**
 * @file VKRenderTarget.cpp
 */
// SPDX-License-Identifier: MIT
#include "VKRenderTarget.hpp"

#include <array>

namespace Nova64 { namespace Video { namespace VK {

VKRenderTargetImpl::VKRenderTargetImpl(const VKContext* ctx, const RenderTargetDesc& d)
    : m_ctx(ctx), m_desc(d), m_w(d.width), m_h(d.height) {
    if (!BuildResources()) {
        Logger::Error("VKRenderTarget: BuildResources failed ({}x{})", m_w, m_h);
    }
}

VKRenderTargetImpl::~VKRenderTargetImpl() {
    DestroyResources();
}

bool VKRenderTargetImpl::BuildResources() {
    if (m_w == 0 || m_h == 0) return false;

    // Color attachment (sampleable).
    TextureDesc cd{};
    cd.width = m_w; cd.height = m_h; cd.mipLevels = 1;
    cd.format = m_desc.colorFormat;
    cd.usage = BufferUsage::Static;
    cd.isRenderTarget = true;
    cd.debugName = m_desc.debugName;
    m_color = std::make_unique<VKTextureImpl>(m_ctx, cd);
    if (!m_color->GetImage()) return false;

    // Depth attachment (optional).
    if (m_desc.hasDepth) {
        TextureDesc dd{};
        dd.width = m_w; dd.height = m_h; dd.mipLevels = 1;
        dd.format = m_desc.depthFormat;
        dd.usage = BufferUsage::Static;
        dd.isDepthStencil = true;
        dd.debugName = m_desc.debugName;
        m_depth = std::make_unique<VKTextureImpl>(m_ctx, dd);
        if (!m_depth->GetImage()) return false;
    }

    // Render pass: color (LOAD_OP_LOAD by default — caller drives clearing
    // explicitly; LOAD lets multi-pass rendering preserve contents). For the
    // first bind we issue a vkCmdClearAttachments if Clear* was called.
    VkFormat colorVk = ToVkFormat(m_desc.colorFormat);
    VkFormat depthVk = m_desc.hasDepth ? ToVkFormat(m_desc.depthFormat) : VK_FORMAT_UNDEFINED;

    std::array<VkAttachmentDescription, 2> atts{};
    atts[0].format         = colorVk;
    atts[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    atts[0].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    atts[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[0].initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    atts[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription sub{};
    sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments    = &colorRef;

    if (m_desc.hasDepth) {
        atts[1].format         = depthVk;
        atts[1].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[1].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        atts[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        atts[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        atts[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        sub.pDepthStencilAttachment = &depthRef;
    }

    // External dependencies so SHADER_READ → COLOR_ATTACHMENT_WRITE and back
    // are properly synchronized when the same target is sampled later.
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                          | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                          | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                          | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                          | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = m_desc.hasDepth ? 2u : 1u;
    rpi.pAttachments    = atts.data();
    rpi.subpassCount    = 1;
    rpi.pSubpasses      = &sub;
    rpi.dependencyCount = static_cast<uint32_t>(deps.size());
    rpi.pDependencies   = deps.data();
    if (vkCreateRenderPass(m_ctx->device, &rpi, nullptr, &m_renderPass) != VK_SUCCESS) {
        Logger::Error("VKRenderTarget: vkCreateRenderPass failed");
        return false;
    }

    // Framebuffer.
    std::array<VkImageView, 2> views{};
    views[0] = m_color->GetView();
    if (m_desc.hasDepth) views[1] = m_depth->GetView();

    VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbi.renderPass      = m_renderPass;
    fbi.attachmentCount = m_desc.hasDepth ? 2u : 1u;
    fbi.pAttachments    = views.data();
    fbi.width           = m_w;
    fbi.height          = m_h;
    fbi.layers          = 1;
    if (vkCreateFramebuffer(m_ctx->device, &fbi, nullptr, &m_framebuffer) != VK_SUCCESS) {
        Logger::Error("VKRenderTarget: vkCreateFramebuffer failed");
        return false;
    }

    // One-shot: transition both images from UNDEFINED → the layout the render
    // pass expects on first use (matches initialLayout in the attachment desc).
    VkCommandBuffer cmd = BeginOneShot(m_ctx->device, m_ctx->transientPool);
    if (cmd) {
        auto barrier = [&](VkImage img, VkImageAspectFlags asp, VkImageLayout newL) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = newL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img;
            b.subresourceRange.aspectMask = asp;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = 1;
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &b);
        };
        barrier(m_color->GetImage(), VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (m_desc.hasDepth) {
            VkImageAspectFlags asp = VK_IMAGE_ASPECT_DEPTH_BIT;
            VkFormat df = ToVkFormat(m_desc.depthFormat);
            if (df == VK_FORMAT_D24_UNORM_S8_UINT || df == VK_FORMAT_D32_SFLOAT_S8_UINT)
                asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
            barrier(m_depth->GetImage(), asp,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        }
        EndOneShotAndWait(*m_ctx, cmd);
    }
    return true;
}

void VKRenderTargetImpl::DestroyResources() {
    if (!m_ctx || !m_ctx->device) return;
    if (m_framebuffer) {
        vkDestroyFramebuffer(m_ctx->device, m_framebuffer, nullptr);
        m_framebuffer = VK_NULL_HANDLE;
    }
    if (m_renderPass) {
        vkDestroyRenderPass(m_ctx->device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
    m_color.reset();
    m_depth.reset();
}

void VKRenderTargetImpl::Resize(uint32_t w, uint32_t h) {
    if (w == m_w && h == m_h) return;
    if (m_ctx && m_ctx->device) vkDeviceWaitIdle(m_ctx->device);
    DestroyResources();
    m_w = w; m_h = h;
    BuildResources();
}

}}} // namespace Nova64::Video::VK
