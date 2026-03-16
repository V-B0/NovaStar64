/**
 * @file AudioPlugin.hpp
 * @brief Audio plugin interface for N64 audio emulation
 * 
 * Handles all audio-related functionality:
 * - Audio DAC output
 * - Audio buffer management
 * - Sample rate conversion
 * - Audio synchronization
 */

#pragma once

#include "PluginBase.hpp"
#include <cstdint>

namespace Plugins {

/**
 * @struct AudioSettings
 * @brief Audio configuration
 */
struct AudioSettings {
    uint32_t Frequency;             // Sample rate (Hz)
    uint32_t BitDepth;              // Bits per sample (8, 16, 32)
    uint32_t Channels;              // Number of channels (1=mono, 2=stereo)
    uint32_t BufferSize;            // Audio buffer size in samples
    bool SyncAudio;                 // Synchronize to audio
    uint32_t Volume;                // Master volume (0-100)
};

/**
 * @struct AudioInfo
 * @brief Audio interface information (passed to plugin)
 */
struct AudioInfo {
    void* hwnd;                     // Window handle
    uint32_t* RDRAM;                // Pointer to RDRAM
    uint32_t* DMEM;                 // Pointer to RSP DMEM
    uint32_t* IMEM;                 // Pointer to RSP IMEM
    uint32_t* MI_INTR_REG;          // MI interrupt register
    
    uint32_t* AI_DRAM_ADDR_REG;     // AI registers
    uint32_t* AI_LEN_REG;
    uint32_t* AI_CONTROL_REG;
    uint32_t* AI_STATUS_REG;
    uint32_t* AI_DACRATE_REG;
    uint32_t* AI_BITRATE_REG;
    
    void (*CheckInterrupts)();      // Interrupt check callback
};

/**
 * @class AudioPlugin
 * @brief Audio plugin interface
 */
class AudioPlugin : public PluginBase {
public:
    virtual ~AudioPlugin() = default;
    
    // ========================================================================
    // Audio Initialization
    // ========================================================================
    
    /**
     * @brief Initialize audio output
     * @param settings Audio settings
     * @return true on success
     */
    virtual bool InitializeAudio(const AudioSettings& settings) = 0;
    
    /**
     * @brief Set audio information structure
     * @param info Audio interface info
     */
    virtual void SetAudioInfo(const AudioInfo& info) = 0;
    
    // ========================================================================
    // Audio Interface (AI) Processing
    // ========================================================================
    
    /**
     * @brief Process AI DMA transfer
     * Called when AI_LEN_REG is written
     */
    virtual void ProcessAICommand() = 0;
    
    /**
     * @brief Get length of audio to play
     * @return Number of bytes to process
     */
    virtual uint32_t GetAILength() = 0;
    
    /**
     * @brief Update audio DAC rate
     * @param frequency New frequency in Hz
     */
    virtual void SetDACRate(uint32_t frequency) = 0;
    
    /**
     * @brief Update audio bit rate
     * @param bitrate New bit rate
     */
    virtual void SetBitRate(uint32_t bitrate) = 0;
    
    // ========================================================================
    // Audio Buffer Management
    // ========================================================================
    
    /**
     * @brief Add samples to audio buffer
     * @param buffer Sample data
     * @param length Number of samples
     */
    virtual void AddAudioSamples(const int16_t* buffer, uint32_t length) = 0;
    
    /**
     * @brief Check if audio is playing
     * @return true if audio is active
     */
    virtual bool IsPlaying() const = 0;
    
    /**
     * @brief Get number of samples in buffer
     * @return Buffer fill level
     */
    virtual uint32_t GetBufferLevel() const = 0;
    
    // ========================================================================
    // Synchronization
    // ========================================================================
    
    /**
     * @brief Wait for audio buffer to have space
     * Used for audio-based synchronization
     */
    virtual void WaitForBuffer() = 0;
    
    /**
     * @brief Set audio sync mode
     * @param sync true to enable audio sync
     */
    virtual void SetSyncMode(bool sync) = 0;
    
    // ========================================================================
    // Volume Control
    // ========================================================================
    
    /**
     * @brief Set master volume
     * @param volume Volume level (0-100)
     */
    virtual void SetVolume(uint32_t volume) = 0;
    
    /**
     * @brief Mute audio output
     * @param mute true to mute
     */
    virtual void SetMute(bool mute) = 0;
};

} // namespace Plugins
