/**
 * @file VKInstance.hpp
 * @brief Vulkan instance + physical device selection.
 */
// SPDX-License-Identifier: MIT
#pragma once

#include "VKCommon.hpp"

#include <vector>

namespace Nova64 { namespace Video { namespace VK {

class VKInstance {
public:
    /// Initialize volk and create the VkInstance.
    /// @param requiredInstanceExts Extensions queried from SDL_Vulkan (surface, platform-surface).
    /// @param enableValidation If true, attempts to enable VK_LAYER_KHRONOS_validation.
    bool Create(const std::vector<const char*>& requiredInstanceExts, bool enableValidation);
    void Destroy();

    /// Pick a discrete GPU (preferred) or first available physical device that
    /// supports graphics+present on the given surface and the swapchain extension.
    bool PickPhysicalDevice(VkSurfaceKHR surface,
                            uint32_t& outGraphicsFamily,
                            uint32_t& outPresentFamily);

    VkInstance       Get()         const { return m_instance; }
    VkPhysicalDevice GetPhysical() const { return m_physical; }
    const VkPhysicalDeviceProperties& GetProps() const { return m_physicalProps; }
    bool             IsValidationEnabled() const { return m_validationEnabled; }

private:
    bool LoadAndCheckLayer(const char* name);

    VkInstance               m_instance       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkPhysicalDevice         m_physical       = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties m_physicalProps{};
    bool                     m_validationEnabled = false;
    bool                     m_volkInitialized   = false;
};

}}} // namespace Nova64::Video::VK
