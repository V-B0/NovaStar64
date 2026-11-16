/**
 * @file x64RecompilerOps.hpp
 * @brief x64 recompiler operations - MIPS to x64 translation
 * 
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 * 
 * This class implements the translation of MIPS R4300i instructions to x64
 * native code. It handles instruction decoding, register allocation,
 * code generation, and optimization for the x64 architecture.
 */

#pragma once

#if defined(__amd64__) || defined(_M_X64)

#include "../ExitInfo.hpp"
#include "../RecompilerOps.hpp"
#include "../RegInfo.hpp"
#include "x64ops.hpp"
#include "x64RegInfo.hpp"
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <functional>
#include <optional>

namespace Nova64 {
namespace Recompiler {

// Forward declarations
class CX64Ops;
class CJumpInfo;

/**
 * @enum TranslationResult
 * @brief Result of instruction translation
 */
enum class TranslationResult : uint8_t {
    Success,       ///< Translation successful
    Unsupported,   ///< Instruction not supported
    Invalid,       ///< Invalid instruction
    NeedsFallback, ///< Needs interpreter fallback
    BlockBoundary, ///< End of basic block
    Error          ///< Translation error
};

/**
 * @struct TranslationContext
 * @brief Context for instruction translation
 */
struct TranslationContext {
    const R4300iOps& opcode;
    uint32_t virtualAddress;
    uint32_t physicalAddress;
    PipelineStage pipelineStage;
    bool inDelaySlot;
    bool isBranchLikely;

    // For branches and jumps
    std::optional<uint32_t> branchTarget;
    std::optional<uint32_t> linkAddress;

    // Debug information
    std::string disassembly;
    bool emitDebugComments;
};

/**
 * @class CX64RecompilerOps
 * @brief x64 implementation of recompiler operations
 * 
 * This class orchestrates the translation of MIPS instructions to x64 code,
 * managing register allocation, code generation, and optimization.
 */
class CX64RecompilerOps final : public IRecompilerOps {
public:
    /**
     * @brief Construct the x64 recompiler operations
     * @param context Recompiler context
     * @param codeBlock Code block being compiled
     */
    explicit CX64RecompilerOps(RecompilerContext& context, CodeBlock& codeBlock);
    ~CX64RecompilerOps() override;

    // Non-copyable, movable
    CX64RecompilerOps(const CX64RecompilerOps&) = delete;
    CX64RecompilerOps& operator=(const CX64RecompilerOps&) = delete;
    CX64RecompilerOps(CX64RecompilerOps&&) = delete;
    CX64RecompilerOps& operator=(CX64RecompilerOps&&) = delete;

    // IRecompilerOps interface
    bool Initialize(RecompilerContext& context, CodeBlock& codeBlock) override;
    void Cleanup() override;

    bool CompileInstruction(const InstructionContext& ctx) override;
    bool CompileDelaySlot(const InstructionContext& ctx) override;
    bool CompileBranch(const InstructionContext& ctx, BranchType type, ConditionType condition) override;
    bool CompileJump(const InstructionContext& ctx, bool withLink) override;
    bool CompileTrap(const InstructionContext& ctx, ConditionType condition) override;
    bool CompileSystemCall(const InstructionContext& ctx) override;
    bool CompileBreak(const InstructionContext& ctx) override;

    void LoadRegister(uint32_t regIndex, bool is64Bit = false) override;
    void StoreRegister(uint32_t regIndex, bool is64Bit = false) override;
    void FlushRegisters() override;

    bool CompileLoad(const InstructionContext& ctx, uint32_t size, bool isSigned, bool is64Bit) override;
    bool CompileStore(const InstructionContext& ctx, uint32_t size, bool is64Bit) override;

    bool CompileFloatingPointOperation(const InstructionContext& ctx) override;
    bool CompileFloatingPointBranch(const InstructionContext& ctx, ConditionType condition) override;

    bool CompileCoprocessorOperation(const InstructionContext& ctx, uint32_t coprocessor) override;

    void GenerateDebugComment(std::string_view comment) override;
    void GenerateAddressCheck(uint32_t address) override;

    bool SupportsFeature(std::string_view feature) const override;
    size_t GetCodeSizeEstimate(const InstructionContext& ctx) const override;

    // Extended interface for x64-specific functionality
    /**
     * @brief Get the x64 assembler
     */
    CX64Ops& GetAssembler() noexcept { return *m_Assembler; }
    const CX64Ops& GetAssembler() const noexcept { return *m_Assembler; }

    /**
     * @brief Get the register working set
     */
    CX64RegInfo& GetRegisterSet() noexcept { return *m_RegisterSet; }
    const CX64RegInfo& GetRegisterSet() const noexcept { return *m_RegisterSet; }

    /**
     * @brief Set optimization level
     */
    enum class OptimizationLevel {
        None,       ///< No optimizations
        Basic,      ///< Basic optimizations
        Aggressive, ///< Aggressive optimizations
        Maximum     ///< Maximum optimizations (may be unsafe)
    };

    void SetOptimizationLevel(OptimizationLevel level) noexcept;
    OptimizationLevel GetOptimizationLevel() const noexcept { return m_OptimizationLevel; }

    // Control flow management
    void EnterCodeBlock();
    void ExitCodeBlock(ExitReason reason = ExitReason::NormalBlockEnd);
    void LinkJump(CJumpInfo& jumpInfo);
    void LinkJumpToSection(CodeSection* section);
    void LinkJumpToUnknown(CJumpInfo* jumpInfo);

    // Bind the skip-delay-slot label after a branch-likely instruction.
    // Called from generate_code after each instruction's compilation.
    void BindPendingBranchLikelyLabel();

    // Emit a check after each instruction: if PipelineStage == Jump
    // (exception occurred), skip remaining instructions and exit block.
    void EmitPipelineStageCheck();

    // Set up correct CPU state before executing a branch delay slot.
    // Saves the branch target (m_PROGRAM_COUNTER) into m_JumpToLocation,
    // sets m_PROGRAM_COUNTER to the delay slot address, and sets
    // m_PipelineStage to Jump.  This ensures TriggerException computes
    // the correct BranchDelay bit and EPC if the delay slot faults.
    void EmitDelaySlotSetup(uint32_t delaySlotAddr);

    // Mark whether the current instruction being compiled is in a delay slot.
    void SetInDelaySlot(bool inDelaySlot) { m_InDelaySlot = inDelaySlot; }

    // Emit per-instruction timer decrement: sub [&m_NextTimer], countPerOp.
    // This keeps COUNT accurate within blocks and handles early exits correctly.
    void EmitTimerDecrement();

    // Update m_PROGRAM_COUNTER to the current instruction address before
    // callbacks that may generate exceptions (SYSCALL, BREAK, MTC0, traps).
    // This ensures TriggerException saves the correct EPC.
    void EmitPCUpdate();

    // Register state management
    void SynchronizeRegisterState(const CX64RegInfo& other);
    void PropagateRegisterState();
    void SaveRegisterState();
    void RestoreRegisterState();

    // Statistics and debugging
    struct TranslationStats {
        size_t instructionsTranslated = 0;
        size_t branchesTranslated = 0;
        size_t loadsStoresTranslated = 0;
        size_t floatingPointOps = 0;
        size_t coprocessorOps = 0;
        size_t cyclesEstimate = 0;
        size_t codeSizeGenerated = 0;

        struct PerCategory {
            size_t arithmetic = 0;
            size_t logical = 0;
            size_t shift = 0;
            size_t memory = 0;
            size_t control = 0;
            size_t system = 0;
            size_t floating = 0;
            size_t coprocessor = 0;
        } categories;

        void Reset() noexcept {
            instructionsTranslated = 0;
            branchesTranslated = 0;
            loadsStoresTranslated = 0;
            floatingPointOps = 0;
            coprocessorOps = 0;
            cyclesEstimate = 0;
            codeSizeGenerated = 0;
            categories = {};
        }
    };

    const TranslationStats& GetStats() const noexcept { return m_Stats; }
    void ResetStats() noexcept { m_Stats.Reset(); }

    /**
     * @brief Generate disassembly of translated code
     */
    std::string GenerateDisassembly() const;

    /**
     * @brief Validate generated code
     */
    bool ValidateGeneratedCode() const;

private:
    // Instruction translation helpers
    using InstructionHandler = TranslationResult (CX64RecompilerOps::*)(const TranslationContext&);
    using InstructionHandlerMap = std::unordered_map<uint32_t, InstructionHandler>;

    void InitializeInstructionHandlers();
    TranslationResult TranslateInstruction(const TranslationContext& ctx);
    TranslationResult TranslateSpecialInstruction(const TranslationContext& ctx);
    TranslationResult TranslateRegImmInstruction(const TranslationContext& ctx);
    TranslationResult TranslateCop0Instruction(const TranslationContext& ctx);
    TranslationResult TranslateCop1Instruction(const TranslationContext& ctx);
    TranslationResult TranslateCop2Instruction(const TranslationContext& ctx);

    // Specific instruction category handlers
    TranslationResult HandleArithmetic(const TranslationContext& ctx);
    TranslationResult HandleLogical(const TranslationContext& ctx);
    TranslationResult HandleShift(const TranslationContext& ctx);
    TranslationResult HandleLoad(const TranslationContext& ctx);
    TranslationResult HandleStore(const TranslationContext& ctx);
    TranslationResult HandleBranch(const TranslationContext& ctx);
    TranslationResult HandleJump(const TranslationContext& ctx);
    TranslationResult HandleTrap(const TranslationContext& ctx);
    TranslationResult HandleSystem(const TranslationContext& ctx);
    TranslationResult HandleFloatingPoint(const TranslationContext& ctx);

    // Individual instruction handlers (organized by category)

    // SPECIAL opcodes
    TranslationResult HandleSPECIAL_SLL(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_SRL(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_SRA(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_SLLV(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_SRLV(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_SRAV(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_JR(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_JALR(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_SYSCALL(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_BREAK(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_MFLO(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_MTLO(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_MFHI(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_MTHI(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DSLLV(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DSRLV(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DSRAV(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_MULT(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_MULTU(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DIV(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DIVU(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DMULT(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DMULTU(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DDIV(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DDIVU(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_ADD(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_ADDU(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_SUB(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_SUBU(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_AND(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_OR(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_XOR(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_NOR(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_SLT(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_SLTU(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DADD(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DADDU(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DSUB(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DSUBU(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DSLL(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DSRL(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DSRA(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DSLL32(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DSRL32(const TranslationContext& ctx);
    TranslationResult HandleSPECIAL_DSRA32(const TranslationContext& ctx);

    // REGIMM opcodes
    TranslationResult HandleREGIMM_BLTZ(const TranslationContext& ctx);
    TranslationResult HandleREGIMM_BGEZ(const TranslationContext& ctx);
    TranslationResult HandleREGIMM_BLTZL(const TranslationContext& ctx);
    TranslationResult HandleREGIMM_BGEZL(const TranslationContext& ctx);
    TranslationResult HandleREGIMM_TGEI(const TranslationContext& ctx);
    TranslationResult HandleREGIMM_TGEIU(const TranslationContext& ctx);
    TranslationResult HandleREGIMM_TLTI(const TranslationContext& ctx);
    TranslationResult HandleREGIMM_TLTIU(const TranslationContext& ctx);
    TranslationResult HandleREGIMM_TEQI(const TranslationContext& ctx);
    TranslationResult HandleREGIMM_TNEI(const TranslationContext& ctx);
    TranslationResult HandleREGIMM_BLTZAL(const TranslationContext& ctx);
    TranslationResult HandleREGIMM_BGEZAL(const TranslationContext& ctx);
    TranslationResult HandleREGIMM_BLTZALL(const TranslationContext& ctx);
    TranslationResult HandleREGIMM_BGEZALL(const TranslationContext& ctx);

    // Load/Store opcodes
    TranslationResult HandleLB(const TranslationContext& ctx);
    TranslationResult HandleLH(const TranslationContext& ctx);
    TranslationResult HandleLWL(const TranslationContext& ctx);
    TranslationResult HandleLW(const TranslationContext& ctx);
    TranslationResult HandleLBU(const TranslationContext& ctx);
    TranslationResult HandleLHU(const TranslationContext& ctx);
    TranslationResult HandleLWR(const TranslationContext& ctx);
    TranslationResult HandleLWU(const TranslationContext& ctx);
    TranslationResult HandleSB(const TranslationContext& ctx);
    TranslationResult HandleSH(const TranslationContext& ctx);
    TranslationResult HandleSWL(const TranslationContext& ctx);
    TranslationResult HandleSW(const TranslationContext& ctx);
    TranslationResult HandleSWR(const TranslationContext& ctx);
    TranslationResult HandleSDL(const TranslationContext& ctx);
    TranslationResult HandleSDR(const TranslationContext& ctx);
    TranslationResult HandleLD(const TranslationContext& ctx);
    TranslationResult HandleSD(const TranslationContext& ctx);
    TranslationResult HandleLDL(const TranslationContext& ctx);
    TranslationResult HandleLDR(const TranslationContext& ctx);

    // Immediate opcodes
    TranslationResult HandleADDI(const TranslationContext& ctx);
    TranslationResult HandleADDIU(const TranslationContext& ctx);
    TranslationResult HandleSLTI(const TranslationContext& ctx);
    TranslationResult HandleSLTIU(const TranslationContext& ctx);
    TranslationResult HandleANDI(const TranslationContext& ctx);
    TranslationResult HandleORI(const TranslationContext& ctx);
    TranslationResult HandleXORI(const TranslationContext& ctx);
    TranslationResult HandleLUI(const TranslationContext& ctx);
    TranslationResult HandleDADDI(const TranslationContext& ctx);
    TranslationResult HandleDADDIU(const TranslationContext& ctx);

    // Branch opcodes
    TranslationResult HandleJ(const TranslationContext& ctx);
    TranslationResult HandleJAL(const TranslationContext& ctx);
    TranslationResult HandleBEQ(const TranslationContext& ctx);
    TranslationResult HandleBNE(const TranslationContext& ctx);
    TranslationResult HandleBLEZ(const TranslationContext& ctx);
    TranslationResult HandleBGTZ(const TranslationContext& ctx);
    TranslationResult HandleBEQL(const TranslationContext& ctx);
    TranslationResult HandleBNEL(const TranslationContext& ctx);
    TranslationResult HandleBLEZL(const TranslationContext& ctx);
    TranslationResult HandleBGTZL(const TranslationContext& ctx);

    // COP1 opcodes
    TranslationResult HandleLWC1(const TranslationContext& ctx);
    TranslationResult HandleLDC1(const TranslationContext& ctx);
    TranslationResult HandleSWC1(const TranslationContext& ctx);
    TranslationResult HandleSDC1(const TranslationContext& ctx);

    // Helper methods
    void EmitCop1UsableCheck();
    void EmitCop1BranchUsableCheck(uint32_t pc);
    void EmitRegisterCallback(void* callback, uint32_t opcVal);
    void EmitMemoryCallback(void* callback, uint32_t opcVal);
    void EmitTlbCallback(void* callback, uint32_t opcVal);
    void EmitBranchCallback(void* callback, uint32_t opcVal, uint32_t pc);
    void EmitBranchLikelyCallback(void* callback, uint32_t opcVal, uint32_t pc);
    void EmitLoadOperation(uint32_t size, bool isSigned, bool is64Bit, asmjit::x86::Gp dest,
                           const asmjit::x86::Mem& src);
    void EmitStoreOperation(uint32_t size, bool is64Bit, const asmjit::x86::Mem& dest, asmjit::x86::Gp src);
    void EmitBranchCondition(ConditionType condition, asmjit::Label target);
    void EmitTrapCondition(ConditionType condition);
    void EmitSystemCall();
    void EmitBreak();

    // Register allocation and management
    void AllocateRegister(uint32_t regIndex, bool is64Bit = false);
    void FreeRegister(uint32_t regIndex);
    void SpillRegister(uint32_t regIndex);
    void ReloadRegister(uint32_t regIndex);

    // Memory access helpers
    asmjit::x86::Mem CreateMemoryOperand(uint32_t baseReg, int32_t offset, uint32_t size, bool is64Bit);
    void EmitMemoryBoundsCheck(uint32_t address);

    // Optimization helpers
    void OptimizeCurrentBlock();
    void PeepholeOptimize();
    void ConstantPropagation();
    void DeadCodeElimination();
    void RedundantMoveElimination();

    // Branch comparison instructions
    void BNE_Compare();
    void BEQ_Compare();
    void BGTZ_Compare();
    void BLEZ_Compare();
    void BLTZ_Compare();
    void BGEZ_Compare();
    void BLTZAL_Compare();
    void BGEZAL_Compare();
    void COP1_BCF_Compare();
    void COP1_BCT_Compare();

    // Branch-likely comparison instructions (skip delay slot when not taken)
    void BEQL_Compare_Likely();
    void BNEL_Compare_Likely();
    void BLEZL_Compare_Likely();
    void BGTZL_Compare_Likely();
    void BLTZL_Compare_Likely();
    void BGEZL_Compare_Likely();
    void BLTZALL_Compare_Likely();
    void BGEZALL_Compare_Likely();
    void BC1FL_Compare_Likely();
    void BC1TL_Compare_Likely();

    // Jump instructions
    void J();
    void JAL();

    // Immediate arithmetic instructions
    void ADDI();
    void ADDIU();
    void SLTI();
    void SLTIU();
    void ANDI();
    void ORI();
    void XORI();
    void LUI();
    void DADDI();
    void DADDIU();

    // Load instructions
    void LB();
    void LH();
    void LWL();
    void LW();
    void LBU();
    void LHU();
    void LWR();
    void LWU();
    void LD();
    void LDL();
    void LDR();
    void LL();
    void LLD();
    void LWC1();
    void LDC1();

    // Store instructions
    void SB();
    void SH();
    void SWL();
    void SW();
    void SWR();
    void SD();
    void SDL();
    void SDR();
    void SC();
    void SCD();
    void SWC1();
    void SDC1();
    void CACHE();

    // SPECIAL format instructions
    void SPECIAL_SLL();
    void SPECIAL_SRL();
    void SPECIAL_SRA();
    void SPECIAL_SLLV();
    void SPECIAL_SRLV();
    void SPECIAL_SRAV();
    void SPECIAL_JR();
    void SPECIAL_JALR();
    void SPECIAL_SYSCALL();
    void SPECIAL_BREAK();
    void SPECIAL_SYNC();
    void SPECIAL_MFLO();
    void SPECIAL_MTLO();
    void SPECIAL_MFHI();
    void SPECIAL_MTHI();
    void SPECIAL_DSLLV();
    void SPECIAL_DSRLV();
    void SPECIAL_DSRAV();
    void SPECIAL_MULT();
    void SPECIAL_MULTU();
    void SPECIAL_DIV();
    void SPECIAL_DIVU();
    void SPECIAL_DMULT();
    void SPECIAL_DMULTU();
    void SPECIAL_DDIV();
    void SPECIAL_DDIVU();
    void SPECIAL_ADD();
    void SPECIAL_ADDU();
    void SPECIAL_SUB();
    void SPECIAL_SUBU();
    void SPECIAL_AND();
    void SPECIAL_OR();
    void SPECIAL_XOR();
    void SPECIAL_NOR();
    void SPECIAL_SLT();
    void SPECIAL_SLTU();
    void SPECIAL_DADD();
    void SPECIAL_DADDU();
    void SPECIAL_DSUB();
    void SPECIAL_DSUBU();
    void SPECIAL_DSLL();
    void SPECIAL_DSRL();
    void SPECIAL_DSRA();
    void SPECIAL_DSLL32();
    void SPECIAL_DSRL32();
    void SPECIAL_DSRA32();

    // COP0 instructions
    void COP0_MF();
    void COP0_DMF();
    void COP0_MT();
    void COP0_DMT();
    void COP0_CO_TLBR();
    void COP0_CO_TLBWI();
    void COP0_CO_TLBWR();
    void COP0_CO_TLBP();
    void COP0_CO_ERET();
    void COP0_CO_WAIT();

    // COP1 (FPU) instructions
    void COP1_MF();
    void COP1_DMF();
    void COP1_CF();
    void COP1_MT();
    void COP1_DMT();
    void COP1_CT();
    void COP1_S_ADD();
    void COP1_S_SUB();
    void COP1_S_MUL();
    void COP1_S_DIV();
    void COP1_S_ABS();
    void COP1_S_NEG();
    void COP1_S_SQRT();
    void COP1_S_MOV();
    void COP1_S_ROUND_L();
    void COP1_S_TRUNC_L();
    void COP1_S_CEIL_L();
    void COP1_S_FLOOR_L();
    void COP1_S_ROUND_W();
    void COP1_S_TRUNC_W();
    void COP1_S_CEIL_W();
    void COP1_S_FLOOR_W();
    void COP1_S_CVT_D();
    void COP1_S_CVT_W();
    void COP1_S_CVT_L();
    void COP1_S_CMP();
    void COP1_D_ADD();
    void COP1_D_SUB();
    void COP1_D_MUL();
    void COP1_D_DIV();
    void COP1_D_ABS();
    void COP1_D_NEG();
    void COP1_D_SQRT();
    void COP1_D_MOV();
    void COP1_D_ROUND_L();
    void COP1_D_TRUNC_L();
    void COP1_D_CEIL_L();
    void COP1_D_FLOOR_L();
    void COP1_D_ROUND_W();
    void COP1_D_TRUNC_W();
    void COP1_D_CEIL_W();
    void COP1_D_FLOOR_W();
    void COP1_D_CVT_S();
    void COP1_D_CVT_W();
    void COP1_D_CVT_L();
    void COP1_D_CMP();
    void COP1_W_CVT_S();
    void COP1_W_CVT_D();
    void COP1_L_CVT_S();
    void COP1_L_CVT_D();

    // Unknown/unimplemented instruction
    void UnknownOpcode();

    // Configuration and state
    RecompilerContext& m_Context;
    CodeBlock& m_CodeBlock;
    std::unique_ptr<CX64Ops> m_Assembler;
    std::unique_ptr<CX64RegInfo> m_RegisterSet;

    // Instruction dispatch
    InstructionHandlerMap m_InstructionHandlers;
    InstructionHandlerMap m_SpecialHandlers;
    InstructionHandlerMap m_RegImmHandlers;
    InstructionHandlerMap m_Cop1Handlers;

    // Current translation state
    std::optional<TranslationContext> m_CurrentContext;
    OptimizationLevel m_OptimizationLevel = OptimizationLevel::Basic;
    bool m_InDelaySlot = false;
    bool m_BlockEnded = false;

    // Branch-likely delay slot skip
    asmjit::Label m_BranchLikelySkipLabel;
    bool m_HasPendingBranchLikelySkip = false;

public:
    bool HasPendingBranchLikelySkip() const { return m_HasPendingBranchLikelySkip; }

private:
    // Block exit label for early exit on exception
    asmjit::Label m_BlockExitLabel;

    // Statistics
    TranslationStats m_Stats;

    // Cached labels and jump targets
    std::unordered_map<uint32_t, asmjit::Label> m_JumpTargets;
    std::vector<CJumpInfo*> m_PendingJumps;
};

// Factory function
std::unique_ptr<IRecompilerOps> CreateX64RecompilerOps(RecompilerContext& context, CodeBlock& codeBlock);

} // namespace Recompiler
} // namespace Nova64

#endif // __amd64__ || _M_X64
