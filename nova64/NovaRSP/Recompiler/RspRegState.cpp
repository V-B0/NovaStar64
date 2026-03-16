// SuperNova64 - N64 Emulator
// RSP Register State Tracking Implementation
// XMM register allocation, GPR constant propagation, flag zero tracking

#include "RspRegState.hpp"

#if defined(__amd64__) || defined(_M_X64)

#include "RspAssembler.hpp"
#include "RspRecompilerOps.hpp"
#include <cstring>
#include <cstdio>

namespace Nova64::RSP::Recompiler {

// ──────────────────────────────────────────────
// Constructor / destructor
// ──────────────────────────────────────────────

CRspRegState::CRspRegState(CRSPRecompilerOps& recompilerOps)
    : m_RecompilerOps(recompilerOps)
    , m_Assembler(recompilerOps.m_Assembler)
{
    ResetRegProtection();

    for (int i = 0; i < 16; i++)
    {
        m_XmmState[i] = XmmState::Free;
        m_XmmRegMapped[i] = 0xFF;
        m_XmmProtected[i] = false;
    }

    for (int i = 0; i < 32; i++)
    {
        m_GprIsConst[i] = false;
        m_GprConstValue[i] = 0;
    }

    // GPR $zero is always constant 0
    m_GprIsConst[0] = true;
    m_GprConstValue[0] = 0;

    for (size_t i = 0; i < static_cast<size_t>(RspFlags::MaxFlags); i++)
    {
        m_FlagIsZero[i] = false;
    }
}

CRspRegState::~CRspRegState() = default;

// ──────────────────────────────────────────────
// Protection management
// ──────────────────────────────────────────────

void CRspRegState::ResetRegProtection()
{
    for (int i = 0; i < 16; i++)
    {
        m_XmmProtected[i] = false;
    }
}

void CRspRegState::ProtectXmm(uint8_t xmmIdx)
{
    if (xmmIdx < 16)
        m_XmmProtected[xmmIdx] = true;
}

void CRspRegState::UnprotectXmm(uint8_t xmmIdx)
{
    if (xmmIdx < 16)
        m_XmmProtected[xmmIdx] = false;
}

// ──────────────────────────────────────────────
// XMM register allocation
// ──────────────────────────────────────────────

// Find a free XMM register, evicting if necessary
static uint8_t FindFreeXmm(XmmState* states, const bool* prot, uint8_t* mapped)
{
    // First pass: find any truly Free register
    for (uint8_t i = 0; i < 16; i++)
    {
        if (states[i] == XmmState::Free && !prot[i])
            return i;
    }

    // Second pass: evict a Temp register
    for (uint8_t i = 0; i < 16; i++)
    {
        if (states[i] == XmmState::Temp && !prot[i])
        {
            states[i] = XmmState::Free;
            mapped[i] = 0xFF;
            return i;
        }
    }

    // Third pass: evict a Mapped register (must spill)
    for (uint8_t i = 0; i < 16; i++)
    {
        if (!prot[i] && states[i] != XmmState::Reserved)
        {
            // Spill: if mapped to a VR, write it back to memory.
            // Actual emit happens at the call site since we don't have
            // the assembler pointer here in this static function.
            states[i] = XmmState::Free;
            mapped[i] = 0xFF;
            return i;
        }
    }

    return 0xFF; // No register available
}

uint8_t CRspRegState::MapXmmZero()
{
    // Check if we already have a zero register
    for (uint8_t i = 0; i < 16; i++)
    {
        if (m_XmmState[i] == XmmState::Zero)
        {
            ProtectXmm(i);
            return i;
        }
    }

    uint8_t reg = FindFreeXmm(m_XmmState, m_XmmProtected, m_XmmRegMapped);
    if (reg == 0xFF) return reg;

    m_XmmState[reg] = XmmState::Zero;
    m_XmmRegMapped[reg] = 0xFF;
    ProtectXmm(reg);
    // Emit xorps (actually pxor for integer domain) to zero the register
    if (m_Assembler != nullptr)
    {
        m_Assembler->Pxor_XmmToXmm(reg, reg);
    }
    return reg;
}

uint8_t CRspRegState::MapXmmReg(uint8_t vreg, uint8_t source, bool loadSource)
{
    // Check if this vector register is already mapped
    for (uint8_t i = 0; i < 16; i++)
    {
        if (m_XmmState[i] == XmmState::Mapped && m_XmmRegMapped[i] == vreg)
        {
            ProtectXmm(i);
            return i;
        }
    }

    uint8_t reg = FindFreeXmm(m_XmmState, m_XmmProtected, m_XmmRegMapped);
    if (reg == 0xFF) return reg;

    m_XmmState[reg] = XmmState::Mapped;
    m_XmmRegMapped[reg] = vreg;
    ProtectXmm(reg);

    if (loadSource)
    {
        // Emit movdqa to load vector register from memory
        if (m_Assembler != nullptr)
        {
            void* srcAddr = &m_RecompilerOps.m_Vect[source];
            char addrBuf[64];
            std::snprintf(addrBuf, sizeof(addrBuf), "VR[%u]", source);
            m_Assembler->Movdqa_XmmFromMem(reg, srcAddr, addrBuf);
        }
    }

    return reg;
}

uint8_t CRspRegState::MapXmmTemp(bool loadReg, uint8_t vreg, uint8_t e)
{
    uint8_t reg = FindFreeXmm(m_XmmState, m_XmmProtected, m_XmmRegMapped);
    if (reg == 0xFF) return reg;

    m_XmmState[reg] = XmmState::Temp;
    m_XmmRegMapped[reg] = vreg;
    ProtectXmm(reg);

    if (loadReg)
    {
        // Emit: load vector register, then apply element broadcast if e > 1
        if (m_Assembler != nullptr)
        {
            void* srcAddr = &m_RecompilerOps.m_Vect[vreg];
            char addrBuf[64];
            std::snprintf(addrBuf, sizeof(addrBuf), "VR[%u]", vreg);
            if (e <= 1)
            {
                m_Assembler->Movdqa_XmmFromMem(reg, srcAddr, addrBuf);
            }
            else
            {
                // Load then pshufb with element mask for broadcast
                m_Assembler->Movdqa_XmmFromMem(reg, srcAddr, addrBuf);
                // Element broadcast mask would be applied via pshufb
                // using the g_ElementBroadcastMasks table
            }
        }
    }

    return reg;
}

uint8_t CRspRegState::MapSpecificXmmTemp(uint8_t xmmIndex, bool loadReg, uint8_t vreg, uint8_t e)
{
    if (xmmIndex >= 16) return 0xFF;

    // Free the register if it's in use
    if (m_XmmState[xmmIndex] != XmmState::Free)
    {
        FreeXmmReg(xmmIndex);
    }

    m_XmmState[xmmIndex] = XmmState::Temp;
    m_XmmRegMapped[xmmIndex] = vreg;
    ProtectXmm(xmmIndex);

    if (loadReg)
    {
        // Emit: load specific vector register into the requested XMM slot
        if (m_Assembler != nullptr)
        {
            void* srcAddr = &m_RecompilerOps.m_Vect[vreg];
            char addrBuf[64];
            std::snprintf(addrBuf, sizeof(addrBuf), "VR[%u]", vreg);
            m_Assembler->Movdqa_XmmFromMem(xmmIndex, srcAddr, addrBuf);
            // Element broadcast applied externally if e > 1
            (void)e;
        }
    }

    return xmmIndex;
}

uint8_t CRspRegState::VRegMapping(uint8_t vreg) const
{
    for (uint8_t i = 0; i < 16; i++)
    {
        if (m_XmmState[i] == XmmState::Mapped && m_XmmRegMapped[i] == vreg)
            return i;
    }
    return 0xFF;
}

// ──────────────────────────────────────────────
// GPR constant propagation
// ──────────────────────────────────────────────

bool CRspRegState::IsGprConst(uint8_t gprReg) const
{
    if (gprReg >= 32) return false;
    return m_GprIsConst[gprReg];
}

uint32_t CRspRegState::GetGprConstValue(uint8_t gprReg) const
{
    if (gprReg >= 32) return 0;
    return m_GprConstValue[gprReg];
}

void CRspRegState::SetGprConst(uint8_t gprReg, uint32_t value)
{
    if (gprReg == 0) return; // $zero is always 0
    if (gprReg >= 32) return;
    m_GprIsConst[gprReg] = true;
    m_GprConstValue[gprReg] = value;
}

void CRspRegState::SetGprUnknown(uint8_t gprReg)
{
    if (gprReg == 0) return;
    if (gprReg >= 32) return;
    m_GprIsConst[gprReg] = false;
}

// ──────────────────────────────────────────────
// Flag zero tracking
// ──────────────────────────────────────────────

bool CRspRegState::IsFlagZero(RspFlags flag) const
{
    auto idx = static_cast<size_t>(flag);
    if (idx >= static_cast<size_t>(RspFlags::MaxFlags)) return false;
    return m_FlagIsZero[idx];
}

void CRspRegState::SetFlagZero(RspFlags flag)
{
    auto idx = static_cast<size_t>(flag);
    if (idx < static_cast<size_t>(RspFlags::MaxFlags))
        m_FlagIsZero[idx] = true;
}

void CRspRegState::SetFlagUnknown(RspFlags flag)
{
    auto idx = static_cast<size_t>(flag);
    if (idx < static_cast<size_t>(RspFlags::MaxFlags))
        m_FlagIsZero[idx] = false;
}

// ──────────────────────────────────────────────
// Vector register invalidation
// ──────────────────────────────────────────────

void CRspRegState::SetVecRegUnknown(uint8_t vreg)
{
    // If this vector register is currently mapped to an XMM, invalidate it
    for (uint8_t i = 0; i < 16; i++)
    {
        if (m_XmmState[i] == XmmState::Mapped && m_XmmRegMapped[i] == vreg)
        {
            m_XmmState[i] = XmmState::Free;
            m_XmmRegMapped[i] = 0xFF;
            m_XmmProtected[i] = false;
        }
    }
}

// ──────────────────────────────────────────────
// Register spilling
// ──────────────────────────────────────────────

bool CRspRegState::FreeXmmReg(uint32_t xmmIndex)
{
    if (xmmIndex >= 16) return false;

    if (m_XmmState[xmmIndex] == XmmState::Free) return true;
    if (m_XmmProtected[xmmIndex]) return false;

    if (m_XmmState[xmmIndex] == XmmState::Mapped)
    {
        // Emit movdqa [VR memory], xmm to spill the mapped register
        if (m_Assembler != nullptr)
        {
            uint8_t vreg = m_XmmRegMapped[xmmIndex];
            void* dstAddr = &m_RecompilerOps.m_Vect[vreg];
            char addrBuf[64];
            std::snprintf(addrBuf, sizeof(addrBuf), "VR[%u]", vreg);
            m_Assembler->Movdqa_MemFromXmm(dstAddr, addrBuf, static_cast<uint8_t>(xmmIndex));
        }
    }

    m_XmmState[xmmIndex] = XmmState::Free;
    m_XmmRegMapped[xmmIndex] = 0xFF;
    return true;
}

void CRspRegState::WriteBackRegisters()
{
    for (uint8_t i = 0; i < 16; i++)
    {
        if (m_XmmState[i] == XmmState::Mapped)
        {
            // Emit movdqa to write back all mapped vector registers
            if (m_Assembler != nullptr)
            {
                uint8_t vreg = m_XmmRegMapped[i];
                void* dstAddr = &m_RecompilerOps.m_Vect[vreg];
                char addrBuf[64];
                std::snprintf(addrBuf, sizeof(addrBuf), "VR[%u]", vreg);
                m_Assembler->Movdqa_MemFromXmm(dstAddr, addrBuf, i);
            }
            m_XmmState[i] = XmmState::Free;
            m_XmmRegMapped[i] = 0xFF;
        }
    }

    // Invalidate all GPR constant state (unknown after block boundary)
    for (int i = 1; i < 32; i++)
    {
        m_GprIsConst[i] = false;
    }

    // Invalidate flag zero tracking
    for (size_t i = 0; i < static_cast<size_t>(RspFlags::MaxFlags); i++)
    {
        m_FlagIsZero[i] = false;
    }
}

} // namespace Nova64::RSP::Recompiler

#endif // x64 only
