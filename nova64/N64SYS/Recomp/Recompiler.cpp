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
#elif defined(__i386__) || defined(_M_IX86)
#include "x86/x86RecompilerOps.hpp"
#elif defined(__aarch64__) || defined(_M_ARM64)
#include "Arm64/arm64RecompilerOps.hpp"
#endif
#include "../MainSYS.hpp"
#include "../SysGlobals.hpp"
#include "../Types.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../MIPSY/TLB.hpp"
#include "../MIPSY/R4300Instruction.hpp"
#include "../MIPSY/Systiming.hpp"
#include "../MIPSY/Sysevents.hpp"
#include "../../common/log.hpp"
#include "../../common/trace.hpp"
#include "../../common/MemoryManagment.hpp"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

// =============================================================================
// SEH wrapper for safe JIT code execution (MSVC/Windows only)
// =============================================================================
// __try/__except cannot be used in functions with C++ objects that have
// non-trivial destructors (MSVC restriction), so this must be a standalone
// function.
#ifdef _MSC_VER
static bool SafeExecuteBlock(Nova64::Recompiler::CCompiledFunc* func, uint32_t pc) {
    __try {
        func->Execute();
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
              ? EXCEPTION_EXECUTE_HANDLER
              : EXCEPTION_CONTINUE_SEARCH) {
        Log::errorf("CRecompiler: Access violation executing compiled block at PC 0x%08X", pc);
        return false;
    }
}
#else
static bool SafeExecuteBlock(Nova64::Recompiler::CCompiledFunc* func, uint32_t /*pc*/) {
    func->Execute();
    return true;
}
#endif

namespace Nova64::Recompiler {

// Diagnostic counters (defined in Recompiler.hpp as extern)
std::atomic<uint64_t> g_CompileCount{0};
std::atomic<uint64_t> g_CompileTimeNs{0};

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
    , m_SharedJitRuntime(std::make_unique<asmjit::JitRuntime>())
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

    // Register DMA invalidation callback so PI/SP DMA invalidates
    // any compiled blocks that overlap the RDRAM target range.
    auto dmaInvalidate = [](void* ctx, uint32_t physAddr, uint32_t length) {
        static_cast<CRecompiler*>(ctx)->ClearRecompCode_Phys(
            physAddr, length, RemoveReason::DMA);
    };
    m_System.m_DMAInvalidateFn  = dmaInvalidate;
    m_System.m_DMAInvalidateCtx = this;

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
    Log::infof("CRecompiler STATS: blocksCompiled=%llu blocksExecuted=%llu "
               "cacheHits=%llu cacheMisses=%llu codeInvalidations=%llu "
               "interpretedFallbacks=%llu memoryUsed=%zu compilationTimeNs=%llu",
               m_Stats.blocksCompiled, m_Stats.blocksExecuted,
               m_Stats.cacheHits, m_Stats.cacheMisses,
               m_Stats.codeInvalidations, m_Stats.interpretedFallbacks,
               m_Stats.memoryUsed, m_Stats.compilationTimeNs);

    // Unregister DMA invalidation callback
    m_System.m_DMAInvalidateFn  = nullptr;
    m_System.m_DMAInvalidateCtx = nullptr;
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
    
    // When DMA/CACHE invalidates code, reset the SMC thrash guard for
    // any PCs in the cleared range. DMA provides a "known-good" state,
    // so future MemoryValid() checks should recompile from fresh code
    // instead of being suppressed by the stale thrash counter.
    if (reason == RemoveReason::DMA) {
        for (auto it = m_SmcThrashMap.begin(); it != m_SmcThrashMap.end(); ) {
            if (it->first >= virtualAddress && it->first < endAddress) {
                it = m_SmcThrashMap.erase(it);
            } else {
                ++it;
            }
        }
    }
    
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
    Log::debugf("CRecompiler: ClearRecompCode_Phys(0x%08X, len=%zu, reason=%d) activeFuncs=%zu",
               physicalAddress, length, static_cast<int>(reason), m_ActiveFunctions.size());
    
    // Clear SMC bitmap for the affected physical chunks
    if (physicalAddress < 0x800000) {
        auto* bitmap = m_MMU.GetSmcPageBitmap();
        uint32_t chunkStart = physicalAddress >> MipsMemoryVM::kSmcShift;
        uint32_t chunkEnd = std::min((physicalAddress + static_cast<uint32_t>(length) +
                                      MipsMemoryVM::kSmcChunkSize - 1) >> MipsMemoryVM::kSmcShift,
                                     MipsMemoryVM::kSmcChunks);
        for (uint32_t chunk = chunkStart; chunk < chunkEnd; ++chunk)
            bitmap[chunk] = 0;
    }

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
    
    m_LogFile = new CLog();
    if (!m_LogFile->Open("recompiler.log")) {
        Log::error("CRecompiler: Failed to create log file");
        delete m_LogFile;
        m_LogFile = nullptr;
        return;
    }
    
    Log::info("CRecompiler: Started logging");
}

void CRecompiler::StopLog() {
    if (!m_LogFile) {
        Log::warning("CRecompiler: Logging not active");
        return;
    }
    
    m_LogFile->Close();
    delete m_LogFile;
    m_LogFile = nullptr;
    
    Log::info("CRecompiler: Stopped logging");
}

void CRecompiler::ResetLog() {
    bool wasLogging = (m_LogFile != nullptr);
    
    if (wasLogging) {
        StopLog();
    }
    
    // Delete existing log file
    std::filesystem::remove("recompiler.log");
    
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
    
    // Check if already compiled
    auto* existingFunc = Lookup(address);
    if (existingFunc) {
        ++m_Stats.cacheHits;
        return existingFunc;
    }
    
    ++m_Stats.cacheMisses;
    
    CCompiledFunc* compiledFunc = nullptr;
    
    Log::infof("CRecompiler: Compiling new block at 0x%08X", address);

    try {
        // ---- Step 1: Create and analyse the CodeBlock -------------------
        CodeBlock::Config blockCfg;
        blockCfg.enable_profiling = m_ProfilingEnabled;
        blockCfg.enable_optimizations = true;
        blockCfg.max_instructions = 500;

        auto codeBlock = std::make_unique<CodeBlock>(
            m_System, address, blockCfg, m_SharedJitRuntime.get());

        CodeBlock::CompileOptions opts;
        opts.optimization_level = 2;

        auto compileResult = codeBlock->compile(opts);
        if (compileResult != CodeBlock::CompileResult::Success) {
            // Read the failing instruction to help debug
            uint32_t failInstr = 0;
            m_MMU.MemoryValue32(address, failInstr);
            uint32_t failOp = (failInstr >> 26) & 0x3F;
            uint32_t failFunct = failInstr & 0x3F;
            Log::debugf("CRecompiler: CodeBlock analysis/codegen failed at 0x%08X (result=%d)",
                        address, static_cast<int>(compileResult));
            // Dump the internal log from CodeBlock for debugging
            auto blockLog = codeBlock->get_code_log();
            if (!blockLog.empty()) {
                Log::debugf("CRecompiler: CodeBlock log: %s", blockLog.c_str());
            }
            return nullptr;
        }

        // ---- Step 2: Finalize -- get executable memory from AsmJit ------
        // finalize() calls runtime_->add() which copies the code into
        // executable (RWX) memory and returns the entry-point address.
        // We pass a dummy RecompilationMemory -- the actual backing comes
        // from AsmJit's JitRuntime which internally uses VirtualAlloc/mmap.
        auto entryOpt = codeBlock->finalize(
            *reinterpret_cast<RecompilationMemory*>(this));  // unused param

        if (!entryOpt.has_value()) {
            Log::debugf("CRecompiler: Finalize failed at 0x%08X", address);
            return nullptr;
        }

        auto entryPtr = reinterpret_cast<BlockFunction>(entryOpt.value());

        // ---- Step 3: Wrap in CCompiledFunc and store in map ------------
        
        // Cache addresses BEFORE moving codeBlock (argument evaluation order is unspecified!)
        uint32_t startAddr = codeBlock->start_address();
        uint32_t endAddr   = codeBlock->end_address();

        auto* funcMem = new CCompiledFunc(
            address,
            startAddr,
            endAddr,
            entryPtr,
            std::move(codeBlock));

        // Store in function map (O(1) lookup by virtual address)
        Store(address, funcMem);

        // Mark physical RDRAM chunks covered by this function in the SMC bitmap
        // so that SW fast paths can detect writes to code regions.
        {
            uint32_t physStart = startAddr & 0x1FFFFFFF;
            uint32_t physEnd   = endAddr   & 0x1FFFFFFF;
            if (physEnd < 0x800000) { // within RDRAM
                uint32_t chunkStart = physStart >> MipsMemoryVM::kSmcShift;
                uint32_t chunkEnd   = physEnd   >> MipsMemoryVM::kSmcShift;
                auto* bitmap = m_MMU.GetSmcPageBitmap();
                for (uint32_t chunk = chunkStart; chunk <= chunkEnd; ++chunk)
                    bitmap[chunk] = 1;
            }
        }

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

    // Diagnostic globals (read by MainSYS SPIKE log to attribute frame stalls)
    g_CompileCount.fetch_add(1, std::memory_order_relaxed);
    g_CompileTimeNs.fetch_add(compileTime.count(), std::memory_order_relaxed);
    
    return compiledFunc;
}

// =============================================================================
// Execution Engine Implementations
// =============================================================================

void CRecompiler::Execute_VirtualTable() {
    Log::info("CRecompiler: Executing with VirtualTable mode");
    
    const uint32_t countPerOp = m_System.CountPerOp();
    int32_t& nextTimer = m_System.m_NextTimer;
    auto* systemTimer = m_System.m_SystemTimer.get();
    auto* systemEvents = m_System.m_SystemEvents.get();
    
    while (!m_EndEmulation.load(std::memory_order_relaxed)) {
        uint32_t pc = static_cast<uint32_t>(m_ProgramCounter);

        // Lookup compiled function
        auto* func = Lookup(pc);
        
        if (!func) {
            // Need to compile
            func = CompileCode(pc);
            
            if (!func) {
                // For TLB-mapped regions (USEG or KSEG2), trigger I-TLB miss
                // instead of silently skipping the instruction.
                if (pc < 0x80000000 || pc >= 0xC0000000) {
                    m_Reg.TriggerAddressException(
                        static_cast<int32_t>(pc),
                        2 /*EXC_RMISS*/);
                    if (m_System.m_PipelineStage == N64Types::PipelineStage::Jump) {
                        m_ProgramCounter = m_System.m_JumpToLocation;
                        m_System.m_PipelineStage = N64Types::PipelineStage::Normal;
                    }
                    continue;
                }

                // Compilation failed for this block (KSEG0/KSEG1).
                // Interpret the instruction at current PC and continue.
                InterpretSingleInstruction();
                
                // Timer and events already handled by interpreter.
                if (m_System.m_PipelineStage == N64Types::PipelineStage::Jump) {
                    m_ProgramCounter = m_System.m_JumpToLocation;
                    m_System.m_PipelineStage = N64Types::PipelineStage::Normal;
                }
                continue;
            }
        }

        // Execute compiled function directly (no per-block SMC check;
        // DMA and CACHE callbacks handle code invalidation).
        ++m_Stats.blocksExecuted;
        func->Execute();
        ProcessPendingDeletions();
        
        // Deferred CheckInterrupts after ERET: on real VR4300, ERET lets at
        // least one instruction execute before servicing pending interrupts.
        // The ERET callback sets this flag instead of calling CheckInterrupts
        // directly, and we service it here — after the block at the ERET
        // target has executed.
        if (m_System.m_DeferredCheckInterrupts) {
            m_System.m_DeferredCheckInterrupts = false;
            m_Reg.CheckInterrupts();
        }

        // Apply any pending jump (branch target or synchronous exception).
        if (m_System.m_PipelineStage == N64Types::PipelineStage::Jump) {
            m_ProgramCounter = m_System.m_JumpToLocation;
            m_System.m_PipelineStage = N64Types::PipelineStage::Normal;
        }

        // Address Error on instruction fetch: JR/JALR can set a misaligned
        // target.  On real hardware this fires after the delay slot executes,
        // which matches our timing (block already ran the delay slot above).
        {
            uint32_t nextPC = static_cast<uint32_t>(m_ProgramCounter);
            if ((nextPC & 3) != 0) {
                m_Reg.DoAddressError(
                    static_cast<int64_t>(static_cast<int32_t>(nextPC)), true);
                if (m_System.m_PipelineStage == N64Types::PipelineStage::Jump) {
                    m_ProgramCounter = m_System.m_JumpToLocation;
                    m_System.m_PipelineStage = N64Types::PipelineStage::Normal;
                }
            }
        }

        // ================================================================
        // FAST PATH: For forward branches and fallthrough, skip timer/event
        // processing entirely and immediately execute the next block.
        // Timer/events are only checked on backward branches or TestTimer,
        // matching the interpreter's gating where CheckTimer is true only
        // for backward branches (target < branch_addr + 4) or TestTimer.
        // This eliminates ~150-300ns of per-block overhead for the common
        // case (forward flow), which dominates with ~300K blocks/frame.
        // ================================================================
        {
            uint32_t newPC = static_cast<uint32_t>(m_ProgramCounter);

            // Add periodic logging to identify where the CPU is spinning
            static uint64_t blockCounter = 0;
            if ((++blockCounter % 100000) == 0) {
                Log::infof("CRecompiler: PC=0x%08X (blocks: %llu)", newPC, (unsigned long long)m_Stats.blocksExecuted);
            }

            // CRITICAL FIX: If m_NextTimer <= 0, we MUST enter the slow path to process timers
            // and trigger interrupts. Also compare against this block's end, not its entry:
            // a branch can loop backward inside the same compiled block while still landing
            // at an address greater than the block entry PC.
            const uint32_t blockEndPC = func->MaxPC();
            if (newPC > blockEndPC && !m_System.m_TestTimer && m_System.m_NextTimer > 0) {
                // Even on the fast path, process pending system events
                // (interrupts, resets) so they aren't delayed across many
                // forward-only blocks.  Timer processing is still deferred
                // to the slow path for performance.
                if (systemEvents && systemEvents->DoSomething()) {
                    systemEvents->ExecuteEvents();
                    if (m_System.m_PipelineStage == N64Types::PipelineStage::Jump) {
                        m_ProgramCounter = m_System.m_JumpToLocation;
                        m_System.m_PipelineStage = N64Types::PipelineStage::Normal;
                    }
                }
                continue;
            }
        }

        // ================================================================
        // SLOW PATH: Backward branch, self-loop, or TestTimer.
        // Process idle detection, timers, events, and frame limit.
        // ================================================================
        static uint64_t slowPathCounter = 0;
        if ((++slowPathCounter % 10000) == 0) {
            // Log::infof("CRecompiler: SLOW PATH PC=0x%08X (nextTimer=%d testTimer=%d EXL=%d IE=%d pend=%x)", 
            //           pc, nextTimer, 
            //           m_System.m_TestTimer, m_Reg.STATUS_REGISTER.ExceptionLevel, m_Reg.STATUS_REGISTER.InterruptEnable, (m_Reg.CAUSE_REGISTER.PendingInterrupts & m_Reg.STATUS_REGISTER.InterruptMask));
        }

        m_System.m_TestTimer = false;

        // Periodic interrupt check in slow path (e.g. after Status register write)
        m_Reg.CheckInterrupts();
        
        // Idle loop detection: if the block is a tiny self-loop (e.g. `b .` + NOP),
        // advance the timer directly to the next event instead of spinning.
        {
            uint32_t newPC = static_cast<uint32_t>(m_ProgramCounter);
            if (newPC == pc && nextTimer > 0) {
                // Check instruction count for idle detection
                uint32_t instrCount = 1;
                if (auto* block = func->GetCodeBlock()) {
                    instrCount = static_cast<uint32_t>(block->instruction_count());
                    if (instrCount == 0) instrCount = 1;
                }
                if (instrCount <= 2) {
                    systemTimer->UpdateTimers();
                    nextTimer = 0 - static_cast<int32_t>(countPerOp);
                    systemTimer->UpdateTimers();
                }
            }
        }
        
        // Timer and event processing
        m_System.m_TestTimer = false;
        if (nextTimer < 0) {
            systemTimer->TimerDone();
        }
        if (systemEvents && systemEvents->DoSomething()) {
            systemEvents->ExecuteEvents();
        }
        
        // After processing events, check if an async interrupt redirected
        // execution via TriggerException (from ExecuteEvents -> DoIntrException).
        if (m_System.m_PipelineStage == N64Types::PipelineStage::Jump) {
            m_ProgramCounter = m_System.m_JumpToLocation;
            m_System.m_PipelineStage = N64Types::PipelineStage::Normal;
        }

        // Check frame limit (for --frames N CLI option)
        if (m_System.IsFrameLimitReached()) {
            m_EndEmulation.store(true, std::memory_order_release);
            break;
        }
    }
}

void CRecompiler::Execute_VirtualTable_Validate() {
    Log::info("CRecompiler: Executing with VirtualTable_Validate mode");
    
    const uint32_t countPerOp = m_System.CountPerOp();
    int32_t& nextTimer = m_System.m_NextTimer;
    auto* systemTimer = m_System.m_SystemTimer.get();
    auto* systemEvents = m_System.m_SystemEvents.get();
    
    while (!m_EndEmulation.load(std::memory_order_acquire)) {
        uint32_t pc = static_cast<uint32_t>(m_ProgramCounter);
        
        // Lookup and validate compiled function
        auto* func = Lookup(pc);
        
        if (func) {
            // Validate function is still valid (self-modifying code detection)
            if (m_EnableSelfModifyingCodeDetection && !func->MemoryValid()) {
                // Code changed -- invalidate and recompile
                ClearRecompCode_Virt(pc, func->MaxPC() - func->MinPC() + 4,
                                     RemoveReason::StoreInstruc);
                func = nullptr;
            }
        }
        
        if (!func) {
            func = CompileCode(pc);
            if (!func) {
                Log::warningf("CRecompiler: Compilation failed at 0x%08X, interpreting instruction", pc);
                InterpretSingleInstruction();
                continue;
            }
        }
        
        ++m_Stats.blocksExecuted;
        if (!SafeExecuteBlock(func, pc)) { ProcessPendingDeletions();
            ClearRecompCode_Virt(pc, func->MaxPC() - func->MinPC() + 4,
                                 RemoveReason::Cache);
            InterpretSingleInstruction();
            continue;
        }
        
        ProcessPendingDeletions(); // Apply any pending jump (branch or synchronous exception)
        if (m_System.m_PipelineStage == N64Types::PipelineStage::Jump) {
            m_ProgramCounter = m_System.m_JumpToLocation;
            m_System.m_PipelineStage = N64Types::PipelineStage::Normal;
        }
        
        // Timer handling
        uint32_t instrCount = 1;
        if (auto* block = func->GetCodeBlock()) {
            instrCount = static_cast<uint32_t>(block->instruction_count());
            if (instrCount == 0) instrCount = 1;
        }
        // nextTimer decrement now done per-instruction inside compiled blocks
        
        // Idle loop detection
        {
            uint32_t newPC = static_cast<uint32_t>(m_ProgramCounter);
            if (newPC == pc && instrCount <= 2 && nextTimer > 0) {
                nextTimer = 0 - static_cast<int32_t>(m_System.CountPerOp());
            }
        }
        
        {
            uint32_t newPC = static_cast<uint32_t>(m_ProgramCounter);
            uint32_t blockEndPC = pc + instrCount * 4;
            bool checkTimer = (newPC < blockEndPC || m_System.m_TestTimer);
            if (checkTimer) {
                m_System.m_TestTimer = false;
                if (nextTimer < 0) {
                    systemTimer->TimerDone();
                }
                if (systemEvents && systemEvents->DoSomething()) {
                    systemEvents->ExecuteEvents();
                }
            }
        }
        
        if (m_System.m_PipelineStage == N64Types::PipelineStage::Jump) {
            m_ProgramCounter = m_System.m_JumpToLocation;
            m_System.m_PipelineStage = N64Types::PipelineStage::Normal;
        }

        if (m_System.IsFrameLimitReached()) {
            m_EndEmulation.store(true, std::memory_order_release);
            break;
        }
    }
}

void CRecompiler::Execute_MemoryChange() {
    Log::info("CRecompiler: Executing with MemoryChange mode");
    
    const uint32_t countPerOp = m_System.CountPerOp();
    int32_t& nextTimer = m_System.m_NextTimer;
    auto* systemTimer = m_System.m_SystemTimer.get();
    auto* systemEvents = m_System.m_SystemEvents.get();
    
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
            if (!func) {
                Log::warningf("CRecompiler: Compilation failed at 0x%08X, interpreting instruction", pc);
                InterpretSingleInstruction();
                continue;
            }
        }
        
        ++m_Stats.blocksExecuted;
        if (!SafeExecuteBlock(func, pc)) { ProcessPendingDeletions();
            ClearRecompCode_Virt(pc, func->MaxPC() - func->MinPC() + 4,
                                 RemoveReason::Cache);
            InterpretSingleInstruction();
            continue;
        }
        
        ProcessPendingDeletions(); // Apply any pending jump (branch or synchronous exception)
        if (m_System.m_PipelineStage == N64Types::PipelineStage::Jump) {
            m_ProgramCounter = m_System.m_JumpToLocation;
            m_System.m_PipelineStage = N64Types::PipelineStage::Normal;
        }
        
        uint32_t instrCount = 1;
        if (auto* block = func->GetCodeBlock()) {
            instrCount = static_cast<uint32_t>(block->instruction_count());
            if (instrCount == 0) instrCount = 1;
        }
        // nextTimer decrement now done per-instruction inside compiled blocks
        
        // Idle loop detection
        {
            uint32_t newPC = static_cast<uint32_t>(m_ProgramCounter);
            if (newPC == pc && instrCount <= 2 && nextTimer > 0) {
                nextTimer = 0 - static_cast<int32_t>(m_System.CountPerOp());
            }
        }
        
        {
            uint32_t newPC = static_cast<uint32_t>(m_ProgramCounter);
            uint32_t blockEndPC = pc + instrCount * 4;
            bool checkTimer = (newPC < blockEndPC || m_System.m_TestTimer);
            if (checkTimer) {
                m_System.m_TestTimer = false;
                if (nextTimer < 0) {
                    systemTimer->TimerDone();
                }
                if (systemEvents && systemEvents->DoSomething()) {
                    systemEvents->ExecuteEvents();
                }
            }
        }
        
        if (m_System.m_PipelineStage == N64Types::PipelineStage::Jump) {
            m_ProgramCounter = m_System.m_JumpToLocation;
            m_System.m_PipelineStage = N64Types::PipelineStage::Normal;
        }

        if (m_System.IsFrameLimitReached()) {
            m_EndEmulation.store(true, std::memory_order_release);
            break;
        }
    }
}

void CRecompiler::Execute_Lookup() {
    Log::info("CRecompiler: Executing with Lookup mode");
    
    const uint32_t countPerOp = m_System.CountPerOp();
    int32_t& nextTimer = m_System.m_NextTimer;
    auto* systemTimer = m_System.m_SystemTimer.get();
    auto* systemEvents = m_System.m_SystemEvents.get();
    
    while (!m_EndEmulation.load(std::memory_order_acquire)) {
        uint32_t pc = static_cast<uint32_t>(m_ProgramCounter);
        
        auto* func = Lookup(pc);
        if (!func) {
            func = CompileCode(pc);
            if (!func) {
                Log::warningf("CRecompiler: Compilation failed at 0x%08X, interpreting instruction", pc);
                InterpretSingleInstruction();
                continue;
            }
        }
        
        ++m_Stats.blocksExecuted;
        if (!SafeExecuteBlock(func, pc)) { ProcessPendingDeletions();
            ClearRecompCode_Virt(pc, func->MaxPC() - func->MinPC() + 4,
                                 RemoveReason::Cache);
            InterpretSingleInstruction();
            continue;
        }
        
        ProcessPendingDeletions(); // Apply any pending jump (branch or synchronous exception)
        if (m_System.m_PipelineStage == N64Types::PipelineStage::Jump) {
            m_ProgramCounter = m_System.m_JumpToLocation;
            m_System.m_PipelineStage = N64Types::PipelineStage::Normal;
        }
        
        uint32_t instrCount = 1;
        if (auto* block = func->GetCodeBlock()) {
            instrCount = static_cast<uint32_t>(block->instruction_count());
            if (instrCount == 0) instrCount = 1;
        }
        // nextTimer decrement now done per-instruction inside compiled blocks
        
        // Idle loop detection
        {
            uint32_t newPC = static_cast<uint32_t>(m_ProgramCounter);
            if (newPC == pc && instrCount <= 2 && nextTimer > 0) {
                nextTimer = 0 - static_cast<int32_t>(m_System.CountPerOp());
            }
        }
        
        {
            uint32_t newPC = static_cast<uint32_t>(m_ProgramCounter);
            uint32_t blockEndPC = pc + instrCount * 4;
            bool checkTimer = (newPC < blockEndPC || m_System.m_TestTimer);
            if (checkTimer) {
                m_System.m_TestTimer = false;
                if (nextTimer < 0) {
                    systemTimer->TimerDone();
                }
                if (systemEvents && systemEvents->DoSomething()) {
                    systemEvents->ExecuteEvents();
                }
            }
        }
        
        if (m_System.m_PipelineStage == N64Types::PipelineStage::Jump) {
            m_ProgramCounter = m_System.m_JumpToLocation;
            m_System.m_PipelineStage = N64Types::PipelineStage::Normal;
        }

        if (m_System.IsFrameLimitReached()) {
            m_EndEmulation.store(true, std::memory_order_release);
            break;
        }
    }
}

void CRecompiler::Execute_Lookup_Validate() {
    Log::info("CRecompiler: Executing with Lookup_Validate mode");
    
    const uint32_t countPerOp = m_System.CountPerOp();
    int32_t& nextTimer = m_System.m_NextTimer;
    auto* systemTimer = m_System.m_SystemTimer.get();
    auto* systemEvents = m_System.m_SystemEvents.get();
    
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
            if (!func) {
                Log::warningf("CRecompiler: Compilation failed at 0x%08X, interpreting instruction", pc);
                InterpretSingleInstruction();
                continue;
            }
        }
        
        ++m_Stats.blocksExecuted;
        if (!SafeExecuteBlock(func, pc)) { ProcessPendingDeletions();
            ClearRecompCode_Virt(pc, func->MaxPC() - func->MinPC() + 4,
                                 RemoveReason::Cache);
            InterpretSingleInstruction();
            continue;
        }
        
        ProcessPendingDeletions(); // Apply any pending jump (branch or synchronous exception)
        if (m_System.m_PipelineStage == N64Types::PipelineStage::Jump) {
            m_ProgramCounter = m_System.m_JumpToLocation;
            m_System.m_PipelineStage = N64Types::PipelineStage::Normal;
        }
        
        uint32_t instrCount = 1;
        if (auto* block = func->GetCodeBlock()) {
            instrCount = static_cast<uint32_t>(block->instruction_count());
            if (instrCount == 0) instrCount = 1;
        }
        // nextTimer decrement now done per-instruction inside compiled blocks
        
        // Idle loop detection
        {
            uint32_t newPC = static_cast<uint32_t>(m_ProgramCounter);
            if (newPC == pc && instrCount <= 2 && nextTimer > 0) {
                nextTimer = 0 - static_cast<int32_t>(m_System.CountPerOp());
            }
        }
        
        {
            uint32_t newPC = static_cast<uint32_t>(m_ProgramCounter);
            uint32_t blockEndPC = pc + instrCount * 4;
            bool checkTimer = (newPC < blockEndPC || m_System.m_TestTimer);
            if (checkTimer) {
                m_System.m_TestTimer = false;
                if (nextTimer < 0) {
                    systemTimer->TimerDone();
                }
                if (systemEvents && systemEvents->DoSomething()) {
                    systemEvents->ExecuteEvents();
                }
            }
        }
        
        if (m_System.m_PipelineStage == N64Types::PipelineStage::Jump) {
            m_ProgramCounter = m_System.m_JumpToLocation;
            m_System.m_PipelineStage = N64Types::PipelineStage::Normal;
        }

        if (m_System.IsFrameLimitReached()) {
            m_EndEmulation.store(true, std::memory_order_release);
            break;
        }
    }
}

// =============================================================================
// Interpreter Fallback
// =============================================================================

void CRecompiler::FallbackToInterpreter() {
    Log::info("CRecompiler: Falling back to interpreter for remaining execution");
    m_System.ExecuteInterpret();
}

void CRecompiler::InterpretSingleInstruction() {
    // Execute exactly one instruction via the interpreter.
    // Routed through CN64System to avoid include conflicts.
    uint32_t pc = static_cast<uint32_t>(m_ProgramCounter);
    if (m_Stats.interpretedFallbacks < 20) {
        Log::warningf("CRecompiler: InterpretSingleInstruction fallback at PC=0x%08X (count=%llu)",
                      pc, m_Stats.interpretedFallbacks + 1);
    }
    m_System.ExecuteSingleOp();
    ++m_Stats.interpretedFallbacks;
}

// =============================================================================
// Helper Methods
// =============================================================================

void CRecompiler::ClearCodeRange(uint32_t start, uint32_t end, RemoveReason reason) {
    // Find all functions whose code overlaps [start, end) using block address ranges.
    std::vector<CCompiledFunc*> toRemove;
    
    for (auto* func : m_ActiveFunctions) {
        if (!func) continue;
        
        // A block overlaps the invalidation range if:
        //   block.MinPC < end  AND  block.MaxPC >= start
        uint32_t blockMin = func->MinPC();
        uint32_t blockMax = func->MaxPC();
        if (blockMin < end && blockMax >= start) {
            toRemove.push_back(func);
        }
    }
    
    if (!toRemove.empty()) {
        Log::debugf("CRecompiler: ClearCodeRange [0x%08X-0x%08X) removing %zu funcs",
                   start, end, toRemove.size());
    }
    
    // Remove functions
    for (auto* func : toRemove) {
        // Remove the function's entry from the lookup map
        Remove(func->EnterPC());
        
        // Remove from active list
        auto it = std::find(m_ActiveFunctions.begin(), m_ActiveFunctions.end(), func);
        if (it != m_ActiveFunctions.end()) {
            m_ActiveFunctions.erase(it);
        }
        
        // Erase from profiling map to avoid dangling pointers
        m_FunctionProfiles.erase(func);
        
        m_PendingDeletions.push_back(func);
    }
    
    Log::debugf("CRecompiler: Removed %zu functions", toRemove.size());
}

void CRecompiler::ProcessPendingDeletions() {
    if (m_PendingDeletions.empty()) return;
    for (auto* func : m_PendingDeletions) {
        delete func;
    }
    m_PendingDeletions.clear();
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
