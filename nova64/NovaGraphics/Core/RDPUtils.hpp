/**
 * @file RDPUtils.hpp
 * @brief Utility functions for RDP command decoding, address translation,
 *        format conversions, and fixed-point arithmetic.
 *
 * These are pure-function helpers used throughout the RDP pipeline to decode
 * hardware-specific formats, translate segmented addresses, and perform
 * the fixed-point conversions that the N64 RDP uses internally.
 */

#pragma once

#include "N64Types.hpp"
#include "../Common/BitUtils.hpp"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string>

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════════════════
//  Endian / Byte-Swap Utilities
// ═══════════════════════════════════════════════════════════════════════════

/// Read a big-endian 32-bit word from RDRAM
inline uint32_t ReadBE32(const uint8_t* ptr) {
    return (static_cast<uint32_t>(ptr[0]) << 24) |
           (static_cast<uint32_t>(ptr[1]) << 16) |
           (static_cast<uint32_t>(ptr[2]) << 8)  |
            static_cast<uint32_t>(ptr[3]);
}

/// Read a big-endian 16-bit halfword from RDRAM
inline uint16_t ReadBE16(const uint8_t* ptr) {
    return (static_cast<uint16_t>(ptr[0]) << 8) | static_cast<uint16_t>(ptr[1]);
}

/// Read a big-endian 64-bit doubleword from RDRAM
inline uint64_t ReadBE64(const uint8_t* ptr) {
    return (static_cast<uint64_t>(ReadBE32(ptr)) << 32) | ReadBE32(ptr + 4);
}

/// Write a big-endian 32-bit word to RDRAM
inline void WriteBE32(uint8_t* ptr, uint32_t val) {
    ptr[0] = static_cast<uint8_t>(val >> 24);
    ptr[1] = static_cast<uint8_t>(val >> 16);
    ptr[2] = static_cast<uint8_t>(val >> 8);
    ptr[3] = static_cast<uint8_t>(val);
}

/// Write a big-endian 16-bit halfword to RDRAM
inline void WriteBE16(uint8_t* ptr, uint16_t val) {
    ptr[0] = static_cast<uint8_t>(val >> 8);
    ptr[1] = static_cast<uint8_t>(val);
}

// ByteSwap32 / ByteSwap16 provided by BitUtils.hpp

// ═══════════════════════════════════════════════════════════════════════════
//  Segment Address Translation
// ═══════════════════════════════════════════════════════════════════════════

/// Resolve a segmented address using the segment table
/// Segment = bits [27:24], offset = bits [23:0]
inline uint32_t SegmentResolve(uint32_t segAddr, const uint32_t segments[16]) {
    uint8_t seg = (segAddr >> 24) & 0x0F;
    return (segments[seg] + (segAddr & 0x00FFFFFF)) & 0x007FFFFF; // Mask to 8MB RDRAM
}

/// Check if an address is a valid RDRAM address
inline bool IsValidRDRAMAddr(uint32_t addr, size_t rdramSize = RDRAM_SIZE) {
    return addr < rdramSize;
}

/// Check if an address range is within RDRAM bounds
inline bool IsValidRDRAMRange(uint32_t addr, uint32_t bytes, size_t rdramSize = RDRAM_SIZE) {
    return addr < rdramSize && (addr + bytes) <= rdramSize;
}

/// Physical address to RDRAM offset (mask out high bits)
inline uint32_t PhysicalToRDRAM(uint32_t addr) {
    return addr & 0x007FFFFF;
}

/// Check if address is in SP DMEM range
inline bool IsDMEMAddr(uint32_t addr) {
    return (addr >= SP_DMEM_BASE) && (addr < SP_DMEM_BASE + SP_DMEM_SIZE);
}

/// Check if address is in SP IMEM range
inline bool IsIMEMAddr(uint32_t addr) {
    return (addr >= SP_IMEM_BASE) && (addr < SP_IMEM_BASE + SP_IMEM_SIZE);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Fixed-Point Conversion (N64 hardware uses various fixed-point formats)
// ═══════════════════════════════════════════════════════════════════════════

/// Convert s10.2 fixed-point to float (used for scissor/rect coordinates)
inline float FixedS10_2ToFloat(int32_t raw) {
    return static_cast<float>(raw) / 4.0f;
}

/// Convert 10.2 unsigned fixed-point to float
inline float FixedU10_2ToFloat(uint32_t raw) {
    return static_cast<float>(raw) / 4.0f;
}

// FixedS15_16ToFloat / FixedS10_5ToFloat provided by BitUtils.hpp

/// Convert s5.10 fixed-point to float (used for texture scale)
inline float FixedS5_10ToFloat(int16_t raw) {
    return static_cast<float>(raw) / 1024.0f;
}

/// Convert 0.16 unsigned fixed-point to float [0, 1)
inline float FixedU0_16ToFloat(uint16_t raw) {
    return static_cast<float>(raw) / 65536.0f;
}

/// Convert 0.8 unsigned fixed-point to float [0, 1]
inline float FixedU0_8ToFloat(uint8_t raw) {
    return static_cast<float>(raw) / 255.0f;
}

/// Convert s0.15 fixed-point to float [-1, 1)
inline float FixedS0_15ToFloat(int16_t raw) {
    return static_cast<float>(raw) / 32768.0f;
}

/// Convert float to s15.16 fixed-point
inline int32_t FloatToFixedS15_16(float f) {
    return static_cast<int32_t>(f * 65536.0f);
}

// FloatToFixedS10_5 provided by BitUtils.hpp

/// Convert float [0,1] to 0.8 unsigned fixed-point
inline uint8_t FloatToFixedU0_8(float f) {
    return static_cast<uint8_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Color Packing / Unpacking
// ═══════════════════════════════════════════════════════════════════════════

/// Unpack RGBA5551 (16-bit) to r, g, b, a (8-bit each)
inline void UnpackRGBA5551(uint16_t packed, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    r = static_cast<uint8_t>(((packed >> 11) & 0x1F) << 3);
    g = static_cast<uint8_t>(((packed >> 6)  & 0x1F) << 3);
    b = static_cast<uint8_t>(((packed >> 1)  & 0x1F) << 3);
    a = (packed & 1) ? 0xFF : 0x00;
    // Replicate high bits into low bits for better precision
    r |= (r >> 5);
    g |= (g >> 5);
    b |= (b >> 5);
}

/// Pack RGBA8888 to RGBA5551
inline uint16_t PackRGBA5551(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return static_cast<uint16_t>(
        ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7)
    );
}

/// Unpack IA16 (8-bit intensity + 8-bit alpha)
inline void UnpackIA16(uint16_t packed, uint8_t& i, uint8_t& a) {
    i = static_cast<uint8_t>(packed >> 8);
    a = static_cast<uint8_t>(packed);
}

/// Unpack IA8 (4-bit intensity + 4-bit alpha)
inline void UnpackIA8(uint8_t packed, uint8_t& i, uint8_t& a) {
    i = (packed >> 4) | ((packed >> 4) << 4); // Replicate high nibble
    a = (packed & 0x0F) | ((packed & 0x0F) << 4);
}

/// Unpack IA4 (3-bit intensity + 1-bit alpha)
inline void UnpackIA4(uint8_t nibble, uint8_t& i, uint8_t& a) {
    uint8_t intensity3 = (nibble >> 1) & 0x07;
    i = static_cast<uint8_t>((intensity3 << 5) | (intensity3 << 2) | (intensity3 >> 1));
    a = (nibble & 1) ? 0xFF : 0x00;
}

/// Unpack I8 (8-bit intensity → replicated to all channels)
inline void UnpackI8(uint8_t packed, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    r = g = b = a = packed;
}

/// Unpack I4 (4-bit intensity → replicated)
inline void UnpackI4(uint8_t nibble, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    uint8_t v = nibble | (nibble << 4);
    r = g = b = a = v;
}

/// Decode an N64 fill color from the 32-bit register
/// For 16-bit modes, it contains two RGBA5551 pixels; for 32-bit, one RGBA8888
inline void DecodeFillColor(uint32_t raw, ImageSize size, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    if (size == ImageSize::Bpp32) {
        r = (raw >> 24) & 0xFF;
        g = (raw >> 16) & 0xFF;
        b = (raw >> 8)  & 0xFF;
        a =  raw        & 0xFF;
    } else {
        // 16-bit: take upper 16 bits as RGBA5551
        UnpackRGBA5551(static_cast<uint16_t>(raw >> 16), r, g, b, a);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Texture Coordinate Utilities
// ═══════════════════════════════════════════════════════════════════════════

/// Apply texture coordinate wrapping (mirror / clamp / mask)
inline float WrapTexCoord(float coord, int mask, bool mirror, bool clamp, float maxCoord) {
    if (mask == 0) {
        // No mask means clamp
        return std::clamp(coord, 0.0f, maxCoord);
    }

    int maskBits = (1 << mask) - 1;
    int intCoord = static_cast<int>(coord);

    if (mirror) {
        // Mirror: if odd period, flip
        if ((intCoord >> mask) & 1) {
            intCoord = maskBits - (intCoord & maskBits);
        } else {
            intCoord = intCoord & maskBits;
        }
    } else {
        intCoord = intCoord & maskBits;
    }

    if (clamp) {
        intCoord = std::clamp(intCoord, 0, static_cast<int>(maxCoord));
    }

    return static_cast<float>(intCoord);
}

/// Apply texture coordinate shift (hardware shift field)
/// Shift 0-10 is >> shift, shift 11-15 is << (16 - shift)
inline float ApplyTexShift(float coord, int shift) {
    if (shift == 0) return coord;
    if (shift <= 10) {
        return coord / static_cast<float>(1 << shift);
    } else {
        return coord * static_cast<float>(1 << (16 - shift));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  TMEM Address Calculations
// ═══════════════════════════════════════════════════════════════════════════

/// Calculate TMEM byte address from word address
inline uint32_t TMEMWordToByteAddr(uint16_t tmemWord) {
    return static_cast<uint32_t>(tmemWord) * 8;
}

/// Calculate texel address in TMEM for a given (s, t) coordinate and tile
inline uint32_t CalcTMEMTexelAddr(int s, int t, const TileDescriptor& tile) {
    int bpp = GetBitsPerPixel(static_cast<ImageSize>(tile.size));
    int lineBytes = tile.line * 8;
    uint32_t base = TMEMWordToByteAddr(tile.tmemAddr);

    int byteOffset;
    if (bpp == 4) {
        byteOffset = t * lineBytes + (s >> 1);
    } else {
        byteOffset = t * lineBytes + s * (bpp / 8);
    }

    return base + static_cast<uint32_t>(byteOffset);
}

/// Look up a 4-bit texel from TMEM (returns value 0-15)
inline uint8_t ReadTMEMTexel4(const uint8_t* tmem, uint32_t addr, int s) {
    uint8_t byte = tmem[addr & (TMEM_SIZE - 1)];
    return (s & 1) ? (byte & 0x0F) : (byte >> 4);
}

/// Look up an 8-bit texel from TMEM
inline uint8_t ReadTMEMTexel8(const uint8_t* tmem, uint32_t addr) {
    return tmem[addr & (TMEM_SIZE - 1)];
}

/// Look up a 16-bit texel from TMEM
inline uint16_t ReadTMEMTexel16(const uint8_t* tmem, uint32_t addr) {
    addr &= (TMEM_SIZE - 2);
    return (static_cast<uint16_t>(tmem[addr]) << 8) | tmem[addr + 1];
}

/// Look up a 32-bit texel from TMEM
inline uint32_t ReadTMEMTexel32(const uint8_t* tmem, uint32_t addr) {
    addr &= (TMEM_SIZE - 4);
    return ReadBE32(tmem + addr);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Palette (TLUT) Lookup
// ═══════════════════════════════════════════════════════════════════════════

/// Look up a CI4 color from the palette area of TMEM
inline uint16_t LookupTLUT_CI4(const uint8_t* tmem, uint8_t index, uint8_t palette) {
    // TLUT for CI4: 16 entries per palette, stored at TMEM offset 0x800 + palette * 0x80
    uint32_t addr = TMEM_PALETTE_BASE + static_cast<uint32_t>(palette) * 0x80 + static_cast<uint32_t>(index) * 4;
    return ReadTMEMTexel16(tmem, addr);
}

/// Look up a CI8 color from the palette area of TMEM
inline uint16_t LookupTLUT_CI8(const uint8_t* tmem, uint8_t index) {
    uint32_t addr = TMEM_PALETTE_BASE + static_cast<uint32_t>(index) * 4;
    return ReadTMEMTexel16(tmem, addr);
}

// ═══════════════════════════════════════════════════════════════════════════
//  DXT (Interleave) Calculations for LoadBlock
// ═══════════════════════════════════════════════════════════════════════════

/// Calculate the DXT value for LoadBlock
/// DXT = (2048 + lineSize - 1) / lineSize   (in 1.11 fixed-point)
inline uint32_t CalcDXT(uint32_t lineBytes) {
    if (lineBytes == 0) return 0;
    return (2048 + lineBytes - 1) / lineBytes;
}

/// Calculate number of texels per TMEM line for a given size
inline uint32_t CalcTexelsPerLine(ImageSize size) {
    switch (size) {
        case ImageSize::Bpp4:  return 64;
        case ImageSize::Bpp8:  return 32;
        case ImageSize::Bpp16: return 16;
        case ImageSize::Bpp32: return 8;
        default:               return 16;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Matrix Utilities (N64 Fixed-Point Matrix Format)
// ═══════════════════════════════════════════════════════════════════════════

/// Load a 4x4 matrix from N64 RDRAM in the interleaved fixed-point format:
///   First 32 bytes: 4x4 integer parts (s15)
///   Next 32 bytes:  4x4 fractional parts (u16)
///   Combined = integer + fraction/65536
inline void LoadMatrixFromRDRAM(const uint8_t* rdram, uint32_t addr, float mtx[4][4]) {
    // Integer part: 4 rows × 4 × 2 bytes = 32 bytes starting at addr
    // Fraction part: 32 bytes starting at addr + 32
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            uint32_t intOff  = addr + (i * 4 + j) * 2;
            uint32_t fracOff = addr + 32 + (i * 4 + j) * 2;

            int16_t  intPart  = static_cast<int16_t>(ReadBE16(rdram + intOff));
            uint16_t fracPart = ReadBE16(rdram + fracOff);

            mtx[i][j] = static_cast<float>(intPart) + static_cast<float>(fracPart) / 65536.0f;
        }
    }
}

/// Load a viewport from N64 RDRAM  
/// Format: 4x s.15 scale values, 4x s.15 translate values
inline void LoadViewportFromRDRAM(const uint8_t* rdram, uint32_t addr, N64Viewport& vp) {
    auto ReadS15 = [rdram](uint32_t off) -> float {
        int16_t raw = static_cast<int16_t>(ReadBE16(rdram + off));
        return static_cast<float>(raw) / 4.0f;
    };
    vp.scaleX = ReadS15(addr + 0);
    vp.scaleY = ReadS15(addr + 2);
    vp.scaleZ = ReadS15(addr + 4);
    // addr + 6 = padding
    vp.transX = ReadS15(addr + 8);
    vp.transY = ReadS15(addr + 10);
    vp.transZ = ReadS15(addr + 12);
}

/// Load a vertex from N64 RDRAM (16 bytes each)
inline void LoadN64Vertex(const uint8_t* rdram, uint32_t addr, N64Vertex& vtx) {
    vtx.x    = static_cast<int16_t>(ReadBE16(rdram + addr));
    vtx.y    = static_cast<int16_t>(ReadBE16(rdram + addr + 2));
    vtx.z    = static_cast<int16_t>(ReadBE16(rdram + addr + 4));
    vtx.flag = ReadBE16(rdram + addr + 6);
    vtx.s    = static_cast<int16_t>(ReadBE16(rdram + addr + 8));
    vtx.t    = static_cast<int16_t>(ReadBE16(rdram + addr + 10));
    vtx.r    = rdram[addr + 12];
    vtx.g    = rdram[addr + 13];
    vtx.b    = rdram[addr + 14];
    vtx.a    = rdram[addr + 15];
}

/// Load a light structure from N64 RDRAM (16 bytes: 2 copies of color + direction)
inline void LoadN64Light(const uint8_t* rdram, uint32_t addr, N64Light& light) {
    light.r    = rdram[addr + 0];
    light.g    = rdram[addr + 1];
    light.b    = rdram[addr + 2];
    light.pad0 = 0;
    light.r2   = rdram[addr + 4];
    light.g2   = rdram[addr + 5];
    light.b2   = rdram[addr + 6];
    light.pad1 = 0;
    light.dirX = static_cast<int8_t>(rdram[addr + 8]);
    light.dirY = static_cast<int8_t>(rdram[addr + 9]);
    light.dirZ = static_cast<int8_t>(rdram[addr + 10]);
    light.pad2 = 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Debugging: Format Descriptions
// ═══════════════════════════════════════════════════════════════════════════

/// Return a human-readable description of a tile's format
inline std::string DescribeTileFormat(const TileDescriptor& tile) {
    std::string s;
    s += GetImageFormatName(static_cast<ImageFormat>(tile.format));
    s += " ";
    s += GetImageSizeName(static_cast<ImageSize>(tile.size));
    s += " ";
    s += std::to_string(tile.GetWidth()) + "x" + std::to_string(tile.GetHeight());
    s += " @TMEM:" + std::to_string(tile.tmemAddr);
    return s;
}

/// Return a human-readable description of a color image descriptor
inline std::string DescribeImageDescriptor(const ImageDescriptor& img) {
    std::string s;
    s += GetImageFormatName(img.format);
    s += " ";
    s += GetImageSizeName(img.size);
    s += " w=" + std::to_string(img.width);
    s += " @0x";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06X", img.address);
    s += buf;
    return s;
}

/// Describe a cycle type + combine mode for debugging
inline std::string DescribeCycleMode(CycleType ct, const CombineMode& cm) {
    std::string s = GetCycleTypeName(ct);
    s += " Cyc0: (";
    s += std::to_string(cm.a_rgb0) + "-" + std::to_string(cm.b_rgb0);
    s += ")*" + std::to_string(cm.c_rgb0) + "+" + std::to_string(cm.d_rgb0);
    s += " a:(";
    s += std::to_string(cm.a_alpha0) + "-" + std::to_string(cm.b_alpha0);
    s += ")*" + std::to_string(cm.c_alpha0) + "+" + std::to_string(cm.d_alpha0);
    s += " Cyc1: (";
    s += std::to_string(cm.a_rgb1) + "-" + std::to_string(cm.b_rgb1);
    s += ")*" + std::to_string(cm.c_rgb1) + "+" + std::to_string(cm.d_rgb1);
    return s;
}

} // namespace Nova64::Video
