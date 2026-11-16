/**
 * @file SPregistersHandler.cpp
 * @brief Signal Processor (RSP) register handler implementation
 * 
 * Implements hardware-level emulation of the N64's RSP registers including:
 * - DMA operations between RDRAM and RSP memory (DMEM/IMEM)
 * - Status register control (halt, break, interrupts, signals)
 * - Semaphore operations
 */
#include "SPregistersHandler.hpp"
#include "../StateArchive.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Systiming.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../../NovaRSP/RSPInfo.hpp"
#include <algorithm>
#include <cstring>
#include <cstdio>

// MI Interrupt bits (matching MIPSYInterfaceHandler convention)
namespace {
constexpr uint32_t MI_INTR_SP = 0x01;
constexpr uint32_t MI_INTR_DP = 0x20;
} // namespace

SPRegisters::SPRegisters(SRegistersHandler* SignalProcessorInterface) :
    SP_MEM_ADDR(reinterpret_cast<uint32_t&>(SignalProcessorInterface->m_SPRegs[0])),
    SP_DRAM_ADDR(reinterpret_cast<uint32_t&>(SignalProcessorInterface->m_SPRegs[1])),
    SP_RD_LEN(reinterpret_cast<uint32_t&>(SignalProcessorInterface->m_SPRegs[2])),
    SP_WR_LEN(reinterpret_cast<uint32_t&>(SignalProcessorInterface->m_SPRegs[3])),
    SP_STATUS(reinterpret_cast<uint32_t&>(SignalProcessorInterface->m_SPRegs[4])),
    SP_DMA_FULL(reinterpret_cast<uint32_t&>(SignalProcessorInterface->m_SPRegs[5])),
    SP_DMA_BUSY(reinterpret_cast<uint32_t&>(SignalProcessorInterface->m_SPRegs[6])),
    SP_SEMAPHORE(reinterpret_cast<uint32_t&>(SignalProcessorInterface->m_SPRegs[7])),
    SP_PC(reinterpret_cast<uint32_t&>(SignalProcessorInterface->m_SPRegs[8])),
    SP_IBIST(reinterpret_cast<uint32_t&>(SignalProcessorInterface->m_SPRegs[9])) {}

bool SRegistersHandler::Read32(uint32_t Address, uint32_t& Value) {
    // Handle DMEM/IMEM reads (0x04000000-0x04001FFF)
    // Matching P64: CPU can directly read RSP memory
    if (Address < 0x04040000) {
        if ((Address & 0x1000) == 0) {
            Value = *reinterpret_cast<uint32_t*>(&m_DMEM[Address & 0xFFC]);
        } else {
            Value = *reinterpret_cast<uint32_t*>(&m_IMEM[Address & 0xFFC]);
        }
        return true;
    }

    switch (Address) {
    case SPRegAddr::SP_MEM_ADDR: Value = SP_MEM_ADDR & 0x1FFF; break;

    case SPRegAddr::SP_DRAM_ADDR: Value = SP_DRAM_ADDR & 0xFFFFFF; break;

    case SPRegAddr::SP_RD_LEN: Value = SP_RD_LEN; break;

    case SPRegAddr::SP_WR_LEN: Value = SP_WR_LEN; break;

    case SPRegAddr::SP_STATUS: Value = SP_STATUS; break;

    case SPRegAddr::SP_DMA_FULL: Value = (SP_STATUS & SPStatus::DMA_FULL) ? 1 : 0; break;

    case SPRegAddr::SP_DMA_BUSY: Value = (SP_STATUS & SPStatus::DMA_BUSY) ? 1 : 0; break;

    case SPRegAddr::SP_SEMAPHORE:
        // Reading semaphore acquires it (returns 0) or fails (returns 1)
        Value = SP_SEMAPHORE;
        SP_SEMAPHORE = 1; // Lock the semaphore
        break;

    case SPRegAddr::SP_PC: Value = SP_PC & 0xFFF; break;

    case SPRegAddr::SP_IBIST: Value = SP_IBIST; break;

    default: Value = 0; return false;
    }

    return true;
}

bool SRegistersHandler::Write32(uint32_t Address, uint32_t Value, uint32_t Mask) {
    uint32_t MaskedValue = Value & Mask;

    // Handle DMEM/IMEM writes (0x04000000-0x04001FFF)
    // Matching P64: CPU can directly write RSP memory
    if (Address < 0x04040000) {
        if ((Address & 0x1000) == 0) {
            *reinterpret_cast<uint32_t*>(&m_DMEM[Address & 0xFFC]) = MaskedValue;
        } else {
            *reinterpret_cast<uint32_t*>(&m_IMEM[Address & 0xFFC]) = MaskedValue;
        }
        return true;
    }

    switch (Address) {
    case SPRegAddr::SP_MEM_ADDR:
        SP_MEM_ADDR = MaskedValue & 0x1FFF;
        m_PendingSPMemAddr = MaskedValue; // Latch full value for DMA (P64 pattern)
        break;

    case SPRegAddr::SP_DRAM_ADDR:
        SP_DRAM_ADDR = MaskedValue & 0xFFFFFF;
        m_PendingSPDramAddr = MaskedValue; // Latch full value for DMA (P64 pattern)
        break;

    case SPRegAddr::SP_RD_LEN:
        // Write triggers DMA read (RDRAM -> DMEM/IMEM)
        SP_RD_LEN = MaskedValue;
        SP_DMA_READ();
        break;

    case SPRegAddr::SP_WR_LEN:
        // Write triggers DMA write (DMEM/IMEM -> RDRAM)
        SP_WR_LEN = MaskedValue;
        SP_DMA_WRITE();
        break;

    case SPRegAddr::SP_STATUS: ProcessStatusWrite(MaskedValue); break;

    case SPRegAddr::SP_SEMAPHORE:
        // Writing any value releases the semaphore (P64 pattern)
        SP_SEMAPHORE = 0;
        break;

    case SPRegAddr::SP_PC:
        SP_PC = MaskedValue & 0xFFC; // P64: 4-byte aligned, 12-bit
        break;

    default: return false;
    }

    return true;
}

/**
 * @brief DMA Read: Transfer data from RDRAM to RSP memory (DMEM or IMEM)
 * 
 * Based on N64 hardware behavior:
 * - Length = ((SP_RD_LEN & 0xFFF) | 7) + 1  (rounds UP to 8-byte boundary, min 8)
 * - Count = ((SP_RD_LEN >> 12) & 0xFF) + 1
 * - Skip = (SP_RD_LEN >> 20) & 0xFFF
 */
void SRegistersHandler::SP_DMA_READ() {
    if (!m_RDRAM) return;

    SP_STATUS |= SPStatus::DMA_BUSY;

    // Matching P64: Use pending addresses with proper alignment
    uint8_t* Dest = ((m_PendingSPMemAddr & 0x1000) != 0) ? m_IMEM : m_DMEM;
    uint8_t* Source = m_RDRAM;
    uint32_t ReadPos = m_PendingSPDramAddr & 0x00FFFFF8; // 8-byte aligned RDRAM addr
    int32_t Length = ((SP_RD_LEN & 0xFFF) | 7) + 1;      // Round up to 8 bytes
    int32_t Count = ((SP_RD_LEN >> 12) & 0xFF) + 1;
    int32_t Skip = (SP_RD_LEN >> 20) & 0xFF8;            // 8-byte aligned skip
    int32_t Pos = (m_PendingSPMemAddr & 0x0FF8);         // 8-byte aligned SP mem offset

    for (int32_t i = 0; i < Count; i++) {
        int32_t CopyLength = Length;

        // Handle wrap-around at 4KB DMEM/IMEM boundary (matching P64)
        if ((Pos + Length) > 0x1000) {
            CopyLength = 0x1000 - Pos;
            if (CopyLength <= 0) break;
        }

        // Bounds check RDRAM
        uint32_t NullLen = 0;
        if ((ReadPos + Length) > m_RDRAMSize) {
            if ((m_RDRAMSize - ReadPos) < (uint32_t)CopyLength) {
                CopyLength = (int32_t)m_RDRAMSize - (int32_t)ReadPos;
            } else {
                NullLen = CopyLength;
            }
        }

        if (NullLen != 0) {
            std::memset(&Dest[Pos], 0, NullLen);
        } else {
            std::memcpy(&Dest[Pos], &Source[ReadPos], CopyLength);
        }

        // Handle wrap: if CopyLength < Length, continue from SP mem offset 0
        if (CopyLength != Length) {
            ReadPos = (ReadPos + CopyLength) & 0x00FFFFFF;
            CopyLength = Length - CopyLength;
            Pos = 0;

            if ((CopyLength + ReadPos) > m_RDRAMSize) {
                int32_t CopyAmount = m_RDRAMSize - ReadPos;
                if (CopyAmount < 0) CopyAmount = 0;
                NullLen = CopyLength - CopyAmount;
                if (CopyAmount > 0) std::memcpy(&Dest[Pos], &Source[ReadPos], CopyAmount);
                if (NullLen > 0) std::memset(&Dest[Pos + CopyAmount], 0, NullLen);
            } else {
                std::memcpy(&Dest[Pos], &Source[ReadPos], CopyLength);
            }
        }

        ReadPos += CopyLength + Skip;
        Pos += CopyLength;
    }

    if (Count > 1) {
        ReadPos -= Skip;
    }

    // Update registers (matching P64)
    SP_DMA_BUSY = 0;
    SP_STATUS &= ~SPStatus::DMA_BUSY;

    SP_MEM_ADDR = (Pos & 0xFFF) | (m_PendingSPMemAddr & 0x1000);
    SP_DRAM_ADDR = ReadPos;
    SP_RD_LEN = (SP_RD_LEN & 0xFF800000) | 0x00000FF8; // P64: length field → 0xFF8
    SP_WR_LEN = (SP_WR_LEN & 0xFF800000) | 0x00000FF8;
}

/**
 * @brief DMA Write: Transfer data from RSP memory (DMEM or IMEM) to RDRAM
 * 
 * Based on N64 hardware behavior:
 * - Length = ((SP_WR_LEN & 0xFFF) | 7) + 1  (rounds UP to 8-byte boundary, min 8)
 * - Count = ((SP_WR_LEN >> 12) & 0xFF) + 1
 * - Skip = (SP_WR_LEN >> 20) & 0xFFF
 */
void SRegistersHandler::SP_DMA_WRITE() {
    if (!m_RDRAM) return;

    SP_STATUS |= SPStatus::DMA_BUSY;

    // Matching P64: Use pending addresses with proper alignment
    uint8_t* Source = ((m_PendingSPMemAddr & 0x1000) != 0) ? m_IMEM : m_DMEM;
    uint8_t* Dest = m_RDRAM;
    uint32_t WritePos = m_PendingSPDramAddr & 0x00FFFFF8; // 8-byte aligned RDRAM addr
    int32_t Length = ((SP_WR_LEN & 0xFFF) | 7) + 1;       // Round up to 8 bytes
    int32_t Count = ((SP_WR_LEN >> 12) & 0xFF) + 1;
    int32_t Skip = (SP_WR_LEN >> 20) & 0xFF8;             // 8-byte aligned skip
    int32_t Pos = (m_PendingSPMemAddr & 0x0FF8);          // 8-byte aligned SP mem offset

    for (int32_t i = 0; i < Count; i++) {
        int32_t CopyLength = Length;

        // Handle wrap-around at 4KB DMEM/IMEM boundary (matching P64)
        if (Pos + Length > 0x1000) {
            CopyLength = 0x1000 - Pos;
            if (CopyLength <= 0) break;
        }

        // Bounds check RDRAM
        if (WritePos < m_RDRAMSize) {
            int32_t CopyAmount = (int32_t)m_RDRAMSize - (int32_t)WritePos;
            if (CopyLength < CopyAmount) {
                CopyAmount = CopyLength;
            }
            if (CopyAmount > 0) {
                std::memcpy(&Dest[WritePos], &Source[Pos], CopyAmount);
            }
        }

        // Handle wrap: if CopyLength < Length, continue from SP mem offset 0
        if (CopyLength != Length) {
            WritePos = (WritePos + CopyLength) & 0x00FFFFFF;
            CopyLength = Length - CopyLength;
            Pos = 0;

            int32_t CopyAmount = (int32_t)m_RDRAMSize - (int32_t)WritePos;
            if (CopyLength < CopyAmount) {
                CopyAmount = CopyLength;
            }
            if (CopyAmount > 0) {
                std::memcpy(&Dest[WritePos], &Source[Pos], CopyAmount);
            }
        }

        WritePos += CopyLength + Skip;
        Pos += CopyLength;
    }

    if (Count > 1) {
        WritePos -= Skip;
    }

    // Update registers (matching P64)
    SP_DMA_BUSY = 0;
    SP_STATUS &= ~SPStatus::DMA_BUSY;

    SP_MEM_ADDR = (Pos & 0xFFF) | (m_PendingSPMemAddr & 0x1000);
    SP_DRAM_ADDR = WritePos;
    SP_RD_LEN = (SP_RD_LEN & 0xFF800000) | 0x00000FF8; // P64: length field → 0xFF8
    SP_WR_LEN = (SP_WR_LEN & 0xFF800000) | 0x00000FF8;

    // Invalidate any compiled code that overlaps the RDRAM target range.
    // Without this, SP DMA writes to code regions (e.g., N64 OS overlay loading
    // via DMEM→RDRAM) would leave stale compiled blocks in the recompiler cache.
    uint32_t dramStart = m_PendingSPDramAddr & 0x00FFFFF8;
    uint32_t totalLen = static_cast<uint32_t>(Length * Count + (Count > 1 ? Skip * (Count - 1) : 0));
    m_Reg.InvalidateRecompCode(dramStart, totalLen);
    Nova64::WatchDMA(dramStart, totalLen, "SP");
}

/**
 * @brief Process writes to SP_STATUS register
 * @param Value Value written to status register
 * 
 * The status register uses a bit-pair system where writing specific
 * bits can either set or clear various status flags.
 * 
 * All flag changes must be applied to SP_STATUS before ClearHalt() runs. On real hardware the
 * write is atomic: the RSP does not start until the full register update completes. If ClearHalt()
 * ran before SET_INTR_BREAK was applied, rsp_break() would not see IntrBreak and would not raise
 * the SP interrupt.
 */
void SRegistersHandler::ProcessStatusWrite(uint32_t Value) {
    bool shouldClearHalt = false;
    bool shouldSetHalt = false;

    // Phase 1: apply all SET/CLR flags to SP_STATUS atomically.

    // Halt control (defer actual halt/unhalt until after all flags applied)
    // P64: CLR only if SET is not also set, and vice versa
    if ((Value & SPStatusWrite::CLR_HALT) && !(Value & SPStatusWrite::SET_HALT)) {
        const bool wasHalted = (SP_STATUS & SPStatus::HALT) != 0;
        if (wasHalted) {
            Log::debugf("RSP: Starting execution from PC 0x%03X", SP_PC & 0xFFF);
        }
        SP_STATUS &= ~SPStatus::HALT;
        shouldClearHalt = wasHalted;
    }
    if ((Value & SPStatusWrite::SET_HALT) && !(Value & SPStatusWrite::CLR_HALT) &&
        !(SP_STATUS & SPStatus::HALT)) // P64: only set if not already halted
    {
        SP_STATUS |= SPStatus::HALT;
        shouldSetHalt = true;
    }

    // Broke flag control
    if (Value & SPStatusWrite::CLR_BROKE) {
        SP_STATUS &= ~SPStatus::BROKE;
    }

    // Interrupt control (P64: CLR/SET conflict → neither applies)
    if ((Value & SPStatusWrite::CLR_INTR) && !(Value & SPStatusWrite::SET_INTR)) {
        ClearSPInterrupt();
    }
    if ((Value & SPStatusWrite::SET_INTR) && !(Value & SPStatusWrite::CLR_INTR)) {
        SetSPInterrupt();
    }

    // Single-step control
    if ((Value & SPStatusWrite::CLR_SSTEP) && !(Value & SPStatusWrite::SET_SSTEP)) {
        SP_STATUS &= ~SPStatus::SSTEP;
    }
    if ((Value & SPStatusWrite::SET_SSTEP) && !(Value & SPStatusWrite::CLR_SSTEP)) {
        SP_STATUS |= SPStatus::SSTEP;
    }

    // Interrupt on break control
    if ((Value & SPStatusWrite::CLR_INTR_BREAK) && !(Value & SPStatusWrite::SET_INTR_BREAK)) {
        SP_STATUS &= ~SPStatus::INTR_BREAK;
    }
    if ((Value & SPStatusWrite::SET_INTR_BREAK) && !(Value & SPStatusWrite::CLR_INTR_BREAK)) {
        SP_STATUS |= SPStatus::INTR_BREAK;
    }

    if ((Value & SPStatusWrite::CLR_SIG0) && !(Value & SPStatusWrite::SET_SIG0)) {
        SP_STATUS &= ~SPStatus::SIG0;
    }
    if ((Value & SPStatusWrite::SET_SIG0) && !(Value & SPStatusWrite::CLR_SIG0)) {
        SP_STATUS |= SPStatus::SIG0;
    }

    if ((Value & SPStatusWrite::CLR_SIG1) && !(Value & SPStatusWrite::SET_SIG1)) {
        SP_STATUS &= ~SPStatus::SIG1;
    }
    if ((Value & SPStatusWrite::SET_SIG1) && !(Value & SPStatusWrite::CLR_SIG1)) {
        SP_STATUS |= SPStatus::SIG1;
    }

    if ((Value & SPStatusWrite::CLR_SIG2) && !(Value & SPStatusWrite::SET_SIG2)) {
        SP_STATUS &= ~SPStatus::SIG2;
    }
    if ((Value & SPStatusWrite::SET_SIG2) && !(Value & SPStatusWrite::CLR_SIG2)) {
        SP_STATUS |= SPStatus::SIG2;
    }

    if ((Value & SPStatusWrite::CLR_SIG3) && !(Value & SPStatusWrite::SET_SIG3)) {
        SP_STATUS &= ~SPStatus::SIG3;
    }
    if ((Value & SPStatusWrite::SET_SIG3) && !(Value & SPStatusWrite::CLR_SIG3)) {
        SP_STATUS |= SPStatus::SIG3;
    }

    if ((Value & SPStatusWrite::CLR_SIG4) && !(Value & SPStatusWrite::SET_SIG4)) {
        SP_STATUS &= ~SPStatus::SIG4;
    }
    if ((Value & SPStatusWrite::SET_SIG4) && !(Value & SPStatusWrite::CLR_SIG4)) {
        SP_STATUS |= SPStatus::SIG4;
    }

    if ((Value & SPStatusWrite::CLR_SIG5) && !(Value & SPStatusWrite::SET_SIG5)) {
        SP_STATUS &= ~SPStatus::SIG5;
    }
    if ((Value & SPStatusWrite::SET_SIG5) && !(Value & SPStatusWrite::CLR_SIG5)) {
        SP_STATUS |= SPStatus::SIG5;
    }

    if ((Value & SPStatusWrite::CLR_SIG6) && !(Value & SPStatusWrite::SET_SIG6)) {
        SP_STATUS &= ~SPStatus::SIG6;
    }
    if ((Value & SPStatusWrite::SET_SIG6) && !(Value & SPStatusWrite::CLR_SIG6)) {
        SP_STATUS |= SPStatus::SIG6;
    }

    if ((Value & SPStatusWrite::CLR_SIG7) && !(Value & SPStatusWrite::SET_SIG7)) {
        SP_STATUS &= ~SPStatus::SIG7;
    }
    if ((Value & SPStatusWrite::SET_SIG7) && !(Value & SPStatusWrite::CLR_SIG7)) {
        SP_STATUS |= SPStatus::SIG7;
    }

    // Phase 2: now that all flags are applied, trigger halt/unhalt.
    // This ensures rsp_break() sees the correct IntrBreak state.
    if (shouldSetHalt) {
        SetHalt();
    }
    if (shouldClearHalt) {
        ClearHalt();
    }
}

/**
 * @brief Clear RSP interrupt
 * 
 * Clears the SP interrupt bit in the MI_INTR register.
 */
void SRegistersHandler::ClearSPInterrupt() {
    m_Reg.m_RspIntrReg &= ~MI_INTR_SP;
    m_Reg.m_Mips_Interface[2] &= ~MI_INTR_SP;
    m_Reg.CheckInterrupts();
}

/**
 * @brief Set RSP interrupt
 * 
 * Sets the SP interrupt bit in the MI_INTR register.
 * This triggers a CPU interrupt if the SP interrupt is unmasked.
 */
void SRegistersHandler::SetSPInterrupt() {
    m_Reg.m_RspIntrReg |= MI_INTR_SP;
    m_Reg.m_Mips_Interface[2] |= MI_INTR_SP;
    m_Reg.CheckInterrupts();
}

/**
 * @brief Halt the RSP
 * 
 * Called when the RSP is halted. Nothing needs to stop: the RSP only runs synchronously inside
 * ClearHalt().
 */
void SRegistersHandler::SetHalt() {
    // Nothing to do: RSP execution is synchronous inside ClearHalt().
}

/**
 * @brief Resume RSP execution
 * 
 * Called when the RSP halt is cleared. Triggers RSP execution
 * via HLE dispatch (or LLE fallback).
 */
void SRegistersHandler::ClearHalt() {
    // Guard against recursive ClearHalt (e.g., if HLE processing triggers another CLR_HALT)
    static bool s_inClearHalt = false;
    if (s_inClearHalt) {
        return;
    }
    s_inClearHalt = true;

    // Set defer flag so that RSP HLE code (ProcessDList, rsp_break) does not
    // set MI_INTR bits or call CheckInterrupts synchronously. Instead they
    // accumulate bits in m_deferred_mi_intr for the RspTimer to apply later.
    m_Reg.m_defer_hle_interrupts = true;
    m_Reg.m_deferred_mi_intr = 0;

    // Execute RSP until it BREAKs or halts itself
    Nova64::RSP::DoRspCycles(0xFFFFFFFF);

    m_Reg.m_defer_hle_interrupts = false;

    // ── P64-style immediate interrupt delivery ──
    // Set MI_INTR bits NOW (not deferred). Then schedule a minimal timer
    // that only calls CheckInterrupts. This matches Project64's approach:
    // bits are set immediately, exception fires at next instruction boundary.
    uint32_t pendingBits = m_Reg.m_deferred_mi_intr;

    // If RSP broke with IntrBreak, add SP interrupt bit
    if ((SP_STATUS & SPStatus::INTR_BREAK) != 0 && (SP_STATUS & SPStatus::BROKE) != 0) {
        pendingBits |= MI_INTR_SP; // 0x01
    }

    // Apply SP interrupt bit immediately (fires at next instruction boundary)
    if (pendingBits & MI_INTR_SP) {
        m_Reg.m_RspIntrReg |= MI_INTR_SP;
        m_Reg.m_Mips_Interface[2] |= MI_INTR_SP;
    }

    // DP interrupt: set immediately AND via RspTimer. The game's cleanup code
    // (MI_MODE CLR_DP_INTR) will clear the DP bit within the same block.
    // The RspTimer fires at the block boundary (after cleanup has run) and
    // re-sets the DP bit, ensuring the interrupt is properly delivered.
    bool hasDP = (pendingBits & 0x20) != 0;
    m_Reg.m_deferred_mi_intr = 0;

    if (hasDP) {
        m_Reg.m_GfxIntrReg |= MI_INTR_DP;
        m_Reg.m_Mips_Interface[2] |= MI_INTR_DP;
        m_Reg.m_DpFreshFromHLE = true; // Tell RspTimer handler to restore DP
        // SyncFull drains the RDP pipeline — clear activity flags
        m_Reg.m_Display_ControlReg[3] &= ~(0x020u | 0x008u); // PIPE_BUSY | START_GCLK
    }

    // Schedule SP+DP interrupt delivery via RspTimer (fires at block boundary)
    if (pendingBits & (MI_INTR_SP | MI_INTR_DP)) {
        static constexpr uint32_t MIN_DELAY = 2; // 1 instruction at CountPerOp=2
        m_Reg.GetSystemTimer().set_timer(N64System::TimerType::RspTimer, MIN_DELAY, false);

        // Force the recompiler's slow path at the next block boundary so that
        // TimerDone() fires the RspTimer promptly.  Without this, forward-only
        // code paths (function returns, forward branches) can skip TimerDone
        // indefinitely, delaying SP/DP interrupt delivery by hundreds of
        // instructions — enough to stall OoT's graphics scheduler during
        // scene transitions.
        m_Reg.SetTestTimer();
    }

    s_inClearHalt = false;
}

void SRegistersHandler::SystemReset() {
    std::memset(m_SPRegs, 0, sizeof(m_SPRegs));

    std::memset(m_IMEM, 0, sizeof(m_IMEM));
    std::memset(m_DMEM, 0, sizeof(m_DMEM));

    SP_STATUS = SPStatus::HALT;

    // P64: SP_RD_LEN and SP_WR_LEN reset to 0xFF8
    SP_RD_LEN = 0x00000FF8;
    SP_WR_LEN = 0x00000FF8;

    m_PendingSPMemAddr = 0;
    m_PendingSPDramAddr = 0;
}

void SRegistersHandler::SerializeState(Nova64::StateArchive& ar) {
    ar.Section("SP  ");
    ar.Value(m_IMEM);
    ar.Value(m_DMEM);
    ar.Value(m_SPRegs);
    ar.Value(m_PendingSPMemAddr);
    ar.Value(m_PendingSPDramAddr);
}
