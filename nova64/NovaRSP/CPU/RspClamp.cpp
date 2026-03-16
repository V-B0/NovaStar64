// SuperNova64 - N64 Emulator
// RSP Clamp Utilities - Implementation
// All core functions are inline constexpr in the header.
// This TU exists for:
//  1. Explicit template instantiation (future SIMD paths).
//  2. Static assertions validating clamp behaviors at compile time.

#include "RspClamp.hpp"
#include <cassert>
#include <climits>

namespace Nova64::RSP {

// ── Compile-time validation ─────────────────────────────────────
static_assert(Clamp16(INT16_MIN)     == static_cast<uint16_t>(INT16_MIN));
static_assert(Clamp16(INT16_MAX)     == static_cast<uint16_t>(INT16_MAX));
static_assert(Clamp16(INT16_MIN - 1) == static_cast<uint16_t>(INT16_MIN));
static_assert(Clamp16(INT16_MAX + 1) == static_cast<uint16_t>(INT16_MAX));
static_assert(Clamp16(0)             == 0);
static_assert(Clamp16(-1)            == static_cast<uint16_t>(0xFFFF));

static_assert(Clip48(0)               == 0);
static_assert(Clip48(0x7FFFFFFFFFFF)  == 0x7FFFFFFFFFFF);
static_assert(Clip48(0x800000000000)  == -0x800000000000LL);

static_assert(ClampS16(100000)  == INT16_MAX);
static_assert(ClampS16(-100000) == INT16_MIN);
static_assert(ClampS16(42)      == 42);

static_assert(ClampU16(-1)     == 0);
static_assert(ClampU16(100000) == UINT16_MAX);
static_assert(ClampU16(42)     == 42);

// 0x4000 * 0x4000 = 0x10000000, +0x4000 = 0x10004000, >>15 = 0x2000
static_assert(VMulF(0x4000, 0x4000) == 0x2000);

} // namespace Nova64::RSP
