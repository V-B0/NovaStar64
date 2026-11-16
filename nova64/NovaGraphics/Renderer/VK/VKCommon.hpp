/**
 * @file VKCommon.hpp
 * @brief Shared Vulkan helpers (volk include, error macro).
 */
// SPDX-License-Identifier: MIT
#pragma once

// volk pulls in <vulkan/vulkan.h> with VK_NO_PROTOTYPES and provides
// global function pointers populated by volkInitialize / volkLoadInstance /
// volkLoadDevice. This is set up project-wide; volk must be initialized
// once on backend startup before any vk* call.
#include <volk.h>

#include "../../Common/Logger.hpp"

#include <cstdint>
#include <string>

namespace Nova64 {
namespace Video {
namespace VK {

inline const char* VkResultToString(VkResult r) {
    switch (r) {
    case VK_SUCCESS:                        return "VK_SUCCESS";
    case VK_NOT_READY:                      return "VK_NOT_READY";
    case VK_TIMEOUT:                        return "VK_TIMEOUT";
    case VK_EVENT_SET:                      return "VK_EVENT_SET";
    case VK_EVENT_RESET:                    return "VK_EVENT_RESET";
    case VK_INCOMPLETE:                     return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:         return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:          return "VK_ERROR_FRAGMENTED_POOL";
    case (VkResult)(-1000069000):           return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    default:                                return "VK_<unknown>";
    }
}

#define VK_CHECK(expr)                                                                                                 \
    do {                                                                                                               \
        VkResult _vkr = (expr);                                                                                        \
        if (_vkr != VK_SUCCESS) {                                                                                      \
            Logger::Error("Vulkan call failed: {} -> {} at {}:{}", #expr, ::Nova64::Video::VK::VkResultToString(_vkr), \
                          __FILE__, __LINE__);                                                                         \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

#define VK_CHECK_VOID(expr)                                                                                            \
    do {                                                                                                               \
        VkResult _vkr = (expr);                                                                                        \
        if (_vkr != VK_SUCCESS) {                                                                                      \
            Logger::Error("Vulkan call failed: {} -> {} at {}:{}", #expr, ::Nova64::Video::VK::VkResultToString(_vkr), \
                          __FILE__, __LINE__);                                                                         \
        }                                                                                                              \
    } while (0)

} // namespace VK
} // namespace Video
} // namespace Nova64
