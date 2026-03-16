// SuperNova64 - N64 Emulator
// RSP Register State Tracking - Modernized from Project64's RspRegState.h
// x64 recompiler register allocation and constant propagation

#pragma once
#include <cstdint>

// Only used for x64 recompiler
#if defined(__amd64__) || defined(_M_X64)

namespace Nova64::RSP::Recompiler {

class RspAssembler;
class CRSPRecompilerOps;

// ──────────────────────────────────────────────
// RSP flag identifiers
// ──────────────────────────────────────────────
enum class RspFlags : uint32_t {
    VCOL,
    VCOH,
    VCCL,
    VCCH,
    VCE,
    MaxFlags,
};

// ──────────────────────────────────────────────
// XMM register allocation state
// ──────────────────────────────────────────────
enum class XmmState : uint8_t {
    Free,       // Available for allocation
    Zero,       // Contains zeroed register
    Mapped,     // Mapped to RSP vector register
    Temp,       // Temporary (will be freed after use)
    Reserved,   // Reserved for system use
};

// ──────────────────────────────────────────────
// Recompiler register state tracker
// Manages XMM/GPR register allocation and constant propagation
// ──────────────────────────────────────────────
class CRspRegState {
public:
    explicit CRspRegState(CRSPRecompilerOps& recompilerOps);
    ~CRspRegState();

    CRspRegState(const CRspRegState&) = delete;
    CRspRegState& operator=(const CRspRegState&) = delete;

    void ResetRegProtection();

    // XMM register mapping (vector registers → XMM)
    // asmjit types would go here - using uint8_t indices for now
    uint8_t MapXmmZero();
    uint8_t MapXmmReg(uint8_t vreg, uint8_t source, bool loadSource = true);
    uint8_t MapXmmTemp(bool loadReg, uint8_t vreg, uint8_t e = 0);
    uint8_t MapSpecificXmmTemp(uint8_t xmmIndex, bool loadReg, uint8_t vreg, uint8_t e = 0);
    uint8_t VRegMapping(uint8_t vreg) const;
    void ProtectXmm(uint8_t xmmIdx);
    void UnprotectXmm(uint8_t xmmIdx);

    // GPR constant propagation
    [[nodiscard]] bool IsGprConst(uint8_t gprReg) const;
    [[nodiscard]] uint32_t GetGprConstValue(uint8_t gprReg) const;
    void SetGprConst(uint8_t gprReg, uint32_t value);
    void SetGprUnknown(uint8_t gprReg);

    // Flag zero tracking
    [[nodiscard]] bool IsFlagZero(RspFlags flag) const;
    void SetFlagZero(RspFlags flag);
    void SetFlagUnknown(RspFlags flag);

    // Vector register invalidation
    void SetVecRegUnknown(uint8_t vreg);

    // Register spilling
    bool FreeXmmReg(uint32_t xmmIndex);
    void WriteBackRegisters();

private:
    CRSPRecompilerOps& m_RecompilerOps;
    RspAssembler*&     m_Assembler;

    // XMM state (16 XMM registers on x64)
    XmmState m_XmmState[16]{};
    uint8_t  m_XmmRegMapped[16]{};
    bool     m_XmmProtected[16]{};

    // GPR constant propagation
    bool     m_GprIsConst[32]{};
    uint32_t m_GprConstValue[32]{};

    // Flag zero tracking
    bool m_FlagIsZero[static_cast<size_t>(RspFlags::MaxFlags)]{};
};

} // namespace Nova64::RSP::Recompiler

#endif // x64 only
