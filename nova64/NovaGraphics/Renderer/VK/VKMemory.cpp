/**
 * @file VKMemory.cpp
 */
// SPDX-License-Identifier: MIT
#include "VKMemory.hpp"

namespace Nova64 { namespace Video { namespace VK {

uint32_t FindMemoryType(VkPhysicalDevice physical,
                        uint32_t typeBits,
                        VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        const bool typeOk = (typeBits & (1u << i)) != 0;
        const bool propOk = (mp.memoryTypes[i].propertyFlags & props) == props;
        if (typeOk && propOk) return i;
    }
    Logger::Error("VK: no memory type matches typeBits=0x{:x} props=0x{:x}",
                  typeBits, static_cast<uint32_t>(props));
    return UINT32_MAX;
}

VkCommandBuffer BeginOneShot(VkDevice device, VkCommandPool pool) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = pool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
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

void EndOneShotAndWait(VkDevice device, VkCommandPool pool,
                       VkQueue queue, VkCommandBuffer cmd) {
    if (!cmd) return;
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        Logger::Error("VK: EndOneShot vkEndCommandBuffer failed");
        vkFreeCommandBuffers(device, pool, 1, &cmd);
        return;
    }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    VkResult r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        Logger::Error("VK: EndOneShot vkQueueSubmit failed: {}", VkResultToString(r));
    } else {
        vkQueueWaitIdle(queue);
    }
    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

}}} // namespace Nova64::Video::VK
