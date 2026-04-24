/**
 * @file VKMemory.hpp
 * @brief Lightweight helpers for memory-type lookup and one-shot command buffers.
 *
 * Stage 2 uses a per-resource VkDeviceMemory allocation (no sub-allocator).
 * This is suboptimal in steady state but trivially correct, dependency-free,
 * and easy to replace with VMA later.
 */
// SPDX-License-Identifier: MIT
#pragma once

#include "VKCommon.hpp"

namespace Nova64 { namespace Video { namespace VK {

/// Bundle of handles every resource needs. Owned by VulkanRenderer; passed by
/// pointer to each resource. Stable for the lifetime of the renderer.
struct VKContext {
    VkPhysicalDevice physical      = VK_NULL_HANDLE;
    VkDevice         device        = VK_NULL_HANDLE;
    VkQueue          transferQueue = VK_NULL_HANDLE; // = graphics queue
    VkCommandPool    transientPool = VK_NULL_HANDLE; // for one-shot uploads
    VkPipelineCache  pipelineCache = VK_NULL_HANDLE; // shared cache (optional)
};

/// Find a memory type matching `typeBits` (from VkMemoryRequirements) and `props`.
/// Returns UINT32_MAX on failure (logged).
uint32_t FindMemoryType(VkPhysicalDevice physical,
                        uint32_t typeBits,
                        VkMemoryPropertyFlags props);

/// Allocate a one-shot primary command buffer from `pool` and begin recording
/// with ONE_TIME_SUBMIT. Returns VK_NULL_HANDLE on failure.
VkCommandBuffer BeginOneShot(VkDevice device, VkCommandPool pool);

/// End the command buffer, submit on `queue`, vkQueueWaitIdle, then free it.
void EndOneShotAndWait(VkDevice device, VkCommandPool pool,
                       VkQueue queue, VkCommandBuffer cmd);

}}} // namespace Nova64::Video::VK
