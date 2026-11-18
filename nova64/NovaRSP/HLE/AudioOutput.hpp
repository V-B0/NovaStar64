/**
 * @file AudioOutput.hpp
 * @brief RSP HLE Audio Output - SDL2 callback output with a ring buffer and dynamic rate control.
 *
 * The emulator thread writes the N64's PCM into a lock-free ring buffer; the SDL audio thread reads it,
 * resamples it to the device's own rate and plays it. The game hands over each DMA buffer just as the
 * previous one ends, so the output keeps a cushion of about 60 ms: it waits for the cushion before
 * it starts, plays slightly faster or slower to hold it there (the emulator's pace and the sound card's
 * clock never agree exactly), and fades out and back in instead of clicking if it ever does run dry.
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

    /// Opens the SDL audio device (at the device's own rate; `sampleRate` is the rate of the samples
    /// that will be queued). The device is always stereo; `channels` is ignored. Calling it again
    /// while the device is open only changes the source rate, without a gap in the sound.
    bool Initialize(uint32_t sampleRate = 44100, uint32_t channels = 2);
    void Shutdown();

    /// Queues `sampleCount` 16-bit values (not stereo frames), optionally updating the source rate.
    /// Never waits: audio that does not fit in the ring buffer (the emulator running far faster than
    /// real time) is dropped and counted.
    void QueueSamples(const int16_t* samples, uint32_t sampleCount, uint32_t sourceRate = 0);

    /// Rate of the samples being queued; may be changed at any time.
    void SetSourceRate(uint32_t rate) {
        if (rate > 0) m_sourceRate.store(rate, std::memory_order_relaxed);
    }

    /// Volume for all output, 0-100, and mute. Take effect immediately (with a short fade).
    static void SetMasterVolume(uint32_t percent) { s_volume.store(percent > 100 ? 100 : percent); }
    static void SetMuted(bool muted) { s_muted.store(muted); }

    void Start();
    void Stop();

    /// Throws away everything queued so far (after a state load, say). The sound fades out and the
    /// output refills its cushion before it plays again.
    void Flush();

    /// Sets the output up without a sound device, so a test can call Render() itself: the same buffer and
    /// resampling, at `deviceRate`, fed at `sourceRate`.
    void InitializeWithoutDevice(uint32_t deviceRate, uint32_t sourceRate);

    /// Fills `frames` stereo frames for the device. SDL calls it on the audio thread; it is public so that
    /// a test can play the part of the device.
    void Render(int16_t* out, int frames);

    /// Times the output ran dry, and times queued audio did not fit, since it was opened; and how many
    /// frames are waiting to be played.
    [[nodiscard]] uint32_t Underruns() const { return m_underruns.load(); }
    [[nodiscard]] uint32_t Dropped() const { return m_dropped.load(); }
    [[nodiscard]] uint32_t FramesWaiting() const { return m_head.load() - m_tail.load(); }

    [[nodiscard]] bool IsInitialized() const { return m_initialized.load(); }
    [[nodiscard]] bool IsPlaying() const { return m_playing.load(); }
    [[nodiscard]] uint32_t SampleRate() const { return m_sampleRate; }

    void FeedDevice() {}

private:
    enum class State : uint8_t { Prebuffering, Playing };

    static void AudioCallback(void* userdata, uint8_t* stream, int len);
    void ResetStream();

    static constexpr uint32_t kRingFrames = 1u << 15; ///< stereo frames; 0.68 s at 48 kHz
    static constexpr uint32_t kTargetMs = 60;         ///< the cushion the output holds to begin with
    static constexpr uint32_t kMaxTargetMs = 140;     ///< ... and the most it grows to, after underruns
    static constexpr uint32_t kTargetStepMs = 20;     ///< growth after an underrun
    static constexpr uint32_t kSettleSeconds =
        5; ///< underruns in the first seconds of play are not held against the game
    static constexpr uint32_t kRelaxSeconds = 60;  ///< clean play after which the cushion shrinks a step
    static constexpr uint32_t kMaxFillMs = 250;    ///< more than this queued is dropped, not waited for
    static constexpr uint32_t kFadeFrames = 96;    ///< fade out into a gap and in after it
    static constexpr double kMaxRateAdjust = 0.01; ///< at most 1% (17 cents) faster or slower
    static constexpr double kRateGain = 2.0;       ///< full adjustment when the cushion is half off target
    static constexpr double kFillTauSeconds = 1.0; ///< how slowly the measured cushion follows the buffer

    uint32_t m_sampleRate{48000};                  ///< the device's rate
    std::atomic<uint32_t> m_sourceRate{32000};

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_playing{false};

    uint32_t m_sdlDeviceId{0};
    bool m_sdlAudioInit{false};

    // Ring buffer of interleaved stereo frames. The emulator thread only moves m_head, the audio thread
    // only moves m_tail; Flush() moves m_tail with the audio thread locked out.
    std::vector<int16_t> m_ring;
    std::atomic<uint32_t> m_head{0};
    std::atomic<uint32_t> m_tail{0};

    // Owned by the audio thread (or touched with the device locked).
    State m_state{State::Prebuffering};
    double m_pos{0};          ///< read position past m_tail, in source frames
    double m_fillAvg{0};      ///< smoothed number of frames waiting, for the rate control
    float m_prev[2]{0, 0};    ///< the frame before m_tail, for the interpolator
    float m_lastOut[2]{0, 0}; ///< the last frame played, for fading out
    float m_tailGain{0};      ///< fade-out still to play after the sound stopped, 1 -> 0
    float m_gain{1};
    uint32_t m_fadeIn{kFadeFrames};
    uint32_t m_targetMs{kTargetMs};       ///< the cushion now: grows after underruns, shrinks after long clean play
    bool m_stalled{false};                ///< ran dry, and has not yet got its cushion back
    uint64_t m_dryFrames{0};              ///< device frames since the output last ran dry or was flushed
    uint64_t m_playedFrames{0};           ///< device frames played since the output was opened
    uint64_t m_cleanFrames{0};            ///< ... since the last underrun

    std::atomic<uint32_t> m_underruns{0}; ///< times the output ran dry
    std::atomic<uint32_t> m_dropped{0};   ///< times queued audio did not fit

    inline static std::atomic<uint32_t> s_volume{100};
    inline static std::atomic<bool> s_muted{false};
};

} // namespace Nova64::RSP::HLE
