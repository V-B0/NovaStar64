/**
 * @file AudioOutput.hpp
 * @brief RSP HLE Audio Output - SDL2 queue-based implementation.
 *
 * Minimal audio output: pushes raw N64 PCM samples to SDL2 via
 * SDL_QueueAudio.  No custom resampler, no ring buffer, no callback.
 * SDL2 handles all format conversion and buffering internally.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#pragma once
#include <cstdint>
#include <atomic>
#include <vector>

namespace Nova64::RSP::HLE {

class CAudioOutput {
public:
    CAudioOutput();
    ~CAudioOutput();

    CAudioOutput(const CAudioOutput&) = delete;
    CAudioOutput& operator=(const CAudioOutput&) = delete;

    bool Initialize(uint32_t sampleRate = 44100, uint32_t channels = 2);
    void Shutdown();

    void QueueSamples(const int16_t* samples, uint32_t sampleCount, uint32_t sourceRate = 0);

    void SetSourceRate(uint32_t rate) { m_sourceRate = rate; }

    /// Volume for all output, 0-100, and mute. Take effect immediately.
    static void SetMasterVolume(uint32_t percent) { s_volume.store(percent > 100 ? 100 : percent); }
    static void SetMuted(bool muted) { s_muted.store(muted); }

    void Start();
    void Stop();

    [[nodiscard]] bool IsInitialized() const { return m_initialized.load(); }
    [[nodiscard]] bool IsPlaying() const { return m_playing.load(); }
    [[nodiscard]] uint32_t SampleRate() const { return m_sampleRate; }

    void FeedDevice() {}

private:
    uint32_t m_sampleRate{44100};
    uint32_t m_sourceRate{32000};

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_playing{false};

    uint32_t m_sdlDeviceId{0};
    bool m_sdlAudioInit{false};
    std::vector<int16_t> m_scaled; ///< samples after volume, when it isn't 100%

    inline static std::atomic<uint32_t> s_volume{100};
    inline static std::atomic<bool> s_muted{false};
};

} // namespace Nova64::RSP::HLE
