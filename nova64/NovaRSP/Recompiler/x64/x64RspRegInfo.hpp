/**
 * @file x64RspRegInfo.hpp
 * @brief x64 RSP Register Info.
 */
// SuperNova64 - N64 Emulator
// x64 RSP Register Info
// Defines the x86-64 register mapping for RSP recompiler:
//   - Which x64 GPRs hold RSP state pointers
//   - XMM register allocation for RSP vector registers
//   - Calling convention helpers for Windows x64 ABI

#pragma once

#include "x64RspOps.hpp"

#if defined(__amd64__) || defined(_M_X64)

#include <cstdint>

namespace Nova64::RSP::Recompiler::x64 {

// ──────────────────────────────────────────────
// x64 ABI register roles
// ──────────────────────────────────────────────
//
// Windows x64 ABI:
//   Volatile (caller-saved):    RAX, RCX, RDX, R8-R11, XMM0-XMM5
//   Nonvolatile (callee-saved): RBX, RBP, RSI, RDI, R12-R15, XMM6-XMM15
//
// Our allocation:
//   RBX  = pointer to RSP registers (CRSPRegisters*)
//   RBP  = pointer to DMEM (uint8_t*)
//   R12  = pointer to RSP vector registers (RSPVector*)
//   R13  = pointer to accumulator data
//   R14  = compiler scratch / cycle counter
//   R15  = reserved (return address / block exit)
//   R11  = scratch for absolute addressing in CX64RspOps
//
//   XMM6-XMM15 = mapped RSP vector registers (callee-saved on Win64)
//   XMM0-XMM5  = scratch / temporaries

// ──────────────────────────────────────────────
// x64 register assignments
// ──────────────────────────────────────────────

static constexpr GpReg REG_RSP_STATE    = GpReg::RBX;   // CRSPRegisters* base
static constexpr GpReg REG_DMEM_BASE    = GpReg::RBP;   // uint8_t* DMEM
static constexpr GpReg REG_VECT_BASE    = GpReg::R12;   // RSPVector* array base
static constexpr GpReg REG_ACCUM_BASE   = GpReg::R13;   // Accumulator base
static constexpr GpReg REG_CYCLE_COUNT  = GpReg::R14;   // Cycle/instruction counter
static constexpr GpReg REG_BLOCK_EXIT   = GpReg::R15;   // Block exit address cache
static constexpr GpReg REG_SCRATCH      = GpReg::R11;   // Scratch (used by x64RspOps)

static constexpr GpReg REG_ARG0         = GpReg::RCX;   // Win64 1st argument
static constexpr GpReg REG_ARG1         = GpReg::RDX;   // Win64 2nd argument
static constexpr GpReg REG_ARG2         = GpReg::R8;    // Win64 3rd argument
static constexpr GpReg REG_ARG3         = GpReg::R9;    // Win64 4th argument
static constexpr GpReg REG_RETURN       = GpReg::RAX;   // Return value

// ──────────────────────────────────────────────
// XMM register pools
// ──────────────────────────────────────────────

static constexpr int MAX_MAPPED_XMM     = 10;   // XMM6-XMM15 for vector regs
static constexpr int MAX_SCRATCH_XMM    = 6;    // XMM0-XMM5 for temporaries

static constexpr XmmReg SCRATCH_XMM[MAX_SCRATCH_XMM] = {
    XmmReg::XMM0, XmmReg::XMM1, XmmReg::XMM2,
    XmmReg::XMM3, XmmReg::XMM4, XmmReg::XMM5,
};

static constexpr XmmReg MAPPED_XMM[MAX_MAPPED_XMM] = {
    XmmReg::XMM6, XmmReg::XMM7, XmmReg::XMM8, XmmReg::XMM9, XmmReg::XMM10,
    XmmReg::XMM11, XmmReg::XMM12, XmmReg::XMM13, XmmReg::XMM14, XmmReg::XMM15,
};

// ──────────────────────────────────────────────
// Register info / state tracking
// ──────────────────────────────────────────────

enum class XmmMapState : uint8_t
{
    Free,       // Not mapped to any RSP register
    Mapped,     // Holds a specific RSP VR[n]
    Dirty,      // Mapped and modified (needs writeback)
    Temp,       // Temporary allocation (not an RSP register)
    AccumHigh,  // Holds accumulator high part
    AccumMid,   // Holds accumulator mid part
    AccumLow,   // Holds accumulator low part
    Flags,      // Holds flag register (VCO/VCC/VCE)
};

struct XmmMapEntry
{
    XmmMapState state = XmmMapState::Free;
    int         rspReg = -1;      // RSP VR index (0-31), or -1
    int         accessAge = 0;    // LRU counter for eviction
    bool        dirty = false;    // Needs writeback to memory
};

class CX64RspRegInfo
{
public:
    CX64RspRegInfo();
    ~CX64RspRegInfo() = default;

    void Reset();

    // ── XMM allocation ──
    XmmReg MapVectorReg(int vr, CX64RspOps& ops);
    XmmReg MapVectorRegDirty(int vr, CX64RspOps& ops);
    XmmReg AllocScratchXmm(CX64RspOps& ops);
    void   FreeXmm(XmmReg xmm);
    void   MarkDirty(XmmReg xmm);
    XmmReg FindMappedVR(int vr) const;

    // ── Spill / writeback ──
    void   SpillXmm(XmmReg xmm, CX64RspOps& ops);
    void   SpillAll(CX64RspOps& ops);
    void   WriteBackAll(CX64RspOps& ops);

    // ── Accumulator mapping ──
    XmmReg GetAccumHigh(CX64RspOps& ops);
    XmmReg GetAccumMid(CX64RspOps& ops);
    XmmReg GetAccumLow(CX64RspOps& ops);
    void   SpillAccum(CX64RspOps& ops);

    // ── Flag register mapping ──
    XmmReg GetFlagReg(int flagIdx, CX64RspOps& ops);
    void   SpillFlags(CX64RspOps& ops);

    // ── Prologue / Epilogue ──
    void   EmitPrologue(CX64RspOps& ops, void* rspRegs, void* dmem, void* vectBase, void* accumBase);
    void   EmitEpilogue(CX64RspOps& ops);

    // ── GPR state (scalar tracking) ──
    bool   IsGprConst(int gpr) const { return m_GprConst[gpr]; }
    int32_t GetGprConstVal(int gpr) const { return m_GprConstVal[gpr]; }
    void   SetGprConst(int gpr, int32_t value);
    void   ClearGprConst(int gpr);
    void   ClearAllGpr();

    // ── Stats ──
    int GetTotalSpills() const { return m_TotalSpills; }
    int GetTotalLoads() const { return m_TotalLoads; }

private:
    XmmReg FindFreeXmm() const;
    XmmReg EvictLRU(CX64RspOps& ops);
    void   IncrementAge();
    int    XmmToIndex(XmmReg xmm) const;
    int    ComputeVRMemOffset(int vr) const;
    int    ComputeAccumMemOffset(int part) const;
    int    ComputeFlagMemOffset(int flagIdx) const;

    XmmMapEntry m_XmmMap[16];    // All 16 XMM registers
    int         m_AccessTick = 0;

    // GPR constant propagation
    bool    m_GprConst[32] = {};
    int32_t m_GprConstVal[32] = {};

    // Stats
    int     m_TotalSpills = 0;
    int     m_TotalLoads = 0;
};

} // namespace Nova64::RSP::Recompiler::x64

#endif // x64 only
