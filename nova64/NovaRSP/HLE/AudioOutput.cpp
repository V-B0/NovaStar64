/**
 * @file AudioOutput.cpp
 * @brief RSP HLE Audio Output - SDL2 callback output with a ring buffer and dynamic rate control.
 *
 * The emulator thread queues the N64's PCM into a ring buffer (QueueSamples, never blocking); the SDL
 * audio thread (Render) pulls it out, resamples it to the device's rate with a cubic interpolator and
 * plays it. Three things keep it from crackling:
 *  - a cushion: playback starts only once about 60 ms is waiting, and starts again after a gap only
 *    when the cushion is back, because the game queues each DMA buffer just as the last one ends;
 *  - rate control: the emulator's pace and the sound card's clock never agree exactly, so the read
 *    speed is nudged (at most 1%) to hold the cushion steady instead of letting it drain or fill;
 *  - fades: if the buffer does run dry the sound fades out and back in, with no step to click.
 */
#include "AudioOutput.hpp"
#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

/// With NOVA64_AUDIO_STATS=1, prints every two seconds how much audio was waiting in the ring buffer
/// each time the game queued more, how many times the output ran dry (a gap in the sound) and how
/// many times queued audio did not fit.
class QueueStats {
public:
    void Sample(uint32_t framesWaiting, uint32_t rate, uint32_t underruns, uint32_t dropped) {
        if (!m_enabled || rate == 0) return;
        const double ms = framesWaiting * 1000.0 / static_cast<double>(rate);
        m_min = std::min(m_min, ms);
        m_max = std::max(m_max, ms);
        m_sum += ms;
        ++m_calls;
        const auto now = std::chrono::steady_clock::now();
        if (m_calls == 1) m_since = now;
        if (m_last.time_since_epoch().count() != 0)
            m_longestGap = std::max(m_longestGap, std::chrono::duration<double, std::milli>(now - m_last).count());
        m_last = now;
        if (now - m_since >= std::chrono::seconds(2)) {
            std::fprintf(stderr,
                         "[AudioStats] %u queues, waiting ms min/avg/max %.1f/%.1f/%.1f, ran dry %u times (%u in all), "
                         "dropped %u times, longest wait for the game %.0f ms\n",
                         m_calls, m_min, m_sum / m_calls, m_max, underruns - m_lastUnderruns, underruns,
                         dropped - m_lastDropped, m_longestGap);
            m_lastUnderruns = underruns;
            m_lastDropped = dropped;
            m_min = 1e9;
            m_max = m_sum = m_longestGap = 0;
            m_calls = 0;
        }
    }

private:
    const bool m_enabled = [] {
        const char* v = std::getenv("NOVA64_AUDIO_STATS");
        return v && *v && *v != '0';
    }();
    double m_min = 1e9, m_max = 0, m_sum = 0, m_longestGap = 0;
    std::chrono::steady_clock::time_point m_last;
    uint32_t m_calls = 0, m_lastUnderruns = 0, m_lastDropped = 0;
    std::chrono::steady_clock::time_point m_since;
};

QueueStats g_stats;

int16_t ToS16(float v) {
    v = std::clamp(v, -32768.0f, 32767.0f);
    return static_cast<int16_t>(std::lrintf(v));
}

} // namespace

CAudioOutput::CAudioOutput() = default;

CAudioOutput::~CAudioOutput() { Shutdown(); }

bool CAudioOutput::Initialize(uint32_t sampleRate, uint32_t /*channels*/) {
    SetSourceRate(sampleRate);
    if (m_initialized.load()) return true;

    if (!m_sdlAudioInit) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            std::fprintf(stderr, "[AudioOutput] SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
            return false;
        }
        m_sdlAudioInit = true;
    }

    ResetStream();

    // The device runs at its own rate (the change is allowed) and Render() resamples to it, so a
    // change of the game's DAC rate never needs the device reopened.
    SDL_AudioSpec desired{};
    SDL_AudioSpec obtained{};

    desired.freq = 48000;
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 512;
    desired.callback = &CAudioOutput::AudioCallback;
    desired.userdata = this;

    m_sdlDeviceId = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);

    if (m_sdlDeviceId == 0) {
        std::fprintf(stderr, "[AudioOutput] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }

    m_sampleRate = static_cast<uint32_t>(obtained.freq);
    m_initialized.store(true);

    std::fprintf(stderr, "[AudioOutput] SDL2 callback audio: device %u Hz ch=%u samples=%u fmt=0x%04X, source %u Hz\n",
                 obtained.freq, obtained.channels, obtained.samples, obtained.format,
                 m_sourceRate.load(std::memory_order_relaxed));
    return true;
}

void CAudioOutput::ResetStream() {
    m_ring.assign(static_cast<size_t>(kRingFrames) * 2, 0);
    m_head.store(0);
    m_tail.store(0);
    m_state = State::Prebuffering;
    m_pos = 0;
    m_fillAvg = 0;
    m_prev[0] = m_prev[1] = 0;
    m_lastOut[0] = m_lastOut[1] = 0;
    m_tailGain = 0;
    m_fadeIn = kFadeFrames;
    m_targetMs = kTargetMs;
    m_stalled = false;
    m_dryFrames = 0;
    m_playedFrames = 0;
    m_cleanFrames = 0;
    m_gain = 1;
    m_underruns.store(0);
    m_dropped.store(0);
}

void CAudioOutput::InitializeWithoutDevice(uint32_t deviceRate, uint32_t sourceRate) {
    ResetStream();
    SetSourceRate(sourceRate);
    m_sampleRate = deviceRate;
    m_initialized.store(true);
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
    if (!m_initialized.load() || sampleCount < 2 || samples == nullptr) return;
    SetSourceRate(sourceRate);
    const uint32_t srcRate = m_sourceRate.load(std::memory_order_relaxed);
    g_dump.Write(samples, sampleCount, srcRate);

    const uint32_t head = m_head.load(std::memory_order_relaxed);
    const uint32_t used = head - m_tail.load(std::memory_order_acquire);
    g_stats.Sample(used, srcRate, m_underruns.load(std::memory_order_relaxed),
                   m_dropped.load(std::memory_order_relaxed));

    // Never wait for room: the emulator thread also paces the game. Audio that does not fit (the
    // emulator running far ahead of real time) is dropped.
    const uint32_t maxFill =
        std::min<uint32_t>(static_cast<uint32_t>(uint64_t(srcRate) * kMaxFillMs / 1000), kRingFrames);
    const uint32_t room = used >= maxFill ? 0 : maxFill - used;
    uint32_t frames = sampleCount / 2;
    if (frames > room) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        frames = room;
    }
    if (frames == 0) return;

    const uint32_t mask = kRingFrames - 1;
    int16_t* ring = m_ring.data();
    for (uint32_t i = 0; i < frames; ++i) {
        const size_t at = static_cast<size_t>((head + i) & mask) * 2;
        ring[at] = samples[i * 2];
        ring[at + 1] = samples[i * 2 + 1];
    }
    m_head.store(head + frames, std::memory_order_release);
}

void CAudioOutput::AudioCallback(void* userdata, uint8_t* stream, int len) {
    static_cast<CAudioOutput*>(userdata)->Render(reinterpret_cast<int16_t*>(stream), len / 4);
}

void CAudioOutput::Render(int16_t* out, int frames) {
    if (frames <= 0) return;
    const uint32_t srcRate = std::max<uint32_t>(m_sourceRate.load(std::memory_order_relaxed), 1u);
    const double devRate = m_sampleRate > 0 ? static_cast<double>(m_sampleRate) : 48000.0;
    const uint32_t head = m_head.load(std::memory_order_acquire);
    const uint32_t tail = m_tail.load(std::memory_order_relaxed);
    const uint32_t avail = head - tail;
    const uint32_t targetFrames = std::max<uint32_t>(static_cast<uint32_t>(uint64_t(srcRate) * m_targetMs / 1000), 64u);

    // Volume (squared, so the slider sounds even across its range) and mute, ramped over the block so
    // a change makes no click.
    const uint32_t volume = s_muted.load(std::memory_order_relaxed) ? 0 : std::min<uint32_t>(s_volume.load(), 100u);
    const float gainTarget = static_cast<float>(volume * volume) / 10000.0f;
    float gain = m_gain;
    const float gainStep = (gainTarget - gain) / static_cast<float>(frames);
    m_gain = gainTarget;

    if (m_state == State::Prebuffering) {
        m_dryFrames += static_cast<uint64_t>(frames);
        if (avail >= targetFrames) {
            m_state = State::Playing;
            m_fadeIn = 0;
            m_tailGain = 0;
            m_fillAvg = static_cast<double>(avail);
            // After a hitch the cushion could have covered with a bit more in it, keep a bit more (once
            // start-up is over). A longer gap (a pause, a load, a stall in the graphics driver) could not have
            // been covered, so it changes nothing: more cushion would only add delay.
            if (m_stalled && m_playedFrames > kSettleSeconds * devRate &&
                m_dryFrames <= (kMaxTargetMs - kTargetStepMs) * devRate / 1000.0)
                m_targetMs = std::min(kMaxTargetMs, m_targetMs + kTargetStepMs);
            m_stalled = false;
            m_dryFrames = 0;
        }
    }

    int n = 0;
    if (m_state == State::Playing) {
        // Hold the cushion at its target by reading a little faster when it is fuller and slower when
        // it is emptier. The measured fill is smoothed over about a second, because it jumps by a whole
        // DMA buffer each time the game queues one.
        const double alpha = std::min(1.0, (frames / devRate) / kFillTauSeconds);
        m_fillAvg += (static_cast<double>(avail) - m_fillAvg) * alpha;
        const double error = std::clamp((m_fillAvg - targetFrames) / static_cast<double>(targetFrames), -1.0, 1.0);
        const double step = (srcRate / devRate) * (1.0 + std::clamp(error * kRateGain, -1.0, 1.0) * kMaxRateAdjust);

        const uint32_t mask = kRingFrames - 1;
        const int16_t* ring = m_ring.data();
        auto sample = [&](uint32_t index, int ch) {
            return static_cast<float>(ring[static_cast<size_t>((tail + index) & mask) * 2 + ch]);
        };

        double pos = m_pos;
        for (; n < frames; ++n) {
            const uint32_t i = static_cast<uint32_t>(pos);
            if (i + 2 >= avail) break; // the interpolator needs two frames ahead
            const float t = static_cast<float>(pos - i);
            gain += gainStep;
            const float fade = m_fadeIn < kFadeFrames ? static_cast<float>(++m_fadeIn) / kFadeFrames : 1.0f;
            for (int ch = 0; ch < 2; ++ch) {
                // Catmull-Rom cubic through the four frames around the read position.
                const float s0 = i == 0 ? m_prev[ch] : sample(i - 1, ch);
                const float s1 = sample(i, ch);
                const float s2 = sample(i + 1, ch);
                const float s3 = sample(i + 2, ch);
                const float c1 = 0.5f * (s2 - s0);
                const float c2 = s0 - 2.5f * s1 + 2.0f * s2 - 0.5f * s3;
                const float c3 = 0.5f * (s3 - s0) + 1.5f * (s1 - s2);
                const float y = (((c3 * t + c2) * t + c1) * t + s1) * gain * fade;
                m_lastOut[ch] = y;
                out[n * 2 + ch] = ToS16(y);
            }
            pos += step;
        }

        m_playedFrames += static_cast<uint64_t>(n);
        m_cleanFrames += static_cast<uint64_t>(n);
        if (n == frames && m_cleanFrames > kRelaxSeconds * devRate && m_targetMs > kTargetMs) {
            m_targetMs = std::max(kTargetMs, m_targetMs - kTargetStepMs / 2);
            m_cleanFrames = 0;
        }

        const uint32_t consumed = std::min(static_cast<uint32_t>(pos), avail);
        if (consumed > 0) {
            const size_t last = static_cast<size_t>((tail + consumed - 1) & mask) * 2;
            m_prev[0] = ring[last];
            m_prev[1] = ring[last + 1];
            m_tail.store(tail + consumed, std::memory_order_release);
        }
        m_pos = std::max(0.0, pos - consumed);

        if (n < frames) {
            // Ran dry: fade out below, then wait for the cushion before playing again.
            m_underruns.fetch_add(1, std::memory_order_relaxed);
            m_state = State::Prebuffering;
            m_tailGain = 1.0f;
            m_cleanFrames = 0;
            m_dryFrames = 0;
            m_stalled = true;
        }
    }

    // The rest of the block: the end of a fade-out, then silence.
    for (; n < frames; ++n) {
        float a = 0.0f;
        if (m_tailGain > 0.0f) {
            m_tailGain = std::max(0.0f, m_tailGain - 1.0f / kFadeFrames);
            a = m_tailGain;
        }
        out[n * 2] = ToS16(m_lastOut[0] * a);
        out[n * 2 + 1] = ToS16(m_lastOut[1] * a);
    }
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
    SDL_LockAudioDevice(m_sdlDeviceId);
    m_tail.store(m_head.load(std::memory_order_relaxed), std::memory_order_release);
    m_state = State::Prebuffering;
    m_pos = 0;
    m_prev[0] = m_prev[1] = 0;
    m_lastOut[0] = m_lastOut[1] = 0; // a stopped device has already cut the sound; nothing to fade
    m_tailGain = 0;
    SDL_UnlockAudioDevice(m_sdlDeviceId);
    m_playing.store(false);
    std::fprintf(stderr, "[AudioOutput] SDL2 audio stopped\n");
}

void CAudioOutput::Flush() {
    if (!m_initialized.load()) return;
    // The audio thread is locked out while its read position moves.
    if (m_sdlDeviceId != 0) SDL_LockAudioDevice(m_sdlDeviceId);
    m_tail.store(m_head.load(std::memory_order_relaxed), std::memory_order_release);
    if (m_state == State::Playing) {
        m_state = State::Prebuffering;
        m_tailGain = 1.0f; // fade the old sound out rather than cut it
    }
    m_pos = 0;
    m_prev[0] = m_prev[1] = 0;
    if (m_sdlDeviceId != 0) SDL_UnlockAudioDevice(m_sdlDeviceId);
}

} // namespace Nova64::RSP::HLE
