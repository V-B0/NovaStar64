#pragma once
#include "Memoryhandler.hpp"
#include "SPregistersHandler.hpp"
#include <cstdint>
#include <array>
#include <span>

namespace DPCRegisters {
    // Use enum classes for type safety
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
    constexpr bool isCommandSet(uint32_t value, DPCCommand cmd) {
        return (value & static_cast<uint32_t>(cmd)) != 0;
    }
    
    constexpr bool isStatusSet(uint32_t value, DPCStatus status) {
        return (value & static_cast<uint32_t>(status)) != 0;
    }
    
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
}