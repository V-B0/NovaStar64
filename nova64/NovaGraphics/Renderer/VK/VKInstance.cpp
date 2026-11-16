/**
 * @file VKInstance.cpp
 */
// SPDX-License-Identifier: MIT
#include "VKInstance.hpp"

#include <cstring>
#include <vector>

namespace Nova64 {
namespace Video {
namespace VK {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user*/) {
    if (!data || !data->pMessage) return VK_FALSE;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        Logger::Error("[Vulkan] {}", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        Logger::Warn("[Vulkan] {}", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        Logger::Info("[Vulkan] {}", data->pMessage);
    }
    return VK_FALSE;
}

bool HasLayer(const char* name, const std::vector<VkLayerProperties>& layers) {
    for (const auto& l : layers) {
        if (std::strcmp(l.layerName, name) == 0) return true;
    }
    return false;
}

bool HasExtension(const char* name, const std::vector<VkExtensionProperties>& exts) {
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

} // namespace

bool VKInstance::Create(const std::vector<const char*>& requiredInstanceExts, bool enableValidation) {
    // ── volk loader ─────────────────────────────────────────
    if (!m_volkInitialized) {
        VkResult r = volkInitialize();
        if (r != VK_SUCCESS) {
            Logger::Warn("VKInstance: volkInitialize failed ({}). "
                         "Vulkan loader (vulkan-1.dll) likely not installed.",
                         VkResultToString(r));
            return false;
        }
        m_volkInitialized = true;
    }

    // Probe API version (volk has vkEnumerateInstanceVersion already loaded)
    uint32_t apiVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) {
        VK_CHECK(vkEnumerateInstanceVersion(&apiVersion));
    }
    Logger::Info("VKInstance: loader API version {}.{}.{}", VK_VERSION_MAJOR(apiVersion), VK_VERSION_MINOR(apiVersion),
                 VK_VERSION_PATCH(apiVersion));

    // ── available layers / extensions ──────────────────────
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> availLayers(layerCount);
    if (layerCount) vkEnumerateInstanceLayerProperties(&layerCount, availLayers.data());

    uint32_t extCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availExts(extCount);
    if (extCount) vkEnumerateInstanceExtensionProperties(nullptr, &extCount, availExts.data());

    // ── enabled layers ─────────────────────────────────────
    std::vector<const char*> enabledLayers;
    if (enableValidation && HasLayer("VK_LAYER_KHRONOS_validation", availLayers)) {
        enabledLayers.push_back("VK_LAYER_KHRONOS_validation");
        m_validationEnabled = true;
        Logger::Info("VKInstance: validation layer enabled");
    }

    // ── enabled extensions ─────────────────────────────────
    std::vector<const char*> enabledExts = requiredInstanceExts;
    if (m_validationEnabled && HasExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, availExts)) {
        enabledExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    } else {
        m_validationEnabled = false; // can't actually attach messenger
    }

    // ── instance create ────────────────────────────────────
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "NovaGraphics";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "NovaGraphics";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = (apiVersion >= VK_API_VERSION_1_2) ? VK_API_VERSION_1_2 : apiVersion;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &appInfo;
    ici.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size());
    ici.ppEnabledLayerNames = enabledLayers.empty() ? nullptr : enabledLayers.data();
    ici.enabledExtensionCount = static_cast<uint32_t>(enabledExts.size());
    ici.ppEnabledExtensionNames = enabledExts.empty() ? nullptr : enabledExts.data();

    VK_CHECK(vkCreateInstance(&ici, nullptr, &m_instance));
    volkLoadInstance(m_instance);

    // ── debug messenger ────────────────────────────────────
    if (m_validationEnabled && vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT dmci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        dmci.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        dmci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dmci.pfnUserCallback = DebugCallback;
        VK_CHECK_VOID(vkCreateDebugUtilsMessengerEXT(m_instance, &dmci, nullptr, &m_debugMessenger));
    }

    Logger::Info("VKInstance: created (extensions={}, layers={})", enabledExts.size(), enabledLayers.size());
    return true;
}

void VKInstance::Destroy() {
    if (m_debugMessenger && vkDestroyDebugUtilsMessengerEXT) {
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);
        m_debugMessenger = VK_NULL_HANDLE;
    }
    if (m_instance) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
    m_physical = VK_NULL_HANDLE;
}

bool VKInstance::PickPhysicalDevice(VkSurfaceKHR surface, uint32_t& outGraphicsFamily, uint32_t& outPresentFamily) {
    if (!m_instance) return false;

    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(m_instance, &count, nullptr));
    if (count == 0) {
        Logger::Error("VKInstance: no Vulkan-capable physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    VK_CHECK(vkEnumeratePhysicalDevices(m_instance, &count, devices.data()));

    auto scoreDevice = [&](VkPhysicalDevice dev, uint32_t& gfx, uint32_t& present) -> int {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);

        // Must support swapchain extension
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        if (extCount) vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, exts.data());
        bool hasSwapchain = false;
        for (const auto& e : exts) {
            if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                hasSwapchain = true;
                break;
            }
        }
        if (!hasSwapchain) return -1;

        // Find graphics + present queues (may be the same family)
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, qprops.data());

        gfx = UINT32_MAX;
        present = UINT32_MAX;
        for (uint32_t i = 0; i < qCount; ++i) {
            if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) gfx = i;
            if (surface) {
                VkBool32 canPresent = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &canPresent);
                if (canPresent) present = i;
            }
            if (gfx != UINT32_MAX && (present != UINT32_MAX || !surface)) break;
        }
        if (!surface) present = gfx;
        if (gfx == UINT32_MAX || present == UINT32_MAX) return -1;

        int score = 1;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 100;
        return score;
    };

    int bestScore = -1;
    VkPhysicalDevice bestDev = VK_NULL_HANDLE;
    uint32_t bestGfx = 0, bestPresent = 0;
    for (auto dev : devices) {
        uint32_t g = 0, p = 0;
        int s = scoreDevice(dev, g, p);
        if (s > bestScore) {
            bestScore = s;
            bestDev = dev;
            bestGfx = g;
            bestPresent = p;
        }
    }
    if (bestScore < 0 || !bestDev) {
        Logger::Error("VKInstance: no suitable physical device (need graphics+present+swapchain)");
        return false;
    }

    m_physical = bestDev;
    vkGetPhysicalDeviceProperties(m_physical, &m_physicalProps);
    outGraphicsFamily = bestGfx;
    outPresentFamily = bestPresent;
    Logger::Info("VKInstance: picked {} (driver {}, API {}.{}.{})", m_physicalProps.deviceName,
                 m_physicalProps.driverVersion, VK_VERSION_MAJOR(m_physicalProps.apiVersion),
                 VK_VERSION_MINOR(m_physicalProps.apiVersion), VK_VERSION_PATCH(m_physicalProps.apiVersion));
    return true;
}

bool VKInstance::HasInstanceExtension(const char* name) {
    if (!vkEnumerateInstanceExtensionProperties) return false;
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> exts(count);
    if (count && vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data()) != VK_SUCCESS) return false;
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

} // namespace VK
} // namespace Video
} // namespace Nova64
