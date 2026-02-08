/**
 * @file RecompilerOps.hpp
 * @brief Base interface for recompiler operations across platforms
 * 
 * Nova64 N64 Emulator - Based on Project64 architecture
 * Original Project64 code licensed under GPL-2.0
 * 
 * This file defines the interface and common types for recompiling
 * MIPS R4300i instructions to native code. Platform-specific
 * implementations handle the actual code generation.
 */

#pragma once

#include "ExitInfo.hpp"
#include "../MIPSY/R4300Instruction.hpp"
#include "../MIPSY/R4300Iops.hpp"
#include <cstdint>
#include <memory>
#include <functional>
#include <string_view>
#include <atomic>

namespace Nova64 {

// Forward declarations for system components
class CN64System;
class CMipsMemoryVM;
class CRegisters;
class CTLB;

namespace Recompiler {

// Forward declarations
class CCodeBlock;
class CCodeSection;
class ICodeGenerator;
class IRegisterAllocator;

/**
 * @enum BranchType
 * @brief Classification of branch instruction types
 */
enum class BranchType {
    Direct,         ///< Direct branch with immediate offset
    Register,       ///< Branch using register value (JR, JALR)
    Conditional,    ///< Conditional branch (BEQ, BNE, etc.)
    Coprocessor1,   ///< COP1 (floating-point) conditional branch
    Link,           ///< Branch with link (JAL, JALR)
    Likely,         ///< Branch likely (delay slot only on taken path)
    Unknown
};

/**
 * @enum ConditionType
 * @brief Condition tests for branches and traps
 */
enum class ConditionType : uint8_t {
    // Integer comparisons
    Equal,                  ///< Equal (==)
    NotEqual,               ///< Not equal (!=)
    LessThanZero,           ///< Less than zero (< 0)
    LessOrEqualZero,        ///< Less or equal zero (<= 0)
    GreaterThanZero,        ///< Greater than zero (> 0)
    GreaterOrEqualZero,     ///< Greater or equal zero (>= 0)
    LessThan,               ///< Less than (<)
    LessOrEqual,            ///< Less or equal (<=)
    GreaterThan,            ///< Greater than (>)
    GreaterOrEqual,         ///< Greater or equal (>=)
    
    // Unsigned comparisons
    LessThanUnsigned,       ///< Less than, unsigned
    LessOrEqualUnsigned,    ///< Less or equal, unsigned
    GreaterThanUnsigned,    ///< Greater than, unsigned
    GreaterOrEqualUnsigned, ///< Greater or equal, unsigned
    
    // Floating-point comparisons
    FloatingFalse,          ///< COP1: false
    FloatingTrue,           ///< COP1: true
    FloatingEqual,          ///< COP1: equal
    FloatingNotEqual,       ///< COP1: not equal
    FloatingLessThan,       ///< COP1: less than
    FloatingLessOrEqual,    ///< COP1: less or equal
    FloatingGreaterThan,    ///< COP1: greater than
    FloatingGreaterOrEqual, ///< COP1: greater or equal
    
    // Special
    Always,                 ///< Always true (unconditional)
    Never                   ///< Always false
};

/**
 * @enum InstructionCategory
 * @brief High-level categorization of MIPS instructions
 */
enum class InstructionCategory : uint8_t {
    Arithmetic,
    Logical,
    Shift,
    Load,
    Store,
    Branch,
    Jump,
    Trap,
    System,
    Coprocessor0,
    Coprocessor1,
    Coprocessor2,
    Unknown
};

/**
 * @struct RecompilerContext
 * @brief Context containing references to system components needed for recompilation
 */
struct RecompilerContext {
    CN64System& system;
    CMipsMemoryVM& mmu;
    CRegisters& registers;
    CTLB& tlb;
    std::atomic<bool>& endEmulation;
    
    RecompilerContext(CN64System& sys, CMipsMemoryVM& mem, 
                      CRegisters& regs, CTLB& t, std::atomic<bool>& end)
        : system(sys), mmu(mem), registers(regs), tlb(t), endEmulation(end) {}
};

/**
 * @struct InstructionContext
 * @brief Context for instruction recompilation
 */
struct InstructionContext {
    const R4300iInstruction& instruction;
    R4300iOps opcode;
    uint32_t virtualAddress;
    uint32_t physicalAddress;
    PipelineStage pipelineStage;
    CCodeSection* currentSection;
    
    InstructionContext(const R4300iInstruction& inst, uint32_t vaddr, 
                      uint32_t paddr, PipelineStage stage, CCodeSection* section)
        : instruction(inst)
        , opcode{inst.GetInstruction()}
        , virtualAddress(vaddr)
        , physicalAddress(paddr)
        , pipelineStage(stage)
        , currentSection(section)
    {}
};

/**
 * @class IRecompilerOps
 * @brief Interface for platform-specific recompiler operations
 * 
 * Defines the contract for all platform-specific implementations
 * to recompile MIPS instructions to native code.
 */
class IRecompilerOps {
public:
    virtual ~IRecompilerOps() = default;
    
    // Setup and teardown
    virtual bool Initialize(RecompilerContext& context, CCodeBlock& codeBlock) = 0;
    virtual void Cleanup() = 0;
    
    // Code generation
    virtual bool CompileInstruction(const InstructionContext& ctx) = 0;
    virtual bool CompileDelaySlot(const InstructionContext& ctx) = 0;
    virtual bool CompileBranch(const InstructionContext& ctx, 
                              BranchType type, ConditionType condition) = 0;
    virtual bool CompileJump(const InstructionContext& ctx, bool withLink) = 0;
    virtual bool CompileTrap(const InstructionContext& ctx, ConditionType condition) = 0;
    virtual bool CompileSystemCall(const InstructionContext& ctx) = 0;
    virtual bool CompileBreak(const InstructionContext& ctx) = 0;
    
    // Register management
    virtual void LoadRegister(uint32_t regIndex, bool is64Bit = false) = 0;
    virtual void StoreRegister(uint32_t regIndex, bool is64Bit = false) = 0;
    virtual void FlushRegisters() = 0;
    
    // Memory operations
    virtual bool CompileLoad(const InstructionContext& ctx, 
                            uint32_t size, bool isSigned, bool is64Bit) = 0;
    virtual bool CompileStore(const InstructionContext& ctx, 
                             uint32_t size, bool is64Bit) = 0;
    
    // Floating-point operations
    virtual bool CompileFloatingPointOperation(const InstructionContext& ctx) = 0;
    virtual bool CompileFloatingPointBranch(const InstructionContext& ctx, 
                                           ConditionType condition) = 0;
    
    // Coprocessor operations
    virtual bool CompileCoprocessorOperation(const InstructionContext& ctx, 
                                            uint32_t coprocessor) = 0;
    
    // Debug and profiling
    virtual void GenerateDebugComment(std::string_view comment) = 0;
    virtual void GenerateAddressCheck(uint32_t address) = 0;
    
    // Query capabilities
    virtual bool SupportsFeature(std::string_view feature) const = 0;
    virtual size_t GetCodeSizeEstimate(const InstructionContext& ctx) const = 0;
};

/**
 * @class RecompilerOpsBase
 * @brief Common base implementation with shared functionality
 * 
 * Provides common functionality and helper methods that are
 * shared across all platform implementations.
 */
class RecompilerOpsBase : public IRecompilerOps {
public:
    explicit RecompilerOpsBase(RecompilerContext& context);
    virtual ~RecompilerOpsBase() override;
    
    // Non-copyable, movable
    RecompilerOpsBase(const RecompilerOpsBase&) = delete;
    RecompilerOpsBase& operator=(const RecompilerOpsBase&) = delete;
    RecompilerOpsBase(RecompilerOpsBase&&) = default;
    RecompilerOpsBase& operator=(RecompilerOpsBase&&) = default;
    
    // IRecompilerOps interface
    bool Initialize(RecompilerContext& context, CCodeBlock& codeBlock) override final;
    void Cleanup() override final;
    
protected:
    // Helper methods for derived classes
    InstructionCategory ClassifyInstruction(const R4300iOps& opcode) const noexcept;
    BranchType DetermineBranchType(const R4300iOps& opcode) const noexcept;
    ConditionType DetermineCondition(const R4300iOps& opcode) const noexcept;
    
    bool IsBranchLikely(const R4300iOps& opcode) const noexcept;
    bool IsBranchDelaySlot(const InstructionContext& ctx) const noexcept;
    bool ShouldEmitDebugComments() const noexcept;
    
    // Access to system components
    CN64System& GetSystem() noexcept { return m_Context.system; }
    const CN64System& GetSystem() const noexcept { return m_Context.system; }
    
    CMipsMemoryVM& GetMMU() noexcept { return m_Context.mmu; }
    const CMipsMemoryVM& GetMMU() const noexcept { return m_Context.mmu; }
    
    CRegisters& GetRegisters() noexcept { return m_Context.registers; }
    const CRegisters& GetRegisters() const noexcept { return m_Context.registers; }
    
    CTLB& GetTLB() noexcept { return m_Context.tlb; }
    const CTLB& GetTLB() const noexcept { return m_Context.tlb; }
    
    std::atomic<bool>& GetEndEmulationFlag() noexcept { return m_Context.endEmulation; }
    
    // Current compilation state
    CCodeBlock* GetCurrentBlock() noexcept { return m_CurrentBlock; }
    const CCodeBlock* GetCurrentBlock() const noexcept { return m_CurrentBlock; }
    
    void SetCurrentSection(CCodeSection* section) noexcept { m_CurrentSection = section; }
    CCodeSection* GetCurrentSection() noexcept { return m_CurrentSection; }
    const CCodeSection* GetCurrentSection() const noexcept { return m_CurrentSection; }
    
    // Statistics
    struct CompilationStats {
        size_t instructionsCompiled = 0;
        size_t branchesCompiled = 0;
        size_t loadsStoresCompiled = 0;
        size_t floatingPointOps = 0;
        
        void Reset() {
            instructionsCompiled = 0;
            branchesCompiled = 0;
            loadsStoresCompiled = 0;
            floatingPointOps = 0;
        }
    };
    
    const CompilationStats& GetStats() const noexcept { return m_Stats; }
    
private:
    // Platform-specific implementations must override these
    virtual bool InitializeImpl() = 0;
    virtual void CleanupImpl() = 0;
    
    virtual bool CompileInstructionImpl(const InstructionContext& ctx) = 0;
    virtual bool CompileDelaySlotImpl(const InstructionContext& ctx) = 0;
    
    // Common state
    RecompilerContext m_Context;
    CCodeBlock* m_CurrentBlock = nullptr;
    CCodeSection* m_CurrentSection = nullptr;
    
    // Statistics
    CompilationStats m_Stats;
    
    // Configuration
    bool m_DebugCommentsEnabled = false;
    bool m_OptimizationsEnabled = true;
};

/**
 * @brief Create a platform-specific recompiler operations instance
 * @param context Recompiler context
 * @return Unique pointer to platform-specific implementation
 */
std::unique_ptr<IRecompilerOps> CreatePlatformRecompilerOps(RecompilerContext& context);

} // namespace Recompiler
} // namespace Nova64

// Platform-specific implementations are included via factory function
// No conditional includes in this header - cleaner separation