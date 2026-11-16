/**
 * @file VKMemory.hpp
 * @brief Lightweight helpers for memory-type lookup and one-shot command buffers.
 *
 * Every resource gets its own VkDeviceMemory allocation (no sub-allocator).
 * This is suboptimal in steady state but simple, dependency-free and easy to
 * replace with VMA.
 */
// SPDX-License-Identifier: MIT
#pragma once

#include "VKCommon.hpp"
#include <atomic>

namespace Nova64 {
namespace Video {
namespace VK {

/// Bundle of handles every resource needs. Owned by VulkanRenderer; passed by
/// pointer to each resource. Stable for the lifetime of the renderer.
struct VKContext {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue transferQueue = VK_NULL_HANDLE;         // = graphics queue
    VkCommandPool transientPool = VK_NULL_HANDLE;   // for one-shot uploads
    VkPipelineCache pipelineCache = VK_NULL_HANDLE; // shared cache (optional)

    /// Set to true when vkAcquireNextImageKHR / vkQueueSubmit /
    /// vkQueuePresentKHR returns VK_ERROR_DEVICE_LOST.  All GPU operations
    /// check this flag and bail out immediately to avoid flooding the log
    /// with hundreds of DEVICE_LOST errors per second (e.g. async texture
    /// loading that keeps firing after the device is gone).
    /// Mutable so it can be set through a const VKContext* (e.g. resource classes).
    mutable std::atomic<bool> deviceLost{false};

    /// Reset all handles and status flags to their default null values.
    void Reset() {
        physical = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
        transferQueue = VK_NULL_HANDLE;
        transientPool = VK_NULL_HANDLE;
        pipelineCache = VK_NULL_HANDLE;
        deviceLost.store(false, std::memory_order_relaxed);
    }
};

/// Find a memory type matching `typeBits` (from VkMemoryRequirements) and `props`.
/// Returns UINT32_MAX on failure (logged).
uint32_t FindMemoryType(VkPhysicalDevice physical, uint32_t typeBits, VkMemoryPropertyFlags props);

/// Allocate a one-shot primary command buffer from `pool` and begin recording
/// with ONE_TIME_SUBMIT. Returns VK_NULL_HANDLE on failure.
VkCommandBuffer BeginOneShot(VkDevice device, VkCommandPool pool);

/// End the command buffer, submit on `queue`, vkQueueWaitIdle, then free it.
/// If `ctx.deviceLost` is already true the command buffer is just freed.
/// On VK_ERROR_DEVICE_LOST, `ctx.deviceLost` is set to true.
void EndOneShotAndWait(const VKContext& ctx, VkCommandBuffer cmd);

} // namespace VK
} // namespace Video
} // namespace Nova64
