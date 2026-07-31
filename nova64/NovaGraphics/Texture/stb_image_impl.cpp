/**
 * @file stb_image_impl.cpp
 * @brief stb_image_impl implementation.
 */
// stb_image implementation compilation unit
// This file compiles the stb_image library for PNG/BMP/TGA/etc loading.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG        // Only need PNG support (BMP/TGA/DDS handled natively)
#define STBI_NO_STDIO        // We'll use our own file I/O
#define STBI_NO_LINEAR
#define STBI_NO_HDR

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244 4100 4456)  // Suppress stb_image warnings
#endif

#include "stb_image.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

// ── stb_image_write: headless framebuffer PNG capture (NOVA64_SHOT_*) ──
// STBI_NO_STDIO above only gates the *read* side; stbi_write_png (stdio) stays
// available.  Exposed via Nova::WritePNG so GPU backends don't pull in stb.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244 4100 4456)
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "ImageWrite.hpp"

namespace Nova {
bool WritePNG(const char* path, int w, int h, int comp,
              const void* data, int strideBytes) {
    if (!path || !data || w <= 0 || h <= 0) return false;
    if (strideBytes <= 0) strideBytes = w * comp;
    return stbi_write_png(path, w, h, comp, data, strideBytes) != 0;
}
} // namespace Nova
