/**
 * @file arithmetics.cpp
 * @brief RSP HLE Audio Arithmetics.
 *
 * Derived from Project64 (https://github.com/project64/project64), whose RSP HLE code in turn
 * comes from mupen64plus-rsp-hle (https://github.com/mupen64plus/mupen64plus-rsp-hle),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 *
 * Copyright(C) 2001-2021 Project64
 * Copyright(C) 2014 Bobby Smiles
 */

#include "arithmetics.hpp"
#include <type_traits>

namespace Nova64::RSP::HLE {

// ──────────────────────────────────────────────
// Static assertions validating constexpr helpers
// ──────────────────────────────────────────────

// clamp_s16 boundary tests
static_assert(clamp_s16(0) == 0);
static_assert(clamp_s16(32767) == 32767);
static_assert(clamp_s16(-32768) == -32768);
static_assert(clamp_s16(40000) == 32767);
static_assert(clamp_s16(-40000) == -32768);
static_assert(clamp_s16(1) == 1);
static_assert(clamp_s16(-1) == -1);

// vmulf correctness (fractional multiply with round)
static_assert(vmulf(0, 0) == 0);
static_assert(vmulf(0x7FFF, 0x7FFF) == 0x7FFE);  // Near-max positive
static_assert(vmulf(1, 1) == 0);                   // Below rounding threshold
static_assert(vmulf(0x4000, 2) == 1);              // 0.5 * 2/32768 ≈ round up
static_assert(vmulf(-1, 1) == 0);                   // Tiny negative

// Type traits
static_assert(std::is_same_v<decltype(clamp_s16(0)), int16_t>);
static_assert(std::is_same_v<decltype(vmulf(0, 0)), int32_t>);
static_assert(noexcept(clamp_s16(0)));
static_assert(noexcept(vmulf(0, 0)));

} // namespace Nova64::RSP::HLE
