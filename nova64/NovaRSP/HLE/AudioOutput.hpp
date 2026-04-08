/**
 * @file AudioOutput.hpp
 * @brief RSP HLE Audio Output - WASAPI implementation for Windows.
 */
// SuperNova64 - N64 Emulator
// RSP HLE Audio Output - WASAPI implementation for Windows
// Provides audio device output for processed RSP audio samples

#pragma once
#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>

#ifdef _WIN32
struct IMMDevice;
struct IAudioClient;
struct IAudioRenderClient;
#endif

namespace Nova64::RSP::HLE {

// ──────────────────────────────────────────────
// CAudioOutput - Low-latency audio rendering via WASAPI
// Thread-safe ring buffer: HLE thread writes, WASAPI pulls
// ──────────────────────────────────────────────
class CAudioOutput {
public:
    CAudioOutput();
    ~CAudioOutput();

    CAudioOutput(const CAudioOutput&) = delete;
    CAudioOutput& operator=(const CAudioOutput&) = delete;

    // Initialize WASAPI shared-mode audio device
    // Returns true on success
    bool Initialize(uint32_t sampleRate = 44100, uint32_t channels = 2);

    // Shutdown and release audio device
    void Shutdown();

    // Queue interleaved 16-bit PCM samples (L,R,L,R,...)
    // Thread-safe. Samples are buffered until the device pulls them.
    // If sourceRate != 0 and differs from device rate, samples are resampled.
    void QueueSamples(const int16_t* samples, uint32_t sampleCount, uint32_t sourceRate = 0);

    // Set the expected source sample rate (N64 typically uses 32000Hz)
    void SetSourceRate(uint32_t rate) { m_sourceRate = rate; }

    // Start/Stop audio playback
    void Start();
    void Stop();

    // Query state
    [[nodiscard]] bool IsInitialized() const { return m_initialized.load(); }
    [[nodiscard]] bool IsPlaying()     const { return m_playing.load(); }
    [[nodiscard]] uint32_t SampleRate() const { return m_sampleRate; }

    // Feed pending samples to WASAPI device buffer
    // Call this periodically (e.g., once per frame or from a timer)
    void FeedDevice();

private:
    // Ring buffer for audio samples
    std::mutex          m_bufferMutex;
    std::vector<int16_t> m_ringBuffer;
    uint32_t            m_readPos{0};
    uint32_t            m_writePos{0};
    uint32_t            m_bufferedSamples{0};

    // Configuration
    uint32_t m_sampleRate{44100};        // device native sample rate
    uint32_t m_channels{2};              // device native channel count
    uint32_t m_internalChannels{2};      // ring buffer is always stereo
    uint32_t m_ringBufferSize{0};
    uint32_t m_sourceRate{32000};        // N64 audio source rate
    bool     m_deviceIsFloat{false};     // true if WASAPI device wants float32

    // Resampler state (persistent across QueueSamples calls)
    double   m_resamplePos{0.0};
    int16_t  m_lastInputSample[2]{0, 0};  // last stereo sample for cross-boundary interpolation
    std::vector<int16_t> m_resampleBuffer;

    // State
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_playing{false};

#ifdef _WIN32
    // WASAPI handles
    IMMDevice*           m_device{nullptr};
    IAudioClient*        m_audioClient{nullptr};
    IAudioRenderClient*  m_renderClient{nullptr};
    uint32_t             m_deviceBufferFrames{0};
    bool                 m_comInitialized{false};
#endif
};

} // namespace Nova64::RSP::HLE
