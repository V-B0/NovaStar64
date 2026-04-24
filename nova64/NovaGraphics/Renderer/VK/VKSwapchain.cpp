/**
 * @file VKSwapchain.cpp
 */
// SPDX-License-Identifier: MIT
#include "VKSwapchain.hpp"

#include <algorithm>
#include <vector>

namespace Nova64 { namespace Video { namespace VK {

namespace {

VkSurfaceFormatKHR PickSurfaceFormat(VkPhysicalDevice phys, VkSurfaceKHR surf) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &count, fmts.data());

    // Prefer 8-bit BGRA UNORM with sRGB nonlinear (matches GL backbuffer behavior).
    for (const auto& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLORSPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    if (!fmts.empty()) return fmts[0];
    return { VK_FORMAT_B8G8R8A8_UNORM, VK_COLORSPACE_SRGB_NONLINEAR_KHR };
}

VkPresentModeKHR PickPresentMode(VkPhysicalDevice phys, VkSurfaceKHR surf, bool vsync) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surf, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surf, &count, modes.data());

    if (vsync) {
        // FIFO is guaranteed available; FIFO_RELAXED is preferred when available.
        for (auto m : modes) if (m == VK_PRESENT_MODE_FIFO_RELAXED_KHR) return m;
        return VK_PRESENT_MODE_FIFO_KHR;
    } else {
        for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR)   return m;
        for (auto m : modes) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) return m;
        return VK_PRESENT_MODE_FIFO_KHR;
    }
}

VkExtent2D PickExtent(const VkSurfaceCapabilitiesKHR& caps,
                      uint32_t desiredW, uint32_t desiredH) {
    if (caps.currentExtent.width != UINT32_MAX) {
        return caps.currentExtent;
    }
    VkExtent2D e{ desiredW, desiredH };
    e.width  = std::clamp(e.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
    e.height = std::clamp(e.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    return e;
}

} // anonymous

bool VKSwapchain::Create(VkPhysicalDevice physical,
                         VkDevice         device,
                         VkSurfaceKHR     surface,
                         uint32_t         graphicsFamily,
                         uint32_t         presentFamily,
                         uint32_t         desiredWidth,
                         uint32_t         desiredHeight,
                         bool             vsync) {
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));

    VkSurfaceFormatKHR fmt    = PickSurfaceFormat(physical, surface);
    VkPresentModeKHR  present = PickPresentMode(physical, surface, vsync);
    m_extent = PickExtent(caps, desiredWidth, desiredHeight);
    m_format = fmt.format;

    if (m_extent.width == 0 || m_extent.height == 0) {
        Logger::Warn("VKSwapchain: surface extent is zero (window minimized?)");
        return false;
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface          = surface;
    sci.minImageCount    = imageCount;
    sci.imageFormat      = fmt.format;
    sci.imageColorSpace  = fmt.colorSpace;
    sci.imageExtent      = m_extent;
    sci.imageArrayLayers = 1;
    // TRANSFER_DST so we can vkCmdClearColorImage in Stage 1.
    // COLOR_ATTACHMENT will be added in later stages.
    sci.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                         | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t qIndices[2] = { graphicsFamily, presentFamily };
    if (graphicsFamily != presentFamily) {
        sci.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        sci.queueFamilyIndexCount = 2;
        sci.pQueueFamilyIndices   = qIndices;
    } else {
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    sci.preTransform   = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode    = present;
    sci.clipped        = VK_TRUE;
    sci.oldSwapchain   = VK_NULL_HANDLE;

    VK_CHECK(vkCreateSwapchainKHR(device, &sci, nullptr, &m_swapchain));

    uint32_t actualCount = 0;
    vkGetSwapchainImagesKHR(device, m_swapchain, &actualCount, nullptr);
    m_images.resize(actualCount);
    vkGetSwapchainImagesKHR(device, m_swapchain, &actualCount, m_images.data());

    // Create image views (used by Stage 2+ render passes; harmless to make now).
    m_imageViews.resize(actualCount);
    for (uint32_t i = 0; i < actualCount; ++i) {
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image    = m_images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format   = m_format;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(device, &ivci, nullptr, &m_imageViews[i]));
    }

    if (!CreateRenderPass(device)) return false;
    if (!CreateFramebuffers(device)) return false;

    Logger::Info("VKSwapchain: created {}x{}, format={}, presentMode={}, images={}",
                 m_extent.width, m_extent.height,
                 static_cast<int>(m_format),
                 static_cast<int>(present),
                 actualCount);
    return true;
}

void VKSwapchain::Destroy(VkDevice device) {
    for (auto fb : m_framebuffers) {
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    }
    m_framebuffers.clear();
    if (m_renderPass) {
        vkDestroyRenderPass(device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
    for (auto v : m_imageViews) {
        if (v) vkDestroyImageView(device, v, nullptr);
    }
    m_imageViews.clear();
    m_images.clear();
    if (m_swapchain) {
        vkDestroySwapchainKHR(device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

bool VKSwapchain::Recreate(VkPhysicalDevice physical,
                           VkDevice         device,
                           VkSurfaceKHR     surface,
                           uint32_t         graphicsFamily,
                           uint32_t         presentFamily,
                           uint32_t         desiredWidth,
                           uint32_t         desiredHeight,
                           bool             vsync) {
    vkDeviceWaitIdle(device);
    Destroy(device);
    return Create(physical, device, surface, graphicsFamily, presentFamily,
                  desiredWidth, desiredHeight, vsync);
}

bool VKSwapchain::CreateRenderPass(VkDevice device) {
    VkAttachmentDescription color{};
    color.format         = m_format;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments    = &colorRef;

    // Wait for swapchain image to be acquired before writing to it.
    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments    = &color;
    rpi.subpassCount    = 1;
    rpi.pSubpasses      = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies   = &dep;
    VK_CHECK(vkCreateRenderPass(device, &rpi, nullptr, &m_renderPass));
    return true;
}

bool VKSwapchain::CreateFramebuffers(VkDevice device) {
    m_framebuffers.resize(m_imageViews.size());
    for (size_t i = 0; i < m_imageViews.size(); ++i) {
        VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbi.renderPass      = m_renderPass;
        fbi.attachmentCount = 1;
        fbi.pAttachments    = &m_imageViews[i];
        fbi.width           = m_extent.width;
        fbi.height          = m_extent.height;
        fbi.layers          = 1;
        VK_CHECK(vkCreateFramebuffer(device, &fbi, nullptr, &m_framebuffers[i]));
    }
    return true;
}

}}} // namespace Nova64::Video::VK
