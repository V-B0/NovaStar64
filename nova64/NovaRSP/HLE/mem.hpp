/**
 * @file mem.hpp
 * @brief RSP HLE Memory Access.
 */
// SuperNova64 - N64 Emulator
// RSP HLE Memory Access - Modernized from Project64's mem.h
// C++20 with safe inline DRAM/DMEM accessors, no macros

#pragma once
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace Nova64::RSP::HLE {

class CHle;

// ──────────────────────────────────────────────
// Byte-swap constants for big-endian access on little-endian host
// ──────────────────────────────────────────────
inline constexpr uint32_t XOR_S8  = 3;  // Byte XOR for 8-bit access
inline constexpr uint32_t XOR_S16 = 2;  // Byte XOR for 16-bit access

// ──────────────────────────────────────────────
// DMEM task table offsets
// ──────────────────────────────────────────────
namespace TaskOffset {
    inline constexpr uint32_t Type          = 0xFC0;
    inline constexpr uint32_t Flags         = 0xFC4;
    inline constexpr uint32_t UcodeBoot     = 0xFC8;
    inline constexpr uint32_t UcodeBootSize = 0xFCC;
    inline constexpr uint32_t Ucode         = 0xFD0;
    inline constexpr uint32_t UcodeSize     = 0xFD4;
    inline constexpr uint32_t UcodeData     = 0xFD8;
    inline constexpr uint32_t UcodeDataSize = 0xFDC;
    inline constexpr uint32_t DramStack     = 0xFE0;
    inline constexpr uint32_t DramStackSize = 0xFE4;
    inline constexpr uint32_t OutputBuff    = 0xFE8;
    inline constexpr uint32_t OutputBuffSize = 0xFEC;
    inline constexpr uint32_t DataPtr       = 0xFF0;
    inline constexpr uint32_t DataSize      = 0xFF4;
    inline constexpr uint32_t YieldDataPtr  = 0xFF8;
    inline constexpr uint32_t YieldDataSize = 0xFFC;
}

// ──────────────────────────────────────────────
// Alignment helper
// ──────────────────────────────────────────────
[[nodiscard]] inline constexpr unsigned int align(unsigned int x, unsigned amount) noexcept {
    --amount;
    return (x + amount) & ~amount;
}

// ──────────────────────────────────────────────
// Buffer element access (big-endian on little-endian host)
// ──────────────────────────────────────────────
[[nodiscard]] inline uint8_t* u8(const uint8_t* buffer, unsigned address) noexcept {
    return const_cast<uint8_t*>(buffer + (address ^ XOR_S8));
}

[[nodiscard]] inline uint16_t* u16(const uint8_t* buffer, unsigned address) noexcept {
    assert((address & 1) == 0);
    return reinterpret_cast<uint16_t*>(const_cast<uint8_t*>(buffer + (address ^ XOR_S16)));
}

[[nodiscard]] inline uint32_t* u32(const uint8_t* buffer, unsigned address) noexcept {
    assert((address & 3) == 0);
    return reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(buffer + address));
}

// ──────────────────────────────────────────────
// Bulk load/store operations
// ──────────────────────────────────────────────
void load_u8(uint8_t* dst, const uint8_t* buffer, unsigned address, size_t count, unsigned mask);
void load_u16(uint16_t* dst, const uint8_t* buffer, unsigned address, size_t count, unsigned mask);
void load_u32(uint32_t* dst, const uint8_t* buffer, unsigned address, size_t count, unsigned mask);
void store_u16(uint8_t* buffer, unsigned address, const uint16_t* src, size_t count, unsigned mask);
void store_u32(uint8_t* buffer, unsigned address, const uint32_t* src, size_t count, unsigned mask);

// ──────────────────────────────────────────────
// DMEM convenience accessors
// ──────────────────────────────────────────────
uint32_t* dmem_u32(CHle* hle, uint16_t address);
void dmem_store_u32(CHle* hle, const uint32_t* src, uint16_t address, size_t count);

// ──────────────────────────────────────────────
// DRAM convenience accessors (masked to 16MB)
// ──────────────────────────────────────────────
uint8_t*  dram_u8(CHle* hle, uint32_t address);
uint16_t* dram_u16(CHle* hle, uint32_t address);
uint32_t* dram_u32(CHle* hle, uint32_t address);

void dram_load_u8(CHle* hle, uint8_t* dst, uint32_t address, size_t count);
void dram_load_u16(CHle* hle, uint16_t* dst, uint32_t address, size_t count);
void dram_load_u32(CHle* hle, uint32_t* dst, uint32_t address, size_t count);
void dram_store_u16(CHle* hle, const uint16_t* src, uint32_t address, size_t count);
void dram_store_u32(CHle* hle, const uint32_t* src, uint32_t address, size_t count);

} // namespace Nova64::RSP::HLE
