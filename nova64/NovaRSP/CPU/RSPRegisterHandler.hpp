// SuperNova64 - N64 Emulator
// RSP Register Handler - Modernized from Project64's RSPRegisterHandler.h
// C++20 abstract DMA/register interface with proper length encoding

#pragma once
#include "RSPRegisters.hpp"
#include <cstdint>

namespace Nova64::RSP {

// ──────────────────────────────────────────────
// RSP Hardware Register indices (COP0 mapping)
// ──────────────────────────────────────────────
enum class RSPRegister : uint32_t {
    MemAddr   = 0,   // SP_MEM_ADDR_REG
    DramAddr  = 1,   // SP_DRAM_ADDR_REG
    RdLen     = 2,   // SP_RD_LEN_REG  (trigger DMA read)
    WrLen     = 3,   // SP_WR_LEN_REG  (trigger DMA write)
    Status    = 4,   // SP_STATUS_REG
    Semaphore = 5,   // SP_SEMAPHORE_REG
    PC        = 6,   // SP_PC_REG (mapped at 0x04080000)
};

// ──────────────────────────────────────────────
// DMA Length Register encoding
// Bits [0:11]  = Length (actual = value + 1)
// Bits [12:19] = Count  (actual = value + 1)
// Bits [20:31] = Skip   (bytes to skip between rows)
// ──────────────────────────────────────────────
#pragma warning(push)
#pragma warning(disable : 4201)
union DMALengthReg {
    uint32_t Value;
    struct {
        unsigned Length : 12;
        unsigned Count  : 8;
        unsigned Skip   : 12;
    };
    
    constexpr DMALengthReg() : Value(0) {}
    constexpr explicit DMALengthReg(uint32_t v) : Value(v) {}

    // Decoded accessors — N64 hardware rounds DMA length UP to next 8-byte boundary.
    // Real hardware ignores bits 0-2 so the formula is: (field | 7) + 1
    // giving a minimum transfer of 8 bytes and a maximum of 4096 (0x1000).
    [[nodiscard]] constexpr uint32_t ActualLength() const { return (Length | 7u) + 1; }
    [[nodiscard]] constexpr uint32_t ActualCount()  const { return Count + 1; }
    [[nodiscard]] constexpr uint32_t ActualSkip()   const { return Skip; }
};
#pragma warning(pop)

// ──────────────────────────────────────────────
// Abstract RSP Register Handler
// Manages SP interface registers and DMA transfers
// ──────────────────────────────────────────────
class RSPRegisterHandler {
public:
    RSPRegisterHandler(
        uint32_t* signalProcessorInterface,
        uint8_t*& rdram,
        const uint32_t& rdramSize,
        uint8_t* imem,
        uint8_t* dmem
    );

    virtual ~RSPRegisterHandler() = default;

    // DMA operations
    void SP_DMA_READ();   // RDRAM → SP memory
    void SP_DMA_WRITE();  // SP memory → RDRAM

    // Register read/write interface
    [[nodiscard]] uint32_t ReadReg(RSPRegister reg) const;
    void WriteReg(RSPRegister reg, uint32_t value);

protected:
    // Pure virtual callbacks for system integration
    virtual void ClearSPInterrupt() = 0;
    virtual void SetSPInterrupt() = 0;
    virtual void SetHalt() = 0;
    virtual void DmaReadDone(uint32_t end) = 0;

    // SP interface registers (from hardware MMIO)
    uint32_t&   SP_MEM_ADDR_REG;
    uint32_t&   SP_DRAM_ADDR_REG;
    DMALengthReg& SP_RD_LEN_REG;
    DMALengthReg& SP_WR_LEN_REG;
    uint32_t&   SP_STATUS_REG;
    uint32_t&   SP_DMA_FULL_REG;
    uint32_t&   SP_DMA_BUSY_REG;
    uint32_t&   SP_SEMAPHORE_REG;
    uint32_t&   SP_PC_REG;

    // Memory pointers
    uint8_t*&       m_Rdram;
    const uint32_t& m_RdramSize;
    uint8_t*        m_IMEM;
    uint8_t*        m_DMEM;

    // Pending DMA addresses (double-buffered)
    uint32_t m_PendingSPMemAddr{0};
    uint32_t m_PendingSPDramAddr{0};
    bool     m_IgnoreWrites{false};
};

// ──────────────────────────────────────────────
// Concrete handler for plugin-style integration
// ──────────────────────────────────────────────
class CRSPSystem;

class RSPRegisterHandlerPlugin : public RSPRegisterHandler {
public:
    explicit RSPRegisterHandlerPlugin(CRSPSystem& system);

    [[nodiscard]] uint32_t& PendingSPMemAddr()  { return m_PendingSPMemAddr; }
    [[nodiscard]] uint32_t& PendingSPDramAddr() { return m_PendingSPDramAddr; }

private:
    void ClearSPInterrupt() override;
    void SetSPInterrupt() override;
    void SetHalt() override;
    void DmaReadDone(uint32_t end) override;

    CRSPSystem& m_System;
};

} // namespace Nova64::RSP
