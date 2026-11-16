#pragma once
/**
 * @file ImageWrite.hpp
 * @brief Minimal PNG writer wrapper (stb_image_write) for headless framebuffer
 *        capture. Keeps stb_image_write out of GPU-backend translation units.
 */
#include <cstdint>

namespace Nova {

// Write an 8-bit-per-channel image to a PNG file.
//   path        : output file path
//   w, h        : dimensions in pixels
//   comp        : channels per pixel (3 = RGB, 4 = RGBA)
//   data        : pixel data, top row first
//   strideBytes : bytes between rows (0 = tightly packed w*comp)
// Returns true on success.
bool WritePNG(const char* path, int w, int h, int comp, const void* data, int strideBytes = 0);

} // namespace Nova
