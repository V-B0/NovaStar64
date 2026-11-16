/**
 * @file VKBuffer.hpp
 * @brief Vulkan-backed IVertexBuffer / IIndexBuffer / IUniformBuffer.
 *
 * Allocation strategy:
 *   * `BufferUsage::Static`  → DEVICE_LOCAL; uploads via a staging buffer
 *                              with a one-shot transfer + queue wait-idle.
 *   * `BufferUsage::Dynamic` / `Stream` → HOST_VISIBLE | HOST_COHERENT,
 *                              persistently mapped, written directly.
 */
// SPDX-License-Identifier: MIT
#pragma once

#include "VKCommon.hpp"
#include "VKMemory.hpp"
#include "../Renderer.hpp"

namespace Nova64 {
namespace Video {
namespace VK {

// Global current-UBO slot table. Mirrors the OpenGL-ish "BindSlot" pattern:
// IUniformBuffer::BindSlot(slot) writes the buffer here; VulkanRenderer reads
// the slot used for descriptor binding 0 at draw time. Set 0 is the standard
// "per-draw" UBO; engine code uses BindSlot(0) for that.
constexpr uint32_t kVKMaxUBOSlots = 8;

struct VKUBOSlot {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

inline VKUBOSlot g_boundUBOs[kVKMaxUBOSlots] = {};

/// Common storage for any GPU buffer. Not exposed to clients directly.
struct VKBufferStorage {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr; // non-null only for HOST_VISIBLE buffers
};

/// Allocate `size` bytes with `usage` and `props`. On success populates `out`.
/// Maps the memory if HOST_VISIBLE is set in `props`.
bool CreateBufferStorage(const VKContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                         VKBufferStorage& out);

void DestroyBufferStorage(const VKContext& ctx, VKBufferStorage& s);

/// Copy `data` into a DEVICE_LOCAL buffer via a staging buffer.
/// `dstOffset` is bytes into `dst.buffer`.
bool UploadDeviceLocal(const VKContext& ctx, VKBufferStorage& dst, VkDeviceSize dstOffset, const void* data,
                       VkDeviceSize size);

class VKVertexBufferImpl final : public IVertexBuffer {
public:
    VKVertexBufferImpl(const VKContext* ctx, size_t size, BufferUsage usage);
    ~VKVertexBufferImpl() override;

    void Upload(const void* data, size_t sizeBytes) override;
    void Update(const void* data, size_t offset, size_t sizeBytes) override;
    size_t GetSize() const override { return m_size; }
    BufferUsage GetUsage() const override { return m_usage; }

    VkBuffer GetVk() const { return m_storage.buffer; }

private:
    const VKContext* m_ctx;
    VKBufferStorage m_storage{};
    size_t m_size;
    BufferUsage m_usage;
};

class VKIndexBufferImpl final : public IIndexBuffer {
public:
    VKIndexBufferImpl(const VKContext* ctx, size_t size, BufferUsage usage);
    ~VKIndexBufferImpl() override;

    void Upload(const void* data, size_t sizeBytes, IndexType type) override;
    void Update(const void* data, size_t offset, size_t sizeBytes) override;
    size_t GetCount() const override { return m_count; }
    IndexType GetIndexType() const override { return m_indexType; }

    VkBuffer GetVk() const { return m_storage.buffer; }

private:
    const VKContext* m_ctx;
    VKBufferStorage m_storage{};
    [[maybe_unused]] size_t m_size;
    size_t m_count = 0;
    IndexType m_indexType = IndexType::UInt16;
    BufferUsage m_usage;
};

class VKUniformBufferImpl final : public IUniformBuffer {
public:
    VKUniformBufferImpl(const VKContext* ctx, size_t size);
    ~VKUniformBufferImpl() override;

    void Upload(const void* data, size_t sizeBytes) override;
    void Update(const void* data, size_t offset, size_t sizeBytes) override;
    void BindSlot(uint32_t slot) override {
        m_slot = slot;
        if (slot < kVKMaxUBOSlots) {
            g_boundUBOs[slot] = {m_storage.buffer, m_size};
        }
    }
    size_t GetSize() const override { return m_size; }

    VkBuffer GetVk() const { return m_storage.buffer; }
    uint32_t GetSlot() const { return m_slot; }

private:
    const VKContext* m_ctx;
    VKBufferStorage m_storage{};
    size_t m_size;
    uint32_t m_slot = 0;
};

} // namespace VK
} // namespace Video
} // namespace Nova64
