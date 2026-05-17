/**
 * @file VKDevice.hpp
 * @brief Vulkan logical device + queues.
 */
// SPDX-License-Identifier: MIT
#pragma once

#include "VKCommon.hpp"

namespace Nova64 { namespace Video { namespace VK {

class VKDevice {
public:
    bool Create(VkPhysicalDevice physical,
                uint32_t graphicsFamily,
                uint32_t presentFamily);
    void Destroy();

    /// Block until all device work is complete (safe wrapper).
    void WaitIdle();

    VkDevice  Get()             const { return m_device; }
    VkQueue   GetGraphicsQueue() const { return m_graphicsQueue; }
    VkQueue   GetPresentQueue()  const { return m_presentQueue; }
    uint32_t  GetGraphicsFamily() const { return m_graphicsFamily; }
    uint32_t  GetPresentFamily()  const { return m_presentFamily; }

    /// True when VK_KHR_ray_tracing_pipeline + AS extensions were enabled.
    bool      SupportsRayTracing() const { return m_supportsRayTracing; }

private:
    VkDevice m_device         = VK_NULL_HANDLE;
    VkQueue  m_graphicsQueue  = VK_NULL_HANDLE;
    VkQueue  m_presentQueue   = VK_NULL_HANDLE;
    uint32_t m_graphicsFamily = 0;
    uint32_t m_presentFamily  = 0;
    bool     m_supportsRayTracing = false;
};

}}} // namespace Nova64::Video::VK
