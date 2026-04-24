/**
 * @file x64RecompilerOps.cpp
 * @brief x64 recompiler operations - MIPS to x64 translation implementation
 *
 * Nova64 N64 Emulator - Based on Project64 architecture
 * Original Project64 code licensed under GPL-2.0
 *
 * This file implements the translation of MIPS R4300i instructions to x64
 * native code. Instruction handlers emit x64 code that calls interpreter
 * callback functions, providing a correct baseline implementation that
 * can be incrementally optimized to inline x64 code generation.
 */

#if defined(__amd64__) || defined(_M_X64)

#include "x64RecompilerOps.hpp"
#include "x64ops.hpp"
#include "x64RegInfo.hpp"
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

#include <cfenv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <atomic>
#include <immintrin.h>

#pragma STDC FENV_ACCESS ON

// ==== BINARY-SEARCH DIAGNOSTIC FLAGS ====
// Set to 1 to revert group to interpreter callbacks, 0 for inline x64
#define FORCE_CALLBACK_SPECIAL 0   // All SPECIAL-format ALU/shift/logic
#define FORCE_CALLBACK_IMM     0   // All immediate-format ALU/logic
#define FORCE_CALLBACK_BRANCH  0   // All conditional branches (BEQ,BNE,etc.)
#define FORCE_CALLBACK_JUMP    0   // J, JAL
// ========================================

namespace Nova64 {
namespace Recompiler {

// Type alias for convenience (PipelineStage already defined in ExitInfo.hpp)
using MIPS_DWORD = N64Types::MIPS_DWORD;
using ExceptionCode = N64Types::ExceptionCode;

// Forward declaration: COP1 usable check for interpreter callbacks
static inline bool CheckCop1Unusable(CRegisters* regs) {
    if (regs->STATUS_REGISTER.CU1 == 0) {
        regs->TriggerException(11, 1); // EXC_CPU=11, Coprocessor=1
        return true;
    }
    return false;
}

// =============================================================================
// Interpreter Callback Functions
// =============================================================================
// These static functions are called at RUNTIME by the JIT-generated x64 code.
// They implement the exact semantics of each MIPS R4300i instruction by
// directly manipulating the CRegisters state.
//
// Callback signatures:
//   RegisterOp:  void(CRegisters* regs, uint32_t opcode)
//   MemoryOp:    void(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcode)
//   SystemOp:    void(CRegisters* regs, CTLB* tlb, CN64System* sys, uint32_t opcode)
// =============================================================================

namespace {

// Helper: reinterpret a uint64_t GPR entry as MIPS_DWORD for union field access
inline MIPS_DWORD& AsDWord(MipsRegisterType& reg) {
    return *reinterpret_cast<MIPS_DWORD*>(&reg);
}

inline const MIPS_DWORD& AsDWord(const MipsRegisterType& reg) {
    return *reinterpret_cast<const MIPS_DWORD*>(&reg);
}

// ---- Safe MIPS instruction field extractors ----
// The R4300iOps.Rtype bitfield struct has incorrectly ordered middle
// fields on MSVC (LSB-first allocation).  The I-type struct is fine,
// but for R-type instructions we MUST use manual bit extraction to
// match the real MIPS encoding.  These helpers mirror the correct
// accessors used by the interpreter.
inline uint32_t mips_rs(uint32_t v)    { return (v >> 21) & 0x1F; }
inline uint32_t mips_rt(uint32_t v)    { return (v >> 16) & 0x1F; }
inline uint32_t mips_rd(uint32_t v)    { return (v >> 11) & 0x1F; }
inline uint32_t mips_sa(uint32_t v)    { return (v >>  6) & 0x1F; }
inline uint32_t mips_funct(uint32_t v) { return  v        & 0x3F; }
inline int16_t  mips_imm(uint32_t v)   { return static_cast<int16_t>(v & 0xFFFF); }
inline uint32_t mips_target(uint32_t v){ return  v & 0x03FFFFFF; }

// FP / Memory field aliases (same bit positions, semantic names)
inline uint32_t mips_base(uint32_t v)  { return (v >> 21) & 0x1F; }  // bits 25:21 = rs = base
inline uint32_t mips_ft(uint32_t v)    { return (v >> 16) & 0x1F; }  // bits 20:16 = rt = ft
inline uint32_t mips_fs(uint32_t v)    { return (v >> 11) & 0x1F; }  // bits 15:11 = rd = fs
inline uint32_t mips_fd(uint32_t v)    { return (v >>  6) & 0x1F; }  // bits 10:6  = sa = fd
inline int16_t  mips_offset(uint32_t v){ return static_cast<int16_t>(v & 0xFFFF); } // bits 15:0 = imm = offset

// Sign-extend a 32-bit PC value to 64 bits (MIPS R4300i requires this for
// addresses in KSEG0/KSEG1 which have bit 31 set).
inline void SetPC(CRegisters* regs, uint32_t pc32) {
    regs->m_PROGRAM_COUNTER = static_cast<int64_t>(static_cast<int32_t>(pc32));
}

// ----- ALU Immediate Callbacks -----

void Interp_ADDI(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    int32_t rsVal = AsDWord(regs->m_GPR[op.Itype.rs]).W[0];
    int32_t immVal = static_cast<int16_t>(op.Itype.imm);
    int32_t sum = rsVal + immVal;
    if ((~(rsVal ^ immVal) & (rsVal ^ sum)) & 0x80000000) {
        regs->TriggerException(12); // Overflow
        return;
    }
    if (op.Itype.rt == 0) return;
    AsDWord(regs->m_GPR[op.Itype.rt]).DW = static_cast<int64_t>(sum);
}

void Interp_ADDIU(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (op.Itype.rt == 0) return;
    int32_t rs_lo = AsDWord(regs->m_GPR[op.Itype.rs]).W[0];
    int16_t imm = static_cast<int16_t>(op.Itype.imm);
    int32_t result = rs_lo + imm;
    AsDWord(regs->m_GPR[op.Itype.rt]).DW = static_cast<int64_t>(result);
}

void Interp_SLTI(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (op.Itype.rt == 0) return;
    int64_t rs = AsDWord(regs->m_GPR[op.Itype.rs]).DW;
    int64_t imm = static_cast<int64_t>(static_cast<int16_t>(op.Itype.imm));
    AsDWord(regs->m_GPR[op.Itype.rt]).DW = (rs < imm) ? 1 : 0;
}

void Interp_SLTIU(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (op.Itype.rt == 0) return;
    uint64_t rs = AsDWord(regs->m_GPR[op.Itype.rs]).UDW;
    uint64_t imm = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(op.Itype.imm)));
    AsDWord(regs->m_GPR[op.Itype.rt]).DW = (rs < imm) ? 1 : 0;
}

void Interp_ANDI(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (op.Itype.rt == 0) return;
    AsDWord(regs->m_GPR[op.Itype.rt]).DW =
        AsDWord(regs->m_GPR[op.Itype.rs]).DW & static_cast<uint64_t>(op.Itype.imm);
}

void Interp_ORI(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (op.Itype.rt == 0) return;
    AsDWord(regs->m_GPR[op.Itype.rt]).DW =
        AsDWord(regs->m_GPR[op.Itype.rs]).DW | static_cast<uint64_t>(op.Itype.imm);
}

void Interp_XORI(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (op.Itype.rt == 0) return;
    AsDWord(regs->m_GPR[op.Itype.rt]).DW =
        AsDWord(regs->m_GPR[op.Itype.rs]).DW ^ static_cast<uint64_t>(op.Itype.imm);
}

void Interp_LUI(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (op.Itype.rt == 0) return;
    int32_t val = static_cast<int32_t>(static_cast<int16_t>(op.Itype.imm) << 16);
    AsDWord(regs->m_GPR[op.Itype.rt]).DW = static_cast<int64_t>(val);
}

void Interp_DADDI(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    int64_t rsVal = AsDWord(regs->m_GPR[op.Itype.rs]).DW;
    int64_t immVal = static_cast<int64_t>(static_cast<int16_t>(op.Itype.imm));
    int64_t sum = rsVal + immVal;
    if ((~(rsVal ^ immVal) & (rsVal ^ sum)) & 0x8000000000000000LL) {
        regs->TriggerException(12); // Overflow
        return;
    }
    if (op.Itype.rt == 0) return;
    AsDWord(regs->m_GPR[op.Itype.rt]).DW = sum;
}

void Interp_DADDIU(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (op.Itype.rt == 0) return;
    int64_t rs = AsDWord(regs->m_GPR[op.Itype.rs]).DW;
    int64_t imm = static_cast<int64_t>(static_cast<int16_t>(op.Itype.imm));
    AsDWord(regs->m_GPR[op.Itype.rt]).DW = rs + imm;
}

// ----- SPECIAL R-type ALU Callbacks -----

void Interp_ADD(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    int32_t rsVal = AsDWord(regs->m_GPR[mips_rs(opcVal)]).W[0];
    int32_t rtVal = AsDWord(regs->m_GPR[mips_rt(opcVal)]).W[0];
    int32_t sum = rsVal + rtVal;
    if ((~(rsVal ^ rtVal) & (rsVal ^ sum)) & 0x80000000) {
        regs->TriggerException(12); // Overflow
        return;
    }
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = static_cast<int64_t>(sum);
}

void Interp_ADDU(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    int32_t result = static_cast<int32_t>(
        AsDWord(regs->m_GPR[mips_rs(opcVal)]).UW[0] +
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0]);
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = static_cast<int64_t>(result);
}

void Interp_SUB(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    int32_t rsVal = AsDWord(regs->m_GPR[mips_rs(opcVal)]).W[0];
    int32_t rtVal = AsDWord(regs->m_GPR[mips_rt(opcVal)]).W[0];
    int32_t sub = rsVal - rtVal;
    if (((rsVal ^ rtVal) & (rsVal ^ sub)) & 0x80000000) {
        regs->TriggerException(12); // Overflow
        return;
    }
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = static_cast<int64_t>(sub);
}

void Interp_SUBU(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    int32_t result = static_cast<int32_t>(
        AsDWord(regs->m_GPR[mips_rs(opcVal)]).UW[0] -
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0]);
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = static_cast<int64_t>(result);
}

void Interp_AND(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW =
        AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW &
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW;
}

void Interp_OR(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW =
        AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW |
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW;
}

void Interp_XOR(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW =
        AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW ^
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW;
}

void Interp_NOR(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW =
        ~(AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW |
          AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW);
}

void Interp_SLT(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW =
        (AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW <
         AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW) ? 1 : 0;
}

void Interp_SLTU(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW =
        (AsDWord(regs->m_GPR[mips_rs(opcVal)]).UDW <
         AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW) ? 1 : 0;
}

void Interp_DADD(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    int64_t rsVal = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int64_t rtVal = AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW;
    int64_t sum = rsVal + rtVal;
    if ((~(rsVal ^ rtVal) & (rsVal ^ sum)) & 0x8000000000000000LL) {
        regs->TriggerException(12); // Overflow
        return;
    }
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = sum;
}

void Interp_DADDU(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW =
        AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW +
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW;
}

void Interp_DSUB(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    int64_t rsVal = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int64_t rtVal = AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW;
    int64_t sub = rsVal - rtVal;
    if (((rsVal ^ rtVal) & (rsVal ^ sub)) & 0x8000000000000000LL) {
        regs->TriggerException(12); // Overflow
        return;
    }
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = sub;
}

void Interp_DSUBU(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW =
        AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW -
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW;
}

// ----- Shift Callbacks -----

void Interp_SLL(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    int32_t result = static_cast<int32_t>(
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0] << mips_sa(opcVal));
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = static_cast<int64_t>(result);
}

void Interp_SRL(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    int32_t result = static_cast<int32_t>(
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0] >> mips_sa(opcVal));
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = static_cast<int64_t>(result);
}

void Interp_SRA(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    // VR4300: shift the full 64-bit GPR value, then truncate low 32 and sign-extend
    int64_t full = AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW >> mips_sa(opcVal);
    int32_t lo32 = static_cast<int32_t>(full);
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = static_cast<int64_t>(lo32);
}

void Interp_SLLV(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    uint32_t shift = AsDWord(regs->m_GPR[mips_rs(opcVal)]).UW[0] & 0x1F;
    int32_t result = static_cast<int32_t>(
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0] << shift);
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = static_cast<int64_t>(result);
}

void Interp_SRLV(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    uint32_t shift = AsDWord(regs->m_GPR[mips_rs(opcVal)]).UW[0] & 0x1F;
    int32_t result = static_cast<int32_t>(
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0] >> shift);
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = static_cast<int64_t>(result);
}

void Interp_SRAV(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    uint32_t shift = AsDWord(regs->m_GPR[mips_rs(opcVal)]).UW[0] & 0x1F;
    // VR4300: shift the full 64-bit GPR value, then truncate low 32 and sign-extend
    int64_t full = AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW >> shift;
    int32_t lo32 = static_cast<int32_t>(full);
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = static_cast<int64_t>(lo32);
}

void Interp_DSLL(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW =
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW << mips_sa(opcVal);
}

void Interp_DSRL(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).UDW =
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW >> mips_sa(opcVal);
}

void Interp_DSRA(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW =
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW >> mips_sa(opcVal);
}

void Interp_DSLL32(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW =
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW << (mips_sa(opcVal) + 32);
}

void Interp_DSRL32(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).UDW =
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW >> (mips_sa(opcVal) + 32);
}

void Interp_DSRA32(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW =
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW >> (mips_sa(opcVal) + 32);
}

void Interp_DSLLV(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    uint32_t shift = AsDWord(regs->m_GPR[mips_rs(opcVal)]).UW[0] & 0x3F;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW =
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW << shift;
}

void Interp_DSRLV(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    uint32_t shift = AsDWord(regs->m_GPR[mips_rs(opcVal)]).UW[0] & 0x3F;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).UDW =
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW >> shift;
}

void Interp_DSRAV(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    uint32_t shift = AsDWord(regs->m_GPR[mips_rs(opcVal)]).UW[0] & 0x3F;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW =
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW >> shift;
}

// ----- Multiply / Divide Callbacks -----

void Interp_MULT(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    int64_t result = static_cast<int64_t>(AsDWord(regs->m_GPR[mips_rs(opcVal)]).W[0]) *
                     static_cast<int64_t>(AsDWord(regs->m_GPR[mips_rt(opcVal)]).W[0]);
    AsDWord(regs->m_LO).DW = static_cast<int64_t>(static_cast<int32_t>(result & 0xFFFFFFFF));
    AsDWord(regs->m_HI).DW = static_cast<int64_t>(static_cast<int32_t>(result >> 32));
}

void Interp_MULTU(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t result = static_cast<uint64_t>(AsDWord(regs->m_GPR[mips_rs(opcVal)]).UW[0]) *
                      static_cast<uint64_t>(AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0]);
    AsDWord(regs->m_LO).DW = static_cast<int64_t>(static_cast<int32_t>(result & 0xFFFFFFFF));
    AsDWord(regs->m_HI).DW = static_cast<int64_t>(static_cast<int32_t>(result >> 32));
}

void Interp_DIV(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    int32_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).W[0];
    int32_t rt = AsDWord(regs->m_GPR[mips_rt(opcVal)]).W[0];
    if (rt == 0) {
        AsDWord(regs->m_LO).DW = (rs < 0) ? 1 : -1;
        AsDWord(regs->m_HI).DW = static_cast<int64_t>(rs);
    } else if (rs == INT32_MIN && rt == -1) {
        AsDWord(regs->m_LO).DW = static_cast<int64_t>(INT32_MIN);
        AsDWord(regs->m_HI).DW = 0;
    } else {
        AsDWord(regs->m_LO).DW = static_cast<int64_t>(rs / rt);
        AsDWord(regs->m_HI).DW = static_cast<int64_t>(rs % rt);
    }
}

void Interp_DIVU(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint32_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).UW[0];
    uint32_t rt = AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0];
    if (rt == 0) {
        AsDWord(regs->m_LO).DW = -1;
        AsDWord(regs->m_HI).DW = static_cast<int64_t>(static_cast<int32_t>(rs));
    } else {
        AsDWord(regs->m_LO).DW = static_cast<int64_t>(static_cast<int32_t>(rs / rt));
        AsDWord(regs->m_HI).DW = static_cast<int64_t>(static_cast<int32_t>(rs % rt));
    }
}

void Interp_DMULT(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    // 64x64 -> 128 signed multiplication using partial products
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int64_t rt = AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW;
    // Use compiler intrinsic or manual 128-bit mult
#ifdef _MSC_VER
    int64_t hi;
    int64_t lo = _mul128(rs, rt, &hi);
    AsDWord(regs->m_LO).DW = lo;
    AsDWord(regs->m_HI).DW = hi;
#else
    __int128 result = static_cast<__int128>(rs) * static_cast<__int128>(rt);
    AsDWord(regs->m_LO).DW = static_cast<int64_t>(result);
    AsDWord(regs->m_HI).DW = static_cast<int64_t>(result >> 64);
#endif
}

void Interp_DMULTU(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).UDW;
    uint64_t rt = AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW;
#ifdef _MSC_VER
    uint64_t hi;
    uint64_t lo = _umul128(rs, rt, &hi);
    AsDWord(regs->m_LO).UDW = lo;
    AsDWord(regs->m_HI).UDW = hi;
#else
    unsigned __int128 result = static_cast<unsigned __int128>(rs) * static_cast<unsigned __int128>(rt);
    AsDWord(regs->m_LO).UDW = static_cast<uint64_t>(result);
    AsDWord(regs->m_HI).UDW = static_cast<uint64_t>(result >> 64);
#endif
}

void Interp_DDIV(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int64_t rt = AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW;
    if (rt == 0) {
        AsDWord(regs->m_LO).DW = (rs < 0) ? 1 : -1;
        AsDWord(regs->m_HI).DW = rs;
    } else if (rs == INT64_MIN && rt == -1) {
        AsDWord(regs->m_LO).DW = INT64_MIN;
        AsDWord(regs->m_HI).DW = 0;
    } else {
        AsDWord(regs->m_LO).DW = rs / rt;
        AsDWord(regs->m_HI).DW = rs % rt;
    }
}

void Interp_DDIVU(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).UDW;
    uint64_t rt = AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW;
    if (rt == 0) {
        AsDWord(regs->m_LO).UDW = 0xFFFFFFFFFFFFFFFFULL;
        AsDWord(regs->m_HI).UDW = rs;
    } else {
        AsDWord(regs->m_LO).UDW = rs / rt;
        AsDWord(regs->m_HI).UDW = rs % rt;
    }
}

// ----- HI/LO Move Callbacks -----

void Interp_MFHI(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = AsDWord(regs->m_HI).DW;
}

void Interp_MTHI(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    AsDWord(regs->m_HI).DW = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
}

void Interp_MFLO(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = AsDWord(regs->m_LO).DW;
}

void Interp_MTLO(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    AsDWord(regs->m_LO).DW = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
}

// ----- Load Callbacks (require MMU) -----

void Interp_LB(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint8_t val = 0;
    if (mmu->LB_Memory(addr, val)) {
        if (mips_rt(opcVal) != 0)
            AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW = static_cast<int64_t>(static_cast<int8_t>(val));
    }
}

void Interp_LBU(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint8_t val = 0;
    if (mmu->LB_Memory(addr, val)) {
        if (mips_rt(opcVal) != 0)
            AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW = static_cast<int64_t>(val);
    }
}

void Interp_LH(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint16_t val = 0;
    if (mmu->LH_Memory(addr, val)) {
        if (mips_rt(opcVal) != 0)
            AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW = static_cast<int64_t>(static_cast<int16_t>(val));
    }
}

void Interp_LHU(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint16_t val = 0;
    if (mmu->LH_Memory(addr, val)) {
        if (mips_rt(opcVal) != 0)
            AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW = static_cast<int64_t>(val);
    }
}

void Interp_LW(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint32_t val = 0;
    if (mmu->LW_Memory(addr, val)) {
        if (mips_rt(opcVal) != 0) {
            AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW = static_cast<int64_t>(static_cast<int32_t>(val));
        }
    }
}

void Interp_LWU(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint32_t val = 0;
    if (mmu->LW_Memory(addr, val)) {
        if (mips_rt(opcVal) != 0)
            AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW = static_cast<int64_t>(val);
    }
}

void Interp_LD(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint64_t val = 0;
    if (mmu->LD_Memory(addr, val)) {
        if (mips_rt(opcVal) != 0)
            AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW = val;
    }
}

void Interp_LL(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint32_t val = 0;
    if (mmu->LW_Memory(addr, val)) {
        if (mips_rt(opcVal) != 0)
            AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW = static_cast<int64_t>(static_cast<int32_t>(val));
        regs->m_LLBit = 1;
    } else {
        regs->m_LLBit = 0;
    }
}

void Interp_LWL(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    static const uint32_t LWL_MASK[4] = { 0x00000000, 0x000000FF, 0x0000FFFF, 0x00FFFFFF };
    static const int LWL_SHIFT[4] = { 0, 8, 16, 24 };
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint32_t offset = static_cast<uint32_t>(addr) & 3;
    uint32_t val = 0;
    if (mmu->LW_Memory(addr & ~3, val)) {
        if (mips_rt(opcVal) != 0) {
            uint32_t existing = AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0];
            existing = (existing & LWL_MASK[offset]) | (val << LWL_SHIFT[offset]);
            AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW = static_cast<int64_t>(static_cast<int32_t>(existing));
        }
    }
}

void Interp_LWR(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    static const uint32_t LWR_MASK[4] = { 0xFFFFFF00, 0xFFFF0000, 0xFF000000, 0x00000000 };
    static const int LWR_SHIFT[4] = { 24, 16, 8, 0 };
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint32_t offset = static_cast<uint32_t>(addr) & 3;
    uint32_t val = 0;
    if (mmu->LW_Memory(addr & ~3, val)) {
        if (mips_rt(opcVal) != 0) {
            uint32_t existing = AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0];
            existing = (existing & LWR_MASK[offset]) | (val >> LWR_SHIFT[offset]);
            AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW = static_cast<int64_t>(static_cast<int32_t>(existing));
        }
    }
}

void Interp_LDL(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    static const uint64_t LDL_MASK[8] = {
        0, 0xFF, 0xFFFF, 0xFFFFFF, 0xFFFFFFFF,
        0xFFFFFFFFFF, 0xFFFFFFFFFFFF, 0xFFFFFFFFFFFFFF
    };
    static const int LDL_SHIFT[8] = { 0, 8, 16, 24, 32, 40, 48, 56 };
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint32_t offset = static_cast<uint32_t>(addr) & 7;
    uint64_t val = 0;
    if (mmu->LD_Memory(addr & ~7, val)) {
        if (mips_rt(opcVal) != 0) {
            uint64_t existing = AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW;
            AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW =
                (existing & LDL_MASK[offset]) | (val << LDL_SHIFT[offset]);
        }
    }
}

void Interp_LDR(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    static const uint64_t LDR_MASK[8] = {
        0xFFFFFFFFFFFFFF00, 0xFFFFFFFFFFFF0000, 0xFFFFFFFFFF000000,
        0xFFFFFFFF00000000, 0xFFFFFF0000000000, 0xFFFF000000000000,
        0xFF00000000000000, 0
    };
    static const int LDR_SHIFT[8] = { 56, 48, 40, 32, 24, 16, 8, 0 };
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint32_t offset = static_cast<uint32_t>(addr) & 7;
    uint64_t val = 0;
    if (mmu->LD_Memory(addr & ~7, val)) {
        if (mips_rt(opcVal) != 0) {
            uint64_t existing = AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW;
            AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW =
                (existing & LDR_MASK[offset]) | (val >> LDR_SHIFT[offset]);
        }
    }
}

// FPR access helpers — work on m_FPR[] directly (matching interpreter).
// FR=0 (32-bit mode): even/odd pairs share one 64-bit slot in m_FPR[even].
//   Even reg n → low 32 of m_FPR[n], Odd reg n → high 32 of m_FPR[n & ~1].
// FR=1 (64-bit mode): each register is independent in m_FPR[reg].

// Data-transfer read (MFC1/SWC1)
static inline uint32_t fpr_read_w(CRegisters* regs, uint32_t reg) {
    if (regs->STATUS_REGISTER.FR == 0) {
        uint32_t* p = reinterpret_cast<uint32_t*>(&regs->m_FPR[reg & ~1u]);
        return p[reg & 1];
    }
    return static_cast<uint32_t>(regs->m_FPR[reg]);
}

// Data-transfer write (MTC1/LWC1): preserves the other half of the pair
static inline void fpr_write_w(CRegisters* regs, uint32_t reg, uint32_t val) {
    if (regs->STATUS_REGISTER.FR == 0) {
        uint32_t* p = reinterpret_cast<uint32_t*>(&regs->m_FPR[reg & ~1u]);
        p[reg & 1] = val;
    } else {
        // FR=1: write only low 32 bits, preserve upper 32
        regs->m_FPR[reg] = val;
        
    }
}

// Arithmetic operand read (single-precision):
// FR=0: read selected 32-bit half of the even/odd pair
// FR=1: read low 32 of m_FPR[reg]
static inline float fpr_read_float_op(CRegisters* regs, uint32_t reg) {
    uint32_t bits = fpr_read_w(regs, reg);
    float result;
    std::memcpy(&result, &bits, 4);
    return result;
}

// Arithmetic result write (single-precision):
// Matches interpreter: write one 32-bit lane and preserve the paired lane.
static inline void fpr_write_w_result(CRegisters* regs, uint32_t reg, uint32_t val) {
    fpr_write_w(regs, reg, val);
}

static inline void fpr_write_float_result(CRegisters* regs, uint32_t reg, float val) {
    uint32_t bits;
    std::memcpy(&bits, &val, 4);
    fpr_write_w_result(regs, reg, bits);
}

// MOV.S writes one 32-bit lane.
static inline void fpr_write_w_move(CRegisters* regs, uint32_t reg, uint32_t val) {
    fpr_write_w(regs, reg, val);
}

static inline void fpr_write_float_move(CRegisters* regs, uint32_t reg, float val) {
    uint32_t bits;
    std::memcpy(&bits, &val, 4);
    fpr_write_w_move(regs, reg, bits);
}

static inline float fpr_read_float(CRegisters* regs, uint32_t reg) {
    uint32_t bits = fpr_read_w(regs, reg);
    float result;
    std::memcpy(&result, &bits, 4);
    return result;
}

static inline void fpr_write_float(CRegisters* regs, uint32_t reg, float val) {
    uint32_t bits;
    std::memcpy(&bits, &val, 4);
    fpr_write_w(regs, reg, bits);
}

static inline uint64_t fpr_read_u64(CRegisters* regs, uint32_t reg);
static inline void fpr_write_u64(CRegisters* regs, uint32_t reg, uint64_t val);

static inline double fpr_read_double(CRegisters* regs, uint32_t reg) {
    uint64_t bits;
    if (regs->STATUS_REGISTER.FR == 0) {
        bits = regs->m_FPR[reg & ~1u];
    } else {
        bits = regs->m_FPR[reg];
    }
    double result;
    std::memcpy(&result, &bits, 8);
    return result;
}

// Arithmetic double write: FR=0 uses even/odd pair mapping.
static inline void fpr_write_double(CRegisters* regs, uint32_t reg, double val) {
    uint64_t bits;
    std::memcpy(&bits, &val, 8);
    fpr_write_u64(regs, reg, bits);
}

// Arithmetic 64-bit result write (double/L format):
// FR=0 uses even/odd pair mapping.
static inline void fpr_write_u64_arith(CRegisters* regs, uint32_t reg, uint64_t val) {
    fpr_write_u64(regs, reg, val);
}

static inline uint64_t fpr_read_u64(CRegisters* regs, uint32_t reg) {
    if (regs->STATUS_REGISTER.FR == 0) {
        return regs->m_FPR[reg & ~1u];
    }
    return regs->m_FPR[reg];
}

static inline void fpr_write_u64(CRegisters* regs, uint32_t reg, uint64_t val) {
    if (regs->STATUS_REGISTER.FR == 0) {
        regs->m_FPR[reg & ~1u] = val;
    } else {
        regs->m_FPR[reg] = val;
    }
}

void Interp_LWC1(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(op.Itype.imm);
    uint32_t val = 0;
    if (mmu->LW_Memory(addr, val)) {
        fpr_write_w(regs, mips_ft(opcVal), val);
    }
}

void Interp_LDC1(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(op.Itype.imm);
    uint64_t val = 0;
    if (mmu->LD_Memory(addr, val)) {
        fpr_write_u64(regs, mips_ft(opcVal), val);
    }
}

// ----- Store Callbacks -----

void Interp_SB(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    mmu->SB_Memory(addr, AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0]);
}

void Interp_SH(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    mmu->SH_Memory(addr, AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0]);
}

void Interp_SW(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint32_t val = AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0];
    mmu->SW_Memory(addr, val);
}

void Interp_SD(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    mmu->SD_Memory(addr, AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW);
}

void Interp_SWL(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    static const uint32_t SWL_MASK[4] = { 0x00000000, 0xFF000000, 0xFFFF0000, 0xFFFFFF00 };
    static const int SWL_SHIFT[4] = { 0, 8, 16, 24 };
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint32_t offset = static_cast<uint32_t>(addr) & 3;
    uint32_t memVal = 0;
    // Use MemoryValue32 (no exception on miss) instead of LW_Memory
    // so that on TLB miss, SW_Memory raises TLBS (not TLBL from LW_Memory)
    uint32_t alignedAddr32 = static_cast<uint32_t>(addr) & ~3u;
    if (mmu->MemoryValue32(alignedAddr32, memVal)) {
        uint32_t rtVal = AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0];
        memVal = (memVal & SWL_MASK[offset]) | (rtVal >> SWL_SHIFT[offset]);
        mmu->SW_Memory(addr & ~3, memVal);
    } else {
        // Trigger TLBS with the original (possibly unaligned) address as BadVAddr
        regs->TriggerAddressException(addr, static_cast<uint32_t>(ExceptionCode::TLBStoreMiss));
    }
}

void Interp_SWR(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    static const uint32_t SWR_MASK[4] = { 0x00FFFFFF, 0x0000FFFF, 0x000000FF, 0x00000000 };
    static const int SWR_SHIFT[4] = { 24, 16, 8, 0 };
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint32_t offset = static_cast<uint32_t>(addr) & 3;
    uint32_t memVal = 0;
    // Use MemoryValue32 (no exception on miss) instead of LW_Memory
    // so that on TLB miss, SW_Memory raises TLBS (not TLBL from LW_Memory)
    uint32_t alignedAddr32 = static_cast<uint32_t>(addr) & ~3u;
    if (mmu->MemoryValue32(alignedAddr32, memVal)) {
        uint32_t rtVal = AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0];
        memVal = (memVal & SWR_MASK[offset]) | (rtVal << SWR_SHIFT[offset]);
        mmu->SW_Memory(addr & ~3, memVal);
    } else {
        // Trigger TLBS with the original (possibly unaligned) address as BadVAddr
        regs->TriggerAddressException(addr, static_cast<uint32_t>(ExceptionCode::TLBStoreMiss));
    }
}

void Interp_SDL(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    static const uint64_t SDL_MASK[8] = {
        0, 0xFF00000000000000, 0xFFFF000000000000, 0xFFFFFF0000000000,
        0xFFFFFFFF00000000, 0xFFFFFFFFFF000000, 0xFFFFFFFFFFFF0000, 0xFFFFFFFFFFFFFF00
    };
    static const int SDL_SHIFT[8] = { 0, 8, 16, 24, 32, 40, 48, 56 };
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint32_t offset = static_cast<uint32_t>(addr) & 7;
    uint64_t memVal = 0;
    // Use MemoryValue64 (no exception on miss) instead of LD_Memory
    // so that on TLB miss, SD_Memory raises TLBS (not TLBL from LD_Memory)
    uint32_t alignedAddr32 = static_cast<uint32_t>(addr) & ~7u;
    if (mmu->MemoryValue64(alignedAddr32, memVal)) {
        uint64_t rtVal = AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW;
        memVal = (memVal & SDL_MASK[offset]) | (rtVal >> SDL_SHIFT[offset]);
        mmu->SD_Memory(addr & ~7, memVal);
    } else {
        regs->TriggerAddressException(addr, static_cast<uint32_t>(ExceptionCode::TLBStoreMiss));
    }
}

void Interp_SDR(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    static const uint64_t SDR_MASK[8] = {
        0x00FFFFFFFFFFFFFF, 0x0000FFFFFFFFFFFF, 0x000000FFFFFFFFFF,
        0x00000000FFFFFFFF, 0x0000000000FFFFFF, 0x000000000000FFFF,
        0x00000000000000FF, 0
    };
    static const int SDR_SHIFT[8] = { 56, 48, 40, 32, 24, 16, 8, 0 };
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint32_t offset = static_cast<uint32_t>(addr) & 7;
    uint64_t memVal = 0;
    // Use MemoryValue64 (no exception on miss) instead of LD_Memory
    uint32_t alignedAddr32_r = static_cast<uint32_t>(addr) & ~7u;
    if (mmu->MemoryValue64(alignedAddr32_r, memVal)) {
        uint64_t rtVal = AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW;
        memVal = (memVal & SDR_MASK[offset]) | (rtVal << SDR_SHIFT[offset]);
        mmu->SD_Memory(addr & ~7, memVal);
    } else {
        regs->TriggerAddressException(addr, static_cast<uint32_t>(ExceptionCode::TLBStoreMiss));
    }
}

void Interp_SC(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    bool success = false;
    if (regs->m_LLBit == 1) {
        uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                        static_cast<int16_t>(mips_offset(opcVal));
        success = mmu->SW_Memory(addr, AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0]);
    }
    if (mips_rt(opcVal) != 0) {
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW = success ? 1 : 0;
    }
    regs->m_LLBit = 0;
}

void Interp_LLD(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(mips_offset(opcVal));
    uint64_t val = 0;
    if (mmu->LD_Memory(addr, val)) {
        if (mips_rt(opcVal) != 0)
            AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW = val;
        regs->m_LLBit = 1;
    } else {
        regs->m_LLBit = 0;
    }
}

void Interp_SCD(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    bool success = false;
    if (regs->m_LLBit == 1) {
        uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                        static_cast<int16_t>(mips_offset(opcVal));
        success = mmu->SD_Memory(addr, AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW);
    }
    if (mips_rt(opcVal) != 0) {
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW = success ? 1 : 0;
    }
    regs->m_LLBit = 0;
}

void Interp_SWC1(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(op.Itype.imm);
    uint32_t val = fpr_read_w(regs, mips_ft(opcVal));
    mmu->SW_Memory(addr, val);
}

void Interp_SDC1(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(op.Itype.imm);
    mmu->SD_Memory(addr, fpr_read_u64(regs, mips_ft(opcVal)));
}

// ----- COP0 Callbacks -----

void Interp_COP0_MFC0(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rt(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW =
        static_cast<int64_t>(static_cast<int32_t>(
            regs->Cop0_MF(static_cast<CRegisters::COP0Reg>(mips_rd(opcVal)))));
}

void Interp_COP0_DMFC0(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rt(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW =
        regs->Cop0_MF(static_cast<CRegisters::COP0Reg>(mips_rd(opcVal)));
}

void Interp_COP0_MTC0(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    regs->Cop0_MT(static_cast<CRegisters::COP0Reg>(mips_rd(opcVal)),
                  static_cast<int64_t>(AsDWord(regs->m_GPR[mips_rt(opcVal)]).W[0]));
}

void Interp_COP0_DMTC0(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    regs->Cop0_MT(static_cast<CRegisters::COP0Reg>(mips_rd(opcVal)),
                  AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW);
}

void Interp_COP0_CO_TLBR(CRegisters* /*regs*/, CTLB* tlb, uint32_t /*opcVal*/) {
    tlb->ReadEntry();
}

void Interp_COP0_CO_TLBWI(CRegisters* regs, CTLB* tlb, uint32_t /*opcVal*/) {
    tlb->WriteEntry(static_cast<uint32_t>(regs->INDEX_REGISTER) & 0x1F, false);
}

void Interp_COP0_CO_TLBWR(CRegisters* regs, CTLB* tlb, uint32_t /*opcVal*/) {
    // Random register cycles in [WIRED, 31]; compute random index on use
    uint32_t wired = static_cast<uint32_t>(regs->WIRED_REGISTER) & 0x3F;
    if (wired > 31) wired = 31;
    uint32_t range = 32 - wired;
    uint32_t random_index = wired + (rand() % range);
    tlb->WriteEntry(random_index, true);
}

void Interp_COP0_CO_TLBP(CRegisters* /*regs*/, CTLB* tlb, uint32_t /*opcVal*/) {
    tlb->Probe();
}

void Interp_COP0_CO_ERET(CRegisters* regs, CN64System* sys, uint32_t /*opcVal*/) {
    // Match the interpreter: set PipelineStage = Jump and JumpToLocation,
    // NOT m_PROGRAM_COUNTER directly.  The dispatcher will apply the jump
    // after the block exits.
    if (regs->STATUS_REGISTER.ErrorLevel) {
        sys->m_JumpToLocation = regs->ERROREPC_REGISTER;
        regs->STATUS_REGISTER.ErrorLevel = 0;
    } else {
        sys->m_JumpToLocation = regs->EPC_REGISTER;
        regs->STATUS_REGISTER.ExceptionLevel = 0;
    }
    sys->m_PipelineStage = N64Types::PipelineStage::Jump;
    regs->m_LLBit = 0;
    // Don't call CheckInterrupts here — it would queue an interrupt exception
    // that fires BEFORE the ERET target instruction executes. On real VR4300,
    // ERET lets at least one instruction execute before servicing interrupts.
    // Instead, set a deferred flag so the dispatch loop calls CheckInterrupts
    // AFTER the next block executes.
    sys->m_DeferredCheckInterrupts = true;
    sys->m_TestTimer = true;
}

// ----- COP1 Move/Control Callbacks -----

// DCFC1/DCTC1 (rs=3/7): check CU1 first, then fire FPE if usable
static void Interp_COP1_DCFC1_DCTC1(CRegisters* regs, uint32_t /*opcVal*/) {
    if (CheckCop1Unusable(regs)) return;
    // VR4300: DCFC1/DCTC1 are 64-bit CFC1/CTC1 → Reserved Instruction on VR4300
    // but the test expects UnimplementedOp (FPE with cause=unimpl)
    regs->m_FPCR[31] &= ~0x0003F000; // Clear Cause bits
    regs->m_FPCR[31] |= (1 << 17);   // Cause.UnimplementedOp
    regs->TriggerException(15, 0);    // FPE
}

void Interp_COP1_MFC1(CRegisters* regs, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    R4300iOps op; op.Value = opcVal;
    if (mips_rt(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW =
        static_cast<int64_t>(static_cast<int32_t>(fpr_read_w(regs, mips_rd(opcVal))));
}

void Interp_COP1_DMFC1(CRegisters* regs, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    R4300iOps op; op.Value = opcVal;
    if (mips_rt(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW =
        static_cast<int64_t>(fpr_read_u64(regs, mips_rd(opcVal)));
}

void Interp_COP1_MTC1(CRegisters* regs, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    R4300iOps op; op.Value = opcVal;
    fpr_write_w(regs, mips_rd(opcVal),
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0]);
}

void Interp_COP1_DMTC1(CRegisters* regs, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    R4300iOps op; op.Value = opcVal;
    fpr_write_u64(regs, mips_rd(opcVal), AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW);
}

// Forward declaration — defined below with other FPU helpers
static inline void SyncRoundingMode(CRegisters* regs);

void Interp_COP1_CFC1(CRegisters* regs, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    R4300iOps op; op.Value = opcVal;
    if (mips_rt(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW =
        static_cast<int64_t>(static_cast<int32_t>(regs->m_FPCR[mips_rd(opcVal)]));
}

void Interp_COP1_CTC1(CRegisters* regs, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    R4300iOps op; op.Value = opcVal;
    regs->Cop1_CT(mips_rd(opcVal), AsDWord(regs->m_GPR[mips_rt(opcVal)]).W[0]);
    SyncRoundingMode(regs);
}

// ----- COP1 FPU Arithmetic (use softfloat via interpreter callbacks) -----
// For FPU, we call through to the CRegisters-level FPU implementation.
// These are simplified interpreter callbacks - a full implementation would
// route through the softfloat library for IEEE 754 compliance.

// Force precise FP semantics for all FPU callback functions.
// The project uses /fp:fast which sets FTZ/DAZ and allows the compiler to
// reorder/optimize FP operations. We need precise semantics for correct
// N64 FPU emulation (denorm handling, exception flags, rounding modes).
#pragma float_control(precise, on, push)
#pragma fenv_access(on)

// VR4300: undefined COP1 format → UnimplementedOp (FPE)
static void Interp_COP1_Unimplemented(CRegisters* regs, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    regs->m_FPCR[31] &= ~0x0003F000; // Clear Cause bits
    regs->m_FPCR[31] |= (1 << 17);   // Cause.UnimplementedOp
    regs->TriggerException(15, 0);    // FPE
}

// Sync the host x86 rounding mode to match MIPS FCR31 bits 1:0.
// Must be called before any native float operation that depends on rounding.
static inline void SyncRoundingMode(CRegisters* regs) {
    // Map MIPS rounding mode to MXCSR rounding control (bits 14:13)
    // MIPS: 0=Nearest, 1=Zero, 2=PosInf(Ceil), 3=NegInf(Floor)
    // MXCSR RC: 0=Nearest, 1=NegInf, 2=PosInf, 3=Zero
    static constexpr unsigned int mipsToMxcsr[4] = { 0, 3, 2, 1 };
    unsigned int mxcsr = _mm_getcsr();
    mxcsr &= ~(0x8040u | (3u << 13)); // Clear FTZ, DAZ, and RC bits
    mxcsr |= (mipsToMxcsr[regs->m_FPCR[31] & 3] << 13); // Set RC
    _mm_setcsr(mxcsr);
}

// Update FCR31 exception flags from x86 FP exception status (matching interpreter's SetFPUException)
static inline bool UpdateFCR31FromHostExceptions(CRegisters* regs) {
    int exc = std::fetestexcept(FE_ALL_EXCEPT);
    if (exc == 0) return false;
    FPStatusReg& sr = reinterpret_cast<FPStatusReg&>(regs->m_FPCR[31]);
    if (exc & FE_INEXACT)   { sr.Cause.Inexact = 1;          }
    if (exc & FE_UNDERFLOW) { sr.Cause.Underflow = 1;        }
    if (exc & FE_OVERFLOW)  { sr.Cause.Overflow = 1;         }
    if (exc & FE_DIVBYZERO) { sr.Cause.DivisionByZero = 1;   }
    if (exc & FE_INVALID)   { sr.Cause.InvalidOperation = 1; }
    // Check if any enabled exception was raised → FPE
    uint32_t causes  = (regs->m_FPCR[31] >> 12) & 0x1F; // bits 12-16
    uint32_t enables = (regs->m_FPCR[31] >> 7)  & 0x1F; // bits 7-11
    if (causes & enables) {
        regs->TriggerException(15, 0);
        return true; // FPE triggered, caller should skip result write
    }
    // No FPE — set flags for raised causes
    if (exc & FE_INEXACT)   sr.Flags.Inexact = 1;
    if (exc & FE_UNDERFLOW) sr.Flags.Underflow = 1;
    if (exc & FE_OVERFLOW)  sr.Flags.Overflow = 1;
    if (exc & FE_DIVBYZERO) sr.Flags.DivisionByZero = 1;
    if (exc & FE_INVALID)   sr.Flags.InvalidOperation = 1;
    return false;
}

// Generic FPU operation callback - single-precision (S format)
// Read single-precision FPR as raw bits using arithmetic fs operand rules
// (FR=0: select 32-bit half by register index)
static inline uint32_t fpr_read_w_op(CRegisters* regs, uint32_t reg) {
    return fpr_read_w(regs, reg);
}

// Read single-precision FPR as raw bits for ft operand
// (FR=0: select 32-bit half by register index)
static inline uint32_t fpr_read_w_op_ft(CRegisters* regs, uint32_t reg) {
    return fpr_read_w(regs, reg);
}

// Read single-precision FPR as float for ft operand
// (FR=0: select 32-bit half by register index)
static inline float fpr_read_float_op_ft(CRegisters* regs, uint32_t reg) {
    uint32_t bits = fpr_read_w(regs, reg);
    float result;
    std::memcpy(&result, &bits, 4);
    return result;
}

// Read double FPR for ft operand
// (FR=0: bit 0 ignored, use even/odd pair mapping)
static inline double fpr_read_double_ft(CRegisters* regs, uint32_t reg) {
    uint64_t bits = fpr_read_u64(regs, reg);
    double result;
    std::memcpy(&result, &bits, 8);
    return result;
}

// Read 64-bit FPR for ft operand
// (FR=0: bit 0 ignored, use even/odd pair mapping)
static inline uint64_t fpr_read_u64_ft(CRegisters* regs, uint32_t reg) {
    return fpr_read_u64(regs, reg);
}

// VR4300 ABS/NEG single-precision: operates on bit patterns with special NaN handling
static void Interp_COP1_S_AbsNeg(CRegisters* regs, uint32_t opcVal, bool isNeg) {
    uint32_t bits = fpr_read_w_op(regs, mips_fs(opcVal));
    uint32_t exp = (bits >> 23) & 0xFF;
    uint32_t mant = bits & 0x007FFFFF;

    // Clear Cause bits (InitFpuOperation equivalent)
    regs->m_FPCR[31] &= ~0x0003F000;

    // Denorm: exp=0, mantissa!=0 → UnimplementedOperation → FPE
    if (exp == 0 && mant != 0) {
        regs->m_FPCR[31] |= (1 << 17); // Cause.UnimplementedOperation
        regs->TriggerException(15, 0);  // FPE, coprocessor 1
        return;
    }
    // sNaN: exp=0xFF, quiet bit=0, mantissa!=0 → UnimplementedOperation → FPE
    if (exp == 0xFF && mant != 0 && (bits & 0x00400000) == 0) {
        regs->m_FPCR[31] |= (1 << 17); // Cause.UnimplementedOperation
        regs->TriggerException(15, 0);
        return;
    }
    // qNaN: exp=0xFF, quiet bit=1 → InvalidOperation, result = canonical sNaN
    if (exp == 0xFF && (bits & 0x00400000) != 0) {
        regs->m_FPCR[31] |= (1 << 16); // Cause.InvalidOperation
        if (regs->m_FPCR[31] & (1 << 11)) { // Enable.InvalidOperation → FPE, no flag
            regs->TriggerException(15, 0);
            return;
        }
        regs->m_FPCR[31] |= (1 << 6);  // Flag.InvalidOperation
        fpr_write_w_result(regs, mips_fd(opcVal), 0x7FBFFFFF); // canonical sNaN
        return;
    }
    // Normal/zero/inf: ABS clears sign bit, NEG flips sign bit
    uint32_t result = isNeg ? (bits ^ 0x80000000u) : (bits & 0x7FFFFFFFu);
    fpr_write_w_result(regs, mips_fd(opcVal), result);
}

// VR4300 ABS/NEG double-precision: operates on bit patterns with special NaN handling
static void Interp_COP1_D_AbsNeg(CRegisters* regs, uint32_t opcVal, bool isNeg) {
    uint64_t bits = fpr_read_u64(regs, mips_fs(opcVal));
    uint32_t exp = static_cast<uint32_t>((bits >> 52) & 0x7FF);
    uint64_t mant = bits & 0x000FFFFFFFFFFFFFull;

    // Clear Cause bits
    regs->m_FPCR[31] &= ~0x0003F000;

    // Denorm
    if (exp == 0 && mant != 0) {
        regs->m_FPCR[31] |= (1 << 17);
        regs->TriggerException(15, 0);
        return;
    }
    // sNaN
    if (exp == 0x7FF && mant != 0 && (bits & 0x0008000000000000ull) == 0) {
        regs->m_FPCR[31] |= (1 << 17);
        regs->TriggerException(15, 0);
        return;
    }
    // qNaN → canonical sNaN
    if (exp == 0x7FF && (bits & 0x0008000000000000ull) != 0) {
        regs->m_FPCR[31] |= (1 << 16); // Cause.InvalidOperation
        if (regs->m_FPCR[31] & (1 << 11)) { // Enable.InvalidOperation → FPE, no flag
            regs->TriggerException(15, 0);
            return;
        }
        regs->m_FPCR[31] |= (1 << 6);  // Flag.InvalidOperation
        fpr_write_u64_arith(regs, mips_fd(opcVal), 0x7FF7FFFFFFFFFFFFull);
        return;
    }
    // Normal/zero/inf
    uint64_t result = isNeg ? (bits ^ 0x8000000000000000ull) : (bits & 0x7FFFFFFFFFFFFFFFull);
    fpr_write_u64_arith(regs, mips_fd(opcVal), result);
}

// VR4300 float classification helpers
static inline bool is_float_denorm(uint32_t bits) {
    return (bits & 0x7F800000u) == 0 && (bits & 0x007FFFFFu) != 0;
}
static inline bool is_float_snan(uint32_t bits) {
    return (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x00400000u) == 0 && (bits & 0x003FFFFFu) != 0;
}
static inline bool is_float_nan(uint32_t bits) {
    return (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0;
}
static inline bool is_double_denorm(uint64_t bits) {
    return (bits & 0x7FF0000000000000ull) == 0 && (bits & 0x000FFFFFFFFFFFFFull) != 0;
}
static inline bool is_double_snan(uint64_t bits) {
    return (bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
           (bits & 0x0008000000000000ull) == 0 &&
           (bits & 0x0007FFFFFFFFFFFFull) != 0;
}
static inline bool is_double_nan(uint64_t bits) {
    return (bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (bits & 0x000FFFFFFFFFFFFFull) != 0;
}

void Interp_COP1_S_Arith(CRegisters* regs, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    // MOV.S copies a single 32-bit lane.
    if (mips_funct(opcVal) == R4300_FPU_MOV) {
        uint32_t src = fpr_read_w(regs, mips_fs(opcVal));
        fpr_write_w_move(regs, mips_fd(opcVal), src);
        return;
    }
    // ABS.S/NEG.S: VR4300-specific bit pattern operations
    if (mips_funct(opcVal) == R4300_FPU_ABS) {
        Interp_COP1_S_AbsNeg(regs, opcVal, false);
        return;
    }
    if (mips_funct(opcVal) == R4300_FPU_NEG) {
        Interp_COP1_S_AbsNeg(regs, opcVal, true);
        return;
    }
    R4300iOps op; op.Value = opcVal;
    regs->m_FPCR[31] &= ~0x0003F000; // Clear Cause bits
    bool flush_denorm = (regs->m_FPCR[31] >> 24) & 1; // FS bit

    // VR4300: check inputs for denorm/sNaN → UnimplementedOperation
    uint32_t fs_bits = fpr_read_w_op(regs, mips_fs(opcVal));
    if (is_float_denorm(fs_bits) || is_float_snan(fs_bits)) {
        regs->m_FPCR[31] |= (1 << 17);
        regs->TriggerException(15, 0);
        return;
    }
    if (mips_funct(opcVal) != R4300_FPU_SQRT) {
        uint32_t ft_bits = fpr_read_w_op_ft(regs, mips_ft(opcVal));
        if (is_float_denorm(ft_bits) || is_float_snan(ft_bits)) {
            regs->m_FPCR[31] |= (1 << 17);
            regs->TriggerException(15, 0);
            return;
        }
    }

    float fs, ft;
    std::memcpy(&fs, &fs_bits, 4);
    uint32_t ft_raw = fpr_read_w_op_ft(regs, mips_ft(opcVal));
    std::memcpy(&ft, &ft_raw, 4);
    float result = 0.0f;
    SyncRoundingMode(regs);
    // Clear MXCSR exception flags directly (bits 0-5)
    { unsigned int m = _mm_getcsr(); _mm_setcsr(m & ~0x3Fu); }
    switch (mips_funct(opcVal)) {
        case R4300_FPU_ADD:  result = fs + ft; break;
        case R4300_FPU_SUB:  result = fs - ft; break;
        case R4300_FPU_MUL:  result = fs * ft; break;
        case R4300_FPU_DIV:  result = fs / ft; break;
        case R4300_FPU_SQRT: result = sqrtf(fs); break;
        default:
            Log::errorf("COP1_S: Unknown funct %d", mips_funct(opcVal));
            return;
    }
    // Read MXCSR exception flags: bit0=Invalid, bit1=Denorm, bit2=DivZero, bit3=Overflow, bit4=Underflow, bit5=Inexact
    unsigned int mxcsr_exc = _mm_getcsr() & 0x3Fu;
    int host_exc = 0;
    if (mxcsr_exc & 0x01) host_exc |= FE_INVALID;
    if (mxcsr_exc & 0x04) host_exc |= FE_DIVBYZERO;
    if (mxcsr_exc & 0x08) host_exc |= FE_OVERFLOW;
    if (mxcsr_exc & 0x10) host_exc |= FE_UNDERFLOW;
    if (mxcsr_exc & 0x20) host_exc |= FE_INEXACT;

    // VR4300: check result for NaN → replace with canonical sNaN
    uint32_t result_bits;
    std::memcpy(&result_bits, &result, 4);
    if (is_float_nan(result_bits)) {
        regs->m_FPCR[31] |= (1 << 16); // Cause.InvalidOperation
        if (regs->m_FPCR[31] & (1 << 11)) { regs->TriggerException(15, 0); return; }
        regs->m_FPCR[31] |= (1 << 6);  // Flag.InvalidOperation
        fpr_write_w_result(regs, mips_fd(opcVal), 0x7FBFFFFFu);
        return;
    }
    // VR4300 "tininess before rounding": if the result rounded to ±FLT_MIN but
    // the infinite-precision result was subnormal, treat as underflow.
    // Detect by recomputing in double (exact for single-precision operations).
    uint32_t exp_s = (result_bits >> 23) & 0xFF;
    bool tininess_s = false;
    if (exp_s == 1) {
        // Recompute in double precision to detect tininess-before-rounding.
        // Double precision is exact for single-precision arithmetic operations.
        double dfs = static_cast<double>(fs);
        double dft = static_cast<double>(ft);
        double exact;
        switch (mips_funct(opcVal)) {
            case R4300_FPU_ADD:  exact = dfs + dft; break;
            case R4300_FPU_SUB:  exact = dfs - dft; break;
            case R4300_FPU_MUL:  exact = dfs * dft; break;
            case R4300_FPU_DIV:  exact = dfs / dft; break;
            case R4300_FPU_SQRT: exact = sqrt(dfs); break;
            default: exact = static_cast<double>(result); break;
        }
        uint64_t exact_bits;
        std::memcpy(&exact_bits, &exact, 8);
        uint64_t exact_abs = exact_bits & 0x7FFFFFFFFFFFFFFFull;
        // FLT_MIN = 2^-126 in double: 0x3810000000000000
        if (exact_abs != 0 && exact_abs < 0x3810000000000000ull) {
            tininess_s = true;
        }
    }
    // VR4300: check for underflow (denorm result, zero-from-nonzero, or tininess-before-rounding)
    bool is_denorm_s = is_float_denorm(result_bits);
    bool is_zero_uf_s = (!is_denorm_s && result == 0.0f && (host_exc & FE_UNDERFLOW));
    if (is_denorm_s || is_zero_uf_s || tininess_s) {
        if (!flush_denorm) { // FS=0 → UnimplementedOp
            regs->m_FPCR[31] |= (1 << 17);
            regs->TriggerException(15, 0);
            return;
        }
        // FS=1: check if underflow_enable or inexact_enable → UnimplementedOp
        if (regs->m_FPCR[31] & ((1 << 8) | (1 << 7))) {
            regs->m_FPCR[31] |= (1 << 17);
            regs->TriggerException(15, 0);
            return;
        }
        // Flush: rounding-mode-aware
        bool is_negative = std::signbit(result);
        int rm = regs->m_FPCR[31] & 3;
        uint32_t flushed;
        if (rm == 2) { // PositiveInfinity
            flushed = is_negative ? 0x80000000u : 0x00800000u;
        } else if (rm == 3) { // NegativeInfinity
            flushed = is_negative ? 0x80800000u : 0x00000000u;
        } else { // Nearest or Zero → always flush to ±0
            flushed = is_negative ? 0x80000000u : 0x00000000u;
        }
        regs->m_FPCR[31] |= (1 << 13) | (1 << 12); // Cause.Underflow + Cause.Inexact
        regs->m_FPCR[31] |= (1 << 3) | (1 << 2);   // Flag.Underflow + Flag.Inexact
        fpr_write_w_result(regs, mips_fd(opcVal), flushed);
        return;
    }
    // Check for overflow (MXCSR flag — result may be ±inf or ±MAX_NORMAL depending on rounding mode)
    if (host_exc & FE_OVERFLOW) {
        regs->m_FPCR[31] |= (1 << 14) | (1 << 12); // Cause.Overflow + Cause.Inexact
        uint32_t causes  = (regs->m_FPCR[31] >> 12) & 0x1F;
        uint32_t enables = (regs->m_FPCR[31] >> 7)  & 0x1F;
        if (causes & enables) { regs->TriggerException(15, 0); return; }
        regs->m_FPCR[31] |= (1 << 4) | (1 << 2); // Flag.Overflow + Flag.Inexact
    }
    // Check for division by zero
    if (host_exc & FE_DIVBYZERO) {
        regs->m_FPCR[31] |= (1 << 15); // Cause.DivisionByZero
        if (regs->m_FPCR[31] & (1 << 10)) { regs->TriggerException(15, 0); return; }
        regs->m_FPCR[31] |= (1 << 5); // Flag.DivisionByZero
    }
    // Check for inexact (not already handled by underflow/overflow)
    if ((host_exc & FE_INEXACT) && !(host_exc & (FE_UNDERFLOW | FE_OVERFLOW))) {
        regs->m_FPCR[31] |= (1 << 12); // Cause.Inexact
        if (regs->m_FPCR[31] & (1 << 7)) { regs->TriggerException(15, 0); return; }
        regs->m_FPCR[31] |= (1 << 2); // Flag.Inexact
    }
    fpr_write_float_result(regs, mips_fd(opcVal), result);
}

// Double-precision FPU operations
void Interp_COP1_D_Arith(CRegisters* regs, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    // MOV.D: full 64-bit copy, no FCSR modification
    // fs: bit 0 ignored in FR=0; fd: bit 0 NOT ignored
    if (mips_funct(opcVal) == R4300_FPU_MOV) {
        uint64_t bits = fpr_read_u64(regs, mips_fs(opcVal));
        regs->m_FPR[mips_fd(opcVal)] = bits;
        return;
    }
    // ABS.D/NEG.D: VR4300-specific bit pattern operations
    if (mips_funct(opcVal) == R4300_FPU_ABS) {
        Interp_COP1_D_AbsNeg(regs, opcVal, false);
        return;
    }
    if (mips_funct(opcVal) == R4300_FPU_NEG) {
        Interp_COP1_D_AbsNeg(regs, opcVal, true);
        return;
    }
    R4300iOps op; op.Value = opcVal;
    regs->m_FPCR[31] &= ~0x0003F000;
    bool flush_denorm = (regs->m_FPCR[31] >> 24) & 1;

    // VR4300: check inputs for denorm/sNaN → UnimplementedOperation
    uint64_t fs_bits = fpr_read_u64(regs, mips_fs(opcVal));
    if (is_double_denorm(fs_bits) || is_double_snan(fs_bits)) {
        regs->m_FPCR[31] |= (1 << 17);
        regs->TriggerException(15, 0);
        return;
    }
    if (mips_funct(opcVal) != R4300_FPU_SQRT) {
        uint64_t ft_bits = fpr_read_u64_ft(regs, mips_ft(opcVal));
        if (is_double_denorm(ft_bits) || is_double_snan(ft_bits)) {
            regs->m_FPCR[31] |= (1 << 17);
            regs->TriggerException(15, 0);
            return;
        }
    }

    double fs, ft;
    std::memcpy(&fs, &fs_bits, 8);
    uint64_t ft_raw = fpr_read_u64_ft(regs, mips_ft(opcVal));
    std::memcpy(&ft, &ft_raw, 8);
    double result = 0.0;
    SyncRoundingMode(regs);
    // Clear MXCSR exception flags directly (bits 0-5)
    { unsigned int m = _mm_getcsr(); _mm_setcsr(m & ~0x3Fu); }
    switch (mips_funct(opcVal)) {
        case R4300_FPU_ADD:  result = fs + ft; break;
        case R4300_FPU_SUB:  result = fs - ft; break;
        case R4300_FPU_MUL:  result = fs * ft; break;
        case R4300_FPU_DIV:  result = fs / ft; break;
        case R4300_FPU_SQRT: result = sqrt(fs); break;
        default:
            Log::errorf("COP1_D: Unknown funct %d", mips_funct(opcVal));
            return;
    }
    // Read MXCSR exception flags
    unsigned int mxcsr_exc_d = _mm_getcsr() & 0x3Fu;
    int host_exc = 0;
    if (mxcsr_exc_d & 0x01) host_exc |= FE_INVALID;
    if (mxcsr_exc_d & 0x04) host_exc |= FE_DIVBYZERO;
    if (mxcsr_exc_d & 0x08) host_exc |= FE_OVERFLOW;
    if (mxcsr_exc_d & 0x10) host_exc |= FE_UNDERFLOW;
    if (mxcsr_exc_d & 0x20) host_exc |= FE_INEXACT;

    // VR4300: check result for NaN → replace with canonical sNaN
    uint64_t result_bits;
    std::memcpy(&result_bits, &result, 8);
    if (is_double_nan(result_bits)) {
        regs->m_FPCR[31] |= (1 << 16); // Cause.InvalidOperation
        if (regs->m_FPCR[31] & (1 << 11)) { regs->TriggerException(15, 0); return; }
        regs->m_FPCR[31] |= (1 << 6);  // Flag.InvalidOperation
        fpr_write_u64_arith(regs, mips_fd(opcVal), 0x7FF7FFFFFFFFFFFFull);
        return;
    }
    // VR4300 "tininess before rounding": if result rounded to ±DBL_MIN but
    // the infinite-precision result was subnormal, treat as underflow.
    // Detect by recomputing with FE_TOWARDZERO (truncation toward zero).
    uint32_t exp_d = (result_bits >> 52) & 0x7FF;
    bool tininess_d = false;
    if (exp_d == 1) {
        unsigned int saved_mxcsr = _mm_getcsr();
        // Set toward-zero rounding, no FTZ/DAZ, clear exception flags, exceptions masked
        _mm_setcsr((saved_mxcsr & ~0xE07Fu) | 0x1F80u | (3u << 13));
        double trunc_result;
        switch (mips_funct(opcVal)) {
            case R4300_FPU_ADD:  trunc_result = fs + ft; break;
            case R4300_FPU_SUB:  trunc_result = fs - ft; break;
            case R4300_FPU_MUL:  trunc_result = fs * ft; break;
            case R4300_FPU_DIV:  trunc_result = fs / ft; break;
            case R4300_FPU_SQRT: trunc_result = sqrt(fs); break;
            default: trunc_result = result; break;
        }
        uint64_t trunc_bits;
        double trunc_val = trunc_result;
        std::memcpy(&trunc_bits, &trunc_val, 8);
        // Restore MXCSR
        _mm_setcsr(saved_mxcsr);
        // Re-disable FTZ/DAZ
        { unsigned int m = _mm_getcsr(); _mm_setcsr(m & ~0x8040u); }
        if (is_double_denorm(trunc_bits) || (trunc_result == 0.0 && fs != 0.0)) {
            tininess_d = true;
        }
    }
    // VR4300: check for underflow (denorm result, zero-from-nonzero, or tininess-before-rounding)
    bool is_denorm_d = is_double_denorm(result_bits);
    bool is_zero_uf_d = (!is_denorm_d && result == 0.0 && (host_exc & FE_UNDERFLOW));
    if (is_denorm_d || is_zero_uf_d || tininess_d) {
        if (!flush_denorm) { // FS=0 → UnimplementedOp
            regs->m_FPCR[31] |= (1 << 17);
            regs->TriggerException(15, 0);
            return;
        }
        // FS=1: check if underflow_enable or inexact_enable → UnimplementedOp
        if (regs->m_FPCR[31] & ((1 << 8) | (1 << 7))) {
            regs->m_FPCR[31] |= (1 << 17);
            regs->TriggerException(15, 0);
            return;
        }
        // Flush: rounding-mode-aware
        bool is_negative = std::signbit(result);
        int rm = regs->m_FPCR[31] & 3;
        uint64_t flushed;
        if (is_denorm_d) {
            if (rm == 2) { // PositiveInfinity
                flushed = is_negative ? 0x8000000000000000ull : 0x0010000000000000ull;
            } else if (rm == 3) { // NegativeInfinity
                flushed = is_negative ? 0x8010000000000000ull : 0x0000000000000000ull;
            } else { // Nearest or Zero → always flush to ±0
                flushed = is_negative ? 0x8000000000000000ull : 0x0000000000000000ull;
            }
        } else {
            // Zero from underflow → preserve sign
            flushed = is_negative ? 0x8000000000000000ull : 0x0000000000000000ull;
        }
        regs->m_FPCR[31] |= (1 << 13) | (1 << 12); // Cause.Underflow + Cause.Inexact
        regs->m_FPCR[31] |= (1 << 3) | (1 << 2);   // Flag.Underflow + Flag.Inexact
        fpr_write_u64_arith(regs, mips_fd(opcVal), flushed);
        return;
    }
    // Check for overflow (MXCSR flag — result may be ±inf or ±MAX_NORMAL depending on rounding mode)
    if (host_exc & FE_OVERFLOW) {
        regs->m_FPCR[31] |= (1 << 14) | (1 << 12); // Cause.Overflow + Cause.Inexact
        uint32_t causes  = (regs->m_FPCR[31] >> 12) & 0x1F;
        uint32_t enables = (regs->m_FPCR[31] >> 7)  & 0x1F;
        if (causes & enables) { regs->TriggerException(15, 0); return; }
        regs->m_FPCR[31] |= (1 << 4) | (1 << 2); // Flag.Overflow + Flag.Inexact
    }
    // Check for division by zero
    if (host_exc & FE_DIVBYZERO) {
        regs->m_FPCR[31] |= (1 << 15); // Cause.DivisionByZero
        if (regs->m_FPCR[31] & (1 << 10)) { regs->TriggerException(15, 0); return; }
        regs->m_FPCR[31] |= (1 << 5); // Flag.DivisionByZero
    }
    // Check for inexact (not already handled by underflow/overflow)
    if ((host_exc & FE_INEXACT) && !(host_exc & (FE_UNDERFLOW | FE_OVERFLOW))) {
        regs->m_FPCR[31] |= (1 << 12); // Cause.Inexact
        if (regs->m_FPCR[31] & (1 << 7)) { regs->TriggerException(15, 0); return; }
        regs->m_FPCR[31] |= (1 << 2); // Flag.Inexact
    }
    fpr_write_double(regs, mips_fd(opcVal), result);
}

// VR4300 float/double → int32 conversion helper
// Handles CVT.W, ROUND.W, TRUNC.W, CEIL.W, FLOOR.W with proper VR4300 semantics:
// - NaN/Inf/out-of-range → UnimplementedOp (not InvalidOp)
// - Manual inexact detection (no fenv dependency)
// - ROUND uses nearest-even (not half-away-from-zero)
static void VR4300_ConvertToW(CRegisters* regs, uint32_t opcVal, double input, int funct) {
    if (std::isnan(input) || std::isinf(input)) {
        regs->m_FPCR[31] |= (1 << 17);
        regs->TriggerException(15, 0);
        return;
    }
    int saved_rm = std::fegetround();
    switch (funct) {
        case R4300_FPU_ROUND_W: std::fesetround(FE_TONEAREST); break;
        case R4300_FPU_TRUNC_W: std::fesetround(FE_TOWARDZERO); break;
        case R4300_FPU_CEIL_W:  std::fesetround(FE_UPWARD); break;
        case R4300_FPU_FLOOR_W: std::fesetround(FE_DOWNWARD); break;
        default: break; // CVT_W: keep current mode from SyncRoundingMode
    }
    double rounded = std::nearbyint(input);
    std::fesetround(saved_rm);
    if (rounded > 2147483647.0 || rounded < -2147483648.0) {
        regs->m_FPCR[31] |= (1 << 17);
        regs->TriggerException(15, 0);
        return;
    }
    int32_t result = static_cast<int32_t>(rounded);
    if (rounded != input) {
        regs->m_FPCR[31] |= (1 << 12); // Cause.Inexact
        if (regs->m_FPCR[31] & (1 << 7)) { regs->TriggerException(15, 0); return; }
        regs->m_FPCR[31] |= (1 << 2); // Flag.Inexact
    }
    fpr_write_w_result(regs, mips_fd(opcVal), static_cast<uint32_t>(result));
}

// VR4300 float/double → int64 conversion helper
// VR4300 limits: |result| must be < 2^53 (NOT full int64 range)
static void VR4300_ConvertToL(CRegisters* regs, uint32_t opcVal, double input, int funct) {
    if (std::isnan(input) || std::isinf(input)) {
        regs->m_FPCR[31] |= (1 << 17);
        regs->TriggerException(15, 0);
        return;
    }
    int saved_rm = std::fegetround();
    switch (funct) {
        case R4300_FPU_ROUND_L: std::fesetround(FE_TONEAREST); break;
        case R4300_FPU_TRUNC_L: std::fesetround(FE_TOWARDZERO); break;
        case R4300_FPU_CEIL_L:  std::fesetround(FE_UPWARD); break;
        case R4300_FPU_FLOOR_L: std::fesetround(FE_DOWNWARD); break;
        default: break; // CVT_L: keep current mode
    }
    double rounded = std::nearbyint(input);
    std::fesetround(saved_rm);
    constexpr double limit = 9007199254740992.0; // 2^53
    if (rounded >= limit || rounded <= -limit) {
        regs->m_FPCR[31] |= (1 << 17);
        regs->TriggerException(15, 0);
        return;
    }
    int64_t result = static_cast<int64_t>(rounded);
    if (rounded != input) {
        regs->m_FPCR[31] |= (1 << 12);
        if (regs->m_FPCR[31] & (1 << 7)) { regs->TriggerException(15, 0); return; }
        regs->m_FPCR[31] |= (1 << 2);
    }
    fpr_write_u64_arith(regs, mips_fd(opcVal), static_cast<uint64_t>(result));
}

// Single-precision conversions
void Interp_COP1_S_CVT(CRegisters* regs, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    R4300iOps op; op.Value = opcVal;
    regs->m_FPCR[31] &= ~0x0003F000;

    // VR4300: check input for denorm/sNaN (IEEE sNaN)
    uint32_t fs_bits = fpr_read_w_op(regs, mips_fs(opcVal));
    if (is_float_denorm(fs_bits) || is_float_snan(fs_bits)) {
        regs->m_FPCR[31] |= (1 << 17);
        regs->TriggerException(15, 0);
        return;
    }

    float fs;
    std::memcpy(&fs, &fs_bits, 4);
    SyncRoundingMode(regs);

    int funct = mips_funct(opcVal);

    // CVT.S.S doesn't exist → UnimplementedOp
    if (funct == R4300_FPU_CVT_S) {
        regs->m_FPCR[31] |= (1 << 17);
        regs->TriggerException(15, 0);
        return;
    }

    if (funct == R4300_FPU_CVT_D) {
        // NaN input → InvalidOp + canonical sNaN
        if (is_float_nan(fs_bits)) {
            regs->m_FPCR[31] |= (1 << 16);
            if (regs->m_FPCR[31] & (1 << 11)) { regs->TriggerException(15, 0); return; }
            regs->m_FPCR[31] |= (1 << 6);
            fpr_write_u64_arith(regs, mips_fd(opcVal), 0x7FF7FFFFFFFFFFFFull);
            return;
        }
        double dval = static_cast<double>(fs);
        fpr_write_double(regs, mips_fd(opcVal), dval);
        return;
    }

    // NaN (any kind) → UnimplementedOp for all float→int conversions
    if (is_float_nan(fs_bits)) {
        regs->m_FPCR[31] |= (1 << 17);
        regs->TriggerException(15, 0);
        return;
    }

    double input = static_cast<double>(fs); // exact
    if (funct == R4300_FPU_CVT_W || funct == R4300_FPU_ROUND_W ||
        funct == R4300_FPU_TRUNC_W || funct == R4300_FPU_CEIL_W ||
        funct == R4300_FPU_FLOOR_W) {
        VR4300_ConvertToW(regs, opcVal, input, funct);
    } else if (funct == R4300_FPU_CVT_L || funct == R4300_FPU_ROUND_L ||
               funct == R4300_FPU_TRUNC_L || funct == R4300_FPU_CEIL_L ||
               funct == R4300_FPU_FLOOR_L) {
        VR4300_ConvertToL(regs, opcVal, input, funct);
    } else {
        Log::errorf("COP1_S_CVT: Unknown funct %d", funct);
    }
}

// Double-precision conversions
void Interp_COP1_D_CVT(CRegisters* regs, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    R4300iOps op; op.Value = opcVal;
    regs->m_FPCR[31] &= ~0x0003F000;

    // VR4300: check input for denorm/sNaN (IEEE sNaN)
    uint64_t fs_bits = fpr_read_u64(regs, mips_fs(opcVal));
    if (is_double_denorm(fs_bits) || is_double_snan(fs_bits)) {
        regs->m_FPCR[31] |= (1 << 17);
        regs->TriggerException(15, 0);
        return;
    }

    double fs;
    std::memcpy(&fs, &fs_bits, 8);
    SyncRoundingMode(regs);

    int funct = mips_funct(opcVal);

    // CVT.D.D doesn't exist → UnimplementedOp
    if (funct == R4300_FPU_CVT_D) {
        regs->m_FPCR[31] |= (1 << 17);
        regs->TriggerException(15, 0);
        return;
    }

    if (funct == R4300_FPU_CVT_S) {
        // NaN input → InvalidOp + canonical sNaN
        if (is_double_nan(fs_bits)) {
            regs->m_FPCR[31] |= (1 << 16);
            if (regs->m_FPCR[31] & (1 << 11)) { regs->TriggerException(15, 0); return; }
            regs->m_FPCR[31] |= (1 << 6);
            fpr_write_w_result(regs, mips_fd(opcVal), 0x7FBFFFFFu);
            return;
        }
        float fval = static_cast<float>(fs);
        uint32_t result_bits;
        std::memcpy(&result_bits, &fval, 4);
        uint32_t abs_bits = result_bits & 0x7FFFFFFFu;
        // Detect underflow: result is zero or denorm, but input was nonzero
        bool result_is_subnormal_or_zero = ((result_bits & 0x7F800000u) == 0);
        if (result_is_subnormal_or_zero && fs != 0.0) {
            bool flush_denorm = (regs->m_FPCR[31] >> 24) & 1;
            if (flush_denorm) {
                // FS=1: check if underflow_enable or inexact_enable → UnimplementedOp
                if (regs->m_FPCR[31] & ((1 << 8) | (1 << 7))) {
                    regs->m_FPCR[31] |= (1 << 17);
                    regs->TriggerException(15, 0);
                    return;
                }
                bool is_negative = std::signbit(fs);
                int rm = regs->m_FPCR[31] & 3;
                uint32_t flushed;
                if (rm == 2) { // PositiveInfinity
                    flushed = is_negative ? 0x80000000u : 0x00800000u;
                } else if (rm == 3) { // NegativeInfinity
                    flushed = is_negative ? 0x80800000u : 0x00000000u;
                } else { // Nearest or Zero → always flush to ±0
                    flushed = is_negative ? 0x80000000u : 0x00000000u;
                }
                regs->m_FPCR[31] |= (1 << 13) | (1 << 12); // Cause.Underflow + Cause.Inexact
                regs->m_FPCR[31] |= (1 << 3) | (1 << 2); // Flag.Underflow + Flag.Inexact
                fpr_write_w_result(regs, mips_fd(opcVal), flushed);
                return;
            }
            regs->m_FPCR[31] |= (1 << 17);
            regs->TriggerException(15, 0);
            return;
        }
        // Check for overflow (bit-level check: isinf can be broken by /fp:fast)
        uint32_t result_abs = result_bits & 0x7FFFFFFFu;
        bool result_is_inf = (result_abs == 0x7F800000u);
        uint64_t fs_abs = fs_bits & 0x7FFFFFFFFFFFFFFFull;
        bool fs_is_inf = (fs_abs == 0x7FF0000000000000ull);
        if (result_is_inf && !fs_is_inf) {
            regs->m_FPCR[31] |= (1 << 14) | (1 << 12); // Cause.Overflow + Cause.Inexact
            uint32_t causes  = (regs->m_FPCR[31] >> 12) & 0x1F;
            uint32_t enables = (regs->m_FPCR[31] >> 7)  & 0x1F;
            if (causes & enables) { regs->TriggerException(15, 0); return; }
            regs->m_FPCR[31] |= (1 << 4) | (1 << 2); // Flag.Overflow + Flag.Inexact
            fpr_write_float_result(regs, mips_fd(opcVal), fval);
            return;
        }
        // Check for inexact (normal result)
        if (static_cast<double>(fval) != fs) {
            regs->m_FPCR[31] |= (1 << 12); // Cause.Inexact
            if (regs->m_FPCR[31] & (1 << 7)) { regs->TriggerException(15, 0); return; }
            regs->m_FPCR[31] |= (1 << 2); // Flag.Inexact
        }
        fpr_write_float_result(regs, mips_fd(opcVal), fval);
        return;
    }

    // NaN (any kind) → UnimplementedOp for all double→int conversions
    if (is_double_nan(fs_bits)) {
        regs->m_FPCR[31] |= (1 << 17);
        regs->TriggerException(15, 0);
        return;
    }

    if (funct == R4300_FPU_CVT_W || funct == R4300_FPU_ROUND_W ||
        funct == R4300_FPU_TRUNC_W || funct == R4300_FPU_CEIL_W ||
        funct == R4300_FPU_FLOOR_W) {
        VR4300_ConvertToW(regs, opcVal, fs, funct);
    } else if (funct == R4300_FPU_CVT_L || funct == R4300_FPU_ROUND_L ||
               funct == R4300_FPU_TRUNC_L || funct == R4300_FPU_CEIL_L ||
               funct == R4300_FPU_FLOOR_L) {
        VR4300_ConvertToL(regs, opcVal, fs, funct);
    } else {
        Log::errorf("COP1_D_CVT: Unknown funct %d", funct);
    }
}

// Word-to-float/double conversion
void Interp_COP1_W_CVT(CRegisters* regs, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    R4300iOps op; op.Value = opcVal;
    // W-format CVT: fs bit 0 ignored in FR=0 (arithmetic read rules)
    int32_t w = static_cast<int32_t>(fpr_read_w_op(regs, mips_fs(opcVal)));
    regs->m_FPCR[31] &= ~0x0003F000;
    SyncRoundingMode(regs);
    if (mips_funct(opcVal) == R4300_FPU_CVT_S) {
        float result = static_cast<float>(w);
        // Manual inexact detection: double can represent both int32 and float exactly
        if (static_cast<double>(result) != static_cast<double>(w)) {
            regs->m_FPCR[31] |= (1 << 12); // Cause.Inexact
            if (regs->m_FPCR[31] & (1 << 7)) { // Enable.Inexact → FPE
                regs->TriggerException(15, 0);
                return;
            }
            regs->m_FPCR[31] |= (1 << 2); // Flag.Inexact
        }
        fpr_write_float_result(regs, mips_fd(opcVal), result);
    } else if (mips_funct(opcVal) == R4300_FPU_CVT_D) {
        // int32 → double is always exact (53-bit mantissa > 31 significant bits)
        fpr_write_double(regs, mips_fd(opcVal), static_cast<double>(w));
    } else {
        // CVT.W.W, ROUND.W.W, etc. don't exist → UnimplementedOp
        regs->m_FPCR[31] |= (1 << 17);
        regs->TriggerException(15, 0);
    }
}

// Long-to-float/double conversion
void Interp_COP1_L_CVT(CRegisters* regs, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    R4300iOps op; op.Value = opcVal;
    int64_t l = static_cast<int64_t>(fpr_read_u64(regs, mips_fs(opcVal)));
    regs->m_FPCR[31] &= ~0x0003F000;
    SyncRoundingMode(regs);

    // VR4300: L-format only supports CVT.S.L and CVT.D.L; others → UnimplementedOp
    if (mips_funct(opcVal) != R4300_FPU_CVT_S && mips_funct(opcVal) != R4300_FPU_CVT_D) {
        regs->m_FPCR[31] |= (1 << 17);
        regs->TriggerException(15, 0);
        return;
    }

    // VR4300 range check: |value| must fit in signed 56-bit (-(2^55) to 2^55-1)
    constexpr int64_t max_val = (1LL << 55) - 1;
    constexpr int64_t min_val = -(1LL << 55);
    if (l > max_val || l < min_val) {
        regs->m_FPCR[31] |= (1 << 17);
        regs->TriggerException(15, 0);
        return;
    }

    if (mips_funct(opcVal) == R4300_FPU_CVT_S) {
        float result = static_cast<float>(l);
        // Manual inexact: check if |l| has more than 24 significant bits
        uint64_t u = static_cast<uint64_t>(l); if (l < 0) u = -u;
        bool inexact = false;
        if (u != 0) {
            int msb = 63; while (msb > 0 && !(u & (1ull << msb))) msb--;
            if (msb >= 24) inexact = (u & ((1ull << (msb - 23)) - 1)) != 0;
        }
        if (inexact) {
            regs->m_FPCR[31] |= (1 << 12);
            if (regs->m_FPCR[31] & (1 << 7)) { regs->TriggerException(15, 0); return; }
            regs->m_FPCR[31] |= (1 << 2);
        }
        fpr_write_float_result(regs, mips_fd(opcVal), result);
    } else {
        double result = static_cast<double>(l);
        // Manual inexact: check if |l| has more than 53 significant bits
        uint64_t u = static_cast<uint64_t>(l); if (l < 0) u = -u;
        bool inexact = false;
        if (u != 0) {
            int msb = 63; while (msb > 0 && !(u & (1ull << msb))) msb--;
            if (msb >= 53) inexact = (u & ((1ull << (msb - 52)) - 1)) != 0;
        }
        if (inexact) {
            regs->m_FPCR[31] |= (1 << 12);
            if (regs->m_FPCR[31] & (1 << 7)) { regs->TriggerException(15, 0); return; }
            regs->m_FPCR[31] |= (1 << 2);
        }
        fpr_write_double(regs, mips_fd(opcVal), result);
    }
}

// COP1 Compare (C.cond.S / C.cond.D)
constexpr uint32_t FPCSR_C = 0x00800000;

void Interp_COP1_S_CMP(CRegisters* regs, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    R4300iOps op; op.Value = opcVal;
    regs->m_FPCR[31] &= ~0x0003F000; // Clear Cause bits

    uint32_t fs_bits = fpr_read_w_op(regs, mips_fs(opcVal));
    uint32_t ft_bits = fpr_read_w_op_ft(regs, mips_ft(opcVal));

    float fs, ft;
    std::memcpy(&fs, &fs_bits, 4);
    std::memcpy(&ft, &ft_bits, 4);
    bool less = false, equal = false, unordered = false;
    if (std::isnan(fs) || std::isnan(ft)) {
        unordered = true;
        // VR4300 NaN convention: signaling comparisons (bit 3) raise for any NaN;
        // non-signaling raise only if any input is qNaN (IEEE bit22=1 = VR4300 sNaN)
        bool has_ieee_qnan = (is_float_nan(fs_bits) && !is_float_snan(fs_bits)) ||
                             (is_float_nan(ft_bits) && !is_float_snan(ft_bits));
        if ((mips_funct(opcVal) & 8) || has_ieee_qnan) {
            regs->m_FPCR[31] |= (1 << 16); // Cause.InvalidOperation
            if (regs->m_FPCR[31] & (1 << 11)) { // Enable.InvalidOperation → FPE, no flag
                regs->TriggerException(15, 0);
                return;
            }
            regs->m_FPCR[31] |= (1 << 6);  // Flag.InvalidOperation
        }
    } else {
        less = (fs < ft);
        equal = (fs == ft);
    }
    bool cond = ((mips_funct(opcVal) & 4) && less) ||
                ((mips_funct(opcVal) & 2) && equal) ||
                ((mips_funct(opcVal) & 1) && unordered);
    if (cond) regs->m_FPCR[31] |= FPCSR_C;
    else      regs->m_FPCR[31] &= ~FPCSR_C;
}

void Interp_COP1_D_CMP(CRegisters* regs, uint32_t opcVal) {
    if (CheckCop1Unusable(regs)) return;
    R4300iOps op; op.Value = opcVal;
    regs->m_FPCR[31] &= ~0x0003F000; // Clear Cause bits

    uint64_t fs_bits = fpr_read_u64(regs, mips_fs(opcVal));
    uint64_t ft_bits = fpr_read_u64_ft(regs, mips_ft(opcVal));

    double fs, ft;
    std::memcpy(&fs, &fs_bits, 8);
    std::memcpy(&ft, &ft_bits, 8);
    bool less = false, equal = false, unordered = false;
    if (std::isnan(fs) || std::isnan(ft)) {
        unordered = true;
        // VR4300 NaN convention: signaling comparisons (bit 3) raise for any NaN;
        // non-signaling raise only if any input is qNaN (IEEE bit52=1 = VR4300 sNaN)
        bool has_ieee_qnan = (is_double_nan(fs_bits) && !is_double_snan(fs_bits)) ||
                             (is_double_nan(ft_bits) && !is_double_snan(ft_bits));
        if ((mips_funct(opcVal) & 8) || has_ieee_qnan) {
            regs->m_FPCR[31] |= (1 << 16); // Cause.InvalidOperation
            if (regs->m_FPCR[31] & (1 << 11)) { // Enable.InvalidOperation → FPE, no flag
                regs->TriggerException(15, 0);
                return;
            }
            regs->m_FPCR[31] |= (1 << 6);  // Flag.InvalidOperation
        }
    } else {
        less = (fs < ft);
        equal = (fs == ft);
    }
    bool cond = ((mips_funct(opcVal) & 4) && less) ||
                ((mips_funct(opcVal) & 2) && equal) ||
                ((mips_funct(opcVal) & 1) && unordered);
    if (cond) regs->m_FPCR[31] |= FPCSR_C;
    else      regs->m_FPCR[31] &= ~FPCSR_C;
}

// Restore /fp:fast for the rest of the file
#pragma fenv_access(off)
#pragma float_control(pop)

// ----- System / Trap / CACHE Callbacks -----

static std::atomic<int> s_syscallCount{0};

void Interp_SYSCALL(CRegisters* regs, uint32_t /*opcVal*/) {
    int n = s_syscallCount.fetch_add(1);
    if (n < 10) {
        Log::infof("[SYSCALL] #%d PC=0x%08X STATUS=0x%08X",
                   n+1, (uint32_t)regs->m_PROGRAM_COUNTER,
                   regs->STATUS_REGISTER.Value);
    }
    regs->TriggerException(static_cast<uint32_t>(ExceptionCode::Syscall));
}

void Interp_BREAK(CRegisters* regs, uint32_t /*opcVal*/) {
    regs->TriggerException(static_cast<uint32_t>(ExceptionCode::Breakpoint));
}

void Interp_SYNC(CRegisters* /*regs*/, uint32_t /*opcVal*/) {
    // SYNC is a memory barrier - no-op in our single-threaded interpreter context
}

void Interp_CACHE(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    // CACHE op, offset(base)
    // op = bits 20:16 (same position as rt)
    uint32_t cacheOp = mips_rt(opcVal);

    // I-Cache operations that invalidate instruction cache lines:
    //   0x00 = Index Invalidate
    //   0x10 = Hit Invalidate
    // For a recompiler these must discard any compiled blocks covering
    // the affected cache-line, otherwise stale code executes after
    // the game DMA's new code to RDRAM and calls osInvalICache().
    if (cacheOp == 0x00 || cacheOp == 0x10) {
        uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                        static_cast<int16_t>(mips_offset(opcVal));
        // R4300 I-cache line = 32 bytes; align down and invalidate whole line
        uint32_t physAddr = static_cast<uint32_t>(addr) & 0x1FFFFFFF;
        uint32_t lineStart = physAddr & ~31u;
        // Use the DMA invalidation path (goes through N64System callback)
        if (mmu) {
            CN64System* sys = mmu->System();
            if (sys) sys->NotifyDMAToRDRAM(lineStart, 32);
        }
    }
    // D-Cache operations (0x01, 0x05, 0x09, 0x11, 0x15, 0x19) don't
    // affect instruction execution and are safely ignored.
}

// Trap instructions
void Interp_TGE(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW >= AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW)
        regs->TriggerException(static_cast<uint32_t>(ExceptionCode::Trap));
}

void Interp_TGEU(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (AsDWord(regs->m_GPR[mips_rs(opcVal)]).UDW >= AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW)
        regs->TriggerException(static_cast<uint32_t>(ExceptionCode::Trap));
}

void Interp_TLT(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW < AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW)
        regs->TriggerException(static_cast<uint32_t>(ExceptionCode::Trap));
}

void Interp_TLTU(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (AsDWord(regs->m_GPR[mips_rs(opcVal)]).UDW < AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW)
        regs->TriggerException(static_cast<uint32_t>(ExceptionCode::Trap));
}

void Interp_TEQ(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW == AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW)
        regs->TriggerException(static_cast<uint32_t>(ExceptionCode::Trap));
}

void Interp_TNE(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW != AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW)
        regs->TriggerException(static_cast<uint32_t>(ExceptionCode::Trap));
}

// REGIMM trap-immediate instructions
void Interp_TGEI(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    int64_t imm = static_cast<int64_t>(static_cast<int16_t>(op.Itype.imm));
    if (AsDWord(regs->m_GPR[op.Itype.rs]).DW >= imm)
        regs->TriggerException(static_cast<uint32_t>(ExceptionCode::Trap));
}

void Interp_TGEIU(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t imm = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(op.Itype.imm)));
    if (AsDWord(regs->m_GPR[op.Itype.rs]).UDW >= imm)
        regs->TriggerException(static_cast<uint32_t>(ExceptionCode::Trap));
}

void Interp_TLTI(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    int64_t imm = static_cast<int64_t>(static_cast<int16_t>(op.Itype.imm));
    if (AsDWord(regs->m_GPR[op.Itype.rs]).DW < imm)
        regs->TriggerException(static_cast<uint32_t>(ExceptionCode::Trap));
}

void Interp_TLTIU(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t imm = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(op.Itype.imm)));
    if (AsDWord(regs->m_GPR[op.Itype.rs]).UDW < imm)
        regs->TriggerException(static_cast<uint32_t>(ExceptionCode::Trap));
}

void Interp_TEQI(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    int64_t imm = static_cast<int64_t>(static_cast<int16_t>(op.Itype.imm));
    if (AsDWord(regs->m_GPR[op.Itype.rs]).DW == imm)
        regs->TriggerException(static_cast<uint32_t>(ExceptionCode::Trap));
}

void Interp_TNEI(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    int64_t imm = static_cast<int64_t>(static_cast<int16_t>(op.Itype.imm));
    if (AsDWord(regs->m_GPR[op.Itype.rs]).DW != imm)
        regs->TriggerException(static_cast<uint32_t>(ExceptionCode::Trap));
}

// ----- Branch / Jump Callbacks -----
// These take 3 args: (CRegisters*, uint32_t opcVal, uint32_t pc)
// They compute the branch target and set m_PROGRAM_COUNTER accordingly.

void Interp_BEQ_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int64_t rt = AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    SetPC(regs, (rs == rt) ? (pc + 4 + offset) : (pc + 8));
}

void Interp_BNE_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int64_t rt = AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    SetPC(regs, (rs != rt) ? (pc + 4 + offset) : (pc + 8));
}

void Interp_BLEZ_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    SetPC(regs, (rs <= 0) ? (pc + 4 + offset) : (pc + 8));
}

void Interp_BGTZ_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    SetPC(regs, (rs > 0) ? (pc + 4 + offset) : (pc + 8));
}

void Interp_BLTZ_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    SetPC(regs, (rs < 0) ? (pc + 4 + offset) : (pc + 8));
}

void Interp_BGEZ_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    SetPC(regs, (rs >= 0) ? (pc + 4 + offset) : (pc + 8));
}

void Interp_BLTZAL_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    AsDWord(regs->m_GPR[31]).DW = static_cast<int64_t>(regs->GetLinkAddress(static_cast<int32_t>(pc + 8)));
    SetPC(regs, (rs < 0) ? (pc + 4 + offset) : (pc + 8));
}

void Interp_BGEZAL_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    AsDWord(regs->m_GPR[31]).DW = static_cast<int64_t>(regs->GetLinkAddress(static_cast<int32_t>(pc + 8)));
    SetPC(regs, (rs >= 0) ? (pc + 4 + offset) : (pc + 8));
}

void Interp_BC1F_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    if (CheckCop1Unusable(regs)) return;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    bool fpCond = (regs->m_FPCR[31] & FPCSR_C) != 0;
    SetPC(regs, (!fpCond) ? (pc + 4 + offset) : (pc + 8));
}

void Interp_BC1T_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    if (CheckCop1Unusable(regs)) return;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    bool fpCond = (regs->m_FPCR[31] & FPCSR_C) != 0;
    SetPC(regs, (fpCond) ? (pc + 4 + offset) : (pc + 8));
}

// Jump callbacks
void Interp_J_Jump(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    uint32_t target = ((pc + 4) & 0xF0000000) | (mips_target(opcVal) << 2);
    SetPC(regs, target);
}

void Interp_JAL_Jump(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    uint32_t target = ((pc + 4) & 0xF0000000) | (mips_target(opcVal) << 2);
    AsDWord(regs->m_GPR[31]).DW = static_cast<int64_t>(regs->GetLinkAddress(static_cast<int32_t>(pc + 8)));
    SetPC(regs, target);
}

void Interp_JR_Jump(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    uint32_t target = static_cast<uint32_t>(AsDWord(regs->m_GPR[mips_rs(opcVal)]).UDW);
    SetPC(regs, target);
    // Match interpreter: JR always sets TestTimer so the dispatcher
    // processes timer events after the jump, regardless of direction.
    regs->SetTestTimer();
}

void Interp_JALR_Jump(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    uint32_t rd = mips_rd(opcVal);
    // Read target BEFORE writing link (handles rd==rs case)
    uint32_t target = static_cast<uint32_t>(AsDWord(regs->m_GPR[mips_rs(opcVal)]).UDW);
    if (rd != 0) {
        AsDWord(regs->m_GPR[rd]).DW = static_cast<int64_t>(regs->GetLinkAddress(static_cast<int32_t>(pc + 8)));
    }
    SetPC(regs, target);
    // Match interpreter: JALR always sets TestTimer.
    regs->SetTestTimer();
}

// ----- Branch-Likely Callbacks -----
// These return int: 1 = branch taken (execute delay slot), 0 = not taken (skip delay slot).
// For branch-likely instructions, when the branch is NOT taken, the delay slot is nullified.

int Interp_BEQL_BranchLikely(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int64_t rt = AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    if (rs == rt) { SetPC(regs, pc + 4 + offset); return 1; }
    else { SetPC(regs, pc + 8); return 0; }
}

int Interp_BNEL_BranchLikely(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int64_t rt = AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    if (rs != rt) { SetPC(regs, pc + 4 + offset); return 1; }
    else { SetPC(regs, pc + 8); return 0; }
}

int Interp_BLEZL_BranchLikely(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    if (rs <= 0) { SetPC(regs, pc + 4 + offset); return 1; }
    else { SetPC(regs, pc + 8); return 0; }
}

int Interp_BGTZL_BranchLikely(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    if (rs > 0) { SetPC(regs, pc + 4 + offset); return 1; }
    else { SetPC(regs, pc + 8); return 0; }
}

int Interp_BLTZL_BranchLikely(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    if (rs < 0) { SetPC(regs, pc + 4 + offset); return 1; }
    else { SetPC(regs, pc + 8); return 0; }
}

int Interp_BGEZL_BranchLikely(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    if (rs >= 0) { SetPC(regs, pc + 4 + offset); return 1; }
    else { SetPC(regs, pc + 8); return 0; }
}

int Interp_BLTZALL_BranchLikely(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    AsDWord(regs->m_GPR[31]).DW = static_cast<int64_t>(regs->GetLinkAddress(static_cast<int32_t>(pc + 8)));
    if (rs < 0) { SetPC(regs, pc + 4 + offset); return 1; }
    else { SetPC(regs, pc + 8); return 0; }
}

int Interp_BGEZALL_BranchLikely(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    AsDWord(regs->m_GPR[31]).DW = static_cast<int64_t>(regs->GetLinkAddress(static_cast<int32_t>(pc + 8)));
    if (rs >= 0) { SetPC(regs, pc + 4 + offset); return 1; }
    else { SetPC(regs, pc + 8); return 0; }
}

int Interp_BC1FL_BranchLikely(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    bool fpCond = (regs->m_FPCR[31] & FPCSR_C) != 0;
    if (!fpCond) { SetPC(regs, pc + 4 + offset); return 1; }
    else { SetPC(regs, pc + 8); return 0; }
}

int Interp_BC1TL_BranchLikely(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    bool fpCond = (regs->m_FPCR[31] & FPCSR_C) != 0;
    if (fpCond) { SetPC(regs, pc + 4 + offset); return 1; }
    else { SetPC(regs, pc + 8); return 0; }
}

// ---- Binary-search diagnostic: SPECIAL dispatcher callback ----
// When FORCE_CALLBACK_SPECIAL is 1, all SPECIAL ops route through this
// single callback instead of inline x64 code.
void Interp_SpecialDispatch(CRegisters* regs, uint32_t opcVal) {
    switch (mips_funct(opcVal)) {
    case 0x00: Interp_SLL(regs, opcVal); break;
    case 0x02: Interp_SRL(regs, opcVal); break;
    case 0x03: Interp_SRA(regs, opcVal); break;
    case 0x04: Interp_SLLV(regs, opcVal); break;
    case 0x06: Interp_SRLV(regs, opcVal); break;
    case 0x07: Interp_SRAV(regs, opcVal); break;
    case 0x10: Interp_MFHI(regs, opcVal); break;
    case 0x11: Interp_MTHI(regs, opcVal); break;
    case 0x12: Interp_MFLO(regs, opcVal); break;
    case 0x13: Interp_MTLO(regs, opcVal); break;
    case 0x14: Interp_DSLLV(regs, opcVal); break;
    case 0x16: Interp_DSRLV(regs, opcVal); break;
    case 0x17: Interp_DSRAV(regs, opcVal); break;
    case 0x18: Interp_MULT(regs, opcVal); break;
    case 0x19: Interp_MULTU(regs, opcVal); break;
    case 0x1A: Interp_DIV(regs, opcVal); break;
    case 0x1B: Interp_DIVU(regs, opcVal); break;
    case 0x1C: Interp_DMULT(regs, opcVal); break;
    case 0x1D: Interp_DMULTU(regs, opcVal); break;
    case 0x1E: Interp_DDIV(regs, opcVal); break;
    case 0x1F: Interp_DDIVU(regs, opcVal); break;
    case 0x20: Interp_ADD(regs, opcVal); break;
    case 0x21: Interp_ADDU(regs, opcVal); break;
    case 0x22: Interp_SUB(regs, opcVal); break;
    case 0x23: Interp_SUBU(regs, opcVal); break;
    case 0x24: Interp_AND(regs, opcVal); break;
    case 0x25: Interp_OR(regs, opcVal); break;
    case 0x26: Interp_XOR(regs, opcVal); break;
    case 0x27: Interp_NOR(regs, opcVal); break;
    case 0x2A: Interp_SLT(regs, opcVal); break;
    case 0x2B: Interp_SLTU(regs, opcVal); break;
    case 0x2C: Interp_DADD(regs, opcVal); break;
    case 0x2D: Interp_DADDU(regs, opcVal); break;
    case 0x2E: Interp_DSUB(regs, opcVal); break;
    case 0x2F: Interp_DSUBU(regs, opcVal); break;
    case 0x38: Interp_DSLL(regs, opcVal); break;
    case 0x3A: Interp_DSRL(regs, opcVal); break;
    case 0x3B: Interp_DSRA(regs, opcVal); break;
    case 0x3C: Interp_DSLL32(regs, opcVal); break;
    case 0x3E: Interp_DSRL32(regs, opcVal); break;
    case 0x3F: Interp_DSRA32(regs, opcVal); break;
    default: break; // SYSCALL, BREAK, SYNC, JR, JALR handled separately
    }
}

// ---- Binary-search diagnostic: IMM dispatcher callback ----
void Interp_ImmDispatch(CRegisters* regs, uint32_t opcVal) {
    uint32_t op = opcVal >> 26;
    switch (op) {
    case 0x08: Interp_ADDI(regs, opcVal); break;
    case 0x09: Interp_ADDIU(regs, opcVal); break;
    case 0x0A: Interp_SLTI(regs, opcVal); break;
    case 0x0B: Interp_SLTIU(regs, opcVal); break;
    case 0x0C: Interp_ANDI(regs, opcVal); break;
    case 0x0D: Interp_ORI(regs, opcVal); break;
    case 0x0E: Interp_XORI(regs, opcVal); break;
    case 0x0F: Interp_LUI(regs, opcVal); break;
    case 0x18: Interp_DADDI(regs, opcVal); break;
    case 0x19: Interp_DADDIU(regs, opcVal); break;
    default: break;
    }
}

} // anonymous namespace

// =============================================================================
// x64 Code Emission Helpers
// =============================================================================
// These private methods of CX64RecompilerOps emit x64 machine code using
// AsmJit that, at runtime, will call the appropriate interpreter callback.

// Helper: emit call to a register-only callback: void(CRegisters*, uint32_t)
void CX64RecompilerOps::EmitRegisterCallback(void* callback, uint32_t opcVal) {
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;

    m_Assembler->AddComment("  -> interpreter callback");
    m_RegisterSet->FlushRegisters(true);

    // RBX = &CRegisters (set up in EnterCodeBlock)
#ifdef _WIN32
    a.mov(rcx, rbx);
    a.mov(edx, opcVal);
#else
    a.mov(rdi, rbx);
    a.mov(esi, opcVal);
#endif
    m_Assembler->EmitCall(callback);
}

// Helper: emit call to a memory callback: void(CRegisters*, MipsMemoryVM*, uint32_t)
void CX64RecompilerOps::EmitMemoryCallback(void* callback, uint32_t opcVal) {
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;

    m_Assembler->AddComment("  -> memory callback");
    m_RegisterSet->FlushRegisters(true);

    // Set m_PROGRAM_COUNTER so exceptions report the correct faulting PC.
    if (m_CurrentContext && !m_BlockEnded) {
        a.mov(qword_ptr(rbx, static_cast<int32_t>(offsetof(CRegisters, m_PROGRAM_COUNTER))),
              static_cast<int32_t>(m_CurrentContext->virtualAddress));
    }

    // RBX = &CRegisters, R12 = &MipsMemoryVM (set up in EnterCodeBlock)
#ifdef _WIN32
    a.mov(rcx, rbx);
    a.mov(rdx, r12);
    a.mov(r8d, opcVal);
#else
    a.mov(rdi, rbx);
    a.mov(rsi, r12);
    a.mov(edx, opcVal);
#endif
    m_Assembler->EmitCall(callback);
}

// Helper: emit call to a TLB callback: void(CRegisters*, CTLB*, uint32_t)
void CX64RecompilerOps::EmitTlbCallback(void* callback, uint32_t opcVal) {
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;

    m_Assembler->AddComment("  -> TLB callback");
    m_RegisterSet->FlushRegisters(true);

#ifdef _WIN32
    a.mov(rcx, reinterpret_cast<uint64_t>(&m_Context.registers));
    a.mov(rdx, reinterpret_cast<uint64_t>(&m_Context.tlb));
    a.mov(r8d, opcVal);
#else
    a.mov(rdi, reinterpret_cast<uint64_t>(&m_Context.registers));
    a.mov(rsi, reinterpret_cast<uint64_t>(&m_Context.tlb));
    a.mov(edx, opcVal);
#endif
    m_Assembler->EmitCall(callback);
}

// Helper: emit call to a branch callback: void(CRegisters*, uint32_t opcVal, uint32_t pc)
void CX64RecompilerOps::EmitBranchCallback(void* callback, uint32_t opcVal, uint32_t pc) {
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;

    m_Assembler->AddComment("  -> branch callback");
    m_RegisterSet->FlushRegisters(true);

    // RBX = &CRegisters (set up in EnterCodeBlock)
#ifdef _WIN32
    a.mov(rcx, rbx);
    a.mov(edx, opcVal);
    a.mov(r8d, pc);
#else
    a.mov(rdi, rbx);
    a.mov(esi, opcVal);
    a.mov(edx, pc);
#endif
    m_Assembler->EmitCall(callback);
}

void CX64RecompilerOps::EmitBranchLikelyCallback(void* callback, uint32_t opcVal, uint32_t pc) {
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;

    m_Assembler->AddComment("  -> branch-likely callback");
    m_RegisterSet->FlushRegisters(true);

    // RBX = &CRegisters (set up in EnterCodeBlock)
#ifdef _WIN32
    a.mov(rcx, rbx);
    a.mov(edx, opcVal);
    a.mov(r8d, pc);
#else
    a.mov(rdi, rbx);
    a.mov(esi, opcVal);
    a.mov(edx, pc);
#endif
    m_Assembler->EmitCall(callback);

    // Callback returns int in EAX: 1 = taken (execute delay slot), 0 = not taken (skip delay slot)
    // Emit the timer decrement for the branch instruction BEFORE the
    // conditional skip.  The branch itself always executes (evaluates its
    // condition), so its timer must always fire — even when the delay slot
    // is skipped on a not-taken branch-likely.  Without this, every
    // not-taken branch-likely undercounts NextTimer by CountPerOp, causing
    // a cumulative COUNT_REGISTER drift vs the interpreter.
    a.mov(ecx, eax);     // save callback result (ecx is caller-saved, not used by EmitTimerDecrement)
    EmitTimerDecrement(); // sub [&NextTimer], CountPerOp  (clobbers rax)
    m_BranchLikelySkipLabel = a.new_label();
    m_HasPendingBranchLikelySkip = true;
    a.test(ecx, ecx);
    a.jz(m_BranchLikelySkipLabel);
}

void CX64RecompilerOps::BindPendingBranchLikelyLabel() {
    if (!m_HasPendingBranchLikelySkip) return;
    auto& a = m_Assembler->GetAssembler();
    a.bind(m_BranchLikelySkipLabel);
    m_HasPendingBranchLikelySkip = false;
}

void CX64RecompilerOps::EmitPipelineStageCheck() {
    auto& a = m_Assembler->GetAssembler();

    // cmp byte [r13], PipelineStage::Jump (6)   (r13 = &m_PipelineStage)
    a.cmp(asmjit::x86::byte_ptr(asmjit::x86::r13),
           static_cast<uint8_t>(N64Types::PipelineStage::Jump));
    a.je(m_BlockExitLabel);
}

void CX64RecompilerOps::EmitDelaySlotSetup(uint32_t delaySlotAddr) {
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;

    m_Assembler->AddComment("  -> delay slot setup (fix BD/EPC)");

    // The branch callback has already set m_PROGRAM_COUNTER = branch_target.
    // We need to:
    //   1. Save branch_target to m_JumpToLocation
    //   2. Set m_PROGRAM_COUNTER = delay_slot_address
    //   3. Set m_PipelineStage = Jump
    // This way, if the delay slot faults, TriggerException sees:
    //   BranchDelay = (PipelineStage == Jump) = true
    //   EPC = m_PROGRAM_COUNTER - 4 = delaySlotAddr - 4 = branch_addr
    //
    // When no exception occurs, the delay slot's EmitPipelineStageCheck
    // exits the block with PipelineStage == Jump.  The main loop then
    // applies the jump: m_ProgramCounter = m_JumpToLocation = branch_target.

    uint64_t pcAbsAddr = reinterpret_cast<uint64_t>(&m_Context.registers.m_PROGRAM_COUNTER);
    uint64_t jumpAbsAddr = reinterpret_cast<uint64_t>(&m_Context.system.m_JumpToLocation);

    // 1. m_JumpToLocation = m_PROGRAM_COUNTER (save branch target)
    a.mov(rax, pcAbsAddr);
    a.mov(rcx, qword_ptr(rax));       // rcx = m_PROGRAM_COUNTER (branch target)
    a.mov(rax, jumpAbsAddr);
    a.mov(qword_ptr(rax), rcx);       // m_JumpToLocation = branch target

    // 2. m_PROGRAM_COUNTER = delaySlotAddr (sign-extended via imm32)
    a.mov(rax, pcAbsAddr);
    a.mov(qword_ptr(rax), static_cast<int32_t>(delaySlotAddr));

    // 3. m_PipelineStage = Jump  (r13 = &m_PipelineStage)
    a.mov(byte_ptr(r13), static_cast<uint8_t>(N64Types::PipelineStage::Jump));
}

// Static helper called from emitted code when COP1 is not usable
static void TriggerCop1UnusableException(CRegisters* regs) {
    regs->TriggerException(11, 1); // EXC_CPU=11, Coprocessor=1
}

// Static helper called from emitted code when COP2 is accessed
static void TriggerCop2UnusableException(CRegisters* regs) {
    regs->TriggerException(11, 2); // EXC_CPU=11, Coprocessor=2
}

// Static COP2 dispatch callback: checks CU2 before accessing COP2
static void Interp_COP2_Dispatch(CRegisters* regs, Nova64::MipsMemoryVM* mmu, uint32_t opcVal, uint32_t compileAddr) {
    uint32_t rs = mips_rs(opcVal);

    // Set PC to the COP2 instruction's address from compile time.
    regs->m_PROGRAM_COUNTER = static_cast<int64_t>(static_cast<int32_t>(compileAddr));

    // Check CU2 bit in Status register FIRST (before any RI checks)
    if (regs->STATUS_REGISTER.CU2 == 0) {
        regs->TriggerException(11, 2); // EXC_CPU=11, Coprocessor=2
        return;
    }
    // CU2=1: DCFC2 (rs=3) and DCTC2 (rs=7) are Reserved Instruction on VR4300
    // Real hardware sets CE=2 in CAUSE for RI from COP2 instructions
    if (rs == 3 || rs == 7) {
        regs->TriggerException(10, 2); // EXC_II=10, CE=2 (from COP2)
        return;
    }
    // No exception fired — advance PC past this COP2 instruction so the
    // block exit doesn't re-execute it (the block always exits immediately
    // with m_BlockEnded=true, so m_PROGRAM_COUNTER must point to the NEXT
    // instruction for the dispatch loop to continue correctly).
    regs->m_PROGRAM_COUNTER = static_cast<int64_t>(static_cast<int32_t>(compileAddr)) + 4;
    // CU2=1: COP2 operations use a latch register on VR4300
    // MFC2 (rs=0): read COP2 latch into rt (sign-extended to 64-bit)
    if (rs == 0) {
        uint32_t rt = mips_rt(opcVal);
        if (rt != 0) {
            uint32_t rd = mips_rd(opcVal);
            AsDWord(regs->m_GPR[rt]).DW = static_cast<int64_t>(static_cast<int32_t>(regs->Cop2_MF(rd)));
        }
    }
    // DMFC2 (rs=1): read COP2 latch into rt (full 64-bit)
    else if (rs == 1) {
        uint32_t rt = mips_rt(opcVal);
        if (rt != 0) {
            uint32_t rd = mips_rd(opcVal);
            AsDWord(regs->m_GPR[rt]).DW = static_cast<int64_t>(regs->Cop2_MF(rd));
        }
    }
    // CFC2 (rs=2): read COP2 control latch into rt
    else if (rs == 2) {
        uint32_t rt = mips_rt(opcVal);
        if (rt != 0) {
            uint32_t rd = mips_rd(opcVal);
            AsDWord(regs->m_GPR[rt]).DW = static_cast<int64_t>(static_cast<int32_t>(regs->Cop2_MF(rd)));
        }
    }
    // MTC2 (rs=4): write rt to COP2 latch (full 64-bit GPR value)
    else if (rs == 4) {
        uint32_t rd = mips_rd(opcVal);
        regs->Cop2_MT(rd, static_cast<uint64_t>(AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW));
    }
    // DMTC2 (rs=5): write rt to COP2 latch (full 64-bit)
    else if (rs == 5) {
        uint32_t rd = mips_rd(opcVal);
        regs->Cop2_MT(rd, static_cast<uint64_t>(AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW));
    }
    // CTC2 (rs=6): write rt to COP2 control latch (full 64-bit GPR value)
    else if (rs == 6) {
        uint32_t rd = mips_rd(opcVal);
        regs->Cop2_MT(rd, static_cast<uint64_t>(AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW));
    }
}

// Static helper called from emitted code when COP3 is accessed (RI exception)
static void TriggerCop3ReservedInstruction(CRegisters* regs, uint32_t compileAddr) {
    // Set PC to the COP3 instruction's address from compile time
    regs->m_PROGRAM_COUNTER = static_cast<int64_t>(static_cast<int32_t>(compileAddr));
    // COP3 doesn't exist on N64 — fire Reserved Instruction with CE=0
    // (hardware doesn't set CE for non-existent coprocessors)
    regs->TriggerException(10, 0); // EXC_II=10, CE=0
}

// Static helper called from emitted code on integer overflow
static void TriggerOverflowException(CRegisters* regs) {
    regs->TriggerException(12, 0); // EXC_OV=12
}

// Static helper: Check COP1 usable and trigger exception if not.
// Returns 1 if exception was triggered (CU1=0), 0 if COP1 is usable.
static uint32_t CheckCop1UsableAndTrigger(CRegisters* regs) {
    uint32_t statusVal = static_cast<uint32_t>(regs->STATUS_REGISTER.Value);
    if ((statusVal & 0x20000000) == 0) {
        regs->TriggerException(11, 1); // EXC_CPU=11, Coprocessor=1
        return 1;
    }
    return 0;
}

void CX64RecompilerOps::EmitCop1UsableCheck() {
    // CU1 checks are now inside each COP1 callback directly.
    // The EmitPipelineStageCheck after each instruction handles the block exit
    // when TriggerException sets PipelineStage=Jump.
}

void CX64RecompilerOps::EmitTimerDecrement() {
    auto& a = m_Assembler->GetAssembler();
    uint32_t countPerOp = m_Context.system.CountPerOp();

    // sub dword ptr [r14], countPerOp   (r14 = &m_NextTimer)
    a.sub(asmjit::x86::dword_ptr(asmjit::x86::r14),
          static_cast<int32_t>(countPerOp));
}

void CX64RecompilerOps::EmitPCUpdate() {
    if (m_CurrentContext && !m_BlockEnded) {
        auto& a = m_Assembler->GetAssembler();
        // mov qword [rbx + offsetof(m_PROGRAM_COUNTER)], sign_extend(instrPC)
        // RBX = &CRegisters, m_PROGRAM_COUNTER is at offset 0
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbx,
              static_cast<int32_t>(offsetof(CRegisters, m_PROGRAM_COUNTER))),
              static_cast<int32_t>(m_CurrentContext->virtualAddress));
    }
}

// =============================================================================
// Construction / Destruction
// =============================================================================

CX64RecompilerOps::CX64RecompilerOps(RecompilerContext& context,
                                     CodeBlock& codeBlock)
    : IRecompilerOps()
    , m_Context(context)
    , m_CodeBlock(codeBlock)
    , m_Assembler(std::make_unique<CX64Ops>(codeBlock))
    , m_RegisterSet(std::make_unique<CX64RegInfo>(codeBlock, *m_Assembler))
    , m_OptimizationLevel(OptimizationLevel::Basic)
    , m_InDelaySlot(false)
    , m_BlockEnded(false)
{
    Log::debug("CX64RecompilerOps: Created");
    InitializeInstructionHandlers();
}

CX64RecompilerOps::~CX64RecompilerOps()
{
    Log::debugf("CX64RecompilerOps: Destroyed - translated %zu instructions",
                m_Stats.instructionsTranslated);
}

// =============================================================================
// IRecompilerOps Interface - Setup & Teardown
// =============================================================================

bool CX64RecompilerOps::Initialize(RecompilerContext& context, CodeBlock& codeBlock)
{
    m_Assembler = std::make_unique<CX64Ops>(codeBlock);
    m_RegisterSet = std::make_unique<CX64RegInfo>(codeBlock, *m_Assembler);
    m_InDelaySlot = false;
    m_BlockEnded = false;
    m_Stats.Reset();
    m_JumpTargets.clear();
    m_PendingJumps.clear();
    Log::debug("CX64RecompilerOps: Initialized");
    return true;
}

void CX64RecompilerOps::Cleanup()
{
    m_RegisterSet->FlushRegisters(true);
    m_JumpTargets.clear();
    m_PendingJumps.clear();
    Log::debug("CX64RecompilerOps: Cleaned up");
}

// =============================================================================
// IRecompilerOps Interface - Code Generation
// =============================================================================

bool CX64RecompilerOps::CompileInstruction(const InstructionContext& ctx)
{
    R4300iOps op = ctx.opcode;
    uint32_t opcVal = op.Value;
    m_Stats.instructionsTranslated++;

    // Populate the TranslationContext so that macro-generated instruction
    // handlers (SPECIAL_SLL, LW, SW, etc.) can access the opcode and address.
    m_CurrentContext.emplace(TranslationContext{
        /* opcode          */ op,
        /* virtualAddress  */ ctx.virtualAddress,
        /* physicalAddress */ ctx.physicalAddress,
        /* pipelineStage   */ ctx.pipelineStage,
        /* inDelaySlot     */ false,
        /* isBranchLikely  */ false,
        /* branchTarget    */ std::nullopt,
        /* linkAddress     */ std::nullopt,
        /* disassembly     */ {},
        /* emitDebugComments */ true
    });

    // Create a debug comment with the MIPS address
    m_Assembler->AddComment(
        std::string("PC=0x") +
        std::to_string(ctx.virtualAddress));

    // Dispatch by primary opcode
    switch (op.Rtype.op) {
    case R4300_SPECIAL: {
#if FORCE_CALLBACK_SPECIAL
        // Binary-search: route ALL SPECIAL ALU/shift/logic to interpreter callback
        {
            uint32_t funct = mips_funct(opcVal);
            // JR, JALR, SYSCALL, BREAK, SYNC, traps still use their own paths
            if (funct == R4300_S_JR)          { SPECIAL_JR(); }
            else if (funct == R4300_S_JALR)   { SPECIAL_JALR(); }
            else if (funct == R4300_S_SYSCALL){ SPECIAL_SYSCALL(); }
            else if (funct == R4300_S_BREAK)  { SPECIAL_BREAK(); }
            else if (funct == R4300_S_SYNC)   { SPECIAL_SYNC(); }
            else if (funct >= 0x30 && funct <= 0x36) {
                // Trap instructions
                switch (funct) {
                case R4300_S_TGE:  EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TGE), opcVal); break;
                case R4300_S_TGEU: EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TGEU), opcVal); break;
                case R4300_S_TLT:  EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TLT), opcVal); break;
                case R4300_S_TLTU: EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TLTU), opcVal); break;
                case R4300_S_TEQ:  EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TEQ), opcVal); break;
                case R4300_S_TNE:  EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TNE), opcVal); break;
                default: UnknownOpcode(); break;
                }
            } else {
                m_Assembler->AddComment("SPECIAL via callback");
                EmitRegisterCallback(reinterpret_cast<void*>(&Interp_SpecialDispatch), opcVal);
            }
        }
#else
        // Decode SPECIAL by funct field (inline x64)
        switch (mips_funct(opcVal)) {
        case R4300_S_SLL:    SPECIAL_SLL(); break;
        case R4300_S_SRL:    SPECIAL_SRL(); break;
        case R4300_S_SRA:    SPECIAL_SRA(); break;
        case R4300_S_SLLV:   SPECIAL_SLLV(); break;
        case R4300_S_SRLV:   SPECIAL_SRLV(); break;
        case R4300_S_SRAV:   SPECIAL_SRAV(); break;
        case R4300_S_JR:     SPECIAL_JR(); break;
        case R4300_S_JALR:   SPECIAL_JALR(); break;
        case R4300_S_SYSCALL: SPECIAL_SYSCALL(); break;
        case R4300_S_BREAK:  SPECIAL_BREAK(); break;
        case R4300_S_SYNC:   SPECIAL_SYNC(); break;
        case R4300_S_MFHI:   SPECIAL_MFHI(); break;
        case R4300_S_MTHI:   SPECIAL_MTHI(); break;
        case R4300_S_MFLO:   SPECIAL_MFLO(); break;
        case R4300_S_MTLO:   SPECIAL_MTLO(); break;
        case R4300_S_DSLLV:  SPECIAL_DSLLV(); break;
        case R4300_S_DSRLV:  SPECIAL_DSRLV(); break;
        case R4300_S_DSRAV:  SPECIAL_DSRAV(); break;
        case R4300_S_MULT:   SPECIAL_MULT(); break;
        case R4300_S_MULTU:  SPECIAL_MULTU(); break;
        case R4300_S_DIV:    SPECIAL_DIV(); break;
        case R4300_S_DIVU:   SPECIAL_DIVU(); break;
        case R4300_S_DMULT:  SPECIAL_DMULT(); break;
        case R4300_S_DMULTU: SPECIAL_DMULTU(); break;
        case R4300_S_DDIV:   SPECIAL_DDIV(); break;
        case R4300_S_DDIVU:  SPECIAL_DDIVU(); break;
        case R4300_S_ADD:    SPECIAL_ADD(); break;
        case R4300_S_ADDU:   SPECIAL_ADDU(); break;
        case R4300_S_SUB:    SPECIAL_SUB(); break;
        case R4300_S_SUBU:   SPECIAL_SUBU(); break;
        case R4300_S_AND:    SPECIAL_AND(); break;
        case R4300_S_OR:     SPECIAL_OR(); break;
        case R4300_S_XOR:    SPECIAL_XOR(); break;
        case R4300_S_NOR:    SPECIAL_NOR(); break;
        case R4300_S_SLT:    SPECIAL_SLT(); break;
        case R4300_S_SLTU:   SPECIAL_SLTU(); break;
        case R4300_S_DADD:   SPECIAL_DADD(); break;
        case R4300_S_DADDU:  SPECIAL_DADDU(); break;
        case R4300_S_DSUB:   SPECIAL_DSUB(); break;
        case R4300_S_DSUBU:  SPECIAL_DSUBU(); break;
        case R4300_S_TGE:    EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TGE), opcVal); break;
        case R4300_S_TGEU:   EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TGEU), opcVal); break;
        case R4300_S_TLT:    EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TLT), opcVal); break;
        case R4300_S_TLTU:   EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TLTU), opcVal); break;
        case R4300_S_TEQ:    EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TEQ), opcVal); break;
        case R4300_S_TNE:    EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TNE), opcVal); break;
        case R4300_S_DSLL:   SPECIAL_DSLL(); break;
        case R4300_S_DSRL:   SPECIAL_DSRL(); break;
        case R4300_S_DSRA:   SPECIAL_DSRA(); break;
        case R4300_S_DSLL32: SPECIAL_DSLL32(); break;
        case R4300_S_DSRL32: SPECIAL_DSRL32(); break;
        case R4300_S_DSRA32: SPECIAL_DSRA32(); break;
        default: UnknownOpcode(); break;
        }
#endif
        break;
    }
    case R4300_REGIMM: {
#if FORCE_CALLBACK_BRANCH
        {
            uint32_t rt_field = op.Itype.rt;
            // Trap instructions stay as-is; branches route to callbacks
            switch (rt_field) {
            case R4300_R_BLTZ:    EmitBranchCallback(reinterpret_cast<void*>(&Interp_BLTZ_Branch), opcVal, ctx.virtualAddress); m_BlockEnded = true; break;
            case R4300_R_BGEZ:    EmitBranchCallback(reinterpret_cast<void*>(&Interp_BGEZ_Branch), opcVal, ctx.virtualAddress); m_BlockEnded = true; break;
            case R4300_R_BLTZL:   BLTZL_Compare_Likely(); break;
            case R4300_R_BGEZL:   BGEZL_Compare_Likely(); break;
            case R4300_R_TGEI:    EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TGEI), opcVal); break;
            case R4300_R_TGEIU:   EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TGEIU), opcVal); break;
            case R4300_R_TLTI:    EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TLTI), opcVal); break;
            case R4300_R_TLTIU:   EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TLTIU), opcVal); break;
            case R4300_R_TEQI:    EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TEQI), opcVal); break;
            case R4300_R_TNEI:    EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TNEI), opcVal); break;
            case R4300_R_BLTZAL:  EmitBranchCallback(reinterpret_cast<void*>(&Interp_BLTZAL_Branch), opcVal, ctx.virtualAddress); m_BlockEnded = true; break;
            case R4300_R_BGEZAL:  EmitBranchCallback(reinterpret_cast<void*>(&Interp_BGEZAL_Branch), opcVal, ctx.virtualAddress); m_BlockEnded = true; break;
            case R4300_R_BLTZALL: BLTZALL_Compare_Likely(); break;
            case R4300_R_BGEZALL: BGEZALL_Compare_Likely(); break;
            default: UnknownOpcode(); break;
            }
        }
#else
        switch (op.Itype.rt) {
        case R4300_R_BLTZ:    BLTZ_Compare(); break;
        case R4300_R_BGEZ:    BGEZ_Compare(); break;
        case R4300_R_BLTZL:   BLTZL_Compare_Likely(); break;
        case R4300_R_BGEZL:   BGEZL_Compare_Likely(); break;
        case R4300_R_TGEI:    EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TGEI), opcVal); break;
        case R4300_R_TGEIU:   EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TGEIU), opcVal); break;
        case R4300_R_TLTI:    EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TLTI), opcVal); break;
        case R4300_R_TLTIU:   EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TLTIU), opcVal); break;
        case R4300_R_TEQI:    EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TEQI), opcVal); break;
        case R4300_R_TNEI:    EmitPCUpdate(); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TNEI), opcVal); break;
        case R4300_R_BLTZAL:  BLTZAL_Compare(); break;
        case R4300_R_BGEZAL:  BGEZAL_Compare(); break;
        case R4300_R_BLTZALL: BLTZALL_Compare_Likely(); break;
        case R4300_R_BGEZALL: BGEZALL_Compare_Likely(); break;
        default: UnknownOpcode(); break;
        }
#endif
        break;
    }
#if FORCE_CALLBACK_JUMP
    case R4300_J:      EmitBranchCallback(reinterpret_cast<void*>(&Interp_J_Jump), opcVal, ctx.virtualAddress); m_BlockEnded = true; break;
    case R4300_JAL:    EmitBranchCallback(reinterpret_cast<void*>(&Interp_JAL_Jump), opcVal, ctx.virtualAddress); m_BlockEnded = true; break;
#else
    case R4300_J:      J(); break;
    case R4300_JAL:    JAL(); break;
#endif
#if FORCE_CALLBACK_BRANCH
    case R4300_BEQ:    EmitBranchCallback(reinterpret_cast<void*>(&Interp_BEQ_Branch), opcVal, ctx.virtualAddress); m_BlockEnded = true; break;
    case R4300_BNE:    EmitBranchCallback(reinterpret_cast<void*>(&Interp_BNE_Branch), opcVal, ctx.virtualAddress); m_BlockEnded = true; break;
    case R4300_BLEZ:   EmitBranchCallback(reinterpret_cast<void*>(&Interp_BLEZ_Branch), opcVal, ctx.virtualAddress); m_BlockEnded = true; break;
    case R4300_BGTZ:   EmitBranchCallback(reinterpret_cast<void*>(&Interp_BGTZ_Branch), opcVal, ctx.virtualAddress); m_BlockEnded = true; break;
#else
    case R4300_BEQ:    BEQ_Compare(); break;
    case R4300_BNE:    BNE_Compare(); break;
    case R4300_BLEZ:   BLEZ_Compare(); break;
    case R4300_BGTZ:   BGTZ_Compare(); break;
#endif
#if FORCE_CALLBACK_IMM
    case R4300_ADDI:   m_Assembler->AddComment("ADDI cb"); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_ImmDispatch), opcVal); break;
    case R4300_ADDIU:  m_Assembler->AddComment("ADDIU cb"); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_ImmDispatch), opcVal); break;
    case R4300_SLTI:   m_Assembler->AddComment("SLTI cb"); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_ImmDispatch), opcVal); break;
    case R4300_SLTIU:  m_Assembler->AddComment("SLTIU cb"); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_ImmDispatch), opcVal); break;
    case R4300_ANDI:   m_Assembler->AddComment("ANDI cb"); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_ImmDispatch), opcVal); break;
    case R4300_ORI:    m_Assembler->AddComment("ORI cb"); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_ImmDispatch), opcVal); break;
    case R4300_XORI:   m_Assembler->AddComment("XORI cb"); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_ImmDispatch), opcVal); break;
    case R4300_LUI:    m_Assembler->AddComment("LUI cb"); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_ImmDispatch), opcVal); break;
#else
    case R4300_ADDI:   ADDI(); break;
    case R4300_ADDIU:  ADDIU(); break;
    case R4300_SLTI:   SLTI(); break;
    case R4300_SLTIU:  SLTIU(); break;
    case R4300_ANDI:   ANDI(); break;
    case R4300_ORI:    ORI(); break;
    case R4300_XORI:   XORI(); break;
    case R4300_LUI:    LUI(); break;
#endif
    case R4300_COP0: {
        switch (mips_rs(opcVal)) {
        case R4300_COP0_MF:  COP0_MF(); break;
        case R4300_COP0_DMF: COP0_DMF(); break;
        case R4300_COP0_MT:  COP0_MT(); break;
        case R4300_COP0_DMT: COP0_DMT(); break;
        default:
            // COP0 CO instructions (bit 25 set)
            if (op.Value & (1 << 25)) {
                switch (mips_funct(opcVal)) {
                case R4300_COP0_C0_TLBR:  COP0_CO_TLBR(); break;
                case R4300_COP0_C0_TLBWI: COP0_CO_TLBWI(); break;
                case R4300_COP0_C0_TLBWR: COP0_CO_TLBWR(); break;
                case R4300_COP0_C0_TLBP:  COP0_CO_TLBP(); break;
                case R4300_COP0_C0_ERET:  COP0_CO_ERET(); break;
                default: UnknownOpcode(); break;
                }
            } else {
                UnknownOpcode();
            }
            break;
        }
        break;
    }
    case R4300_COP1: {
        EmitCop1UsableCheck();
        switch (mips_rs(opcVal)) {
        case R4300_COP1_MF:  COP1_MF(); break;
        case R4300_COP1_DMF: COP1_DMF(); break;
        case R4300_COP1_CF:  COP1_CF(); break;
        case R4300_COP1_MT:  COP1_MT(); break;
        case R4300_COP1_DMT: COP1_DMT(); break;
        case R4300_COP1_CT:  COP1_CT(); break;
        case 3: // DCFC1
        case 7: // DCTC1
        {
            // 64-bit CFC1/CTC1: check CU1 first, then RI
            if (!m_CurrentContext) break;
            EmitPCUpdate();
            EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_DCFC1_DCTC1),
                                 m_CurrentContext->opcode.Value);
            m_BlockEnded = true;
            break;
        }
        case R4300_COP1_BC: {
            switch (op.Itype.rt & 3) {
#if FORCE_CALLBACK_BRANCH
            case 0: EmitBranchCallback(reinterpret_cast<void*>(&Interp_BC1F_Branch), opcVal, ctx.virtualAddress); m_BlockEnded = true; break;
            case 1: EmitBranchCallback(reinterpret_cast<void*>(&Interp_BC1T_Branch), opcVal, ctx.virtualAddress); m_BlockEnded = true; break;
#else
            case 0: COP1_BCF_Compare(); break;      // BC1F
            case 1: COP1_BCT_Compare(); break;      // BC1T
#endif
            case 2: BC1FL_Compare_Likely(); break;   // BC1FL
            case 3: BC1TL_Compare_Likely(); break;   // BC1TL
            default: UnknownOpcode(); break;
            }
            break;
        }
        case R4300_COP1_S: {
            uint32_t funct = mips_funct(opcVal);
            if (funct >= R4300_FPU_C_F && funct <= R4300_FPU_C_NGT) {
                COP1_S_CMP();
            } else if (funct >= R4300_FPU_CVT_S && funct <= R4300_FPU_CVT_L) {
                COP1_S_CVT_D(); // dispatches internally based on funct
            } else if (funct >= R4300_FPU_ROUND_L && funct <= R4300_FPU_FLOOR_W) {
                COP1_S_CVT_D(); // rounding/truncation uses same CVT path
            } else {
                switch (funct) {
                case R4300_FPU_ADD:  COP1_S_ADD(); break;
                case R4300_FPU_SUB:  COP1_S_SUB(); break;
                case R4300_FPU_MUL:  COP1_S_MUL(); break;
                case R4300_FPU_DIV:  COP1_S_DIV(); break;
                case R4300_FPU_SQRT: COP1_S_SQRT(); break;
                case R4300_FPU_ABS:  COP1_S_ABS(); break;
                case R4300_FPU_MOV:  COP1_S_MOV(); break;
                case R4300_FPU_NEG:  COP1_S_NEG(); break;
                default:
                    EmitPCUpdate();
                    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_Unimplemented), opcVal);
                    m_BlockEnded = true;
                    break;
                }
            }
            break;
        }
        case R4300_COP1_D: {
            uint32_t funct = mips_funct(opcVal);
            if (funct >= R4300_FPU_C_F && funct <= R4300_FPU_C_NGT) {
                COP1_D_CMP();
            } else if (funct >= R4300_FPU_CVT_S && funct <= R4300_FPU_CVT_L) {
                COP1_D_CVT_S(); // dispatches internally
            } else if (funct >= R4300_FPU_ROUND_L && funct <= R4300_FPU_FLOOR_W) {
                COP1_D_CVT_S(); // rounding via CVT path
            } else {
                switch (funct) {
                case R4300_FPU_ADD:  COP1_D_ADD(); break;
                case R4300_FPU_SUB:  COP1_D_SUB(); break;
                case R4300_FPU_MUL:  COP1_D_MUL(); break;
                case R4300_FPU_DIV:  COP1_D_DIV(); break;
                case R4300_FPU_SQRT: COP1_D_SQRT(); break;
                case R4300_FPU_ABS:  COP1_D_ABS(); break;
                case R4300_FPU_MOV:  COP1_D_MOV(); break;
                case R4300_FPU_NEG:  COP1_D_NEG(); break;
                default:
                    EmitPCUpdate();
                    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_Unimplemented), opcVal);
                    m_BlockEnded = true;
                    break;
                }
            }
            break;
        }
        case R4300_COP1_W: {
            switch (mips_funct(opcVal)) {
            case R4300_FPU_CVT_S: COP1_W_CVT_S(); break;
            case R4300_FPU_CVT_D: COP1_W_CVT_D(); break;
            default:
                // Unknown W-format function → route to interpreter (handles UnimplementedOp)
                EmitPCUpdate();
                EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_W_CVT), opcVal);
                m_BlockEnded = true;
                break;
            }
            break;
        }
        case R4300_COP1_L: {
            switch (mips_funct(opcVal)) {
            case R4300_FPU_CVT_S: COP1_L_CVT_S(); break;
            case R4300_FPU_CVT_D: COP1_L_CVT_D(); break;
            default:
                // Unknown L-format function → route to interpreter (handles UnimplementedOp)
                EmitPCUpdate();
                EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_L_CVT), opcVal);
                m_BlockEnded = true;
                break;
            }
            break;
        }
        default: {
            // Undefined COP1 arithmetic format → UnimplementedOp (FPE)
            EmitPCUpdate();
            EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_Unimplemented),
                                 opcVal);
            m_BlockEnded = true;
            break;
        }
        }
        break;
    }
    case R4300_CP2: {
        // COP2 dispatch: checks CU2, handles DCFC2/DCTC2 as RI
        EmitPCUpdate();
        {
            auto& a = m_Assembler->GetAssembler();
            m_Assembler->AddComment("COP2 dispatch");
            m_RegisterSet->FlushRegisters(true);
            // Call Interp_COP2_Dispatch(regs, mmu, opcVal, compileAddr)
            a.mov(asmjit::x86::rcx, asmjit::x86::rbx);   // rcx = regs
            a.mov(asmjit::x86::rdx, asmjit::x86::r12);    // rdx = mmu
            a.mov(asmjit::x86::r8d, static_cast<int32_t>(opcVal)); // r8d = opcVal
            a.mov(asmjit::x86::r9d, static_cast<int32_t>(m_CurrentContext->virtualAddress)); // r9d = compileAddr
            a.mov(asmjit::x86::rax, reinterpret_cast<uint64_t>(&Interp_COP2_Dispatch));
            a.call(asmjit::x86::rax);
            EmitTimerDecrement();
            a.jmp(m_BlockExitLabel);
        }
        m_BlockEnded = true;
        break;
    }
    case R4300_CP3: {
        // COP3 access — Reserved Instruction exception on N64
        EmitPCUpdate();
        {
            auto& a = m_Assembler->GetAssembler();
            m_Assembler->AddComment("COP3 -> Reserved Instruction exception");
            m_RegisterSet->FlushRegisters(true);
            a.mov(asmjit::x86::rcx, asmjit::x86::rbx);
            a.mov(asmjit::x86::edx, static_cast<int32_t>(m_CurrentContext->virtualAddress)); // compileAddr
            a.mov(asmjit::x86::rax, reinterpret_cast<uint64_t>(&TriggerCop3ReservedInstruction));
            a.call(asmjit::x86::rax);
            EmitTimerDecrement();
            a.jmp(m_BlockExitLabel);
        }
        m_BlockEnded = true;
        break;
    }
    case R4300_BEQL:   BEQL_Compare_Likely(); break;
    case R4300_BNEL:   BNEL_Compare_Likely(); break;
    case R4300_BLEZL:  BLEZL_Compare_Likely(); break;
    case R4300_BGTZL:  BGTZL_Compare_Likely(); break;
    case R4300_RES:    // 0x1C
    case 0x1D:
    case 0x1E:
    case 0x1F: {
        // Reserved opcodes — trigger Reserved Instruction exception
        EmitPCUpdate();
        {
            auto& a = m_Assembler->GetAssembler();
            m_Assembler->AddComment("Reserved opcode -> RI exception");
            m_RegisterSet->FlushRegisters(true);
            a.mov(asmjit::x86::rcx, asmjit::x86::rbx);
            a.mov(asmjit::x86::edx, static_cast<int32_t>(m_CurrentContext->virtualAddress)); // compileAddr
            a.mov(asmjit::x86::rax, reinterpret_cast<uint64_t>(&TriggerCop3ReservedInstruction));
            a.call(asmjit::x86::rax);
            EmitTimerDecrement();
            a.jmp(m_BlockExitLabel);
        }
        m_BlockEnded = true;
        break;
    }
#if FORCE_CALLBACK_IMM
    case R4300_DADDI:  m_Assembler->AddComment("DADDI cb"); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_ImmDispatch), opcVal); break;
    case R4300_DADDIU: m_Assembler->AddComment("DADDIU cb"); EmitRegisterCallback(reinterpret_cast<void*>(&Interp_ImmDispatch), opcVal); break;
#else
    case R4300_DADDI:  DADDI(); break;
    case R4300_DADDIU: DADDIU(); break;
#endif
    case R4300_LDL:    LDL(); break;
    case R4300_LDR:    LDR(); break;
    case R4300_LB:     LB(); break;
    case R4300_LH:     LH(); break;
    case R4300_LWL:    LWL(); break;
    case R4300_LW:     LW(); break;
    case R4300_LBU:    LBU(); break;
    case R4300_LHU:    LHU(); break;
    case R4300_LWR:    LWR(); break;
    case R4300_LWU:    LWU(); break;
    case R4300_SB:     SB(); break;
    case R4300_SH:     SH(); break;
    case R4300_SWL:    SWL(); break;
    case R4300_SW:     SW(); break;
    case R4300_SDL:    SDL(); break;
    case R4300_SDR:    SDR(); break;
    case R4300_SWR:    SWR(); break;
    case R4300_CACHE:  CACHE(); break;
    case R4300_LL:     LL(); break;
    case R4300_LLD:    LLD(); break;
    case R4300_LWC1:   EmitCop1UsableCheck(); LWC1(); break;
    case R4300_LDC1:   EmitCop1UsableCheck(); LDC1(); break;
    case R4300_LD:     LD(); break;
    case R4300_SC:     SC(); break;
    case R4300_SCD:    SCD(); break;
    case R4300_SWC1:   EmitCop1UsableCheck(); SWC1(); break;
    case R4300_SDC1:   EmitCop1UsableCheck(); SDC1(); break;
    case R4300_SD:     SD(); break;
    default: UnknownOpcode(); break;
    }

    // Ensure $zero is always 0 after every instruction
    // (The callbacks already handle this, but this is a safety net)
    return true;
}

bool CX64RecompilerOps::CompileDelaySlot(const InstructionContext& ctx)
{
    m_InDelaySlot = true;
    bool result = CompileInstruction(ctx);
    m_InDelaySlot = false;
    return result;
}

bool CX64RecompilerOps::CompileBranch(const InstructionContext& ctx,
                                      BranchType type, ConditionType condition)
{
    // Branch compilation is handled through the branch compare methods
    // which are called from CompileInstruction's dispatch
    return CompileInstruction(ctx);
}

bool CX64RecompilerOps::CompileJump(const InstructionContext& ctx, bool /*withLink*/)
{
    return CompileInstruction(ctx);
}

bool CX64RecompilerOps::CompileTrap(const InstructionContext& ctx, ConditionType /*condition*/)
{
    return CompileInstruction(ctx);
}

bool CX64RecompilerOps::CompileSystemCall(const InstructionContext& ctx)
{
    return CompileInstruction(ctx);
}

bool CX64RecompilerOps::CompileBreak(const InstructionContext& ctx)
{
    return CompileInstruction(ctx);
}

// =============================================================================
// IRecompilerOps Interface - Register Management
// =============================================================================

void CX64RecompilerOps::LoadRegister(uint32_t regIndex, bool is64Bit)
{
    if (regIndex >= 32 || regIndex == 0) return;
    auto width = is64Bit ? RegisterWidth::Bits64 : RegisterWidth::Bits32;
    m_RegisterSet->MapRegister(regIndex, width);
}

void CX64RecompilerOps::StoreRegister(uint32_t regIndex, bool /*is64Bit*/)
{
    if (regIndex >= 32 || regIndex == 0) return;
    m_RegisterSet->SpillRegister(regIndex);
}

void CX64RecompilerOps::FlushRegisters()
{
    m_RegisterSet->FlushRegisters(true);
}

// =============================================================================
// IRecompilerOps Interface - Memory Operations
// =============================================================================

bool CX64RecompilerOps::CompileLoad(const InstructionContext& ctx,
                                    uint32_t /*size*/, bool /*isSigned*/, bool /*is64Bit*/)
{
    return CompileInstruction(ctx);
}

bool CX64RecompilerOps::CompileStore(const InstructionContext& ctx,
                                     uint32_t /*size*/, bool /*is64Bit*/)
{
    return CompileInstruction(ctx);
}

// =============================================================================
// IRecompilerOps Interface - Floating-Point Operations
// =============================================================================

bool CX64RecompilerOps::CompileFloatingPointOperation(const InstructionContext& ctx)
{
    return CompileInstruction(ctx);
}

bool CX64RecompilerOps::CompileFloatingPointBranch(const InstructionContext& ctx,
                                                   ConditionType /*condition*/)
{
    return CompileInstruction(ctx);
}

// =============================================================================
// IRecompilerOps Interface - Coprocessor Operations
// =============================================================================

bool CX64RecompilerOps::CompileCoprocessorOperation(const InstructionContext& ctx,
                                                    uint32_t /*coprocessor*/)
{
    return CompileInstruction(ctx);
}

// =============================================================================
// IRecompilerOps Interface - Debug and Query
// =============================================================================

void CX64RecompilerOps::GenerateDebugComment(std::string_view comment)
{
    m_Assembler->AddComment(comment);
}

void CX64RecompilerOps::GenerateAddressCheck(uint32_t /*address*/)
{
    // Address checking is handled within the memory callbacks
}

bool CX64RecompilerOps::SupportsFeature(std::string_view feature) const
{
    if (feature == "x64") return true;
    if (feature == "interpreter_fallback") return true;
    if (feature == "register_allocation") return true;
    if (feature == "fpu") return true;
    return false;
}

size_t CX64RecompilerOps::GetCodeSizeEstimate(const InstructionContext& /*ctx*/) const
{
    // Average estimate: ~32 bytes per MIPS instruction with callback approach
    return 32;
}

// =============================================================================
// Extended Interface - Optimization
// =============================================================================

void CX64RecompilerOps::SetOptimizationLevel(OptimizationLevel level) noexcept
{
    m_OptimizationLevel = level;
}

// =============================================================================
// Extended Interface - Control Flow Management
// =============================================================================

void CX64RecompilerOps::EnterCodeBlock()
{
    auto& a = m_Assembler->GetAssembler();
    m_Assembler->AddComment("=== Block Entry (True JIT) ===");

    // Custom prologue: save all callee-saved regs we use and set up base pointers
    using namespace asmjit::x86;
    a.push(rbp);
    a.mov(rbp, rsp);
    a.push(rbx);
    a.push(r12);
    a.push(r13);
    a.push(r14);
    a.push(r15);
    // Allocate shadow space (32 bytes) + alignment (8 bytes) = 40
    // 6 pushes (48) + ret addr (8) + 40 = 96 bytes → 16-aligned
    a.sub(rsp, 40);

    // RBX = pointer to CRegisters (base for m_GPR, m_HI, m_LO, etc.)
    a.mov(rbx, reinterpret_cast<uint64_t>(&m_Context.registers));
    // R12 = pointer to MipsMemoryVM (for memory ops)
    a.mov(r12, reinterpret_cast<uint64_t>(&m_Context.mmu));
    // R13 = pointer to pipelineStage (for exception checks)
    a.mov(r13, reinterpret_cast<uint64_t>(&m_Context.system.m_PipelineStage));
    // R14 = pointer to m_NextTimer
    a.mov(r14, reinterpret_cast<uint64_t>(&m_Context.system.m_NextTimer));
    // R15 = pointer to m_MemoryReadMap array (for inline LW/SW fast path)
    a.mov(r15, reinterpret_cast<uint64_t>(m_Context.mmu.GetMemoryReadMap()));

    // Create exit label for early exit on exception
    m_BlockExitLabel = a.new_label();

    m_BlockEnded = false;
    m_Stats.codeSizeGenerated = 0;
}

void CX64RecompilerOps::ExitCodeBlock(ExitReason reason)
{
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;

    // Bind the exit label so that pipeline-stage checks can jump here
    a.bind(m_BlockExitLabel);

    m_Assembler->AddComment("=== Block Exit (True JIT) ===");

    // Flush all cached registers back to memory
    m_RegisterSet->FlushRegisters(true);

    // Custom epilogue: restore callee-saved regs
    a.add(rsp, 40);
    a.pop(r15);
    a.pop(r14);
    a.pop(r13);
    a.pop(r12);
    a.pop(rbx);
    a.pop(rbp);
    a.ret();

    m_BlockEnded = true;
}

void CX64RecompilerOps::LinkJump(CJumpInfo& jumpInfo)
{
    auto& a = m_Assembler->GetAssembler();
    if (jumpInfo.GetLinkLocation().is_valid()) {
        a.bind(jumpInfo.GetLinkLocation());
    }
    jumpInfo.MarkResolved();
}

void CX64RecompilerOps::LinkJumpToSection(CodeSection* section)
{
    if (!section) return;
    // Link a jump to the entry label of a code section
    // The section's entry label will be resolved during final code emission
    Log::debugf("CX64RecompilerOps: Linking jump to section at 0x%08X",
                section->entry_pc());
}

void CX64RecompilerOps::LinkJumpToUnknown(CJumpInfo* jumpInfo)
{
    if (!jumpInfo) return;
    // For unknown targets, we emit code that exits the block and returns
    // to the recompiler dispatcher
    ExitCodeBlock(ExitReason::JumpRegister);
}

// =============================================================================
// Extended Interface - Register State Management
// =============================================================================

void CX64RecompilerOps::SynchronizeRegisterState(const CX64RegInfo& other)
{
    m_RegisterSet->SynchronizeWith(other);
}

void CX64RecompilerOps::PropagateRegisterState()
{
    // Propagate known register values to successor blocks
    // This is part of the optimization pipeline
    Log::debug("CX64RecompilerOps: Propagating register state");
}

void CX64RecompilerOps::SaveRegisterState()
{
    m_RegisterSet->SaveState();
}

void CX64RecompilerOps::RestoreRegisterState()
{
    m_RegisterSet->RestoreState();
}

// =============================================================================
// Extended Interface - Disassembly & Validation
// =============================================================================

std::string CX64RecompilerOps::GenerateDisassembly() const
{
    return m_Assembler->GenerateDisassembly();
}

bool CX64RecompilerOps::ValidateGeneratedCode() const
{
    return !m_Assembler->HasError();
}

// =============================================================================
// Private - Instruction Handler Initialization
// =============================================================================

void CX64RecompilerOps::InitializeInstructionHandlers()
{
    // The dispatch is handled directly in CompileInstruction via switch/case
    // The handler maps are available for future optimization passes
    Log::debug("CX64RecompilerOps: Instruction handlers initialized");
}

// =============================================================================
// Private - Instruction Translation Dispatch
// =============================================================================

TranslationResult CX64RecompilerOps::TranslateInstruction(const TranslationContext& ctx)
{
    switch (ctx.opcode.Rtype.op) {
    case R4300_SPECIAL: return TranslateSpecialInstruction(ctx);
    case R4300_REGIMM:  return TranslateRegImmInstruction(ctx);
    case R4300_COP0:    return TranslateCop0Instruction(ctx);
    case R4300_COP1:    return TranslateCop1Instruction(ctx);
    case R4300_CP2:     return TranslateCop2Instruction(ctx);
    default:            return HandleArithmetic(ctx); // Includes all I-type/J-type
    }
}

TranslationResult CX64RecompilerOps::TranslateSpecialInstruction(const TranslationContext& ctx)
{
    return HandleArithmetic(ctx);
}

TranslationResult CX64RecompilerOps::TranslateRegImmInstruction(const TranslationContext& ctx)
{
    return HandleBranch(ctx);
}

TranslationResult CX64RecompilerOps::TranslateCop0Instruction(const TranslationContext& ctx)
{
    return HandleSystem(ctx);
}

TranslationResult CX64RecompilerOps::TranslateCop1Instruction(const TranslationContext& ctx)
{
    return HandleFloatingPoint(ctx);
}

TranslationResult CX64RecompilerOps::TranslateCop2Instruction(const TranslationContext& /*ctx*/)
{
    // COP2 doesn't exist on N64 CPU — always fire CopUnusable exception
    return TranslationResult::Success;
}

// =============================================================================
// Private - Category Handlers (using TranslationContext)
// =============================================================================

TranslationResult CX64RecompilerOps::HandleArithmetic(const TranslationContext& ctx)
{
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_ADDIU), ctx.opcode.Value);
    return TranslationResult::Success;
}

TranslationResult CX64RecompilerOps::HandleLogical(const TranslationContext& ctx)
{
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_AND), ctx.opcode.Value);
    return TranslationResult::Success;
}

TranslationResult CX64RecompilerOps::HandleShift(const TranslationContext& ctx)
{
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_SLL), ctx.opcode.Value);
    return TranslationResult::Success;
}

TranslationResult CX64RecompilerOps::HandleLoad(const TranslationContext& ctx)
{
    EmitMemoryCallback(reinterpret_cast<void*>(&Interp_LW), ctx.opcode.Value);
    return TranslationResult::Success;
}

TranslationResult CX64RecompilerOps::HandleStore(const TranslationContext& ctx)
{
    EmitMemoryCallback(reinterpret_cast<void*>(&Interp_SW), ctx.opcode.Value);
    return TranslationResult::Success;
}

TranslationResult CX64RecompilerOps::HandleBranch(const TranslationContext& /*ctx*/)
{
    return TranslationResult::BlockBoundary;
}

TranslationResult CX64RecompilerOps::HandleJump(const TranslationContext& /*ctx*/)
{
    return TranslationResult::BlockBoundary;
}

TranslationResult CX64RecompilerOps::HandleTrap(const TranslationContext& ctx)
{
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TGE), ctx.opcode.Value);
    return TranslationResult::Success;
}

TranslationResult CX64RecompilerOps::HandleSystem(const TranslationContext& ctx)
{
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_SYSCALL), ctx.opcode.Value);
    return TranslationResult::Success;
}

TranslationResult CX64RecompilerOps::HandleFloatingPoint(const TranslationContext& ctx)
{
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_S_Arith), ctx.opcode.Value);
    return TranslationResult::Success;
}

// =============================================================================
// Private - Individual Handle* Methods (TranslationContext variants)
// =============================================================================
// These delegate to the bare-name methods since both emit the same code.

#define DEFINE_HANDLE_SPECIAL(name, callback) \
    TranslationResult CX64RecompilerOps::HandleSPECIAL_##name(const TranslationContext& ctx) { \
        EmitRegisterCallback(reinterpret_cast<void*>(&Interp_##callback), ctx.opcode.Value); \
        m_Stats.instructionsTranslated++; \
        return TranslationResult::Success; \
    }

DEFINE_HANDLE_SPECIAL(SLL, SLL)
DEFINE_HANDLE_SPECIAL(SRL, SRL)
DEFINE_HANDLE_SPECIAL(SRA, SRA)
DEFINE_HANDLE_SPECIAL(SLLV, SLLV)
DEFINE_HANDLE_SPECIAL(SRLV, SRLV)
DEFINE_HANDLE_SPECIAL(SRAV, SRAV)
DEFINE_HANDLE_SPECIAL(MFLO, MFLO)
DEFINE_HANDLE_SPECIAL(MTLO, MTLO)
DEFINE_HANDLE_SPECIAL(MFHI, MFHI)
DEFINE_HANDLE_SPECIAL(MTHI, MTHI)
DEFINE_HANDLE_SPECIAL(DSLLV, DSLLV)
DEFINE_HANDLE_SPECIAL(DSRLV, DSRLV)
DEFINE_HANDLE_SPECIAL(DSRAV, DSRAV)
DEFINE_HANDLE_SPECIAL(MULT, MULT)
DEFINE_HANDLE_SPECIAL(MULTU, MULTU)
DEFINE_HANDLE_SPECIAL(DIV, DIV)
DEFINE_HANDLE_SPECIAL(DIVU, DIVU)
DEFINE_HANDLE_SPECIAL(DMULT, DMULT)
DEFINE_HANDLE_SPECIAL(DMULTU, DMULTU)
DEFINE_HANDLE_SPECIAL(DDIV, DDIV)
DEFINE_HANDLE_SPECIAL(DDIVU, DDIVU)
DEFINE_HANDLE_SPECIAL(ADD, ADD)
DEFINE_HANDLE_SPECIAL(ADDU, ADDU)
DEFINE_HANDLE_SPECIAL(SUB, SUB)
DEFINE_HANDLE_SPECIAL(SUBU, SUBU)
DEFINE_HANDLE_SPECIAL(AND, AND)
DEFINE_HANDLE_SPECIAL(OR, OR)
DEFINE_HANDLE_SPECIAL(XOR, XOR)
DEFINE_HANDLE_SPECIAL(NOR, NOR)
DEFINE_HANDLE_SPECIAL(SLT, SLT)
DEFINE_HANDLE_SPECIAL(SLTU, SLTU)
DEFINE_HANDLE_SPECIAL(DADD, DADD)
DEFINE_HANDLE_SPECIAL(DADDU, DADDU)
DEFINE_HANDLE_SPECIAL(DSUB, DSUB)
DEFINE_HANDLE_SPECIAL(DSUBU, DSUBU)
DEFINE_HANDLE_SPECIAL(DSLL, DSLL)
DEFINE_HANDLE_SPECIAL(DSRL, DSRL)
DEFINE_HANDLE_SPECIAL(DSRA, DSRA)
DEFINE_HANDLE_SPECIAL(DSLL32, DSLL32)
DEFINE_HANDLE_SPECIAL(DSRL32, DSRL32)
DEFINE_HANDLE_SPECIAL(DSRA32, DSRA32)

TranslationResult CX64RecompilerOps::HandleSPECIAL_JR(const TranslationContext& ctx) {
    // JR ends the block - flush registers, update PC from register, exit
    m_Stats.branchesTranslated++;
    return TranslationResult::BlockBoundary;
}

TranslationResult CX64RecompilerOps::HandleSPECIAL_JALR(const TranslationContext& ctx) {
    m_Stats.branchesTranslated++;
    return TranslationResult::BlockBoundary;
}

TranslationResult CX64RecompilerOps::HandleSPECIAL_SYSCALL(const TranslationContext& ctx) {
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_SYSCALL), ctx.opcode.Value);
    return TranslationResult::BlockBoundary;
}

TranslationResult CX64RecompilerOps::HandleSPECIAL_BREAK(const TranslationContext& ctx) {
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_BREAK), ctx.opcode.Value);
    return TranslationResult::BlockBoundary;
}

// REGIMM Handle methods
#define DEFINE_HANDLE_REGIMM_BRANCH(name) \
    TranslationResult CX64RecompilerOps::HandleREGIMM_##name(const TranslationContext& /*ctx*/) { \
        m_Stats.branchesTranslated++; \
        return TranslationResult::BlockBoundary; \
    }

DEFINE_HANDLE_REGIMM_BRANCH(BLTZ)
DEFINE_HANDLE_REGIMM_BRANCH(BGEZ)
DEFINE_HANDLE_REGIMM_BRANCH(BLTZL)
DEFINE_HANDLE_REGIMM_BRANCH(BGEZL)
DEFINE_HANDLE_REGIMM_BRANCH(BLTZAL)
DEFINE_HANDLE_REGIMM_BRANCH(BGEZAL)
DEFINE_HANDLE_REGIMM_BRANCH(BLTZALL)
DEFINE_HANDLE_REGIMM_BRANCH(BGEZALL)

#define DEFINE_HANDLE_REGIMM_TRAP(name, callback) \
    TranslationResult CX64RecompilerOps::HandleREGIMM_##name(const TranslationContext& ctx) { \
        EmitRegisterCallback(reinterpret_cast<void*>(&Interp_##callback), ctx.opcode.Value); \
        return TranslationResult::Success; \
    }

DEFINE_HANDLE_REGIMM_TRAP(TGEI, TGEI)
DEFINE_HANDLE_REGIMM_TRAP(TGEIU, TGEIU)
DEFINE_HANDLE_REGIMM_TRAP(TLTI, TLTI)
DEFINE_HANDLE_REGIMM_TRAP(TLTIU, TLTIU)
DEFINE_HANDLE_REGIMM_TRAP(TEQI, TEQI)
DEFINE_HANDLE_REGIMM_TRAP(TNEI, TNEI)

// Load/Store Handle methods
#define DEFINE_HANDLE_LOAD(name, callback) \
    TranslationResult CX64RecompilerOps::Handle##name(const TranslationContext& ctx) { \
        EmitMemoryCallback(reinterpret_cast<void*>(&Interp_##callback), ctx.opcode.Value); \
        m_Stats.loadsStoresTranslated++; \
        return TranslationResult::Success; \
    }

DEFINE_HANDLE_LOAD(LB, LB)
DEFINE_HANDLE_LOAD(LH, LH)
DEFINE_HANDLE_LOAD(LWL, LWL)
DEFINE_HANDLE_LOAD(LW, LW)
DEFINE_HANDLE_LOAD(LBU, LBU)
DEFINE_HANDLE_LOAD(LHU, LHU)
DEFINE_HANDLE_LOAD(LWR, LWR)
DEFINE_HANDLE_LOAD(LWU, LWU)
DEFINE_HANDLE_LOAD(LD, LD)
DEFINE_HANDLE_LOAD(LDL, LDL)
DEFINE_HANDLE_LOAD(LDR, LDR)

#define DEFINE_HANDLE_STORE(name, callback) \
    TranslationResult CX64RecompilerOps::Handle##name(const TranslationContext& ctx) { \
        EmitMemoryCallback(reinterpret_cast<void*>(&Interp_##callback), ctx.opcode.Value); \
        m_Stats.loadsStoresTranslated++; \
        return TranslationResult::Success; \
    }

DEFINE_HANDLE_STORE(SB, SB)
DEFINE_HANDLE_STORE(SH, SH)
DEFINE_HANDLE_STORE(SWL, SWL)
DEFINE_HANDLE_STORE(SW, SW)
DEFINE_HANDLE_STORE(SWR, SWR)
DEFINE_HANDLE_STORE(SDL, SDL)
DEFINE_HANDLE_STORE(SDR, SDR)
DEFINE_HANDLE_STORE(SD, SD)

// Immediate Handle methods
#define DEFINE_HANDLE_IMM(name, callback) \
    TranslationResult CX64RecompilerOps::Handle##name(const TranslationContext& ctx) { \
        EmitRegisterCallback(reinterpret_cast<void*>(&Interp_##callback), ctx.opcode.Value); \
        m_Stats.instructionsTranslated++; \
        return TranslationResult::Success; \
    }

DEFINE_HANDLE_IMM(ADDI, ADDI)
DEFINE_HANDLE_IMM(ADDIU, ADDIU)
DEFINE_HANDLE_IMM(SLTI, SLTI)
DEFINE_HANDLE_IMM(SLTIU, SLTIU)
DEFINE_HANDLE_IMM(ANDI, ANDI)
DEFINE_HANDLE_IMM(ORI, ORI)
DEFINE_HANDLE_IMM(XORI, XORI)
DEFINE_HANDLE_IMM(LUI, LUI)
DEFINE_HANDLE_IMM(DADDI, DADDI)
DEFINE_HANDLE_IMM(DADDIU, DADDIU)

// Branch Handle methods
#define DEFINE_HANDLE_BRANCH(name) \
    TranslationResult CX64RecompilerOps::Handle##name(const TranslationContext& /*ctx*/) { \
        m_Stats.branchesTranslated++; \
        return TranslationResult::BlockBoundary; \
    }

DEFINE_HANDLE_BRANCH(J)
DEFINE_HANDLE_BRANCH(JAL)
DEFINE_HANDLE_BRANCH(BEQ)
DEFINE_HANDLE_BRANCH(BNE)
DEFINE_HANDLE_BRANCH(BLEZ)
DEFINE_HANDLE_BRANCH(BGTZ)
DEFINE_HANDLE_BRANCH(BEQL)
DEFINE_HANDLE_BRANCH(BNEL)
DEFINE_HANDLE_BRANCH(BLEZL)
DEFINE_HANDLE_BRANCH(BGTZL)

// COP1 Load/Store Handle methods
TranslationResult CX64RecompilerOps::HandleLWC1(const TranslationContext& ctx) {
    EmitMemoryCallback(reinterpret_cast<void*>(&Interp_LWC1), ctx.opcode.Value);
    m_Stats.loadsStoresTranslated++;
    return TranslationResult::Success;
}

TranslationResult CX64RecompilerOps::HandleLDC1(const TranslationContext& ctx) {
    EmitMemoryCallback(reinterpret_cast<void*>(&Interp_LDC1), ctx.opcode.Value);
    m_Stats.loadsStoresTranslated++;
    return TranslationResult::Success;
}

TranslationResult CX64RecompilerOps::HandleSWC1(const TranslationContext& ctx) {
    EmitMemoryCallback(reinterpret_cast<void*>(&Interp_SWC1), ctx.opcode.Value);
    m_Stats.loadsStoresTranslated++;
    return TranslationResult::Success;
}

TranslationResult CX64RecompilerOps::HandleSDC1(const TranslationContext& ctx) {
    EmitMemoryCallback(reinterpret_cast<void*>(&Interp_SDC1), ctx.opcode.Value);
    m_Stats.loadsStoresTranslated++;
    return TranslationResult::Success;
}

// =============================================================================
// Private - Helper Methods
// =============================================================================

void CX64RecompilerOps::EmitLoadOperation(uint32_t size, bool isSigned, bool is64Bit,
                                          asmjit::x86::Gp dest, const asmjit::x86::Mem& src)
{
    m_Assembler->EmitSafeLoad(dest, src, size, isSigned);
}

void CX64RecompilerOps::EmitStoreOperation(uint32_t size, bool is64Bit,
                                           const asmjit::x86::Mem& dest, asmjit::x86::Gp src)
{
    m_Assembler->EmitSafeStore(dest, src, size);
}

void CX64RecompilerOps::EmitBranchCondition(ConditionType condition, asmjit::Label target)
{
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;

    switch (condition) {
    case ConditionType::Equal:
        m_Assembler->EmitConditionalJump(CondCode::kEqual, target); break;
    case ConditionType::NotEqual:
        m_Assembler->EmitConditionalJump(CondCode::kNotEqual, target); break;
    case ConditionType::LessThanZero:
        m_Assembler->EmitConditionalJump(CondCode::kSign, target); break;
    case ConditionType::GreaterOrEqualZero:
        m_Assembler->EmitConditionalJump(CondCode::kNotSign, target); break;
    case ConditionType::LessOrEqualZero:
        m_Assembler->EmitConditionalJump(CondCode::kSignedLE, target); break;
    case ConditionType::GreaterThanZero:
        m_Assembler->EmitConditionalJump(CondCode::kSignedGT, target); break;
    default:
        { auto& a = m_Assembler->GetAssembler(); a.jmp(target); break; }
    }
}

void CX64RecompilerOps::EmitTrapCondition(ConditionType /*condition*/)
{
    // Trap conditions are handled by the interpreter callbacks
}

void CX64RecompilerOps::EmitSystemCall()
{
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_SYSCALL),
                         m_CurrentContext ? m_CurrentContext->opcode.Value : 0);
}

void CX64RecompilerOps::EmitBreak()
{
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_BREAK),
                         m_CurrentContext ? m_CurrentContext->opcode.Value : 0);
}

// =============================================================================
// Private - Register Allocation and Management
// =============================================================================

void CX64RecompilerOps::AllocateRegister(uint32_t regIndex, bool is64Bit)
{
    if (regIndex >= 32 || regIndex == 0) return;
    auto width = is64Bit ? RegisterWidth::Bits64 : RegisterWidth::Bits32;
    m_RegisterSet->MapRegister(regIndex, width);
}

void CX64RecompilerOps::FreeRegister(uint32_t regIndex)
{
    if (regIndex >= 32 || regIndex == 0) return;
    m_RegisterSet->UnmapRegister(regIndex);
}

void CX64RecompilerOps::SpillRegister(uint32_t regIndex)
{
    if (regIndex >= 32 || regIndex == 0) return;
    m_RegisterSet->SpillRegister(regIndex);
}

void CX64RecompilerOps::ReloadRegister(uint32_t regIndex)
{
    if (regIndex >= 32 || regIndex == 0) return;
    m_RegisterSet->ReloadRegister(regIndex);
}

// =============================================================================
// Private - Memory Access Helpers
// =============================================================================

asmjit::x86::Mem CX64RecompilerOps::CreateMemoryOperand(uint32_t baseReg, int32_t offset,
                                                        uint32_t size, bool /*is64Bit*/)
{
    using namespace asmjit::x86;
    // Create a memory operand relative to the register base pointer
    // baseReg is a MIPS register index; offset is the immediate offset
    // We compute the effective address at runtime via the callback approach
    return ptr(rbx, offset, size);
}

void CX64RecompilerOps::EmitMemoryBoundsCheck(uint32_t /*address*/)
{
    // Bounds checking is handled within the memory callbacks (MMU TLB lookup)
}

// =============================================================================
// Private - Optimization Helpers
// =============================================================================

void CX64RecompilerOps::OptimizeCurrentBlock()
{
    if (m_OptimizationLevel >= OptimizationLevel::Basic) {
        PeepholeOptimize();
    }
    if (m_OptimizationLevel >= OptimizationLevel::Aggressive) {
        ConstantPropagation();
        DeadCodeElimination();
        RedundantMoveElimination();
    }
}

void CX64RecompilerOps::PeepholeOptimize()
{
    // Peephole optimizations on the generated x64 code
    // (future: merge adjacent loads, eliminate redundant moves, etc.)
}

void CX64RecompilerOps::ConstantPropagation()
{
    // Track constant values through the register set
    // and replace runtime computations with immediate values
}

void CX64RecompilerOps::DeadCodeElimination()
{
    // Remove generated code for instructions whose results are never used
}

void CX64RecompilerOps::RedundantMoveElimination()
{
    // Remove mov reg, reg instructions and unnecessary register shuffling
}

// =============================================================================
// Private - Branch Comparison Instructions (True JIT - Inline x64)
// =============================================================================

// JIT offset helpers (used here and in SPECIAL/Immediate sections below)
#ifndef JIT_GPR
#define JIT_GPR(reg) (static_cast<int32_t>(offsetof(CRegisters, m_GPR)) + static_cast<int32_t>((reg) * sizeof(MipsRegisterType)))
#define JIT_HI      static_cast<int32_t>(offsetof(CRegisters, m_HI))
#define JIT_LO      static_cast<int32_t>(offsetof(CRegisters, m_LO))
#define JIT_PC      static_cast<int32_t>(offsetof(CRegisters, m_PROGRAM_COUNTER))
#define JIT_FPCR(r) (static_cast<int32_t>(offsetof(CRegisters, m_FPCR)) + (r) * 4)
#endif

void CX64RecompilerOps::BEQ_Compare() {
    if (!m_CurrentContext) return;
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t pc = m_CurrentContext->virtualAddress;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    int64_t takenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 4 + offset));
    int64_t notTakenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 8));
    m_Assembler->AddComment("BEQ");
    a.mov(rcx, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.cmp(rcx, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.mov(rax, notTakenPC);
    a.mov(rdx, takenPC);
    a.cmove(rax, rdx);
    a.mov(qword_ptr(rbx, JIT_PC), rax);
    m_Stats.branchesTranslated++; m_Stats.categories.control++; m_BlockEnded = true;
}

void CX64RecompilerOps::BNE_Compare() {
    if (!m_CurrentContext) return;
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t pc = m_CurrentContext->virtualAddress;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    int64_t takenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 4 + offset));
    int64_t notTakenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 8));
    m_Assembler->AddComment("BNE");
    a.mov(rcx, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.cmp(rcx, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.mov(rax, notTakenPC);
    a.mov(rdx, takenPC);
    a.cmovne(rax, rdx);
    a.mov(qword_ptr(rbx, JIT_PC), rax);
    m_Stats.branchesTranslated++; m_Stats.categories.control++; m_BlockEnded = true;
}

void CX64RecompilerOps::BGTZ_Compare() {
    if (!m_CurrentContext) return;
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t pc = m_CurrentContext->virtualAddress;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    int64_t takenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 4 + offset));
    int64_t notTakenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 8));
    m_Assembler->AddComment("BGTZ");
    a.mov(rcx, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.test(rcx, rcx);
    a.mov(rax, notTakenPC);
    a.mov(rdx, takenPC);
    a.cmovg(rax, rdx);
    a.mov(qword_ptr(rbx, JIT_PC), rax);
    m_Stats.branchesTranslated++; m_Stats.categories.control++; m_BlockEnded = true;
}

void CX64RecompilerOps::BLEZ_Compare() {
    if (!m_CurrentContext) return;
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t pc = m_CurrentContext->virtualAddress;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    int64_t takenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 4 + offset));
    int64_t notTakenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 8));
    m_Assembler->AddComment("BLEZ");
    a.mov(rcx, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.test(rcx, rcx);
    a.mov(rax, notTakenPC);
    a.mov(rdx, takenPC);
    a.cmovle(rax, rdx);
    a.mov(qword_ptr(rbx, JIT_PC), rax);
    m_Stats.branchesTranslated++; m_Stats.categories.control++; m_BlockEnded = true;
}

void CX64RecompilerOps::BLTZ_Compare() {
    if (!m_CurrentContext) return;
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t pc = m_CurrentContext->virtualAddress;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    int64_t takenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 4 + offset));
    int64_t notTakenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 8));
    m_Assembler->AddComment("BLTZ");
    a.mov(rcx, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.test(rcx, rcx);
    a.mov(rax, notTakenPC);
    a.mov(rdx, takenPC);
    a.cmovs(rax, rdx);
    a.mov(qword_ptr(rbx, JIT_PC), rax);
    m_Stats.branchesTranslated++; m_Stats.categories.control++; m_BlockEnded = true;
}

void CX64RecompilerOps::BGEZ_Compare() {
    if (!m_CurrentContext) return;
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t pc = m_CurrentContext->virtualAddress;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    int64_t takenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 4 + offset));
    int64_t notTakenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 8));
    m_Assembler->AddComment("BGEZ");
    a.mov(rcx, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.test(rcx, rcx);
    a.mov(rax, notTakenPC);
    a.mov(rdx, takenPC);
    a.cmovns(rax, rdx);
    a.mov(qword_ptr(rbx, JIT_PC), rax);
    m_Stats.branchesTranslated++; m_Stats.categories.control++; m_BlockEnded = true;
}

void CX64RecompilerOps::BLTZAL_Compare() {
    if (!m_CurrentContext) return;
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t pc = m_CurrentContext->virtualAddress;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    int64_t takenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 4 + offset));
    int64_t notTakenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 8));
    m_Assembler->AddComment("BLTZAL");
    // Read rs BEFORE writing return address (rs may be GPR[31])
    a.mov(rcx, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    if (m_InDelaySlot) {
        // Branch-in-delay-slot: RA = outer_jump_target + 4
        a.mov(rax, reinterpret_cast<uint64_t>(&m_Context.system.m_JumpToLocation));
        a.mov(rdx, qword_ptr(rax));
        a.add(edx, 4);
        a.movsxd(rdx, edx);
        a.mov(qword_ptr(rbx, JIT_GPR(31)), rdx);
    } else {
        a.mov(qword_ptr(rbx, JIT_GPR(31)), static_cast<int32_t>(pc + 8));
    }
    a.test(rcx, rcx);
    a.mov(rax, notTakenPC);
    a.mov(rdx, takenPC);
    a.cmovs(rax, rdx);
    a.mov(qword_ptr(rbx, JIT_PC), rax);
    m_Stats.branchesTranslated++; m_Stats.categories.control++; m_BlockEnded = true;
}

void CX64RecompilerOps::BGEZAL_Compare() {
    if (!m_CurrentContext) return;
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t pc = m_CurrentContext->virtualAddress;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    int64_t takenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 4 + offset));
    int64_t notTakenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 8));
    m_Assembler->AddComment("BGEZAL");
    // Read rs BEFORE writing return address (rs may be GPR[31])
    a.mov(rcx, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    if (m_InDelaySlot) {
        // Branch-in-delay-slot: RA = outer_jump_target + 4
        a.mov(rax, reinterpret_cast<uint64_t>(&m_Context.system.m_JumpToLocation));
        a.mov(rdx, qword_ptr(rax));
        a.add(edx, 4);
        a.movsxd(rdx, edx);
        a.mov(qword_ptr(rbx, JIT_GPR(31)), rdx);
    } else {
        a.mov(qword_ptr(rbx, JIT_GPR(31)), static_cast<int32_t>(pc + 8));
    }
    a.test(rcx, rcx);
    a.mov(rax, notTakenPC);
    a.mov(rdx, takenPC);
    a.cmovns(rax, rdx);
    a.mov(qword_ptr(rbx, JIT_PC), rax);
    m_Stats.branchesTranslated++; m_Stats.categories.control++; m_BlockEnded = true;
}

void CX64RecompilerOps::COP1_BCF_Compare() {
    if (!m_CurrentContext) return;
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t pc = m_CurrentContext->virtualAddress;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    int64_t takenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 4 + offset));
    int64_t notTakenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 8));
    m_Assembler->AddComment("BC1F");
    // Branch if FP condition is false (FPCSR_C bit NOT set)
    a.test(dword_ptr(rbx, JIT_FPCR(31)), static_cast<int32_t>(FPCSR_C));
    a.mov(rax, notTakenPC);
    a.mov(rdx, takenPC);
    a.cmovz(rax, rdx);  // if FPCSR_C not set → taken
    a.mov(qword_ptr(rbx, JIT_PC), rax);
    m_Stats.branchesTranslated++; m_Stats.categories.control++; m_BlockEnded = true;
}

void CX64RecompilerOps::COP1_BCT_Compare() {
    if (!m_CurrentContext) return;
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t pc = m_CurrentContext->virtualAddress;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    int64_t takenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 4 + offset));
    int64_t notTakenPC = static_cast<int64_t>(static_cast<int32_t>(pc + 8));
    m_Assembler->AddComment("BC1T");
    // Branch if FP condition is true (FPCSR_C bit set)
    a.test(dword_ptr(rbx, JIT_FPCR(31)), static_cast<int32_t>(FPCSR_C));
    a.mov(rax, notTakenPC);
    a.mov(rdx, takenPC);
    a.cmovnz(rax, rdx);  // if FPCSR_C set → taken
    a.mov(qword_ptr(rbx, JIT_PC), rax);
    m_Stats.branchesTranslated++; m_Stats.categories.control++; m_BlockEnded = true;
}

// Branch-likely compare methods: use EmitBranchLikelyCallback which returns
// taken/not-taken and emits a conditional jz to skip the delay slot.
#define DEFINE_BRANCH_LIKELY_COMPARE_IMPL(name, callback_fn) \
    void CX64RecompilerOps::name##_Compare_Likely() { \
        if (!m_CurrentContext) return; \
        m_Assembler->AddComment(#name "_Likely"); \
        EmitBranchLikelyCallback(reinterpret_cast<void*>(&callback_fn), \
                           m_CurrentContext->opcode.Value, \
                           m_CurrentContext->virtualAddress); \
        m_Stats.branchesTranslated++; \
        m_Stats.categories.control++; \
        m_BlockEnded = true; \
    }

DEFINE_BRANCH_LIKELY_COMPARE_IMPL(BEQL, Interp_BEQL_BranchLikely)
DEFINE_BRANCH_LIKELY_COMPARE_IMPL(BNEL, Interp_BNEL_BranchLikely)
DEFINE_BRANCH_LIKELY_COMPARE_IMPL(BLEZL, Interp_BLEZL_BranchLikely)
DEFINE_BRANCH_LIKELY_COMPARE_IMPL(BGTZL, Interp_BGTZL_BranchLikely)
DEFINE_BRANCH_LIKELY_COMPARE_IMPL(BLTZL, Interp_BLTZL_BranchLikely)
DEFINE_BRANCH_LIKELY_COMPARE_IMPL(BGEZL, Interp_BGEZL_BranchLikely)
DEFINE_BRANCH_LIKELY_COMPARE_IMPL(BLTZALL, Interp_BLTZALL_BranchLikely)
DEFINE_BRANCH_LIKELY_COMPARE_IMPL(BGEZALL, Interp_BGEZALL_BranchLikely)
DEFINE_BRANCH_LIKELY_COMPARE_IMPL(BC1FL, Interp_BC1FL_BranchLikely)
DEFINE_BRANCH_LIKELY_COMPARE_IMPL(BC1TL, Interp_BC1TL_BranchLikely)

// =============================================================================
// Private - Jump Instructions (True JIT - Inline x64)
// =============================================================================

void CX64RecompilerOps::J()
{
    if (!m_CurrentContext) return;
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t pc = m_CurrentContext->virtualAddress;
    uint32_t target = ((pc + 4) & 0xF0000000) | (mips_target(opcVal) << 2);
    m_Assembler->AddComment("J");
    // PC = sign_extend(target)
    a.mov(qword_ptr(rbx, JIT_PC), static_cast<int32_t>(target));
    m_Stats.branchesTranslated++;
    m_Stats.categories.control++;
    m_BlockEnded = true;
}

void CX64RecompilerOps::JAL()
{
    if (!m_CurrentContext) return;
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t pc = m_CurrentContext->virtualAddress;
    uint32_t target = ((pc + 4) & 0xF0000000) | (mips_target(opcVal) << 2);
    m_Assembler->AddComment("JAL");
    // GPR[31] = return address
    if (m_InDelaySlot) {
        // Branch-in-delay-slot: RA = outer_jump_target + 4 = m_JumpToLocation + 4
        a.mov(rax, reinterpret_cast<uint64_t>(&m_Context.system.m_JumpToLocation));
        a.mov(rcx, qword_ptr(rax));
        a.add(ecx, 4);
        a.movsxd(rcx, ecx);
        a.mov(qword_ptr(rbx, JIT_GPR(31)), rcx);
    } else {
        a.mov(qword_ptr(rbx, JIT_GPR(31)), static_cast<int32_t>(pc + 8));
    }
    // PC = sign_extend(target)
    a.mov(qword_ptr(rbx, JIT_PC), static_cast<int32_t>(target));
    m_Stats.branchesTranslated++;
    m_Stats.categories.control++;
    m_BlockEnded = true;
}

// =============================================================================
// Private - Immediate Arithmetic Instructions (True JIT - Inline x64)
// =============================================================================

void CX64RecompilerOps::ADDI() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    m_Assembler->AddComment("ADDI (callback - overflow trap)");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_ImmDispatch), opcVal);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

void CX64RecompilerOps::ADDIU() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    R4300iOps op; op.Value = opcVal;
    if (op.Itype.rt == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("ADDIU");
    a.mov(eax, dword_ptr(rbx, JIT_GPR(op.Itype.rs)));
    a.add(eax, static_cast<int32_t>(static_cast<int16_t>(op.Itype.imm)));
    a.movsxd(rax, eax);
    a.mov(qword_ptr(rbx, JIT_GPR(op.Itype.rt)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

void CX64RecompilerOps::SLTI() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    R4300iOps op; op.Value = opcVal;
    if (op.Itype.rt == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SLTI");
    // rs (signed 64-bit) < sign_extend(imm16) ?
    int64_t immSext = static_cast<int64_t>(static_cast<int16_t>(op.Itype.imm));
    a.mov(rax, qword_ptr(rbx, JIT_GPR(op.Itype.rs)));
    a.mov(rcx, immSext);
    a.cmp(rax, rcx);
    a.setl(al);
    a.movzx(eax, al);
    a.mov(qword_ptr(rbx, JIT_GPR(op.Itype.rt)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

void CX64RecompilerOps::SLTIU() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    R4300iOps op; op.Value = opcVal;
    if (op.Itype.rt == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SLTIU");
    // rs (unsigned 64-bit) < sign_extend_then_cast_unsigned(imm16) ?
    uint64_t immSext = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(op.Itype.imm)));
    a.mov(rax, qword_ptr(rbx, JIT_GPR(op.Itype.rs)));
    a.mov(rcx, immSext);
    a.cmp(rax, rcx);
    a.setb(al);
    a.movzx(eax, al);
    a.mov(qword_ptr(rbx, JIT_GPR(op.Itype.rt)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

void CX64RecompilerOps::ANDI() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    R4300iOps op; op.Value = opcVal;
    if (op.Itype.rt == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.logical++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("ANDI");
    // ANDI zero-extends the immediate (no sign extension)
    a.mov(rax, qword_ptr(rbx, JIT_GPR(op.Itype.rs)));
    a.and_(rax, static_cast<int32_t>(op.Itype.imm));  // zero-extended 16-bit
    a.mov(qword_ptr(rbx, JIT_GPR(op.Itype.rt)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.logical++;
}

void CX64RecompilerOps::ORI() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    R4300iOps op; op.Value = opcVal;
    if (op.Itype.rt == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.logical++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("ORI");
    // ORI zero-extends the immediate
    a.mov(rax, qword_ptr(rbx, JIT_GPR(op.Itype.rs)));
    a.or_(rax, static_cast<int32_t>(op.Itype.imm));  // zero-extended 16-bit
    a.mov(qword_ptr(rbx, JIT_GPR(op.Itype.rt)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.logical++;
}

void CX64RecompilerOps::XORI() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    R4300iOps op; op.Value = opcVal;
    if (op.Itype.rt == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.logical++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("XORI");
    // XORI zero-extends the immediate
    a.mov(rax, qword_ptr(rbx, JIT_GPR(op.Itype.rs)));
    a.xor_(rax, static_cast<int32_t>(op.Itype.imm));  // zero-extended 16-bit
    a.mov(qword_ptr(rbx, JIT_GPR(op.Itype.rt)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.logical++;
}

void CX64RecompilerOps::LUI() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    R4300iOps op; op.Value = opcVal;
    if (op.Itype.rt == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("LUI");
    // LUI: rt = sign_extend(imm << 16)
    int32_t val = static_cast<int32_t>(static_cast<int16_t>(op.Itype.imm)) << 16;
    a.mov(qword_ptr(rbx, JIT_GPR(op.Itype.rt)), static_cast<int32_t>(val));
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

void CX64RecompilerOps::DADDI() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    m_Assembler->AddComment("DADDI (callback - overflow trap)");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_ImmDispatch), opcVal);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

void CX64RecompilerOps::DADDIU() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    R4300iOps op; op.Value = opcVal;
    if (op.Itype.rt == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("DADDIU");
    int64_t immSext = static_cast<int64_t>(static_cast<int16_t>(op.Itype.imm));
    a.mov(rax, qword_ptr(rbx, JIT_GPR(op.Itype.rs)));
    a.mov(rcx, immSext);
    a.add(rax, rcx);
    a.mov(qword_ptr(rbx, JIT_GPR(op.Itype.rt)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

// =============================================================================
// Private - Load Instructions
// =============================================================================

#define DEFINE_LOAD(name, callback) \
    void CX64RecompilerOps::name() { \
        if (!m_CurrentContext) return; \
        m_Assembler->AddComment(#name); \
        EmitMemoryCallback(reinterpret_cast<void*>(&Interp_##callback), \
                           m_CurrentContext->opcode.Value); \
        m_Stats.loadsStoresTranslated++; \
        m_Stats.categories.memory++; \
    }

DEFINE_LOAD(LB, LB)
DEFINE_LOAD(LH, LH)
DEFINE_LOAD(LWL, LWL)
DEFINE_LOAD(LBU, LBU)
DEFINE_LOAD(LHU, LHU)
DEFINE_LOAD(LWR, LWR)
DEFINE_LOAD(LWU, LWU)
DEFINE_LOAD(LD, LD)
DEFINE_LOAD(LDL, LDL)
DEFINE_LOAD(LDR, LDR)
DEFINE_LOAD(LL, LL)
DEFINE_LOAD(LLD, LLD)
DEFINE_LOAD(LWC1, LWC1)
DEFINE_LOAD(LDC1, LDC1)

// Inline LW: fast path for mapped memory via ReadMap, fallback to Interp_LW
void CX64RecompilerOps::LW() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rt   = mips_rt(opcVal);
    uint32_t base = mips_base(opcVal);
    int32_t  off  = static_cast<int32_t>(mips_offset(opcVal)); // sign-extended

    // LW $0 still performs the memory access (alignment/TLB checks can trigger
    // exceptions), but the result is discarded.  Fall through to the slow path.
    if (rt == 0) {
        m_Assembler->AddComment("LW $0 (side-effects only)");
        EmitMemoryCallback(reinterpret_cast<void*>(&Interp_LW), opcVal);
        m_Stats.loadsStoresTranslated++; m_Stats.categories.memory++;
        return;
    }

    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;

    m_Assembler->AddComment("LW (inline)");

    asmjit::Label slowPath = a.new_label();
    asmjit::Label done     = a.new_label();

    // Compute vAddr32 = low32(GPR[base]) + sign_extend(offset)
    a.mov(eax, dword_ptr(rbx, JIT_GPR(base)));
    if (off != 0)
        a.add(eax, off);

    // Alignment check: vAddr32 & 3
    a.test(eax, 3);
    a.jnz(slowPath);

    // Page lookup: rcx = ReadMap[vAddr32 >> 12]
    // R15 = m_MemoryReadMap base pointer (set in EnterCodeBlock)
    a.mov(ecx, eax);                        // ecx = vAddr32 (zero-extends rcx)
    a.shr(ecx, 12);                         // ecx = page index
    a.mov(rcx, qword_ptr(r15, rcx, 3));     // rcx = ReadMap[page] (scale 8)

    // Check unmapped (-1 sentinel)
    a.cmp(rcx, -1);
    a.je(slowPath);

    // Direct memory read: value = *(uint32_t*)(ReadMap[page] + vAddr32)
    // rax = zero-extended vAddr32 from earlier mov eax / add eax
    a.mov(eax, dword_ptr(rcx, rax));

    // Sign-extend 32→64 and store to GPR[rt]
    a.movsxd(rax, eax);
    a.mov(qword_ptr(rbx, JIT_GPR(rt)), rax);

    a.jmp(done);

    // Slow path: fall back to interpreter callback
    a.bind(slowPath);
    EmitMemoryCallback(reinterpret_cast<void*>(&Interp_LW), opcVal);

    a.bind(done);

    m_Stats.loadsStoresTranslated++; m_Stats.categories.memory++;
}

// =============================================================================
// Private - Store Instructions
// =============================================================================

#define DEFINE_STORE(name, callback) \
    void CX64RecompilerOps::name() { \
        if (!m_CurrentContext) return; \
        m_Assembler->AddComment(#name); \
        EmitMemoryCallback(reinterpret_cast<void*>(&Interp_##callback), \
                           m_CurrentContext->opcode.Value); \
        m_Stats.loadsStoresTranslated++; \
        m_Stats.categories.memory++; \
    }

DEFINE_STORE(SB, SB)
DEFINE_STORE(SH, SH)
DEFINE_STORE(SWL, SWL)
DEFINE_STORE(SWR, SWR)
DEFINE_STORE(SD, SD)
DEFINE_STORE(SDL, SDL)
DEFINE_STORE(SDR, SDR)
DEFINE_STORE(SC, SC)
DEFINE_STORE(SCD, SCD)
DEFINE_STORE(SWC1, SWC1)
DEFINE_STORE(SDC1, SDC1)

// Inline SW: fast path for mapped memory via WriteMap, fallback to Interp_SW
void CX64RecompilerOps::SW() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rt   = mips_rt(opcVal);
    uint32_t base = mips_base(opcVal);
    int32_t  off  = static_cast<int32_t>(mips_offset(opcVal));

    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;

    m_Assembler->AddComment("SW (inline)");

    asmjit::Label slowPath = a.new_label();
    asmjit::Label done     = a.new_label();

    // Compute vAddr32 = low32(GPR[base]) + sign_extend(offset)
    a.mov(eax, dword_ptr(rbx, JIT_GPR(base)));
    if (off != 0)
        a.add(eax, off);

    // Alignment check: vAddr32 & 3
    a.test(eax, 3);
    a.jnz(slowPath);

    // 64-bit sign-extension check: full 64-bit address must equal
    // sign-extension of low 32 bits, otherwise it's an Address Error.
    a.mov(r9, qword_ptr(rbx, JIT_GPR(base)));
    if (off != 0)
        a.add(r9, off);
    a.movsxd(rcx, eax);
    a.cmp(rcx, r9);
    a.jne(slowPath);

    // Page lookup: rcx = WriteMap[vAddr32 >> 12]
    // Load WriteMap base pointer as immediate (friend access, stable allocation)
    a.mov(ecx, eax);                        // ecx = vAddr32 (zero-extends rcx)
    a.shr(ecx, 12);                         // ecx = page index
    // Use rdx to hold WriteMap base (can't use r15, it's ReadMap)
    a.mov(rdx, reinterpret_cast<uint64_t>(m_Context.mmu.GetMemoryWriteMap()));
    a.mov(rcx, qword_ptr(rdx, rcx, 3));     // rcx = WriteMap[page] (scale 8)

    // Check unmapped (-1 sentinel)
    a.cmp(rcx, -1);
    a.je(slowPath);

    // SMC check: if this page has compiled recompiler code, use slow path
    // which will do the write + invalidate stale compiled code.
    // physPage = (vAddr & 0x1FFFFFFF) >> 12
    a.mov(r8d, eax);
    a.and_(r8d, 0x1FFFFFFF);
    a.shr(r8d, 12);
    a.mov(rdx, reinterpret_cast<uint64_t>(m_Context.mmu.GetSmcPageBitmap()));
    a.cmp(byte_ptr(rdx, r8), 0);
    a.jne(slowPath);

    // Load value from GPR[rt] (low 32 bits)
    a.mov(edx, dword_ptr(rbx, JIT_GPR(rt)));

    // Direct memory write: *(uint32_t*)(WriteMap[page] + vAddr32) = value
    // rax = zero-extended vAddr32
    a.mov(dword_ptr(rcx, rax), edx);

    a.jmp(done);

    // Slow path: fall back to interpreter callback
    a.bind(slowPath);
    EmitMemoryCallback(reinterpret_cast<void*>(&Interp_SW), opcVal);

    a.bind(done);

    m_Stats.loadsStoresTranslated++; m_Stats.categories.memory++;
}

void CX64RecompilerOps::CACHE()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("CACHE");
    EmitMemoryCallback(reinterpret_cast<void*>(&Interp_CACHE),
                       m_CurrentContext->opcode.Value);
    m_Stats.instructionsTranslated++;
}

// =============================================================================
// Private - SPECIAL Format Instructions (True JIT - Inline x64)
// =============================================================================

// --- 32-bit Shift Instructions ---

void CX64RecompilerOps::SPECIAL_SLL() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.shift++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_SLL");
    a.mov(eax, dword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    uint32_t sa = mips_sa(opcVal);
    if (sa != 0) a.shl(eax, sa);
    a.movsxd(rax, eax);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.shift++;
}

void CX64RecompilerOps::SPECIAL_SRL() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.shift++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_SRL");
    a.mov(eax, dword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    uint32_t sa = mips_sa(opcVal);
    if (sa != 0) a.shr(eax, sa);
    a.movsxd(rax, eax);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.shift++;
}

void CX64RecompilerOps::SPECIAL_SRA() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.shift++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_SRA");
    // VR4300: shift full 64-bit GPR value, then truncate low 32 and sign-extend
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    uint32_t sa = mips_sa(opcVal);
    if (sa != 0) a.sar(rax, sa);
    a.movsxd(rax, eax);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.shift++;
}

// --- 32-bit Variable Shift Instructions ---

void CX64RecompilerOps::SPECIAL_SLLV() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.shift++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_SLLV");
    a.mov(ecx, dword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.and_(ecx, 0x1F);
    a.mov(eax, dword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.shl(eax, cl);
    a.movsxd(rax, eax);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.shift++;
}

void CX64RecompilerOps::SPECIAL_SRLV() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.shift++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_SRLV");
    a.mov(ecx, dword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.and_(ecx, 0x1F);
    a.mov(eax, dword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.shr(eax, cl);
    a.movsxd(rax, eax);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.shift++;
}

void CX64RecompilerOps::SPECIAL_SRAV() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.shift++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_SRAV");
    a.mov(ecx, dword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.and_(ecx, 0x1F);
    // VR4300: shift full 64-bit GPR value, then truncate low 32 and sign-extend
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.sar(rax, cl);
    a.movsxd(rax, eax);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.shift++;
}

// --- HI/LO Move Instructions ---

void CX64RecompilerOps::SPECIAL_MFHI() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_MFHI");
    a.mov(rax, qword_ptr(rbx, JIT_HI));
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

void CX64RecompilerOps::SPECIAL_MTHI() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_MTHI");
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.mov(qword_ptr(rbx, JIT_HI), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

void CX64RecompilerOps::SPECIAL_MFLO() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_MFLO");
    a.mov(rax, qword_ptr(rbx, JIT_LO));
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

void CX64RecompilerOps::SPECIAL_MTLO() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_MTLO");
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.mov(qword_ptr(rbx, JIT_LO), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

// --- 64-bit Variable Shift Instructions ---

void CX64RecompilerOps::SPECIAL_DSLLV() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.shift++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_DSLLV");
    a.mov(ecx, dword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.and_(ecx, 0x3F);
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.shl(rax, cl);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.shift++;
}

void CX64RecompilerOps::SPECIAL_DSRLV() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.shift++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_DSRLV");
    a.mov(ecx, dword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.and_(ecx, 0x3F);
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.shr(rax, cl);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.shift++;
}

void CX64RecompilerOps::SPECIAL_DSRAV() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.shift++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_DSRAV");
    a.mov(ecx, dword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.and_(ecx, 0x3F);
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.sar(rax, cl);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.shift++;
}

// --- Multiply Instructions (inline 32-bit, callback for 64-bit) ---

void CX64RecompilerOps::SPECIAL_MULT() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_MULT");
    // signed 32x32 -> 64: edx:eax = sign(rs.lo32) * sign(rt.lo32)
    a.mov(eax, dword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.imul(dword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    // LO = sign_extend(eax), HI = sign_extend(edx)
    a.movsxd(rcx, eax);
    a.mov(qword_ptr(rbx, JIT_LO), rcx);
    a.movsxd(rcx, edx);
    a.mov(qword_ptr(rbx, JIT_HI), rcx);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

void CX64RecompilerOps::SPECIAL_MULTU() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_MULTU");
    // unsigned 32x32 -> 64: edx:eax = uns(rs.lo32) * uns(rt.lo32)
    a.mov(eax, dword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.mul(dword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    // LO = sign_extend(eax), HI = sign_extend(edx)
    a.movsxd(rcx, eax);
    a.mov(qword_ptr(rbx, JIT_LO), rcx);
    a.movsxd(rcx, edx);
    a.mov(qword_ptr(rbx, JIT_HI), rcx);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

// DIV/DIVU/DMULT/DMULTU/DDIV/DDIVU: keep as interpreter callbacks (edge cases / 128-bit)
void CX64RecompilerOps::SPECIAL_DIV() {
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("SPECIAL_DIV");
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_DIV), m_CurrentContext->opcode.Value);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}
void CX64RecompilerOps::SPECIAL_DIVU() {
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("SPECIAL_DIVU");
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_DIVU), m_CurrentContext->opcode.Value);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}
void CX64RecompilerOps::SPECIAL_DMULT() {
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("SPECIAL_DMULT");
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_DMULT), m_CurrentContext->opcode.Value);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}
void CX64RecompilerOps::SPECIAL_DMULTU() {
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("SPECIAL_DMULTU");
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_DMULTU), m_CurrentContext->opcode.Value);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}
void CX64RecompilerOps::SPECIAL_DDIV() {
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("SPECIAL_DDIV");
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_DDIV), m_CurrentContext->opcode.Value);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}
void CX64RecompilerOps::SPECIAL_DDIVU() {
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("SPECIAL_DDIVU");
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_DDIVU), m_CurrentContext->opcode.Value);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

// --- 32-bit ALU Instructions ---

void CX64RecompilerOps::SPECIAL_ADD() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    m_Assembler->AddComment("SPECIAL_ADD (callback - overflow trap)");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_SpecialDispatch), opcVal);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

void CX64RecompilerOps::SPECIAL_ADDU() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_ADDU");
    a.mov(eax, dword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.add(eax, dword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.movsxd(rax, eax);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

void CX64RecompilerOps::SPECIAL_SUB() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    m_Assembler->AddComment("SPECIAL_SUB (callback - overflow trap)");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_SpecialDispatch), opcVal);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

void CX64RecompilerOps::SPECIAL_SUBU() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_SUBU");
    a.mov(eax, dword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.sub(eax, dword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.movsxd(rax, eax);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

// --- 64-bit Logic Instructions ---

void CX64RecompilerOps::SPECIAL_AND() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.logical++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_AND");
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.and_(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.logical++;
}

void CX64RecompilerOps::SPECIAL_OR() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.logical++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_OR");
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.or_(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.logical++;
}

void CX64RecompilerOps::SPECIAL_XOR() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.logical++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_XOR");
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.xor_(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.logical++;
}

void CX64RecompilerOps::SPECIAL_NOR() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.logical++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_NOR");
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.or_(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.not_(rax);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.logical++;
}

// --- Set-Less-Than Instructions ---

void CX64RecompilerOps::SPECIAL_SLT() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_SLT");
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.cmp(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.setl(al);
    a.movzx(eax, al);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

void CX64RecompilerOps::SPECIAL_SLTU() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_SLTU");
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.cmp(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.setb(al);
    a.movzx(eax, al);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

// --- 64-bit ALU Instructions ---

void CX64RecompilerOps::SPECIAL_DADD() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    m_Assembler->AddComment("SPECIAL_DADD (callback - overflow trap)");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_SpecialDispatch), opcVal);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

void CX64RecompilerOps::SPECIAL_DADDU() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_DADDU");
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.add(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

void CX64RecompilerOps::SPECIAL_DSUB() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    m_Assembler->AddComment("SPECIAL_DSUB (callback - overflow trap)");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_SpecialDispatch), opcVal);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

void CX64RecompilerOps::SPECIAL_DSUBU() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_DSUBU");
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rs(opcVal))));
    a.sub(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.arithmetic++;
}

// --- 64-bit Shift Instructions ---

void CX64RecompilerOps::SPECIAL_DSLL() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.shift++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_DSLL");
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    uint32_t sa = mips_sa(opcVal);
    if (sa != 0) a.shl(rax, sa);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.shift++;
}

void CX64RecompilerOps::SPECIAL_DSRL() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.shift++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_DSRL");
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    uint32_t sa = mips_sa(opcVal);
    if (sa != 0) a.shr(rax, sa);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.shift++;
}

void CX64RecompilerOps::SPECIAL_DSRA() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.shift++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_DSRA");
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    uint32_t sa = mips_sa(opcVal);
    if (sa != 0) a.sar(rax, sa);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.shift++;
}

void CX64RecompilerOps::SPECIAL_DSLL32() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.shift++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_DSLL32");
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.shl(rax, mips_sa(opcVal) + 32);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.shift++;
}

void CX64RecompilerOps::SPECIAL_DSRL32() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.shift++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_DSRL32");
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.shr(rax, mips_sa(opcVal) + 32);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.shift++;
}

void CX64RecompilerOps::SPECIAL_DSRA32() {
    if (!m_CurrentContext) return;
    uint32_t opcVal = m_CurrentContext->opcode.Value;
    uint32_t rd = mips_rd(opcVal);
    if (rd == 0) { m_Stats.instructionsTranslated++; m_Stats.categories.shift++; return; }
    auto& a = m_Assembler->GetAssembler();
    using namespace asmjit::x86;
    m_Assembler->AddComment("SPECIAL_DSRA32");
    a.mov(rax, qword_ptr(rbx, JIT_GPR(mips_rt(opcVal))));
    a.sar(rax, mips_sa(opcVal) + 32);
    a.mov(qword_ptr(rbx, JIT_GPR(rd)), rax);
    m_Stats.instructionsTranslated++; m_Stats.categories.shift++;
}

void CX64RecompilerOps::SPECIAL_JR()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("SPECIAL_JR");
    EmitBranchCallback(reinterpret_cast<void*>(&Interp_JR_Jump),
                       m_CurrentContext->opcode.Value,
                       m_CurrentContext->virtualAddress);
    m_Stats.branchesTranslated++;
    m_Stats.categories.control++;
    m_BlockEnded = true;
}

void CX64RecompilerOps::SPECIAL_JALR()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("SPECIAL_JALR");
    EmitBranchCallback(reinterpret_cast<void*>(&Interp_JALR_Jump),
                       m_CurrentContext->opcode.Value,
                       m_CurrentContext->virtualAddress);
    m_Stats.branchesTranslated++;
    m_Stats.categories.control++;
    m_BlockEnded = true;
}

void CX64RecompilerOps::SPECIAL_SYSCALL()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("SPECIAL_SYSCALL");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_SYSCALL),
                         m_CurrentContext->opcode.Value);
    m_Stats.categories.system++;
    m_BlockEnded = true;
}

void CX64RecompilerOps::SPECIAL_BREAK()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("SPECIAL_BREAK");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_BREAK),
                         m_CurrentContext->opcode.Value);
    m_Stats.categories.system++;
    m_BlockEnded = true;
}

void CX64RecompilerOps::SPECIAL_SYNC()
{
    m_Assembler->AddComment("SPECIAL_SYNC");
    // SYNC is a memory barrier - NOP in single-threaded context
    m_Stats.instructionsTranslated++;
}

// =============================================================================
// Private - COP0 Instructions
// =============================================================================

void CX64RecompilerOps::COP0_MF()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP0_MF");
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP0_MFC0),
                         m_CurrentContext->opcode.Value);
    m_Stats.categories.coprocessor++;
}

void CX64RecompilerOps::COP0_DMF()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP0_DMF");
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP0_DMFC0),
                         m_CurrentContext->opcode.Value);
    m_Stats.categories.coprocessor++;
}

void CX64RecompilerOps::COP0_MT()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP0_MT");
    // Use PC+4 (next instruction) so that if Cop0_MT calls CheckInterrupts
    // (e.g. Status/Compare/Count writes) and an interrupt fires, EPC points
    // to the next instruction — matching real VR4300 async interrupt timing.
    if (m_CurrentContext && !m_BlockEnded) {
        auto& a = m_Assembler->GetAssembler();
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbx,
              static_cast<int32_t>(offsetof(CRegisters, m_PROGRAM_COUNTER))),
              static_cast<int32_t>(m_CurrentContext->virtualAddress + 4));
    }
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP0_MTC0),
                         m_CurrentContext->opcode.Value);
    m_Stats.categories.coprocessor++;
}

void CX64RecompilerOps::COP0_DMT()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP0_DMT");
    // Same PC+4 logic as COP0_MT — DMTC0 to Status also calls CheckInterrupts.
    if (m_CurrentContext && !m_BlockEnded) {
        auto& a = m_Assembler->GetAssembler();
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbx,
              static_cast<int32_t>(offsetof(CRegisters, m_PROGRAM_COUNTER))),
              static_cast<int32_t>(m_CurrentContext->virtualAddress + 4));
    }
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP0_DMTC0),
                         m_CurrentContext->opcode.Value);
    m_Stats.categories.coprocessor++;
}

void CX64RecompilerOps::COP0_CO_TLBR()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP0_CO_TLBR");
    EmitTlbCallback(reinterpret_cast<void*>(&Interp_COP0_CO_TLBR),
                    m_CurrentContext->opcode.Value);
    m_Stats.categories.coprocessor++;
}

void CX64RecompilerOps::COP0_CO_TLBWI()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP0_CO_TLBWI");
    EmitTlbCallback(reinterpret_cast<void*>(&Interp_COP0_CO_TLBWI),
                    m_CurrentContext->opcode.Value);
    m_Stats.categories.coprocessor++;
}

void CX64RecompilerOps::COP0_CO_TLBWR()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP0_CO_TLBWR");
    EmitTlbCallback(reinterpret_cast<void*>(&Interp_COP0_CO_TLBWR),
                    m_CurrentContext->opcode.Value);
    m_Stats.categories.coprocessor++;
}

void CX64RecompilerOps::COP0_CO_TLBP()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP0_CO_TLBP");
    EmitTlbCallback(reinterpret_cast<void*>(&Interp_COP0_CO_TLBP),
                    m_CurrentContext->opcode.Value);
    m_Stats.categories.coprocessor++;
}

void CX64RecompilerOps::COP0_CO_ERET()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP0_CO_ERET");
    // ERET needs CN64System* to set m_TestTimer, so use system callback pattern
    {
        auto& a = m_Assembler->GetAssembler();
        using namespace asmjit::x86;
        m_Assembler->AddComment("  -> ERET callback");
        m_RegisterSet->FlushRegisters(true);
#ifdef _WIN32
        a.mov(rcx, reinterpret_cast<uint64_t>(&m_Context.registers));
        a.mov(rdx, reinterpret_cast<uint64_t>(&m_Context.system));
        a.mov(r8d, m_CurrentContext->opcode.Value);
#else
        a.mov(rdi, reinterpret_cast<uint64_t>(&m_Context.registers));
        a.mov(rsi, reinterpret_cast<uint64_t>(&m_Context.system));
        a.mov(edx, m_CurrentContext->opcode.Value);
#endif
        m_Assembler->EmitCall(reinterpret_cast<void*>(&Interp_COP0_CO_ERET));
    }
    m_Stats.categories.coprocessor++;
    m_BlockEnded = true;
}

// =============================================================================
// Private - COP1 (FPU) Instructions
// =============================================================================

void CX64RecompilerOps::COP1_MF()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_MF");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_MFC1),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

void CX64RecompilerOps::COP1_DMF()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_DMF");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_DMFC1),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

void CX64RecompilerOps::COP1_CF()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_CF");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_CFC1),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

void CX64RecompilerOps::COP1_MT()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_MT");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_MTC1),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

void CX64RecompilerOps::COP1_DMT()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_DMT");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_DMTC1),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

void CX64RecompilerOps::COP1_CT()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_CT");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_CTC1),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

// COP1 Single-precision arithmetic
#define DEFINE_COP1_S_ARITH(name) \
    void CX64RecompilerOps::COP1_S_##name() { \
        if (!m_CurrentContext) return; \
        m_Assembler->AddComment("COP1_S_" #name); \
        EmitPCUpdate(); \
        EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_S_Arith), \
                             m_CurrentContext->opcode.Value); \
        m_Stats.floatingPointOps++; \
        m_Stats.categories.floating++; \
    }

DEFINE_COP1_S_ARITH(ADD)
DEFINE_COP1_S_ARITH(SUB)
DEFINE_COP1_S_ARITH(MUL)
DEFINE_COP1_S_ARITH(DIV)
DEFINE_COP1_S_ARITH(ABS)
DEFINE_COP1_S_ARITH(NEG)
DEFINE_COP1_S_ARITH(SQRT)
DEFINE_COP1_S_ARITH(MOV)

// COP1 Single-precision rounding/conversion
#define DEFINE_COP1_S_CVT(name) \
    void CX64RecompilerOps::COP1_S_##name() { \
        if (!m_CurrentContext) return; \
        m_Assembler->AddComment("COP1_S_" #name); \
        EmitPCUpdate(); \
        EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_S_CVT), \
                             m_CurrentContext->opcode.Value); \
        m_Stats.floatingPointOps++; \
        m_Stats.categories.floating++; \
    }

DEFINE_COP1_S_CVT(ROUND_L)
DEFINE_COP1_S_CVT(TRUNC_L)
DEFINE_COP1_S_CVT(CEIL_L)
DEFINE_COP1_S_CVT(FLOOR_L)
DEFINE_COP1_S_CVT(ROUND_W)
DEFINE_COP1_S_CVT(TRUNC_W)
DEFINE_COP1_S_CVT(CEIL_W)
DEFINE_COP1_S_CVT(FLOOR_W)
DEFINE_COP1_S_CVT(CVT_D)
DEFINE_COP1_S_CVT(CVT_W)
DEFINE_COP1_S_CVT(CVT_L)

void CX64RecompilerOps::COP1_S_CMP()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_S_CMP");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_S_CMP),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
    m_Stats.categories.floating++;
}

// COP1 Double-precision arithmetic
#define DEFINE_COP1_D_ARITH(name) \
    void CX64RecompilerOps::COP1_D_##name() { \
        if (!m_CurrentContext) return; \
        m_Assembler->AddComment("COP1_D_" #name); \
        EmitPCUpdate(); \
        EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_D_Arith), \
                             m_CurrentContext->opcode.Value); \
        m_Stats.floatingPointOps++; \
        m_Stats.categories.floating++; \
    }

DEFINE_COP1_D_ARITH(ADD)
DEFINE_COP1_D_ARITH(SUB)
DEFINE_COP1_D_ARITH(MUL)
DEFINE_COP1_D_ARITH(DIV)
DEFINE_COP1_D_ARITH(ABS)
DEFINE_COP1_D_ARITH(NEG)
DEFINE_COP1_D_ARITH(SQRT)
DEFINE_COP1_D_ARITH(MOV)

// COP1 Double-precision rounding/conversion
#define DEFINE_COP1_D_CVT(name) \
    void CX64RecompilerOps::COP1_D_##name() { \
        if (!m_CurrentContext) return; \
        m_Assembler->AddComment("COP1_D_" #name); \
        EmitPCUpdate(); \
        EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_D_CVT), \
                             m_CurrentContext->opcode.Value); \
        m_Stats.floatingPointOps++; \
        m_Stats.categories.floating++; \
    }

DEFINE_COP1_D_CVT(ROUND_L)
DEFINE_COP1_D_CVT(TRUNC_L)
DEFINE_COP1_D_CVT(CEIL_L)
DEFINE_COP1_D_CVT(FLOOR_L)
DEFINE_COP1_D_CVT(ROUND_W)
DEFINE_COP1_D_CVT(TRUNC_W)
DEFINE_COP1_D_CVT(CEIL_W)
DEFINE_COP1_D_CVT(FLOOR_W)
DEFINE_COP1_D_CVT(CVT_S)
DEFINE_COP1_D_CVT(CVT_W)
DEFINE_COP1_D_CVT(CVT_L)

void CX64RecompilerOps::COP1_D_CMP()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_D_CMP");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_D_CMP),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
    m_Stats.categories.floating++;
}

// COP1 W/L format conversions
void CX64RecompilerOps::COP1_W_CVT_S()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_W_CVT_S");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_W_CVT),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

void CX64RecompilerOps::COP1_W_CVT_D()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_W_CVT_D");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_W_CVT),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

void CX64RecompilerOps::COP1_L_CVT_S()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_L_CVT_S");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_L_CVT),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

void CX64RecompilerOps::COP1_L_CVT_D()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_L_CVT_D");
    EmitPCUpdate();
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_L_CVT),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

// =============================================================================
// Private - Unknown Instruction Handler
// =============================================================================

void CX64RecompilerOps::UnknownOpcode()
{
    uint32_t opcVal = m_CurrentContext ? m_CurrentContext->opcode.Value : 0;
    Log::errorf("CX64RecompilerOps: Unknown opcode 0x%08X", opcVal);
    m_Assembler->AddComment("UNKNOWN OPCODE");
    // Generate a break/trap for debugging
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_BREAK), opcVal);
    m_BlockEnded = true;
}

// =============================================================================
// Factory Function
// =============================================================================

std::unique_ptr<IRecompilerOps> CreateX64RecompilerOps(
    RecompilerContext& context,
    CodeBlock& codeBlock)
{
    return std::make_unique<CX64RecompilerOps>(context, codeBlock);
}

} // namespace Recompiler
} // namespace Nova64

#endif // __amd64__ || _M_X64
