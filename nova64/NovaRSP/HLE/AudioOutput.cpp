/**
 * @file AudioOutput.cpp
 * @brief RSP HLE Audio Output - SDL2 queue-based implementation.
 *
 * Minimal audio output: pushes raw N64 PCM directly to SDL2 via
 * SDL_QueueAudio at the N64's native sample rate.  SDL2 handles all
 * resampling, buffering, and device communication internally.
 * Back-pressure via SDL_GetQueuedAudioSize prevents runaway buffering.
 */
#include "AudioOutput.hpp"
#include <SDL.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace Nova64::RSP::HLE {

namespace {

/// Writes everything the game plays to a 16-bit stereo WAV file when NOVA64_AUDIO_DUMP=<file> is
/// set, so a run can be recorded with its sound. The file is complete when the program exits.
class WavDump {
public:
    ~WavDump() { Close(); }

    void Write(const int16_t* samples, uint32_t count, uint32_t rate) {
        if (m_failed) return;
        if (!m_file && !Open(rate)) return;
        if (rate != m_rate && !m_warnedRate) {
            std::fprintf(stderr, "[AudioOutput] sample rate changed from %u to %u; the WAV keeps the first\n", m_rate,
                         rate);
            m_warnedRate = true;
        }
        m_bytes += static_cast<uint32_t>(std::fwrite(samples, sizeof(int16_t), count, m_file) * sizeof(int16_t));
    }

    void Close() {
        if (!m_file) return;
        WriteHeader();
        std::fclose(m_file);
        m_file = nullptr;
        m_failed = true; // do not reopen and overwrite the finished file
    }

private:
    bool Open(uint32_t rate) {
        const char* path = std::getenv("NOVA64_AUDIO_DUMP");
        if (!path || !*path) return (m_failed = true, false);
        m_file = std::fopen(path, "wb");
        if (!m_file) {
            std::fprintf(stderr, "[AudioOutput] cannot write %s\n", path);
            return (m_failed = true, false);
        }
        m_rate = rate;
        WriteHeader(); // sizes are filled in on Close
        return true;
    }

    void WriteHeader() {
        auto u32 = [](uint8_t* out, uint32_t v) {
            for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(v >> (8 * i));
        };
        auto u16 = [](uint8_t* out, uint16_t v) {
            out[0] = static_cast<uint8_t>(v);
            out[1] = static_cast<uint8_t>(v >> 8);
        };
        uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
        u32(h + 4, 36 + m_bytes);
        u32(h + 16, 16);
        u16(h + 20, 1); // PCM
        u16(h + 22, 2); // stereo
        u32(h + 24, m_rate);
        u32(h + 28, m_rate * 4);
        u16(h + 32, 4);
        u16(h + 34, 16);
        std::memcpy(h + 36, "data", 4);
        u32(h + 40, m_bytes);
        std::fseek(m_file, 0, SEEK_SET);
        std::fwrite(h, 1, sizeof(h), m_file);
        std::fseek(m_file, 0, SEEK_END);
    }

    std::FILE* m_file = nullptr;
    uint32_t m_rate = 0;
    uint32_t m_bytes = 0;
    bool m_failed = false;
    bool m_warnedRate = false;
};

WavDump g_dump;

} // namespace

CAudioOutput::CAudioOutput() = default;

CAudioOutput::~CAudioOutput() { Shutdown(); }

bool CAudioOutput::Initialize(uint32_t sampleRate, uint32_t /*channels*/) {
    if (m_initialized.load()) return true;

    m_sourceRate = sampleRate;

    if (!m_sdlAudioInit) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            std::fprintf(stderr, "[AudioOutput] SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
            return false;
        }
        m_sdlAudioInit = true;
    }

    // Open device at the N64's native rate.  No callback — we push data
    // via SDL_QueueAudio, same model as Project64 pushing to DirectSound.
    SDL_AudioSpec desired{};
    SDL_AudioSpec obtained{};

    desired.freq = static_cast<int>(sampleRate);
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 1024;
    desired.callback = nullptr; // queue mode — no callback
    desired.userdata = nullptr;

    m_sdlDeviceId = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);

    if (m_sdlDeviceId == 0) {
        std::fprintf(stderr, "[AudioOutput] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }

    m_sampleRate = static_cast<uint32_t>(obtained.freq);
    m_initialized.store(true);

    std::fprintf(stderr, "[AudioOutput] SDL2 queue-mode audio: freq=%u ch=%u samples=%u fmt=0x%04X\n", obtained.freq,
                 obtained.channels, obtained.samples, obtained.format);
    return true;
}

void CAudioOutput::Shutdown() {
    Stop();
    m_initialized.store(false);

    if (m_sdlDeviceId != 0) {
        SDL_CloseAudioDevice(m_sdlDeviceId);
        m_sdlDeviceId = 0;
    }
    g_dump.Close();
}

void CAudioOutput::QueueSamples(const int16_t* samples, uint32_t sampleCount, uint32_t sourceRate) {
    if (!m_initialized.load() || sampleCount == 0 || samples == nullptr) return;
    if (sourceRate > 0) m_sourceRate = sourceRate;
    g_dump.Write(samples, sampleCount, m_sourceRate);

    // Back-pressure: if SDL2 has more than ~200ms queued, wait.
    const uint32_t maxQueueBytes = m_sourceRate * 4 / 5; // ~200ms of stereo 16-bit

    for (int attempt = 0; attempt < 50; ++attempt) {
        uint32_t queued = SDL_GetQueuedAudioSize(m_sdlDeviceId);
        if (queued < maxQueueBytes) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const uint32_t volume = s_muted.load(std::memory_order_relaxed) ? 0 : s_volume.load(std::memory_order_relaxed);
    if (volume >= 100) {
        SDL_QueueAudio(m_sdlDeviceId, samples, sampleCount * sizeof(int16_t));
        return;
    }
    // Squared, so the slider sounds even across its range. Muted output still
    // queues silence, which keeps the back-pressure above pacing the game.
    const int32_t gain = static_cast<int32_t>(volume * volume); // out of 10000
    m_scaled.resize(sampleCount);
    for (uint32_t i = 0; i < sampleCount; ++i)
        m_scaled[i] = static_cast<int16_t>(static_cast<int32_t>(samples[i]) * gain / 10000);
    SDL_QueueAudio(m_sdlDeviceId, m_scaled.data(), sampleCount * sizeof(int16_t));
}

void CAudioOutput::Start() {
    if (!m_initialized.load() || m_playing.load()) return;
    SDL_PauseAudioDevice(m_sdlDeviceId, 0);
    m_playing.store(true);
    std::fprintf(stderr, "[AudioOutput] SDL2 audio started (device %u)\n", m_sdlDeviceId);
}

void CAudioOutput::Stop() {
    if (!m_initialized.load() || !m_playing.load()) return;
    SDL_PauseAudioDevice(m_sdlDeviceId, 1);
    SDL_ClearQueuedAudio(m_sdlDeviceId);
    m_playing.store(false);
    std::fprintf(stderr, "[AudioOutput] SDL2 audio stopped\n");
}

} // namespace Nova64::RSP::HLE
