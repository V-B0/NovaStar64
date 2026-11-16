/**
 * @file MemoryManagment.cpp
 * @brief Cross-platform memory management implementation
 *
 * Provides virtual memory allocation, protection, and management functions
 * using platform-specific APIs (VirtualAlloc on Windows, mmap on POSIX).
 * Based on Project64's memory management with modern C++ improvements.
 */

#include "MemoryManagment.hpp"
#include "platform.hpp"
#include <cstring>
#include <algorithm>

#if PLATFORM_WINDOWS
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

// =============================================================================
// Internal Helper Functions
// =============================================================================

namespace {

/**
 * @brief Translate MEM_PROTECTION to OS-specific protection flags
 */
bool TranslateToOsProtection(MEM_PROTECTION memProtection, int& osProtection) {
#if PLATFORM_WINDOWS
    switch (memProtection) {
    case MEM_NOACCESS:          osProtection = PAGE_NOACCESS; return true;
    case MEM_READONLY:          osProtection = PAGE_READONLY; return true;
    case MEM_READWRITE:         osProtection = PAGE_READWRITE; return true;
    case MEM_EXECUTE:           osProtection = PAGE_EXECUTE; return true;
    case MEM_EXECUTE_READ:      osProtection = PAGE_EXECUTE_READ; return true;
    case MEM_EXECUTE_READWRITE: osProtection = PAGE_EXECUTE_READWRITE; return true;
    case MEM_GUARD:             osProtection = PAGE_GUARD | PAGE_READWRITE; return true;
    default:                    return false;
    }
#else
    switch (memProtection) {
    case MEM_NOACCESS:          osProtection = PROT_NONE; return true;
    case MEM_READONLY:          osProtection = PROT_READ; return true;
    case MEM_READWRITE:         osProtection = PROT_READ | PROT_WRITE; return true;
    case MEM_EXECUTE:           osProtection = PROT_EXEC; return true;
    case MEM_EXECUTE_READ:      osProtection = PROT_READ | PROT_EXEC; return true;
    case MEM_EXECUTE_READWRITE: osProtection = PROT_READ | PROT_WRITE | PROT_EXEC; return true;
    case MEM_GUARD:
        // POSIX doesn't have guard pages directly, use no access
        osProtection = PROT_NONE;
        return true;
    default: return false;
    }
#endif
}

#if PLATFORM_WINDOWS
/**
 * @brief Translate OS protection flags back to MEM_PROTECTION
 */
bool TranslateFromOsProtection(DWORD osProtection, MEM_PROTECTION& memProtection) {
    // Strip off guard and nocache flags for comparison
    const DWORD baseProtection = osProtection & ~(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);

    switch (baseProtection) {
    case PAGE_NOACCESS:          memProtection = (osProtection & PAGE_GUARD) ? MEM_GUARD : MEM_NOACCESS; return true;
    case PAGE_READONLY:          memProtection = MEM_READONLY; return true;
    case PAGE_READWRITE:         memProtection = MEM_READWRITE; return true;
    case PAGE_EXECUTE:           memProtection = MEM_EXECUTE; return true;
    case PAGE_EXECUTE_READ:      memProtection = MEM_EXECUTE_READ; return true;
    case PAGE_EXECUTE_READWRITE: memProtection = MEM_EXECUTE_READWRITE; return true;
    default:                     return false;
    }
}
#endif

} // anonymous namespace

// =============================================================================
// Basic Memory Management Functions
// =============================================================================

void* AllocateAddressSpace(size_t size, void* base_address) {
#if PLATFORM_WINDOWS
    return VirtualAlloc(base_address, size, MEM_RESERVE | MEM_TOP_DOWN, PAGE_NOACCESS);
#else
    void* ptr =
        mmap(base_address, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | (base_address ? MAP_FIXED : 0), -1, 0);
    if (ptr == MAP_FAILED) {
        return nullptr;
    }
    // Ensure mapping is synchronized
    msync(ptr, size, MS_SYNC | MS_INVALIDATE);
    return ptr;
#endif
}

bool FreeAddressSpace(void* addr, size_t size) {
    if (addr == nullptr) {
        return false;
    }

#if PLATFORM_WINDOWS
    (void)size; // Unused on Windows
    return VirtualFree(addr, 0, MEM_RELEASE) != 0;
#else
    msync(addr, size, MS_SYNC);
    return munmap(addr, size) == 0;
#endif
}

void* CommitMemory(void* addr, size_t size, MEM_PROTECTION memProtection) {
    if (addr == nullptr || size == 0) {
        return nullptr;
    }

    int osProtection;
    if (!TranslateToOsProtection(memProtection, osProtection)) {
        return nullptr;
    }

#if PLATFORM_WINDOWS
    void* result = VirtualAlloc(addr, size, MEM_COMMIT, osProtection);
    return result;
#else
    void* ptr = mmap(addr, size, osProtection, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        return nullptr;
    }
    msync(addr, size, MS_SYNC | MS_INVALIDATE);
    return ptr;
#endif
}

bool DecommitMemory(void* addr, size_t size) {
    if (addr == nullptr || size == 0) {
        return false;
    }

#if PLATFORM_WINDOWS
    return VirtualFree(addr, size, MEM_DECOMMIT) != 0;
#else
    // On POSIX, we use mmap with PROT_NONE to effectively decommit
    // This allows the OS to reclaim physical pages via demand paging
    void* result = mmap(addr, size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result == MAP_FAILED) {
        return false;
    }
    msync(addr, size, MS_SYNC | MS_INVALIDATE);
    return true;
#endif
}

bool ProtectMemory(void* addr, size_t size, MEM_PROTECTION memProtection) {
    if (addr == nullptr || size == 0) {
        return false;
    }

    int osProtection;
    if (!TranslateToOsProtection(memProtection, osProtection)) {
        return false;
    }

#if PLATFORM_WINDOWS
    DWORD oldProtect;
    return VirtualProtect(addr, size, osProtection, &oldProtect) != 0;
#else
    return mprotect(addr, size, osProtection) == 0;
#endif
}

bool ProtectMemory(void* addr, size_t size, MEM_PROTECTION memProtection, MEM_PROTECTION& oldProtection) {
    if (addr == nullptr || size == 0) {
        return false;
    }

    int osProtection;
    if (!TranslateToOsProtection(memProtection, osProtection)) {
        return false;
    }

#if PLATFORM_WINDOWS
    DWORD oldOsProtect;
    if (VirtualProtect(addr, size, osProtection, &oldOsProtect) == 0) {
        return false;
    }
    return TranslateFromOsProtection(oldOsProtect, oldProtection);
#else
    // POSIX doesn't provide a way to query old protection, so we just set it
    // This is a limitation of the POSIX API
    oldProtection = MEM_NOACCESS; // Can't determine on POSIX
    return mprotect(addr, size, osProtection) == 0;
#endif
}

// =============================================================================
// Extended Memory Management API
// =============================================================================

namespace MemoryManagement {

size_t GetPageSize() {
    static size_t page_size = 0;

    if (page_size == 0) {
#if PLATFORM_WINDOWS
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        page_size = sysInfo.dwPageSize;
#else
        page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
    }

    return page_size;
}

size_t GetAllocationGranularity() {
    static size_t granularity = 0;

    if (granularity == 0) {
#if PLATFORM_WINDOWS
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        granularity = sysInfo.dwAllocationGranularity;
#else
        // On POSIX, allocation granularity is typically the same as page size
        granularity = GetPageSize();
#endif
    }

    return granularity;
}

void* AllocateAligned(size_t size, size_t alignment) {
    if (size == 0 || alignment == 0) {
        return nullptr;
    }

    // Check if alignment is power of 2
    if ((alignment & (alignment - 1)) != 0) {
        return nullptr;
    }

#if PLATFORM_WINDOWS
    return _aligned_malloc(size, alignment);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return ptr;
#endif
}

void FreeAligned(void* ptr) {
    if (ptr == nullptr) {
        return;
    }

#if PLATFORM_WINDOWS
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

void* AllocateExecutable(size_t size) {
    if (size == 0) {
        return nullptr;
    }

    // Align size to page boundary
    const size_t page_size = GetPageSize();
    const size_t aligned_size = (size + page_size - 1) & ~(page_size - 1);

    // Allocate and commit with execute permissions
    void* addr = AllocateAddressSpace(aligned_size);
    if (addr == nullptr) {
        return nullptr;
    }

    void* committed = CommitMemory(addr, aligned_size, MEM_EXECUTE_READWRITE);
    if (committed == nullptr) {
        FreeAddressSpace(addr, aligned_size);
        return nullptr;
    }

    return committed;
}

bool FreeExecutable(void* address, size_t size) {
    if (address == nullptr) {
        return false;
    }

    // Align size to page boundary
    const size_t page_size = GetPageSize();
    const size_t aligned_size = (size + page_size - 1) & ~(page_size - 1);

    return FreeAddressSpace(address, aligned_size);
}

bool FlushInstructionCache(void* address, size_t size) {
    if (address == nullptr || size == 0) {
        return false;
    }

#if PLATFORM_WINDOWS
    return ::FlushInstructionCache(GetCurrentProcess(), address, size) != 0;
#elif defined(__GNUC__) || defined(__clang__)
    // GCC/Clang builtin for cache flush
    __builtin___clear_cache(static_cast<char*>(address), static_cast<char*>(address) + size);
    return true;
#else
    // Generic fallback - sync instruction and data caches
    // This may not work on all architectures
    (void)address;
    (void)size;
    return true;
#endif
}

bool LockMemory(void* address, size_t size) {
    if (address == nullptr || size == 0) {
        return false;
    }

#if PLATFORM_WINDOWS
    return VirtualLock(address, size) != 0;
#else
    return mlock(address, size) == 0;
#endif
}

bool UnlockMemory(void* address, size_t size) {
    if (address == nullptr || size == 0) {
        return false;
    }

#if PLATFORM_WINDOWS
    return VirtualUnlock(address, size) != 0;
#else
    return munlock(address, size) == 0;
#endif
}

bool ZeroMemoryRange(void* address, size_t size) {
    if (address == nullptr || size == 0) {
        return false;
    }

    // Use secure zero if available to prevent optimization
#if PLATFORM_WINDOWS
    SecureZeroMemory(address, size);
    return true;
#elif defined(__STDC_LIB_EXT1__)
    return memset_s(address, size, 0, size) == 0;
#else
    // Volatile pointer to prevent optimization
    volatile unsigned char* ptr = static_cast<volatile unsigned char*>(address);
    for (size_t i = 0; i < size; ++i) {
        ptr[i] = 0;
    }
    return true;
#endif
}

MemoryInfo QueryMemory(void* address) {
    MemoryInfo info = {};
    info.base_address = nullptr;
    info.allocation_base = nullptr;
    info.region_size = 0;
    info.protection = MEM_NOACCESS;
    info.is_committed = false;
    info.is_reserved = false;
    info.is_free = true;

    if (address == nullptr) {
        return info;
    }

#if PLATFORM_WINDOWS
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0) {
        return info;
    }

    info.base_address = mbi.BaseAddress;
    info.allocation_base = mbi.AllocationBase;
    info.region_size = mbi.RegionSize;

    // Translate protection
    if (mbi.State == MEM_COMMIT) {
        info.is_committed = true;
        info.is_free = false;
        TranslateFromOsProtection(mbi.Protect, info.protection);
    } else if (mbi.State == MEM_RESERVE) {
        info.is_reserved = true;
        info.is_free = false;
        info.protection = MEM_NOACCESS;
    } else {
        info.is_free = true;
    }
#else
    // POSIX doesn't have a direct equivalent to VirtualQuery
    // We can only provide limited information
    info.base_address = address;
    info.allocation_base = address;
    info.region_size = GetPageSize(); // Minimum granularity
    info.protection = MEM_NOACCESS;   // Can't determine on POSIX
    info.is_committed = true;         // Assume committed if accessible
    info.is_reserved = false;
    info.is_free = false;
#endif

    return info;
}

} // namespace MemoryManagement
