/**
 * @file VKSwapchain.hpp
 * @brief Vulkan swapchain + per-image views.
 */
// SPDX-License-Identifier: MIT
#pragma once

#include "VKCommon.hpp"

#include <vector>

namespace Nova64 { namespace Video { namespace VK {

class VKSwapchain {
public:
    bool Create(VkPhysicalDevice physical,
                VkDevice         device,
                VkSurfaceKHR     surface,
                uint32_t         graphicsFamily,
                uint32_t         presentFamily,
                uint32_t         desiredWidth,
                uint32_t         desiredHeight,
                bool             vsync);
    void Destroy(VkDevice device);

    /// Tear down and recreate (for window resize / vsync toggle).
    bool Recreate(VkPhysicalDevice physical,
                  VkDevice         device,
                  VkSurfaceKHR     surface,
                  uint32_t         graphicsFamily,
                  uint32_t         presentFamily,
                  uint32_t         desiredWidth,
                  uint32_t         desiredHeight,
                  bool             vsync);

    VkSwapchainKHR Get()      const { return m_swapchain; }
    VkFormat       GetFormat() const { return m_format; }
    VkExtent2D     GetExtent() const { return m_extent; }
    uint32_t       GetImageCount() const { return static_cast<uint32_t>(m_images.size()); }
    VkImage        GetImage(uint32_t i)     const { return m_images[i]; }
    VkImageView    GetImageView(uint32_t i) const { return m_imageViews[i]; }

    /// A simple single-color-attachment render pass matching the swapchain format.
    /// Pipelines must be created against this (or a compatible) render pass.
    VkRenderPass   GetRenderPass()             const { return m_renderPass; }
    /// Like GetRenderPass() but uses LOAD_OP_LOAD — preserves existing content.
    /// Compatible with the same framebuffers (same format/sample count).
    VkRenderPass   GetRenderPassLoad()         const { return m_renderPassLoad; }
    VkFramebuffer  GetFramebuffer(uint32_t i)  const { return m_framebuffers[i]; }

    /// Depth buffer access for the RT composite's raster-depth comparison.
    VkImage        GetDepthImage()  const { return m_depthImage; }
    VkFormat       GetDepthFormat() const { return m_depthFormat; }

private:
    bool CreateRenderPass(VkDevice device);
    bool CreateRenderPassLoad(VkDevice device);
    bool CreateDepthBuffer(VkPhysicalDevice physical, VkDevice device);
    bool CreateFramebuffers(VkDevice device);

    VkSwapchainKHR           m_swapchain = VK_NULL_HANDLE;
    VkFormat                 m_format    = VK_FORMAT_UNDEFINED;
    VkExtent2D               m_extent    = {0, 0};
    std::vector<VkImage>     m_images;
    std::vector<VkImageView> m_imageViews;
    VkRenderPass               m_renderPass     = VK_NULL_HANDLE;
    VkRenderPass               m_renderPassLoad = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;

    // Per-swapchain depth buffer (one image shared by all framebuffers).
    VkFormat       m_depthFormat = VK_FORMAT_UNDEFINED;
    VkImage        m_depthImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_depthMemory = VK_NULL_HANDLE;
    VkImageView    m_depthView   = VK_NULL_HANDLE;
};

}}} // namespace Nova64::Video::VK
