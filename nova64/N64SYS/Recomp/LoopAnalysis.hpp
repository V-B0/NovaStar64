/**
 * @file LoopAnalysis.hpp
 * @brief Loop analysis for register optimization in dynamic recompilation
 * 
 * Nova64 N64 Emulator - Based on Project64 architecture
 * Original Project64 code licensed under GPL-2.0
 * 
 * This class performs data flow analysis on loops to determine which registers
 * can be kept in host registers across loop iterations, enabling significant
 * performance optimizations through register allocation.
 */

#pragma once

#include "RegInfo.hpp"
#include "CodeSection.hpp"
#include "CodeBlock.hpp"
#include "ExitInfo.hpp"
#include "../Types.hpp"
#include "../SysGlobals.hpp"
#include "../MIPSY/R4300Instruction.hpp"
#include "../MIPSY/R4300Iops.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../../common/MemoryManagment.hpp"
#include <cstdint>
#include <unordered_map>
#include <memory>
#include <vector>
#include <optional>
#include <functional>
#include <chrono>

namespace Nova64 {
namespace Recompiler {

// Forward declarations
class CCodeSection;
class CCodeBlock;

/**
 * @enum AnalysisPhase
 * @brief Phase of loop analysis
 */
enum class AnalysisPhase {
    InitialScan,
    DataFlow,
    Optimization,
    Finalize
};

/**
 * @struct AnalysisState
 * @brief Current state of loop analysis
 */
struct AnalysisState {
    uint32_t pc = 0;
    PipelineStage pipelineStage = PipelineStage::Execute;
    R4300iOps command{};
    uint32_t test = 0;
    bool changed = false;
    bool skippedSection = false;
    
    void Reset() {
        pc = 0;
        pipelineStage = PipelineStage::Execute;
        test = 0;
        changed = false;
        skippedSection = false;
    }
};

/**
 * @class LoopAnalysis
 * @brief Analyzes loops for register allocation optimization using data flow analysis
 * 
 * This class implements a flow-sensitive analysis that tracks register liveness
 * and values across loop iterations to determine which registers can be:
 * 1. Kept in host registers across loop boundaries
 * 2. Pre-loaded before loop entry
 * 3. Spilled/reloaded only when necessary
 * 
 * The analysis proceeds in multiple phases:
 * 1. Initial scan to identify loop structure
 * 2. Data flow analysis for register liveness
 * 3. Optimization decisions
 * 4. Finalization of register allocation plan
 */
class LoopAnalysis {
public:
    /**
     * @brief Construct loop analysis for a code block
     * @param codeBlock Reference to the containing code block
     * @param entrySection Entry point section of the loop (nullptr for whole-block analysis)
     */
    explicit LoopAnalysis(CCodeBlock& codeBlock, 
                         CCodeSection* entrySection = nullptr);
    
    ~LoopAnalysis() = default;
    
    // Non-copyable, movable
    LoopAnalysis(const LoopAnalysis&) = delete;
    LoopAnalysis& operator=(const LoopAnalysis&) = delete;
    LoopAnalysis(LoopAnalysis&&) = default;
    LoopAnalysis& operator=(LoopAnalysis&&) = default;
    
    /**
     * @brief Perform complete loop analysis and setup register allocation
     * @return true if analysis successful and optimizations can be applied
     */
    bool AnalyzeAndOptimize();
    
    /**
     * @brief Get the recommended register set for loop entry
     * @return Const reference to optimized register set for loop entry
     */
    const CRegInfo& GetEntryRegisterSet() const noexcept;
    
    /**
     * @brief Get the recommended register set for loop continuation
     * @return Const reference to optimized register set for loop continuation
     */
    const CRegInfo& GetContinueRegisterSet() const noexcept;
    
    /**
     * @brief Check if a specific register should be kept in host register
     * @param regIndex MIPS register index (0-31)
     * @return true if register should remain in host register across loop
     */
    bool ShouldKeepInRegister(uint32_t regIndex) const noexcept;
    
    /**
     * @brief Get analysis statistics for debugging/profiling
     */
    struct Statistics {
        size_t instructionsAnalyzed = 0;
        size_t registersOptimized = 0;
        size_t loopIterationsAnalyzed = 0;
        std::chrono::microseconds analysisTime;
        
        void Reset() {
            instructionsAnalyzed = 0;
            registersOptimized = 0;
            loopIterationsAnalyzed = 0;
            analysisTime = std::chrono::microseconds(0);
        }
    };
    
    const Statistics& GetStatistics() const noexcept { return m_Stats; }
    
    /**
     * @brief Reset analysis state for reuse
     */
    void Reset() noexcept;
    
private:
    // Phase handlers
    bool PerformInitialScan();
    bool PerformDataFlowAnalysis();
    bool PerformOptimization();
    bool FinalizeAnalysis();
    
    // Section analysis
    bool AnalyzeSection(CCodeSection* section);
    bool ProcessSectionEnter(CCodeSection* section);
    bool ProcessSectionExit(CCodeSection* section);
    
    // Register state management
    using RegisterLivenessMap = std::unordered_map<int32_t, bool>;
    using RegisterValueMap = std::unordered_map<int32_t, uint64_t>;
    
    struct RegisterInfo {
        bool isLive = false;
        bool isModified = false;
        bool isLoopInvariant = false;
        bool canBeHostRegister = false;
        uint64_t constantValue = 0;
        bool hasConstantValue = false;
    };
    
    void UpdateRegisterLiveness(CCodeSection* section);
    bool IsRegisterLoopInvariant(int32_t regIndex) const;
    void PropagateRegisterStates();
    
    // Instruction analysis
    using InstructionHandler = std::function<void()>;
    using InstructionHandlerMap = std::unordered_map<uint32_t, InstructionHandler>;
    
    void InitializeInstructionHandlers();
    void AnalyzeInstruction(const R4300iOps& opcode);
    void DefaultInstructionHandler();
    
    // MIPS instruction handlers grouped by category
    void HandleArithmeticInstruction();
    void HandleShiftInstruction();
    void HandleBranchInstruction(CCodeSection* section);
    void HandleJumpInstruction(CCodeSection* section);
    void HandleLoadStoreInstruction();
    void HandleSpecialInstruction(CCodeSection* section);
    void HandleCoprocessorInstruction();
    
    // Individual instruction handlers (only for SPECIAL opcodes that need specific handling)
    void HandleSPECIAL_SLL();
    void HandleSPECIAL_SRL();
    void HandleSPECIAL_SRA();
    void HandleSPECIAL_SLLV();
    void HandleSPECIAL_SRLV();
    void HandleSPECIAL_SRAV();
    void HandleSPECIAL_JR(CCodeSection* section);
    void HandleSPECIAL_JALR(CCodeSection* section);
    void HandleSPECIAL_SYSCALL(CCodeSection* section);
    void HandleSPECIAL_BREAK(CCodeSection* section);
    void HandleSPECIAL_MFHI();
    void HandleSPECIAL_MTHI();
    void HandleSPECIAL_MFLO();
    void HandleSPECIAL_MTLO();
    void HandleSPECIAL_DSLLV();
    void HandleSPECIAL_DSRLV();
    void HandleSPECIAL_DSRAV();
    void HandleSPECIAL_ADD();
    void HandleSPECIAL_ADDU();
    void HandleSPECIAL_SUB();
    void HandleSPECIAL_SUBU();
    void HandleSPECIAL_AND();
    void HandleSPECIAL_OR();
    void HandleSPECIAL_XOR();
    void HandleSPECIAL_NOR();
    void HandleSPECIAL_SLT();
    void HandleSPECIAL_SLTU();
    void HandleSPECIAL_DADD();
    void HandleSPECIAL_DADDU();
    void HandleSPECIAL_DSUB();
    void HandleSPECIAL_DSUBU();
    void HandleSPECIAL_DSLL();
    void HandleSPECIAL_DSRL();
    void HandleSPECIAL_DSRA();
    void HandleSPECIAL_DSLL32();
    void HandleSPECIAL_DSRL32();
    void HandleSPECIAL_DSRA32();
    
    // Register set synchronization
    bool SynchronizeRegisterState(CRegInfo& target, const CRegInfo& source);
    void MergeRegisterStates(CRegInfo& result, const CRegInfo& a, const CRegInfo& b);
    
    // Member variables
    CCodeBlock& m_CodeBlock;
    CCodeSection* m_EntrySection;
    
    // Analysis state
    AnalysisState m_State;
    AnalysisPhase m_CurrentPhase = AnalysisPhase::InitialScan;
    
    // Register information
    CRegInfo m_EntryRegSet;
    CRegInfo m_ContinueRegSet;
    CRegInfo m_CurrentRegSet;
    
    // Section mappings (use pointers since CRegInfo is move-only)
    std::unordered_map<CCodeSection*, CRegInfo*> m_SectionEntryStates;
    std::unordered_map<CCodeSection*, CRegInfo*> m_SectionExitStates;
    std::unordered_map<CCodeSection*, CRegInfo*> m_JumpTargetStates;
    
    // Register analysis
    std::array<RegisterInfo, 32> m_GPRInfo;  // GPRs 0-31
    RegisterInfo m_HIInfo;
    RegisterInfo m_LOInfo;
    
    // Instruction handler dispatch
    InstructionHandlerMap m_InstructionHandlers;
    
    // Statistics
    Statistics m_Stats;
    
    // Flags
    bool m_AnalysisComplete = false;
    bool m_HasLoop = false;
};

} // namespace Recompiler
} // namespace Nova64