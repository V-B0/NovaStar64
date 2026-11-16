/**
 * @file VKBuffer.cpp
 */
// SPDX-License-Identifier: MIT
#include "VKBuffer.hpp"

#include <cstring>

namespace Nova64 {
namespace Video {
namespace VK {

// --- Storage helpers ---

bool CreateBufferStorage(const VKContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                         VKBufferStorage& out) {
    out = {};
    if (size == 0) {
        Logger::Warn("VK: CreateBufferStorage size=0");
        return false;
    }

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(ctx.device, &bi, nullptr, &out.buffer) != VK_SUCCESS) {
        Logger::Error("VK: vkCreateBuffer failed (size={})", static_cast<uint64_t>(size));
        return false;
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(ctx.device, out.buffer, &req);

    uint32_t typeIdx = FindMemoryType(ctx.physical, req.memoryTypeBits, props);
    if (typeIdx == UINT32_MAX) {
        vkDestroyBuffer(ctx.device, out.buffer, nullptr);
        out = {};
        return false;
    }

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = typeIdx;
    if (vkAllocateMemory(ctx.device, &ai, nullptr, &out.memory) != VK_SUCCESS) {
        Logger::Error("VK: vkAllocateMemory failed (size={})", static_cast<uint64_t>(req.size));
        vkDestroyBuffer(ctx.device, out.buffer, nullptr);
        out = {};
        return false;
    }

    if (vkBindBufferMemory(ctx.device, out.buffer, out.memory, 0) != VK_SUCCESS) {
        Logger::Error("VK: vkBindBufferMemory failed");
        vkFreeMemory(ctx.device, out.memory, nullptr);
        vkDestroyBuffer(ctx.device, out.buffer, nullptr);
        out = {};
        return false;
    }

    out.size = size;

    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        if (vkMapMemory(ctx.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) != VK_SUCCESS) {
            Logger::Error("VK: vkMapMemory failed");
            vkFreeMemory(ctx.device, out.memory, nullptr);
            vkDestroyBuffer(ctx.device, out.buffer, nullptr);
            out = {};
            return false;
        }
    }
    return true;
}

void DestroyBufferStorage(const VKContext& ctx, VKBufferStorage& s) {
    if (s.mapped) {
        vkUnmapMemory(ctx.device, s.memory);
        s.mapped = nullptr;
    }
    if (s.buffer) {
        vkDestroyBuffer(ctx.device, s.buffer, nullptr);
        s.buffer = VK_NULL_HANDLE;
    }
    if (s.memory) {
        vkFreeMemory(ctx.device, s.memory, nullptr);
        s.memory = VK_NULL_HANDLE;
    }
    s.size = 0;
}

bool UploadDeviceLocal(const VKContext& ctx, VKBufferStorage& dst, VkDeviceSize dstOffset, const void* data,
                       VkDeviceSize size) {
    if (!data || size == 0 || !dst.buffer) return false;
    if (dstOffset + size > dst.size) {
        Logger::Error("VK: UploadDeviceLocal out of range ({}+{} > {})", static_cast<uint64_t>(dstOffset),
                      static_cast<uint64_t>(size), static_cast<uint64_t>(dst.size));
        return false;
    }

    VKBufferStorage staging{};
    if (!CreateBufferStorage(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)) {
        return false;
    }
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));

    VkCommandBuffer cmd = BeginOneShot(ctx.device, ctx.transientPool);
    if (!cmd) {
        DestroyBufferStorage(ctx, staging);
        return false;
    }
    VkBufferCopy copy{};
    copy.srcOffset = 0;
    copy.dstOffset = dstOffset;
    copy.size = size;
    vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &copy);
    EndOneShotAndWait(ctx, cmd);

    DestroyBufferStorage(ctx, staging);
    return true;
}

// --- Helper: pick memory props + extra usage based on BufferUsage ---

namespace {

void PickAlloc(BufferUsage u, VkBufferUsageFlags baseUsage, VkBufferUsageFlags& outUsage,
               VkMemoryPropertyFlags& outProps) {
    if (u == BufferUsage::Static) {
        outUsage = baseUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        outProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    } else {
        outUsage = baseUsage;
        outProps = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
}

bool DoUpload(const VKContext& ctx, VKBufferStorage& s, BufferUsage usage, const void* data, size_t size,
              size_t offset) {
    if (!data || size == 0) return false;
    if (offset + size > s.size) {
        Logger::Error("VK buffer upload out of range");
        return false;
    }
    if (s.mapped) {
        std::memcpy(static_cast<uint8_t*>(s.mapped) + offset, data, size);
        return true;
    }
    // device-local path
    return UploadDeviceLocal(ctx, s, offset, data, size);
    (void)usage;
}

} // namespace

// --- VKVertexBufferImpl ---

VKVertexBufferImpl::VKVertexBufferImpl(const VKContext* ctx, size_t size, BufferUsage usage) :
    m_ctx(ctx), m_size(size), m_usage(usage) {
    VkBufferUsageFlags u;
    VkMemoryPropertyFlags p;
    PickAlloc(usage, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, u, p);
    CreateBufferStorage(*m_ctx, size, u, p, m_storage);
}

VKVertexBufferImpl::~VKVertexBufferImpl() { DestroyBufferStorage(*m_ctx, m_storage); }

void VKVertexBufferImpl::Upload(const void* data, size_t sizeBytes) {
    DoUpload(*m_ctx, m_storage, m_usage, data, sizeBytes, 0);
}

void VKVertexBufferImpl::Update(const void* data, size_t offset, size_t sizeBytes) {
    DoUpload(*m_ctx, m_storage, m_usage, data, sizeBytes, offset);
}

// --- VKIndexBufferImpl ---

VKIndexBufferImpl::VKIndexBufferImpl(const VKContext* ctx, size_t size, BufferUsage usage) :
    m_ctx(ctx), m_size(size), m_usage(usage) {
    VkBufferUsageFlags u;
    VkMemoryPropertyFlags p;
    PickAlloc(usage, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, u, p);
    CreateBufferStorage(*m_ctx, size, u, p, m_storage);
}

VKIndexBufferImpl::~VKIndexBufferImpl() { DestroyBufferStorage(*m_ctx, m_storage); }

void VKIndexBufferImpl::Upload(const void* data, size_t sizeBytes, IndexType type) {
    m_indexType = type;
    m_count = (type == IndexType::UInt16) ? sizeBytes / 2 : sizeBytes / 4;
    DoUpload(*m_ctx, m_storage, m_usage, data, sizeBytes, 0);
}

void VKIndexBufferImpl::Update(const void* data, size_t offset, size_t sizeBytes) {
    DoUpload(*m_ctx, m_storage, m_usage, data, sizeBytes, offset);
}

// --- VKUniformBufferImpl  (always HOST_VISIBLE + persistently mapped) ---

VKUniformBufferImpl::VKUniformBufferImpl(const VKContext* ctx, size_t size) : m_ctx(ctx), m_size(size) {
    CreateBufferStorage(*m_ctx, size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, m_storage);
}

VKUniformBufferImpl::~VKUniformBufferImpl() { DestroyBufferStorage(*m_ctx, m_storage); }

void VKUniformBufferImpl::Upload(const void* data, size_t sizeBytes) {
    if (!data || sizeBytes == 0 || !m_storage.mapped) return;
    if (sizeBytes > m_storage.size) sizeBytes = static_cast<size_t>(m_storage.size);
    std::memcpy(m_storage.mapped, data, sizeBytes);
}

void VKUniformBufferImpl::Update(const void* data, size_t offset, size_t sizeBytes) {
    if (!data || sizeBytes == 0 || !m_storage.mapped) return;
    if (offset + sizeBytes > m_storage.size) return;
    std::memcpy(static_cast<uint8_t*>(m_storage.mapped) + offset, data, sizeBytes);
}

} // namespace VK
} // namespace Video
} // namespace Nova64
