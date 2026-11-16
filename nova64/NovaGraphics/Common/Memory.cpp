/**
 * @file Memory.cpp
 * @brief Custom allocator implementations
 */

#include "Memory.hpp"

#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <new>

#ifdef _WIN32
#include <malloc.h> // _aligned_malloc / _aligned_free
#endif

namespace Nova64::Video {

AllocStats g_allocStats;

// --- Aligned Allocation ---

void* AlignedAlloc(size_t size, size_t alignment) {
    if (size == 0) return nullptr;
#ifdef _WIN32
    void* ptr = _aligned_malloc(size, alignment);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) ptr = nullptr;
#endif
    if (!ptr) throw std::bad_alloc();
    return ptr;
}

void AlignedFree(void* ptr) {
    if (!ptr) return;
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

void* TrackedAlloc(size_t size, size_t alignment) {
    void* ptr = AlignedAlloc(size, alignment);
    g_allocStats.totalAllocations.fetch_add(1, std::memory_order_relaxed);
    g_allocStats.totalBytesAllocated.fetch_add(size, std::memory_order_relaxed);
    auto current = g_allocStats.currentBytesInUse.fetch_add(size, std::memory_order_relaxed) + size;
    auto peak = g_allocStats.peakBytesInUse.load(std::memory_order_relaxed);
    while (current > peak) {
        if (g_allocStats.peakBytesInUse.compare_exchange_weak(peak, current, std::memory_order_relaxed)) break;
    }
    NOVA_TRACK_ALLOC(ptr, size);
    return ptr;
}

void TrackedFree(void* ptr, size_t size) {
    if (!ptr) return;
    NOVA_TRACK_FREE(ptr);
    g_allocStats.totalDeallocations.fetch_add(1, std::memory_order_relaxed);
    g_allocStats.currentBytesInUse.fetch_sub(size, std::memory_order_relaxed);
    AlignedFree(ptr);
}

// --- Pool Allocator ---

PoolAllocator::~PoolAllocator() { Destroy(); }

bool PoolAllocator::Init(size_t blockSize, size_t blockCount, size_t alignment) {
    if (m_memory) Destroy();

    // Ensure block size can hold a FreeNode pointer and is aligned
    m_blockSize = std::max(blockSize, sizeof(FreeNode));
    m_blockSize = (m_blockSize + alignment - 1) & ~(alignment - 1);
    m_blockCount = blockCount;
    m_alignment = alignment;
    m_totalBytes = m_blockSize * m_blockCount;

    m_memory = static_cast<uint8_t*>(AlignedAlloc(m_totalBytes, m_alignment));
    if (!m_memory) return false;

    std::memset(m_memory, 0, m_totalBytes);
    Reset();

    g_allocStats.totalBytesAllocated.fetch_add(m_totalBytes, std::memory_order_relaxed);
    return true;
}

void PoolAllocator::Destroy() {
    if (m_memory) {
        g_allocStats.currentBytesInUse.fetch_sub((m_blockCount - m_freeCount) * m_blockSize, std::memory_order_relaxed);
        AlignedFree(m_memory);
        m_memory = nullptr;
    }
    m_freeList = nullptr;
    m_freeCount = 0;
}

void* PoolAllocator::Alloc() noexcept {
    if (!m_freeList) return nullptr;

    FreeNode* node = m_freeList;
    m_freeList = node->next;
    m_freeCount--;

    g_allocStats.poolAllocations.fetch_add(1, std::memory_order_relaxed);
    g_allocStats.currentBytesInUse.fetch_add(m_blockSize, std::memory_order_relaxed);

    return static_cast<void*>(node);
}

void PoolAllocator::Free(void* ptr) noexcept {
    if (!ptr || !m_memory) return;

    // Validate pointer is within our range
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    auto base = reinterpret_cast<uintptr_t>(m_memory);
    if (addr < base || addr >= base + m_totalBytes) return;

    auto* node = static_cast<FreeNode*>(ptr);
    node->next = m_freeList;
    m_freeList = node;
    m_freeCount++;

    g_allocStats.poolDeallocations.fetch_add(1, std::memory_order_relaxed);
    g_allocStats.currentBytesInUse.fetch_sub(m_blockSize, std::memory_order_relaxed);
}

void PoolAllocator::Reset() noexcept {
    m_freeList = nullptr;
    m_freeCount = m_blockCount;

    // Push every block onto the head of the list, so the last block is handed out first.
    for (size_t i = 0; i < m_blockCount; i++) {
        auto* node = reinterpret_cast<FreeNode*>(m_memory + i * m_blockSize);
        node->next = m_freeList;
        m_freeList = node;
    }
}

// --- Linear Allocator ---

LinearAllocator::~LinearAllocator() { Destroy(); }

bool LinearAllocator::Init(size_t capacity, size_t alignment) {
    if (m_memory) Destroy();

    m_capacity = capacity;
    m_alignment = alignment;
    m_offset = 0;
    m_memory = static_cast<uint8_t*>(AlignedAlloc(capacity, alignment));
    if (!m_memory) return false;

    g_allocStats.totalBytesAllocated.fetch_add(capacity, std::memory_order_relaxed);
    return true;
}

void LinearAllocator::Destroy() {
    if (m_memory) {
        AlignedFree(m_memory);
        m_memory = nullptr;
    }
    m_offset = 0;
    m_capacity = 0;
}

void* LinearAllocator::Alloc(size_t size, size_t alignment) noexcept {
    size_t aligned = (m_offset + alignment - 1) & ~(alignment - 1);
    if (aligned + size > m_capacity) return nullptr;

    void* ptr = m_memory + aligned;
    m_offset = aligned + size;

    g_allocStats.linearAllocations.fetch_add(1, std::memory_order_relaxed);
    return ptr;
}

void LinearAllocator::Reset() noexcept {
    m_offset = 0;
    g_allocStats.linearResets.fetch_add(1, std::memory_order_relaxed);
}

// --- Ring Buffer ---

CPURingBuffer::~CPURingBuffer() { Destroy(); }

bool CPURingBuffer::Init(size_t capacity) {
    if (m_memory) Destroy();
    m_capacity = capacity;
    m_memory = static_cast<uint8_t*>(AlignedAlloc(capacity, 64));
    if (!m_memory) {
        m_capacity = 0;
        return false;
    }
    m_writeOffset = 0;
    m_readOffset = 0;
    return true;
}

void CPURingBuffer::Destroy() {
    if (m_memory) {
        AlignedFree(m_memory);
        m_memory = nullptr;
    }
    m_capacity = 0;
    m_writeOffset = 0;
    m_readOffset = 0;
}

void* CPURingBuffer::Reserve(size_t size) noexcept {
    if (!m_memory || size > m_capacity) return nullptr;

    // Fits before the end of the buffer.
    if (m_writeOffset + size <= m_capacity) {
        void* ptr = m_memory + m_writeOffset;
        m_writeOffset += size;
        if (m_writeOffset == m_capacity) m_writeOffset = 0;
        return ptr;
    }

    // Wrap to the start; only possible if the consumed region there is large enough.
    if (size <= m_readOffset) {
        void* ptr = m_memory;
        m_writeOffset = size;
        return ptr;
    }

    return nullptr;
}

void CPURingBuffer::Consume(size_t size) noexcept {
    m_readOffset += size;
    if (m_readOffset >= m_capacity) m_readOffset -= m_capacity;
}

void CPURingBuffer::Reset() noexcept {
    m_writeOffset = 0;
    m_readOffset = 0;
}

size_t CPURingBuffer::GetUsed() const noexcept {
    if (m_writeOffset >= m_readOffset) return m_writeOffset - m_readOffset;
    return m_capacity - m_readOffset + m_writeOffset;
}

size_t CPURingBuffer::GetFree() const noexcept { return m_capacity - GetUsed(); }

// --- Leak Detector ---

#ifdef _DEBUG

LeakDetector& LeakDetector::Instance() {
    static LeakDetector s_instance;
    return s_instance;
}

void LeakDetector::RecordAlloc(void* ptr, size_t size, const char* file, int line) {
    if (!ptr) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    m_records[reinterpret_cast<uintptr_t>(ptr)] = {size, file, line};
}

void LeakDetector::RecordFree(void* ptr) {
    if (!ptr) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    m_records.erase(reinterpret_cast<uintptr_t>(ptr));
}

void LeakDetector::ReportLeaks() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_records.empty()) {
        std::fprintf(stderr, "[NovaGraphics] Memory leak check: CLEAN (no leaks)\n");
        return;
    }
    std::fprintf(stderr, "[NovaGraphics] *** MEMORY LEAKS DETECTED: %zu allocations ***\n", m_records.size());
    size_t totalLeak = 0;
    for (const auto& [addr, rec] : m_records) {
        std::fprintf(stderr, "  Leak: %zu bytes at 0x%016llX (%s:%d)\n", rec.size,
                     static_cast<unsigned long long>(addr), rec.file, rec.line);
        totalLeak += rec.size;
    }
    std::fprintf(stderr, "[NovaGraphics] Total leaked: %zu bytes\n", totalLeak);
}

#endif

} // namespace Nova64::Video
