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
    VkFramebuffer  GetFramebuffer(uint32_t i)  const { return m_framebuffers[i]; }

private:
    bool CreateRenderPass(VkDevice device);
    bool CreateFramebuffers(VkDevice device);

    VkSwapchainKHR           m_swapchain = VK_NULL_HANDLE;
    VkFormat                 m_format    = VK_FORMAT_UNDEFINED;
    VkExtent2D               m_extent    = {0, 0};
    std::vector<VkImage>     m_images;
    std::vector<VkImageView> m_imageViews;
    VkRenderPass               m_renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;
};

}}} // namespace Nova64::Video::VK
