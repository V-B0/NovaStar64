/**
 * @file RDPUtils.hpp
 * @brief Fill-color decode helper shared by RDP state/draw handlers.
 */

#pragma once

#include "N64Types.hpp"
#include <cstdint>

namespace Nova64::Video {

/// Unpack RGBA5551 (16-bit) to r, g, b, a (8-bit each)
inline void UnpackRGBA5551(uint16_t packed, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    r = static_cast<uint8_t>(((packed >> 11) & 0x1F) << 3);
    g = static_cast<uint8_t>(((packed >> 6) & 0x1F) << 3);
    b = static_cast<uint8_t>(((packed >> 1) & 0x1F) << 3);
    a = (packed & 1) ? 0xFF : 0x00;
    // Replicate high bits into low bits for better precision
    r |= (r >> 5);
    g |= (g >> 5);
    b |= (b >> 5);
}

/// Decode an N64 fill color from the 32-bit register
/// For 16-bit modes, it contains two RGBA5551 pixels; for 32-bit, one RGBA8888
inline void DecodeFillColor(uint32_t raw, ImageSize size, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    if (size == ImageSize::Bpp32) {
        r = (raw >> 24) & 0xFF;
        g = (raw >> 16) & 0xFF;
        b = (raw >> 8) & 0xFF;
        a = raw & 0xFF;
    } else {
        // 16-bit: take upper 16 bits as RGBA5551
        UnpackRGBA5551(static_cast<uint16_t>(raw >> 16), r, g, b, a);
    }
}

} // namespace Nova64::Video
