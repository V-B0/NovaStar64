/**
 * @file RDPProcessor_Private.hpp
 * @brief Private shared helpers for RDPProcessor split translation units.
 *        Include ONLY from RDPProcessor*.cpp — never from headers.
 *        Each including .cpp MUST close the namespace with:
 *            } // namespace Nova64::Video
 */
#pragma once
#include <limits>

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

// NOVA64_MM_RDP_TRACE=1 logs RDP commands; NOVA64_MM_RDP_TRACE_FROM / _TO limit
// it to a range of VI frames, since tracing a whole run is far too slow.
static bool TraceMMRdpEnabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("NOVA64_MM_RDP_TRACE");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    if (!enabled) return false;
    static const long from = []() {
        const char* v = std::getenv("NOVA64_MM_RDP_TRACE_FROM");
        return v ? std::strtol(v, nullptr, 10) : 0L;
    }();
    static const long to = []() {
        const char* v = std::getenv("NOVA64_MM_RDP_TRACE_TO");
        return v ? std::strtol(v, nullptr, 10) : std::numeric_limits<long>::max();
    }();
    const long frame = g_presentIndex.load(std::memory_order_relaxed);
    return frame >= from && frame <= to;
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
// LoadInfo for one TMEM slot, mirroring GLideN64's gDP.loadInfo[] entry.
// Passing nullptr keeps the historical mask/clamp-only behaviour.
struct TileLoadInfo {
    bool     isTileLoad = false;
    uint16_t width      = 0;   // texels actually loaded per row
    uint16_t height     = 0;   // rows actually loaded
    uint16_t texWidth   = 0;   // SetTextureImage width
    uint8_t  size       = 0;   // SetTextureImage pixel size
};

// Build a TileLoadInfo for `tile` from its TMEM slot's load record. Every
// CalcTileSize call site must go through this (not construct TileLoadInfo by
// hand) so they all agree on tile size — a call site that skips it silently
// falls back to CalcTileSize's mask/clamp-only path and can disagree with one
// that doesn't, for the same texture, in the same frame (this was the case
// for FlushBatch's shader texture-size uniforms vs BuildTextureKey's decode
// size — the same LoadTile-sourced texture got sized two different ways,
// which is exactly a UV-scale/actual-decode-size mismatch: stretching).
template <typename RDPStateT>
static TileLoadInfo MakeTileLoadInfo(const RDPStateT& rdp, const TileDescriptor& tile) {
    TileLoadInfo li;
    const uint32_t slot = tile.tmemAddr & 0x1FF;
    li.isTileLoad = (rdp.tmemLoadType[slot] == RDPStateT::LOADTYPE_TILE);
    li.width      = rdp.tmemLoadWidth[slot];
    li.height     = rdp.tmemLoadHeight[slot];
    li.texWidth   = rdp.tmemLoadTexWidth[slot];
    li.size       = rdp.tmemLoadSize[slot];
    return li;
}

// GLideN64 TileSizes parity:
//     _sizes.clampWidth = (clamps && cycleType != G_CYC_COPY) ? tileWidth : width;
// clampWidth/Height bound the SOURCE texel reads during TMEM decode, so texels
// past the loaded extent replicate the edge instead of reading on into whatever
// TMEM holds next (adjacent texture data or the palette). Optional out-params;
// pass nullptr to skip.
static void CalcTileSize(const TileDescriptor& tile, uint8_t tlutType, int& outW, int& outH,
                         const TileLoadInfo* loadInfo = nullptr,
                         int cycleType = 0,
                         int* outClampW = nullptr, int* outClampH = nullptr) {
    int tileW = std::max(1, tile.GetWidth());
    int tileH = std::max(1, tile.GetHeight());

    // ── LOADTYPE_TILE branch (GLideN64 Textures.cpp::_calcTileSizes) ──
    // For data brought in by G_LOADTILE the hardware-visible extent is the
    // region that was actually LOADED, not the tile's mask-derived extent.
    // GLideN64:
    //     width = min(info.width, info.texWidth);
    //     if (width == 0) width = tileWidth;
    //     if (info.size > pTile->size) width <<= info.size - pTile->size;
    //     height = info.height; if (height == 0) height = tileHeight;
    // The size-shift matters because a load performed at a larger pixel size
    // than the tile samples at packs proportionally more texels per row.
    if (loadInfo != nullptr && loadInfo->isTileLoad) {
        int w = static_cast<int>(std::min<uint32_t>(loadInfo->width, loadInfo->texWidth));
        if (w == 0) w = tileW;
        if (loadInfo->size > tile.size)
            w <<= (loadInfo->size - tile.size);
        int h = static_cast<int>(loadInfo->height);
        if (h == 0) h = tileH;
        // Clamp override: a clamped, unmasked axis reports the tile extent.
        outW = (tile.IsClamped_S() && tile.maskS == 0) ? tileW : std::max(1, w);
        outH = (tile.IsClamped_T() && tile.maskT == 0) ? tileH : std::max(1, h);
        constexpr int kCycCopy = 2;
        if (outClampW) *outClampW = (tile.IsClamped_S() && cycleType != kCycCopy) ? tileW : outW;
        if (outClampH) *outClampH = (tile.IsClamped_T() && cycleType != kCycCopy) ? tileH : outH;
        return;
    }

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
    constexpr int kCycCopy = 2;
    if (outClampW) *outClampW = (tile.IsClamped_S() && cycleType != kCycCopy) ? tileW : outW;
    if (outClampH) *outClampH = (tile.IsClamped_T() && cycleType != kCycCopy) ? tileH : outH;
}

// NOTE: namespace Nova64::Video is intentionally left open here.
// Every .cpp that includes this header MUST close it with:
//     } // namespace Nova64::Video
