/**
 * @file RSPRegisters.hpp
 * @brief RSP Registers.
 */
// SuperNova64 - N64 Emulator
// RSP Registers - Modernized from Project64's RSPRegisters.h
// C++20 with proper encapsulation, constexpr status bits

#pragma once
#include "RspTypes.hpp"
#include "RSPOpcode.hpp"
#include <cstdint>
#include <array>

namespace Nova64::RSP {

// ──────────────────────────────────────────────
// SP Status Register bit definitions (read)
// ──────────────────────────────────────────────
namespace SPStatus {
    inline constexpr uint32_t Halt       = 0x0001;  // Bit  0: RSP halted
    inline constexpr uint32_t Broke      = 0x0002;  // Bit  1: RSP hit BREAK
    inline constexpr uint32_t DmaBusy    = 0x0004;  // Bit  2: DMA in progress
    inline constexpr uint32_t DmaFull    = 0x0008;  // Bit  3: DMA queue full
    inline constexpr uint32_t IoFull     = 0x0010;  // Bit  4: IO queue full
    inline constexpr uint32_t SingleStep = 0x0020;  // Bit  5: Single-step mode
    inline constexpr uint32_t IntrBreak  = 0x0040;  // Bit  6: Interrupt on BREAK
    inline constexpr uint32_t Signal0    = 0x0080;  // Bit  7: Signal 0
    inline constexpr uint32_t Signal1    = 0x0100;  // Bit  8: Signal 1
    inline constexpr uint32_t Signal2    = 0x0200;  // Bit  9: Signal 2
    inline constexpr uint32_t Signal3    = 0x0400;  // Bit 10: Signal 3
    inline constexpr uint32_t Signal4    = 0x0800;  // Bit 11: Signal 4
    inline constexpr uint32_t Signal5    = 0x1000;  // Bit 12: Signal 5
    inline constexpr uint32_t Signal6    = 0x2000;  // Bit 13: Signal 6
    inline constexpr uint32_t Signal7    = 0x4000;  // Bit 14: Signal 7
}

// ──────────────────────────────────────────────
// SP Status Register write bits (clear/set pairs)
// ──────────────────────────────────────────────
namespace SPWrite {
    inline constexpr uint32_t ClrHalt       = 0x00001;
    inline constexpr uint32_t SetHalt       = 0x00002;
    inline constexpr uint32_t ClrBroke      = 0x00004;
    inline constexpr uint32_t ClrIntr       = 0x00008;
    inline constexpr uint32_t SetIntr       = 0x00010;
    inline constexpr uint32_t ClrSStep      = 0x00020;
    inline constexpr uint32_t SetSStep      = 0x00040;
    inline constexpr uint32_t ClrIntrBreak  = 0x00080;
    inline constexpr uint32_t SetIntrBreak  = 0x00100;
    inline constexpr uint32_t ClrSignal0    = 0x00200;
    inline constexpr uint32_t SetSignal0    = 0x00400;
    inline constexpr uint32_t ClrSignal1    = 0x00800;
    inline constexpr uint32_t SetSignal1    = 0x01000;
    inline constexpr uint32_t ClrSignal2    = 0x02000;
    inline constexpr uint32_t SetSignal2    = 0x04000;
    inline constexpr uint32_t ClrSignal3    = 0x08000;
    inline constexpr uint32_t SetSignal3    = 0x10000;
    inline constexpr uint32_t ClrSignal4    = 0x20000;
    inline constexpr uint32_t SetSignal4    = 0x40000;
    inline constexpr uint32_t ClrSignal5    = 0x80000;
    inline constexpr uint32_t SetSignal5    = 0x100000;
    inline constexpr uint32_t ClrSignal6    = 0x200000;
    inline constexpr uint32_t SetSignal6    = 0x400000;
    inline constexpr uint32_t ClrSignal7    = 0x800000;
    inline constexpr uint32_t SetSignal7    = 0x1000000;
}

// ──────────────────────────────────────────────
// DPC Status bits
// ──────────────────────────────────────────────
namespace DPCWrite {
    inline constexpr uint32_t ClrXbusDmemDma = 0x0001;
    inline constexpr uint32_t SetXbusDmemDma = 0x0002;
    inline constexpr uint32_t ClrFreeze      = 0x0004;
    inline constexpr uint32_t SetFreeze      = 0x0008;
    inline constexpr uint32_t ClrFlush       = 0x0010;
    inline constexpr uint32_t SetFlush       = 0x0020;
    inline constexpr uint32_t ClrTmemCtr     = 0x0040;
    inline constexpr uint32_t ClrPipeCtr     = 0x0080;
    inline constexpr uint32_t ClrCmdCtr      = 0x0100;
    inline constexpr uint32_t ClrClockCtr    = 0x0200;
}

namespace DPCStatus {
    inline constexpr uint32_t XbusDmemDma = 0x001;
    inline constexpr uint32_t Freeze      = 0x002;
    inline constexpr uint32_t Flush       = 0x004;
    inline constexpr uint32_t StartGclk   = 0x008;
    inline constexpr uint32_t TmemBusy    = 0x010;
    inline constexpr uint32_t PipeBusy    = 0x020;
    inline constexpr uint32_t CmdBusy     = 0x040;
    inline constexpr uint32_t CbufReady   = 0x080;
    inline constexpr uint32_t DmaBusy     = 0x100;
    inline constexpr uint32_t EndValid    = 0x200;
    inline constexpr uint32_t StartValid  = 0x400;
}

// MI interrupt bit for SP
inline constexpr uint32_t MI_INTR_SP = 0x01;

// ──────────────────────────────────────────────
// Named GPR references for readability
// ──────────────────────────────────────────────
class CGPRRegisters {
public:
    explicit CGPRRegisters(Word32 (&gpr)[32])
        : GPR_R0(gpr[0].UW),  GPR_AT(gpr[1].UW),  GPR_V0(gpr[2].UW),  GPR_V1(gpr[3].UW)
        , GPR_A0(gpr[4].UW),  GPR_A1(gpr[5].UW),  GPR_A2(gpr[6].UW),  GPR_A3(gpr[7].UW)
        , GPR_T0(gpr[8].UW),  GPR_T1(gpr[9].UW),  GPR_T2(gpr[10].UW), GPR_T3(gpr[11].UW)
        , GPR_T4(gpr[12].UW), GPR_T5(gpr[13].UW), GPR_T6(gpr[14].UW), GPR_T7(gpr[15].UW)
        , GPR_S0(gpr[16].UW), GPR_S1(gpr[17].UW), GPR_S2(gpr[18].UW), GPR_S3(gpr[19].UW)
        , GPR_S4(gpr[20].UW), GPR_S5(gpr[21].UW), GPR_S6(gpr[22].UW), GPR_S7(gpr[23].UW)
        , GPR_T8(gpr[24].UW), GPR_T9(gpr[25].UW), GPR_K0(gpr[26].UW), GPR_K1(gpr[27].UW)
        , GPR_GP(gpr[28].UW), GPR_SP(gpr[29].UW), GPR_S8(gpr[30].UW), GPR_RA(gpr[31].UW)
    {}

protected:
    uint32_t& GPR_R0; uint32_t& GPR_AT; uint32_t& GPR_V0; uint32_t& GPR_V1;
    uint32_t& GPR_A0; uint32_t& GPR_A1; uint32_t& GPR_A2; uint32_t& GPR_A3;
    uint32_t& GPR_T0; uint32_t& GPR_T1; uint32_t& GPR_T2; uint32_t& GPR_T3;
    uint32_t& GPR_T4; uint32_t& GPR_T5; uint32_t& GPR_T6; uint32_t& GPR_T7;
    uint32_t& GPR_S0; uint32_t& GPR_S1; uint32_t& GPR_S2; uint32_t& GPR_S3;
    uint32_t& GPR_S4; uint32_t& GPR_S5; uint32_t& GPR_S6; uint32_t& GPR_S7;
    uint32_t& GPR_T8; uint32_t& GPR_T9; uint32_t& GPR_K0; uint32_t& GPR_K1;
    uint32_t& GPR_GP; uint32_t& GPR_SP; uint32_t& GPR_S8; uint32_t& GPR_RA;

private:
    CGPRRegisters() = delete;
};

// ──────────────────────────────────────────────
// Complete RSP register file
// ──────────────────────────────────────────────
class CRSPRegisters {
public:
    CRSPRegisters() { Reset(); }

    void Reset() {
        for (auto& r : m_GPR) r.UW = 0;
        m_ACCUM.Reset();
        for (auto& v : m_Vect) v = RSPVector{};
        for (auto& r : m_Reciprocals) r = 0;
        for (auto& r : m_InverseSquareRoots) r = 0;
        m_Result = 0;
        m_In = 0;
        m_High = false;
        m_VCOL.Clear(); m_VCOH.Clear();
        m_VCCL.Clear(); m_VCCH.Clear();
        m_VCE.Clear();
    }

    // Scalar GPRs (32 × 32-bit, $0 always zero)
    Word32          m_GPR[32]{};

    // Vector unit state
    RSPAccumulator  m_ACCUM;
    RSPVector       m_Vect[32];

    // Reciprocal / inverse-sqrt lookup tables
    uint16_t        m_Reciprocals[512]{};
    uint16_t        m_InverseSquareRoots[512]{};
    uint16_t        m_Result{0};
    uint16_t        m_In{0};
    bool            m_High{false};

    // Vector compare/carry/extension flags
    RSPFlag         m_VCOL, m_VCOH;  // VCO low/high
    RSPFlag         m_VCCL, m_VCCH;  // VCC low/high
    RSPFlag         m_VCE;           // VCE
};

} // namespace Nova64::RSP
