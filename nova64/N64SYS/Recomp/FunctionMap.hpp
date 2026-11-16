/**
 * @file FunctionMap.hpp
 * @brief Function lookup table management for the recompiler
 * 
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 * 
 * This class provides fast lookup of compiled functions by virtual address
 * using a two-level page table structure optimized for sparse 64-bit address spaces.
 * 
 * Structure:
 *   - JumpTable: Direct lookup table (256K entries) for fast path
 *   - FunctionTable: Two-level table for full 32-bit address space
 */

#pragma once

#include "CompiledFunc.hpp"
#include "FunctionInfo.hpp"
#include "../Types.hpp"
#include "../SysGlobals.hpp"
#include "../../common/MemoryManagment.hpp"
#include <cstdint>
#include <memory>
#include <array>
#include <optional>
#include <unordered_map>

namespace Nova64 {
namespace Recompiler {

/**
 * @class CFunctionMap
 * @brief Manages lookup tables for compiled functions with RAII memory management
 * 
 * The class provides O(1) lookup for compiled functions using a hybrid approach:
 * 1. Direct jump table for lower 18 bits of address (256K entries)
 * 2. Two-level page table for full 32-bit address space
 * 
 * Memory layout:
 *   - Page directory: 1024 entries (10 bits)
 *   - Page table: 1024 entries per page (10 bits each)
 *   Total: 1M entries possible, but allocated sparsely
 */
class CFunctionMap {
public:
    // Constants for table sizes
    static constexpr uint32_t JUMP_TABLE_SIZE = 0x40000; // 256K entries
    static constexpr uint32_t PAGE_DIR_SIZE = 1024;      // 10 bits
    static constexpr uint32_t PAGE_TABLE_SIZE = 1024;    // 10 bits
    static constexpr uint32_t PAGE_SHIFT = 10;
    static constexpr uint32_t PAGE_MASK = 0x3FF;

    using CompiledFuncPtr = CCompiledFunc*;
    using PageTable = std::unique_ptr<CompiledFuncPtr[]>;
    using PageDirectory = std::array<PageTable, PAGE_DIR_SIZE>;

    CFunctionMap();
    ~CFunctionMap() = default;

    // Non-copyable but movable
    CFunctionMap(const CFunctionMap&) = delete;
    CFunctionMap& operator=(const CFunctionMap&) = delete;
    CFunctionMap(CFunctionMap&&) noexcept = default;
    CFunctionMap& operator=(CFunctionMap&&) noexcept = default;

    /**
     * @brief Initialize the function map with pre-allocated tables
     * @return true if successful, false on allocation failure
     */
    bool Initialize();

    /**
     * @brief Reset all function entries to nullptr
     * @param preserveStructure If true, keeps allocated tables but clears entries
     *                          If false, deallocates all tables
     */
    void Reset(bool preserveStructure = true);

    /**
     * @brief Get a compiled function by virtual address
     * @param vaddr Virtual address (32-bit)
     * @return Pointer to compiled function or nullptr if not found
     */
    CompiledFuncPtr Lookup(uint32_t vaddr) const noexcept;

    /**
     * @brief Get or create an entry for a virtual address
     * @param vaddr Virtual address (32-bit)
     * @return Reference to the function pointer slot
     * @throws std::bad_alloc if memory allocation fails
     */
    CompiledFuncPtr& GetOrCreateEntry(uint32_t vaddr);

    /**
     * @brief Store a compiled function at the specified address
     * @param vaddr Virtual address (32-bit)
     * @param func Pointer to compiled function
     * @return Previous function pointer at this address (may be nullptr)
     */
    CompiledFuncPtr Store(uint32_t vaddr, CompiledFuncPtr func) noexcept;

    /**
     * @brief Remove a function entry
     * @param vaddr Virtual address (32-bit)
     * @return Removed function pointer or nullptr if not found
     */
    CompiledFuncPtr Remove(uint32_t vaddr) noexcept;

    /**
     * @brief Check if an entry exists at the given address
     * @param vaddr Virtual address (32-bit)
     * @return true if an entry exists (even if nullptr)
     */
    bool Contains(uint32_t vaddr) const noexcept;

    /**
     * @brief Get the total number of allocated page tables (for statistics)
     */
    size_t GetAllocatedPageTables() const noexcept;

    /**
     * @brief Get the direct jump table for fast-path access
     * @warning Direct access is unsafe, prefer using Lookup()/Store()
     */
    CompiledFuncPtr* GetJumpTable() noexcept { return m_JumpTable.get(); }
    const CompiledFuncPtr* GetJumpTable() const noexcept { return m_JumpTable.get(); }

    /**
     * @brief Get the page directory (for debugging/profiling)
     */
    const PageDirectory& GetPageDirectory() const noexcept { return m_PageDirectory; }

private:
    // Helper methods for address decomposition
    static constexpr uint32_t GetPageDirectoryIndex(uint32_t vaddr) noexcept {
        return (vaddr >> (PAGE_SHIFT * 2)) & PAGE_MASK;
    }

    static constexpr uint32_t GetPageTableIndex(uint32_t vaddr) noexcept { return (vaddr >> PAGE_SHIFT) & PAGE_MASK; }

    static constexpr uint32_t GetJumpTableIndex(uint32_t vaddr) noexcept {
        return vaddr & 0x3FFFF; // Lower 18 bits
    }

    // Ensure a page table exists at the specified directory index
    PageTable& EnsurePageTable(uint32_t dirIndex);

    // Direct jump table for fast-path lookups (lower 18 bits)
    std::unique_ptr<CompiledFuncPtr[]> m_JumpTable;

    // Two-level page table for full 32-bit address space
    PageDirectory m_PageDirectory;

    // Exact-match hash map: authoritative lookup by PC.
    // The jump table and page table are fast-path caches that can lose entries
    // due to collisions; this map guarantees O(1) exact lookup for all stored
    // functions, preventing runaway recompilation of already-compiled code.
    std::unordered_map<uint32_t, CompiledFuncPtr> m_ExactLookup;

    // Statistics
    size_t m_AllocatedPageTables = 0;
};

// Inline implementations for performance-critical methods
inline CFunctionMap::CompiledFuncPtr CFunctionMap::Lookup(uint32_t vaddr) const noexcept {
    // First try the fast jump table (indexed by lower 18 bits - instruction-level)
    if (auto func = m_JumpTable[GetJumpTableIndex(vaddr)]) {
        // Must verify address: multiple vaddrs can hash to the same 18-bit index
        if (func->EnterPC() == vaddr) {
            return func;
        }
    }

    // Exact-match hash map — authoritative, no collisions
    auto it = m_ExactLookup.find(vaddr);
    if (it != m_ExactLookup.end()) {
        return it->second;
    }

    return nullptr;
}

inline CFunctionMap::CompiledFuncPtr CFunctionMap::Store(uint32_t vaddr, CompiledFuncPtr func) noexcept {
    // Store in exact-match map (authoritative)
    CompiledFuncPtr oldExact = nullptr;
    if (func) {
        auto [it, inserted] = m_ExactLookup.emplace(vaddr, func);
        if (!inserted) {
            oldExact = it->second;
            it->second = func;
        }
    } else {
        auto it = m_ExactLookup.find(vaddr);
        if (it != m_ExactLookup.end()) {
            oldExact = it->second;
            m_ExactLookup.erase(it);
        }
    }

    // Store in jump table (fast-path cache, may collide)
    uint32_t jumpIdx = GetJumpTableIndex(vaddr);
    m_JumpTable[jumpIdx] = func;

    // Return the old function that was at this exact address
    return oldExact;
}

} // namespace Recompiler
} // namespace Nova64
