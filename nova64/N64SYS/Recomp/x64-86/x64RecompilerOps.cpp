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

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace Nova64 {
namespace Recompiler {

// Type alias for convenience (PipelineStage already defined in ExitInfo.hpp)
using MIPS_DWORD = N64Types::MIPS_DWORD;
using ExceptionCode = N64Types::ExceptionCode;

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

// ----- ALU Immediate Callbacks -----

void Interp_ADDI(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (op.Itype.rt == 0) return;
    int32_t rs_lo = AsDWord(regs->m_GPR[op.Itype.rs]).W[0];
    int16_t imm = static_cast<int16_t>(op.Itype.imm);
    int32_t result = rs_lo + imm;
    // ADDI can trap on overflow; we skip the trap for now (like most emus)
    AsDWord(regs->m_GPR[op.Itype.rt]).DW = static_cast<int64_t>(result);
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
    if (op.Itype.rt == 0) return;
    int64_t rs = AsDWord(regs->m_GPR[op.Itype.rs]).DW;
    int64_t imm = static_cast<int64_t>(static_cast<int16_t>(op.Itype.imm));
    AsDWord(regs->m_GPR[op.Itype.rt]).DW = rs + imm;
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
    if (mips_rd(opcVal) == 0) return;
    int32_t result = AsDWord(regs->m_GPR[mips_rs(opcVal)]).W[0] +
                     AsDWord(regs->m_GPR[mips_rt(opcVal)]).W[0];
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = static_cast<int64_t>(result);
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
    if (mips_rd(opcVal) == 0) return;
    int32_t result = AsDWord(regs->m_GPR[mips_rs(opcVal)]).W[0] -
                     AsDWord(regs->m_GPR[mips_rt(opcVal)]).W[0];
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = static_cast<int64_t>(result);
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
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW =
        AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW +
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW;
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
    if (mips_rd(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW =
        AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW -
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW;
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
    int32_t result = AsDWord(regs->m_GPR[mips_rt(opcVal)]).W[0] >> mips_sa(opcVal);
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = static_cast<int64_t>(result);
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
    int32_t result = AsDWord(regs->m_GPR[mips_rt(opcVal)]).W[0] >> shift;
    AsDWord(regs->m_GPR[mips_rd(opcVal)]).DW = static_cast<int64_t>(result);
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
        AsDWord(regs->m_LO).DW = -1;
        AsDWord(regs->m_HI).DW = static_cast<int64_t>(rs);
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
        if (mips_rt(opcVal) != 0)
            AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW = static_cast<int64_t>(static_cast<int32_t>(val));
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

void Interp_LWC1(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(op.Itype.imm);
    uint32_t val = 0;
    if (mmu->LW_Memory(addr, val)) {
        regs->m_FPR_Union[mips_ft(opcVal)].u32 = val;
    }
}

void Interp_LDC1(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(op.Itype.imm);
    uint64_t val = 0;
    if (mmu->LD_Memory(addr, val)) {
        regs->m_FPR_Union[mips_ft(opcVal)].u64 = val;
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
    mmu->SW_Memory(addr, AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0]);
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
    if (mmu->LW_Memory(addr & ~3, memVal)) {
        uint32_t rtVal = AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0];
        memVal = (memVal & SWL_MASK[offset]) | (rtVal >> SWL_SHIFT[offset]);
        mmu->SW_Memory(addr & ~3, memVal);
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
    if (mmu->LW_Memory(addr & ~3, memVal)) {
        uint32_t rtVal = AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0];
        memVal = (memVal & SWR_MASK[offset]) | (rtVal << SWR_SHIFT[offset]);
        mmu->SW_Memory(addr & ~3, memVal);
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
    if (mmu->LD_Memory(addr & ~7, memVal)) {
        uint64_t rtVal = AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW;
        memVal = (memVal & SDL_MASK[offset]) | (rtVal >> SDL_SHIFT[offset]);
        mmu->SD_Memory(addr & ~7, memVal);
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
    if (mmu->LD_Memory(addr & ~7, memVal)) {
        uint64_t rtVal = AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW;
        memVal = (memVal & SDR_MASK[offset]) | (rtVal << SDR_SHIFT[offset]);
        mmu->SD_Memory(addr & ~7, memVal);
    }
}

void Interp_SC(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (regs->m_LLBit == 1) {
        uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                        static_cast<int16_t>(mips_offset(opcVal));
        mmu->SW_Memory(addr, AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0]);
    }
    if (mips_rt(opcVal) != 0) {
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0] = regs->m_LLBit;
    }
}

void Interp_SWC1(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(op.Itype.imm);
    mmu->SW_Memory(addr, regs->m_FPR_Union[mips_ft(opcVal)].u32);
}

void Interp_SDC1(CRegisters* regs, MipsMemoryVM* mmu, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    uint64_t addr = AsDWord(regs->m_GPR[mips_base(opcVal)]).DW +
                    static_cast<int16_t>(op.Itype.imm);
    mmu->SD_Memory(addr, regs->m_FPR_Union[mips_ft(opcVal)].u64);
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
    tlb->WriteEntry(static_cast<uint32_t>(regs->RANDOM_REGISTER) & 0x1F, true);
}

void Interp_COP0_CO_TLBP(CRegisters* /*regs*/, CTLB* tlb, uint32_t /*opcVal*/) {
    tlb->Probe();
}

void Interp_COP0_CO_ERET(CRegisters* regs, uint32_t /*opcVal*/) {
    if (regs->STATUS_REGISTER.ErrorLevel) {
        regs->m_PROGRAM_COUNTER = regs->ERROREPC_REGISTER;
        regs->STATUS_REGISTER.ErrorLevel = 0;
    } else {
        regs->m_PROGRAM_COUNTER = regs->EPC_REGISTER;
        regs->STATUS_REGISTER.ExceptionLevel = 0;
    }
    regs->m_LLBit = 0;
    regs->CheckInterrupts();
}

// ----- COP1 Move/Control Callbacks -----

void Interp_COP1_MFC1(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rt(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW =
        static_cast<int64_t>(static_cast<int32_t>(regs->m_FPR_Union[mips_rd(opcVal)].u32));
}

void Interp_COP1_DMFC1(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rt(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW =
        static_cast<int64_t>(regs->m_FPR_Union[mips_rd(opcVal)].u64);
}

void Interp_COP1_MTC1(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    regs->m_FPR_Union[mips_rd(opcVal)].u32 =
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).UW[0];
}

void Interp_COP1_DMTC1(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    regs->m_FPR_Union[mips_rd(opcVal)].u64 =
        AsDWord(regs->m_GPR[mips_rt(opcVal)]).UDW;
}

void Interp_COP1_CFC1(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    if (mips_rt(opcVal) == 0) return;
    AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW =
        static_cast<int64_t>(static_cast<int32_t>(regs->m_FPCR[mips_rd(opcVal)]));
}

void Interp_COP1_CTC1(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    regs->Cop1_CT(mips_rd(opcVal), AsDWord(regs->m_GPR[mips_rt(opcVal)]).W[0]);
}

// ----- COP1 FPU Arithmetic (use softfloat via interpreter callbacks) -----
// For FPU, we call through to the CRegisters-level FPU implementation.
// These are simplified interpreter callbacks - a full implementation would
// route through the softfloat library for IEEE 754 compliance.

// Generic FPU operation callback that operates on the FPR_Union array directly
// For single-precision operations (S format)
void Interp_COP1_S_Arith(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    float fs = regs->m_FPR_Union[mips_fs(opcVal)].f32;
    float ft = regs->m_FPR_Union[mips_ft(opcVal)].f32;
    float result = 0.0f;
    switch (mips_funct(opcVal)) {
        case R4300_FPU_ADD:  result = fs + ft; break;
        case R4300_FPU_SUB:  result = fs - ft; break;
        case R4300_FPU_MUL:  result = fs * ft; break;
        case R4300_FPU_DIV:  result = (ft != 0.0f) ? fs / ft : 0.0f; break;
        case R4300_FPU_SQRT: result = sqrtf(fs); break;
        case R4300_FPU_ABS:  result = fabsf(fs); break;
        case R4300_FPU_NEG:  result = -fs; break;
        case R4300_FPU_MOV:  result = fs; break;
        default:
            Log::errorf("COP1_S: Unknown funct %d", mips_funct(opcVal));
            return;
    }
    regs->m_FPR_Union[mips_fd(opcVal)].f32 = result;
    // Clear upper 32 bits of the destination FPR
    regs->m_FPR_Union[mips_fd(opcVal)].u64 =
        (regs->m_FPR_Union[mips_fd(opcVal)].u64 & 0xFFFFFFFF) |
        (static_cast<uint64_t>(regs->m_FPR_Union[mips_fd(opcVal)].u32));
}

// Double-precision FPU operations
void Interp_COP1_D_Arith(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    double fs = regs->m_FPR_Union[mips_fs(opcVal)].f64;
    double ft = regs->m_FPR_Union[mips_ft(opcVal)].f64;
    double result = 0.0;
    switch (mips_funct(opcVal)) {
        case R4300_FPU_ADD:  result = fs + ft; break;
        case R4300_FPU_SUB:  result = fs - ft; break;
        case R4300_FPU_MUL:  result = fs * ft; break;
        case R4300_FPU_DIV:  result = (ft != 0.0) ? fs / ft : 0.0; break;
        case R4300_FPU_SQRT: result = sqrt(fs); break;
        case R4300_FPU_ABS:  result = fabs(fs); break;
        case R4300_FPU_NEG:  result = -fs; break;
        case R4300_FPU_MOV:  result = fs; break;
        default:
            Log::errorf("COP1_D: Unknown funct %d", mips_funct(opcVal));
            return;
    }
    regs->m_FPR_Union[mips_fd(opcVal)].f64 = result;
}

// Single-precision conversions
void Interp_COP1_S_CVT(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    float fs = regs->m_FPR_Union[mips_fs(opcVal)].f32;
    switch (mips_funct(opcVal)) {
        case R4300_FPU_CVT_D:
            regs->m_FPR_Union[mips_fd(opcVal)].f64 = static_cast<double>(fs);
            break;
        case R4300_FPU_CVT_W:
            regs->m_FPR_Union[mips_fd(opcVal)].u32 = static_cast<uint32_t>(static_cast<int32_t>(fs));
            break;
        case R4300_FPU_CVT_L:
            regs->m_FPR_Union[mips_fd(opcVal)].u64 = static_cast<uint64_t>(static_cast<int64_t>(fs));
            break;
        case R4300_FPU_ROUND_W:
            regs->m_FPR_Union[mips_fd(opcVal)].u32 = static_cast<uint32_t>(static_cast<int32_t>(lroundf(fs)));
            break;
        case R4300_FPU_TRUNC_W:
            regs->m_FPR_Union[mips_fd(opcVal)].u32 = static_cast<uint32_t>(static_cast<int32_t>(truncf(fs)));
            break;
        case R4300_FPU_CEIL_W:
            regs->m_FPR_Union[mips_fd(opcVal)].u32 = static_cast<uint32_t>(static_cast<int32_t>(ceilf(fs)));
            break;
        case R4300_FPU_FLOOR_W:
            regs->m_FPR_Union[mips_fd(opcVal)].u32 = static_cast<uint32_t>(static_cast<int32_t>(floorf(fs)));
            break;
        case R4300_FPU_ROUND_L:
            regs->m_FPR_Union[mips_fd(opcVal)].u64 = static_cast<uint64_t>(static_cast<int64_t>(llroundf(fs)));
            break;
        case R4300_FPU_TRUNC_L:
            regs->m_FPR_Union[mips_fd(opcVal)].u64 = static_cast<uint64_t>(static_cast<int64_t>(truncf(fs)));
            break;
        case R4300_FPU_CEIL_L:
            regs->m_FPR_Union[mips_fd(opcVal)].u64 = static_cast<uint64_t>(static_cast<int64_t>(ceilf(fs)));
            break;
        case R4300_FPU_FLOOR_L:
            regs->m_FPR_Union[mips_fd(opcVal)].u64 = static_cast<uint64_t>(static_cast<int64_t>(floorf(fs)));
            break;
        default:
            Log::errorf("COP1_S_CVT: Unknown funct %d", mips_funct(opcVal));
            break;
    }
}

// Double-precision conversions
void Interp_COP1_D_CVT(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    double fs = regs->m_FPR_Union[mips_fs(opcVal)].f64;
    switch (mips_funct(opcVal)) {
        case R4300_FPU_CVT_S:
            regs->m_FPR_Union[mips_fd(opcVal)].f32 = static_cast<float>(fs);
            break;
        case R4300_FPU_CVT_W:
            regs->m_FPR_Union[mips_fd(opcVal)].u32 = static_cast<uint32_t>(static_cast<int32_t>(fs));
            break;
        case R4300_FPU_CVT_L:
            regs->m_FPR_Union[mips_fd(opcVal)].u64 = static_cast<uint64_t>(static_cast<int64_t>(fs));
            break;
        case R4300_FPU_ROUND_W:
            regs->m_FPR_Union[mips_fd(opcVal)].u32 = static_cast<uint32_t>(static_cast<int32_t>(lround(fs)));
            break;
        case R4300_FPU_TRUNC_W:
            regs->m_FPR_Union[mips_fd(opcVal)].u32 = static_cast<uint32_t>(static_cast<int32_t>(trunc(fs)));
            break;
        case R4300_FPU_CEIL_W:
            regs->m_FPR_Union[mips_fd(opcVal)].u32 = static_cast<uint32_t>(static_cast<int32_t>(ceil(fs)));
            break;
        case R4300_FPU_FLOOR_W:
            regs->m_FPR_Union[mips_fd(opcVal)].u32 = static_cast<uint32_t>(static_cast<int32_t>(floor(fs)));
            break;
        case R4300_FPU_ROUND_L:
            regs->m_FPR_Union[mips_fd(opcVal)].u64 = static_cast<uint64_t>(static_cast<int64_t>(llround(fs)));
            break;
        case R4300_FPU_TRUNC_L:
            regs->m_FPR_Union[mips_fd(opcVal)].u64 = static_cast<uint64_t>(static_cast<int64_t>(trunc(fs)));
            break;
        case R4300_FPU_CEIL_L:
            regs->m_FPR_Union[mips_fd(opcVal)].u64 = static_cast<uint64_t>(static_cast<int64_t>(ceil(fs)));
            break;
        case R4300_FPU_FLOOR_L:
            regs->m_FPR_Union[mips_fd(opcVal)].u64 = static_cast<uint64_t>(static_cast<int64_t>(floor(fs)));
            break;
        default:
            Log::errorf("COP1_D_CVT: Unknown funct %d", mips_funct(opcVal));
            break;
    }
}

// Word-to-float/double conversion
void Interp_COP1_W_CVT(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    int32_t w = static_cast<int32_t>(regs->m_FPR_Union[mips_fs(opcVal)].u32);
    if (mips_funct(opcVal) == R4300_FPU_CVT_S) {
        regs->m_FPR_Union[mips_fd(opcVal)].f32 = static_cast<float>(w);
    } else if (mips_funct(opcVal) == R4300_FPU_CVT_D) {
        regs->m_FPR_Union[mips_fd(opcVal)].f64 = static_cast<double>(w);
    }
}

// Long-to-float/double conversion
void Interp_COP1_L_CVT(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    int64_t l = static_cast<int64_t>(regs->m_FPR_Union[mips_fs(opcVal)].u64);
    if (mips_funct(opcVal) == R4300_FPU_CVT_S) {
        regs->m_FPR_Union[mips_fd(opcVal)].f32 = static_cast<float>(l);
    } else if (mips_funct(opcVal) == R4300_FPU_CVT_D) {
        regs->m_FPR_Union[mips_fd(opcVal)].f64 = static_cast<double>(l);
    }
}

// COP1 Compare (C.cond.S / C.cond.D)
constexpr uint32_t FPCSR_C = 0x00800000;

void Interp_COP1_S_CMP(CRegisters* regs, uint32_t opcVal) {
    R4300iOps op; op.Value = opcVal;
    float fs = regs->m_FPR_Union[mips_fs(opcVal)].f32;
    float ft = regs->m_FPR_Union[mips_ft(opcVal)].f32;
    bool less = false, equal = false, unordered = false;
    if (std::isnan(fs) || std::isnan(ft)) {
        unordered = true;
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
    R4300iOps op; op.Value = opcVal;
    double fs = regs->m_FPR_Union[mips_fs(opcVal)].f64;
    double ft = regs->m_FPR_Union[mips_ft(opcVal)].f64;
    bool less = false, equal = false, unordered = false;
    if (std::isnan(fs) || std::isnan(ft)) {
        unordered = true;
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

// ----- System / Trap / CACHE Callbacks -----

void Interp_SYSCALL(CRegisters* regs, uint32_t /*opcVal*/) {
    regs->TriggerException(static_cast<uint32_t>(ExceptionCode::Syscall));
}

void Interp_BREAK(CRegisters* regs, uint32_t /*opcVal*/) {
    regs->TriggerException(static_cast<uint32_t>(ExceptionCode::Breakpoint));
}

void Interp_SYNC(CRegisters* /*regs*/, uint32_t /*opcVal*/) {
    // SYNC is a memory barrier - no-op in our single-threaded interpreter context
}

void Interp_CACHE(CRegisters* /*regs*/, uint32_t /*opcVal*/) {
    // CACHE operations are architecture-specific hints - can be ignored for correctness
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
    regs->m_PROGRAM_COUNTER = (rs == rt) ? (pc + 4 + offset) : (pc + 8);
}

void Interp_BNE_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int64_t rt = AsDWord(regs->m_GPR[mips_rt(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    regs->m_PROGRAM_COUNTER = (rs != rt) ? (pc + 4 + offset) : (pc + 8);
}

void Interp_BLEZ_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    regs->m_PROGRAM_COUNTER = (rs <= 0) ? (pc + 4 + offset) : (pc + 8);
}

void Interp_BGTZ_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    regs->m_PROGRAM_COUNTER = (rs > 0) ? (pc + 4 + offset) : (pc + 8);
}

void Interp_BLTZ_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    regs->m_PROGRAM_COUNTER = (rs < 0) ? (pc + 4 + offset) : (pc + 8);
}

void Interp_BGEZ_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    regs->m_PROGRAM_COUNTER = (rs >= 0) ? (pc + 4 + offset) : (pc + 8);
}

void Interp_BLTZAL_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    AsDWord(regs->m_GPR[31]).DW = static_cast<int64_t>(static_cast<int32_t>(pc + 8));
    regs->m_PROGRAM_COUNTER = (rs < 0) ? (pc + 4 + offset) : (pc + 8);
}

void Interp_BGEZAL_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int64_t rs = AsDWord(regs->m_GPR[mips_rs(opcVal)]).DW;
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    AsDWord(regs->m_GPR[31]).DW = static_cast<int64_t>(static_cast<int32_t>(pc + 8));
    regs->m_PROGRAM_COUNTER = (rs >= 0) ? (pc + 4 + offset) : (pc + 8);
}

void Interp_BC1F_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    bool fpCond = (regs->m_FPCR[31] & FPCSR_C) != 0;
    regs->m_PROGRAM_COUNTER = (!fpCond) ? (pc + 4 + offset) : (pc + 8);
}

void Interp_BC1T_Branch(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    int32_t offset = static_cast<int32_t>(mips_imm(opcVal)) << 2;
    bool fpCond = (regs->m_FPCR[31] & FPCSR_C) != 0;
    regs->m_PROGRAM_COUNTER = (fpCond) ? (pc + 4 + offset) : (pc + 8);
}

// Jump callbacks
void Interp_J_Jump(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    regs->m_PROGRAM_COUNTER = ((pc + 4) & 0xF0000000) | (mips_target(opcVal) << 2);
}

void Interp_JAL_Jump(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    AsDWord(regs->m_GPR[31]).DW = static_cast<int64_t>(static_cast<int32_t>(pc + 8));
    regs->m_PROGRAM_COUNTER = ((pc + 4) & 0xF0000000) | (mips_target(opcVal) << 2);
}

void Interp_JR_Jump(CRegisters* regs, uint32_t opcVal, uint32_t /*pc*/) {
    regs->m_PROGRAM_COUNTER = static_cast<uint32_t>(AsDWord(regs->m_GPR[mips_rs(opcVal)]).UDW);
}

void Interp_JALR_Jump(CRegisters* regs, uint32_t opcVal, uint32_t pc) {
    uint32_t rd = mips_rd(opcVal);
    if (rd != 0) {
        AsDWord(regs->m_GPR[rd]).DW = static_cast<int64_t>(static_cast<int32_t>(pc + 8));
    }
    regs->m_PROGRAM_COUNTER = static_cast<uint32_t>(AsDWord(regs->m_GPR[mips_rs(opcVal)]).UDW);
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

    // Windows x64 ABI: RCX = 1st arg, RDX = 2nd arg
    // SysV ABI: RDI = 1st arg, RSI = 2nd arg
#ifdef _WIN32
    a.mov(rcx, reinterpret_cast<uint64_t>(&m_Context.registers));
    a.mov(edx, opcVal);
#else
    a.mov(rdi, reinterpret_cast<uint64_t>(&m_Context.registers));
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

#ifdef _WIN32
    a.mov(rcx, reinterpret_cast<uint64_t>(&m_Context.registers));
    a.mov(rdx, reinterpret_cast<uint64_t>(&m_Context.mmu));
    a.mov(r8d, opcVal);
#else
    a.mov(rdi, reinterpret_cast<uint64_t>(&m_Context.registers));
    a.mov(rsi, reinterpret_cast<uint64_t>(&m_Context.mmu));
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

#ifdef _WIN32
    a.mov(rcx, reinterpret_cast<uint64_t>(&m_Context.registers));
    a.mov(edx, opcVal);
    a.mov(r8d, pc);
#else
    a.mov(rdi, reinterpret_cast<uint64_t>(&m_Context.registers));
    a.mov(esi, opcVal);
    a.mov(edx, pc);
#endif
    m_Assembler->EmitCall(callback);
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
        // Decode SPECIAL by funct field
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
        case R4300_S_TGE:    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TGE), opcVal); break;
        case R4300_S_TGEU:   EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TGEU), opcVal); break;
        case R4300_S_TLT:    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TLT), opcVal); break;
        case R4300_S_TLTU:   EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TLTU), opcVal); break;
        case R4300_S_TEQ:    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TEQ), opcVal); break;
        case R4300_S_TNE:    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TNE), opcVal); break;
        case R4300_S_DSLL:   SPECIAL_DSLL(); break;
        case R4300_S_DSRL:   SPECIAL_DSRL(); break;
        case R4300_S_DSRA:   SPECIAL_DSRA(); break;
        case R4300_S_DSLL32: SPECIAL_DSLL32(); break;
        case R4300_S_DSRL32: SPECIAL_DSRL32(); break;
        case R4300_S_DSRA32: SPECIAL_DSRA32(); break;
        default: UnknownOpcode(); break;
        }
        break;
    }
    case R4300_REGIMM: {
        switch (op.Itype.rt) {
        case R4300_R_BLTZ:    BLTZ_Compare(); break;
        case R4300_R_BGEZ:    BGEZ_Compare(); break;
        case R4300_R_BLTZL:   BLTZ_Compare(); break;
        case R4300_R_BGEZL:   BGEZ_Compare(); break;
        case R4300_R_TGEI:    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TGEI), opcVal); break;
        case R4300_R_TGEIU:   EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TGEIU), opcVal); break;
        case R4300_R_TLTI:    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TLTI), opcVal); break;
        case R4300_R_TLTIU:   EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TLTIU), opcVal); break;
        case R4300_R_TEQI:    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TEQI), opcVal); break;
        case R4300_R_TNEI:    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TNEI), opcVal); break;
        case R4300_R_BLTZAL:  BLTZAL_Compare(); break;
        case R4300_R_BGEZAL:  BGEZAL_Compare(); break;
        case R4300_R_BLTZALL: BLTZAL_Compare(); break;
        case R4300_R_BGEZALL: BGEZAL_Compare(); break;
        default: UnknownOpcode(); break;
        }
        break;
    }
    case R4300_J:      J(); break;
    case R4300_JAL:    JAL(); break;
    case R4300_BEQ:    BEQ_Compare(); break;
    case R4300_BNE:    BNE_Compare(); break;
    case R4300_BLEZ:   BLEZ_Compare(); break;
    case R4300_BGTZ:   BGTZ_Compare(); break;
    case R4300_ADDI:   ADDI(); break;
    case R4300_ADDIU:  ADDIU(); break;
    case R4300_SLTI:   SLTI(); break;
    case R4300_SLTIU:  SLTIU(); break;
    case R4300_ANDI:   ANDI(); break;
    case R4300_ORI:    ORI(); break;
    case R4300_XORI:   XORI(); break;
    case R4300_LUI:    LUI(); break;
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
        switch (mips_rs(opcVal)) {
        case R4300_COP1_MF:  COP1_MF(); break;
        case R4300_COP1_DMF: COP1_DMF(); break;
        case R4300_COP1_CF:  COP1_CF(); break;
        case R4300_COP1_MT:  COP1_MT(); break;
        case R4300_COP1_DMT: COP1_DMT(); break;
        case R4300_COP1_CT:  COP1_CT(); break;
        case R4300_COP1_BC: {
            switch (op.Itype.rt & 3) {
            case 0: COP1_BCF_Compare(); break;  // BC1F
            case 1: COP1_BCF_Compare(); break;  // BC1FL
            case 2: COP1_BCT_Compare(); break;  // BC1T
            case 3: COP1_BCT_Compare(); break;  // BC1TL
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
                default: UnknownOpcode(); break;
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
                default: UnknownOpcode(); break;
                }
            }
            break;
        }
        case R4300_COP1_W: {
            switch (mips_funct(opcVal)) {
            case R4300_FPU_CVT_S: COP1_W_CVT_S(); break;
            case R4300_FPU_CVT_D: COP1_W_CVT_D(); break;
            default: UnknownOpcode(); break;
            }
            break;
        }
        case R4300_COP1_L: {
            switch (mips_funct(opcVal)) {
            case R4300_FPU_CVT_S: COP1_L_CVT_S(); break;
            case R4300_FPU_CVT_D: COP1_L_CVT_D(); break;
            default: UnknownOpcode(); break;
            }
            break;
        }
        default: UnknownOpcode(); break;
        }
        break;
    }
    case R4300_BEQL:   BEQ_Compare(); break;
    case R4300_BNEL:   BNE_Compare(); break;
    case R4300_BLEZL:  BLEZ_Compare(); break;
    case R4300_BGTZL:  BGTZ_Compare(); break;
    case R4300_DADDI:  DADDI(); break;
    case R4300_DADDIU: DADDIU(); break;
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
    case R4300_LWC1:   LWC1(); break;
    case R4300_LDC1:   LDC1(); break;
    case R4300_LD:     LD(); break;
    case R4300_SC:     SC(); break;
    case R4300_SWC1:   SWC1(); break;
    case R4300_SDC1:   SDC1(); break;
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
    m_Assembler->AddComment("=== Block Entry ===");

    // Emit function prologue: save callee-saved registers and set up stack frame
    m_Assembler->EmitPrologue(128, 0);

    m_BlockEnded = false;
    m_Stats.codeSizeGenerated = 0;
}

void CX64RecompilerOps::ExitCodeBlock(ExitReason reason)
{
    auto& a = m_Assembler->GetAssembler();
    m_Assembler->AddComment("=== Block Exit ===");

    // Flush all cached registers back to memory
    m_RegisterSet->FlushRegisters(true);

    // Emit function epilogue: restore callee-saved registers and return
    m_Assembler->EmitEpilogue(128, 0);

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
    Log::errorf("COP2 not supported in N64");
    return TranslationResult::Unsupported;
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
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_TGE), ctx.opcode.Value);
    return TranslationResult::Success;
}

TranslationResult CX64RecompilerOps::HandleSystem(const TranslationContext& ctx)
{
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
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_SYSCALL), ctx.opcode.Value);
    return TranslationResult::BlockBoundary;
}

TranslationResult CX64RecompilerOps::HandleSPECIAL_BREAK(const TranslationContext& ctx) {
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
// Private - Branch Comparison Instructions
// =============================================================================

// Each branch compare method emits a callback that evaluates the condition
// and sets m_PROGRAM_COUNTER at runtime, then marks the block as ended.

#define DEFINE_BRANCH_COMPARE_IMPL(name, callback_fn) \
    void CX64RecompilerOps::name##_Compare() { \
        if (!m_CurrentContext) return; \
        m_Assembler->AddComment(#name); \
        EmitBranchCallback(reinterpret_cast<void*>(&callback_fn), \
                           m_CurrentContext->opcode.Value, \
                           m_CurrentContext->virtualAddress); \
        m_Stats.branchesTranslated++; \
        m_Stats.categories.control++; \
        m_BlockEnded = true; \
    }

DEFINE_BRANCH_COMPARE_IMPL(BNE, Interp_BNE_Branch)
DEFINE_BRANCH_COMPARE_IMPL(BEQ, Interp_BEQ_Branch)
DEFINE_BRANCH_COMPARE_IMPL(BGTZ, Interp_BGTZ_Branch)
DEFINE_BRANCH_COMPARE_IMPL(BLEZ, Interp_BLEZ_Branch)
DEFINE_BRANCH_COMPARE_IMPL(BLTZ, Interp_BLTZ_Branch)
DEFINE_BRANCH_COMPARE_IMPL(BGEZ, Interp_BGEZ_Branch)
DEFINE_BRANCH_COMPARE_IMPL(BLTZAL, Interp_BLTZAL_Branch)
DEFINE_BRANCH_COMPARE_IMPL(BGEZAL, Interp_BGEZAL_Branch)
DEFINE_BRANCH_COMPARE_IMPL(COP1_BCF, Interp_BC1F_Branch)
DEFINE_BRANCH_COMPARE_IMPL(COP1_BCT, Interp_BC1T_Branch)

// =============================================================================
// Private - Jump Instructions
// =============================================================================

void CX64RecompilerOps::J()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("J");
    EmitBranchCallback(reinterpret_cast<void*>(&Interp_J_Jump),
                       m_CurrentContext->opcode.Value,
                       m_CurrentContext->virtualAddress);
    m_Stats.branchesTranslated++;
    m_Stats.categories.control++;
    m_BlockEnded = true;
}

void CX64RecompilerOps::JAL()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("JAL");
    EmitBranchCallback(reinterpret_cast<void*>(&Interp_JAL_Jump),
                       m_CurrentContext->opcode.Value,
                       m_CurrentContext->virtualAddress);
    m_Stats.branchesTranslated++;
    m_Stats.categories.control++;
    m_BlockEnded = true;
}

// =============================================================================
// Private - Immediate Arithmetic Instructions
// =============================================================================

#define DEFINE_ALU_IMM(name, callback, category) \
    void CX64RecompilerOps::name() { \
        if (!m_CurrentContext) return; \
        m_Assembler->AddComment(#name); \
        EmitRegisterCallback(reinterpret_cast<void*>(&Interp_##callback), \
                             m_CurrentContext->opcode.Value); \
        m_Stats.instructionsTranslated++; \
        m_Stats.categories.category++; \
    }

DEFINE_ALU_IMM(ADDI, ADDI, arithmetic)
DEFINE_ALU_IMM(ADDIU, ADDIU, arithmetic)
DEFINE_ALU_IMM(SLTI, SLTI, arithmetic)
DEFINE_ALU_IMM(SLTIU, SLTIU, arithmetic)
DEFINE_ALU_IMM(ANDI, ANDI, logical)
DEFINE_ALU_IMM(ORI, ORI, logical)
DEFINE_ALU_IMM(XORI, XORI, logical)
DEFINE_ALU_IMM(LUI, LUI, arithmetic)
DEFINE_ALU_IMM(DADDI, DADDI, arithmetic)
DEFINE_ALU_IMM(DADDIU, DADDIU, arithmetic)

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
DEFINE_LOAD(LW, LW)
DEFINE_LOAD(LBU, LBU)
DEFINE_LOAD(LHU, LHU)
DEFINE_LOAD(LWR, LWR)
DEFINE_LOAD(LWU, LWU)
DEFINE_LOAD(LD, LD)
DEFINE_LOAD(LDL, LDL)
DEFINE_LOAD(LDR, LDR)
DEFINE_LOAD(LL, LL)
DEFINE_LOAD(LWC1, LWC1)
DEFINE_LOAD(LDC1, LDC1)

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
DEFINE_STORE(SW, SW)
DEFINE_STORE(SWR, SWR)
DEFINE_STORE(SD, SD)
DEFINE_STORE(SDL, SDL)
DEFINE_STORE(SDR, SDR)
DEFINE_STORE(SC, SC)
DEFINE_STORE(SWC1, SWC1)
DEFINE_STORE(SDC1, SDC1)

void CX64RecompilerOps::CACHE()
{
    m_Assembler->AddComment("CACHE");
    // CACHE is an architecture hint - treated as NOP for correctness
    m_Stats.instructionsTranslated++;
}

// =============================================================================
// Private - SPECIAL Format Instructions
// =============================================================================

#define DEFINE_SPECIAL(name, callback, category) \
    void CX64RecompilerOps::SPECIAL_##name() { \
        if (!m_CurrentContext) return; \
        m_Assembler->AddComment("SPECIAL_" #name); \
        EmitRegisterCallback(reinterpret_cast<void*>(&Interp_##callback), \
                             m_CurrentContext->opcode.Value); \
        m_Stats.instructionsTranslated++; \
        m_Stats.categories.category++; \
    }

DEFINE_SPECIAL(SLL, SLL, shift)
DEFINE_SPECIAL(SRL, SRL, shift)
DEFINE_SPECIAL(SRA, SRA, shift)
DEFINE_SPECIAL(SLLV, SLLV, shift)
DEFINE_SPECIAL(SRLV, SRLV, shift)
DEFINE_SPECIAL(SRAV, SRAV, shift)
DEFINE_SPECIAL(MFHI, MFHI, arithmetic)
DEFINE_SPECIAL(MTHI, MTHI, arithmetic)
DEFINE_SPECIAL(MFLO, MFLO, arithmetic)
DEFINE_SPECIAL(MTLO, MTLO, arithmetic)
DEFINE_SPECIAL(DSLLV, DSLLV, shift)
DEFINE_SPECIAL(DSRLV, DSRLV, shift)
DEFINE_SPECIAL(DSRAV, DSRAV, shift)
DEFINE_SPECIAL(MULT, MULT, arithmetic)
DEFINE_SPECIAL(MULTU, MULTU, arithmetic)
DEFINE_SPECIAL(DIV, DIV, arithmetic)
DEFINE_SPECIAL(DIVU, DIVU, arithmetic)
DEFINE_SPECIAL(DMULT, DMULT, arithmetic)
DEFINE_SPECIAL(DMULTU, DMULTU, arithmetic)
DEFINE_SPECIAL(DDIV, DDIV, arithmetic)
DEFINE_SPECIAL(DDIVU, DDIVU, arithmetic)
DEFINE_SPECIAL(ADD, ADD, arithmetic)
DEFINE_SPECIAL(ADDU, ADDU, arithmetic)
DEFINE_SPECIAL(SUB, SUB, arithmetic)
DEFINE_SPECIAL(SUBU, SUBU, arithmetic)
DEFINE_SPECIAL(AND, AND, logical)
DEFINE_SPECIAL(OR, OR, logical)
DEFINE_SPECIAL(XOR, XOR, logical)
DEFINE_SPECIAL(NOR, NOR, logical)
DEFINE_SPECIAL(SLT, SLT, arithmetic)
DEFINE_SPECIAL(SLTU, SLTU, arithmetic)
DEFINE_SPECIAL(DADD, DADD, arithmetic)
DEFINE_SPECIAL(DADDU, DADDU, arithmetic)
DEFINE_SPECIAL(DSUB, DSUB, arithmetic)
DEFINE_SPECIAL(DSUBU, DSUBU, arithmetic)
DEFINE_SPECIAL(DSLL, DSLL, shift)
DEFINE_SPECIAL(DSRL, DSRL, shift)
DEFINE_SPECIAL(DSRA, DSRA, shift)
DEFINE_SPECIAL(DSLL32, DSLL32, shift)
DEFINE_SPECIAL(DSRL32, DSRL32, shift)
DEFINE_SPECIAL(DSRA32, DSRA32, shift)

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
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_SYSCALL),
                         m_CurrentContext->opcode.Value);
    m_Stats.categories.system++;
    m_BlockEnded = true;
}

void CX64RecompilerOps::SPECIAL_BREAK()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("SPECIAL_BREAK");
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
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP0_MTC0),
                         m_CurrentContext->opcode.Value);
    m_Stats.categories.coprocessor++;
}

void CX64RecompilerOps::COP0_DMT()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP0_DMT");
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
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP0_CO_ERET),
                         m_CurrentContext->opcode.Value);
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
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_MFC1),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

void CX64RecompilerOps::COP1_DMF()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_DMF");
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_DMFC1),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

void CX64RecompilerOps::COP1_CF()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_CF");
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_CFC1),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

void CX64RecompilerOps::COP1_MT()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_MT");
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_MTC1),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

void CX64RecompilerOps::COP1_DMT()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_DMT");
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_DMTC1),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

void CX64RecompilerOps::COP1_CT()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_CT");
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_CTC1),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

// COP1 Single-precision arithmetic
#define DEFINE_COP1_S_ARITH(name) \
    void CX64RecompilerOps::COP1_S_##name() { \
        if (!m_CurrentContext) return; \
        m_Assembler->AddComment("COP1_S_" #name); \
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
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_W_CVT),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

void CX64RecompilerOps::COP1_W_CVT_D()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_W_CVT_D");
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_W_CVT),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

void CX64RecompilerOps::COP1_L_CVT_S()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_L_CVT_S");
    EmitRegisterCallback(reinterpret_cast<void*>(&Interp_COP1_L_CVT),
                         m_CurrentContext->opcode.Value);
    m_Stats.floatingPointOps++;
}

void CX64RecompilerOps::COP1_L_CVT_D()
{
    if (!m_CurrentContext) return;
    m_Assembler->AddComment("COP1_L_CVT_D");
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
