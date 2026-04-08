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
#include <cmath>
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
    ++s_aiDmaCompleteCount;
    // Log first 10 and then every 200th DMAComplete
    if (s_aiDmaCompleteCount <= 10 || (s_aiDmaCompleteCount % 200 == 0)) {
        std::fprintf(stderr, "[AI] DMAComplete #%u: pending=%d wasapi=%d\n",
            s_aiDmaCompleteCount, m_pending_valid ? 1 : 0, m_audioInitialized ? 1 : 0);
    }

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

    // Audio data in RDRAM was written by RSP HLE alist_save via
    // dram_store_u16, which uses XOR_S16=2 byte addressing.
    // Read each int16_t sample using the same XOR to get correct values.
    uint32_t sampleCount = len / 2; // 16-bit samples
    std::vector<int16_t> samples(sampleCount);
    for (uint32_t i = 0; i < sampleCount; i++)
    {
        // XOR_S16: toggle bit 1 of the byte address to access
        // the correct halfword within each 32-bit RDRAM word
        uint32_t addr = (dramAddr + i * 2) ^ 2;
        samples[i] = *reinterpret_cast<const int16_t*>(rdram + addr);
    }

    m_audioOutput.QueueSamples(samples.data(), sampleCount);
    m_audioOutput.FeedDevice();

    // ── Diagnostic: dump audio to WAV file for analysis ──
    static uint32_t s_queueCount = 0;
    static uint32_t s_nonSilentCount = 0;
    static FILE* s_wavDump = nullptr;
    static uint32_t s_wavDataBytes = 0;
    ++s_queueCount;
    
    // Check if samples are non-silent
    bool hasAudio = false;
    for (uint32_t i = 0; i < sampleCount; i++) {
        if (samples[i] != 0) { hasAudio = true; break; }
    }
    
    // Dump first 600 non-silent transfers as a proper WAV file
    if (hasAudio) {
        ++s_nonSilentCount;
        if (s_nonSilentCount <= 600) {
            if (!s_wavDump) {
                s_wavDump = std::fopen("audio_dump.wav", "wb");
                if (s_wavDump) {
                    // Write placeholder WAV header (will update sizes at close)
                    uint32_t freq = GetFrequency();
                    uint16_t channels = 2;
                    uint16_t bitsPerSample = 16;
                    uint32_t byteRate = freq * channels * (bitsPerSample / 8);
                    uint16_t blockAlign = channels * (bitsPerSample / 8);
                    // RIFF header
                    std::fwrite("RIFF", 1, 4, s_wavDump);
                    uint32_t placeholder = 0;
                    std::fwrite(&placeholder, 4, 1, s_wavDump); // file size - 8 (placeholder)
                    std::fwrite("WAVE", 1, 4, s_wavDump);
                    // fmt chunk
                    std::fwrite("fmt ", 1, 4, s_wavDump);
                    uint32_t fmtSize = 16;
                    std::fwrite(&fmtSize, 4, 1, s_wavDump);
                    uint16_t audioFormat = 1; // PCM
                    std::fwrite(&audioFormat, 2, 1, s_wavDump);
                    std::fwrite(&channels, 2, 1, s_wavDump);
                    std::fwrite(&freq, 4, 1, s_wavDump);
                    std::fwrite(&byteRate, 4, 1, s_wavDump);
                    std::fwrite(&blockAlign, 2, 1, s_wavDump);
                    std::fwrite(&bitsPerSample, 2, 1, s_wavDump);
                    // data chunk
                    std::fwrite("data", 1, 4, s_wavDump);
                    std::fwrite(&placeholder, 4, 1, s_wavDump); // data size (placeholder)
                    s_wavDataBytes = 0;
                    std::fprintf(stderr, "[AI-DIAG] Dumping WAV to audio_dump.wav (%uHz stereo s16le)\n", freq);
                }
            }
            if (s_wavDump) {
                uint32_t bytes = sampleCount * sizeof(int16_t);
                std::fwrite(samples.data(), 1, bytes, s_wavDump);
                s_wavDataBytes += bytes;
                if (s_nonSilentCount == 600) {
                    // Update WAV header sizes
                    uint32_t riffSize = 36 + s_wavDataBytes;
                    std::fseek(s_wavDump, 4, SEEK_SET);
                    std::fwrite(&riffSize, 4, 1, s_wavDump);
                    std::fseek(s_wavDump, 40, SEEK_SET);
                    std::fwrite(&s_wavDataBytes, 4, 1, s_wavDump);
                    std::fflush(s_wavDump);
                    std::fclose(s_wavDump);
                    s_wavDump = nullptr;
                    std::fprintf(stderr, "[AI-DIAG] WAV dump complete: %u bytes audio data\n", s_wavDataBytes);
                }
            }
        }
    }
}
