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

/**
 * @brief Change protection of a memory region
 * @param addr Address of the memory region
 * @param size Size of the memory region
 * @param memProtection New protection flags
 * @return True on success, false on failure
 */
bool ProtectMemory(void* addr, size_t size, MEM_PROTECTION memProtection);

/**
 * @brief Change protection of a memory region and retrieve old protection
 * @param addr Address of the memory region
 * @param size Size of the memory region
 * @param memProtection New protection flags
 * @param oldProtection Output: previous protection flags
 * @return True on success, false on failure
 */
bool ProtectMemory(void* addr, size_t size, MEM_PROTECTION memProtection, MEM_PROTECTION& oldProtection);

// =============================================================================
// Extended Memory Management API
// =============================================================================

namespace MemoryManagement {

/**
 * @brief Memory region information
 */
struct MemoryInfo {
    void* base_address;        ///< Base address of the region
    void* allocation_base;     ///< Base address of the allocation
    size_t region_size;        ///< Size of the region
    MEM_PROTECTION protection; ///< Current protection
    bool is_committed;         ///< Whether memory is committed
    bool is_reserved;          ///< Whether memory is reserved
    bool is_free;              ///< Whether memory is free
};

/**
 * @brief Get the system page size
 * @return Page size in bytes
 */
size_t GetPageSize();

/**
 * @brief Get the allocation granularity
 * @return Allocation granularity in bytes
 */
size_t GetAllocationGranularity();

/**
 * @brief Allocate aligned memory
 * @param size Size of the allocation
 * @param alignment Required alignment (must be power of 2)
 * @return Pointer to aligned memory, or nullptr on failure
 */
void* AllocateAligned(size_t size, size_t alignment);

/**
 * @brief Free aligned memory
 * @param ptr Pointer to aligned memory
 */
void FreeAligned(void* ptr);

/**
 * @brief Allocate executable memory
 * @param size Size of the allocation
 * @return Pointer to executable memory, or nullptr on failure
 */
void* AllocateExecutable(size_t size);

/**
 * @brief Free executable memory
 * @param address Pointer to executable memory
 * @param size Size of the allocation
 * @return True on success, false on failure
 */
bool FreeExecutable(void* address, size_t size);

/**
 * @brief Flush the instruction cache
 * @param address Address of the code region
 * @param size Size of the code region
 * @return True on success, false on failure
 */
bool FlushInstructionCache(void* address, size_t size);

/**
 * @brief Lock memory pages in physical RAM
 * @param address Address of the memory region
 * @param size Size of the memory region
 * @return True on success, false on failure
 */
bool LockMemory(void* address, size_t size);

/**
 * @brief Unlock memory pages
 * @param address Address of the memory region
 * @param size Size of the memory region
 * @return True on success, false on failure
 */
bool UnlockMemory(void* address, size_t size);

/**
 * @brief Zero a memory range
 * @param address Address of the memory region
 * @param size Size of the memory region
 * @return True on success, false on failure
 */
bool ZeroMemoryRange(void* address, size_t size);

/**
 * @brief Query information about a memory region
 * @param address Address to query
 * @return Memory information structure
 */
MemoryInfo QueryMemory(void* address);

} // namespace MemoryManagement
