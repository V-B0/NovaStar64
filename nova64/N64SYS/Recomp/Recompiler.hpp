/**
 * @file Recompiler.hpp
 * @brief Main dynamic recompiler class
 * 
 * Nova64 N64 Emulator - Based on Project64 architecture
 * Original Project64 code licensed under GPL-2.0
 */

#pragma once

#include "FunctionMap.hpp"
#include "RecompilerMemory.hpp"
#include "CodeBlock.hpp"
#include "CodeSection.hpp"
#include "ExitInfo.hpp"
#include "RegInfo.hpp"
#include "../Types.hpp"
#include "../SysGlobals.hpp"
#include "../MIPSY/R4300Instruction.hpp"
#include "../../common/MemoryManagment.hpp"
#include <cstdint>
#include <atomic>
#include <vector>
#include <functional>
#include <chrono>
#include <memory>


// Global Forward declarations
class CN64System;
class CRegisters;
class CLog;

namespace N64System {
    class CTLB;
}

namespace Nova64 {

using ::N64System::CTLB;
using ::CRegisters;
using ::CN64System;
using ::CLog;

class MipsMemoryVM;

namespace Recompiler {

// Diagnostic: global per-thread counters incremented on every CompileCode call.
// Used by MainSYS SPIKE log to attribute frame stalls to JIT compilation.
extern std::atomic<uint64_t> g_CompileCount;
extern std::atomic<uint64_t> g_CompileTimeNs;

/**
 * @enum RemoveReason
 * @brief Reasons for removing compiled code
 */
enum class RemoveReason {
    InitialCode,    ///< Initial code load
    Cache,          ///< Cache management
    ProtectedMem,   ///< Protected memory access
    ValidateFunc,   ///< Function validation failed
    TLB,            ///< TLB change
    DMA,            ///< DMA transfer
    StoreInstruc,   ///< Self-modifying code
    Cheats,         ///< Cheat code applied
    MemViewer,      ///< Memory viewer modification
};

/**
 * @struct RecompilerStats
 * @brief Statistics for recompiler performance monitoring
 */
struct RecompilerStats {
    uint64_t blocksCompiled = 0;        ///< Total blocks compiled
    uint64_t blocksExecuted = 0;        ///< Total blocks executed
    uint64_t cacheHits = 0;             ///< Cache hit count
    uint64_t cacheMisses = 0;           ///< Cache miss count
    uint64_t codeInvalidations = 0;     ///< Number of code invalidations
    uint64_t interpretedFallbacks = 0;  ///< Number of interpreter fallback invocations
    uint64_t totalCycles = 0;           ///< Total cycles executed
    uint64_t compilationTimeNs = 0;     ///< Total compilation time in nanoseconds
    size_t   memoryUsed = 0;            ///< Memory used by compiled code
    
    void Reset() noexcept {
        blocksCompiled = 0;
        blocksExecuted = 0;
        cacheHits = 0;
        cacheMisses = 0;
        codeInvalidations = 0;
        interpretedFallbacks = 0;
        totalCycles = 0;
        compilationTimeNs = 0;
        memoryUsed = 0;
    }
    
    double GetCacheHitRate() const noexcept {
        uint64_t total = cacheHits + cacheMisses;
        return total > 0 ? static_cast<double>(cacheHits) / total : 0.0;
    }
};

/**
 * @class CRecompiler
 * @brief Main dynamic recompiler for N64 emulation
 * 
 * Translates MIPS R4300i code to native x86/x64 code at runtime
 * for improved emulation performance.
 */
class CRecompiler : public CFunctionMap, public CRecompMemory {
public:
    /**
     * @brief Construct the recompiler
     * @param system Reference to N64 system
     * @param endEmulation Reference to emulation end flag
     */
    CRecompiler(CN64System& system, std::atomic<bool>& endEmulation);
    ~CRecompiler();
    
    // Non-copyable, non-movable for simplicity
    CRecompiler(const CRecompiler&) = delete;
    CRecompiler& operator=(const CRecompiler&) = delete;
    CRecompiler(CRecompiler&&) = delete;
    CRecompiler& operator=(CRecompiler&&) = delete;
    
    /**
     * @brief Main recompiler execution loop
     */
    void Run();
    
    /**
     * @brief Reset the recompiler state
     * @param allocateMemory Whether to allocate new memory
     */
    void Reset(bool allocateMemory = true);
    
    /**
     * @brief Clear recompiled code at virtual address
     * @param virtualAddress Virtual address to clear
     * @param length Length of code to clear
     * @param reason Reason for clearing
     */
    void ClearRecompCode_Virt(uint32_t virtualAddress, size_t length, RemoveReason reason);
    
    /**
     * @brief Clear recompiled code at physical address
     * @param physicalAddress Physical address to clear
     * @param length Length of code to clear
     * @param reason Reason for clearing
     */
    void ClearRecompCode_Phys(uint32_t physicalAddress, size_t length, RemoveReason reason);
    
    /**
     * @brief Get memory stack position pointer
     * @return Reference to memory stack position
     */
    uint8_t*& MemoryStackPos() noexcept { return m_MemoryStack; }
    
    /**
     * @brief Get the memory stack position (const)
     */
    const uint8_t* MemoryStackPos() const noexcept { return m_MemoryStack; }
    
    /**
     * @brief Reset memory stack to initial position
     */
    void ResetMemoryStackPos() noexcept;
    
    // Statistics and profiling
    void ResetFunctionTimes();
    void DumpFunctionTimes();
    
    /**
     * @brief Get statistics structure
     */
    const RecompilerStats& GetStats() const noexcept { return m_Stats; }
    
    /**
     * @brief Reset all statistics
     */
    void ResetStats() noexcept { m_Stats.Reset(); }
    
    /**
     * @brief Enable or disable profiling
     */
    void SetProfilingEnabled(bool enabled) noexcept { m_ProfilingEnabled = enabled; }
    
    /**
     * @brief Check if profiling is enabled
     */
    bool IsProfilingEnabled() const noexcept { return m_ProfilingEnabled; }
    
    /**
     * @brief Set execution mode
     */
    enum class ExecutionMode {
        VirtualTable,           ///< Use virtual function table
        VirtualTableValidate,   ///< Virtual table with validation
        MemoryChange,           ///< Track memory changes
        Lookup,                 ///< Direct lookup
        LookupValidate          ///< Direct lookup with validation
    };
    
    void SetExecutionMode(ExecutionMode mode) noexcept { m_ExecutionMode = mode; }
    ExecutionMode GetExecutionMode() const noexcept { return m_ExecutionMode; }
    
    // Logging
    void StartLog();
    void StopLog();
    void ResetLog();
    bool IsLogging() const noexcept { return m_LogFile != nullptr; }
    
private:
    // Prevent default construction
    CRecompiler() = delete;
    
    // Core compilation
    CCompiledFunc* CompileCode();
    CCompiledFunc* CompileCode(uint32_t address);
    
    // Execution engine implementations
    void Execute_VirtualTable();
    void Execute_VirtualTable_Validate();
    void Execute_MemoryChange();
    void Execute_Lookup();
    void Execute_Lookup_Validate();
    
    // Interpreter fallback when compilation fails
    void FallbackToInterpreter();
    
    // Interpret a single instruction at current PC and advance
    void InterpretSingleInstruction();
    
    // Helper methods
    void ClearCodeRange(uint32_t start, uint32_t end, RemoveReason reason);
    void HandleSelfModifyingCode(uint32_t address, RemoveReason reason);
    void UpdateProfiling(CCompiledFunc* func, std::chrono::nanoseconds time);
    
    // Function profiling data
    struct FunctionProfile {
        uint32_t address = 0;
        uint64_t callCount = 0;
        std::chrono::nanoseconds totalTime{};
        std::chrono::nanoseconds maxTime{};
        std::chrono::nanoseconds minTime{std::chrono::nanoseconds::max()};
        
        void Reset() {
            callCount = 0;
            totalTime = {};
            maxTime = {};
            minTime = std::chrono::nanoseconds::max();
        }
        
        void Update(std::chrono::nanoseconds time) {
            ++callCount;
            totalTime += time;
            if (time > maxTime) maxTime = time;
            if (time < minTime) minTime = time;
        }
    };
    
    using FunctionProfileMap = std::unordered_map<CCompiledFunc*, FunctionProfile>;
    
    CN64System&         m_System;
    MipsMemoryVM&       m_MMU;
    CRegisters&         m_Reg;
    CTLB&               m_TLB;
    std::atomic<bool>&  m_EndEmulation;
    
    // Memory and code
    uint8_t*            m_MemoryStack = nullptr;
    std::vector<CCompiledFunc*> m_ActiveFunctions;  ///< Functions currently in cache
    std::vector<CCompiledFunc*> m_PendingDeletions; ///< Functions queued for deletion (avoids freeing actively executing JIT blocks)
    void ProcessPendingDeletions();
    
    // Statistics and profiling
    RecompilerStats     m_Stats;
    FunctionProfileMap  m_FunctionProfiles;
    bool                m_ProfilingEnabled = false;
    
    // Execution state
    ExecutionMode       m_ExecutionMode = ExecutionMode::VirtualTable;
    
    // Logging
    CLog*               m_LogFile = nullptr;
    
    // Cached references for performance
    uint64_t&           m_ProgramCounter;
    
    // Timing
    std::chrono::high_resolution_clock::time_point m_LastProfilingReset;
    
    // Configuration flags
    bool                m_EnableSelfModifyingCodeDetection = true;
    bool                m_EnableCodeCache = true;
    size_t              m_MaxCodeCacheSize = 64 * 1024 * 1024;  // 64 MB

    // SMC thrash guard: tracks per-PC recompilation count to suppress
    // infinite recompilation loops. Reset for a given PC when DMA/CACHE
    // performs a "known-good" code write (invalidation).
    std::unordered_map<uint32_t, int> m_SmcThrashMap;

    // Shared JIT runtime — all CodeBlocks share this single runtime so that
    // compiled code lives in one virtual-memory pool instead of one pool per block.
    std::unique_ptr<asmjit::JitRuntime> m_SharedJitRuntime;
};

} // namespace Recompiler
} // namespace Nova64