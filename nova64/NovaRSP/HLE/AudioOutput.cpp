/**
 * @file AudioOutput.cpp
 * @brief RSP HLE Audio Output - WASAPI implementation for Windows.
 */
// SuperNova64 - N64 Emulator
// RSP HLE Audio Output - WASAPI implementation for Windows
// Renders processed RSP audio via Windows WASAPI shared mode

#include "AudioOutput.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#endif

#include <algorithm>
#include <cstring>
#include <cstdio>

namespace Nova64::RSP::HLE {

// ──────────────────────────────────────────────
// Construction / Destruction
// ──────────────────────────────────────────────

CAudioOutput::CAudioOutput() = default;

CAudioOutput::~CAudioOutput()
{
    Shutdown();
}

// ──────────────────────────────────────────────
// Initialize WASAPI shared-mode output
// ──────────────────────────────────────────────

bool CAudioOutput::Initialize(uint32_t sampleRate, uint32_t channels)
{
#ifdef _WIN32
    if (m_initialized.load()) return true;

    m_sampleRate = sampleRate;
    m_channels = channels;

    // Initialize COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == S_FALSE) {
        m_comInitialized = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        // COM already initialized in STA mode, that's OK
        m_comInitialized = false;
    } else {
        std::fprintf(stderr, "[AudioOutput] CoInitializeEx failed: 0x%08lX\n", hr);
        return false;
    }

    // Get default audio endpoint
    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) {
        std::fprintf(stderr, "[AudioOutput] Failed to create device enumerator: 0x%08lX\n", hr);
        Shutdown();
        return false;
    }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
    enumerator->Release();
    if (FAILED(hr)) {
        std::fprintf(stderr, "[AudioOutput] No default audio device: 0x%08lX\n", hr);
        Shutdown();
        return false;
    }

    // Activate audio client
    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                            nullptr, reinterpret_cast<void**>(&m_audioClient));
    if (FAILED(hr)) {
        std::fprintf(stderr, "[AudioOutput] Failed to activate audio client: 0x%08lX\n", hr);
        Shutdown();
        return false;
    }

    // Query the device's native mix format — WASAPI shared mode requires this.
    // We'll resample from N64 rate to device rate in QueueSamples().
    WAVEFORMATEX* mixFmt = nullptr;
    hr = m_audioClient->GetMixFormat(&mixFmt);
    if (FAILED(hr) || !mixFmt) {
        std::fprintf(stderr, "[AudioOutput] GetMixFormat failed: 0x%08lX\n", hr);
        Shutdown();
        return false;
    }

    // Store device native rate and format info
    m_sampleRate = mixFmt->nSamplesPerSec;
    m_channels   = mixFmt->nChannels;

    // Detect float vs PCM output
    m_deviceIsFloat = false;
    if (mixFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        m_deviceIsFloat = true;
    } else if (mixFmt->wFormatTag == 0xFFFE /* WAVE_FORMAT_EXTENSIBLE */ && mixFmt->cbSize >= 22) {
        // WAVEFORMATEXTENSIBLE: check SubFormat GUID
        auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mixFmt);
        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {00000003-0000-0010-8000-00AA00389B71}
        static const GUID kFloat = { 0x00000003, 0x0000, 0x0010,
            { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
        if (memcmp(&ext->SubFormat, &kFloat, sizeof(GUID)) == 0)
            m_deviceIsFloat = true;
    }

    // Initialize in shared mode with 40ms buffer
    REFERENCE_TIME bufferDuration = 400000;  // 40ms in 100ns units
    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0,                        // no flags
        bufferDuration,
        0,                        // periodicity (0 for shared mode)
        mixFmt,                   // use device's native format
        nullptr);                 // session GUID
    CoTaskMemFree(mixFmt);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[AudioOutput] Audio client init failed: 0x%08lX\n", hr);
        Shutdown();
        return false;
    }

    // Get buffer size
    hr = m_audioClient->GetBufferSize(&m_deviceBufferFrames);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[AudioOutput] GetBufferSize failed: 0x%08lX\n", hr);
        Shutdown();
        return false;
    }

    // Get render client
    hr = m_audioClient->GetService(__uuidof(IAudioRenderClient),
                                   reinterpret_cast<void**>(&m_renderClient));
    if (FAILED(hr)) {
        std::fprintf(stderr, "[AudioOutput] GetService(RenderClient) failed: 0x%08lX\n", hr);
        Shutdown();
        return false;
    }

    // Allocate ring buffer (~200ms worth of stereo audio)
    // Internal ring buffer is always stereo (2ch); upmixed to device channels in FeedDevice()
    m_internalChannels = 2;
    m_ringBufferSize = m_sampleRate * m_internalChannels / 5;  // 200ms
    if (m_ringBufferSize < 8192) m_ringBufferSize = 8192;
    // Round up to power of 2 for efficient modulo
    uint32_t po2 = 1;
    while (po2 < m_ringBufferSize) po2 <<= 1;
    m_ringBufferSize = po2;
    m_ringBuffer.resize(m_ringBufferSize, 0);
    m_readPos = 0;
    m_writePos = 0;
    m_bufferedSamples = 0;

    m_initialized.store(true);
    std::fprintf(stdout, "[AudioOutput] WASAPI initialized: %uHz %uch, device buffer=%u frames\n",
                 m_sampleRate, m_channels, m_deviceBufferFrames);
    return true;
#else
    (void)sampleRate;
    (void)channels;
    std::fprintf(stderr, "[AudioOutput] WASAPI not available on this platform\n");
    return false;
#endif
}

// ──────────────────────────────────────────────
// Shutdown
// ──────────────────────────────────────────────

void CAudioOutput::Shutdown()
{
    Stop();
    m_initialized.store(false);

#ifdef _WIN32
    if (m_renderClient) { m_renderClient->Release(); m_renderClient = nullptr; }
    if (m_audioClient)  { m_audioClient->Release();  m_audioClient = nullptr; }
    if (m_device)       { m_device->Release();       m_device = nullptr; }
    if (m_comInitialized) { CoUninitialize(); m_comInitialized = false; }
#endif

    m_ringBuffer.clear();
    m_ringBufferSize = 0;
    m_readPos = m_writePos = m_bufferedSamples = 0;
}

// ──────────────────────────────────────────────
// Queue samples from HLE audio processing
// ──────────────────────────────────────────────

void CAudioOutput::QueueSamples(const int16_t* samples, uint32_t sampleCount, uint32_t sourceRate)
{
    if (!m_initialized.load() || sampleCount == 0 || samples == nullptr) return;

    // Update source rate if caller provides one
    if (sourceRate > 0) m_sourceRate = sourceRate;

    const int16_t* finalSamples = samples;
    uint32_t finalCount = sampleCount;

    // ── Resample if source rate differs from device rate ──
    // Ring buffer is always stereo (m_internalChannels=2)
    if (m_sourceRate > 0 && m_sourceRate != m_sampleRate) {
        const uint32_t inputFrames = sampleCount / m_internalChannels;
        if (inputFrames < 2) return;  // need at least 2 frames for interpolation

        const double ratio = static_cast<double>(m_sourceRate) / m_sampleRate;

        // How many output frames can we produce from this input chunk?
        // We need m_resamplePos + (outputFrames-1)*ratio < inputFrames - 1
        const double maxInputPos = static_cast<double>(inputFrames - 1);
        if (m_resamplePos >= maxInputPos) {
            // Input consumed from previous call's residual; carry forward
            m_resamplePos -= inputFrames;
            return;
        }

        const uint32_t outputFrames = static_cast<uint32_t>(
            (maxInputPos - m_resamplePos) / ratio) + 1;
        if (outputFrames == 0) return;

        m_resampleBuffer.resize(outputFrames * m_internalChannels);

        // Linear interpolation resampling (per-channel, interleaved)
        for (uint32_t i = 0; i < outputFrames; i++) {
            const double srcPos = m_resamplePos + i * ratio;
            const uint32_t idx = static_cast<uint32_t>(srcPos);
            const double frac = srcPos - idx;

            // Clamp to valid range
            const uint32_t idx0 = idx;
            const uint32_t idx1 = (idx + 1 < inputFrames) ? idx + 1 : idx;

            for (uint32_t ch = 0; ch < m_internalChannels; ch++) {
                const int32_t s0 = samples[idx0 * m_internalChannels + ch];
                const int32_t s1 = samples[idx1 * m_internalChannels + ch];
                m_resampleBuffer[i * m_internalChannels + ch] = static_cast<int16_t>(
                    s0 + static_cast<int32_t>((s1 - s0) * frac));
            }
        }

        // Carry fractional position into next call
        m_resamplePos = (m_resamplePos + outputFrames * ratio) - inputFrames;
        if (m_resamplePos < 0.0) m_resamplePos = 0.0;

        finalSamples = m_resampleBuffer.data();
        finalCount = outputFrames * m_internalChannels;
    }

    // ── Write (possibly resampled) data into ring buffer ──
    std::lock_guard<std::mutex> lock(m_bufferMutex);

    const uint32_t mask = m_ringBufferSize - 1;
    uint32_t toWrite = finalCount;

    // Drop oldest samples if buffer is full
    if (m_bufferedSamples + toWrite > m_ringBufferSize) {
        uint32_t overflow = (m_bufferedSamples + toWrite) - m_ringBufferSize;
        m_readPos = (m_readPos + overflow) & mask;
        m_bufferedSamples -= overflow;
    }

    // Write to ring buffer
    for (uint32_t i = 0; i < toWrite; i++) {
        m_ringBuffer[m_writePos] = finalSamples[i];
        m_writePos = (m_writePos + 1) & mask;
    }
    m_bufferedSamples += toWrite;
}

// ──────────────────────────────────────────────
// Start / Stop playback
// ──────────────────────────────────────────────

void CAudioOutput::Start()
{
#ifdef _WIN32
    if (!m_initialized.load() || m_playing.load()) return;
    HRESULT hr = m_audioClient->Start();
    if (SUCCEEDED(hr)) {
        m_playing.store(true);
    }
#endif
}

void CAudioOutput::Stop()
{
#ifdef _WIN32
    if (!m_initialized.load() || !m_playing.load()) return;
    m_audioClient->Stop();
    m_audioClient->Reset();
    m_playing.store(false);
#endif
}

// ──────────────────────────────────────────────
// Feed device: copy from ring buffer to WASAPI
// Call once per frame (~16ms) or from timer
// ──────────────────────────────────────────────

void CAudioOutput::FeedDevice()
{
#ifdef _WIN32
    if (!m_initialized.load() || !m_playing.load()) return;

    // How many frames can we write?
    uint32_t padding = 0;
    HRESULT hr = m_audioClient->GetCurrentPadding(&padding);
    if (FAILED(hr)) return;

    uint32_t available = m_deviceBufferFrames - padding;
    if (available == 0) return;

    // How many frames do we have in ring buffer? (ring buffer is always stereo)
    uint32_t framesInBuffer;
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        framesInBuffer = m_bufferedSamples / m_internalChannels;
    }

    uint32_t framesToWrite = (std::min)(available, framesInBuffer);
    if (framesToWrite == 0) return;

    // Get WASAPI buffer
    BYTE* deviceBuffer = nullptr;
    hr = m_renderClient->GetBuffer(framesToWrite, &deviceBuffer);
    if (FAILED(hr) || deviceBuffer == nullptr) return;

    // Copy from ring buffer to device, upmixing stereo → device channels
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        const uint32_t mask = m_ringBufferSize - 1;

        if (m_deviceIsFloat) {
            float* dest = reinterpret_cast<float*>(deviceBuffer);
            for (uint32_t f = 0; f < framesToWrite; f++) {
                // Read stereo pair from ring buffer
                int16_t left  = m_ringBuffer[m_readPos]; m_readPos = (m_readPos + 1) & mask;
                int16_t right = m_ringBuffer[m_readPos]; m_readPos = (m_readPos + 1) & mask;
                float fL = left  / 32768.0f;
                float fR = right / 32768.0f;
                // Write to device channels: L, R, then zero-fill remaining
                dest[f * m_channels + 0] = fL;
                if (m_channels > 1) dest[f * m_channels + 1] = fR;
                for (uint32_t ch = 2; ch < m_channels; ch++)
                    dest[f * m_channels + ch] = 0.0f;
            }
        } else {
            int16_t* dest = reinterpret_cast<int16_t*>(deviceBuffer);
            for (uint32_t f = 0; f < framesToWrite; f++) {
                int16_t left  = m_ringBuffer[m_readPos]; m_readPos = (m_readPos + 1) & mask;
                int16_t right = m_ringBuffer[m_readPos]; m_readPos = (m_readPos + 1) & mask;
                dest[f * m_channels + 0] = left;
                if (m_channels > 1) dest[f * m_channels + 1] = right;
                for (uint32_t ch = 2; ch < m_channels; ch++)
                    dest[f * m_channels + ch] = 0;
            }
        }
        m_bufferedSamples -= framesToWrite * m_internalChannels;
    }

    m_renderClient->ReleaseBuffer(framesToWrite, 0);
#endif
}

} // namespace Nova64::RSP::HLE
