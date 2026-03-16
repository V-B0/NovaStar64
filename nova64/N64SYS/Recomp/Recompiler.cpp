/**
 * @file Recompiler.cpp
 * @brief Implementation of the main dynamic recompiler
 * 
 * Nova64 N64 Emulator - Based on Project64 architecture
 * Original Project64 code licensed under GPL-2.0
 * 
 * This is the main recompiler engine that translates MIPS R4300i code
 * to native x86/x64 code at runtime for improved performance.
 */

#include "Recompiler.hpp"
#include "RecompilerMemory.hpp"
#include "FunctionMap.hpp"
#include "FunctionInfo.hpp"
#include "CompiledFunc.hpp"
#include "LoopAnalysis.hpp"
#include "RecompilerOps.hpp"
#if defined(__amd64__) || defined(_M_X64)
#include "x64-86/x64RecompilerOps.hpp"
#endif
#include "../MainSYS.hpp"
#include "../SysGlobals.hpp"
#include "../Types.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../MIPSY/TLB.hpp"
#include "../MIPSY/R4300Instruction.hpp"
#include "../../common/log.hpp"
#include "../../common/MemoryManagment.hpp"
#include <algorithm>
#include <cstring>

namespace Nova64::Recompiler {

// =============================================================================
// CRecompiler Implementation
// =============================================================================

CRecompiler::CRecompiler(::CN64System& system, std::atomic<bool>& endEmulation)
    : m_System(system)
    , m_MMU(*system.GetMemory())
    , m_Reg(*system.GetRegisters())
    , m_TLB(*system.GetTLB())
    , m_EndEmulation(endEmulation)
    , m_ProgramCounter(m_Reg.m_PROGRAM_COUNTER)
    , m_LastProfilingReset(std::chrono::high_resolution_clock::now())
{
    Log::info("CRecompiler: Initializing dynamic recompiler");
    
    // Initialize function map
    if (!Initialize()) {
        Log::error("CRecompiler: Failed to initialize function map");
        throw std::runtime_error("Failed to initialize recompiler");
    }
    
    // Allocate memory for compiled code
    if (!EnsureSpace(DEFAULT_INITIAL_SIZE)) {
        Log::error("CRecompiler: Failed to allocate recompiler memory");
        throw std::runtime_error("Failed to allocate recompiler memory");
    }
    
    // Initialize memory stack
    m_MemoryStack = GetWritePosition();
    
    Log::info("CRecompiler: Initialization complete");
}

CRecompiler::~CRecompiler() {
    Log::info("CRecompiler: Shutting down");
    
    // Stop logging if active
    if (m_LogFile) {
        StopLog();
    }
    
    // Clear all compiled functions
    Reset(false);
    
    // Free memory (handled by CRecompMemory destructor via RAII)
    CRecompMemory::Reset();
    
    Log::info("CRecompiler: Shutdown complete");
}

void CRecompiler::Run() {
    Log::info("CRecompiler: Starting execution");
    
    // Select execution mode
    switch (m_ExecutionMode) {
        case ExecutionMode::VirtualTable:
            Execute_VirtualTable();
            break;
            
        case ExecutionMode::VirtualTableValidate:
            Execute_VirtualTable_Validate();
            break;
            
        case ExecutionMode::MemoryChange:
            Execute_MemoryChange();
            break;
            
        case ExecutionMode::Lookup:
            Execute_Lookup();
            break;
            
        case ExecutionMode::LookupValidate:
            Execute_Lookup_Validate();
            break;
            
        default:
            Log::error("CRecompiler: Invalid execution mode");
            return;
    }
    
    Log::info("CRecompiler: Execution ended");
}

void CRecompiler::Reset(bool allocateMemory) {
    Log::info("CRecompiler: Resetting recompiler");
    
    // Clear all compiled functions
    for (auto* func : m_ActiveFunctions) {
        if (func) {
            delete func;
        }
    }
    m_ActiveFunctions.clear();
    
    // Reset function map
    CFunctionMap::Reset(false);
    
    // Reset statistics
    m_Stats.Reset();
    
    // Reset profiling data
    m_FunctionProfiles.clear();
    m_LastProfilingReset = std::chrono::high_resolution_clock::now();
    
    // Re-allocate memory if requested
    if (allocateMemory) {
        CRecompMemory::Reset();
        if (!EnsureSpace(DEFAULT_INITIAL_SIZE)) {
            Log::error("CRecompiler: Failed to re-allocate memory");
        }
        m_MemoryStack = GetWritePosition();
    }
    
    // Re-initialize function map
    if (allocateMemory) {
        Initialize();
    }
    
    Log::info("CRecompiler: Reset complete");
}

void CRecompiler::ClearRecompCode_Virt(uint32_t virtualAddress, size_t length, RemoveReason reason) {
    Log::debugf("CRecompiler: Clearing virtual code at 0x%08X, length %zu", virtualAddress, length);
    
    uint32_t endAddress = virtualAddress + static_cast<uint32_t>(length);
    
    // Check memory usage before clearing - uses RecompilerMemory
    {
        const auto& memStats = CRecompMemory::GetStats();
        Log::debugf("CRecompiler: Memory before clear - %u KB used / %u KB allocated",
                   memStats.usedBytes / 1024, memStats.allocatedBytes / 1024);
    }
    
    // Use FunctionMap to clear code in range - creates dependency
    ClearCodeRange(virtualAddress, endAddress, reason);
    
    // Update memory statistics
    {
        const auto& memStats = CRecompMemory::GetStats();
        Log::debugf("CRecompiler: Memory after clear - %u KB used / %u KB allocated",
                   memStats.usedBytes / 1024, memStats.allocatedBytes / 1024);
    }
    
    ++m_Stats.codeInvalidations;
}

void CRecompiler::ClearRecompCode_Phys(uint32_t physicalAddress, size_t length, RemoveReason reason) {
    Log::debugf("CRecompiler: Clearing physical code at 0x%08X, length %zu", physicalAddress, length);
    
    // Convert physical to virtual address
    // This is simplified - full implementation would handle TLB mapping
    uint32_t virtualAddress = physicalAddress | 0x80000000;  // KSEG0 mapping
    
    ClearRecompCode_Virt(virtualAddress, length, reason);
}

void CRecompiler::ResetMemoryStackPos() noexcept {
    m_MemoryStack = GetWritePosition();
}

void CRecompiler::ResetFunctionTimes() {
    Log::info("CRecompiler: Resetting function profiling times");
    
    for (auto& pair : m_FunctionProfiles) {
        pair.second.Reset();
    }
    
    m_LastProfilingReset = std::chrono::high_resolution_clock::now();
}

void CRecompiler::DumpFunctionTimes() {
    if (m_FunctionProfiles.empty()) {
        Log::info("CRecompiler: No profiling data available");
        return;
    }
    
    Log::info("CRecompiler: Function Profiling Report");
    Log::info("========================================");
    
    // Sort by total time
    std::vector<std::pair<CCompiledFunc*, FunctionProfile>> sorted;
    sorted.reserve(m_FunctionProfiles.size());
    
    for (const auto& pair : m_FunctionProfiles) {
        sorted.push_back(pair);
    }
    
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) {
            return a.second.totalTime > b.second.totalTime;
        });
    
    // Print top functions
    size_t count = std::min<size_t>(20, sorted.size());
    for (size_t i = 0; i < count; ++i) {
        const auto& profile = sorted[i].second;
        double avgTimeUs = profile.totalTime.count() / static_cast<double>(profile.callCount) / 1000.0;
        
        Log::infof("Address: 0x%08X, Calls: %llu, Total: %lld us, Avg: %.2f us",
                   profile.address, profile.callCount,
                   profile.totalTime.count() / 1000, avgTimeUs);
    }
}

void CRecompiler::StartLog() {
    if (m_LogFile) {
        Log::warning("CRecompiler: Logging already active");
        return;
    }
    
    // TODO: Create log file
    Log::info("CRecompiler: Started logging");
}

void CRecompiler::StopLog() {
    if (!m_LogFile) {
        Log::warning("CRecompiler: Logging not active");
        return;
    }
    
    // TODO: Close log file
    m_LogFile = nullptr;
    
    Log::info("CRecompiler: Stopped logging");
}

void CRecompiler::ResetLog() {
    bool wasLogging = (m_LogFile != nullptr);
    
    if (wasLogging) {
        StopLog();
    }
    
    // TODO: Delete log file
    
    if (wasLogging) {
        StartLog();
    }
    
    Log::info("CRecompiler: Log reset");
}

// =============================================================================
// Core Compilation
// =============================================================================

CCompiledFunc* CRecompiler::CompileCode() {
    uint32_t pc = static_cast<uint32_t>(m_ProgramCounter);
    return CompileCode(pc);
}

CCompiledFunc* CRecompiler::CompileCode(uint32_t address) {
    auto startTime = std::chrono::high_resolution_clock::now();
    
    Log::debugf("CRecompiler: Compiling code at 0x%08X", address);
    
    // Check if already compiled
    auto* existingFunc = Lookup(address);
    if (existingFunc) {
        ++m_Stats.cacheHits;
        return existingFunc;
    }
    
    ++m_Stats.cacheMisses;
    
    CCompiledFunc* compiledFunc = nullptr;
    
    try {
        // ---- Step 1: Create and analyse the CodeBlock -------------------
        CodeBlock::Config blockCfg;
        blockCfg.enable_profiling = m_ProfilingEnabled;
        blockCfg.enable_optimizations = true;
        blockCfg.max_instructions = 500;

        auto codeBlock = std::make_unique<CodeBlock>(
            m_System, address, blockCfg);

        CodeBlock::CompileOptions opts;
        opts.optimization_level = 2;

        auto compileResult = codeBlock->compile(opts);
        if (compileResult != CodeBlock::CompileResult::Success) {
            Log::debugf("CRecompiler: CodeBlock analysis/codegen failed at 0x%08X (result=%d)",
                        address, static_cast<int>(compileResult));
            return nullptr;
        }

        // ---- Step 2: Finalize – get executable memory from AsmJit ------
        // finalize() calls runtime_->add() which copies the code into
        // executable (RWX) memory and returns the entry-point address.
        // We pass a dummy RecompilationMemory – the actual backing comes
        // from AsmJit's JitRuntime which internally uses VirtualAlloc/mmap.
        auto entryOpt = codeBlock->finalize(
            *reinterpret_cast<RecompilationMemory*>(this));  // unused param

        if (!entryOpt.has_value()) {
            Log::debugf("CRecompiler: Finalize failed at 0x%08X", address);
            return nullptr;
        }

        auto entryPtr = reinterpret_cast<BlockFunction>(entryOpt.value());

        // ---- Step 3: Wrap in CCompiledFunc and store in map ------------
        auto* funcMem = new CCompiledFunc(
            address,
            codeBlock->start_address(),
            codeBlock->end_address(),
            entryPtr,
            std::move(codeBlock));

        // Store in function map (O(1) lookup by virtual address)
        Store(address, funcMem);

        // Track for cleanup
        m_ActiveFunctions.push_back(funcMem);

        compiledFunc = funcMem;
        m_Stats.memoryUsed += funcMem->GetCodeBlock()
            ? funcMem->GetCodeBlock()->statistics().generated_bytes
            : 0;
    }
    catch (const std::exception& e) {
        Log::errorf("CRecompiler: Compilation failed at 0x%08X: %s", address, e.what());
        compiledFunc = nullptr;
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto compileTime = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime);
    m_Stats.compilationTimeNs += compileTime.count();
    
    if (compiledFunc) {
        ++m_Stats.blocksCompiled;
    }
    
    return compiledFunc;
}

// =============================================================================
// Execution Engine Implementations
// =============================================================================

void CRecompiler::Execute_VirtualTable() {
    Log::debug("CRecompiler: Executing with VirtualTable mode");
    
    while (!m_EndEmulation.load(std::memory_order_acquire)) {
        uint32_t pc = static_cast<uint32_t>(m_ProgramCounter);
        
        // Lookup compiled function
        auto* func = Lookup(pc);
        
        if (!func) {
            // Need to compile
            func = CompileCode(pc);
            
            if (!func) {
                // Compilation failed - fall back to interpreter
                Log::debugf("CRecompiler: Compilation failed at 0x%08X, falling back to interpreter", pc);
                break;
            }
        }
        
        // Execute compiled function
        ++m_Stats.blocksExecuted;
        func->Execute();
    }
}

void CRecompiler::Execute_VirtualTable_Validate() {
    Log::debug("CRecompiler: Executing with VirtualTable_Validate mode");
    
    while (!m_EndEmulation.load(std::memory_order_acquire)) {
        uint32_t pc = static_cast<uint32_t>(m_ProgramCounter);
        
        // Lookup and validate compiled function
        auto* func = Lookup(pc);
        
        if (func) {
            // Validate function is still valid (self-modifying code detection)
            if (m_EnableSelfModifyingCodeDetection && !func->MemoryValid()) {
                // Code changed – invalidate and recompile
                ClearRecompCode_Virt(pc, func->MaxPC() - func->MinPC() + 4,
                                     RemoveReason::StoreInstruc);
                func = nullptr;
            }
        }
        
        if (!func) {
            func = CompileCode(pc);
            if (!func) break;
        }
        
        ++m_Stats.blocksExecuted;
        func->Execute();
    }
}

void CRecompiler::Execute_MemoryChange() {
    Log::debug("CRecompiler: Executing with MemoryChange mode");
    
    while (!m_EndEmulation.load(std::memory_order_acquire)) {
        uint32_t pc = static_cast<uint32_t>(m_ProgramCounter);
        
        // Check for memory changes before executing
        if (m_EnableSelfModifyingCodeDetection) {
            auto* existingFunc = Lookup(pc);
            if (existingFunc && !existingFunc->MemoryValid()) {
                ClearRecompCode_Virt(pc, existingFunc->MaxPC() - existingFunc->MinPC() + 4,
                                     RemoveReason::StoreInstruc);
            }
        }
        
        auto* func = Lookup(pc);
        if (!func) {
            func = CompileCode(pc);
            if (!func) break;
        }
        
        ++m_Stats.blocksExecuted;
        func->Execute();
    }
}

void CRecompiler::Execute_Lookup() {
    Log::debug("CRecompiler: Executing with Lookup mode");
    
    while (!m_EndEmulation.load(std::memory_order_acquire)) {
        uint32_t pc = static_cast<uint32_t>(m_ProgramCounter);
        
        auto* func = Lookup(pc);
        if (!func) {
            func = CompileCode(pc);
            if (!func) break;
        }
        
        ++m_Stats.blocksExecuted;
        func->Execute();
    }
}

void CRecompiler::Execute_Lookup_Validate() {
    Log::debug("CRecompiler: Executing with Lookup_Validate mode");
    
    while (!m_EndEmulation.load(std::memory_order_acquire)) {
        uint32_t pc = static_cast<uint32_t>(m_ProgramCounter);
        
        auto* func = Lookup(pc);
        
        if (func && m_EnableSelfModifyingCodeDetection && !func->MemoryValid()) {
            ClearRecompCode_Virt(pc, func->MaxPC() - func->MinPC() + 4,
                                 RemoveReason::StoreInstruc);
            func = nullptr;
        }
        
        if (!func) {
            func = CompileCode(pc);
            if (!func) break;
        }
        
        ++m_Stats.blocksExecuted;
        func->Execute();
    }
}

// =============================================================================
// Helper Methods
// =============================================================================

void CRecompiler::ClearCodeRange(uint32_t start, uint32_t end, RemoveReason reason) {
    Log::debugf("CRecompiler: Clearing code range 0x%08X - 0x%08X", start, end);
    
    // Find and remove all functions in range
    std::vector<CCompiledFunc*> toRemove;
    
    for (auto* func : m_ActiveFunctions) {
        if (!func) continue;
        
        // Check if func is in range [start, end]
        // Since CCompiledFunc is opaque, we iterate through the function map
        // and check if any address in range maps to this function
        bool inRange = false;
        for (uint32_t addr = start; addr < end && !inRange; addr += 4) {
            if (Lookup(addr) == func) {
                inRange = true;
            }
        }
        
        if (inRange) {
            toRemove.push_back(func);
        }
    }
    
    // Remove functions
    for (auto* func : toRemove) {
        // Remove from function map
        // Scan and remove all mappings to this function
        for (uint32_t addr = start; addr < end; addr += 4) {
            if (Lookup(addr) == func) {
                Remove(addr);
            }
        }
        
        // Remove from active list
        auto it = std::find(m_ActiveFunctions.begin(), m_ActiveFunctions.end(), func);
        if (it != m_ActiveFunctions.end()) {
            m_ActiveFunctions.erase(it);
        }
        
        delete func;
    }
    
    Log::debugf("CRecompiler: Removed %zu functions", toRemove.size());
}

void CRecompiler::HandleSelfModifyingCode(uint32_t address, RemoveReason reason) {
    Log::debugf("CRecompiler: Detected self-modifying code at 0x%08X", address);
    
    // Clear affected code
    ClearRecompCode_Virt(address, 4, reason);  // Clear at least one instruction
}

void CRecompiler::UpdateProfiling(CCompiledFunc* func, std::chrono::nanoseconds time) {
    if (!m_ProfilingEnabled || !func) {
        return;
    }
    
    auto& profile = m_FunctionProfiles[func];
    
    // Initialize address on first use
    if (profile.address == 0) {
        // Find address in function map
        // Since we don't have GetAddress(), search the map
        for (uint32_t addr = 0x80000000; addr < 0x80800000; addr += 4) {
            if (Lookup(addr) == func) {
                profile.address = addr;
                break;
            }
        }
    }
    
    profile.Update(time);
}

} // namespace Nova64::Recompiler
