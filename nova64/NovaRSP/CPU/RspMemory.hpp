/**
 * @file RspMemory.hpp
 * @brief RSP Memory.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#pragma once
#include "RspTypes.hpp"
#include <cstdint>

namespace Nova64::RSP {

// ──────────────────────────────────────────────
// Memory constants
// ──────────────────────────────────────────────
inline constexpr uint32_t IMEM_SIZE = 0x1000;  // 4 KB
inline constexpr uint32_t DMEM_SIZE = 0x1000;  // 4 KB
inline constexpr uint32_t IMEM_MASK = 0x0FFF;
inline constexpr uint32_t DMEM_MASK = 0x0FFF;
inline constexpr uint32_t MaxMaps   = 32;

// ──────────────────────────────────────────────
// Recompiler code buffer management
// ──────────────────────────────────────────────
struct RecompilerMemory {
    uint8_t*  Code{nullptr};           // Primary code buffer
    uint8_t*  CodeSecondary{nullptr};  // Secondary code buffer
    uint8_t*  Pos{nullptr};            // Current write position
    void**    JumpTable{nullptr};      // PC → compiled code mapping
    uint32_t  Table{0};
};

// ──────────────────────────────────────────────
// Memory allocation/deallocation
// ──────────────────────────────────────────────
bool AllocateRSPMemory(RecompilerMemory& mem);
void FreeRSPMemory(RecompilerMemory& mem);

// ──────────────────────────────────────────────
// Safe IMEM read (instruction fetch)
// ──────────────────────────────────────────────
inline void RSP_LW_IMEM(const uint8_t* imem, uint32_t addr, uint32_t* value) {
    addr &= IMEM_MASK;
    *value = static_cast<uint32_t>(imem[addr + 0]) << 24
           | static_cast<uint32_t>(imem[addr + 1]) << 16
           | static_cast<uint32_t>(imem[addr + 2]) << 8
           | static_cast<uint32_t>(imem[addr + 3]);
}

// ──────────────────────────────────────────────
// Safe DMEM read helpers (big-endian)
// ──────────────────────────────────────────────
inline uint32_t RSP_LW_DMEM(const uint8_t* dmem, uint32_t addr) {
    addr &= DMEM_MASK;
    return static_cast<uint32_t>(dmem[addr + 0]) << 24
         | static_cast<uint32_t>(dmem[addr + 1]) << 16
         | static_cast<uint32_t>(dmem[addr + 2]) << 8
         | static_cast<uint32_t>(dmem[addr + 3]);
}

inline uint16_t RSP_LH_DMEM(const uint8_t* dmem, uint32_t addr) {
    addr &= DMEM_MASK;
    return static_cast<uint16_t>(dmem[addr + 0]) << 8
         | static_cast<uint16_t>(dmem[addr + 1]);
}

inline uint8_t RSP_LB_DMEM(const uint8_t* dmem, uint32_t addr) {
    return dmem[addr & DMEM_MASK];
}

// ──────────────────────────────────────────────
// Safe DMEM write helpers (big-endian)
// ──────────────────────────────────────────────
inline void RSP_SW_DMEM(uint8_t* dmem, uint32_t addr, uint32_t value) {
    addr &= DMEM_MASK;
    dmem[addr + 0] = static_cast<uint8_t>(value >> 24);
    dmem[addr + 1] = static_cast<uint8_t>(value >> 16);
    dmem[addr + 2] = static_cast<uint8_t>(value >> 8);
    dmem[addr + 3] = static_cast<uint8_t>(value);
}

inline void RSP_SH_DMEM(uint8_t* dmem, uint32_t addr, uint16_t value) {
    addr &= DMEM_MASK;
    dmem[addr + 0] = static_cast<uint8_t>(value >> 8);
    dmem[addr + 1] = static_cast<uint8_t>(value);
}

inline void RSP_SB_DMEM(uint8_t* dmem, uint32_t addr, uint8_t value) {
    dmem[addr & DMEM_MASK] = value;
}

} // namespace Nova64::RSP
