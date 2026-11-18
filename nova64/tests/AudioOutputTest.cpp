/**
 * @file AudioOutputTest.cpp
 * @brief Tests of the audio output (NovaRSP/HLE/AudioOutput): the cushion, the rate control, the fades and the
 *        resampler, with a simulated game feeding it and a simulated sound card draining it. The clocks are
 *        made up, so the runs are exact and repeat; no sound device is needed.
 */
#include "../NovaRSP/HLE/AudioOutput.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using Nova64::RSP::HLE::CAudioOutput;

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::printf("FAILED %s:%d  %s\n", __FILE__, __LINE__, #condition);                                         \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kDeviceRate = 48000;
constexpr int kBlock = 512; // frames the sound card asks for at a time
constexpr double kAmplitude = 8000.0;
constexpr double kToneHz = 1000.0;

/// A deterministic pseudo-random number in [-1, 1].
class Noise {
public:
    double Next() {
        m_state = m_state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(m_state >> 11) / static_cast<double>(1ULL << 52) - 1.0;
    }

private:
    uint64_t m_state = 12345;
};

/// A game that hands over a sine tone in DMA-sized buffers, and a sound card that takes it block by block.
/// Time is in microseconds. The game's clock can run fast or slow against the card's, and be late by a
/// random amount each time, or stop altogether for a while.
struct Run {
    uint32_t sourceRate = 32006;
    uint32_t bufferFrames = 528;
    double gameSpeed = 1.0;  ///< 1.0 = produces exactly as fast as the card plays
    double jitterUs = 0;     ///< each buffer arrives up to this early or late
    double stallFromUs = -1; ///< the game queues nothing from here ...
    double stallToUs = -1;   ///< ... until here
    int repeatStalls = 1;    ///< that many stalls of the same length, each `stallEveryUs` after the last
    double stallEveryUs = 0;
    double seconds = 30;
    double volumeChangeAtUs = -1;
    uint32_t sourceRateAfter = 0; ///< changes the rate at the half-way mark
    int flushAtBlock = -1;

    std::vector<int16_t> left;     ///< what the card played
    std::vector<uint32_t> waiting; ///< frames waiting, sampled each time the card took a block
    uint32_t underruns = 0;
    uint32_t dropped = 0;
};

void Simulate(Run& run) {
    CAudioOutput out;
    out.InitializeWithoutDevice(kDeviceRate, run.sourceRate);

    const double bufferUs = run.bufferFrames * 1e6 / run.sourceRate / run.gameSpeed;
    const double blockUs = kBlock * 1e6 / kDeviceRate;
    const double endUs = run.seconds * 1e6;

    Noise noise;
    double idealUs = bufferUs;                                // when the next buffer is due
    double arrivalUs = idealUs + noise.Next() * run.jitterUs; // when it actually arrives
    double nextBlockUs = blockUs;
    double phase = 0;                                         // of the tone, continuous across buffers
    uint32_t rate = run.sourceRate;
    uint32_t frames = run.bufferFrames;                       // the game sends the same stretch of time in each buffer
    bool rateChanged = false;
    int blockCount = 0;
    std::vector<int16_t> pcm;
    std::vector<int16_t> block(static_cast<size_t>(kBlock) * 2);

    auto playBlock = [&] {
        if (blockCount == run.flushAtBlock) out.Flush();
        out.Render(block.data(), kBlock);
        for (int i = 0; i < kBlock; ++i) run.left.push_back(block[static_cast<size_t>(i) * 2]);
        run.waiting.push_back(out.FramesWaiting());
        nextBlockUs += blockUs;
        ++blockCount;
    };

    while (nextBlockUs < endUs) {
        if (arrivalUs >= nextBlockUs) {
            if (run.volumeChangeAtUs >= 0 && nextBlockUs >= run.volumeChangeAtUs) {
                CAudioOutput::SetMasterVolume(0);
                run.volumeChangeAtUs = -1;
            }
            playBlock();
            continue;
        }
        const double due = idealUs;
        idealUs += bufferUs;
        arrivalUs = idealUs + noise.Next() * run.jitterUs;
        bool stalled = false;
        for (int k = 0; k < run.repeatStalls; ++k)
            stalled |= due >= run.stallFromUs + k * run.stallEveryUs && due < run.stallToUs + k * run.stallEveryUs;
        if (stalled) continue; // the game queues nothing now
        if (!rateChanged && run.sourceRateAfter != 0 && due > endUs / 2) {
            rate = run.sourceRateAfter;
            frames = static_cast<uint32_t>(std::lround(static_cast<double>(run.bufferFrames) * rate / run.sourceRate));
            rateChanged = true;
        }
        pcm.resize(static_cast<size_t>(frames) * 2);
        for (uint32_t i = 0; i < frames; ++i) {
            phase += 2.0 * kPi * kToneHz / rate;
            pcm[i * 2] = pcm[i * 2 + 1] = static_cast<int16_t>(std::lround(kAmplitude * std::sin(phase)));
        }
        out.QueueSamples(pcm.data(), frames * 2, rate);
    }
    CAudioOutput::SetMasterVolume(100);
    run.underruns = out.Underruns();
    run.dropped = out.Dropped();
}

/// The largest step between two neighbouring played samples from `from` on. A clean 1 kHz tone at this
/// amplitude never steps more than about 1050 at 48 kHz; a click or a cut is far more.
int MaxStep(const std::vector<int16_t>& left, size_t from) {
    int step = 0;
    for (size_t i = std::max<size_t>(from, 1); i < left.size(); ++i)
        step = std::max(step, std::abs(left[i] - left[i - 1]));
    return step;
}

/// How many times per second the tone crosses zero going up, measured over [from, to).
double ToneHz(const std::vector<int16_t>& left, size_t from, size_t to) {
    int crossings = 0;
    for (size_t i = from + 1; i < to && i < left.size(); ++i)
        if (left[i - 1] < 0 && left[i] >= 0) ++crossings;
    return crossings * static_cast<double>(kDeviceRate) / static_cast<double>(to - from);
}

uint32_t MaxWaiting(const Run& run, size_t fromBlock) {
    uint32_t m = 0;
    for (size_t i = fromBlock; i < run.waiting.size(); ++i) m = std::max(m, run.waiting[i]);
    return m;
}

uint32_t MinWaiting(const Run& run, size_t fromBlock) {
    uint32_t m = 0xFFFFFFFFu;
    for (size_t i = fromBlock; i < run.waiting.size(); ++i) m = std::min(m, run.waiting[i]);
    return m;
}

const size_t kSettled = 2 * kDeviceRate / kBlock; // blocks in the first two seconds

void Report(const char* name, const Run& run) {
    std::printf("  %-28s ran dry %u, dropped %u, waiting %u..%u frames, largest step %d\n", name, run.underruns,
                run.dropped, MinWaiting(run, kSettled), MaxWaiting(run, kSettled),
                MaxStep(run.left, kSettled * kBlock));
}

void SteadyPlayDoesNotRunDry() {
    Run run;
    run.jitterUs = 4000; // buffers up to 4 ms early or late
    Simulate(run);
    Report("steady, 4 ms jitter", run);
    CHECK(run.underruns == 0);
    CHECK(run.dropped == 0);
    CHECK(MaxStep(run.left, kSettled * kBlock) < 1200); // no click anywhere
    // Plays the tone at the pitch it was made at
    const double hz = ToneHz(run.left, kSettled * kBlock, run.left.size());
    CHECK(std::fabs(hz - kToneHz) < 0.01 * kToneHz);
    // The cushion stays near its 60 ms (1920 source frames) target
    CHECK(MinWaiting(run, kSettled) > 800);
    CHECK(MaxWaiting(run, kSettled) < 3600);
}

void GameRunningALittleFastOrSlowIsFollowed() {
    for (const double speed : {1.0005, 0.9995, 1.002, 0.998, 1.004, 0.996}) {
        Run run;
        run.gameSpeed = speed;
        run.jitterUs = 3000;
        run.seconds = 90;
        Simulate(run);
        char name[64];
        std::snprintf(name, sizeof(name), "game at %.3f x", speed);
        Report(name, run);
        CHECK(run.underruns == 0);
        CHECK(run.dropped == 0);
        CHECK(MinWaiting(run, kSettled) > 400);
        CHECK(MaxWaiting(run, kSettled) < 6000);
        // The pitch moves with the game's speed by that much and no more (the rate control only follows it)
        const double hz = ToneHz(run.left, kSettled * kBlock, run.left.size());
        CHECK(std::fabs(hz - kToneHz) < 0.012 * kToneHz);
    }
}

void AGameFarTooFastIsDroppedNotWaitedFor() {
    Run run;
    run.gameSpeed = 1.03; // 3%: more than the rate control may absorb
    run.seconds = 40;
    Simulate(run);
    Report("game 3% too fast", run);
    CHECK(run.dropped > 0);
    CHECK(MaxWaiting(run, 0) <= 250 * 32006 / 1000 + 600); // never more than the 250 ms limit queued
}

void AStallFadesOutAndBackIn() {
    Run run;
    run.stallFromUs = 10e6;
    run.stallToUs = 10.4e6; // the game stops for 0.4 s
    run.seconds = 20;
    Simulate(run);
    Report("0.4 s stall", run);
    CHECK(run.underruns == 1);
    CHECK(MaxStep(run.left, kSettled * kBlock) < 1200); // no click going into the gap or out of it
    // The gap is silent, and the tone is back well before the end
    size_t silent = 0;
    for (size_t i = 10 * kDeviceRate; i < 11 * kDeviceRate; ++i) silent += run.left[i] == 0;
    CHECK(silent > kDeviceRate / 4);
    const double hz = ToneHz(run.left, 15 * kDeviceRate, 19 * kDeviceRate);
    CHECK(std::fabs(hz - kToneHz) < 0.01 * kToneHz);
    // A stall that long could not have been covered, so the cushion is no bigger afterwards
    uint32_t after = 0;
    for (size_t i = 15 * kDeviceRate / kBlock; i < run.waiting.size(); ++i) after = std::max(after, run.waiting[i]);
    CHECK(after < 2400);
}

void APauseDoesNotRaiseTheCushion() {
    Run run;
    run.stallFromUs = 10e6;
    run.stallToUs = 13e6; // the game is paused for 3 s
    run.stallEveryUs = 20e6;
    run.repeatStalls = 3; // paused again and again
    run.seconds = 80;
    Simulate(run);
    Report("three 3 s pauses", run);
    CHECK(run.underruns == 3);
    const size_t last = run.waiting.size();
    uint64_t late = 0, early = 0;
    for (size_t i = 0; i < 187; ++i) {
        early += run.waiting[kSettled + i];
        late += run.waiting[last - 1 - i];
    }
    CHECK(late < early + early / 5); // back where it began, not grown by each pause
}

void RepeatedStallsRaiseTheCushion() {
    Run run;
    run.stallFromUs = 10e6;
    run.stallToUs = 10.1e6;  // 0.1 s, longer than the first cushion lasts
    run.stallEveryUs = 20e6; // the cushion refills at about 1% of real time, so it needs a while between stalls
    run.repeatStalls = 5;
    run.seconds = 100;
    Simulate(run);
    Report("five 0.1 s stalls", run);
    CHECK(run.underruns >= 1 && run.underruns <= 3); // it learns: the later stalls fit in the cushion
    CHECK(MaxStep(run.left, kSettled * kBlock) < 1200);
    // After the last stall the cushion is bigger than it began
    const size_t last = run.waiting.size();
    uint64_t late = 0, early = 0;
    for (size_t i = 0; i < 187; ++i) {
        early += run.waiting[kSettled + i];
        late += run.waiting[last - 1 - i];
    }
    CHECK(late > early + early / 5);
}

void ADifferentSourceRateNeedsNoGap() {
    Run run;
    run.sourceRateAfter = 22050;
    run.seconds = 20;
    Simulate(run);
    Report("source rate 32006 -> 22050", run);
    CHECK(run.underruns == 0);
    CHECK(MaxStep(run.left, kSettled * kBlock) < 1200);
}

void FlushFadesAndRefills() {
    Run run;
    run.flushAtBlock = 800;
    run.seconds = 20;
    Simulate(run);
    Report("flush", run);
    CHECK(MaxStep(run.left, kSettled * kBlock) < 1200);
    CHECK(run.underruns == 0); // a flush is not an underrun
    const double hz = ToneHz(run.left, 15 * kDeviceRate, 19 * kDeviceRate);
    CHECK(std::fabs(hz - kToneHz) < 0.01 * kToneHz);
}

void VolumeChangeMakesNoClick() {
    Run run;
    run.volumeChangeAtUs = 8e6;
    run.seconds = 12;
    Simulate(run);
    Report("volume to 0", run);
    CHECK(MaxStep(run.left, kSettled * kBlock) < 1200);
    // Silent at the end
    int loud = 0;
    for (size_t i = 11 * kDeviceRate; i < run.left.size(); ++i)
        loud = std::max(loud, std::abs(static_cast<int>(run.left[i])));
    CHECK(loud == 0);
}

} // namespace

int main() {
    SteadyPlayDoesNotRunDry();
    GameRunningALittleFastOrSlowIsFollowed();
    AGameFarTooFastIsDroppedNotWaitedFor();
    AStallFadesOutAndBackIn();
    APauseDoesNotRaiseTheCushion();
    RepeatedStallsRaiseTheCushion();
    ADifferentSourceRateNeedsNoGap();
    FlushFadesAndRefills();
    VolumeChangeMakesNoClick();
    if (g_failures == 0) std::printf("AudioOutput: all tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
