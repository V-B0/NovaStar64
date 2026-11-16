/**
 * @file Memory.hpp
 * @brief Custom memory allocators and tracking for NovaGraphics
 *
 * Provides pool allocators (fixed-size blocks for vertices/tiles), a linear
 * allocator (frame-scoped scratch memory), alignment-aware allocation,
 * and, in debug builds, an allocation tracker that can report leaks.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>

namespace Nova64::Video {

// --- Allocation Stats ---

struct AllocStats {
    std::atomic<uint64_t> totalAllocations{0};
    std::atomic<uint64_t> totalDeallocations{0};
    std::atomic<uint64_t> totalBytesAllocated{0};
    std::atomic<uint64_t> currentBytesInUse{0};
    std::atomic<uint64_t> peakBytesInUse{0};
    std::atomic<uint64_t> poolAllocations{0};
    std::atomic<uint64_t> poolDeallocations{0};
    std::atomic<uint64_t> linearAllocations{0};
    std::atomic<uint64_t> linearResets{0};
};

extern AllocStats g_allocStats;

// --- Aligned Allocation Helpers ---

/// Allocate aligned memory (alignment must be power of 2)
void* AlignedAlloc(size_t size, size_t alignment = 64);

void AlignedFree(void* ptr);

/// Tracked allocation (updates g_allocStats)
void* TrackedAlloc(size_t size, size_t alignment = 16);
void TrackedFree(void* ptr, size_t size);

// --- Pool Allocator — Fixed-size blocks, O(1) alloc/free, no fragmentation ---

class PoolAllocator {
public:
    PoolAllocator() = default;
    ~PoolAllocator();

    /// Initialize pool with `blockCount` blocks of `blockSize` bytes each
    bool Init(size_t blockSize, size_t blockCount, size_t alignment = 16);

    /// Destroy pool and release backing memory
    void Destroy();

    /// Allocate one block (returns nullptr if pool exhausted)
    void* Alloc() noexcept;

    /// Free one block back to pool
    void Free(void* ptr) noexcept;

    /// Reset all blocks to free (does not call destructors)
    void Reset() noexcept;

    size_t GetBlockSize() const noexcept { return m_blockSize; }
    size_t GetBlockCount() const noexcept { return m_blockCount; }
    size_t GetFreeCount() const noexcept { return m_freeCount; }
    size_t GetUsedCount() const noexcept { return m_blockCount - m_freeCount; }
    size_t GetTotalBytes() const noexcept { return m_totalBytes; }
    bool IsInitialized() const noexcept { return m_memory != nullptr; }

private:
    struct FreeNode {
        FreeNode* next;
    };

    uint8_t* m_memory = nullptr;
    FreeNode* m_freeList = nullptr;
    size_t m_blockSize = 0;
    size_t m_blockCount = 0;
    size_t m_freeCount = 0;
    size_t m_totalBytes = 0;
    size_t m_alignment = 16;
};

// --- Linear Allocator — Bump pointer, reset per frame, zero fragmentation ---

class LinearAllocator {
public:
    LinearAllocator() = default;
    ~LinearAllocator();

    /// Initialize with a fixed capacity
    bool Init(size_t capacity, size_t alignment = 16);

    void Destroy();

    /// Allocate `size` bytes (bump pointer forward)
    void* Alloc(size_t size, size_t alignment = 16) noexcept;

    /// Reset pointer to beginning (invalidates all previous allocations)
    void Reset() noexcept;

    size_t GetCapacity() const noexcept { return m_capacity; }
    size_t GetUsed() const noexcept { return m_offset; }
    size_t GetRemaining() const noexcept { return m_capacity - m_offset; }
    bool IsInitialized() const noexcept { return m_memory != nullptr; }

    /// Allocates uninitialised storage for `count` objects of T.
    template <typename T> T* AllocArray(size_t count) { return static_cast<T*>(Alloc(sizeof(T) * count, alignof(T))); }

private:
    uint8_t* m_memory = nullptr;
    size_t m_capacity = 0;
    size_t m_offset = 0;
    size_t m_alignment = 16;
};

// --- Ring Buffer — Circular buffer for streaming vertex data to GPU ---

class CPURingBuffer {
public:
    CPURingBuffer() = default;
    ~CPURingBuffer();

    bool Init(size_t capacity);
    void Destroy();

    /// Reserve space and return pointer. Returns nullptr if not enough room.
    void* Reserve(size_t size) noexcept;

    /// Mark space as consumed (after GPU is done with it)
    void Consume(size_t size) noexcept;

    void Reset() noexcept;

    size_t GetCapacity() const noexcept { return m_capacity; }
    size_t GetUsed() const noexcept;
    size_t GetFree() const noexcept;
    size_t GetWriteOffset() const noexcept { return m_writeOffset; }

private:
    uint8_t* m_memory = nullptr;
    size_t m_capacity = 0;
    size_t m_writeOffset = 0;
    size_t m_readOffset = 0;
};

// --- Leak Detector (debug builds) ---

#ifdef _DEBUG

class LeakDetector {
public:
    static LeakDetector& Instance();

    void RecordAlloc(void* ptr, size_t size, const char* file, int line);
    void RecordFree(void* ptr);
    void ReportLeaks();

private:
    struct AllocRecord {
        size_t size;
        const char* file;
        int line;
    };

    std::mutex m_mutex;
    std::unordered_map<uintptr_t, AllocRecord> m_records;
};

#define NOVA_TRACK_ALLOC(ptr, size) ::Nova64::Video::LeakDetector::Instance().RecordAlloc(ptr, size, __FILE__, __LINE__)
#define NOVA_TRACK_FREE(ptr)        ::Nova64::Video::LeakDetector::Instance().RecordFree(ptr)

#else

#define NOVA_TRACK_ALLOC(ptr, size) ((void)0)
#define NOVA_TRACK_FREE(ptr)        ((void)0)

#endif

} // namespace Nova64::Video
