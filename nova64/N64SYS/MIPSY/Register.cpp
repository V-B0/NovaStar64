/**
 * @file Register.cpp
 * @brief Implementation of N64 CPU registers and COP0 functionality
 * 
 * Based on Project64's Register implementation.
 * Handles CPU registers, COP0 register access, interrupt handling,
 * and exception management for the MIPS R4300i CPU.
 */

#include "Register.hpp"
#include "../StateTrace.hpp"
#include "Sysevents.hpp"
#include "Systiming.hpp"
#include "Vmem.hpp"
#include "TLB.hpp"
#include "../SysGlobals.hpp"
#include <atomic>
#include "../MainSYS.hpp"
#include "../N64Rom.hpp"
#include "../N64DD.hpp"
#include "../../common/log.hpp"

#include <cstring>
#include <cfenv>
#include <cstdlib>

// ============================================================================
// Register Name Tables
// ============================================================================

const char* const CRegName::GPR[32] = {"R0", "AT", "V0", "V1", "A0", "A1", "A2", "A3", "T0", "T1", "T2",
                                       "T3", "T4", "T5", "T6", "T7", "S0", "S1", "S2", "S3", "S4", "S5",
                                       "S6", "S7", "T8", "T9", "K0", "K1", "GP", "SP", "FP", "RA"};

const char* const CRegName::GPR_Hi[32] = {"r0.HI", "at.HI", "v0.HI", "v1.HI", "a0.HI", "a1.HI", "a2.HI", "a3.HI",
                                          "t0.HI", "t1.HI", "t2.HI", "t3.HI", "t4.HI", "t5.HI", "t6.HI", "t7.HI",
                                          "s0.HI", "s1.HI", "s2.HI", "s3.HI", "s4.HI", "s5.HI", "s6.HI", "s7.HI",
                                          "t8.HI", "t9.HI", "k0.HI", "k1.HI", "gp.HI", "sp.HI", "fp.HI", "ra.HI"};

const char* const CRegName::GPR_Lo[32] = {"r0.LO", "at.LO", "v0.LO", "v1.LO", "a0.LO", "a1.LO", "a2.LO", "a3.LO",
                                          "t0.LO", "t1.LO", "t2.LO", "t3.LO", "t4.LO", "t5.LO", "t6.LO", "t7.LO",
                                          "s0.LO", "s1.LO", "s2.LO", "s3.LO", "s4.LO", "s5.LO", "s6.LO", "s7.LO",
                                          "t8.LO", "t9.LO", "k0.LO", "k1.LO", "gp.LO", "sp.LO", "fp.LO", "ra.LO"};

const char* const CRegName::Cop0[32] = {"Index", "Random",   "EntryLo0", "EntryLo1", "Context", "PageMask", "Wired",
                                        "Reg7",  "BadVAddr", "Count",    "EntryHi",  "Compare", "Status",   "Cause",
                                        "EPC",   "PRId",     "Config",   "LLAddr",   "WatchLo", "WatchHi",  "XContext",
                                        "Reg21", "Reg22",    "Reg23",    "Reg24",    "Reg25",   "ECC",      "CacheErr",
                                        "TagLo", "TagHi",    "ErrEPC",   "Reg31"};

const char* const CRegName::FPR[32] = {"F0",  "F1",  "F2",  "F3",  "F4",  "F5",  "F6",  "F7",  "F8",  "F9",  "F10",
                                       "F11", "F12", "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21",
                                       "F22", "F23", "F24", "F25", "F26", "F27", "F28", "F29", "F30", "F31"};

const char* const CRegName::FPR_Ctrl[32] = {
    "Revision", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown",
    "Unknown",  "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown",
    "Unknown",  "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown",
    "Unknown",  "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "FCSR"};

// ============================================================================
// CRegisters Implementation
// ============================================================================

CRegisters::CRegisters(CN64System& System, CSystemEvents& SystemEvents) :
    CP0registers(), m_FirstInterrupt(true), m_System(System), m_SystemEvents(SystemEvents),
    m_SystemTimer(*System.GetSystemTimer()), m_TLB(*System.GetTLB()) {
    Init();
}

void CRegisters::Init() {
    m_FirstInterrupt = true;

    // Clear all general purpose registers
    m_GPR.fill(0);

    // Clear COP0 registers
    m_CP0.fill(0);

    // Clear floating point registers
    m_FPR.fill(0);
    m_FPCR.fill(0);
    m_FPCR[0] = 0xA00; // FPU revision register

    // Clear latches
    m_CP0Latch = 0;
    m_CP2Latch = 0;

    // Clear HI/LO registers
    m_HI = 0;
    m_LO = 0;

    // Clear LL bit
    m_LLBit = 0;

    // Reset system registers
    m_RDRAM_Interface.fill(0);
    m_RDRAM_Registers.fill(0);
    m_Mips_Interface.fill(0);
    m_Video_Interface.fill(0);
    m_Display_ControlReg.fill(0);
    m_Audio_Interface.fill(0);
    m_SigProcessor_Interface.fill(0);
    m_Peripheral_Interface.fill(0);
    m_SerialInterface.fill(0);
    m_DiskInterface.fill(0);

    m_AudioIntrReg = 0;
    m_GfxIntrReg = 0;
    m_RspIntrReg = 0;

    // Initialize RDRAM Interface registers
    m_RDRAM_Interface[0] = 0x0E;       // RI_MODE_REG
    m_RDRAM_Interface[1] = 0x40;       // RI_CONFIG_REG
    m_RDRAM_Interface[3] = 0x14;       // RI_SELECT_REG
    m_RDRAM_Interface[4] = 0x00063634; // RI_REFRESH_REG

    FixFpuLocations();
}

void CRegisters::Reset(bool bPostPif, Nova64::MipsMemoryVM& MMU) {
    Init();

    // COP0 registers initial values
    RANDOM_REGISTER = 0x1F;
    COUNT_REGISTER = 0x5000;
    m_Mips_Interface[1] = 0x02020102;         // MI_VERSION_REG (index 1, not 2)
    m_SigProcessor_Interface[4] = 0x00000001; // SP_STATUS_REG
    CAUSE_REGISTER.Value = 0x00000000;
    CONTEXT_REGISTER.Value = 0x007FFFF0;
    EPC_REGISTER = 0xFFFFFFFFFFFFFFFF;
    BAD_VADDR_REGISTER = 0xFFFFFFFFFFFFFFFF;
    ERROREPC_REGISTER = 0xFFFFFFFFFFFFFFFF;
    PREVID_REGISTER = 0x00000B22;
    CONFIG_REGISTER = 0x7006E463;
    STATUS_REGISTER.Value = 0x34000000;

    // N64DD registers - Start N64DD in reset state and motor not spinning
    m_DiskInterface[0] = 0x00000800 | 0x00001000; // DD_STATUS_RST_STATE | DD_STATUS_MTR_N_SPIN
    m_DiskInterface[2] = 0x00030000;              // ASIC_ID_REG

    FixFpuLocations();

    if (bPostPif) {
        m_PROGRAM_COUNTER = 0xFFFFFFFFA4000040;

        m_GPR[0] = 0x0000000000000000;
        m_GPR[6] = 0xFFFFFFFFA4001F0C;
        m_GPR[7] = 0xFFFFFFFFA4001F08;
        m_GPR[8] = 0x00000000000000C0;
        m_GPR[9] = 0x0000000000000000;
        m_GPR[10] = 0x0000000000000040;
        m_GPR[11] = 0xFFFFFFFFA4000040;
        m_GPR[16] = 0x0000000000000000;
        m_GPR[17] = 0x0000000000000000;
        m_GPR[18] = 0x0000000000000000;
        m_GPR[19] = 0x0000000000000000;
        m_GPR[21] = 0x0000000000000000;
        m_GPR[26] = 0x0000000000000000;
        m_GPR[27] = 0x0000000000000000;
        m_GPR[28] = 0x0000000000000000;
        m_GPR[29] = 0xFFFFFFFFA4001FF0;
        m_GPR[30] = 0x0000000000000000;

        // Default NTSC values
        m_GPR[20] = 0x0000000000000001;
        m_GPR[23] = 0x0000000000000000;
        m_GPR[24] = 0x0000000000000003;
        m_GPR[31] = 0xFFFFFFFFA4001550;

        // CIC-specific register initialization
        // Auto-detect CIC type from ROM header
        using N64::Rom::CICChip;
        CICChip cicChip = CICChip::CIC_NUS_6102; // default fallback
        if (N64Rom* rom = m_System.GetRom()) {
            cicChip = rom->GetImage().GetCICChip();
        }

        switch (cicChip) {
        case CICChip::CIC_NUS_6101:
            m_GPR[1] = 0x0000000000000000;
            m_GPR[2] = 0xFFFFFFFFF58B0FBF;
            m_GPR[3] = 0xFFFFFFFFF58B0FBF;
            m_GPR[4] = 0x0000000000000FBF;
            m_GPR[5] = 0xFFFFFFFF25613A26;
            m_GPR[16] = 0xFFFFFFFFED10D0B3;
            m_GPR[17] = 0x000000001402A4CC;
            m_GPR[18] = 0x000000002DE108EA;
            m_GPR[19] = 0x000000003103E121;
            m_GPR[22] = 0x000000000000003F;
            m_GPR[25] = 0xFFFFFFFF9DEBB54F;
            break;
        case CICChip::CIC_NUS_6102:
            m_GPR[1] = 0x0000000000000001;
            m_GPR[2] = 0x000000000EBDA536;
            m_GPR[3] = 0x000000000EBDA536;
            m_GPR[4] = 0x0000000000000001;
            m_GPR[5] = 0xFFFFFFFFC95973D5;
            m_GPR[11] = 0x0000000000000000;
            m_GPR[16] = 0x0000000000000001;
            m_GPR[17] = 0x0000000000000000;
            m_GPR[18] = 0x000000002449A366;
            m_GPR[19] = 0x000000003103E121;
            m_GPR[22] = 0x000000000000003F;
            m_GPR[25] = 0xFFFFFFFF9DEBB54F;
            break;
        case CICChip::CIC_NUS_6103:
            m_GPR[1] = 0x0000000000000001;
            m_GPR[2] = 0x0000000049A5EE96;
            m_GPR[3] = 0x0000000049A5EE96;
            m_GPR[4] = 0x000000000000EE96;
            m_GPR[5] = 0xFFFFFFFFD4646273;
            m_GPR[16] = 0xFFFFFFFFCE9DFBF7;
            m_GPR[17] = 0xFFFFFFFFCE9DFBF7;
            m_GPR[18] = 0x000000001AF99984;
            m_GPR[19] = 0x0000000018B63D28;
            m_GPR[22] = 0x0000000000000078;
            m_GPR[25] = 0xFFFFFFFF825B21C9;
            break;
        case CICChip::CIC_NUS_6105:
            m_GPR[1] = 0x0000000000000000;
            m_GPR[2] = 0xFFFFFFFFF58B0FBF;
            m_GPR[3] = 0xFFFFFFFFF58B0FBF;
            m_GPR[4] = 0x0000000000000FBF;
            m_GPR[5] = 0xFFFFFFFFDECAAAD1;
            m_GPR[16] = 0xFFFFFFFF9651F81E;
            m_GPR[17] = 0x000000002D42AAC5;
            m_GPR[18] = 0x000000005CD2B70F;
            m_GPR[19] = 0x0000000056584D60;
            m_GPR[22] = 0x0000000000000091;
            m_GPR[25] = 0xFFFFFFFFCDCE565F;
            break;
        case CICChip::CIC_NUS_6106:
            m_GPR[1] = 0x0000000000000000;
            m_GPR[2] = 0xFFFFFFFFA95930A4;
            m_GPR[3] = 0xFFFFFFFFA95930A4;
            m_GPR[4] = 0x00000000000030A4;
            m_GPR[5] = 0xFFFFFFFFB04DC903;
            m_GPR[16] = 0xFFFFFFFFBCB59510;
            m_GPR[17] = 0xFFFFFFFFBCB59510;
            m_GPR[18] = 0x000000000CF85C13;
            m_GPR[19] = 0x000000007A3C07F4;
            m_GPR[22] = 0x0000000000000085;
            m_GPR[25] = 0x00000000465E3F72;
            break;
        default: // Fall back to CIC-6102
            m_GPR[1] = 0x0000000000000001;
            m_GPR[2] = 0x000000000EBDA536;
            m_GPR[3] = 0x000000000EBDA536;
            m_GPR[4] = 0x0000000000000001;
            m_GPR[5] = 0xFFFFFFFFC95973D5;
            m_GPR[11] = 0x0000000000000000;
            m_GPR[16] = 0x0000000000000001;
            m_GPR[17] = 0x0000000000000000;
            m_GPR[18] = 0x000000002449A366;
            m_GPR[19] = 0x000000003103E121;
            m_GPR[22] = 0x000000000000003F;
            m_GPR[25] = 0xFFFFFFFF9DEBB54F;
            break;
        }
    } else {
        m_PROGRAM_COUNTER = 0xFFFFFFFFBFC00000;
    }

    m_TLB.COP0StatusChanged();
}

// ============================================================================
// COP0 Register Access
// ============================================================================

uint64_t& CRegisters::GetCop0RegRef(COP0Reg reg) { return m_CP0[static_cast<uint8_t>(reg)]; }

const uint64_t& CRegisters::GetCop0RegRef(COP0Reg reg) const { return m_CP0[static_cast<uint8_t>(reg)]; }

uint64_t CRegisters::Cop0_MF(COP0Reg Reg) {
    uint8_t regIndex = static_cast<uint8_t>(Reg);

    // Count/Wired/Random: bring the timer system fully up-to-date first.
    // Like Project64, UpdateTimers() modifies COUNT_REGISTER in place,
    // decrements all timer counters, and fires any expired timers.
    if (Reg == COP0Reg::Count || Reg == COP0Reg::Wired) {
        m_SystemTimer.UpdateTimers();
    } else if (Reg == COP0Reg::Random) {
        m_SystemTimer.UpdateTimers();
        // On the R4300i, Random is a down-counter, not a PRNG: it decrements
        // as the CPU runs and wraps from Wired back to 31. Deriving it from
        // Count reproduces that shape while keeping it a pure function of
        // emulated state -- using rand() here made every run of the same ROM
        // take a different path through TLBWR, which defeats save states,
        // trace comparison, and any attempt to reproduce a bug.
        uint32_t wired = static_cast<uint32_t>(WIRED_REGISTER) & 0x3F;
        const uint32_t count = static_cast<uint32_t>(COUNT_REGISTER);
        if (wired > 31) {
            return count & 0x3F;
        }
        const uint32_t range = 32 - wired;
        return 31 - (count % range);
    }
    // Unused registers return latch value
    else if (regIndex == 7 || regIndex == 21 || regIndex == 22 || regIndex == 23 || regIndex == 24 || regIndex == 25 ||
             regIndex == 31) {
        return m_CP0Latch;
    }

    return regIndex <= 31 ? m_CP0[regIndex] : 0;
}

void CRegisters::Cop0_MT(COP0Reg Reg, uint64_t Value) {
    uint8_t regIndex = static_cast<uint8_t>(Reg);
    m_CP0Latch = Value;

    switch (Reg) {
    case COP0Reg::EPC:
    case COP0Reg::TagLo:
    case COP0Reg::TagHi:
    case COP0Reg::ErrorEPC: m_CP0[regIndex] = Value; break;

    case COP0Reg::Index: m_CP0[regIndex] = Value & 0x8000003F; break;

    case COP0Reg::Random:
        // Ignore - Read only
        break;

    case COP0Reg::EntryLo0:
    case COP0Reg::EntryLo1:
        m_CP0[regIndex] = Value & 0x3FFFFFFF; // VR4300: 30-bit (PFN[29:6] C[5:3] D V G)
        break;

    case COP0Reg::Context: m_CP0[regIndex] = (Value & 0xFFFFFFFFFF800000) | (m_CP0[regIndex] & 0x7FFFF0); break;

    case COP0Reg::PageMask: m_CP0[regIndex] = Value & 0x01FFE000; break;

    case COP0Reg::Wired:
        m_SystemTimer.UpdateTimers();
        m_CP0[regIndex] = Value & 0x3F;
        RANDOM_REGISTER = 31; // VR4300: writing Wired resets Random to 31
        break;

    case COP0Reg::BadVAddr:
        // Ignore - Read only
        break;

    case COP0Reg::Count:
        m_SystemTimer.UpdateTimers();
        m_CP0[regIndex] = Value;
        m_SystemTimer.update_compare_timer();
        break;

    case COP0Reg::EntryHi: m_CP0[regIndex] = Value & 0xC00000FFFFFFE0FF; break;

    case COP0Reg::Compare:
        m_SystemTimer.UpdateTimers();
        // Compare is written by the game's scheduler from the Count it just
        // read, so its value is a sensitive indicator of timing divergence.
        Nova64::Trace::RecordEvent("cmpw", COUNT_REGISTER);
        m_CP0[regIndex] = Value;
        CAUSE_REGISTER.PendingInterrupts &= ~COP0Flags::CAUSE_IP7;
        m_SystemTimer.update_compare_timer();
        m_System.m_TestTimer = true;
        break;

    case COP0Reg::Status: {
        bool FRBitChanged = STATUS_REGISTER.FR != ((COP0Status&)Value).FR;
        STATUS_REGISTER.Value = Value & 0xFFF7FFFF;
        if (FRBitChanged) {
            FixFpuLocations();
        }
        m_TLB.COP0StatusChanged();
        CheckInterrupts();
        m_System.m_TestTimer = true;
        break;
    }

    case COP0Reg::Cause:
        // Only bits 9:8 (software interrupt pending IP0, IP1) are writable
        m_CP0[regIndex] = (m_CP0[regIndex] & ~0x300) | (Value & 0x300);
        break;

    case COP0Reg::PRId:
        // Read only
        break;

    case COP0Reg::Config: m_CP0[regIndex] = (Value & 0x0F00800F) | (m_CP0[regIndex] & 0xF0FF7FF0); break;

    case COP0Reg::LLAddr: m_CP0[regIndex] = (Value & 0xFFFFFFFF) | (m_CP0[regIndex] & 0xFFFFFFFF00000000); break;

    case COP0Reg::XContext:
        m_CP0[regIndex] = (Value & 0xFFFFFFFE00000000) | (m_CP0[regIndex] & 0x00000001FFFFFFFF);
        break;

    case COP0Reg::ECC: m_CP0[regIndex] = Value & 0xFF; break;

    default:
        // Unused registers - ignore
        break;
    }
}

// ============================================================================
// COP1 (FPU) Register Access
// ============================================================================

void CRegisters::Cop1_CT(uint32_t Reg, uint32_t Value) {
    if (Reg == 31) {
        FPStatusReg& StatusReg = reinterpret_cast<FPStatusReg&>(m_FPCR[31]);
        StatusReg.Value = (Value & 0x183FFFF);

        // Check for FPU exceptions
        if (((StatusReg.Cause.Inexact & StatusReg.Enable.Inexact) != 0) ||
            ((StatusReg.Cause.Underflow & StatusReg.Enable.Underflow) != 0) ||
            ((StatusReg.Cause.Overflow & StatusReg.Enable.Overflow) != 0) ||
            ((StatusReg.Cause.DivisionByZero & StatusReg.Enable.DivisionByZero) != 0) ||
            ((StatusReg.Cause.InvalidOperation & StatusReg.Enable.InvalidOperation) != 0) ||
            (StatusReg.Cause.UnimplementedOperation != 0)) {
            TriggerException(static_cast<uint32_t>(ExceptionCode::EXC_FPE));
        }
    }
}

// ============================================================================
// COP2 Register Access (RCP vector unit - not used on N64 CPU)
// ============================================================================

uint64_t CRegisters::Cop2_MF(uint32_t /*Reg*/) { return m_CP2Latch; }

void CRegisters::Cop2_MT(uint32_t /*Reg*/, uint64_t Value) { m_CP2Latch = Value; }

// ============================================================================
// Interrupt Handling
// ============================================================================

void CRegisters::CheckInterrupts() {
    uint32_t MI_INTR_REG_Value = m_Mips_Interface[2]; // MI_INTR_REG

    MI_INTR_REG_Value |= (m_RspIntrReg & MIFlags::INTR_SP);
    MI_INTR_REG_Value |= (m_GfxIntrReg & MIFlags::INTR_DP);

    // Check if any enabled interrupts are pending
    if ((m_Mips_Interface[3] & MI_INTR_REG_Value) != 0) // MI_INTR_MASK_REG
    {
        CAUSE_REGISTER.PendingInterrupts |= COP0Flags::CAUSE_IP2;
    } else {
        CAUSE_REGISTER.PendingInterrupts &= ~COP0Flags::CAUSE_IP2;
    }

    m_Mips_Interface[2] = MI_INTR_REG_Value;

    COP0Status STATUS_REGISTER_Value = STATUS_REGISTER;

    // Check if interrupts are enabled
    if (STATUS_REGISTER_Value.InterruptEnable == 0 || STATUS_REGISTER_Value.ExceptionLevel != 0 ||
        STATUS_REGISTER_Value.ErrorLevel != 0) {
        return;
    }

    // Check if there are pending interrupts to service
    if ((STATUS_REGISTER_Value.Value & CAUSE_REGISTER.Value & 0xFF00) != 0) {
        m_SystemEvents.QueueEvent(SysEvent_ExecuteInterrupt);
    }
}

bool CRegisters::DoIntrException() {
    if (STATUS_REGISTER.InterruptEnable == 0 || STATUS_REGISTER.ExceptionLevel != 0 ||
        STATUS_REGISTER.ErrorLevel != 0) {
        return false;
    }

    Nova64::Trace::RecordEvent("intr", COUNT_REGISTER);
    TriggerException(static_cast<uint32_t>(ExceptionCode::EXC_INT), 0);
    return true;
}

// ============================================================================
// Exception Handling
// ============================================================================

void CRegisters::DoAddressError(uint64_t BadVaddr, bool FromRead) {
    TriggerAddressException(BadVaddr, FromRead ? static_cast<uint32_t>(ExceptionCode::EXC_RADE)
                                               : static_cast<uint32_t>(ExceptionCode::EXC_WADE));
}

void CRegisters::TriggerAddressException(uint64_t Address, uint32_t ExceptionCode) {
    bool SpecialOffset = false;

    if (ExceptionCode == static_cast<uint32_t>(ExceptionCode::EXC_RMISS) ||
        ExceptionCode == static_cast<uint32_t>(ExceptionCode::EXC_WMISS) ||
        ExceptionCode == static_cast<uint32_t>(ExceptionCode::EXC_MOD)) {
        Log::infof("Address Exception (TLB/Align): Code=%d BadVAddr=0x%016llX PC=0x%08X", ExceptionCode, Address,
                   static_cast<uint32_t>(m_PROGRAM_COUNTER));
    }

    if (ExceptionCode == static_cast<uint32_t>(ExceptionCode::EXC_RMISS) ||
        ExceptionCode == static_cast<uint32_t>(ExceptionCode::EXC_WMISS)) {
        bool Dirty = false;
        auto result = m_TLB.AddressLookup(Address, Dirty);
        // Only use refill vector for actual TLB miss (no match).
        // Match with V=0 (invalid) uses the general vector.
        SpecialOffset = (result == CTLB::TlbLookupResult::NoMatch);
        if (ExceptionCode == static_cast<uint32_t>(ExceptionCode::EXC_WMISS) &&
            result == CTLB::TlbLookupResult::MatchValid && !Dirty) {
            ExceptionCode = static_cast<uint32_t>(ExceptionCode::EXC_MOD);
        }
    }

    BAD_VADDR_REGISTER = Address;
    ENTRYHI_REGISTER.SetFromAddress(Address);
    CONTEXT_REGISTER.BadVPN2 = Address >> 13;
    XCONTEXT_REGISTER.BadVPN2 = Address >> 13;
    XCONTEXT_REGISTER.R = Address >> 62;

    // Save EXL state before TriggerException may set it
    bool wasExlSet = (STATUS_REGISTER.ExceptionLevel != 0);

    TriggerException(ExceptionCode, 0);

    // TLB refill vector (SpecialOffset) only used when EXL was 0 before exception.
    // When EXL was already 1 (nested exception), use general exception vector.
    if (SpecialOffset && !wasExlSet) {
        m_System.m_JumpToLocation = (m_System.m_JumpToLocation & 0xFFFFFFFFFFFF0000);
        // BEV=1: TLB refill at 0xBFC00200, XTLB at 0xBFC00280
        // BEV=0: TLB refill at 0x80000000, XTLB at 0x80000080
        uint16_t bevBase = STATUS_REGISTER.BEV ? 0x0200 : 0x0000;
        switch (STATUS_REGISTER.PrevMode) {
        case PrivilegeMode_Kernel:
            m_System.m_JumpToLocation |= STATUS_REGISTER.KernelEXAddr == 0 ? bevBase : (bevBase + 0x0080);
            break;
        case PrivilegeMode_Supervisor:
            m_System.m_JumpToLocation |= STATUS_REGISTER.SuperVisorEXAddr == 0 ? bevBase : (bevBase + 0x0080);
            break;
        case PrivilegeMode_User:
            m_System.m_JumpToLocation |= STATUS_REGISTER.UserEXAddr == 0 ? bevBase : (bevBase + 0x0080);
            break;
        }
    }
}

void CRegisters::SetTestTimer() { m_System.m_TestTimer = true; }

int32_t CRegisters::GetLinkAddress(int32_t fallback) const {
    if (m_System.m_PipelineStage == N64Types::PipelineStage::Jump) {
        return static_cast<int32_t>(m_System.m_JumpToLocation + 4);
    }
    return fallback;
}

void CRegisters::InvalidateRecompCode(uint32_t physAddr, uint32_t length) {
    m_System.NotifyDMAToRDRAM(physAddr, length);
}

void CRegisters::TriggerException(uint32_t ExceptionCode, uint32_t Coprocessor) {
    CAUSE_REGISTER.ExceptionCode = ExceptionCode;
    CAUSE_REGISTER.CoprocessorUnitNumber = Coprocessor;

    if (ExceptionCode != 0) {
        Log::infof("GUEST CPU EXCEPTION: Code=%d PC=0x%08X (EXL=%d)", ExceptionCode,
                   static_cast<uint32_t>(m_PROGRAM_COUNTER), STATUS_REGISTER.ExceptionLevel);
    }

    // Per VR4300 spec: only update EPC, BD, and EXL when EXL is currently 0.
    // When EXL is already 1 (nested exception), preserve the original return
    // address so ERET can still return to the correct location.
    if (STATUS_REGISTER.ExceptionLevel == 0) {
        CAUSE_REGISTER.BranchDelay = (m_System.m_PipelineStage == N64Types::PipelineStage::Jump ||
                                      m_System.m_PipelineStage == N64Types::PipelineStage::PermanentLoopDelayDone);

        EPC_REGISTER =
            static_cast<int64_t>(static_cast<int32_t>(m_PROGRAM_COUNTER) - (CAUSE_REGISTER.BranchDelay ? 4 : 0));
        STATUS_REGISTER.ExceptionLevel = 1;
    }

    m_System.m_PipelineStage = N64Types::PipelineStage::Jump;

    // Per MIPS spec: LLBit is cleared by any exception
    m_LLBit = 0;

    // Exception vector depends on BEV (Bootstrap Exception Vector) bit in STATUS
    if (STATUS_REGISTER.BEV) {
        m_System.m_JumpToLocation = 0xFFFFFFFFBFC00380;
    } else {
        m_System.m_JumpToLocation = 0xFFFFFFFF80000180;
    }
}

// ============================================================================
// FPU Location Fixing
// ============================================================================

void CRegisters::FixFpuLocations() {
    for (uint8_t i = 0; i < 32; i++) {
        if (STATUS_REGISTER.FR == 0) {
            // 32-bit FPU mode - pairs of registers share 64-bit storage
            m_FPR_Union[i].u32 = static_cast<uint32_t>(m_FPR[i & ~1] >> (32 * (i & 1)));
        } else {
            // 64-bit FPU mode - each register is independent
            m_FPR_Union[i].u64 = m_FPR[i];
        }
    }
}
