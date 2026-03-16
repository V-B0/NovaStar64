#include "AudioInterfaceHandler.hpp"
#include "../MainSYS.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Systiming.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../../common/log.hpp"
#include <cmath>
#include <cstring>
#include <string>

// MI Register Definitions
#define MI_INTR_REG_INDEX 2
#define MI_INTR_AI_BIT    0x04

AudioInterfaceHandler::AudioInterfaceHandler(CN64System& system, CRegisters& registers)
    : m_system(system),
      m_sys_regs(registers),
      m_system_timer(const_cast<N64System::SystemTimer&>(*system.GetSystemTimer())) // Const cast because referenced
{
    m_registers.fill(0);
}

AudioInterfaceHandler::~AudioInterfaceHandler()
{
    m_audioOutput.Shutdown();
}

bool AudioInterfaceHandler::Read32(uint32_t address, uint32_t& value) {
    uint32_t index = AI::OffsetToIndex(address & 0x1F);
    
    if (index < static_cast<uint32_t>(AI::Register::REGISTER_COUNT)) {
        if (index == static_cast<uint32_t>(AI::Register::STATUS)) {
            value = m_ai_regs.status();
        } else {
             value = m_registers[index];
        }
        return true;
    }
    
    return false;
}

bool AudioInterfaceHandler::Write32(uint32_t address, uint32_t value, uint32_t mask) {
    uint32_t index = AI::OffsetToIndex(address & 0x1F);
    
    switch (static_cast<AI::Register>(index)) {
        case AI::Register::DRAM_ADDR:
            m_ai_regs.dram_addr() = value & 0x00FFFFFF; // 24-bit address
            break;
            
        case AI::Register::LEN: {
            uint32_t len = value & 0x3FFFF; // 18-bit length (align 8?)
            // Note: Register stores the value, but we also use it to trigger
            m_ai_regs.len() = len;
            
            if (len > 0) {
                // Ensure audio output is initialised before first DMA.
                // Normally DACRATE is written before LEN, but some games
                // (or boot sequences) may write LEN first.
                if (!m_audioInitialized && m_ai_regs.dacrate() > 0) {
                    m_lastDacFreq = GetFrequency();
                    InitAudioOutput();
                }

                // Queue audio samples to WASAPI output
                QueueDmaSamples(m_ai_regs.dram_addr(), len);
                
                if (m_ai_regs.status() & AI::STATUS_DMA_BUSY) {
                    // FIFO Logic: Buffer the next DMA
                    if (m_pending_valid) {
                        Logging::log(Logging::Level::Warning, "AI: Register written while FIFO full - Overwriting pending DMA");
                    }
                    
                    m_pending_dram_addr = m_ai_regs.dram_addr();
                    m_pending_len = len;
                    m_pending_valid = true;
                    
                    m_ai_regs.status() |= AI::STATUS_FULL;
                } else {
                    // Start immediately
                    m_current_dma_len = len;
                    m_current_dma_addr = m_ai_regs.dram_addr();
                    
                    m_ai_regs.status() |= AI::STATUS_DMA_BUSY;
                    
                    // Calculate time
                    uint32_t cycles = CalculateCycles(len);
                    
                    // Schedule Timer
                    m_system_timer.set_timer(N64System::TimerType::AiTimerDMA, cycles, false);
                }
            }
            break;
        }
            
        case AI::Register::CONTROL:
            // Write to control usually clears interrupt if bit 0 is set
            if (value & 1) {
                // Determine logic: does it start DMA? 
                // Documentation: "DMA Enable" bit (Bit 0). 
                // Using standard: writing 1 clears interrupt? 
                // Check N64 docs: 
                // AI_CONTROL_REG:
                // Bit 0: DMA Enable (1=Start/Enable) - actually usually writes start DMA if length set
                // Legacy Docs say: Write any value clears AI Interrupt?
                
                // Correction:
                // AI_STATUS: Read (Busy/Full/Etc), Write (Clear Interrupt)
                // Wait, different docs say different things.
                // Standard: Write to Status clears Interrupt.
            }
            break;
            
        case AI::Register::STATUS:
            // Write to Status Register clears AI Interrupt
            m_sys_regs.m_Mips_Interface[MI_INTR_REG_INDEX] &= ~MI_INTR_AI_BIT; 
            m_sys_regs.CheckInterrupts();
            break;
            
        case AI::Register::DACRATE:
            m_ai_regs.dacrate() = value & 0x3FFF; // 14-bit
            // Frequency = VideoClock / (dacrate + 1)
            // Reinitialize audio output if frequency changed
            {
                uint32_t freq = GetFrequency();
                if (freq != m_lastDacFreq) {
                    m_lastDacFreq = freq;
                    // Reset failure flag — a new DACRATE deserves a fresh attempt.
                    // The previous failure may have been caused by a garbage
                    // DACRATE during early boot.
                    m_audioInitFailed = false;
                    InitAudioOutput();
                }
            }
            break;
            
        case AI::Register::BITRATE:
            m_ai_regs.bitrate() = value & 0xF; // 4-bit
            break;
            
        default:
            return false;
    }
    return true;
}

uint32_t AudioInterfaceHandler::CalculateCycles(uint32_t len) {
    // Audio DMA duration in CPU cycles.
    // Stereo 16-bit PCM = 4 bytes per stereo sample pair.
    // The DAC consumes one stereo sample every (dacRate + 1) VI-clock ticks,
    // which equals (dacRate + 1) CPU cycles (the CPU and DAC share the same
    // base clock on the N64).
    uint32_t dac_period = m_ai_regs.dacrate() + 1;
    uint32_t samples = len / 4;  // stereo sample pairs
    return static_cast<uint32_t>(
        static_cast<uint64_t>(samples) * dac_period);
}

uint32_t AudioInterfaceHandler::GetFrequency() const {
    uint32_t dac = m_ai_regs.dacrate();
    if (dac == 0) return 44100; // Safety
    return AI_BASE_FREQ / (dac + 1);
}

void AudioInterfaceHandler::DMAComplete() {
    // Feed any pending audio to the device
    if (m_audioInitialized)
    {
        m_audioOutput.FeedDevice();
    }

    // Current DMA finished, trigger interrupt
    m_sys_regs.m_Mips_Interface[MI_INTR_REG_INDEX] |= MI_INTR_AI_BIT;
    m_sys_regs.CheckInterrupts();
    
    // Check if we have pending DMA in FIFO
    if (m_pending_valid) {
        // Promote pending to current
        m_current_dma_addr = m_pending_dram_addr;
        m_current_dma_len = m_pending_len;
        m_pending_valid = false;
        
        m_ai_regs.status() &= ~AI::STATUS_FULL;
        // Status remains BUSY
        
        // Start Timer for next DMA
        uint32_t cycles = CalculateCycles(m_current_dma_len);
        m_system_timer.set_timer(N64System::TimerType::AiTimerDMA, cycles, false);
    } else {
        // No pending DMA, go idle
        m_ai_regs.status() &= ~AI::STATUS_DMA_BUSY;
        m_ai_regs.status() &= ~AI::STATUS_FULL;
    }
}

void AudioInterfaceHandler::InitAudioOutput()
{
    // Don't retry after a failure — avoids busy-loop on broken audio devices
    if (m_audioInitFailed) return;

    // Shutdown previous instance if reinitializing
    if (m_audioInitialized)
    {
        m_audioOutput.Shutdown();
        m_audioInitialized = false;
    }

    uint32_t freq = GetFrequency();
    // Clamp to reasonable range
    if (freq < 4000) freq = 4000;
    if (freq > 48000) freq = 48000;

    Logging::log(Logging::Level::Info, "AI: Initializing WASAPI audio output at " + std::to_string(freq) + " Hz");

    if (m_audioOutput.Initialize(freq, 2))
    {
        m_audioOutput.Start();
        m_audioInitialized = true;
    }
    else
    {
        Logging::log(Logging::Level::Error, "AI: Failed to initialize WASAPI audio output (will not retry)");
        m_audioInitialized = false;
        m_audioInitFailed = true;
    }
}

void AudioInterfaceHandler::QueueDmaSamples(uint32_t dramAddr, uint32_t len)
{
    // Only attempt init once per DACRATE change (handled in Write32 DACRATE).
    // Don't retry on every DMA write — WASAPI init is expensive and may block.
    if (!m_audioInitialized) return;

    // Get RDRAM pointer
    auto* mem = m_system.GetMemory();
    if (!mem) return;

    uint8_t* rdram = mem->Rdram();
    if (!rdram) return;

    // Validate address range (RDRAM is typically 4MB or 8MB)
    uint32_t rdramSize = mem->RdramSize();
    dramAddr &= 0x00FFFFFF;
    if (dramAddr + len > rdramSize) return;

    // Audio data in RDRAM is big-endian interleaved 16-bit PCM (L,R,L,R,...)
    // WASAPI expects little-endian, so we need to byte-swap
    const uint8_t* src = rdram + dramAddr;
    uint32_t sampleCount = len / 2; // 16-bit samples

    // Temporary buffer for byte-swapped samples
    std::vector<int16_t> samples(sampleCount);
    for (uint32_t i = 0; i < sampleCount; i++)
    {
        // Big-endian to little-endian
        samples[i] = static_cast<int16_t>((src[i * 2] << 8) | src[i * 2 + 1]);
    }

    m_audioOutput.QueueSamples(samples.data(), sampleCount);
    m_audioOutput.FeedDevice();
}
