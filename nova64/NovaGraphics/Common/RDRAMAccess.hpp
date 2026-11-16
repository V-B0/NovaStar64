/**
 * @file RDRAMAccess.hpp
 * @brief Word-swapped RDRAM access helpers for PJ64-style storage
 *
 * The emulator stores RDRAM in 32-bit native word order:
 *   - 32-bit reads: direct native read at address
 *   - 16-bit reads: address XOR 2 (byte swap within 32-bit word)
 *   - 8-bit reads:  address XOR 3
 */

#pragma once

#include <cstdint>

namespace Nova64::Video {

inline uint32_t RDRAMReadWord(const uint8_t* rdram, uint32_t addr) {
    return *reinterpret_cast<const uint32_t*>(rdram + addr);
}

inline uint16_t RDRAMReadHalf(const uint8_t* rdram, uint32_t addr) {
    return *reinterpret_cast<const uint16_t*>(rdram + (addr ^ 2));
}

inline int16_t RDRAMReadHalfSigned(const uint8_t* rdram, uint32_t addr) {
    return *reinterpret_cast<const int16_t*>(rdram + (addr ^ 2));
}

inline uint8_t RDRAMReadByte(const uint8_t* rdram, uint32_t addr) { return rdram[addr ^ 3]; }

inline void RDRAMWriteByte(uint8_t* rdram, uint32_t addr, uint8_t val) { rdram[addr ^ 3] = val; }

inline void RDRAMWriteHalf(uint8_t* rdram, uint32_t addr, uint16_t val) {
    *reinterpret_cast<uint16_t*>(rdram + (addr ^ 2)) = val;
}

} // namespace Nova64::Video
