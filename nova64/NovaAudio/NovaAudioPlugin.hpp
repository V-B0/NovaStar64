/**
 * @file NovaAudioPlugin.hpp
 * @brief Adapter bridging SuperNova64's AudioPlugin interface to the AI hardware emulation
 *
 * This class implements the Plugins::AudioPlugin abstract interface required by
 * the emulator core, and delegates to the AudioInterfaceHandler for DMA/output.
 * Modeled after NovaGraphicsGFXPlugin.
 */

#pragma once

#include "../N64SYS/Plugins/AudioPlugin.hpp"

#include <cstdint>
#include <memory>
#include <vector>

// Forward declarations
class CN64System;
class AudioInterfaceHandler;

namespace Nova64::Audio {

class NovaAudioPlugin final : public ::Plugins::AudioPlugin {
public:
    NovaAudioPlugin();
    ~NovaAudioPlugin() override;

    // ════════════════════════════════════════════════════════════
    //  PluginBase interface
    // ════════════════════════════════════════════════════════════

    ::Plugins::PluginInfo GetPluginInfo() const override;
    bool Initialize(CN64System* system) override;
    void Shutdown() override;
    void Reset(bool coldReset) override;
    void Pause() override;
    void Resume() override;
    bool IsInitialized() const override;
    void ShowConfigDialog(void* hwndParent) override;
    size_t SaveState(uint8_t* data, size_t size) override;
    bool LoadState(const uint8_t* data, size_t size) override;

    // ════════════════════════════════════════════════════════════
    //  AudioPlugin interface
    // ════════════════════════════════════════════════════════════

    bool InitializeAudio(const ::Plugins::AudioSettings& settings) override;
    void SetAudioInfo(const ::Plugins::AudioInfo& info) override;

    // AI Processing
    void ProcessAICommand() override;
    uint32_t GetAILength() override;
    void SetDACRate(uint32_t frequency) override;
    void SetBitRate(uint32_t bitrate) override;

    // Buffer Management
    void AddAudioSamples(const int16_t* buffer, uint32_t length) override;
    bool IsPlaying() const override;
    uint32_t GetBufferLevel() const override;

    // Synchronization
    void WaitForBuffer() override;
    void SetSyncMode(bool sync) override;

    // Volume Control
    void SetVolume(uint32_t volume) override;
    void SetMute(bool mute) override;

private:
    bool m_initialized = false;
    bool m_muted = false;
    uint32_t m_volume = 100;
    bool m_syncAudio = false;

    // Cached system pointers (valid after Initialize)
    CN64System* m_n64System = nullptr;
    AudioInterfaceHandler* m_aiHandler = nullptr;

    // Audio info from SetAudioInfo
    ::Plugins::AudioInfo m_audioInfo{};
    bool m_hasAudioInfo = false;

    // Current audio state
    uint32_t m_dacRate = 0;
    uint32_t m_bitRate = 0;
    uint32_t m_frequency = 44100;
};

/// Factory function — called by PluginManager::CreateDefaultAudioPlugin
std::unique_ptr<::Plugins::AudioPlugin> CreateNovaAudioPlugin();

} // namespace Nova64::Audio
