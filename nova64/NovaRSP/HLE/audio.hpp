/**
 * @file audio.hpp
 * @brief RSP HLE Audio ADPCM/Resample.
 */
// SuperNova64 - N64 Emulator
// RSP HLE Audio ADPCM/Resample - Modernized from Project64's audio.h
// C++20 with constexpr LUT and inline helpers

#pragma once
#include <cstdint>
#include <cstddef>

namespace Nova64::RSP::HLE {

// ──────────────────────────────────────────────
// Resampling lookup table (64 × 4 entries)
// ──────────────────────────────────────────────
extern const int16_t RESAMPLE_LUT[64 * 4];

// ──────────────────────────────────────────────
// Dot product for audio filtering
// ──────────────────────────────────────────────
[[nodiscard]] int32_t rdot(size_t n, const int16_t* x, const int16_t* y);

// ──────────────────────────────────────────────
// ADPCM single sample prediction
// ──────────────────────────────────────────────
[[nodiscard]] inline int16_t adpcm_predict_sample(
    uint8_t byte, uint8_t mask, unsigned lshift, unsigned rshift) noexcept
{
    int16_t sample = static_cast<uint16_t>(byte & mask) << lshift;
    sample >>= rshift; // Signed shift for sign extension
    return sample;
}

// ──────────────────────────────────────────────
// Compute ADPCM residuals from codebook entry
// ──────────────────────────────────────────────
void adpcm_compute_residuals(
    int16_t* dst,
    const int16_t* src,
    const int16_t* cb_entry,
    const int16_t* last_samples,
    size_t count);

} // namespace Nova64::RSP::HLE
