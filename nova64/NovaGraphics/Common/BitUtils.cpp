/**
 * @file BitUtils.cpp
 * @brief CRC32/CRC64 implementation (software + SSE4.2 hw-accelerated)
 */

#include "BitUtils.hpp"

#include <array>
#include <cstring>

#ifdef _MSC_VER
    #include <intrin.h>
    #include <nmmintrin.h>
#else
    #include <x86intrin.h>
#endif

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════════════════
//  CRC32 Software Lookup Table (IEEE 802.3 polynomial 0xEDB88320)
// ═══════════════════════════════════════════════════════════════════════════

namespace {

constexpr uint32_t kCRC32Poly = 0xEDB88320u;

struct CRC32Table {
    std::array<uint32_t, 256> tab{};

    constexpr CRC32Table() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) {
                if (c & 1)
                    c = kCRC32Poly ^ (c >> 1);
                else
                    c >>= 1;
            }
            tab[i] = c;
        }
    }
};

constexpr CRC32Table sCRC32Table{};

// Slicing-by-4 table for faster software CRC32
struct CRC32SliceTable {
    std::array<std::array<uint32_t, 256>, 4> tab{};

    constexpr CRC32SliceTable() {
        // First table is the standard table
        for (uint32_t i = 0; i < 256; i++) {
            tab[0][i] = sCRC32Table.tab[i];
        }
        // Remaining tables for slicing
        for (uint32_t i = 0; i < 256; i++) {
            tab[1][i] = (tab[0][i] >> 8) ^ tab[0][tab[0][i] & 0xFF];
            tab[2][i] = (tab[1][i] >> 8) ^ tab[0][tab[1][i] & 0xFF];
            tab[3][i] = (tab[2][i] >> 8) ^ tab[0][tab[2][i] & 0xFF];
        }
    }
};

constexpr CRC32SliceTable sCRC32Slice{};

} // namespace

uint32_t CRC32_Software(const void* data, size_t length, uint32_t seed) noexcept {
    auto* p = static_cast<const uint8_t*>(data);
    uint32_t crc = ~seed;

    // Process 4 bytes at a time (slicing-by-4)
    while (length >= 4) {
        uint32_t word;
        std::memcpy(&word, p, 4);
        crc ^= word;

        crc = sCRC32Slice.tab[3][(crc >>  0) & 0xFF]
            ^ sCRC32Slice.tab[2][(crc >>  8) & 0xFF]
            ^ sCRC32Slice.tab[1][(crc >> 16) & 0xFF]
            ^ sCRC32Slice.tab[0][(crc >> 24) & 0xFF];

        p += 4;
        length -= 4;
    }

    // Process remaining bytes
    while (length--) {
        crc = sCRC32Table.tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }

    return ~crc;
}

// ═══════════════════════════════════════════════════════════════════════════
//  CRC32C Hardware (SSE4.2 intrinsics)
// ═══════════════════════════════════════════════════════════════════════════

#if defined(_MSC_VER) || defined(__SSE4_2__)

static uint32_t CRC32C_Hardware(const void* data, size_t length, uint32_t seed) noexcept {
    auto* p = static_cast<const uint8_t*>(data);
    uint32_t crc = seed;

#ifdef _M_X64
    // Process 8 bytes at a time on 64-bit
    while (length >= 8) {
        uint64_t val;
        std::memcpy(&val, p, 8);
        crc = static_cast<uint32_t>(_mm_crc32_u64(crc, val));
        p += 8;
        length -= 8;
    }
#endif

    // Process 4 bytes at a time
    while (length >= 4) {
        uint32_t val;
        std::memcpy(&val, p, 4);
        crc = _mm_crc32_u32(crc, val);
        p += 4;
        length -= 4;
    }

    // Process remaining bytes
    while (length--) {
        crc = _mm_crc32_u8(crc, *p++);
    }

    return crc;
}

#endif

// ═══════════════════════════════════════════════════════════════════════════
//  CRC32 Fast (auto-selects hw or sw at runtime)
// ═══════════════════════════════════════════════════════════════════════════

static bool HasSSE42CRC() noexcept {
#ifdef _MSC_VER
    int regs[4];
    __cpuid(regs, 1);
    return (regs[2] & (1 << 20)) != 0;  // SSE4.2 bit
#elif defined(__SSE4_2__)
    return true;
#else
    return false;
#endif
}

uint32_t CRC32_Fast(const void* data, size_t length, uint32_t seed) noexcept {
    static const bool sHasHW = HasSSE42CRC();

#if defined(_MSC_VER) || defined(__SSE4_2__)
    if (sHasHW) {
        return CRC32C_Hardware(data, length, seed);
    }
#endif

    return CRC32_Software(data, length, seed);
}

// ═══════════════════════════════════════════════════════════════════════════
//  CRC64 (ECMA-182 polynomial, for shader cache keys)
// ═══════════════════════════════════════════════════════════════════════════

namespace {

constexpr uint64_t kCRC64Poly = 0xC96C5795D7870F42ULL;

struct CRC64Table {
    std::array<uint64_t, 256> tab{};

    constexpr CRC64Table() {
        for (uint64_t i = 0; i < 256; i++) {
            uint64_t c = i;
            for (int j = 0; j < 8; j++) {
                if (c & 1)
                    c = kCRC64Poly ^ (c >> 1);
                else
                    c >>= 1;
            }
            tab[i] = c;
        }
    }
};

constexpr CRC64Table sCRC64Table{};

} // namespace

uint64_t CRC64(const void* data, size_t length, uint64_t seed) noexcept {
    auto* p = static_cast<const uint8_t*>(data);
    uint64_t crc = ~seed;

    while (length--) {
        crc = sCRC64Table.tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }

    return ~crc;
}

} // namespace Nova64::Video
