/**
 * @file RDPProcessor_Private.hpp
 * @brief Private shared helpers for RDPProcessor split translation units.
 *        Include ONLY from RDPProcessor*.cpp — never from headers.
 *        Each including .cpp MUST close the namespace with:
 *            } // namespace Nova64::Video
 */
#pragma once

#include "RDPProcessor.hpp"
#include "RDPUtils.hpp"
#include "../../common/log.hpp"
#include "FramebufferManager.hpp"
#include "DepthBuffer.hpp"
#include "../Common/BitUtils.hpp"
#include "../Common/RDRAMAccess.hpp"
#include "../Common/Logger.hpp"
#include "../Texture/TextureCache.hpp"
#include "../Renderer/ShaderManager.hpp"
#include "../Renderer/Renderer.hpp"

#include <glad/gl.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <random>
#include <vector>
#include <unordered_set>

// === DEBUG: Set to true to force all fragments to bright red ===============
static constexpr bool FORCE_RED_DEBUG = false;
// === DEBUG: Set to true to skip ALL depth-tested triangle draws ============
static constexpr bool DEBUG_SKIP_DEPTH_TRIS = false;
// === MLE blender toggle: true = legacy GL blend, false = imageLoad/imageStore ==
static constexpr bool DISABLE_MLE_BLENDER = false;
// === DEBUG: Disable N64 image-based depth compare (use GL depth test only) ===
static constexpr bool DISABLE_N64_DEPTH_COMPARE = false;

namespace Nova64::Video {

static bool IsRGBAFramebufferFormat(ImageFormat format, ImageSize size) {
    return format == ImageFormat::RGBA &&
           (size == ImageSize::Bpp16 || size == ImageSize::Bpp32);
}

static bool IsRGBAFramebufferFormat(const Tex::TextureKey& key) {
    return key.format == Tex::N64TexFormat::RGBA &&
           (key.size == Tex::N64TexSize::Bits16 || key.size == Tex::N64TexSize::Bits32);
}

static bool CanAliasFramebuffer(const GPUFramebuffer* fb, uint32_t address, bool framebufferFormat) {
    if (!fb) return false;

    // Depth FBs must never be sampled as color textures.
    if (fb->desc.type != FBType::Color) return false;

    const uint32_t rdramAddress = address & 0x7FFFFF;
    if (fb->desc.address == rdramAddress) return true;

    // For non-exact matches, require RGBA fb-format AND stride-aligned offset.
    // Non-stride-aligned in-range hits are almost always game textures whose
    // RDRAM happens to lie inside an old FB's footprint — sampling the FB
    // there replaces the texture with stale framebuffer pixels.
    if (!framebufferFormat || rdramAddress < fb->desc.address)
        return false;
    if (fb->desc.stride == 0 || rdramAddress >= fb->desc.EndAddress())
        return false;
    return ((rdramAddress - fb->desc.address) % fb->desc.stride) == 0;
}

static bool TraceMMRdpEnabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("NOVA64_MM_RDP_TRACE");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

#define MM_RDP_TRACE(fmt, ...)                                                   \
    do {                                                                         \
        if (TraceMMRdpEnabled()) {                                               \
            NOVA_LOG(LogLevel::Info, LogSubsystem::RDP, "[MMRDP] " fmt,          \
                     ##__VA_ARGS__);                                             \
        }                                                                        \
    } while (0)

static bool ForensicEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("NOVA64_FORENSIC");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

#define FORENSIC_TRACE(fmt, ...)                                                  \
    do {                                                                          \
        if (ForensicEnabled()) {                                                  \
            NOVA_LOG(LogLevel::Info, LogSubsystem::RDP, "[FORENSIC] " fmt,        \
                     ##__VA_ARGS__);                                              \
        }                                                                         \
    } while (0)

// Compute the effective texture dimensions for a tile, accounting for
// mask/clamp settings.
static void CalcTileSize(const TileDescriptor& tile, uint8_t tlutType, int& outW, int& outH) {
    int tileW = std::max(1, tile.GetWidth());
    int tileH = std::max(1, tile.GetHeight());
    bool hasTLUT = (tlutType != 0);
    static const uint32_t noTLUT[]   = { 8192, 4096, 2048, 1024 };
    static const uint32_t withTLUT[] = { 4096, 2048, 2048, 1024 };
    uint32_t maxTexels = hasTLUT ? withTLUT[tile.size & 3] : noTLUT[tile.size & 3];
    int maskW = (tile.maskS == 0) ? tileW : (1 << tile.maskS);
    int maskH = (tile.maskT == 0) ? tileH : (1 << tile.maskT);
    if (tile.IsClamped_S())
        outW = std::min(maskW, tileW);
    else if (static_cast<uint32_t>(maskW * maskH) <= maxTexels)
        outW = maskW;
    else
        outW = tileW;
    if (tile.IsClamped_T())
        outH = std::min(maskH, tileH);
    else if (static_cast<uint32_t>(maskW * maskH) <= maxTexels)
        outH = maskH;
    else
        outH = tileH;
}

// NOTE: namespace Nova64::Video is intentionally left open here.
// Every .cpp that includes this header MUST close it with:
//     } // namespace Nova64::Video
