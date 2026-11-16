/**
 * @file VKMemory.cpp
 */
// SPDX-License-Identifier: MIT
#include "VKMemory.hpp"
#include <chrono>

namespace Nova64 {
namespace Video {
namespace VK {

uint32_t FindMemoryType(VkPhysicalDevice physical, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        const bool typeOk = (typeBits & (1u << i)) != 0;
        const bool propOk = (mp.memoryTypes[i].propertyFlags & props) == props;
        if (typeOk && propOk) return i;
    }
    Logger::Error("VK: no memory type matches typeBits=0x{:x} props=0x{:x}", typeBits, static_cast<uint32_t>(props));
    return UINT32_MAX;
}

VkCommandBuffer BeginOneShot(VkDevice device, VkCommandPool pool) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &ai, &cmd) != VK_SUCCESS) {
        Logger::Error("VK: BeginOneShot vkAllocateCommandBuffers failed");
        return VK_NULL_HANDLE;
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        Logger::Error("VK: BeginOneShot vkBeginCommandBuffer failed");
        vkFreeCommandBuffers(device, pool, 1, &cmd);
        return VK_NULL_HANDLE;
    }
    return cmd;
}

void EndOneShotAndWait(const VKContext& ctx, VkCommandBuffer cmd) {
    if (!cmd) return;
    // Fast-path: device is already dead — just release the command buffer.
    if (ctx.deviceLost.load(std::memory_order_relaxed)) {
        vkFreeCommandBuffers(ctx.device, ctx.transientPool, 1, &cmd);
        return;
    }
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        Logger::Error("VK: EndOneShot vkEndCommandBuffer failed");
        vkFreeCommandBuffers(ctx.device, ctx.transientPool, 1, &cmd);
        return;
    }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkResult r = vkQueueSubmit(ctx.transferQueue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        Logger::Error("VK: EndOneShot vkQueueSubmit failed: {}", VkResultToString(r));
        if (r == VK_ERROR_DEVICE_LOST) ctx.deviceLost.store(true, std::memory_order_relaxed);
    } else {
        auto t0 = std::chrono::steady_clock::now();
        vkQueueWaitIdle(ctx.transferQueue);
        auto dtMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        if (dtMs > 50) {
            Logger::Warn("VK: EndOneShot vkQueueWaitIdle took {}ms", dtMs);
        }
    }
    vkFreeCommandBuffers(ctx.device, ctx.transientPool, 1, &cmd);
}

} // namespace VK
} // namespace Video
} // namespace Nova64
