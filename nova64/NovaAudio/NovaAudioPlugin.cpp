/**
 * @file NovaAudioPlugin.cpp
 * @brief Adapter bridging AudioPlugin → AudioInterfaceHandler (AI DMA + WASAPI)
 *
 * NovaAudio is a built-in audio plugin that implements the Plugins::AudioPlugin
 * interface by delegating to the emulator's AudioInterfaceHandler for hardware-
 * level AI register / DMA emulation and WASAPI audio output.
 */

#include "NovaAudioPlugin.hpp"
#include "../N64SYS/MainSYS.hpp"
#include "../N64SYS/MIPSY/Vmem.hpp"
#include "../N64SYS/Memoryhandler/AudioInterfaceHandler.hpp"
#include "../common/log.hpp"

#include <cstring>
#include <algorithm>

namespace Nova64::Audio {

// ═══════════════════════════════════════════════════════════════════════
//  Construction
// ═══════════════════════════════════════════════════════════════════════

NovaAudioPlugin::NovaAudioPlugin()
{
    std::memset(&m_audioInfo, 0, sizeof(m_audioInfo));
}

NovaAudioPlugin::~NovaAudioPlugin()
{
    if (m_initialized)
        Shutdown();
}

// ═══════════════════════════════════════════════════════════════════════
//  PluginBase interface
// ═══════════════════════════════════════════════════════════════════════

::Plugins::PluginInfo NovaAudioPlugin::GetPluginInfo() const
{
    ::Plugins::PluginInfo info{};
    info.Type                = ::Plugins::PluginType::Audio;
    info.Version             = 0x0100; // 1.0
    info.Name                = "NovaAudio";
    info.Author              = "SuperNova64 Team";
    info.Description         = "Built-in audio plugin with WASAPI output";
    info.SupportsThreading   = false;
    info.SupportsStateSync   = false;
    info.RequiresAudioSync   = false;
    info.SupportsConfigDialog = false;
    return info;
}

bool NovaAudioPlugin::Initialize(CN64System* system)
{
    m_System = system; // PluginBase member
    m_n64System = system;

    if (!system) {
        Log::error("NovaAudioPlugin: Cannot initialize without system");
        return false;
    }

    // Grab AudioInterfaceHandler from the memory subsystem
    if (auto* mem = system->GetMemory()) {
        m_aiHandler = mem->AudioInterface();
    }

    if (!m_aiHandler) {
        Log::warning("NovaAudioPlugin: No AudioInterfaceHandler found - audio output unavailable");
        // Not fatal — the plugin can still stub out calls
    }

    m_initialized = true;
    Log::info("NovaAudioPlugin: Initialized successfully");
    return true;
}

void NovaAudioPlugin::Shutdown()
{
    Log::info("NovaAudioPlugin: Shutting down");
    m_aiHandler = nullptr;
    m_n64System = nullptr;
    m_hasAudioInfo = false;
    m_initialized = false;
}

void NovaAudioPlugin::Reset(bool /*coldReset*/)
{
    m_dacRate = 0;
    m_bitRate = 0;
    m_frequency = 44100;
}

void NovaAudioPlugin::Pause()
{
    // No-op: WASAPI output is driven by DMA completion timers
}

void NovaAudioPlugin::Resume()
{
    // No-op
}

bool NovaAudioPlugin::IsInitialized() const { return m_initialized; }

void NovaAudioPlugin::ShowConfigDialog(void* /*hwndParent*/)
{
    // No config dialog yet
}

size_t NovaAudioPlugin::SaveState(uint8_t* data, size_t size)
{
    // Persist the plugin's audio configuration so save states can
    // restore DAC rate, bitrate and frequency on reload.
    struct AudioState {
        uint32_t dacRate;
        uint32_t bitRate;
        uint32_t frequency;
        uint32_t volume;
        uint8_t  muted;
        uint8_t  syncAudio;
    };

    constexpr size_t needed = sizeof(AudioState);
    if (!data) return needed;          // query required size
    if (size < needed) return 0;       // buffer too small

    AudioState st{};
    st.dacRate   = m_dacRate;
    st.bitRate   = m_bitRate;
    st.frequency = m_frequency;
    st.volume    = m_volume;
    st.muted     = m_muted ? 1 : 0;
    st.syncAudio = m_syncAudio ? 1 : 0;

    std::memcpy(data, &st, sizeof(st));
    return needed;
}

bool NovaAudioPlugin::LoadState(const uint8_t* data, size_t size)
{
    struct AudioState {
        uint32_t dacRate;
        uint32_t bitRate;
        uint32_t frequency;
        uint32_t volume;
        uint8_t  muted;
        uint8_t  syncAudio;
    };

    constexpr size_t needed = sizeof(AudioState);
    if (!data || size < needed) return false;

    AudioState st{};
    std::memcpy(&st, data, sizeof(st));

    m_dacRate   = st.dacRate;
    m_bitRate   = st.bitRate;
    m_frequency = st.frequency;
    m_volume    = st.volume;
    m_muted     = st.muted != 0;
    m_syncAudio = st.syncAudio != 0;

    // Re-apply DAC rate to the hardware handler so audio output frequency matches
    if (m_aiHandler && m_dacRate != 0) {
        SetDACRate(m_dacRate);
    }

    Log::infof("NovaAudioPlugin: LoadState freq=%u dac=%u br=%u",
               m_frequency, m_dacRate, m_bitRate);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  AudioPlugin interface
// ═══════════════════════════════════════════════════════════════════════

bool NovaAudioPlugin::InitializeAudio(const ::Plugins::AudioSettings& settings)
{
    m_frequency  = settings.Frequency;
    m_volume     = settings.Volume;
    m_syncAudio  = settings.SyncAudio;

    Log::infof("NovaAudioPlugin: InitializeAudio freq=%u vol=%u sync=%s",
               m_frequency, m_volume, m_syncAudio ? "true" : "false");

    // The AudioInterfaceHandler manages WASAPI directly — nothing extra to do here.
    return true;
}

void NovaAudioPlugin::SetAudioInfo(const ::Plugins::AudioInfo& info)
{
    m_audioInfo = info;
    m_hasAudioInfo = true;
}

void NovaAudioPlugin::ProcessAICommand()
{
    // Called when the game writes AI_LEN_REG to start a DMA transfer.
    // The AudioInterfaceHandler already handles the DMA scheduling and
    // WASAPI output in Write32(), so this is a notification hook.
    //
    // If we have direct register pointers from SetAudioInfo, we could
    // read AI_DRAM_ADDR and AI_LEN here. But since AIH handles it,
    // we only need to ensure the audio device is fed.

    // FeedDevice is called from the frame loop (MainSYS) — not here,
    // to let audio accumulate in the ring buffer.
}

uint32_t NovaAudioPlugin::GetAILength()
{
    // Return the number of bytes remaining in the current AI DMA.
    // Reading AI_LEN_REG gives the remaining count.
    if (!m_aiHandler) return 0;

    uint32_t value = 0;
    // AI_LEN is at offset 0x04 → physical address 0x04500004
    m_aiHandler->Read32(0x04500004, value);
    return value;
}

void NovaAudioPlugin::SetDACRate(uint32_t dacRate)
{
    m_dacRate = dacRate;

    // Calculate frequency: freq = NTSC_CLOCK / (dacRate + 1)
    static constexpr uint32_t AI_NTSC_CLOCK = 48681812;
    if (dacRate > 0) {
        m_frequency = AI_NTSC_CLOCK / (dacRate + 1);
    }

    Log::infof("NovaAudioPlugin: SetDACRate %u → freq=%u Hz", dacRate, m_frequency);

    // AudioInterfaceHandler reconfigures WASAPI in its Write32(DACRATE) handler,
    // so we don't need to duplicate that here.
}

void NovaAudioPlugin::SetBitRate(uint32_t bitrate)
{
    m_bitRate = bitrate;
}

void NovaAudioPlugin::AddAudioSamples(const int16_t* buffer, uint32_t length)
{
    // Forward raw PCM samples to the AudioInterfaceHandler's WASAPI output
    if (!m_aiHandler || !buffer || length == 0) return;

    m_aiHandler->AudioOutput().QueueSamples(buffer, length);
    // FeedDevice is called from the frame loop (MainSYS) — not here.
}

bool NovaAudioPlugin::IsPlaying() const
{
    // Audio is "playing" if the AI DMA is busy
    if (!m_aiHandler) return false;

    uint32_t status = 0;
    const_cast<AudioInterfaceHandler*>(m_aiHandler)->Read32(0x0450000C, status);
    return (status & AI::STATUS_DMA_BUSY) != 0;
}

uint32_t NovaAudioPlugin::GetBufferLevel() const
{
    // Return approximate number of samples queued in WASAPI buffer
    // For now return 0 — the audio output doesn't expose buffer level yet
    return 0;
}

void NovaAudioPlugin::WaitForBuffer()
{
    // In sync mode, we could block until the audio buffer drains.
    // For now, non-blocking — the AI timer pacing handles sync naturally.
}

void NovaAudioPlugin::SetSyncMode(bool sync)
{
    m_syncAudio = sync;
}

void NovaAudioPlugin::SetVolume(uint32_t volume)
{
    m_volume = std::min(volume, 100u);
}

void NovaAudioPlugin::SetMute(bool mute)
{
    m_muted = mute;
}

// ═══════════════════════════════════════════════════════════════════════
//  Factory
// ═══════════════════════════════════════════════════════════════════════

std::unique_ptr<::Plugins::AudioPlugin> CreateNovaAudioPlugin()
{
    return std::make_unique<NovaAudioPlugin>();
}

} // namespace Nova64::Audio
