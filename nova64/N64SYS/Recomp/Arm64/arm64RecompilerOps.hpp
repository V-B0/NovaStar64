/**
 * @file arm64RecompilerOps.hpp
 * @brief AArch64 recompiler operations - MIPS R4300i to ARM64 translation
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 *
 * Declared as Nova64::Recompiler::Arm::ArmRecompilerOps to satisfy the forward
 * and friend declarations the engine already ships (MIPSY/Vmem.hpp,
 * SysGlobals.hpp) and the integration path in CodeBlock.cpp / Recompiler.cpp.
 *
 * Execution model
 * ---------------
 * This backend mirrors the x64 "callback-threaded" dynarec: a block prologue
 * pins the emulator-state pointers into AArch64 callee-saved registers, then
 * each MIPS instruction is translated into a stub that loads arguments into the
 * AAPCS64 argument registers and calls a C++ interpreter routine.  Block exit
 * restores the callee-saved registers and returns.
 *
 * Pinned register convention (set up in EnterCodeBlock):
 *      x19 = &CRegisters          (mirrors x64 RBX)
 *      x20 = &MipsMemoryVM        (mirrors x64 R12)
 *      x21 = &m_PipelineStage     (mirrors x64 R13)
 *      x22 = &m_NextTimer         (mirrors x64 R14)
 *      x23 = &MemoryReadMap[0]    (mirrors x64 R15)
 *
 * Status: the block scaffolding (prologue/epilogue, pinned pointers, native
 * call/branch/timer/pipeline primitives) is implemented and exercised by the
 * AArch64 build.  Per-opcode translation is being filled in incrementally; the
 * always-correct interpreter remains the default execution mode on AArch64
 * (see CN64System::s_UseInterpreter), so partial coverage is safe.
 */

#pragma once

#if defined(__aarch64__) || defined(_M_ARM64)

#include "../RecompilerOps.hpp" // RecompilerContext, InstructionContext
#include "arm64ops.hpp"
#include "arm64RegInfo.hpp"
#include <memory>
#include <cstdint>

namespace Nova64 {
namespace Recompiler {

class CodeBlock;

namespace Arm {

class ArmRecompilerOps {
public:
    ArmRecompilerOps(RecompilerContext& context, CodeBlock& codeBlock);
    ~ArmRecompilerOps();

    ArmRecompilerOps(const ArmRecompilerOps&) = delete;
    ArmRecompilerOps& operator=(const ArmRecompilerOps&) = delete;

    // --- Block lifetime (called by CodeBlock::generate_code) ----------------
    void EnterCodeBlock();
    void ExitCodeBlock();

    // --- Per-instruction translation ----------------------------------------
    bool CompileInstruction(const InstructionContext& ctx);

    // --- Control-flow / housekeeping primitives -----------------------------
    void EmitTimerDecrement();

    /// Decrements the timer and, if it has expired, leaves the block with the
    /// program counter pointing at `nextPC`.
    ///
    /// This exists so the recompiler takes a pending interrupt at the same
    /// instruction boundary the interpreter would. Without it the timer is only
    /// noticed at a block exit, so the two engines enter the ISR tens of cycles
    /// apart and the game then computes a different CP0 Compare.
    ///
    /// Only valid for straight-line instructions: the caller must not use it for
    /// a branch (the delay slot still has to run) or for a delay slot (the
    /// branch callback has already set the PC, and `nextPC` would clobber it).
    void EmitTimerDecrementAndCheck(uint32_t nextPC);

    void EmitPipelineStageCheck();
    void EmitPCUpdate();
    // Unconditionally write m_PROGRAM_COUNTER = sign_extend32(addr).  Used at
    // the end of a fall-through (non-branch) block to point the dispatcher at
    // the next block (addr = block_end + 4).
    void EmitSetPC(uint32_t addr);
    void BindPendingBranchLikelyLabel();
    bool HasPendingBranchLikelySkip() const { return m_HasPendingBranchLikelySkip; }

    // --- Native callback emitters (AAPCS64) ---------------------------------
    // void(CRegisters*, uint32_t opcVal)
    void EmitRegisterCallback(void* callback, uint32_t opcVal);
    // void(CRegisters*, MipsMemoryVM*, uint32_t opcVal)
    void EmitMemoryCallback(void* callback, uint32_t opcVal);
    // void(CRegisters*, CTLB*, uint32_t opcVal)
    void EmitTlbCallback(void* callback, uint32_t opcVal);
    // void(CRegisters*, CN64System*, uint32_t opcVal)
    void EmitSysCallback(void* callback, uint32_t opcVal);
    // void(CRegisters*, uint32_t opcVal, uint32_t pc)
    void EmitBranchCallback(void* callback, uint32_t opcVal, uint32_t pc);
    // int(CRegisters*, uint32_t opcVal, uint32_t pc) -> 1 taken / 0 not taken / -1 exception
    void EmitBranchLikelyCallback(void* callback, uint32_t opcVal, uint32_t pc);

    CArm64Ops& GetAssembler() noexcept { return *m_Assembler; }

private:
    RecompilerContext& m_Context;
    CodeBlock& m_CodeBlock;
    std::unique_ptr<CArm64Ops> m_Assembler;
    std::unique_ptr<ArmRegInfo> m_RegisterSet;

    asmjit::Label m_BlockExitLabel;
    asmjit::Label m_BranchLikelySkipLabel;
    bool m_HasPendingBranchLikelySkip = false;
    bool m_BlockEnded = false;

    // Address of the instruction currently being compiled (for EmitPCUpdate).
    uint32_t m_CurrentPC = 0;
    bool m_HasCurrentPC = false;
};

} // namespace Arm
} // namespace Recompiler
} // namespace Nova64

#endif // __aarch64__ || _M_ARM64
