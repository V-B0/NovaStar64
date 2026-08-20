/**
 * @file LoopAnalysis.hpp
 * @brief Loop analysis for register optimization in dynamic recompilation
 * 
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 * 
 * This class performs data flow analysis on loops to determine which registers
 * can be kept in host registers across loop iterations, enabling significant
 * performance optimizations through register allocation.
 */

#pragma once

#include "RegBase.hpp"
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
class CodeSection;
class CodeBlock;

/**
 * @class LoopAnalysis
 * @brief Analyzes loops for register allocation optimization using data flow analysis
 * 
 * Implements P64-style loop register usage analysis. Walks through all instructions
 * in sections that form a loop, tracking register state (constant propagation,
 * 32/64-bit width, modified flags) to determine optimal register allocation at
 * loop entry and continuation points.
 */
class LoopAnalysis {
public:
    /**
     * @brief Construct loop analysis for a code block
     * @param codeBlock Reference to the containing code block
     * @param entrySection Entry point section of the loop
     */
    explicit LoopAnalysis(CodeBlock& codeBlock, CodeSection* entrySection);
    
    ~LoopAnalysis();
    
    // Non-copyable, non-movable
    LoopAnalysis(const LoopAnalysis&) = delete;
    LoopAnalysis& operator=(const LoopAnalysis&) = delete;

    /**
     * @brief Check loop register usage and set up entry/continue register sets
     * @return true if analysis successful and optimizations can be applied
     */
    bool CheckLoopRegisterUsage();

    /**
     * @brief Get the recommended register set for loop entry  
     * @return Reference to optimized register set for loop entry
     */
    CRegBase& GetEnterRegisterSet() { return m_EnterRegisters; }
    const CRegBase& GetEnterRegisterSet() const { return m_EnterRegisters; }

    /**
     * @brief Get the recommended register set for loop continuation
     * @return Reference to optimized register set for loop continuation
     */
    CRegBase& GetContinueRegisterSet() { return m_ContinueRegisters; }
    const CRegBase& GetContinueRegisterSet() const { return m_ContinueRegisters; }

private:
    // Helper to extract MIPS instruction fields correctly
    // Note: Rtype bitfields are incorrectly named in R4300iOps.hpp
    // Use Itype.rs/rt/op (correct) and bit extraction for rd/sa/funct
    static uint32_t Cmd_op(const R4300iOps& c)    { return c.Itype.op; }
    static uint32_t Cmd_rs(const R4300iOps& c)    { return c.Itype.rs; }
    static uint32_t Cmd_rt(const R4300iOps& c)    { return c.Itype.rt; }
    static uint32_t Cmd_rd(const R4300iOps& c)    { return (c.Value >> 11) & 0x1F; }
    static uint32_t Cmd_sa(const R4300iOps& c)    { return (c.Value >> 6) & 0x1F; }
    static uint32_t Cmd_funct(const R4300iOps& c) { return c.Value & 0x3F; }
    static uint32_t Cmd_imm(const R4300iOps& c)   { return c.Itype.imm; }
    static int16_t  Cmd_imm_s(const R4300iOps& c) { return static_cast<int16_t>(c.Value & 0xFFFF); }

    // Core analysis: walk through a section checking register usage
    bool CheckSectionRegisterUsage(CodeSection* section);

    // Set up the "enter" register set from the analysis section mappings
    void SetupEnterSection(CodeSection* section, bool bRecursive);

    // Set up register states for the loop from section analysis
    void SetupRegisterForLoop(CodeSection* section);

    // Synchronize register state between analysis and stored section state
    bool SyncRegState(CRegBase& RegSet, const CRegBase& SyncReg);

    // Set the jump target register set for a section
    void SetJumpRegSet(CodeSection* section, const CRegBase& Reg);

    // Set the continue register set for a section
    void SetContinueRegSet(CodeSection* section, const CRegBase& Reg);

    // Get the jump register set for a section (nullptr if not set)
    CRegBase* GetJumpRegSet(CodeSection* section);
    const CRegBase* GetJumpRegSet(CodeSection* section) const;

    // Get the continue register set for a section (nullptr if not set)
    CRegBase* GetContinueRegSet(CodeSection* section);
    const CRegBase* GetContinueRegSet(CodeSection* section) const;

    // SPECIAL opcode instruction handlers for register analysis
    void SPECIAL_SLL(CRegBase& Reg);
    void SPECIAL_SRL(CRegBase& Reg);
    void SPECIAL_SRA(CRegBase& Reg);
    void SPECIAL_SLLV(CRegBase& Reg);
    void SPECIAL_SRLV(CRegBase& Reg);
    void SPECIAL_SRAV(CRegBase& Reg);
    void SPECIAL_DSLLV(CRegBase& Reg);
    void SPECIAL_DSRLV(CRegBase& Reg);
    void SPECIAL_DSRAV(CRegBase& Reg);
    void SPECIAL_ADD(CRegBase& Reg);
    void SPECIAL_ADDU(CRegBase& Reg);
    void SPECIAL_SUB(CRegBase& Reg);
    void SPECIAL_SUBU(CRegBase& Reg);
    void SPECIAL_AND(CRegBase& Reg);
    void SPECIAL_OR(CRegBase& Reg);
    void SPECIAL_XOR(CRegBase& Reg);
    void SPECIAL_NOR(CRegBase& Reg);
    void SPECIAL_SLT(CRegBase& Reg);
    void SPECIAL_SLTU(CRegBase& Reg);
    void SPECIAL_DADD(CRegBase& Reg);
    void SPECIAL_DADDU(CRegBase& Reg);
    void SPECIAL_DSUB(CRegBase& Reg);
    void SPECIAL_DSUBU(CRegBase& Reg);
    void SPECIAL_DSLL(CRegBase& Reg);
    void SPECIAL_DSRL(CRegBase& Reg);
    void SPECIAL_DSRA(CRegBase& Reg);
    void SPECIAL_DSLL32(CRegBase& Reg);
    void SPECIAL_DSRL32(CRegBase& Reg);
    void SPECIAL_DSRA32(CRegBase& Reg);

    CodeBlock& m_CodeBlock;
    CodeSection* m_EntrySection;

    // The command being analyzed
    R4300iOps m_Command{};

    // Pipeline tracking during analysis
    // Uses N64Types::PipelineStage for recompiler-level pipeline tracking
    // (Normal, DelaySlot, EndBlock etc.) - NOT the ExitInfo PipelineStage
    N64Types::PipelineStage m_PipelineStage = N64Types::PipelineStage::Normal;

    // PC during analysis walk
    uint32_t m_PC = 0;
    uint32_t m_NextInstruction = 0;
    uint32_t m_Test = 0;

    // Result register sets
    CRegBase m_EnterRegisters;
    CRegBase m_ContinueRegisters;

    // Per-section register state maps (section -> register set)
    std::unordered_map<CodeSection*, CRegBase*> m_JumpRegSets;
    std::unordered_map<CodeSection*, CRegBase*> m_ContinueRegSets;
    std::unordered_map<CodeSection*, CRegBase*> m_EnterRegSets;

    // Flags
    bool m_Changed = false;
};

} // namespace Recompiler
} // namespace Nova64
