/**
 * @file DisplayCtrlReg.hpp
 * @brief RDP command (DPC) register handler: DPC command and register-index enums and DisplayProcessorHandler.
 */
#pragma once
#include "Memoryhandler.hpp"
#include <cstdint>
#include <array>
#include <span>

class CRegisters;
namespace Nova64 {
class StateArchive;
}
namespace Plugins {
class GFXPlugin;
}

namespace DPCRegisters {
enum class DPCCommand : uint32_t {
    CLR_XBUS_DMEM_DMA = 0x0001,
    SET_XBUS_DMEM_DMA = 0x0002,
    CLR_FREEZE = 0x0004,
    SET_FREEZE = 0x0008,
    CLR_FLUSH = 0x0010,
    SET_FLUSH = 0x0020,
    CLR_TMEM_CTR = 0x0040,
    CLR_PIPE_CTR = 0x0080,
    CLR_CMD_CTR = 0x0100,
    CLR_CLOCK_CTR = 0x0200
};

enum class DPCStatus : uint32_t {
    XBUS_DMEM_DMA = 0x001,
    FREEZE = 0x002,
    FLUSH = 0x004,
    START_GCLK = 0x008,
    TMEM_BUSY = 0x010,
    PIPE_BUSY = 0x020,
    CMD_BUSY = 0x040,
    CBUF_READY = 0x080,
    DMA_BUSY = 0x100,
    END_VALID = 0x200,
    START_VALID = 0x400
};

// Helper functions for bit manipulation
constexpr bool isCommandSet(uint32_t value, DPCCommand cmd) { return (value & static_cast<uint32_t>(cmd)) != 0; }

constexpr bool isStatusSet(uint32_t value, DPCStatus status) { return (value & static_cast<uint32_t>(status)) != 0; }

// Register indices for array access
enum RegisterIndex : size_t {
    DPC_START = 0,
    DPC_END,
    DPC_CURRENT,
    DPC_STATUS,
    DPC_CLOCK,
    DPC_BUFBUSY,
    DPC_PIPEBUSY,
    DPC_TMEM,
    REGISTER_COUNT
};
} // namespace DPCRegisters

class DisplayProcessorHandler : public MemoryHandler {
public:
    DisplayProcessorHandler(CRegisters& reg, Plugins::GFXPlugin* gfxPlugin = nullptr);
    virtual ~DisplayProcessorHandler() = default;

    // MemoryHandler interface
    bool Read32(uint32_t addr, uint32_t& val) override;
    bool Write32(uint32_t addr, uint32_t val, uint32_t mask) override;

    // Plugin Management
    void SetGraphicsPlugin(Plugins::GFXPlugin* plugin);
    Plugins::GFXPlugin* GetGraphicsPlugin() const;

    uint32_t GetStatus() const;

    /// Get a mutable pointer to a specific DPC register (for plugin wiring)
    [[nodiscard]] uint32_t* GetRegisterPtr(DPCRegisters::RegisterIndex index) noexcept { return &m_Registers[index]; }

    void Reset();

    /// Save states and rewind (see StateArchive.hpp).
    void SerializeState(Nova64::StateArchive& ar);

    // Set DMEM pointer for XBUS mode
    void SetDMEM(uint8_t* dmem) { m_DMEM = dmem; }

private:
    void ProcessStatusCommand(uint32_t cmd);
    void ProcessDisplayList();

    CRegisters& m_Reg;
    Plugins::GFXPlugin* m_GfxPlugin;
    uint32_t* m_Registers;
    uint8_t* m_DMEM = nullptr;

    bool m_ProcessingDList;
    bool m_pendingPipelineClear = false;
    uint32_t m_DListStart;
    uint32_t m_DListEnd;
};
