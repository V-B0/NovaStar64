/**
 * @file BitUtils.hpp
 * @brief Bit manipulation, CRC computation, and endian conversion utilities
 *
 * Provides CRC32 (software fallback + SSE4.2 intrinsic), byte-swap helpers,
 * bit counting, alignment, and fixed-point conversion needed by texture cache,
 * microcode detection, and RDP command parsing.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace Nova64::Video {

// --- Endian Swap ---

/// Byte-swap a 16-bit value (big to little endian or back).
inline uint16_t ByteSwap16(uint16_t x) noexcept {
#ifdef _MSC_VER
    return _byteswap_ushort(x);
#else
    return __builtin_bswap16(x);
#endif
}

inline uint32_t ByteSwap32(uint32_t x) noexcept {
#ifdef _MSC_VER
    return _byteswap_ulong(x);
#else
    return __builtin_bswap32(x);
#endif
}

inline uint64_t ByteSwap64(uint64_t x) noexcept {
#ifdef _MSC_VER
    return _byteswap_uint64(x);
#else
    return __builtin_bswap64(x);
#endif
}

// The N64 is big-endian and the supported hosts are little-endian; these name the
// intent of a byte swap.
inline uint16_t FromBE16(uint16_t x) noexcept { return ByteSwap16(x); }
inline uint32_t FromBE32(uint32_t x) noexcept { return ByteSwap32(x); }
inline uint64_t FromBE64(uint64_t x) noexcept { return ByteSwap64(x); }
inline uint16_t ToBE16(uint16_t x) noexcept { return ByteSwap16(x); }
inline uint32_t ToBE32(uint32_t x) noexcept { return ByteSwap32(x); }
inline uint64_t ToBE64(uint64_t x) noexcept { return ByteSwap64(x); }

// --- Bit Counting ---

inline int PopCount32(uint32_t x) noexcept {
#ifdef _MSC_VER
    return static_cast<int>(__popcnt(x));
#else
    return __builtin_popcount(x);
#endif
}

inline int PopCount64(uint64_t x) noexcept {
#ifdef _MSC_VER
    return static_cast<int>(__popcnt64(x));
#else
    return __builtin_popcountll(x);
#endif
}

inline int CountLeadingZeros32(uint32_t x) noexcept {
    if (x == 0) return 32;
#ifdef _MSC_VER
    unsigned long idx;
    _BitScanReverse(&idx, x);
    return 31 - static_cast<int>(idx);
#else
    return __builtin_clz(x);
#endif
}

inline int CountTrailingZeros32(uint32_t x) noexcept {
    if (x == 0) return 32;
#ifdef _MSC_VER
    unsigned long idx;
    _BitScanForward(&idx, x);
    return static_cast<int>(idx);
#else
    return __builtin_ctz(x);
#endif
}

// --- Alignment & Power-of-Two ---

constexpr bool IsPowerOfTwo(uint32_t x) noexcept { return x != 0 && (x & (x - 1)) == 0; }

constexpr uint32_t NextPowerOfTwo(uint32_t x) noexcept {
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return x + 1;
}

constexpr uint32_t AlignUp(uint32_t value, uint32_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr uint32_t AlignDown(uint32_t value, uint32_t alignment) noexcept { return value & ~(alignment - 1); }

constexpr uint64_t AlignUp64(uint64_t value, uint64_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

// --- Bit Field Extraction (N64 RDP commands pack fields into 64-bit words) ---

/// Extract `count` bits starting at bit `startBit` from a 32-bit word
constexpr uint32_t ExtractBits(uint32_t word, int startBit, int count) noexcept {
    return (word >> startBit) & ((1u << count) - 1);
}

constexpr uint64_t ExtractBits64(uint64_t word, int startBit, int count) noexcept {
    return (word >> startBit) & ((1ULL << count) - 1);
}

/// Sign-extend a value of `bits` width to int32_t
constexpr int32_t SignExtend(uint32_t value, int bits) noexcept {
    int32_t shift = 32 - bits;
    return static_cast<int32_t>(value << shift) >> shift;
}

// --- Fixed-Point Conversion (N64 uses various s.f formats) ---

/// Convert 16.16 fixed-point to float
constexpr float Fixed16_16ToFloat(int32_t x) noexcept { return static_cast<float>(x) / 65536.0f; }

/// Convert s15.16 fixed-point to float
constexpr float FixedS15_16ToFloat(int32_t x) noexcept { return static_cast<float>(x) / 65536.0f; }

/// Convert s10.5 fixed-point to float
constexpr float FixedS10_5ToFloat(int16_t x) noexcept { return static_cast<float>(x) / 32.0f; }

/// Convert s10.2 fixed-point to float
constexpr float FixedS10_2ToFloat(int16_t x) noexcept { return static_cast<float>(x) / 4.0f; }

/// Convert float to 16.16 fixed-point
constexpr int32_t FloatToFixed16_16(float f) noexcept { return static_cast<int32_t>(f * 65536.0f); }

/// Convert float to s10.5
constexpr int16_t FloatToFixedS10_5(float f) noexcept { return static_cast<int16_t>(f * 32.0f); }

// --- CRC32 Computation ---

/// IEEE 802.3 CRC32, slicing-by-4 with tables built at compile time. `seed` is the
/// CRC of the preceding data, so calls can be chained.
uint32_t CRC32_Software(const void* data, size_t length, uint32_t seed = 0) noexcept;

/// CRC32 using the SSE4.2 CRC32C instruction when the build has it, otherwise
/// CRC32_Software. The two paths compute different checksums (Castagnoli vs IEEE
/// polynomial), so values must not be compared or stored across builds.
uint32_t CRC32_Fast(const void* data, size_t length, uint32_t seed = 0) noexcept;

/// CRC64 (ECMA-182) for hashing larger data, such as shader cache keys.
uint64_t CRC64(const void* data, size_t length, uint64_t seed = 0) noexcept;

// --- Tile/TMEM Address Utilities ---

/// Convert N64 tile line size to bytes (N64 stores texture data in 64-bit words)
constexpr uint32_t TileLineBytes(uint32_t lineWords) noexcept { return lineWords * 8; }

/// TMEM address `offset` bytes past `tmemBase`, wrapped to the 4 KB TMEM.
constexpr uint32_t TMEMAddress(uint32_t tmemBase, uint32_t offset) noexcept {
    return (tmemBase + offset) & 0xFFF; // TMEM is 4K (0x1000 bytes)
}

/// Place `addr` in the low half of TMEM for even tiles, the high half for odd tiles.
constexpr uint32_t TMEMInterleave(uint32_t addr, int tile) noexcept {
    if (tile & 1) return addr | 0x800;
    return addr & 0x7FF;
}

} // namespace Nova64::Video
