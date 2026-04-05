/**
 * @file FunctionMap.cpp
 * @brief Implementation of function lookup table management
 * 
 * Nova64 N64 Emulator - Based on Project64 architecture
 * Original Project64 code licensed under GPL-2.0
 * 
 * This implementation provides efficient O(1) lookup for compiled functions
 * using a hybrid two-level page table structure optimized for sparse address spaces.
 */

#include "FunctionMap.hpp"
#include "FunctionInfo.hpp"
#include "../Types.hpp"
#include "../SysGlobals.hpp"
#include "../../common/log.hpp"
#include "../../common/trace.hpp"
#include "../../common/MemoryManagment.hpp"
#include <cstring>
#include <stdexcept>

namespace Nova64::Recompiler {

CFunctionMap::CFunctionMap()
    : m_JumpTable(nullptr)
    , m_AllocatedPageTables(0)
{
    // Don't allocate in constructor - wait for Initialize()
}

bool CFunctionMap::Initialize() {
    try {
        // Allocate the direct jump table (256K entries)
        m_JumpTable = std::make_unique<CompiledFuncPtr[]>(JUMP_TABLE_SIZE);
        
        // Zero-initialize the jump table
        std::memset(m_JumpTable.get(), 0, JUMP_TABLE_SIZE * sizeof(CompiledFuncPtr));
        
        // Page tables are allocated on demand, so we don't need to allocate them here
        // The page directory array is already initialized by default construction
        
        Log::debugf("CFunctionMap initialized with %zu jump table entries", 
                    JUMP_TABLE_SIZE);
        
        return true;
    } catch (const std::bad_alloc& e) {
        Log::errorf("CFunctionMap: Failed to allocate jump table: %s", e.what());
        m_JumpTable.reset();
        return false;
    } catch (const std::exception& e) {
        Log::errorf("CFunctionMap: Initialization failed: %s", e.what());
        m_JumpTable.reset();
        return false;
    }
}

void CFunctionMap::Reset(bool preserveStructure) {
    if (!m_JumpTable) {
        return;  // Not initialized
    }
    
    // Clear the jump table
    std::memset(m_JumpTable.get(), 0, JUMP_TABLE_SIZE * sizeof(CompiledFuncPtr));
    
    if (preserveStructure) {
        // Keep allocated page tables but clear their entries
        for (auto& pageTable : m_PageDirectory) {
            if (pageTable) {
                std::memset(pageTable.get(), 0, PAGE_TABLE_SIZE * sizeof(CompiledFuncPtr));
            }
        }
        
        Log::debugf("CFunctionMap reset (structure preserved, %zu page tables)", 
                    m_AllocatedPageTables);
    } else {
        // Deallocate all page tables
        for (auto& pageTable : m_PageDirectory) {
            pageTable.reset();
        }
        m_AllocatedPageTables = 0;
        
        Log::debug("CFunctionMap reset (all page tables deallocated)");
    }
}

CFunctionMap::CompiledFuncPtr& CFunctionMap::GetOrCreateEntry(uint32_t vaddr) {
    if (!m_JumpTable) {
        throw std::runtime_error("CFunctionMap: Not initialized");
    }
    
    // Get the page directory and table indices
    uint32_t dirIndex = GetPageDirectoryIndex(vaddr);
    uint32_t tblIndex = GetPageTableIndex(vaddr);
    
    // Ensure the page table exists
    auto& pageTable = EnsurePageTable(dirIndex);
    
    // Return reference to the entry
    return pageTable[tblIndex];
}

CFunctionMap::CompiledFuncPtr CFunctionMap::Remove(uint32_t vaddr) noexcept {
    if (!m_JumpTable) {
        return nullptr;
    }
    
    // Remove from jump table (only if it belongs to this address)
    uint32_t jumpIdx = GetJumpTableIndex(vaddr);
    auto oldFunc = m_JumpTable[jumpIdx];
    if (oldFunc && oldFunc->EnterPC() == vaddr) {
        m_JumpTable[jumpIdx] = nullptr;
    } else {
        oldFunc = nullptr;  // not ours — don't return it
    }
    
    // Remove from page table if it exists
    uint32_t dirIndex = GetPageDirectoryIndex(vaddr);
    auto& pageTable = m_PageDirectory[dirIndex];
    
    if (pageTable) {
        uint32_t tblIndex = GetPageTableIndex(vaddr);
        auto oldPageFunc = pageTable[tblIndex];
        pageTable[tblIndex] = nullptr;
        
        // Return the function from page table if it was there, otherwise from jump table
        return oldPageFunc ? oldPageFunc : oldFunc;
    }
    
    return oldFunc;
}

bool CFunctionMap::Contains(uint32_t vaddr) const noexcept {
    if (!m_JumpTable) {
        return false;
    }
    
    // Check if jump table has an entry (verify address to avoid collision)
    if (auto func = m_JumpTable[GetJumpTableIndex(vaddr)]) {
        if (func->EnterPC() == vaddr) return true;
    }
    
    // Check if page table has an entry
    uint32_t dirIndex = GetPageDirectoryIndex(vaddr);
    const auto& pageTable = m_PageDirectory[dirIndex];
    
    if (!pageTable) {
        return false;
    }
    
    return pageTable[GetPageTableIndex(vaddr)] != nullptr;
}

size_t CFunctionMap::GetAllocatedPageTables() const noexcept {
    return m_AllocatedPageTables;
}

CFunctionMap::PageTable& CFunctionMap::EnsurePageTable(uint32_t dirIndex) {
    auto& pageTable = m_PageDirectory[dirIndex];
    
    if (!pageTable) {
        // Allocate a new page table
        pageTable = std::make_unique<CompiledFuncPtr[]>(PAGE_TABLE_SIZE);
        
        // Zero-initialize the new page table
        std::memset(pageTable.get(), 0, PAGE_TABLE_SIZE * sizeof(CompiledFuncPtr));
        
        ++m_AllocatedPageTables;
        
        Log::debugf("CFunctionMap: Allocated page table %u (total: %zu)", 
                    dirIndex, m_AllocatedPageTables);
    }
    
    return pageTable;
}

} // namespace Nova64::Recompiler
