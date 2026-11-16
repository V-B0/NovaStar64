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
    // Widen 5 bits to 8 by replicating the top bits into the low bits so 0x1F maps to 0xFF
    r |= (r >> 5);
    g |= (g >> 5);
    b |= (b >> 5);
}

/// Decode a fill color register: one RGBA8888 pixel for 32-bit images, two RGBA5551 pixels for 16-bit
/// (the upper one is used)
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
