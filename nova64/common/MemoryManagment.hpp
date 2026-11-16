/**
 * @file MemoryManagment.hpp
 * @brief Cross-platform memory management interface
 *
 * Provides virtual memory allocation, protection, and management functions
 * using platform-specific APIs (VirtualAlloc on Windows, mmap on POSIX).
 */

#pragma once
#include <cstddef>
#include <cstdint>

/**
 * @brief Memory protection flags
 */
enum MEM_PROTECTION {
    MEM_NOACCESS,          ///< No access allowed
    MEM_READONLY,          ///< Read-only access
    MEM_READWRITE,         ///< Read and write access
    MEM_EXECUTE,           ///< Execute only
    MEM_EXECUTE_READ,      ///< Execute and read access
    MEM_EXECUTE_READWRITE, ///< Execute, read, and write access
    MEM_GUARD              ///< Guard page (Windows-specific, triggers exception on access)
};

/**
 * @brief Reserve a range of virtual address space
 * @param size Size of the address space to reserve
 * @param base_address Optional hint for the base address
 * @return Pointer to the reserved address space, or nullptr on failure
 */
void* AllocateAddressSpace(size_t size, void* base_address = nullptr);

/**
 * @brief Release reserved address space
 * @param addr Address of the reserved space
 * @param size Size of the reserved space
 * @return True on success, false on failure
 */
bool FreeAddressSpace(void* addr, size_t size);

/**
 * @brief Commit physical memory to a reserved address range
 * @param addr Address within reserved space
 * @param size Size of the region to commit
 * @param memProtection Protection flags for the committed memory
 * @return Pointer to the committed memory, or nullptr on failure
 */
void* CommitMemory(void* addr, size_t size, MEM_PROTECTION memProtection);

/**
 * @brief Decommit physical memory from an address range
 * @param addr Address of the committed memory
 * @param size Size of the region to decommit
 * @return True on success, false on failure
 */
bool DecommitMemory(void* addr, size_t size);

// --- Extended Memory Management API ---

namespace MemoryManagement {

/**
 * @brief Flush the instruction cache
 * @param address Address of the code region
 * @param size Size of the code region
 * @return True on success, false on failure
 */
bool FlushInstructionCache(void* address, size_t size);

} // namespace MemoryManagement
