/**
 * @file arm64RecompilerOps.cpp
 * @brief AArch64 recompiler operations - implementation
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 *
 * Callback-threaded AArch64 backend.  The block prologue pins the emulator
 * state pointers into callee-saved registers (x19..x23); each MIPS instruction
 * is translated into a stub that loads AAPCS64 arguments and calls a shared
 * VR4300 interpreter callback (RecompInterpCallbacks.inc, the same source of
 * truth used by the x64 backend).
 */

#include "arm64RecompilerOps.hpp"

#if defined(__aarch64__) || defined(_M_ARM64)

#include "../CodeBlock.hpp"
#include "../CodeSection.hpp"
#include "../ExitInfo.hpp"
#include "../JumpInfo.hpp"
#include "../RecompilerOps.hpp"
#include "../RegInfo.hpp"
#include "../RegBase.hpp"
#include "../FunctionInfo.hpp"
#include "../FunctionMap.hpp"
#include "../LoopAnalysis.hpp"
#include "../../MainSYS.hpp"
#include "../../SysGlobals.hpp"
#include "../../Types.hpp"
#include "../../MIPSY/Register.hpp"
#include "../../MIPSY/Vmem.hpp"
#include "../../MIPSY/TLB.hpp"
#include "../../MIPSY/R4300Instruction.hpp"
#include "../../MIPSY/R4300Iops.hpp"
#include "../../../common/log.hpp"
#include "../../../common/trace.hpp"
#include "../../../common/MemoryManagment.hpp"
#include "../../../common/platform.hpp"
#include "../FpControl.hpp"

#include <cfenv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <atomic>

#pragma STDC FENV_ACCESS ON

// Where the program counter lives in CRegisters. CRegisters is not a standard-layout type, so
// offsetof is technically conditionally-supported; every compiler we build with computes it,
// and the generated code relies on such offsets for all the registers it touches.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
static const int32_t kProgramCounterOffset = static_cast<int32_t>(offsetof(CRegisters, m_PROGRAM_COUNTER));
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace a64 = asmjit::a64;

namespace Nova64 {
namespace Recompiler {

// =============================================================================
// Shared VR4300 interpreter callbacks (same source of truth as the x64 backend)
// =============================================================================
// Brings in: the `using MIPS_DWORD/ExceptionCode` aliases, CheckCop1Unusable,
// and the anonymous-namespace Interp_* callbacks + helpers.  They land in
// Nova64::Recompiler scope, so ArmRecompilerOps (nested below) reaches them by
// ordinary unqualified lookup, exactly as CX64RecompilerOps does.
#include "../RecompInterpCallbacks.inc"

// =============================================================================
// COP2 / COP3 / reserved helpers
// =============================================================================
// The x64 backend keeps these next to its code emitter (they are not part of
// the shared callback block), so the AArch64 backend provides its own copies.
namespace {

void Arm_Interp_Cop2Dispatch(CRegisters* regs, Nova64::MipsMemoryVM* /*mmu*/,
                             uint32_t opcVal, uint32_t compileAddr) {
    const uint32_t rs = mips_rs(opcVal);
    regs->m_PROGRAM_COUNTER = static_cast<int64_t>(static_cast<int32_t>(compileAddr));

    if (regs->STATUS_REGISTER.CU2 == 0) {
        regs->TriggerException(11, 2); // Coprocessor Unusable (CU2)
        return;
    }
    if (rs == 3 || rs == 7) { // DCFC2 / DCTC2 -> Reserved Instruction
        regs->TriggerException(10, 2);
        return;
    }
    // No exception: advance PC past this instruction (block exits immediately).
    regs->m_PROGRAM_COUNTER = static_cast<int64_t>(static_cast<int32_t>(compileAddr)) + 4;

    const uint32_t rt = mips_rt(opcVal);
    const uint32_t rd = mips_rd(opcVal);
    switch (rs) {
        case 0: // MFC2 (sign-extend 32)
            if (rt != 0) AsDWord(regs->m_GPR[rt]).DW =
                static_cast<int64_t>(static_cast<int32_t>(regs->Cop2_MF(rd)));
            break;
        case 1: // DMFC2 (full 64)
            if (rt != 0) AsDWord(regs->m_GPR[rt]).DW =
                static_cast<int64_t>(regs->Cop2_MF(rd));
            break;
        case 2: // CFC2 (sign-extend 32)
            if (rt != 0) AsDWord(regs->m_GPR[rt]).DW =
                static_cast<int64_t>(static_cast<int32_t>(regs->Cop2_MF(rd)));
            break;
        case 4: // MTC2
        case 5: // DMTC2
        case 6: // CTC2
            regs->Cop2_MT(rd, static_cast<uint64_t>(AsDWord(regs->m_GPR[rt]).UDW));
            break;
        default: break;
    }
}

void Arm_Trigger_Cop3RI(CRegisters* regs, uint32_t compileAddr) {
    regs->m_PROGRAM_COUNTER = static_cast<int64_t>(static_cast<int32_t>(compileAddr));
    regs->TriggerException(10, 0); // Reserved Instruction, CE=0
}

} // anonymous namespace

namespace Arm {

// =============================================================================
// Construction
// =============================================================================

ArmRecompilerOps::ArmRecompilerOps(RecompilerContext& context, CodeBlock& codeBlock)
    : m_Context(context)
    , m_CodeBlock(codeBlock)
    , m_Assembler(std::make_unique<CArm64Ops>(codeBlock))
    , m_RegisterSet(std::make_unique<ArmRegInfo>(codeBlock, *m_Assembler))
{
    if (auto* holder = codeBlock.GetCodeHolder()) {
        m_Assembler->Initialize(*holder);
    }
}

ArmRecompilerOps::~ArmRecompilerOps() = default;

// =============================================================================
// Block prologue / epilogue
// =============================================================================

void ArmRecompilerOps::EnterCodeBlock()
{
    auto& a = m_Assembler->GetAssembler();
    m_Assembler->AddComment("=== ARM64 Block Entry ===");

    a.stp(a64::x29, a64::x30, a64::ptr_pre(a64::sp, -96));
    a.mov(a64::x29, a64::sp);
    a.stp(a64::x19, a64::x20, a64::ptr(a64::sp, 16));
    a.stp(a64::x21, a64::x22, a64::ptr(a64::sp, 32));
    a.str(a64::x23, a64::ptr(a64::sp, 48));

    m_Assembler->MovImm64(a64::x19, reinterpret_cast<uint64_t>(&m_Context.registers));
    m_Assembler->MovImm64(a64::x20, reinterpret_cast<uint64_t>(&m_Context.mmu));
    m_Assembler->MovImm64(a64::x21, reinterpret_cast<uint64_t>(&m_Context.system.m_PipelineStage));
    m_Assembler->MovImm64(a64::x22, reinterpret_cast<uint64_t>(&m_Context.system.m_NextTimer));
    m_Assembler->MovImm64(a64::x23, reinterpret_cast<uint64_t>(m_Context.mmu.GetMemoryReadMap()));

    m_BlockExitLabel = a.new_label();
    m_BlockEnded = false;
    m_HasPendingBranchLikelySkip = false;
}

void ArmRecompilerOps::ExitCodeBlock()
{
    auto& a = m_Assembler->GetAssembler();
    a.bind(m_BlockExitLabel);
    m_Assembler->AddComment("=== ARM64 Block Exit ===");

    m_RegisterSet->FlushRegisters(true);

    a.ldp(a64::x19, a64::x20, a64::ptr(a64::sp, 16));
    a.ldp(a64::x21, a64::x22, a64::ptr(a64::sp, 32));
    a.ldr(a64::x23, a64::ptr(a64::sp, 48));
    a.ldp(a64::x29, a64::x30, a64::ptr_post(a64::sp, 96));
    a.ret(a64::x30);

    m_BlockEnded = true;
}

// =============================================================================
// Housekeeping primitives
// =============================================================================

void ArmRecompilerOps::EmitTimerDecrement()
{
    auto& a = m_Assembler->GetAssembler();
    const uint32_t countPerOp = m_Context.system.CountPerOp();
    a.ldr(a64::w9, a64::ptr(a64::x22));
    a.sub(a64::w9, a64::w9, countPerOp);
    a.str(a64::w9, a64::ptr(a64::x22));
}

void ArmRecompilerOps::EmitTimerDecrementAndCheck(uint32_t nextPC)
{
    auto& a = m_Assembler->GetAssembler();
    const uint32_t countPerOp = m_Context.system.CountPerOp();

    a.ldr(a64::w9, a64::ptr(a64::x22));
    a.sub(a64::w9, a64::w9, countPerOp);
    a.str(a64::w9, a64::ptr(a64::x22));

    // w9 still holds the decremented value, so the test costs one compare.
    // m_NextTimer is signed; <= 0 means the next timer event is due.
    asmjit::Label keepGoing = a.new_label();
    a.cmp(a64::w9, 0);
    a.b_gt(keepGoing);

    EmitSetPC(nextPC);   // clobbers x9, whose value has already been consumed
    a.b(m_BlockExitLabel);

    a.bind(keepGoing);
}

void ArmRecompilerOps::EmitPipelineStageCheck()
{
    auto& a = m_Assembler->GetAssembler();
    a.ldrb(a64::w9, a64::ptr(a64::x21));
    a.cmp(a64::w9, static_cast<uint32_t>(N64Types::PipelineStage::Jump));
    a.b_eq(m_BlockExitLabel);
}

void ArmRecompilerOps::EmitPCUpdate()
{
    if (!m_HasCurrentPC || m_BlockEnded) return;
    auto& a = m_Assembler->GetAssembler();
    const int64_t pcVal = static_cast<int64_t>(static_cast<int32_t>(m_CurrentPC));
    m_Assembler->MovImm64(a64::x9, static_cast<uint64_t>(pcVal));
    a.str(a64::x9, a64::ptr(a64::x19,
          kProgramCounterOffset));
}

void ArmRecompilerOps::EmitSetPC(uint32_t addr)
{
    auto& a = m_Assembler->GetAssembler();
    const int64_t pcVal = static_cast<int64_t>(static_cast<int32_t>(addr));
    m_Assembler->MovImm64(a64::x9, static_cast<uint64_t>(pcVal));
    a.str(a64::x9, a64::ptr(a64::x19,
          kProgramCounterOffset));
}

void ArmRecompilerOps::BindPendingBranchLikelyLabel()
{
    if (!m_HasPendingBranchLikelySkip) return;
    auto& a = m_Assembler->GetAssembler();
    a.bind(m_BranchLikelySkipLabel);
    m_HasPendingBranchLikelySkip = false;
}

// =============================================================================
// Native callback emitters (AAPCS64: integer args x0,x1,x2,x3; return x0/w0)
// =============================================================================

void ArmRecompilerOps::EmitRegisterCallback(void* callback, uint32_t opcVal)
{
    EmitPCUpdate();
    auto& a = m_Assembler->GetAssembler();
    m_RegisterSet->FlushRegisters(true);
    a.mov(a64::x0, a64::x19);
    m_Assembler->MovImm64(a64::x1, opcVal);
    m_Assembler->EmitCall(callback);
}

void ArmRecompilerOps::EmitMemoryCallback(void* callback, uint32_t opcVal)
{
    EmitPCUpdate();
    auto& a = m_Assembler->GetAssembler();
    m_RegisterSet->FlushRegisters(true);
    a.mov(a64::x0, a64::x19);
    a.mov(a64::x1, a64::x20);
    m_Assembler->MovImm64(a64::x2, opcVal);
    m_Assembler->EmitCall(callback);
    // A faulting load/store (TLB miss, address error) redirects the pipeline
    // to the exception vector via CRegisters::TriggerAddressException, but
    // that only updates m_System.m_PipelineStage/m_JumpToLocation -- it does
    // NOT touch this compiled block's instruction stream. Without this check,
    // the JIT kept executing the REST of the block (native code compiled
    // assuming the load/store succeeded) against stale/garbage state, which
    // then caused a second, unrelated fault before the real exception handler
    // ever ran. Bail out to the block exit immediately so the recompiler's
    // outer loop (which already checks PipelineStage after a block returns)
    // applies the redirect before any more MIPS instructions execute.
    EmitPipelineStageCheck();
}

void ArmRecompilerOps::EmitTlbCallback(void* callback, uint32_t opcVal)
{
    EmitPCUpdate();
    auto& a = m_Assembler->GetAssembler();
    m_RegisterSet->FlushRegisters(true);
    a.mov(a64::x0, a64::x19);
    m_Assembler->MovImm64(a64::x1, reinterpret_cast<uint64_t>(&m_Context.tlb));
    m_Assembler->MovImm64(a64::x2, opcVal);
    m_Assembler->EmitCall(callback);
    // TLBWI/TLBWR/TLBP/TLBR don't normally fault, but are cheap/rare enough
    // that checking unconditionally (matching EmitMemoryCallback) costs
    // nothing measurable and closes off any edge case symmetrically.
    EmitPipelineStageCheck();
}

void ArmRecompilerOps::EmitSysCallback(void* callback, uint32_t opcVal)
{
    EmitPCUpdate();
    auto& a = m_Assembler->GetAssembler();
    m_RegisterSet->FlushRegisters(true);
    a.mov(a64::x0, a64::x19);
    m_Assembler->MovImm64(a64::x1, reinterpret_cast<uint64_t>(&m_Context.system));
    m_Assembler->MovImm64(a64::x2, opcVal);
    m_Assembler->EmitCall(callback);
}

void ArmRecompilerOps::EmitBranchCallback(void* callback, uint32_t opcVal, uint32_t pc)
{
    auto& a = m_Assembler->GetAssembler();
    m_RegisterSet->FlushRegisters(true);
    a.mov(a64::x0, a64::x19);
    m_Assembler->MovImm64(a64::x1, opcVal);
    m_Assembler->MovImm64(a64::x2, pc);
    m_Assembler->EmitCall(callback);
}

void ArmRecompilerOps::EmitBranchLikelyCallback(void* callback, uint32_t opcVal, uint32_t pc)
{
    auto& a = m_Assembler->GetAssembler();
    m_RegisterSet->FlushRegisters(true);
    a.mov(a64::x0, a64::x19);
    m_Assembler->MovImm64(a64::x1, opcVal);
    m_Assembler->MovImm64(a64::x2, pc);
    m_Assembler->EmitCall(callback);

    // Result in w0: 1=taken, 0=not taken, -1=exception raised (CU1 etc.).
    a.mov(a64::w8, a64::w0); // preserve across timer decrement (which uses w9)

    asmjit::Label noException = a.new_label();
    a.cmn(a64::w8, 1);             // Z set iff w8 == -1
    a.b_ne(noException);
    EmitTimerDecrement();
    a.b(m_BlockExitLabel);
    a.bind(noException);

    // The branch itself always retires, even when the delay slot is skipped.
    EmitTimerDecrement();
    m_BranchLikelySkipLabel = a.new_label();
    m_HasPendingBranchLikelySkip = true;
    a.cmp(a64::w8, 0);            // not taken -> skip the delay slot
    a.b_eq(m_BranchLikelySkipLabel);
}

// =============================================================================
// Per-instruction translation
// =============================================================================

bool ArmRecompilerOps::CompileInstruction(const InstructionContext& ctx)
{
    const R4300iOps op = ctx.opcode;
    const uint32_t  opcVal = op.Value;
    const uint32_t  pc = ctx.virtualAddress;
    m_CurrentPC = pc;
    m_HasCurrentPC = true;

    // Typed dispatch helpers keep the switch readable; the reinterpret_cast to
    // void* (function-pointer -> data-pointer) happens once, here.
    auto reg = [&](void (*cb)(CRegisters*, uint32_t)) {
        EmitRegisterCallback(reinterpret_cast<void*>(cb), opcVal);
    };
    // Same as reg(), but for the handful of register-only ops that can
    // themselves synchronously trigger an exception (SYSCALL, BREAK, the
    // conditional TRAP instructions): the pipeline-stage bail-out is required
    // here for the same reason EmitMemoryCallback needs it -- without it the
    // JIT keeps running the rest of the block against state the exception
    // handler hasn't had a chance to run yet. Kept separate from reg() so the
    // hot-path ALU/shift/logic/mul/div default case pays no extra cost.
    auto regExc = [&](void (*cb)(CRegisters*, uint32_t)) {
        EmitRegisterCallback(reinterpret_cast<void*>(cb), opcVal);
        EmitPipelineStageCheck();
    };
    auto mem = [&](void (*cb)(CRegisters*, MipsMemoryVM*, uint32_t)) {
        EmitMemoryCallback(reinterpret_cast<void*>(cb), opcVal);
    };
    auto brc = [&](void (*cb)(CRegisters*, uint32_t, uint32_t)) {
        EmitBranchCallback(reinterpret_cast<void*>(cb), opcVal, pc);
        m_BlockEnded = true;
    };
    auto brl = [&](int (*cb)(CRegisters*, uint32_t, uint32_t)) {
        EmitBranchLikelyCallback(reinterpret_cast<void*>(cb), opcVal, pc);
        // Match brc(): the branch-likely callback already computed the correct
        // next PC (taken target or pc+8) via SetPC. Without this, the delay
        // slot instruction that follows -- if compiled through a callback that
        // calls EmitPCUpdate() (any memory/register op) -- unconditionally
        // overwrites m_PROGRAM_COUNTER with its own static address, clobbering
        // the branch-likely's decision. This silently redirected every taken
        // branch-likely to the delay slot's own address instead of the real
        // target, corrupting control flow throughout any code using BEQL/BNEL/
        // BLEZL/BGTZL/BLTZL/BGEZL/BLTZALL/BGEZALL/BC1FL/BC1TL (a very common
        // MIPS idiom for loop/null-check conditions).
        m_BlockEnded = true;
    };

    switch (op.Rtype.op) {
    case R4300_SPECIAL:
        switch (mips_funct(opcVal)) {
        case R4300_S_JR:      EmitBranchCallback(reinterpret_cast<void*>(&Interp_JR_Jump),   opcVal, pc); m_BlockEnded = true; break;
        case R4300_S_JALR:    EmitBranchCallback(reinterpret_cast<void*>(&Interp_JALR_Jump), opcVal, pc); m_BlockEnded = true; break;
        case R4300_S_SYSCALL: regExc(&Interp_SYSCALL); break;
        case R4300_S_BREAK:   regExc(&Interp_BREAK);   break;
        case R4300_S_SYNC:    reg(&Interp_SYNC);    break;
        case R4300_S_TGE:     regExc(&Interp_TGE);     break;
        case R4300_S_TGEU:    regExc(&Interp_TGEU);    break;
        case R4300_S_TLT:     regExc(&Interp_TLT);     break;
        case R4300_S_TLTU:    regExc(&Interp_TLTU);    break;
        case R4300_S_TEQ:     regExc(&Interp_TEQ);     break;
        case R4300_S_TNE:     regExc(&Interp_TNE);     break;
        default:              reg(&Interp_SpecialDispatch); break; // ALU/shift/logic/mul/div
        }
        break;

    case R4300_REGIMM:
        switch (op.Itype.rt) {
        case R4300_R_BLTZ:    brc(&Interp_BLTZ_Branch);   break;
        case R4300_R_BGEZ:    brc(&Interp_BGEZ_Branch);   break;
        case R4300_R_BLTZAL:  brc(&Interp_BLTZAL_Branch); break;
        case R4300_R_BGEZAL:  brc(&Interp_BGEZAL_Branch); break;
        case R4300_R_BLTZL:   brl(&Interp_BLTZL_BranchLikely);   break;
        case R4300_R_BGEZL:   brl(&Interp_BGEZL_BranchLikely);   break;
        case R4300_R_BLTZALL: brl(&Interp_BLTZALL_BranchLikely); break;
        case R4300_R_BGEZALL: brl(&Interp_BGEZALL_BranchLikely); break;
        case R4300_R_TGEI:    regExc(&Interp_TGEI);  break;
        case R4300_R_TGEIU:   regExc(&Interp_TGEIU); break;
        case R4300_R_TLTI:    regExc(&Interp_TLTI);  break;
        case R4300_R_TLTIU:   regExc(&Interp_TLTIU); break;
        case R4300_R_TEQI:    regExc(&Interp_TEQI);  break;
        case R4300_R_TNEI:    regExc(&Interp_TNEI);  break;
        default: reg(&Interp_RESERVED); m_BlockEnded = true; break; // invalid REGIMM rt
        }
        break;

    case R4300_J:    brc(&Interp_J_Jump);   break;
    case R4300_JAL:  brc(&Interp_JAL_Jump); break;
    case R4300_BEQ:  brc(&Interp_BEQ_Branch);  break;
    case R4300_BNE:  brc(&Interp_BNE_Branch);  break;
    case R4300_BLEZ: brc(&Interp_BLEZ_Branch); break;
    case R4300_BGTZ: brc(&Interp_BGTZ_Branch); break;

    case R4300_ADDI: case R4300_ADDIU: case R4300_SLTI: case R4300_SLTIU:
    case R4300_ANDI: case R4300_ORI:   case R4300_XORI: case R4300_LUI:
    case R4300_DADDI: case R4300_DADDIU:
        reg(&Interp_ImmDispatch);
        break;

    case R4300_COP0:
        switch (mips_rs(opcVal)) {
        case R4300_COP0_MF:  reg(&Interp_COP0_MFC0);  break;
        case R4300_COP0_DMF: reg(&Interp_COP0_DMFC0); break;
        case R4300_COP0_MT:  reg(&Interp_COP0_MTC0);  break;
        case R4300_COP0_DMT: reg(&Interp_COP0_DMTC0); break;
        default:
            if (op.Value & (1u << 25)) { // CO bit set
                switch (mips_funct(opcVal)) {
                case R4300_COP0_C0_TLBR:  EmitTlbCallback(reinterpret_cast<void*>(&Interp_COP0_CO_TLBR),  opcVal); break;
                case R4300_COP0_C0_TLBWI: EmitTlbCallback(reinterpret_cast<void*>(&Interp_COP0_CO_TLBWI), opcVal); break;
                case R4300_COP0_C0_TLBWR: EmitTlbCallback(reinterpret_cast<void*>(&Interp_COP0_CO_TLBWR), opcVal); break;
                case R4300_COP0_C0_TLBP:  EmitTlbCallback(reinterpret_cast<void*>(&Interp_COP0_CO_TLBP),  opcVal); break;
                case R4300_COP0_C0_ERET:  EmitSysCallback(reinterpret_cast<void*>(&Interp_COP0_CO_ERET),  opcVal); m_BlockEnded = true; break;
                case R4300_COP0_C0_WAIT:  /* low-power wait: treated as NOP */ break;
                default: break;
                }
            }
            break;
        }
        break;

    case R4300_COP1:
        switch (mips_rs(opcVal)) {
        case R4300_COP1_MF:  reg(&Interp_COP1_MFC1);  break;
        case R4300_COP1_DMF: reg(&Interp_COP1_DMFC1); break;
        case R4300_COP1_CF:  reg(&Interp_COP1_CFC1);  break;
        case R4300_COP1_MT:  reg(&Interp_COP1_MTC1);  break;
        case R4300_COP1_DMT: reg(&Interp_COP1_DMTC1); break;
        case R4300_COP1_CT:  reg(&Interp_COP1_CTC1);  break;
        case 3: case 7:      reg(&Interp_COP1_DCFC1_DCTC1); m_BlockEnded = true; break;
        case R4300_COP1_BC:
            switch (op.Itype.rt & 3) {
            case 0: brc(&Interp_BC1F_Branch); break;
            case 1: brc(&Interp_BC1T_Branch); break;
            case 2: brl(&Interp_BC1FL_BranchLikely); break;
            case 3: brl(&Interp_BC1TL_BranchLikely); break;
            }
            break;
        case R4300_COP1_S: {
            const uint32_t funct = mips_funct(opcVal);
            if (funct >= R4300_FPU_C_F)                                        reg(&Interp_COP1_S_CMP);
            else if (funct >= R4300_FPU_CVT_S && funct <= R4300_FPU_CVT_L)     reg(&Interp_COP1_S_CVT);
            else if (funct >= R4300_FPU_ROUND_L && funct <= R4300_FPU_FLOOR_W) reg(&Interp_COP1_S_CVT);
            else if (funct <= R4300_FPU_NEG)                                   reg(&Interp_COP1_S_Arith); // ADD..NEG (0x00-0x07)
            else { reg(&Interp_COP1_Unimplemented); m_BlockEnded = true; }                                // invalid funct -> FPE
            break;
        }
        case R4300_COP1_D: {
            const uint32_t funct = mips_funct(opcVal);
            if (funct >= R4300_FPU_C_F)                                        reg(&Interp_COP1_D_CMP);
            else if (funct >= R4300_FPU_CVT_S && funct <= R4300_FPU_CVT_L)     reg(&Interp_COP1_D_CVT);
            else if (funct >= R4300_FPU_ROUND_L && funct <= R4300_FPU_FLOOR_W) reg(&Interp_COP1_D_CVT);
            else if (funct <= R4300_FPU_NEG)                                   reg(&Interp_COP1_D_Arith);
            else { reg(&Interp_COP1_Unimplemented); m_BlockEnded = true; }
            break;
        }
        case R4300_COP1_W: reg(&Interp_COP1_W_CVT); break;
        case R4300_COP1_L: reg(&Interp_COP1_L_CVT); break;
        default:           reg(&Interp_COP1_Unimplemented); m_BlockEnded = true; break;
        }
        break;

    case R4300_CP2: {
        // COP2: validate CU2 and service MFC2/MTC2/etc; always exits the block.
        EmitPCUpdate();
        auto& a = m_Assembler->GetAssembler();
        m_RegisterSet->FlushRegisters(true);
        a.mov(a64::x0, a64::x19);
        a.mov(a64::x1, a64::x20);
        m_Assembler->MovImm64(a64::x2, opcVal);
        m_Assembler->MovImm64(a64::x3, pc);
        m_Assembler->EmitCall(reinterpret_cast<void*>(&Arm_Interp_Cop2Dispatch));
        EmitTimerDecrement();
        a.b(m_BlockExitLabel);
        m_BlockEnded = true;
        break;
    }

    case R4300_CP3:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: {
        // COP3 / reserved opcodes -> Reserved Instruction exception.
        EmitPCUpdate();
        auto& a = m_Assembler->GetAssembler();
        m_RegisterSet->FlushRegisters(true);
        a.mov(a64::x0, a64::x19);
        m_Assembler->MovImm64(a64::x1, pc);
        m_Assembler->EmitCall(reinterpret_cast<void*>(&Arm_Trigger_Cop3RI));
        EmitTimerDecrement();
        a.b(m_BlockExitLabel);
        m_BlockEnded = true;
        break;
    }

    case R4300_BEQL:  brl(&Interp_BEQL_BranchLikely);  break;
    case R4300_BNEL:  brl(&Interp_BNEL_BranchLikely);  break;
    case R4300_BLEZL: brl(&Interp_BLEZL_BranchLikely); break;
    case R4300_BGTZL: brl(&Interp_BGTZL_BranchLikely); break;

    case R4300_LDL:  mem(&Interp_LDL);  break;
    case R4300_LDR:  mem(&Interp_LDR);  break;
    case R4300_LB:   mem(&Interp_LB);   break;
    case R4300_LH:   mem(&Interp_LH);   break;
    case R4300_LWL:  mem(&Interp_LWL);  break;
    case R4300_LW:   mem(&Interp_LW);   break;
    case R4300_LBU:  mem(&Interp_LBU);  break;
    case R4300_LHU:  mem(&Interp_LHU);  break;
    case R4300_LWR:  mem(&Interp_LWR);  break;
    case R4300_LWU:  mem(&Interp_LWU);  break;
    case R4300_SB:   mem(&Interp_SB);   break;
    case R4300_SH:   mem(&Interp_SH);   break;
    case R4300_SWL:  mem(&Interp_SWL);  break;
    case R4300_SW:   mem(&Interp_SW);   break;
    case R4300_SDL:  mem(&Interp_SDL);  break;
    case R4300_SDR:  mem(&Interp_SDR);  break;
    case R4300_SWR:  mem(&Interp_SWR);  break;
    case R4300_CACHE:mem(&Interp_CACHE);break;
    case R4300_LL:   mem(&Interp_LL);   break;
    case R4300_LLD:  mem(&Interp_LLD);  break;
    case R4300_LWC1: mem(&Interp_LWC1); break;
    case R4300_LDC1: mem(&Interp_LDC1); break;
    case R4300_LD:   mem(&Interp_LD);   break;
    case R4300_SC:   mem(&Interp_SC);   break;
    case R4300_SCD:  mem(&Interp_SCD);  break;
    case R4300_SWC1: mem(&Interp_SWC1); break;
    case R4300_SDC1: mem(&Interp_SDC1); break;
    case R4300_SD:   mem(&Interp_SD);   break;

    default:
        // Unknown primary opcode -> Reserved Instruction exception (matches the
        // x64 backend's UnknownOpcode path), then end the block.
        reg(&Interp_RESERVED);
        m_BlockEnded = true;
        break;
    }

    return true;
}

} // namespace Arm
} // namespace Recompiler
} // namespace Nova64

#endif // __aarch64__ || _M_ARM64
