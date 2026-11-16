/**
 * @file FunctionMap.cpp
 * @brief Implementation of function lookup table management
 * 
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
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

CFunctionMap::CFunctionMap() : m_JumpTable(nullptr), m_AllocatedPageTables(0) {
    // Don't allocate in constructor - wait for Initialize()
}

bool CFunctionMap::Initialize() {
    try {
        m_JumpTable = std::make_unique<CompiledFuncPtr[]>(JUMP_TABLE_SIZE);

        std::memset(m_JumpTable.get(), 0, JUMP_TABLE_SIZE * sizeof(CompiledFuncPtr));

        // Page tables are allocated on demand (see EnsurePageTable).

        Log::debugf("CFunctionMap initialized with %zu jump table entries", JUMP_TABLE_SIZE);

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
        return; // Not initialized
    }

    std::memset(m_JumpTable.get(), 0, JUMP_TABLE_SIZE * sizeof(CompiledFuncPtr));

    m_ExactLookup.clear();

    if (preserveStructure) {
        for (auto& pageTable : m_PageDirectory) {
            if (pageTable) {
                std::memset(pageTable.get(), 0, PAGE_TABLE_SIZE * sizeof(CompiledFuncPtr));
            }
        }

        Log::debugf("CFunctionMap reset (structure preserved, %zu page tables)", m_AllocatedPageTables);
    } else {
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

    uint32_t dirIndex = GetPageDirectoryIndex(vaddr);
    uint32_t tblIndex = GetPageTableIndex(vaddr);

    auto& pageTable = EnsurePageTable(dirIndex);

    return pageTable[tblIndex];
}

CFunctionMap::CompiledFuncPtr CFunctionMap::Remove(uint32_t vaddr) noexcept {
    if (!m_JumpTable) {
        return nullptr;
    }

    CompiledFuncPtr removed = nullptr;
    auto it = m_ExactLookup.find(vaddr);
    if (it != m_ExactLookup.end()) {
        removed = it->second;
        m_ExactLookup.erase(it);
    }

    // Remove from jump table (only if it belongs to this address)
    uint32_t jumpIdx = GetJumpTableIndex(vaddr);
    if (auto func = m_JumpTable[jumpIdx]) {
        if (func->EnterPC() == vaddr) {
            m_JumpTable[jumpIdx] = nullptr;
        }
    }

    return removed;
}

bool CFunctionMap::Contains(uint32_t vaddr) const noexcept { return m_ExactLookup.count(vaddr) != 0; }

size_t CFunctionMap::GetAllocatedPageTables() const noexcept { return m_AllocatedPageTables; }

CFunctionMap::PageTable& CFunctionMap::EnsurePageTable(uint32_t dirIndex) {
    auto& pageTable = m_PageDirectory[dirIndex];

    if (!pageTable) {
        pageTable = std::make_unique<CompiledFuncPtr[]>(PAGE_TABLE_SIZE);

        std::memset(pageTable.get(), 0, PAGE_TABLE_SIZE * sizeof(CompiledFuncPtr));

        ++m_AllocatedPageTables;

        Log::debugf("CFunctionMap: Allocated page table %u (total: %zu)", dirIndex, m_AllocatedPageTables);
    }

    return pageTable;
}

} // namespace Nova64::Recompiler
