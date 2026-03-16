// SuperNova64 - N64 Emulator
// RSP HLE Audio Arithmetics - Modernized from Project64's arithmetics.h
// C++20 constexpr helper functions

#pragma once
#include <cstdint>
#include <limits>

namespace Nova64::RSP::HLE {

// ──────────────────────────────────────────────
// Clamp to signed 16-bit range
// ──────────────────────────────────────────────
[[nodiscard]] inline constexpr int16_t clamp_s16(int32_t x) noexcept {
    if (x < INT16_MIN) return INT16_MIN;
    if (x > INT16_MAX) return INT16_MAX;
    return static_cast<int16_t>(x);
}

// ──────────────────────────────────────────────
// Signed fractional multiply (used by VMULF HLE)
// ──────────────────────────────────────────────
[[nodiscard]] inline constexpr int32_t vmulf(int16_t x, int16_t y) noexcept {
    return (static_cast<int32_t>(x) * static_cast<int32_t>(y) + 0x4000) >> 15;
}

} // namespace Nova64::RSP::HLE
