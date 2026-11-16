/**
 * @file RspClamp.hpp
 * @brief RSP Clamping Utilities.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#pragma once
#include <cstdint>
#include <algorithm>
#include <limits>

namespace Nova64::RSP {

// --- Clamp a 32-bit value to signed 16-bit range ---
[[nodiscard]] inline constexpr uint16_t Clamp16(int32_t value) noexcept {
    if (value < INT16_MIN) return static_cast<uint16_t>(INT16_MIN);
    if (value > INT16_MAX) return static_cast<uint16_t>(INT16_MAX);
    return static_cast<uint16_t>(value);
}

// Clip a 64-bit unsigned value to 48-bit signed range
// Used for accumulator overflow detection
[[nodiscard]] inline constexpr int64_t Clip48(uint64_t value) noexcept {
    // Sign-extend from 48 bits
    int64_t result = static_cast<int64_t>(value);
    result = (result << 16) >> 16;
    return result;
}

// --- Saturate to signed 16-bit (for HLE audio) ---
[[nodiscard]] inline constexpr int16_t ClampS16(int32_t x) noexcept {
    return static_cast<int16_t>(std::clamp(x, static_cast<int32_t>(INT16_MIN), static_cast<int32_t>(INT16_MAX)));
}

// --- Saturate to unsigned 16-bit ---
[[nodiscard]] inline constexpr uint16_t ClampU16(int32_t x) noexcept {
    if (x < 0) return 0;
    if (x > UINT16_MAX) return UINT16_MAX;
    return static_cast<uint16_t>(x);
}

// --- HLE vmulf helper (signed fractional multiply) ---
[[nodiscard]] inline constexpr int32_t VMulF(int16_t x, int16_t y) noexcept {
    return (static_cast<int32_t>(x) * static_cast<int32_t>(y) + 0x4000) >> 15;
}

} // namespace Nova64::RSP
