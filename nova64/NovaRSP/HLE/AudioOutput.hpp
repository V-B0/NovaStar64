/**
 * @file AudioOutput.hpp
 * @brief RSP HLE Audio Output - SDL2 queue-based implementation.
 *
 * Minimal audio output: pushes raw N64 PCM samples to SDL2 via
 * SDL_QueueAudio.  No custom resampler, no ring buffer, no callback.
 * SDL2 handles all format conversion and buffering internally.
 */
// SuperNova64 - N64 Emulator

#pragma once
#include <cstdint>
#include <atomic>

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

    void Start();
    void Stop();

    [[nodiscard]] bool IsInitialized() const { return m_initialized.load(); }
    [[nodiscard]] bool IsPlaying()     const { return m_playing.load(); }
    [[nodiscard]] uint32_t SampleRate() const { return m_sampleRate; }

    void FeedDevice() {}

private:
    uint32_t m_sampleRate{44100};
    uint32_t m_sourceRate{32000};

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_playing{false};

    uint32_t m_sdlDeviceId{0};
    bool     m_sdlAudioInit{false};
};

} // namespace Nova64::RSP::HLE
