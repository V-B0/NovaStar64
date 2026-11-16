/**
 * @file VKAccelStruct.cpp
 * @brief Vulkan acceleration structure build implementation.
 */
// SPDX-License-Identifier: MIT

#include "VKAccelStruct.hpp"

#include <cstring>
#include <cstdint>

namespace Nova64 {
namespace Video {
namespace VK {

// --- Helpers ---

static VkDeviceAddress GetBufferDeviceAddress(VkDevice device, VkBuffer buffer) {
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(device, &info);
}

static VkDeviceAddress GetASDeviceAddress(VkDevice device, VkAccelerationStructureKHR as) {
    VkAccelerationStructureDeviceAddressInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    info.accelerationStructure = as;
    return vkGetAccelerationStructureDeviceAddressKHR(device, &info);
}

/// Allocate a VKBufferStorage suitable for an acceleration structure.
static bool AllocASBuffer(const VKContext& ctx, VkDeviceSize size, VKBufferStorage& out) {
    return CreateBufferStorage(
        ctx, size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out);
}

/// Allocate a scratch buffer for AS builds.
static bool AllocScratchBuffer(const VKContext& ctx, VkDeviceSize size, VKBufferStorage& out) {
    return CreateBufferStorage(ctx, size,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out);
}

// --- VKBlas ---

bool VKBlas::Build(const VKContext& ctx, VkCommandBuffer cmd, const float* vertices, uint32_t vertexCount,
                   const uint32_t* indices, uint32_t indexCount) {
    if (!ctx.device || !vertices || vertexCount == 0) return false;

    Destroy(); // clean up any previous state
    m_device = ctx.device;

    // ── 1. Upload vertex data to device-local buffer ─────────────────────
    const VkDeviceSize vertSize = vertexCount * 3 * sizeof(float);
    if (!CreateBufferStorage(ctx, vertSize,
                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_vertBuf)) {
        Logger::Error("VKBlas: failed to allocate vertex buffer");
        return false;
    }
    if (!UploadDeviceLocal(ctx, m_vertBuf, 0, vertices, vertSize)) {
        Logger::Error("VKBlas: failed to upload vertices");
        return false;
    }

    // ── 2. Optionally upload index data ──────────────────────────────────
    const bool hasIndices = (indices && indexCount > 0);
    if (hasIndices) {
        const VkDeviceSize idxSize = indexCount * sizeof(uint32_t);
        if (!CreateBufferStorage(ctx, idxSize,
                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_idxBuf)) {
            Logger::Error("VKBlas: failed to allocate index buffer");
            return false;
        }
        if (!UploadDeviceLocal(ctx, m_idxBuf, 0, indices, idxSize)) {
            Logger::Error("VKBlas: failed to upload indices");
            return false;
        }
    }

    // ── 3. Describe geometry ─────────────────────────────────────────────
    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

    auto& tri = geom.geometry.triangles;
    tri = VkAccelerationStructureGeometryTrianglesDataKHR{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    tri.vertexData.deviceAddress = GetBufferDeviceAddress(ctx.device, m_vertBuf.buffer);
    tri.vertexStride = 3 * sizeof(float);
    tri.maxVertex = vertexCount - 1;
    if (hasIndices) {
        tri.indexType = VK_INDEX_TYPE_UINT32;
        tri.indexData.deviceAddress = GetBufferDeviceAddress(ctx.device, m_idxBuf.buffer);
    } else {
        tri.indexType = VK_INDEX_TYPE_NONE_KHR;
    }

    // ── 4. Get build sizes ────────────────────────────────────────────────
    const uint32_t primitiveCount = hasIndices ? (indexCount / 3) : (vertexCount / 3);
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geom;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(ctx.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
                                            &primitiveCount, &sizeInfo);

    // ── 5. Allocate backing + scratch ─────────────────────────────────────
    if (!AllocASBuffer(ctx, sizeInfo.accelerationStructureSize, m_backing)) {
        Logger::Error("VKBlas: failed to allocate AS backing buffer");
        return false;
    }
    if (!AllocScratchBuffer(ctx, sizeInfo.buildScratchSize, m_scratch)) {
        Logger::Error("VKBlas: failed to allocate scratch buffer");
        return false;
    }

    // ── 6. Create AS handle ───────────────────────────────────────────────
    VkAccelerationStructureCreateInfoKHR asCI{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    asCI.buffer = m_backing.buffer;
    asCI.size = sizeInfo.accelerationStructureSize;
    asCI.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (vkCreateAccelerationStructureKHR(ctx.device, &asCI, nullptr, &m_as) != VK_SUCCESS) {
        Logger::Error("VKBlas: vkCreateAccelerationStructureKHR failed");
        return false;
    }
    m_deviceAddress = GetASDeviceAddress(ctx.device, m_as);

    // ── 7. Record build into cmd ──────────────────────────────────────────
    buildInfo.dstAccelerationStructure = m_as;
    buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(ctx.device, m_scratch.buffer);

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primitiveCount;
    range.primitiveOffset = 0;
    range.firstVertex = 0;
    range.transformOffset = 0;
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRange);

    // Memory barrier: AS build writes must be visible before TLAS build reads.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &mb, 0, nullptr, 0, nullptr);

    return true;
}

void VKBlas::Destroy() {
    if (!m_device) return;
    if (m_as) {
        vkDestroyAccelerationStructureKHR(m_device, m_as, nullptr);
        m_as = VK_NULL_HANDLE;
    }
    // We can't call DestroyBufferStorage without a VKContext. Use raw vk calls.
    auto destroyBuf = [&](VKBufferStorage& s) {
        if (s.buffer) {
            vkDestroyBuffer(m_device, s.buffer, nullptr);
            s.buffer = VK_NULL_HANDLE;
        }
        if (s.memory) {
            vkFreeMemory(m_device, s.memory, nullptr);
            s.memory = VK_NULL_HANDLE;
        }
    };
    destroyBuf(m_backing);
    destroyBuf(m_scratch);
    destroyBuf(m_vertBuf);
    destroyBuf(m_idxBuf);
    m_deviceAddress = 0;
    m_device = VK_NULL_HANDLE;
}

// --- VKTlas ---

bool VKTlas::Build(const VKContext& ctx, VkCommandBuffer cmd, const std::vector<TlasInstance>& instances) {
    if (!ctx.device || instances.empty()) return false;

    Destroy();
    m_device = ctx.device;

    // ── 1. Convert TlasInstance -> VkAccelerationStructureInstanceKHR ────
    std::vector<VkAccelerationStructureInstanceKHR> vkInst;
    vkInst.reserve(instances.size());
    for (const auto& ti : instances) {
        VkAccelerationStructureInstanceKHR vi{};
        // Row-major 3×4 float array → VkTransformMatrixKHR (row-major 3×4)
        static_assert(sizeof(ti.transform) == sizeof(vi.transform), "transform size mismatch");
        std::memcpy(&vi.transform, ti.transform, sizeof(vi.transform));
        vi.instanceCustomIndex = ti.instanceCustomIndex & 0xFFFFFF;
        vi.mask = ti.mask & 0xFF;
        vi.instanceShaderBindingTableRecordOffset = ti.hitGroupIndex & 0xFFFFFF;
        vi.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        vi.accelerationStructureReference = ti.blasDeviceAddress;
        vkInst.push_back(vi);
    }
    const VkDeviceSize instSize = vkInst.size() * sizeof(VkAccelerationStructureInstanceKHR);

    // ── 2. Upload instance buffer ─────────────────────────────────────────
    if (!CreateBufferStorage(ctx, instSize,
                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_instanceBuf)) {
        Logger::Error("VKTlas: failed to allocate instance buffer");
        return false;
    }
    if (!UploadDeviceLocal(ctx, m_instanceBuf, 0, vkInst.data(), instSize)) {
        Logger::Error("VKTlas: failed to upload instances");
        return false;
    }

    // ── 3. Describe geometry ──────────────────────────────────────────────
    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances = VkAccelerationStructureGeometryInstancesDataKHR{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    geom.geometry.instances.arrayOfPointers = VK_FALSE;
    geom.geometry.instances.data.deviceAddress = GetBufferDeviceAddress(ctx.device, m_instanceBuf.buffer);

    // ── 4. Get build sizes ────────────────────────────────────────────────
    const uint32_t instanceCount = static_cast<uint32_t>(vkInst.size());
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geom;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(ctx.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
                                            &instanceCount, &sizeInfo);

    // ── 5. Allocate backing + scratch ──────────────────────────────────────
    if (!AllocASBuffer(ctx, sizeInfo.accelerationStructureSize, m_backing)) {
        Logger::Error("VKTlas: failed to allocate AS backing buffer");
        return false;
    }
    if (!AllocScratchBuffer(ctx, sizeInfo.buildScratchSize, m_scratch)) {
        Logger::Error("VKTlas: failed to allocate scratch buffer");
        return false;
    }

    // ── 6. Create AS handle ────────────────────────────────────────────────
    VkAccelerationStructureCreateInfoKHR asCI{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    asCI.buffer = m_backing.buffer;
    asCI.size = sizeInfo.accelerationStructureSize;
    asCI.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (vkCreateAccelerationStructureKHR(ctx.device, &asCI, nullptr, &m_as) != VK_SUCCESS) {
        Logger::Error("VKTlas: vkCreateAccelerationStructureKHR failed");
        return false;
    }
    m_deviceAddress = GetASDeviceAddress(ctx.device, m_as);

    // ── 7. Record build ────────────────────────────────────────────────────
    buildInfo.dstAccelerationStructure = m_as;
    buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(ctx.device, m_scratch.buffer);

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = instanceCount;
    range.primitiveOffset = 0;
    range.firstVertex = 0;
    range.transformOffset = 0;
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRange);

    // Barrier: TLAS write visible to ray-tracing stage.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 1, &mb, 0, nullptr, 0, nullptr);

    return true;
}

void VKTlas::Destroy() {
    if (!m_device) return;
    if (m_as) {
        vkDestroyAccelerationStructureKHR(m_device, m_as, nullptr);
        m_as = VK_NULL_HANDLE;
    }
    auto destroyBuf = [&](VKBufferStorage& s) {
        if (s.buffer) {
            vkDestroyBuffer(m_device, s.buffer, nullptr);
            s.buffer = VK_NULL_HANDLE;
        }
        if (s.memory) {
            vkFreeMemory(m_device, s.memory, nullptr);
            s.memory = VK_NULL_HANDLE;
        }
    };
    destroyBuf(m_backing);
    destroyBuf(m_scratch);
    destroyBuf(m_instanceBuf);
    m_deviceAddress = 0;
    m_device = VK_NULL_HANDLE;
}

} // namespace VK
} // namespace Video
} // namespace Nova64
