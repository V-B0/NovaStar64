/**
 * @file RSPRegisterHandler.cpp
 * @brief RSP register handler: SP DMA transfers and hardware register access.
 */

#include "RSPRegisterHandler.hpp"
#include "RspMemory.hpp"
#include "RspSystem.hpp"
#include "../Jit/RspJit.hpp"
#include "RSPCpu.hpp"
#include "../../common/log.hpp"
#include <algorithm>
#include <cstring>

namespace Nova64::RSP {

// ──────────────────────────────────────────────
// Base RSP Register Handler
// ──────────────────────────────────────────────

namespace {

// An SP DMA row can be longer than what is left of the 4 KB memory bank it starts in,
// and its RDRAM side can run past the installed memory. The hardware wraps inside the
// bank, so the copies below do too, and anything outside RDRAM reads as zero and
// is not written. (Copying with a plain memcpy overran the bank and the RDRAM buffer.)
void CopyRdramToSp(uint8_t* bank, uint32_t spOffset, const uint8_t* rdram, uint32_t rdramSize,
                   uint32_t dramAddr, uint32_t length)
{
    while (length != 0)
    {
        const uint32_t run   = std::min(length, 0x1000u - spOffset);
        const uint32_t avail = dramAddr < rdramSize ? rdramSize - dramAddr : 0;
        const uint32_t n     = std::min(run, avail);
        if (n != 0)   std::memcpy(bank + spOffset, rdram + dramAddr, n);
        if (n < run)  std::memset(bank + spOffset + n, 0, run - n);
        spOffset = (spOffset + run) & 0x0FFF;
        dramAddr += run;
        length   -= run;
    }
}

void CopySpToRdram(uint8_t* rdram, uint32_t rdramSize, uint32_t dramAddr,
                   const uint8_t* bank, uint32_t spOffset, uint32_t length)
{
    while (length != 0)
    {
        const uint32_t run   = std::min(length, 0x1000u - spOffset);
        const uint32_t avail = dramAddr < rdramSize ? rdramSize - dramAddr : 0;
        const uint32_t n     = std::min(run, avail);
        if (n != 0)   std::memcpy(rdram + dramAddr, bank + spOffset, n);
        spOffset = (spOffset + run) & 0x0FFF;
        dramAddr += run;
        length   -= run;
    }
}

} // namespace

RSPRegisterHandler::RSPRegisterHandler(
    uint32_t* signalProcessorInterface,
    uint8_t*& rdram,
    const uint32_t& rdramSize,
    uint8_t* imem,
    uint8_t* dmem)
    : SP_MEM_ADDR_REG(signalProcessorInterface[0])
    , SP_DRAM_ADDR_REG(signalProcessorInterface[1])
    , SP_RD_LEN_REG(reinterpret_cast<DMALengthReg&>(signalProcessorInterface[2]))
    , SP_WR_LEN_REG(reinterpret_cast<DMALengthReg&>(signalProcessorInterface[3]))
    , SP_STATUS_REG(signalProcessorInterface[4])
    , SP_DMA_FULL_REG(signalProcessorInterface[5])
    , SP_DMA_BUSY_REG(signalProcessorInterface[6])
    , SP_SEMAPHORE_REG(signalProcessorInterface[7])
    , SP_PC_REG(signalProcessorInterface[8])
    , m_Rdram(rdram)
    , m_RdramSize(rdramSize)
    , m_IMEM(imem)
    , m_DMEM(dmem)
{
}

// ──────────────────────────────────────────────
// SP DMA Read: RDRAM → SP memory (IMEM/DMEM)
// Triggered by writing to SP_RD_LEN_REG
// ──────────────────────────────────────────────
void RSPRegisterHandler::SP_DMA_READ()
{
    const uint32_t spAddr   = SP_MEM_ADDR_REG & 0x1FF8;
    uint32_t dramAddr       = SP_DRAM_ADDR_REG & 0x00FFFFF8;
    const uint32_t length   = SP_RD_LEN_REG.ActualLength();
    const uint32_t count    = SP_RD_LEN_REG.ActualCount();
    const uint32_t skip     = SP_RD_LEN_REG.ActualSkip();

    // Select target: IMEM if bit 12 set, DMEM otherwise
    uint8_t* target = (SP_MEM_ADDR_REG & 0x1000) ? m_IMEM : m_DMEM;
    uint32_t spOffset = spAddr & 0x0FFF;

    for (uint32_t c = 0; c < count; c++)
    {
        CopyRdramToSp(target, spOffset, m_Rdram, m_RdramSize, dramAddr, length);

        spOffset   = (spOffset + length) & 0x0FFF;
        dramAddr   = (dramAddr + length + skip) & 0x00FFFFF8;
    }

    // Log RSP-initiated DMA reads

    // Update registers after DMA
    SP_MEM_ADDR_REG  = (SP_MEM_ADDR_REG & 0x1000) | spOffset;
    SP_DRAM_ADDR_REG = dramAddr;
    SP_RD_LEN_REG.Value = 0xFF8 | (SP_RD_LEN_REG.Value & 0xFF800000);

    DmaReadDone(dramAddr);
}

// ──────────────────────────────────────────────
// SP DMA Write: SP memory (IMEM/DMEM) → RDRAM
// Triggered by writing to SP_WR_LEN_REG
// ──────────────────────────────────────────────
void RSPRegisterHandler::SP_DMA_WRITE()
{
    const uint32_t spAddr   = SP_MEM_ADDR_REG & 0x1FF8;
    uint32_t dramAddr       = SP_DRAM_ADDR_REG & 0x00FFFFF8;
    const uint32_t length   = SP_WR_LEN_REG.ActualLength();
    const uint32_t count    = SP_WR_LEN_REG.ActualCount();
    const uint32_t skip     = SP_WR_LEN_REG.ActualSkip();

    const uint8_t* source = (SP_MEM_ADDR_REG & 0x1000) ? m_IMEM : m_DMEM;
    uint32_t spOffset = spAddr & 0x0FFF;

    for (uint32_t c = 0; c < count; c++)
    {
        CopySpToRdram(m_Rdram, m_RdramSize, dramAddr, source, spOffset, length);

        spOffset   = (spOffset + length) & 0x0FFF;
        dramAddr   = (dramAddr + length + skip) & 0x00FFFFF8;
    }


    SP_MEM_ADDR_REG  = (SP_MEM_ADDR_REG & 0x1000) | spOffset;
    SP_DRAM_ADDR_REG = dramAddr;
    SP_WR_LEN_REG.Value = 0xFF8 | (SP_WR_LEN_REG.Value & 0xFF800000);
}

// ──────────────────────────────────────────────
// Register read
// ──────────────────────────────────────────────
uint32_t RSPRegisterHandler::ReadReg(RSPRegister reg) const
{
    switch (reg)
    {
    case RSPRegister::MemAddr:   return SP_MEM_ADDR_REG;
    case RSPRegister::DramAddr:  return SP_DRAM_ADDR_REG;
    case RSPRegister::RdLen:     return SP_RD_LEN_REG.Value;
    case RSPRegister::WrLen:     return SP_WR_LEN_REG.Value;
    case RSPRegister::Status:    return SP_STATUS_REG;
    case RSPRegister::Semaphore:
    {
        // Reading semaphore returns current value and sets it to 1
        uint32_t value = SP_SEMAPHORE_REG;
        SP_SEMAPHORE_REG = 1;
        return value;
    }
    case RSPRegister::PC:        return SP_PC_REG;
    default:                     return 0;
    }
}

// ──────────────────────────────────────────────
// Register write with side effects
// ──────────────────────────────────────────────
void RSPRegisterHandler::WriteReg(RSPRegister reg, uint32_t value)
{
    if (m_IgnoreWrites) return;

    switch (reg)
    {
    case RSPRegister::MemAddr:
        SP_MEM_ADDR_REG = value;
        m_PendingSPMemAddr = value;
        break;
    case RSPRegister::DramAddr:
        SP_DRAM_ADDR_REG = value;
        m_PendingSPDramAddr = value;
        break;
    case RSPRegister::RdLen:
        SP_RD_LEN_REG.Value = value;
        SP_DMA_READ();
        break;
    case RSPRegister::WrLen:
        SP_WR_LEN_REG.Value = value;
        SP_DMA_WRITE();
        break;
    case RSPRegister::Status:
        // Status write is a set of clear/set pairs
        if (value & SPWrite::ClrHalt)       SP_STATUS_REG &= ~SPStatus::Halt;
        if (value & SPWrite::SetHalt)       { SP_STATUS_REG |= SPStatus::Halt; SetHalt(); }
        if (value & SPWrite::ClrBroke)      SP_STATUS_REG &= ~SPStatus::Broke;
        if (value & SPWrite::ClrIntr)       ClearSPInterrupt();
        if (value & SPWrite::SetIntr)       SetSPInterrupt();
        if (value & SPWrite::ClrSStep)      SP_STATUS_REG &= ~SPStatus::SingleStep;
        if (value & SPWrite::SetSStep)      SP_STATUS_REG |= SPStatus::SingleStep;
        if (value & SPWrite::ClrIntrBreak)  SP_STATUS_REG &= ~SPStatus::IntrBreak;
        if (value & SPWrite::SetIntrBreak)  SP_STATUS_REG |= SPStatus::IntrBreak;
        if (value & SPWrite::ClrSignal0)    SP_STATUS_REG &= ~SPStatus::Signal0;
        if (value & SPWrite::SetSignal0)    SP_STATUS_REG |= SPStatus::Signal0;
        if (value & SPWrite::ClrSignal1)    SP_STATUS_REG &= ~SPStatus::Signal1;
        if (value & SPWrite::SetSignal1)    SP_STATUS_REG |= SPStatus::Signal1;
        if (value & SPWrite::ClrSignal2)    SP_STATUS_REG &= ~SPStatus::Signal2;
        if (value & SPWrite::SetSignal2)    SP_STATUS_REG |= SPStatus::Signal2;
        if (value & SPWrite::ClrSignal3)    SP_STATUS_REG &= ~SPStatus::Signal3;
        if (value & SPWrite::SetSignal3)    SP_STATUS_REG |= SPStatus::Signal3;
        if (value & SPWrite::ClrSignal4)    SP_STATUS_REG &= ~SPStatus::Signal4;
        if (value & SPWrite::SetSignal4)    SP_STATUS_REG |= SPStatus::Signal4;
        if (value & SPWrite::ClrSignal5)    SP_STATUS_REG &= ~SPStatus::Signal5;
        if (value & SPWrite::SetSignal5)    SP_STATUS_REG |= SPStatus::Signal5;
        if (value & SPWrite::ClrSignal6)    SP_STATUS_REG &= ~SPStatus::Signal6;
        if (value & SPWrite::SetSignal6)    SP_STATUS_REG |= SPStatus::Signal6;
        if (value & SPWrite::ClrSignal7)    SP_STATUS_REG &= ~SPStatus::Signal7;
        if (value & SPWrite::SetSignal7)    SP_STATUS_REG |= SPStatus::Signal7;
        break;
    case RSPRegister::Semaphore:
        SP_SEMAPHORE_REG = 0;  // Writing any value clears semaphore
        break;
    case RSPRegister::PC:
        SP_PC_REG = value & 0xFFC;
        break;
    default:
        break;
    }
}

// ──────────────────────────────────────────────
// Plugin-style concrete handler
// ──────────────────────────────────────────────

RSPRegisterHandlerPlugin::RSPRegisterHandlerPlugin(CRSPSystem& system)
    : RSPRegisterHandler(
        system.m_SP_MEM_ADDR_REG,
        system.m_RDRAM,
        system.m_RdramSize,
        system.m_IMEM,
        system.m_DMEM)
    , m_System(system)
{
}

void RSPRegisterHandlerPlugin::ClearSPInterrupt()
{
    if (m_System.m_MI_INTR_REG != nullptr)
    {
        *m_System.m_MI_INTR_REG &= ~MI_INTR_SP;
    }
}

void RSPRegisterHandlerPlugin::SetSPInterrupt()
{
    if (m_System.m_MI_INTR_REG != nullptr)
    {
        *m_System.m_MI_INTR_REG |= MI_INTR_SP;
        if (m_System.CheckInterrupts != nullptr)
        {
            m_System.CheckInterrupts();
        }
    }
}

void RSPRegisterHandlerPlugin::SetHalt()
{
    // RSP halted - stop execution loop
    RSP_Running = 0;
}

void RSPRegisterHandlerPlugin::DmaReadDone(uint32_t /*end*/)
{
    // A DMA into IMEM rewrites code the recompiler may have compiled.
    if (m_System.m_Jit && m_System.m_SP_MEM_ADDR_REG && (*m_System.m_SP_MEM_ADDR_REG & 0x1000))
        m_System.m_Jit->NotifyImemWritten();
}

} // namespace Nova64::RSP
