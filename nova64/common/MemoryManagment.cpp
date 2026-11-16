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

// --- Internal Helper Functions ---

namespace {

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
        // POSIX has no guard pages; PROT_NONE is the closest equivalent.
        osProtection = PROT_NONE;
        return true;
    default: return false;
    }
#endif
}

#if PLATFORM_WINDOWS
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

// --- Basic Memory Management Functions ---

void* AllocateAddressSpace(size_t size, void* base_address) {
#if PLATFORM_WINDOWS
    return VirtualAlloc(base_address, size, MEM_RESERVE | MEM_TOP_DOWN, PAGE_NOACCESS);
#else
    void* ptr =
        mmap(base_address, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | (base_address ? MAP_FIXED : 0), -1, 0);
    if (ptr == MAP_FAILED) {
        return nullptr;
    }
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
    // POSIX has no decommit: remapping the range as PROT_NONE anonymous memory
    // lets the OS reclaim its pages.
    void* result = mmap(addr, size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result == MAP_FAILED) {
        return false;
    }
    msync(addr, size, MS_SYNC | MS_INVALIDATE);
    return true;
#endif
}

// --- Extended Memory Management API ---

namespace MemoryManagement {

bool FlushInstructionCache(void* address, size_t size) {
    if (address == nullptr || size == 0) {
        return false;
    }

#if PLATFORM_WINDOWS
    return ::FlushInstructionCache(GetCurrentProcess(), address, size) != 0;
#elif defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache(static_cast<char*>(address), static_cast<char*>(address) + size);
    return true;
#else
    // No known way to flush the instruction cache with this compiler; does nothing.
    (void)address;
    (void)size;
    return true;
#endif
}

} // namespace MemoryManagement
