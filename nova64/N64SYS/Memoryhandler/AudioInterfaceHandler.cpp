/**
 * @file AudioInterfaceHandler.cpp
 * @brief Audio Interface Handler implementation.
 */
#include "AudioInterfaceHandler.hpp"
#include "../MainSYS.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Systiming.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../../common/log.hpp"
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

// MI Register Definitions
#define MI_INTR_REG_INDEX 2
#define MI_INTR_AI_BIT    0x04

// ── Audio flow diagnostics ──
static uint32_t s_aiLenWriteCount = 0;
static uint32_t s_aiDmaCompleteCount = 0;
static uint32_t s_aiDacRateWriteCount = 0;
static uint32_t s_aiWasapiInitCount = 0;

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
                ++s_aiDacRateWriteCount;
                uint32_t freq = GetFrequency();
                if (s_aiDacRateWriteCount <= 5 || freq != m_lastDacFreq) {
                    std::fprintf(stderr, "[AI] DACRATE write #%u: raw=%u freq=%u Hz (prev=%u)\n",
                        s_aiDacRateWriteCount, value & 0x3FFF, freq, m_lastDacFreq);
                }
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
    // The DAC consumes one stereo sample every (dacRate + 1) video-clock ticks.
    // Video clock (NTSC) = 48,681,812 Hz.  CPU clock = 93,750,000 Hz.
    // Convert: cpu_cycles = video_ticks * CPU_FREQ / VI_FREQ.
    uint32_t dac_period = m_ai_regs.dacrate() + 1;
    uint32_t samples = len / 4;  // stereo sample pairs
    uint64_t video_ticks = static_cast<uint64_t>(samples) * dac_period;
    return static_cast<uint32_t>(video_ticks * 93750000ULL / AI_BASE_FREQ);
}

uint32_t AudioInterfaceHandler::GetFrequency() const {
    uint32_t dac = m_ai_regs.dacrate();
    if (dac == 0) return 44100; // Safety
    return AI_BASE_FREQ / (dac + 1);
}

void AudioInterfaceHandler::DMAComplete() {
    ++s_aiDmaCompleteCount;

    // Audio feeding is handled by the dedicated audio thread in CAudioOutput.

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
    ++s_aiWasapiInitCount;
    std::fprintf(stderr, "[AI] InitAudioOutput #%u: freq=%u Hz\n", s_aiWasapiInitCount, freq);

    if (m_audioOutput.Initialize(freq, 2))
    {
        m_audioOutput.SetSourceRate(freq);
        m_audioOutput.Start();
        m_audioInitialized = true;
        std::fprintf(stderr, "[AI] WASAPI initialized successfully (source=%u Hz)\n", freq);
    }
    else
    {
        std::fprintf(stderr, "[AI] WASAPI initialization FAILED\n");
        Logging::log(Logging::Level::Error, "AI: Failed to initialize WASAPI audio output (will not retry)");
        m_audioInitialized = false;
        m_audioInitFailed = true;
    }
}

void AudioInterfaceHandler::QueueDmaSamples(uint32_t dramAddr, uint32_t len)
{
    if (!m_audioInitialized) return;

    auto* mem = m_system.GetMemory();
    if (!mem) return;

    uint8_t* rdram = mem->Rdram();
    if (!rdram) return;

    uint32_t rdramSize = mem->RdramSize();
    dramAddr &= 0x00FFFFFF;
    if (dramAddr + len > rdramSize) return;

    uint32_t sampleCount = len / 2;
    std::vector<int16_t> samples(sampleCount);

    // Read 16-bit samples from RDRAM with XOR byte-swap (N64 LE word layout)
    for (uint32_t i = 0; i < sampleCount; i++)
    {
        uint32_t addr = (dramAddr + i * 2) ^ 2;
        samples[i] = *reinterpret_cast<const int16_t*>(rdram + addr);
    }

    m_audioOutput.QueueSamples(samples.data(), sampleCount, GetFrequency());
}
