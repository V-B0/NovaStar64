/**
 * @file VKSwapchain.cpp
 * @brief Vulkan swapchain (or offscreen image set): images, views, depth buffer, render passes and framebuffers.
 */
// SPDX-License-Identifier: MIT
#include "VKSwapchain.hpp"
#include "VKMemory.hpp"

#include <algorithm>
#include <vector>

namespace Nova64 {
namespace Video {
namespace VK {

namespace {

VkSurfaceFormatKHR PickSurfaceFormat(VkPhysicalDevice phys, VkSurfaceKHR surf) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &count, fmts.data());

    // Prefer 8-bit BGRA UNORM with sRGB nonlinear (matches GL backbuffer behavior).
    for (const auto& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLORSPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    if (!fmts.empty()) return fmts[0];
    return {VK_FORMAT_B8G8R8A8_UNORM, VK_COLORSPACE_SRGB_NONLINEAR_KHR};
}

VkPresentModeKHR PickPresentMode(VkPhysicalDevice phys, VkSurfaceKHR surf, bool vsync) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surf, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surf, &count, modes.data());

    if (vsync) {
        // FIFO is guaranteed available; FIFO_RELAXED is preferred when available.
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_FIFO_RELAXED_KHR) return m;
        return VK_PRESENT_MODE_FIFO_KHR;
    } else {
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) return m;
        return VK_PRESENT_MODE_FIFO_KHR;
    }
}

VkExtent2D PickExtent(const VkSurfaceCapabilitiesKHR& caps, uint32_t desiredW, uint32_t desiredH) {
    if (caps.currentExtent.width != UINT32_MAX) {
        return caps.currentExtent;
    }
    VkExtent2D e{desiredW, desiredH};
    e.width = std::clamp(e.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    e.height = std::clamp(e.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    return e;
}

} // namespace

bool VKSwapchain::Create(VkPhysicalDevice physical, VkDevice device, VkSurfaceKHR surface, uint32_t graphicsFamily,
                         uint32_t presentFamily, uint32_t desiredWidth, uint32_t desiredHeight, bool vsync) {
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));

    VkSurfaceFormatKHR fmt = PickSurfaceFormat(physical, surface);
    VkPresentModeKHR present = PickPresentMode(physical, surface, vsync);
    m_extent = PickExtent(caps, desiredWidth, desiredHeight);
    m_format = fmt.format;

    if (m_extent.width == 0 || m_extent.height == 0) {
        Logger::Warn("VKSwapchain: surface extent is zero (window minimized?)");
        return false;
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = imageCount;
    sci.imageFormat = fmt.format;
    sci.imageColorSpace = fmt.colorSpace;
    sci.imageExtent = m_extent;
    sci.imageArrayLayers = 1;
    // TRANSFER_DST for vkCmdClearColorImage, TRANSFER_SRC for vkCmdCopyImage
    // from the swapchain (BlitToTexture / ReadPixels), COLOR_ATTACHMENT for the
    // render pass.
    sci.imageUsage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t qIndices[2] = {graphicsFamily, presentFamily};
    if (graphicsFamily != presentFamily) {
        sci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        sci.queueFamilyIndexCount = 2;
        sci.pQueueFamilyIndices = qIndices;
    } else {
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = present;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = VK_NULL_HANDLE;

    VK_CHECK(vkCreateSwapchainKHR(device, &sci, nullptr, &m_swapchain));

    uint32_t actualCount = 0;
    vkGetSwapchainImagesKHR(device, m_swapchain, &actualCount, nullptr);
    m_images.resize(actualCount);
    vkGetSwapchainImagesKHR(device, m_swapchain, &actualCount, m_images.data());

    // Image views for the framebuffers.
    m_imageViews.resize(actualCount);
    for (uint32_t i = 0; i < actualCount; ++i) {
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = m_images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = m_format;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(device, &ivci, nullptr, &m_imageViews[i]));
    }

    if (!CreateDepthBuffer(physical, device)) return false;
    if (!CreateRenderPass(device)) return false;
    if (!CreateRenderPassLoad(device)) return false;
    if (!CreateFramebuffers(device)) return false;

    Logger::Info("VKSwapchain: created {}x{}, format={}, presentMode={}, images={}", m_extent.width, m_extent.height,
                 static_cast<int>(m_format), static_cast<int>(present), actualCount);
    return true;
}

bool VKSwapchain::CreateOffscreen(VkPhysicalDevice physical, VkDevice device, uint32_t width, uint32_t height,
                                  uint32_t imageCount) {
    m_offscreen = true;
    m_format = VK_FORMAT_B8G8R8A8_UNORM;
    m_extent = {width, height};
    if (width == 0 || height == 0 || imageCount == 0) return false;

    m_images.assign(imageCount, VK_NULL_HANDLE);
    m_imageMemory.assign(imageCount, VK_NULL_HANDLE);
    m_imageViews.assign(imageCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = m_format;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        // Same usage as the swapchain images in Create().
        ici.usage =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device, &ici, nullptr, &m_images[i]));

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, m_images[i], &req);
        const uint32_t typeIdx = FindMemoryType(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (typeIdx == UINT32_MAX) {
            Logger::Error("VKSwapchain: no suitable memory type for offscreen image");
            return false;
        }
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = typeIdx;
        VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &m_imageMemory[i]));
        VK_CHECK(vkBindImageMemory(device, m_images[i], m_imageMemory[i], 0));

        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = m_images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = m_format;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(device, &ivci, nullptr, &m_imageViews[i]));
    }

    if (!CreateDepthBuffer(physical, device)) return false;
    if (!CreateRenderPass(device)) return false;
    if (!CreateRenderPassLoad(device)) return false;
    if (!CreateFramebuffers(device)) return false;

    Logger::Info("VKSwapchain: created offscreen targets {}x{}, images={}", width, height, imageCount);
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
    if (m_renderPassLoad) {
        vkDestroyRenderPass(device, m_renderPassLoad, nullptr);
        m_renderPassLoad = VK_NULL_HANDLE;
    }
    // Depth buffer (must outlive framebuffers).
    if (m_depthView) {
        vkDestroyImageView(device, m_depthView, nullptr);
        m_depthView = VK_NULL_HANDLE;
    }
    if (m_depthImage) {
        vkDestroyImage(device, m_depthImage, nullptr);
        m_depthImage = VK_NULL_HANDLE;
    }
    if (m_depthMemory) {
        vkFreeMemory(device, m_depthMemory, nullptr);
        m_depthMemory = VK_NULL_HANDLE;
    }
    for (auto v : m_imageViews) {
        if (v) vkDestroyImageView(device, v, nullptr);
    }
    m_imageViews.clear();
    // Swapchain images belong to the swapchain; offscreen ones are ours.
    if (m_offscreen) {
        for (auto img : m_images) {
            if (img) vkDestroyImage(device, img, nullptr);
        }
    }
    for (auto mem : m_imageMemory) {
        if (mem) vkFreeMemory(device, mem, nullptr);
    }
    m_imageMemory.clear();
    m_images.clear();
    if (m_swapchain) {
        vkDestroySwapchainKHR(device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

bool VKSwapchain::Recreate(VkPhysicalDevice physical, VkDevice device, VkSurfaceKHR surface, uint32_t graphicsFamily,
                           uint32_t presentFamily, uint32_t desiredWidth, uint32_t desiredHeight, bool vsync) {
    vkDeviceWaitIdle(device);
    if (m_offscreen) {
        const uint32_t count = std::max<uint32_t>(1, static_cast<uint32_t>(m_images.size()));
        Destroy(device);
        return CreateOffscreen(physical, device, desiredWidth, desiredHeight, count);
    }
    Destroy(device);
    return Create(physical, device, surface, graphicsFamily, presentFamily, desiredWidth, desiredHeight, vsync);
}

bool VKSwapchain::CreateDepthBuffer(VkPhysicalDevice physical, VkDevice device) {
    // Pick the best supported depth format.
    const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D16_UNORM,
    };
    m_depthFormat = VK_FORMAT_UNDEFINED;
    for (VkFormat f : candidates) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(physical, f, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            m_depthFormat = f;
            break;
        }
    }
    if (m_depthFormat == VK_FORMAT_UNDEFINED) {
        Logger::Error("VKSwapchain: no supported depth format");
        return false;
    }

    // Create depth image.
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = m_depthFormat;
    ici.extent = {m_extent.width, m_extent.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(device, &ici, nullptr, &m_depthImage));

    // Allocate DEVICE_LOCAL memory.
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, m_depthImage, &req);
    uint32_t typeIdx = FindMemoryType(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (typeIdx == UINT32_MAX) {
        Logger::Error("VKSwapchain: no suitable memory type for depth image");
        return false;
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = typeIdx;
    VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &m_depthMemory));
    VK_CHECK(vkBindImageMemory(device, m_depthImage, m_depthMemory, 0));

    // Create image view.
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = m_depthImage;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = m_depthFormat;
    vci.subresourceRange.aspectMask = aspect;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(device, &vci, nullptr, &m_depthView));

    Logger::Info("VKSwapchain: depth buffer format={}, {}x{}", static_cast<int>(m_depthFormat), m_extent.width,
                 m_extent.height);
    return true;
}

bool VKSwapchain::CreateRenderPass(VkDevice device) {
    VkAttachmentDescription attachments[2]{};

    // Attachment 0: swapchain color image.
    attachments[0].format = m_format;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // Attachment 1: shared depth/stencil buffer.
    attachments[1].format = m_depthFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // STORE rather than DONT_CARE: the RT composite samples this depth buffer
    // after the pass for billboard suppression, and on tile-based GPUs (Apple)
    // DONT_CARE really discards the contents.
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;

    // Wait for swapchain image and depth buffer before writing to them.
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 2;
    rpi.pAttachments = attachments;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    VK_CHECK(vkCreateRenderPass(device, &rpi, nullptr, &m_renderPass));
    return true;
}

bool VKSwapchain::CreateRenderPassLoad(VkDevice device) {
    // Same as CreateRenderPass but with LOAD_OP_LOAD for color and depth, which
    // preserves the existing swapchain content. Used to resume the render pass
    // after a mid-frame GPU snapshot (SnapshotToTexture) without clearing the screen.
    VkAttachmentDescription attachments[2]{};

    attachments[0].format = m_format;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    attachments[1].format = m_depthFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE; // same reason as in CreateRenderPass
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 2;
    rpi.pAttachments = attachments;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    VK_CHECK(vkCreateRenderPass(device, &rpi, nullptr, &m_renderPassLoad));
    return true;
}

bool VKSwapchain::CreateFramebuffers(VkDevice device) {
    m_framebuffers.resize(m_imageViews.size());
    for (size_t i = 0; i < m_imageViews.size(); ++i) {
        VkImageView views[2] = {m_imageViews[i], m_depthView};
        VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbi.renderPass = m_renderPass;
        fbi.attachmentCount = 2;
        fbi.pAttachments = views;
        fbi.width = m_extent.width;
        fbi.height = m_extent.height;
        fbi.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device, &fbi, nullptr, &m_framebuffers[i]));
    }
    return true;
}

} // namespace VK
} // namespace Video
} // namespace Nova64
