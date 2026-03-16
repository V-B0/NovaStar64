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
