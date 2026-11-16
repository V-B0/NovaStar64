/**
 * @file SPregistersHandler.hpp
 * @brief Signal Processor (RSP) register handler for N64 emulation
 * 
 * Implements the RSP's register interface including:
 * - DMA transfers between RDRAM and DMEM/IMEM
 * - Status and control registers
 * - Interrupt handling
 * - Semaphore operations
 */

#pragma once
#include "MIPSYinterfacehandler.hpp"
#include "Memoryhandler.hpp"
#include <cstdint>
#include <cstring>
#include <algorithm>

class CRegisters;
namespace Nova64 {
class MipsMemoryVM;
class StateArchive;
} // namespace Nova64

// SP register addresses (physical)
namespace SPRegAddr {
constexpr uint32_t SP_MEM_ADDR = 0x04040000;
constexpr uint32_t SP_DRAM_ADDR = 0x04040004;
constexpr uint32_t SP_RD_LEN = 0x04040008;
constexpr uint32_t SP_WR_LEN = 0x0404000C;
constexpr uint32_t SP_STATUS = 0x04040010;
constexpr uint32_t SP_DMA_FULL = 0x04040014;
constexpr uint32_t SP_DMA_BUSY = 0x04040018;
constexpr uint32_t SP_SEMAPHORE = 0x0404001C;
constexpr uint32_t SP_PC = 0x04080000;
constexpr uint32_t SP_IBIST = 0x04080004;
} // namespace SPRegAddr

// SP Status Register Flags
namespace SPStatus {
constexpr uint32_t HALT = 0x0001;
constexpr uint32_t BROKE = 0x0002;
constexpr uint32_t DMA_BUSY = 0x0004;
constexpr uint32_t DMA_FULL = 0x0008;
constexpr uint32_t IO_FULL = 0x0010;
constexpr uint32_t SSTEP = 0x0020;
constexpr uint32_t INTR_BREAK = 0x0040;
constexpr uint32_t SIG0 = 0x0080;
constexpr uint32_t SIG1 = 0x0100;
constexpr uint32_t SIG2 = 0x0200;
constexpr uint32_t SIG3 = 0x0400;
constexpr uint32_t SIG4 = 0x0800;
constexpr uint32_t SIG5 = 0x1000;
constexpr uint32_t SIG6 = 0x2000;
constexpr uint32_t SIG7 = 0x4000;
} // namespace SPStatus

// SP Status Register Write Flags
namespace SPStatusWrite {
constexpr uint32_t CLR_HALT = 0x00001;
constexpr uint32_t SET_HALT = 0x00002;
constexpr uint32_t CLR_BROKE = 0x00004;
constexpr uint32_t CLR_INTR = 0x00008;
constexpr uint32_t SET_INTR = 0x00010;
constexpr uint32_t CLR_SSTEP = 0x00020;
constexpr uint32_t SET_SSTEP = 0x00040;
constexpr uint32_t CLR_INTR_BREAK = 0x00080;
constexpr uint32_t SET_INTR_BREAK = 0x00100;
constexpr uint32_t CLR_SIG0 = 0x00200;
constexpr uint32_t SET_SIG0 = 0x00400;
constexpr uint32_t CLR_SIG1 = 0x00800;
constexpr uint32_t SET_SIG1 = 0x01000;
constexpr uint32_t CLR_SIG2 = 0x02000;
constexpr uint32_t SET_SIG2 = 0x04000;
constexpr uint32_t CLR_SIG3 = 0x08000;
constexpr uint32_t SET_SIG3 = 0x10000;
constexpr uint32_t CLR_SIG4 = 0x20000;
constexpr uint32_t SET_SIG4 = 0x40000;
constexpr uint32_t CLR_SIG5 = 0x80000;
constexpr uint32_t SET_SIG5 = 0x100000;
constexpr uint32_t CLR_SIG6 = 0x200000;
constexpr uint32_t SET_SIG6 = 0x400000;
constexpr uint32_t CLR_SIG7 = 0x800000;
constexpr uint32_t SET_SIG7 = 0x1000000;
} // namespace SPStatusWrite

class SRegistersHandler;

class SPRegisters {
protected:
    SPRegisters(SRegistersHandler* SignalProcessorInterface);

public:
    uint32_t& SP_MEM_ADDR;
    uint32_t& SP_DRAM_ADDR;
    uint32_t& SP_RD_LEN;
    uint32_t& SP_WR_LEN;
    uint32_t& SP_STATUS;
    uint32_t& SP_DMA_FULL;
    uint32_t& SP_DMA_BUSY;
    uint32_t& SP_SEMAPHORE;
    uint32_t& SP_PC;
    uint32_t& SP_IBIST;

private:
    SPRegisters(const SPRegisters&);
    SPRegisters& operator=(const SPRegisters&);
};

class MIPSYInterface;

class SRegistersHandler : public MemoryHandler, private SPRegisters {
public:
    SRegistersHandler(CRegisters& reg) : SPRegisters(this), m_Reg(reg) {
        std::memset(m_IMEM, 0, sizeof(m_IMEM));
        std::memset(m_DMEM, 0, sizeof(m_DMEM));
        std::memset(m_SPRegs, 0, sizeof(m_SPRegs));
        // RSP starts halted — game writes CLR_HALT to start tasks
        m_SPRegs[4] = SPStatus::HALT;
        // Match P64: SP_RD_LEN and SP_WR_LEN reset to 0xFF8
        m_SPRegs[2] = 0x00000FF8; // SP_RD_LEN
        m_SPRegs[3] = 0x00000FF8; // SP_WR_LEN
        m_PendingSPMemAddr = 0;
        m_PendingSPDramAddr = 0;
        m_RDRAM = nullptr;
        m_RDRAMSize = 0;
    }

    bool Read32(uint32_t Address, uint32_t& Value) override;
    bool Write32(uint32_t Address, uint32_t Value, uint32_t Mask) override;

    uint8_t* Imem() { return &m_IMEM[0]; }
    uint8_t* Dmem() { return &m_DMEM[0]; }

    /// Save states and rewind (see StateArchive.hpp).
    void SerializeState(Nova64::StateArchive& ar);

    void SetRDRAM(uint8_t* rdram, uint32_t size) {
        m_RDRAM = rdram;
        m_RDRAMSize = size;
    }

private:
    SRegistersHandler(const SRegistersHandler&) = delete;
    SRegistersHandler& operator=(const SRegistersHandler&) = delete;

    static void stSystemReset(SRegistersHandler* _this) { _this->SystemReset(); }

    // Hardware emulation methods
    void SP_DMA_READ();  // DMA from RDRAM to DMEM/IMEM
    void SP_DMA_WRITE(); // DMA from DMEM/IMEM to RDRAM
    void ProcessStatusWrite(uint32_t Value);

    void ClearSPInterrupt();
    void SetSPInterrupt();
    void SetHalt();
    void ClearHalt();
    void SystemReset();

    // RSP memory
    uint8_t m_IMEM[0x1000]; // 4KB instruction memory
    uint8_t m_DMEM[0x1000]; // 4KB data memory

    // Register storage
public:
    uint32_t m_SPRegs[10]; // SP registers storage

    // Pending DMA addresses (latched on register write, used by DMA)
    uint32_t m_PendingSPMemAddr;
    uint32_t m_PendingSPDramAddr;

    // RDRAM access (provided by system)
private:
    CRegisters& m_Reg;
    uint8_t* m_RDRAM;
    uint32_t m_RDRAMSize;
};
