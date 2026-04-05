/**
 * @file x64RspRegInfo.cpp
 * @brief x64 RSP Register Info Implementation.
 */
// SuperNova64 - N64 Emulator
// x64 RSP Register Info Implementation
// Register allocation, spilling, and calling convention management

#include "x64RspRegInfo.hpp"

#if defined(__amd64__) || defined(_M_X64)

#include <cstring>

namespace Nova64::RSP::Recompiler::x64 {

CX64RspRegInfo::CX64RspRegInfo()
{
    Reset();
}

void CX64RspRegInfo::Reset()
{
    for (int i = 0; i < 16; i++)
    {
        m_XmmMap[i].state = XmmMapState::Free;
        m_XmmMap[i].rspReg = -1;
        m_XmmMap[i].accessAge = 0;
        m_XmmMap[i].dirty = false;
    }
    m_AccessTick = 0;
    m_TotalSpills = 0;
    m_TotalLoads = 0;
    ClearAllGpr();
}

// ──────────────────────────────────────────────
// XMM index helpers
// ──────────────────────────────────────────────

int CX64RspRegInfo::XmmToIndex(XmmReg xmm) const
{
    return static_cast<int>(xmm);
}

int CX64RspRegInfo::ComputeVRMemOffset(int vr) const
{
    // Each RSP vector register is 16 bytes (128 bits)
    return vr * 16;
}

int CX64RspRegInfo::ComputeAccumMemOffset(int part) const
{
    // Accum layout: 3 x 16 bytes (high, mid, low) per 8 lanes
    // part 0 = high, 1 = mid, 2 = low
    return part * 16;
}

int CX64RspRegInfo::ComputeFlagMemOffset(int flagIdx) const
{
    // Flag registers: VCO_lo(0), VCO_hi(1), VCC_lo(2), VCC_hi(3), VCE(4)
    // Each is 16 bytes (8 lanes x 16 bits packed)
    return flagIdx * 16;
}

void CX64RspRegInfo::IncrementAge()
{
    m_AccessTick++;
}

// ──────────────────────────────────────────────
// Find free / evict LRU
// ──────────────────────────────────────────────

XmmReg CX64RspRegInfo::FindFreeXmm() const
{
    // Prefer scratch registers first (XMM0-5, volatile on Win64)
    for (int i = 0; i < MAX_SCRATCH_XMM; i++)
    {
        int idx = XmmToIndex(SCRATCH_XMM[i]);
        if (m_XmmMap[idx].state == XmmMapState::Free)
            return SCRATCH_XMM[i];
    }
    // Then check mapped pool (XMM6-15, callee-saved)
    for (int i = 0; i < MAX_MAPPED_XMM; i++)
    {
        int idx = XmmToIndex(MAPPED_XMM[i]);
        if (m_XmmMap[idx].state == XmmMapState::Free)
            return MAPPED_XMM[i];
    }
    return XmmReg::XMM0; // sentinel — caller should check via state
}

XmmReg CX64RspRegInfo::EvictLRU(CX64RspOps& ops)
{
    // Find the oldest (LRU) mapped register and spill it
    int oldestAge = m_AccessTick + 1;
    XmmReg victim = XmmReg::XMM0;
    bool found = false;

    // Prefer evicting from mapped pool (callee-saved, long-lived)
    for (int i = 0; i < MAX_MAPPED_XMM; i++)
    {
        int idx = XmmToIndex(MAPPED_XMM[i]);
        if (m_XmmMap[idx].state == XmmMapState::Mapped ||
            m_XmmMap[idx].state == XmmMapState::Dirty)
        {
            if (m_XmmMap[idx].accessAge < oldestAge)
            {
                oldestAge = m_XmmMap[idx].accessAge;
                victim = MAPPED_XMM[i];
                found = true;
            }
        }
    }
    // Fallback: evict from scratch pool
    if (!found)
    {
        for (int i = 0; i < MAX_SCRATCH_XMM; i++)
        {
            int idx = XmmToIndex(SCRATCH_XMM[i]);
            if (m_XmmMap[idx].state != XmmMapState::Free)
            {
                if (m_XmmMap[idx].accessAge < oldestAge)
                {
                    oldestAge = m_XmmMap[idx].accessAge;
                    victim = SCRATCH_XMM[i];
                    found = true;
                }
            }
        }
    }

    if (found)
        SpillXmm(victim, ops);

    return victim;
}

// ──────────────────────────────────────────────
// Map RSP vector register to XMM
// ──────────────────────────────────────────────

XmmReg CX64RspRegInfo::FindMappedVR(int vr) const
{
    for (int i = 0; i < 16; i++)
    {
        if ((m_XmmMap[i].state == XmmMapState::Mapped ||
             m_XmmMap[i].state == XmmMapState::Dirty) &&
            m_XmmMap[i].rspReg == vr)
        {
            return static_cast<XmmReg>(i);
        }
    }
    return static_cast<XmmReg>(-1); // not found
}

XmmReg CX64RspRegInfo::MapVectorReg(int vr, CX64RspOps& ops)
{
    // Check if already mapped
    XmmReg existing = FindMappedVR(vr);
    if (static_cast<int>(existing) >= 0 && static_cast<int>(existing) < 16)
    {
        int idx = XmmToIndex(existing);
        m_XmmMap[idx].accessAge = m_AccessTick;
        IncrementAge();
        return existing;
    }

    // Find a free register
    XmmReg xmm = FindFreeXmm();
    int idx = XmmToIndex(xmm);
    if (m_XmmMap[idx].state != XmmMapState::Free)
    {
        // Need to evict
        xmm = EvictLRU(ops);
        idx = XmmToIndex(xmm);
    }

    // Load VR from memory: movdqa xmm, [REG_VECT_BASE + offset]
    int offset = ComputeVRMemOffset(vr);
    ops.Movdqa_XmmBaseDisp(xmm, REG_VECT_BASE, offset);
    m_TotalLoads++;

    m_XmmMap[idx].state = XmmMapState::Mapped;
    m_XmmMap[idx].rspReg = vr;
    m_XmmMap[idx].accessAge = m_AccessTick;
    m_XmmMap[idx].dirty = false;
    IncrementAge();

    return xmm;
}

XmmReg CX64RspRegInfo::MapVectorRegDirty(int vr, CX64RspOps& ops)
{
    XmmReg xmm = MapVectorReg(vr, ops);
    MarkDirty(xmm);
    return xmm;
}

XmmReg CX64RspRegInfo::AllocScratchXmm(CX64RspOps& ops)
{
    XmmReg xmm = FindFreeXmm();
    int idx = XmmToIndex(xmm);
    if (m_XmmMap[idx].state != XmmMapState::Free)
    {
        xmm = EvictLRU(ops);
        idx = XmmToIndex(xmm);
    }

    m_XmmMap[idx].state = XmmMapState::Temp;
    m_XmmMap[idx].rspReg = -1;
    m_XmmMap[idx].accessAge = m_AccessTick;
    m_XmmMap[idx].dirty = false;
    IncrementAge();

    return xmm;
}

void CX64RspRegInfo::FreeXmm(XmmReg xmm)
{
    int idx = XmmToIndex(xmm);
    m_XmmMap[idx].state = XmmMapState::Free;
    m_XmmMap[idx].rspReg = -1;
    m_XmmMap[idx].dirty = false;
}

void CX64RspRegInfo::MarkDirty(XmmReg xmm)
{
    int idx = XmmToIndex(xmm);
    m_XmmMap[idx].state = XmmMapState::Dirty;
    m_XmmMap[idx].dirty = true;
}

// ──────────────────────────────────────────────
// Spill / writeback
// ──────────────────────────────────────────────

void CX64RspRegInfo::SpillXmm(XmmReg xmm, CX64RspOps& ops)
{
    int idx = XmmToIndex(xmm);
    auto& entry = m_XmmMap[idx];

    if (entry.dirty && entry.rspReg >= 0)
    {
        // Write back to memory: movdqa [REG_VECT_BASE + offset], xmm
        int offset = ComputeVRMemOffset(entry.rspReg);
        ops.Movdqa_BaseDispXmm(REG_VECT_BASE, offset, xmm);
        m_TotalSpills++;
    }
    else if (entry.state == XmmMapState::AccumHigh ||
             entry.state == XmmMapState::AccumMid ||
             entry.state == XmmMapState::AccumLow)
    {
        int part = (entry.state == XmmMapState::AccumHigh) ? 0 :
                   (entry.state == XmmMapState::AccumMid) ? 1 : 2;
        int offset = ComputeAccumMemOffset(part);
        ops.Movdqa_BaseDispXmm(REG_ACCUM_BASE, offset, xmm);
        m_TotalSpills++;
    }
    else if (entry.state == XmmMapState::Flags && entry.rspReg >= 0)
    {
        // Spill flag register
        // Flags are stored relative to RSP state
        // (exact offset depends on CRSPRegisters layout)
        int offset = ComputeFlagMemOffset(entry.rspReg);
        ops.Movdqa_BaseDispXmm(REG_RSP_STATE, offset, xmm); // approximate
        m_TotalSpills++;
    }

    entry.state = XmmMapState::Free;
    entry.rspReg = -1;
    entry.dirty = false;
}

void CX64RspRegInfo::SpillAll(CX64RspOps& ops)
{
    for (int i = 0; i < 16; i++)
    {
        if (m_XmmMap[i].state != XmmMapState::Free)
        {
            SpillXmm(static_cast<XmmReg>(i), ops);
        }
    }
}

void CX64RspRegInfo::WriteBackAll(CX64RspOps& ops)
{
    // Only write back dirty registers, leave them mapped
    for (int i = 0; i < 16; i++)
    {
        if (m_XmmMap[i].dirty && m_XmmMap[i].rspReg >= 0)
        {
            XmmReg xmm = static_cast<XmmReg>(i);
            int offset = ComputeVRMemOffset(m_XmmMap[i].rspReg);
            ops.Movdqa_BaseDispXmm(REG_VECT_BASE, offset, xmm);
            m_XmmMap[i].dirty = false;
            m_TotalSpills++;
        }
    }
}

// ──────────────────────────────────────────────
// Accumulator mapping
// ──────────────────────────────────────────────

XmmReg CX64RspRegInfo::GetAccumHigh(CX64RspOps& ops)
{
    for (int i = 0; i < 16; i++)
    {
        if (m_XmmMap[i].state == XmmMapState::AccumHigh)
            return static_cast<XmmReg>(i);
    }
    // Not mapped — allocate and load
    XmmReg xmm = AllocScratchXmm(ops);
    int idx = XmmToIndex(xmm);
    ops.Movdqa_XmmBaseDisp(xmm, REG_ACCUM_BASE, ComputeAccumMemOffset(0));
    m_XmmMap[idx].state = XmmMapState::AccumHigh;
    m_XmmMap[idx].rspReg = -1;
    m_TotalLoads++;
    return xmm;
}

XmmReg CX64RspRegInfo::GetAccumMid(CX64RspOps& ops)
{
    for (int i = 0; i < 16; i++)
    {
        if (m_XmmMap[i].state == XmmMapState::AccumMid)
            return static_cast<XmmReg>(i);
    }
    XmmReg xmm = AllocScratchXmm(ops);
    int idx = XmmToIndex(xmm);
    ops.Movdqa_XmmBaseDisp(xmm, REG_ACCUM_BASE, ComputeAccumMemOffset(1));
    m_XmmMap[idx].state = XmmMapState::AccumMid;
    m_XmmMap[idx].rspReg = -1;
    m_TotalLoads++;
    return xmm;
}

XmmReg CX64RspRegInfo::GetAccumLow(CX64RspOps& ops)
{
    for (int i = 0; i < 16; i++)
    {
        if (m_XmmMap[i].state == XmmMapState::AccumLow)
            return static_cast<XmmReg>(i);
    }
    XmmReg xmm = AllocScratchXmm(ops);
    int idx = XmmToIndex(xmm);
    ops.Movdqa_XmmBaseDisp(xmm, REG_ACCUM_BASE, ComputeAccumMemOffset(2));
    m_XmmMap[idx].state = XmmMapState::AccumLow;
    m_XmmMap[idx].rspReg = -1;
    m_TotalLoads++;
    return xmm;
}

void CX64RspRegInfo::SpillAccum(CX64RspOps& ops)
{
    for (int i = 0; i < 16; i++)
    {
        if (m_XmmMap[i].state == XmmMapState::AccumHigh ||
            m_XmmMap[i].state == XmmMapState::AccumMid ||
            m_XmmMap[i].state == XmmMapState::AccumLow)
        {
            SpillXmm(static_cast<XmmReg>(i), ops);
        }
    }
}

// ──────────────────────────────────────────────
// Flag register mapping
// ──────────────────────────────────────────────

XmmReg CX64RspRegInfo::GetFlagReg(int flagIdx, CX64RspOps& ops)
{
    for (int i = 0; i < 16; i++)
    {
        if (m_XmmMap[i].state == XmmMapState::Flags && m_XmmMap[i].rspReg == flagIdx)
            return static_cast<XmmReg>(i);
    }
    XmmReg xmm = AllocScratchXmm(ops);
    int idx = XmmToIndex(xmm);
    // Load from RSP state + flag offset
    ops.Movdqa_XmmBaseDisp(xmm, REG_RSP_STATE, ComputeFlagMemOffset(flagIdx));
    m_XmmMap[idx].state = XmmMapState::Flags;
    m_XmmMap[idx].rspReg = flagIdx;
    m_XmmMap[idx].dirty = false;
    m_TotalLoads++;
    return xmm;
}

void CX64RspRegInfo::SpillFlags(CX64RspOps& ops)
{
    for (int i = 0; i < 16; i++)
    {
        if (m_XmmMap[i].state == XmmMapState::Flags)
        {
            SpillXmm(static_cast<XmmReg>(i), ops);
        }
    }
}

// ──────────────────────────────────────────────
// Prologue / Epilogue
// ──────────────────────────────────────────────

void CX64RspRegInfo::EmitPrologue(CX64RspOps& ops, void* rspRegs, void* dmem,
                                   void* vectBase, void* accumBase)
{
    // Save callee-saved registers
    ops.Push(GpReg::RBX);
    ops.Push(GpReg::RBP);
    ops.Push(GpReg::R12);
    ops.Push(GpReg::R13);
    ops.Push(GpReg::R14);
    ops.Push(GpReg::R15);

    // Allocate shadow space (32 bytes) + alignment
    // Also reserve space for saving XMM6-XMM15 (10 * 16 = 160 bytes)
    // Total: 32 + 160 = 192, round to 16-byte aligned = 192 + 8 (for 6 pushes) → 200 total
    ops.SubRegImm32(GpReg::RSP, 208); // 32 shadow + 160 XMM save + 16 align pad

    // Save callee-saved XMM registers (XMM6-XMM15) to stack
    for (int i = 0; i < 10; i++)
    {
        int stackOffset = 32 + (i * 16); // After shadow space
        ops.Movdqa_BaseDispXmm(GpReg::RSP, stackOffset, static_cast<XmmReg>(6 + i));
    }

    // Load state pointers into callee-saved GPRs
    ops.MovRegImm64(REG_RSP_STATE, reinterpret_cast<uint64_t>(rspRegs));
    ops.MovRegImm64(REG_DMEM_BASE, reinterpret_cast<uint64_t>(dmem));
    ops.MovRegImm64(REG_VECT_BASE, reinterpret_cast<uint64_t>(vectBase));
    ops.MovRegImm64(REG_ACCUM_BASE, reinterpret_cast<uint64_t>(accumBase));

    // Initialize cycle counter
    ops.XorRegReg(REG_CYCLE_COUNT, REG_CYCLE_COUNT);
}

void CX64RspRegInfo::EmitEpilogue(CX64RspOps& ops)
{
    // Write back all dirty mapped registers
    SpillAll(ops);

    // Store cycle count result
    ops.MovRegReg(GpReg::RAX, REG_CYCLE_COUNT);

    // Restore callee-saved XMM registers
    for (int i = 0; i < 10; i++)
    {
        int stackOffset = 32 + (i * 16);
        ops.Movdqa_XmmBaseDisp(static_cast<XmmReg>(6 + i), GpReg::RSP, stackOffset);
    }

    // Deallocate stack
    ops.AddRegImm32(GpReg::RSP, 208);

    // Restore callee-saved GPRs
    ops.Pop(GpReg::R15);
    ops.Pop(GpReg::R14);
    ops.Pop(GpReg::R13);
    ops.Pop(GpReg::R12);
    ops.Pop(GpReg::RBP);
    ops.Pop(GpReg::RBX);

    ops.Ret();
}

// ──────────────────────────────────────────────
// GPR constant propagation
// ──────────────────────────────────────────────

void CX64RspRegInfo::SetGprConst(int gpr, int32_t value)
{
    if (gpr == 0) return; // R0 is always zero
    m_GprConst[gpr] = true;
    m_GprConstVal[gpr] = value;
}

void CX64RspRegInfo::ClearGprConst(int gpr)
{
    m_GprConst[gpr] = false;
    m_GprConstVal[gpr] = 0;
}

void CX64RspRegInfo::ClearAllGpr()
{
    std::memset(m_GprConst, 0, sizeof(m_GprConst));
    std::memset(m_GprConstVal, 0, sizeof(m_GprConstVal));
    // R0 is always constant 0
    m_GprConst[0] = true;
    m_GprConstVal[0] = 0;
}

} // namespace Nova64::RSP::Recompiler::x64

#endif // x64 only
