// SuperNova64 - N64 Emulator
// CRC32 utility for microcode identification
// Standard CRC32 (ISO 3309 / ITU-T V.42) with lookup table

#pragma once
#include <cstdint>
#include <cstddef>
#include <array>

namespace Nova64::RSP::HLE {

// ──────────────────────────────────────────────
// Compile-time CRC32 lookup table generation
// ──────────────────────────────────────────────

namespace detail {

consteval std::array<uint32_t, 256> GenerateCrc32Table() noexcept
{
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i)
    {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320u;
            else
                crc >>= 1;
        }
        table[i] = crc;
    }
    return table;
}

inline constexpr auto kCrc32Table = GenerateCrc32Table();

} // namespace detail

// ──────────────────────────────────────────────
// CRC32 computation
// ──────────────────────────────────────────────

[[nodiscard]] inline uint32_t Crc32(const uint8_t* data, size_t length) noexcept
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i)
    {
        crc = detail::kCrc32Table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

[[nodiscard]] inline uint32_t Crc32(const uint8_t* data, size_t length, uint32_t seed) noexcept
{
    uint32_t crc = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i)
    {
        crc = detail::kCrc32Table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// Legacy additive checksum (for backward compatibility with boot ROM detection)
[[nodiscard]] inline uint32_t AdditiveChecksum(const uint32_t* data, size_t count) noexcept
{
    uint32_t sum = 0;
    for (size_t i = 0; i < count; ++i)
    {
        sum += data[i];
    }
    return sum;
}

} // namespace Nova64::RSP::HLE
