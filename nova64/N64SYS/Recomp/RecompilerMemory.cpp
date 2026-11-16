/**
 * @file RecompilerMemory.cpp
 * @brief Implementation of memory management for dynamically recompiled code
 * 
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 * 
 * This implementation manages executable memory for the recompiler with:
 * - Dynamic buffer growth for large code caches
 * - Executable memory protection (VirtualAlloc on Windows)
 * - Alignment and page boundary handling
 * - Memory statistics and profiling
 * - Safe bounds checking and error handling
 */

#include "RecompilerMemory.hpp"
#include "Recompiler.hpp"
#include "../SysGlobals.hpp"
#include "../../common/MemoryManagment.hpp"
#include "../../common/log.hpp"
#include "../../common/platform.hpp"
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <pthread.h>
#include <TargetConditionals.h>
#endif

namespace Nova64::Recompiler {

// =============================================================================
// ExecutableMemoryDeleter Implementation
// =============================================================================

void ExecutableMemoryDeleter::operator()(uint8_t* ptr) const {
    if (!ptr) {
        return;
    }

#ifdef _WIN32
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    if (size > 0) {
        munmap(ptr, size);
    }
#endif

    Log::debug("RecompilerMemory: Released executable memory");
}

// =============================================================================
// Construction & Destruction
// =============================================================================

CRecompMemory::CRecompMemory(uint32_t initialSize) :
    m_Buffer(nullptr), m_WritePosition(nullptr), m_CurrentSize(0), m_OldWritePosition(nullptr) {
    // Use default size if none specified
    if (initialSize == 0) {
        initialSize = DEFAULT_INITIAL_SIZE;
    }

    // Clamp to maximum allowed size
    if (initialSize > MAX_BUFFER_SIZE) {
        Log::warning("RecompilerMemory: Requested size exceeds maximum, clamping to 60MB");
        initialSize = MAX_BUFFER_SIZE;
    }

// Align to page boundaries for better performance and memory protection
#ifdef _WIN32
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    uint32_t pageSize = sysInfo.dwPageSize;
#else
    uint32_t pageSize = static_cast<uint32_t>(sysconf(_SC_PAGESIZE));
#endif

    // Round up to next page boundary
    initialSize = ((initialSize + pageSize - 1) / pageSize) * pageSize;

    Log::infof("RecompilerMemory: Initializing with %u bytes (%u KB)", initialSize, initialSize / 1024);

    // Allocate initial buffer
    m_Buffer = AllocateBuffer(initialSize);
    if (!m_Buffer) {
        Log::error("RecompilerMemory: Failed to allocate initial buffer");
        throw std::bad_alloc();
    }

    m_CurrentSize = initialSize;
    m_WritePosition = m_Buffer.get();
    m_OldWritePosition = m_WritePosition;

    // Initialize statistics
    m_Stats.allocatedBytes = initialSize;
    m_Stats.usedBytes = 0;
    m_Stats.peakUsedBytes = 0;
    m_Stats.growthCount = 0;

    Log::debug("RecompilerMemory: Initialization complete");
}

CRecompMemory::~CRecompMemory() {
    if (m_Buffer) {
        Log::debugf("RecompilerMemory: Destroying - Used %u / %u bytes (%.1f%%), grew %u times", m_Stats.peakUsedBytes,
                    m_Stats.allocatedBytes,
                    (m_Stats.allocatedBytes > 0) ? (100.0f * m_Stats.peakUsedBytes / m_Stats.allocatedBytes) : 0.0f,
                    m_Stats.growthCount);

#ifdef _WIN32
// Free with VirtualFree if we used VirtualAlloc
// Note: unique_ptr will handle this via custom deleter if set
#endif
    }
}

CRecompMemory::CRecompMemory(CRecompMemory&& other) noexcept :
    m_Buffer(std::move(other.m_Buffer)), m_WritePosition(other.m_WritePosition), m_CurrentSize(other.m_CurrentSize),
    m_Stats(other.m_Stats), m_OldWritePosition(other.m_OldWritePosition) {
    // Clear the moved-from object
    other.m_WritePosition = nullptr;
    other.m_CurrentSize = 0;
    other.m_OldWritePosition = nullptr;
    other.m_Stats.Reset();
}

CRecompMemory& CRecompMemory::operator=(CRecompMemory&& other) noexcept {
    if (this != &other) {
        m_Buffer = std::move(other.m_Buffer);
        m_WritePosition = other.m_WritePosition;
        m_CurrentSize = other.m_CurrentSize;
        m_Stats = other.m_Stats;
        m_OldWritePosition = other.m_OldWritePosition;

        // Clear the moved-from object
        other.m_WritePosition = nullptr;
        other.m_CurrentSize = 0;
        other.m_OldWritePosition = nullptr;
        other.m_Stats.Reset();
    }
    return *this;
}

// =============================================================================
// Buffer Allocation
// =============================================================================

std::unique_ptr<uint8_t[], ExecutableMemoryDeleter> CRecompMemory::AllocateBuffer(uint32_t size) {
    if (size == 0) {
        Log::error("RecompilerMemory: Cannot allocate zero-size buffer");
        return nullptr;
    }

#ifdef _WIN32
    // Use VirtualAlloc for executable memory
    // This gives us page-aligned memory with execute permissions
    void* memory = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

    if (!memory) {
        DWORD error = GetLastError();
        Log::errorf("RecompilerMemory: VirtualAlloc failed with error 0x%X", error);
        return nullptr;
    }

    Log::debugf("RecompilerMemory: Allocated executable memory at %p (%u bytes)", memory, size);

    // Return with custom deleter
    ExecutableMemoryDeleter deleter;
    deleter.size = size;
    return std::unique_ptr<uint8_t[], ExecutableMemoryDeleter>(static_cast<uint8_t*>(memory), deleter);

#else
// Use mmap for executable memory on Unix-like systems.
//
// Apple Silicon (and Intel Macs, more loosely) enforce hardware W^X:
// requesting PROT_EXEC|PROT_WRITE together via a plain anonymous mmap is
// rejected outright with EPERM ("Permission denied") unless the mapping
// is created with MAP_JIT. Even with MAP_JIT, the page is only writable
// OR executable at any instant per-thread — pthread_jit_write_protect_np()
// toggles which side is active, so callers must disable write-protection
// before emitting code into this buffer and re-enable it before jumping
// into the generated code (done at the write/execute call sites, not here).
#if defined(__APPLE__)
    int mmapFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT;
#else
    int mmapFlags = MAP_PRIVATE | MAP_ANONYMOUS;
#endif
    void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, mmapFlags, -1, 0);

    if (memory == MAP_FAILED) {
        Log::errorf("RecompilerMemory: mmap failed: %s", strerror(errno));
        return nullptr;
    }

#if defined(__APPLE__)
    // Start in writable mode (execute-protected) so the recompiler can emit
    // code immediately after allocation without an extra call at every
    // call site that happens to write into a freshly grown buffer.
    pthread_jit_write_protect_np(0);
#endif

    Log::debugf("RecompilerMemory: Allocated executable memory at %p (%u bytes)", memory, size);

    // Return with custom deleter
    ExecutableMemoryDeleter deleter;
    deleter.size = size;
    return std::unique_ptr<uint8_t[], ExecutableMemoryDeleter>(static_cast<uint8_t*>(memory), deleter);
#endif
}

// =============================================================================
// Buffer Growth
// =============================================================================

bool CRecompMemory::GrowBuffer(uint32_t minRequiredSize) {
    // Calculate new size - either double current size or add minimum required
    uint32_t newSize = m_CurrentSize * GROWTH_FACTOR;
    uint32_t requiredTotal = GetUsedMemory() + minRequiredSize;

    if (newSize < requiredTotal) {
        // Need more than double - add in increments
        newSize = requiredTotal + GROWTH_INCREMENT;
    }

    // Clamp to maximum allowed
    if (newSize > MAX_BUFFER_SIZE) {
        if (requiredTotal > MAX_BUFFER_SIZE) {
            Log::errorf("RecompilerMemory: Cannot grow beyond %u MB - requested %u bytes",
                        MAX_BUFFER_SIZE / (1024 * 1024), requiredTotal);
            return false;
        }
        newSize = MAX_BUFFER_SIZE;
    }

// Align to page boundaries
#ifdef _WIN32
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    uint32_t pageSize = sysInfo.dwPageSize;
#else
    uint32_t pageSize = static_cast<uint32_t>(sysconf(_SC_PAGESIZE));
#endif

    newSize = ((newSize + pageSize - 1) / pageSize) * pageSize;

    Log::infof("RecompilerMemory: Growing buffer from %u KB to %u KB", m_CurrentSize / 1024, newSize / 1024);

    // Allocate new buffer
    auto newBuffer = AllocateBuffer(newSize);
    if (!newBuffer) {
        Log::error("RecompilerMemory: Failed to allocate larger buffer");
        return false;
    }

    // Calculate current usage
    uint32_t usedBytes = GetUsedMemory();

    // Copy existing code to new buffer
    if (usedBytes > 0) {
        std::memcpy(newBuffer.get(), m_Buffer.get(), usedBytes);
        Log::debugf("RecompilerMemory: Copied %u bytes to new buffer", usedBytes);
    }

    // Calculate new write position offset
    ptrdiff_t writeOffset = m_WritePosition - m_Buffer.get();

    // Replace old buffer
    m_Buffer = std::move(newBuffer);
    m_WritePosition = m_Buffer.get() + writeOffset;
    m_OldWritePosition = m_WritePosition;
    m_CurrentSize = newSize;

    // Update statistics
    m_Stats.allocatedBytes = newSize;
    m_Stats.growthCount++;
    m_Stats.usedBytes = usedBytes;

    Log::infof("RecompilerMemory: Growth complete - now %u KB total", newSize / 1024);

// Flush instruction cache after buffer growth
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), m_Buffer.get(), m_CurrentSize);
#elif defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache(reinterpret_cast<char*>(m_Buffer.get()),
                            reinterpret_cast<char*>(m_Buffer.get() + m_CurrentSize));
#endif

    return true;
}

// =============================================================================
// Memory Usage & Statistics
// =============================================================================

void CRecompMemory::ShowMemoryUsage() const {
    uint32_t used = GetUsedMemory();
    uint32_t available = GetRemainingSpace();
    float usagePercent = (m_CurrentSize > 0) ? (100.0f * used / m_CurrentSize) : 0.0f;

    Log::infof("=== Recompiler Memory Usage ===");
    Log::infof("  Total allocated: %u KB (%u MB)", m_CurrentSize / 1024, m_CurrentSize / (1024 * 1024));
    Log::infof("  Currently used:  %u KB (%u MB) [%.1f%%]", used / 1024, used / (1024 * 1024), usagePercent);
    Log::infof("  Peak usage:      %u KB (%u MB)", m_Stats.peakUsedBytes / 1024, m_Stats.peakUsedBytes / (1024 * 1024));
    Log::infof("  Available:       %u KB (%u MB)", available / 1024, available / (1024 * 1024));
    Log::infof("  Growth count:    %u times", m_Stats.growthCount);

    // Efficiency metrics
    if (m_Stats.peakUsedBytes > 0) {
        float efficiency =
            (m_Stats.allocatedBytes > 0) ? (100.0f * m_Stats.peakUsedBytes / m_Stats.allocatedBytes) : 0.0f;
        Log::infof("  Peak efficiency: %.1f%%", efficiency);

        if (efficiency < 50.0f && m_Stats.allocatedBytes > DEFAULT_INITIAL_SIZE) {
            Log::warning("RecompilerMemory: Low memory efficiency - consider reducing initial size");
        }
    }

    // Memory waste analysis
    uint32_t waste = m_Stats.allocatedBytes - m_Stats.peakUsedBytes;
    if (waste > 10 * 1024 * 1024) { // More than 10 MB wasted
        Log::warningf("RecompilerMemory: %u MB of allocated memory never used", waste / (1024 * 1024));
    }

    Log::infof("===============================");
}

} // namespace Nova64::Recompiler
