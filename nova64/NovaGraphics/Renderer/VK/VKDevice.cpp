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

    // ── enumerate available extensions ─────────────────────
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availExts(extCount);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &extCount, availExts.data());
    auto hasExt = [&](const char* name) -> bool {
        for (const auto& e : availExts)
            if (strcmp(e.extensionName, name) == 0) return true;
        return false;
    };

    // ── enabled extensions ─────────────────────────────────
    std::vector<const char*> deviceExts = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    // RT extension quartet — all four must be present together.
    const bool hasAS   = hasExt(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    const bool hasRTP  = hasExt(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    const bool hasBDA  = hasExt(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    const bool hasDHO  = hasExt(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    m_supportsRayTracing = hasAS && hasRTP && hasBDA && hasDHO;
    if (m_supportsRayTracing) {
        deviceExts.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        deviceExts.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        deviceExts.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
        deviceExts.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        Logger::Info("VKDevice: RT extensions available — hardware raytracing ENABLED");
    } else {
        Logger::Info("VKDevice: RT extensions not available — hardware raytracing DISABLED");
    }

    // ── feature chain ──────────────────────────────────────
    // Base VkPhysicalDeviceFeatures2 always used so we can append pNext nodes.
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};

    // VK_EXT_extended_dynamic_state: enables dynamic depth-test/write/compare
    // so that ISA warmup draws can force depth-test-off (guaranteeing the full
    // fragment shader path is compiled rather than only the early-depth-exit
    // variant), eliminating the Clock Town TDR on Intel Iris Xe.
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT edsFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    bool hasEDS = hasExt(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    if (hasEDS) {
        deviceExts.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
        edsFeatures.extendedDynamicState = VK_TRUE;
        edsFeatures.pNext = features2.pNext;
        features2.pNext   = &edsFeatures;
        Logger::Info("VKDevice: VK_EXT_extended_dynamic_state ENABLED (ISA warmup depth override)");
    } else {
        Logger::Info("VKDevice: VK_EXT_extended_dynamic_state not available (ISA warmup may be less effective)");
    }

    VkPhysicalDeviceBufferDeviceAddressFeaturesKHR bdaFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtpFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};

    // Query available features first, then enable only what's there.
    if (m_supportsRayTracing) {
        bdaFeatures.pNext  = nullptr;
        asFeatures.pNext   = &bdaFeatures;
        rtpFeatures.pNext  = &asFeatures;
        features2.pNext    = &rtpFeatures;
        vkGetPhysicalDeviceFeatures2(physical, &features2);

        // Enforce minimum requirements.  Disable RT if core features are absent.
        if (!bdaFeatures.bufferDeviceAddress ||
            !asFeatures.accelerationStructure ||
            !rtpFeatures.rayTracingPipeline) {
            Logger::Info("VKDevice: RT feature flags missing — hardware raytracing DISABLED");
            m_supportsRayTracing = false;
            deviceExts.erase(deviceExts.begin() + 1, deviceExts.end()); // keep swapchain only
            features2.pNext = nullptr;
            vkGetPhysicalDeviceFeatures2(physical, &features2); // re-query without RT
        }
    } else {
        vkGetPhysicalDeviceFeatures2(physical, &features2);
    }

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext                   = &features2;
    dci.queueCreateInfoCount    = static_cast<uint32_t>(qcis.size());
    dci.pQueueCreateInfos       = qcis.data();
    dci.enabledExtensionCount   = static_cast<uint32_t>(deviceExts.size());
    dci.ppEnabledExtensionNames = deviceExts.data();
    // pEnabledFeatures must be NULL when pNext chain contains features2
    dci.pEnabledFeatures        = nullptr;

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
