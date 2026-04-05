/**
 * @file AudioInterfaceHandler.hpp
 * @brief Audio Interface Handler declarations and types.
 */
#pragma once

#include "Memoryhandler.hpp"
#include "../../common/log.hpp"
#include "../../common/Span.hpp"
#include "../../NovaRSP/HLE/AudioOutput.hpp"
#include <cstdint>
#include <array>
#include <memory>

// Forward declarations
class CN64System;
class CRegisters;
namespace N64System { class SystemTimer; }

namespace AI {
    // Register indices (sequential, for array indexing)
    // Mapped from byte offsets: 0x00, 0x04, 0x08, 0x0C, 0x10, 0x14
    enum class Register : uint32_t {
        DRAM_ADDR = 0,
        LEN = 1,
        CONTROL = 2,
        STATUS = 3,
        DACRATE = 4,
        BITRATE = 5,
        REGISTER_COUNT = 6
    };
    
    // Convert byte offset (address & 0x1F) to register index
    inline uint32_t OffsetToIndex(uint32_t byteOffset) { return (byteOffset & 0x1C) >> 2; }
    
    // Status bits
    constexpr uint32_t STATUS_DMA_BUSY  = 0x40000000; // Bit 30
    constexpr uint32_t STATUS_FULL      = 0x80000000; // Bit 31
    constexpr uint32_t STATUS_DMA_ENABLE    = 0x01; // Not a real status bit, but used in control? NO.
    
    // Control bits (Write)
    constexpr uint32_t CONTROL_DMA_ENABLE   = 0x01;
    constexpr uint32_t CONTROL_CLEAR_INTR   = 0x00; // Any write clears interrupt generally? No, specific bits usually.
}

// Register wrapper
class AudioInterfaceReg {
public:
    explicit AudioInterfaceReg(Common::Span<uint32_t, static_cast<size_t>(AI::Register::REGISTER_COUNT)> regs) 
        : m_regs(regs) {}
        
    uint32_t& dram_addr() { return m_regs[static_cast<size_t>(AI::Register::DRAM_ADDR)]; }
    uint32_t& len() { return m_regs[static_cast<size_t>(AI::Register::LEN)]; }
    uint32_t& control() { return m_regs[static_cast<size_t>(AI::Register::CONTROL)]; }
    uint32_t& status() { return m_regs[static_cast<size_t>(AI::Register::STATUS)]; }
    uint32_t& dacrate() { return m_regs[static_cast<size_t>(AI::Register::DACRATE)]; }
    uint32_t& bitrate() { return m_regs[static_cast<size_t>(AI::Register::BITRATE)]; }
    
    uint32_t dram_addr() const { return m_regs[static_cast<size_t>(AI::Register::DRAM_ADDR)]; }
    uint32_t len() const { return m_regs[static_cast<size_t>(AI::Register::LEN)]; }
    uint32_t control() const { return m_regs[static_cast<size_t>(AI::Register::CONTROL)]; }
    uint32_t status() const { return m_regs[static_cast<size_t>(AI::Register::STATUS)]; }
    uint32_t dacrate() const { return m_regs[static_cast<size_t>(AI::Register::DACRATE)]; }
    uint32_t bitrate() const { return m_regs[static_cast<size_t>(AI::Register::BITRATE)]; }

private:
   Common::Span<uint32_t, static_cast<size_t>(AI::Register::REGISTER_COUNT)> m_regs;
};

class AudioInterfaceHandler final : public MemoryHandler {
public:
    AudioInterfaceHandler(CN64System& system, CRegisters& registers);
    ~AudioInterfaceHandler() override;

    // MemoryHandler interface
    bool Read32(uint32_t address, uint32_t& value) override;
    bool Write32(uint32_t address, uint32_t value, uint32_t mask) override;
    
    // Audio processing
    void UpdateAudio();
    uint32_t CalculateCycles(uint32_t len);
    
    // helper to get frequency
    uint32_t GetFrequency() const;
    
    // WASAPI audio output
    void InitAudioOutput();
    void QueueDmaSamples(uint32_t dramAddr, uint32_t len);
    Nova64::RSP::HLE::CAudioOutput& AudioOutput() { return m_audioOutput; }
    
    // Called by timer system when AI DMA completes
    void DMAComplete();

private:
    CN64System& m_system;
    CRegisters& m_sys_regs;
    N64System::SystemTimer& m_system_timer;
    
    std::array<uint32_t, static_cast<size_t>(AI::Register::REGISTER_COUNT)> m_registers;
    AudioInterfaceReg m_ai_regs{m_registers};
    
    // State
    uint32_t m_current_dma_len = 0;
    uint32_t m_current_dma_addr = 0;
    
    // Double buffering (FIFO)
    uint32_t m_pending_dram_addr = 0;
    uint32_t m_pending_len = 0;
    bool m_pending_valid = false;
    
    static constexpr uint32_t AI_BASE_FREQ = 48681812; // NTSC DAC Clock
    
    // WASAPI audio output
    Nova64::RSP::HLE::CAudioOutput m_audioOutput;
    bool m_audioInitialized{false};
    bool m_audioInitFailed{false};   // Set after first WASAPI failure — stops retries
    uint32_t m_lastDacFreq{0};
};
