/**
 * @file VKAccelStruct.hpp
 * @brief Vulkan KHR acceleration structure wrappers for hardware ray tracing.
 *
 * VKBlas  — Bottom-Level Acceleration Structure (one mesh / geometry group).
 * VKTlas  — Top-Level Acceleration Structure (scene of BLAS instances).
 *
 * Both are built on the GPU via recorded command buffers.  Callers must
 * ensure the command buffer is submitted and the queue is idle before reading
 * any RT results.
 */
// SPDX-License-Identifier: MIT
#pragma once

#include "VKCommon.hpp"
#include "VKMemory.hpp"
#include "VKBuffer.hpp"

#include <vector>
#include <cstdint>

namespace Nova64 {
namespace Video {
namespace VK {

// --- VKBlas  —  Bottom-Level Acceleration Structure ---

class VKBlas {
public:
    VKBlas() = default;
    ~VKBlas() { Destroy(); }

    VKBlas(const VKBlas&) = delete;
    VKBlas& operator=(const VKBlas&) = delete;

    /**
     * Build a BLAS from flat interleaved vertex data.
     *
     * @param ctx      Vulkan context (device, physical, pool, queue).
     * @param cmd      Open command buffer to record build commands into.
     * @param vertices Pointer to packed float3 position data (stride=12 bytes).
     * @param vertexCount  Number of vertices.
     * @param indices  Optional index buffer (uint32_t).  Pass nullptr for non-indexed.
     * @param indexCount   Number of indices (0 when indices==nullptr).
     */
    bool Build(const VKContext& ctx, VkCommandBuffer cmd, const float* vertices, uint32_t vertexCount,
               const uint32_t* indices, uint32_t indexCount);

    void Destroy();

    VkAccelerationStructureKHR Get() const { return m_as; }
    VkDeviceAddress GetDeviceAddress() const { return m_deviceAddress; }
    bool IsBuilt() const { return m_as != VK_NULL_HANDLE; }

private:
    VkAccelerationStructureKHR m_as = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VKBufferStorage m_backing{}; // AS buffer
    VKBufferStorage m_scratch{}; // scratch buffer (freed after build)
    VKBufferStorage m_vertBuf{}; // uploaded vertices
    VKBufferStorage m_idxBuf{};  // uploaded indices (may be empty)
    VkDeviceAddress m_deviceAddress = 0;
};

// --- VKTlas  —  Top-Level Acceleration Structure ---

struct TlasInstance {
    VkDeviceAddress blasDeviceAddress;
    uint32_t instanceCustomIndex; // gl_InstanceCustomIndexEXT in shaders
    uint32_t mask;                // 0xFF = visible to all rays
    uint32_t hitGroupIndex;       // SBT offset into hit groups
    float transform[12];          // row-major 3×4 transform (identity = {1,0,0,0, 0,1,0,0, 0,0,1,0})
};

class VKTlas {
public:
    VKTlas() = default;
    ~VKTlas() { Destroy(); }

    VKTlas(const VKTlas&) = delete;
    VKTlas& operator=(const VKTlas&) = delete;

    /**
     * Build or rebuild the TLAS from a list of BLAS instances.
     * Safe to call each frame (destroys and recreates internal resources).
     */
    bool Build(const VKContext& ctx, VkCommandBuffer cmd, const std::vector<TlasInstance>& instances);

    void Destroy();

    VkAccelerationStructureKHR Get() const { return m_as; }
    VkDeviceAddress GetDeviceAddress() const { return m_deviceAddress; }
    bool IsBuilt() const { return m_as != VK_NULL_HANDLE; }

private:
    VkAccelerationStructureKHR m_as = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VKBufferStorage m_backing{};
    VKBufferStorage m_scratch{};
    VKBufferStorage m_instanceBuf{};
    VkDeviceAddress m_deviceAddress = 0;
};

} // namespace VK
} // namespace Video
} // namespace Nova64
