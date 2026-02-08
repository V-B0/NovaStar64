/**
 * @file x64RecompilerOps.hpp
 * @brief x64 recompiler operations - MIPS to x64 translation
 * 
 * Nova64 N64 Emulator - Based on Project64 architecture
 * Original Project64 code licensed under GPL-2.0
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
struct CJumpInfo;
class IInstructionTranslator;
class IRegisterAllocator;

/**
 * @enum TranslationResult
 * @brief Result of instruction translation
 */
enum class TranslationResult : uint8_t {
    Success,            ///< Translation successful
    Unsupported,        ///< Instruction not supported
    Invalid,            ///< Invalid instruction
    NeedsFallback,      ///< Needs interpreter fallback
    BlockBoundary,      ///< End of basic block
    Error               ///< Translation error
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
    explicit CX64RecompilerOps(RecompilerContext& context, 
                               CCodeBlock& codeBlock);
    ~CX64RecompilerOps() override;
    
    // Non-copyable, movable
    CX64RecompilerOps(const CX64RecompilerOps&) = delete;
    CX64RecompilerOps& operator=(const CX64RecompilerOps&) = delete;
    CX64RecompilerOps(CX64RecompilerOps&&) = delete;
    CX64RecompilerOps& operator=(CX64RecompilerOps&&) = delete;
    
    // IRecompilerOps interface
    bool Initialize(RecompilerContext& context, CCodeBlock& codeBlock) override;
    void Cleanup() override;
    
    bool CompileInstruction(const InstructionContext& ctx) override;
    bool CompileDelaySlot(const InstructionContext& ctx) override;
    bool CompileBranch(const InstructionContext& ctx, 
                      BranchType type, ConditionType condition) override;
    bool CompileJump(const InstructionContext& ctx, bool withLink) override;
    bool CompileTrap(const InstructionContext& ctx, ConditionType condition) override;
    bool CompileSystemCall(const InstructionContext& ctx) override;
    bool CompileBreak(const InstructionContext& ctx) override;
    
    void LoadRegister(uint32_t regIndex, bool is64Bit = false) override;
    void StoreRegister(uint32_t regIndex, bool is64Bit = false) override;
    void FlushRegisters() override;
    
    bool CompileLoad(const InstructionContext& ctx, 
                    uint32_t size, bool isSigned, bool is64Bit) override;
    bool CompileStore(const InstructionContext& ctx, 
                     uint32_t size, bool is64Bit) override;
    
    bool CompileFloatingPointOperation(const InstructionContext& ctx) override;
    bool CompileFloatingPointBranch(const InstructionContext& ctx, 
                                   ConditionType condition) override;
    
    bool CompileCoprocessorOperation(const InstructionContext& ctx, 
                                    uint32_t coprocessor) override;
    
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
    void LinkJumpToSection(CCodeSection* section);
    void LinkJumpToUnknown(CJumpInfo* jumpInfo);
    
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
    void EmitLoadOperation(uint32_t size, bool isSigned, bool is64Bit, 
                          asmjit::x86::Gp dest, const asmjit::x86::Mem& src);
    void EmitStoreOperation(uint32_t size, bool is64Bit,
                           const asmjit::x86::Mem& dest, asmjit::x86::Gp src);
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
    asmjit::x86::Mem CreateMemoryOperand(uint32_t baseReg, int32_t offset, 
                                        uint32_t size, bool is64Bit);
    void EmitMemoryBoundsCheck(uint32_t address);
    
    // Optimization helpers
    void OptimizeCurrentBlock();
    void PeepholeOptimize();
    void ConstantPropagation();
    void DeadCodeElimination();
    void RedundantMoveElimination();
    
    // Configuration and state
    RecompilerContext& m_Context;
    CCodeBlock& m_CodeBlock;
    std::unique_ptr<CX64Ops> m_Assembler;
    std::unique_ptr<CX64RegInfo> m_RegisterSet;
    std::unique_ptr<IInstructionTranslator> m_Translator;
    std::unique_ptr<IRegisterAllocator> m_RegisterAllocator;
    
    // Instruction dispatch
    InstructionHandlerMap m_InstructionHandlers;
    InstructionHandlerMap m_SpecialHandlers;
    InstructionHandlerMap m_RegImmHandlers;
    InstructionHandlerMap m_Cop1Handlers;
    
    // Current translation state
    TranslationContext m_CurrentContext;
    OptimizationLevel m_OptimizationLevel = OptimizationLevel::Basic;
    bool m_InDelaySlot = false;
    bool m_BlockEnded = false;
    
    // Statistics
    TranslationStats m_Stats;
    
    // Cached labels and jump targets
    std::unordered_map<uint32_t, asmjit::Label> m_JumpTargets;
    std::vector<CJumpInfo*> m_PendingJumps;
};

// Factory function
std::unique_ptr<IRecompilerOps> CreateX64RecompilerOps(
    RecompilerContext& context, 
    CCodeBlock& codeBlock);

} // namespace Recompiler
} // namespace Nova64

#endif // __amd64__ || _M_X64