/**
 * @file mem.cpp
 * @brief RSP HLE Memory Access Implementation.
 *
 * Derived from Project64 (https://github.com/project64/project64), whose RSP HLE code in turn
 * comes from mupen64plus-rsp-hle (https://github.com/mupen64plus/mupen64plus-rsp-hle),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 *
 * Copyright(C) 2001-2021 Project64
 * Copyright(C) 2012 Bobby Smiles
 */

#include "mem.hpp"
#include "hle.hpp"
#include <cstring>

namespace Nova64::RSP::HLE {

// ──────────────────────────────────────────────
// DRAM size mask (8 MB)
// ──────────────────────────────────────────────
static constexpr uint32_t DRAM_MASK = 0x007FFFFF;

// ──────────────────────────────────────────────
// Bulk load: copy from big-endian buffer into
// host-native array, with byte swapping
// ──────────────────────────────────────────────

void load_u8(uint8_t* dst, const uint8_t* buffer, unsigned address, size_t count, unsigned mask) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = *u8(buffer, (address + static_cast<unsigned>(i)) & mask);
    }
}

void load_u16(uint16_t* dst, const uint8_t* buffer, unsigned address, size_t count, unsigned mask) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = *u16(buffer, (address + static_cast<unsigned>(i * 2)) & mask);
    }
}

void load_u32(uint32_t* dst, const uint8_t* buffer, unsigned address, size_t count, unsigned mask) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = *u32(buffer, (address + static_cast<unsigned>(i * 4)) & mask);
    }
}

void store_u16(uint8_t* buffer, unsigned address, const uint16_t* src, size_t count, unsigned mask) {
    for (size_t i = 0; i < count; i++) {
        *u16(buffer, (address + static_cast<unsigned>(i * 2)) & mask) = src[i];
    }
}

void store_u32(uint8_t* buffer, unsigned address, const uint32_t* src, size_t count, unsigned mask) {
    for (size_t i = 0; i < count; i++) {
        *u32(buffer, (address + static_cast<unsigned>(i * 4)) & mask) = src[i];
    }
}

// ──────────────────────────────────────────────
// DMEM convenience accessors
// ──────────────────────────────────────────────

uint32_t* dmem_u32(CHle* hle, uint16_t address) { return u32(hle->dmem(), address & 0xFFC); }

void dmem_store_u32(CHle* hle, const uint32_t* src, uint16_t address, size_t count) {
    store_u32(hle->dmem(), address & 0xFFC, src, count, 0xFFF);
}

// ──────────────────────────────────────────────
// DRAM convenience accessors (masked to 8 MB)
// ──────────────────────────────────────────────

uint8_t* dram_u8(CHle* hle, uint32_t address) { return u8(hle->dram(), address & DRAM_MASK); }

uint16_t* dram_u16(CHle* hle, uint32_t address) { return u16(hle->dram(), (address & DRAM_MASK) & ~1u); }

uint32_t* dram_u32(CHle* hle, uint32_t address) { return u32(hle->dram(), (address & DRAM_MASK) & ~3u); }

void dram_load_u8(CHle* hle, uint8_t* dst, uint32_t address, size_t count) {
    load_u8(dst, hle->dram(), address & DRAM_MASK, count, DRAM_MASK);
}

void dram_load_u16(CHle* hle, uint16_t* dst, uint32_t address, size_t count) {
    load_u16(dst, hle->dram(), (address & DRAM_MASK) & ~1u, count, DRAM_MASK);
}

void dram_load_u32(CHle* hle, uint32_t* dst, uint32_t address, size_t count) {
    load_u32(dst, hle->dram(), (address & DRAM_MASK) & ~3u, count, DRAM_MASK);
}

void dram_store_u16(CHle* hle, const uint16_t* src, uint32_t address, size_t count) {
    store_u16(hle->dram(), (address & DRAM_MASK) & ~1u, src, count, DRAM_MASK);
}

void dram_store_u32(CHle* hle, const uint32_t* src, uint32_t address, size_t count) {
    store_u32(hle->dram(), (address & DRAM_MASK) & ~3u, src, count, DRAM_MASK);
}

} // namespace Nova64::RSP::HLE
