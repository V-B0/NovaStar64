/**
 * @file VKDevice.cpp
 */
// SPDX-License-Identifier: MIT
#include "VKDevice.hpp"

#include <vector>
#include <set>

namespace Nova64 { namespace Video { namespace VK {

bool VKDevice::Create(VkPhysicalDevice physical,
                      uint32_t graphicsFamily,
                      uint32_t presentFamily) {
    if (!physical) return false;

    m_graphicsFamily = graphicsFamily;
    m_presentFamily  = presentFamily;

    // ── unique queue create infos ──────────────────────────
    std::set<uint32_t> uniqueFamilies = { graphicsFamily, presentFamily };
    std::vector<VkDeviceQueueCreateInfo> qcis;
    qcis.reserve(uniqueFamilies.size());
    const float prio = 1.0f;
    for (uint32_t fam : uniqueFamilies) {
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = fam;
        qci.queueCount       = 1;
        qci.pQueuePriorities = &prio;
        qcis.push_back(qci);
    }

    // ── enabled extensions ─────────────────────────────────
    const char* deviceExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkPhysicalDeviceFeatures features{};
    // No special features required for clear-only Stage 1.

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount    = static_cast<uint32_t>(qcis.size());
    dci.pQueueCreateInfos       = qcis.data();
    dci.enabledExtensionCount   = 1;
    dci.ppEnabledExtensionNames = deviceExts;
    dci.pEnabledFeatures        = &features;

    VK_CHECK(vkCreateDevice(physical, &dci, nullptr, &m_device));
    volkLoadDevice(m_device);

    vkGetDeviceQueue(m_device, graphicsFamily, 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, presentFamily,  0, &m_presentQueue);

    Logger::Info("VKDevice: created (graphicsFamily={}, presentFamily={})",
                 graphicsFamily, presentFamily);
    return true;
}

void VKDevice::Destroy() {
    if (m_device) {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
        m_graphicsQueue = m_presentQueue = VK_NULL_HANDLE;
    }
}

void VKDevice::WaitIdle() {
    if (m_device) vkDeviceWaitIdle(m_device);
}

}}} // namespace Nova64::Video::VK
