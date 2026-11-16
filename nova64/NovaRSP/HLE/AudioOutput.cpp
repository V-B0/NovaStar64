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
#include <cstdio>
#include <thread>
#include <chrono>

namespace Nova64::RSP::HLE {

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
}

void CAudioOutput::QueueSamples(const int16_t* samples, uint32_t sampleCount, uint32_t sourceRate) {
    if (!m_initialized.load() || sampleCount == 0 || samples == nullptr) return;
    if (sourceRate > 0) m_sourceRate = sourceRate;

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
