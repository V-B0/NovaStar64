/**
 * @file TextureCache.cpp
 * @brief Texture Cache.
 */
// SPDX-License-Identifier: MIT
// NovaGraphics - Texture Cache Implementation
// N64 texture decoding, caching, and GPU upload

#include "TextureCache.hpp"
#include "ImageWrite.hpp"
#include "../Common/BitUtils.hpp"
#include "../Common/Logger.hpp"
#include "../../common/log.hpp"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cassert>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace Nova64 { namespace Video { namespace Tex {

static bool ForensicEnabled() {
    static const bool enabled = []() {
#pragma warning(push)
#pragma warning(disable: 4996)
        const char* v = std::getenv("NOVA64_FORENSIC");
#pragma warning(pop)
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  ALPHA BLEEDING
//  Many N64 textures use alpha=0 to mark transparent texels but leave the RGB
//  channels at undefined values (often white).  When the GPU does bilinear
//  filtering across the transparent edge, the white RGB bleeds into the
//  visible color, producing a "white halo / faint white gradient" outline
//  around foliage, paths, decals, etc.
//
//  Fix: for every fully transparent pixel (alpha == 0), copy the RGB from a
//  nearby non-transparent neighbor.  This way bilinear filtering at the edge
//  blends sensible color values together and the alpha gradient handles
//  transparency.  This matches the "alpha bleed" pre-processing used by
//  GLideN64 and ParaLLEl-RDP.
// ═══════════════════════════════════════════════════════════════════════════════
static void BleedAlphaEdges(uint32_t* pixels, uint32_t width, uint32_t height) {
    if (!pixels || width == 0 || height == 0) return;
    // Single 8-neighbour pass — sufficient for the 1-2 texel bleed that bilinear
    // filtering can sample.  Multiple passes would propagate further but cost
    // more; one pass is the GLideN64-default and works well in practice.
    std::vector<uint32_t> src(pixels, pixels + width * height);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t idx = y * width + x;
            uint32_t p = src[idx];
            if ((p >> 24) != 0) continue; // already opaque, skip
            // Search 8 neighbours; pick the first opaque one.
            uint32_t r = 0, g = 0, b = 0, count = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                int ny = static_cast<int>(y) + dy;
                if (ny < 0 || ny >= static_cast<int>(height)) continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = static_cast<int>(x) + dx;
                    if (nx < 0 || nx >= static_cast<int>(width)) continue;
                    uint32_t np = src[ny * width + nx];
                    if ((np >> 24) == 0) continue; // also transparent
                    r += (np >> 0)  & 0xFF;
                    g += (np >> 8)  & 0xFF;
                    b += (np >> 16) & 0xFF;
                    ++count;
                }
            }
            if (count > 0) {
                r /= count; g /= count; b /= count;
                pixels[idx] = (r) | (g << 8) | (b << 16); // alpha stays 0
            } else {
                // No opaque neighbour at all — zero RGB to avoid bleeding white.
                pixels[idx] = 0;
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  TEXTURE CACHE LIFECYCLE
// ═══════════════════════════════════════════════════════════════════════════════

TextureCache::TextureCache() = default;
TextureCache::~TextureCache() { Shutdown(); }

bool TextureCache::Initialize(IRenderer* renderer, const TextureCacheConfig& config) {
    m_renderer = renderer;
    m_config = config;
    // NOVA64_NO_MIPMAPS=1 turns mipmapping off, to check whether automatic GPU
    // mip-level selection is behind a texture problem (real hardware never
    // does it for TEXRECT, which is always sampled at mip 0).
    if (std::getenv("NOVA64_NO_MIPMAPS")) m_config.enableMipmaps = false;
    m_frameCounter = 0;
    m_stats = {};
    m_cache.reserve(config.maxEntries);
    return true;
}

void TextureCache::Shutdown() {
    m_zombies.clear(); // safe: GPU must be idle at shutdown
    m_cache.clear();
    m_lruList.clear();
    m_stats = {};
    m_renderer = nullptr;
}

void TextureCache::BeginFrame() {
    m_frameCounter++;
    // Release deferred-destroy zombies whose last GPU frame has finished.
    const uint64_t completed = m_renderer ? m_renderer->CompletedFrameSerial() : UINT64_MAX;
    while (!m_zombies.empty() && m_zombies.front().retireSerial <= completed) {
        m_zombies.pop_front(); // unique_ptr destruction happens here -- the GPU is done
    }
    // Periodic eviction — every 60 frames, clean stale textures
    if ((m_frameCounter & 0x3F) == 0) {
        EvictStale(m_frameCounter);
    }
}

void TextureCache::SetConfig(const TextureCacheConfig& config) {
    m_config = config;
    // If max cache reduced, evict excess
    while (m_stats.currentBytes > config.maxCacheBytes && !m_lruList.empty()) {
        EvictLRU();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  TEXTURE LOOKUP / CREATION
// ═══════════════════════════════════════════════════════════════════════════════

namespace {
// Replicate the edge texel past the clamp extent on each axis (no-op when the
// key carries no decode-time clamp).
void ApplyDecodeClamp(const TextureKey& key, std::vector<uint32_t>& pixels)
{
    const uint32_t w = key.width, h = key.height;
    const uint32_t cw = key.clampWidth, ch = key.clampHeight;
    if (w == 0 || h == 0) return;
    if (pixels.size() < static_cast<size_t>(w) * h) return;
    if (cw > 0 && cw < w) {
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t* row = pixels.data() + static_cast<size_t>(y) * w;
            const uint32_t edge = row[cw - 1];
            for (uint32_t x = cw; x < w; ++x) row[x] = edge;
        }
    }
    if (ch > 0 && ch < h) {
        const uint32_t* edgeRow = pixels.data() + static_cast<size_t>(ch - 1) * w;
        for (uint32_t y = ch; y < h; ++y)
            std::memcpy(pixels.data() + static_cast<size_t>(y) * w,
                        edgeRow, static_cast<size_t>(w) * sizeof(uint32_t));
    }
}
} // namespace

CachedTexture* TextureCache::GetTexture(const TextureKey& key, const uint8_t* rdram,
                                          const uint8_t* tmem) {
    // Look up in cache
    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        // Cache hit — update LRU
        m_stats.hits++;
        if (ForensicEnabled())
            Logger::Info("[FORENSIC] CACHE HIT  crc=0x{:016X} fmt={} size={} pal={} w={} h={}",
                         key.crc, static_cast<int>(key.format), static_cast<int>(key.size),
                         key.palette, key.width, key.height);
        auto& entry = it->second;
        entry.texture.lastUsedFrame = m_frameCounter;
        entry.texture.useCount++;

        // Move to front of LRU list
        m_lruList.erase(entry.lruIterator);
        m_lruList.push_front(key);
        entry.lruIterator = m_lruList.begin();

        return &entry.texture;
    }

    // Cache miss — decode and upload
    m_stats.misses++;
    if (ForensicEnabled())
        Logger::Info("[FORENSIC] CACHE MISS crc=0x{:016X} fmt={} size={} pal={} w={} h={}",
                     key.crc, static_cast<int>(key.format), static_cast<int>(key.size),
                     key.palette, key.width, key.height);

    // Ensure we have room
    while (m_stats.currentEntries >= m_config.maxEntries ||
           m_stats.currentBytes >= m_config.maxCacheBytes) {
        if (m_lruList.empty()) break;
        EvictLRU();
    }

    // Try hi-res texture replacement first
    if (m_config.enableHiResTextures) {
        CachedTexture hiResTex;
        if (LoadHiResTexture(hiResTex, key)) {
            m_lruList.push_front(key);
            CacheEntry cacheEntry;
            cacheEntry.texture = std::move(hiResTex);
            cacheEntry.texture.lastUsedFrame = m_frameCounter;
            cacheEntry.texture.useCount = 1;
            cacheEntry.lruIterator = m_lruList.begin();

            m_stats.currentEntries++;
            m_stats.currentBytes += cacheEntry.texture.sizeBytes;
            m_stats.uploads++;
            if (m_stats.currentBytes > m_stats.peakBytes)
                m_stats.peakBytes = m_stats.currentBytes;

            auto result = m_cache.emplace(key, std::move(cacheEntry));
            return &result.first->second.texture;
        }
    }

    // Decode texture from TMEM or RDRAM.
    // When decoding from TMEM, key.tmemOffset tells DecodeTexture where in the
    // 4096-byte TMEM buffer this tile's data starts. When falling back to RDRAM,
    // the pointer already points at the tile data so we zero the offset locally.
    std::vector<uint32_t> pixels;
    if (tmem) {
        DecodeTexture(key, tmem, pixels, true);
    } else {
        TextureKey rawKey = key;
        rawKey.tmemOffset = 0;
        DecodeTexture(rawKey, rdram + key.address, pixels, false);
    }

    if (pixels.empty()) return nullptr;

    // ── Decode-time CLAMP (GLideN64 Textures.cpp clampSClamp/clampTClamp) ──
    // On a CLAMPed axis the RDP re-reads the edge texel past the loaded extent
    // rather than continuing into TMEM. GLideN64 bounds the source read while
    // decoding; replicating the edge texel across the remainder of the decoded
    // image is equivalent and avoids touching all nine format decoders.
    // Without this, texels between the loaded extent and the sampled extent
    // came from whatever followed in TMEM (a neighbouring texture, or the
    // palette) -- garbage/smeared columns and rows at texture edges.
    ApplyDecodeClamp(key, pixels);
    if (!m_dumpFolder.empty()) DumpTexture(key, pixels);

    {
        static const bool s_texDbg = std::getenv("NOVA64_TEXDBG") != nullptr;
        if (s_texDbg) {
            size_t nonZero = 0, opaque = 0;
            for (uint32_t px : pixels) {
                if (px != 0) ++nonZero;
                if (px & 0xFF000000u) ++opaque;
            }
            std::fprintf(stderr,
                "[TEXDBG] decode addr=0x%06X fmt=%d size=%d w=%u h=%u line=%u tmem=%u pal=%u tlut=%u crc=0x%016llX fromTMEM=%d nonZero=%zu/%zu opaque=%zu\n",
                key.address & 0x7FFFFF, static_cast<int>(key.format), static_cast<int>(key.size),
                key.width, key.height, key.line, key.tmemOffset, key.palette, key.tlutType,
                static_cast<unsigned long long>(key.crc), tmem ? 1 : 0,
                nonZero, pixels.size(), opaque);
        }
    }

    // Eliminate the white-halo / alpha-edge bleed produced by GPU bilinear
    // filtering on N64 textures with undefined RGB at alpha=0 texels.
    BleedAlphaEdges(pixels.data(), key.width, key.height);

    // The GPU texture is exactly the tile's size, as in GLideN64. Rounding up
    // to a power of two left a transparent margin past the tile that repeat
    // wrapping, bilinear/3-point filtering and generated mip levels all
    // sampled: gaps in tiled surfaces, dark seams at texture edges, and
    // surfaces fading at a distance. Vulkan and GL 4.6 mipmap any size.
    uint32_t gpuWidth = key.width;
    uint32_t gpuHeight = key.height;

    // Create GPU texture
    TextureDesc desc;
    desc.width = gpuWidth;
    desc.height = gpuHeight;
    desc.format = TextureFormat::RGBA8;
    desc.usage = BufferUsage::Dynamic;
    desc.mipLevels = m_config.enableMipmaps
        ? std::min<uint32_t>(4, MaxMipLevels(gpuWidth, gpuHeight)) : 1;

    auto gpuTex = m_renderer->CreateTexture(desc);
    if (!gpuTex) return nullptr;

    // Upload pixel data
    // If GPU texture is larger than N64 texture (power-of-2 rounding),
    // we need to upload to the correct sub-region
    if (gpuWidth == key.width && gpuHeight == key.height) {
        gpuTex->Upload(pixels.data(), pixels.size() * sizeof(uint32_t));
    } else {
        // Clear to transparent first, then upload sub-region
        std::vector<uint32_t> clearData(gpuWidth * gpuHeight, 0);
        gpuTex->Upload(clearData.data(), clearData.size() * sizeof(uint32_t));
        gpuTex->UploadSubRegion(0, 0, key.width, key.height,
                                  pixels.data(), pixels.size() * sizeof(uint32_t));
    }

    // Generate mipmaps if enabled
    if (m_config.enableMipmaps && desc.mipLevels > 1) {
        gpuTex->GenerateMipmaps();
    }

    // Set sampler state based on N64 tile settings
    SamplerDesc samplerDesc;
    if (m_config.forceLinearFilter) {
        samplerDesc.minFilter = SamplerFilter::LinearMipLinear;
        samplerDesc.magFilter = SamplerFilter::Linear;
    } else if (m_config.enable3PointFilter) {
        samplerDesc.minFilter = SamplerFilter::N64_3Point;
        samplerDesc.magFilter = SamplerFilter::N64_3Point;
    } else {
        samplerDesc.minFilter = SamplerFilter::NearestMipNearest;
        samplerDesc.magFilter = SamplerFilter::Nearest;
    }

    // Wrap modes from N64 tile
    // cms/cmt bits: bit0=mirror, bit1=clamp
    // When the tile has a mask (maskS/maskT > 0) the fragment shader performs
    // ALL wrap/mirror/clamp in texel space, so the GPU sampler must be
    // CLAMP_TO_EDGE to avoid re-wrapping the already-resolved UVs (matches the
    // OpenGL path's getGLWrapMode). Without this the Vulkan sampler used
    // REPEAT/MIRRORED_REPEAT on masked tiles and re-wrapped edge UVs into the
    // opposite edge / transparent-black border — the MM file-select black
    // shard mosaic on the MoltenVK backend.
    auto translateWrap = [](uint8_t mode, uint8_t mask) -> TextureWrap {
        if (mask > 0)   return TextureWrap::ClampToEdge;
        if (mode & 0x2) return TextureWrap::ClampToEdge;
        if (mode & 0x1) return TextureWrap::MirroredRepeat;
        return TextureWrap::Repeat;
    };
    samplerDesc.wrapS = translateWrap(key.cms, key.maskS);
    samplerDesc.wrapT = translateWrap(key.cmt, key.maskT);
    samplerDesc.maxAnisotropy = m_config.anisotropicLevel;
    samplerDesc.lodBias = m_config.lodBias;

    gpuTex->SetSampler(samplerDesc);

    // Create cache entry
    m_lruList.push_front(key);

    CacheEntry cacheEntry;
    cacheEntry.texture.key = key;
    cacheEntry.texture.gpuTexture = std::move(gpuTex);
    cacheEntry.texture.realWidth = gpuWidth;
    cacheEntry.texture.realHeight = gpuHeight;
    cacheEntry.texture.sizeBytes = gpuWidth * gpuHeight * 4 * desc.mipLevels;
    cacheEntry.texture.lastUsedFrame = m_frameCounter;
    cacheEntry.texture.useCount = 1;
    cacheEntry.texture.scaleS = static_cast<float>(key.width) / gpuWidth;
    cacheEntry.texture.scaleT = static_cast<float>(key.height) / gpuHeight;
    cacheEntry.lruIterator = m_lruList.begin();

    m_stats.currentEntries++;
    m_stats.currentBytes += cacheEntry.texture.sizeBytes;
    m_stats.uploads++;
    if (m_stats.currentBytes > m_stats.peakBytes)
        m_stats.peakBytes = m_stats.currentBytes;

    auto result = m_cache.emplace(key, std::move(cacheEntry));
    return &result.first->second.texture;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  MIPMAPPED TEXTURE LOOKUP / CREATION (TILE-CHAIN)
// ═══════════════════════════════════════════════════════════════════════════════

CachedTexture* TextureCache::GetTexture(const TextureKey& key, const uint8_t* rdram,
                                          const uint8_t* tmem,
                                          const MipTileInfo* mipTiles, int mipCount) {
    // Look up in cache — same as non-mip path
    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        m_stats.hits++;
        auto& entry = it->second;
        entry.texture.lastUsedFrame = m_frameCounter;
        entry.texture.useCount++;
        m_lruList.erase(entry.lruIterator);
        m_lruList.push_front(key);
        entry.lruIterator = m_lruList.begin();
        return &entry.texture;
    }

    // Cache miss — decode base level
    m_stats.misses++;
    while (m_stats.currentEntries >= m_config.maxEntries ||
           m_stats.currentBytes >= m_config.maxCacheBytes) {
        if (m_lruList.empty()) break;
        EvictLRU();
    }

    // Decode base tile (level 0)
    std::vector<uint32_t> pixels;
    if (tmem) {
        DecodeTexture(key, tmem, pixels, true);
    } else {
        TextureKey rawKey = key;
        rawKey.tmemOffset = 0;
        DecodeTexture(rawKey, rdram + key.address, pixels, false);
    }
    if (pixels.empty()) return nullptr;

    // ── Decode-time CLAMP (GLideN64 Textures.cpp clampSClamp/clampTClamp) ──
    // On a CLAMPed axis the RDP re-reads the edge texel past the loaded extent
    // rather than continuing into TMEM. GLideN64 bounds the source read while
    // decoding; replicating the edge texel across the remainder of the decoded
    // image is equivalent and avoids touching all nine format decoders.
    // Without this, texels between the loaded extent and the sampled extent
    // came from whatever followed in TMEM (a neighbouring texture, or the
    // palette) -- garbage/smeared columns and rows at texture edges.
    ApplyDecodeClamp(key, pixels);

    // Exactly the tile's size -- see the note in the other upload path.
    uint32_t gpuWidth = key.width;
    uint32_t gpuHeight = key.height;

    // Determine actual mip level count: base + mipCount additional levels,
    // clamped to what the (post-pow2) base dimensions can actually support.
    int totalLevels = 1 + std::min(mipCount, 7);
    totalLevels = std::min<uint32_t>(totalLevels, MaxMipLevels(gpuWidth, gpuHeight));

    TextureDesc desc;
    desc.width = gpuWidth;
    desc.height = gpuHeight;
    desc.format = TextureFormat::RGBA8;
    desc.usage = BufferUsage::Dynamic;
    desc.mipLevels = static_cast<uint32_t>(totalLevels);

    auto gpuTex = m_renderer->CreateTexture(desc);
    if (!gpuTex) return nullptr;

    // Upload base level (level 0)
    if (gpuWidth == key.width && gpuHeight == key.height) {
        gpuTex->Upload(pixels.data(), pixels.size() * sizeof(uint32_t), 0);
    } else {
        std::vector<uint32_t> clearData(gpuWidth * gpuHeight, 0);
        gpuTex->Upload(clearData.data(), clearData.size() * sizeof(uint32_t), 0);
        gpuTex->UploadSubRegion(0, 0, key.width, key.height,
                                  pixels.data(), pixels.size() * sizeof(uint32_t), 0);
    }

    // Upload each mip level from N64 tile chain TMEM data
    for (int mip = 0; mip < mipCount; mip++) {
        const auto& mt = mipTiles[mip];
        uint16_t mipW = mt.width;
        uint16_t mipH = mt.height;
        if (mipW == 0 || mipH == 0) continue;

        // Build a temporary key for this mip tile's format/size/TMEM location
        TextureKey mipKey{};
        mipKey.width      = mipW;
        mipKey.height     = mipH;
        mipKey.format     = static_cast<N64TexFormat>(mt.format);
        mipKey.size       = static_cast<N64TexSize>(mt.size);
        mipKey.line       = mt.line;
        mipKey.tmemOffset = mt.tmemAddr;
        mipKey.palette    = mt.palette;
        mipKey.tlutType   = key.tlutType;

        std::vector<uint32_t> mipPixels;
        const uint8_t* mipData = tmem ? tmem : (rdram + key.address);
            DecodeTexture(mipKey, mipData, mipPixels, tmem != nullptr);
        if (mipPixels.empty()) continue;

        uint32_t mipGpuW = std::max(1u, gpuWidth >> (mip + 1));
        uint32_t mipGpuH = std::max(1u, gpuHeight >> (mip + 1));

        if (mipGpuW == mipW && mipGpuH == mipH) {
            gpuTex->Upload(mipPixels.data(), mipPixels.size() * sizeof(uint32_t),
                           static_cast<uint32_t>(mip + 1));
        } else {
            // Mip level dimensions don't match the GL mip size — upload as sub-region
            std::vector<uint32_t> clearMip(mipGpuW * mipGpuH, 0);
            gpuTex->Upload(clearMip.data(), clearMip.size() * sizeof(uint32_t),
                           static_cast<uint32_t>(mip + 1));
            uint32_t uploadW = std::min(mipW, static_cast<uint16_t>(mipGpuW));
            uint32_t uploadH = std::min(mipH, static_cast<uint16_t>(mipGpuH));
            gpuTex->UploadSubRegion(0, 0, uploadW, uploadH,
                                      mipPixels.data(), mipPixels.size() * sizeof(uint32_t),
                                      static_cast<uint32_t>(mip + 1));
        }
    }

    // Set sampler state
    SamplerDesc samplerDesc;
    if (m_config.forceLinearFilter) {
        samplerDesc.minFilter = SamplerFilter::LinearMipLinear;
        samplerDesc.magFilter = SamplerFilter::Linear;
    } else if (m_config.enable3PointFilter) {
        samplerDesc.minFilter = SamplerFilter::N64_3Point;
        samplerDesc.magFilter = SamplerFilter::N64_3Point;
    } else {
        samplerDesc.minFilter = SamplerFilter::NearestMipNearest;
        samplerDesc.magFilter = SamplerFilter::Nearest;
    }
    auto translateWrap = [](uint8_t mode, uint8_t mask) -> TextureWrap {
        if (mask > 0)   return TextureWrap::ClampToEdge;  // shader handles wrap
        if (mode & 0x2) return TextureWrap::ClampToEdge;
        if (mode & 0x1) return TextureWrap::MirroredRepeat;
        return TextureWrap::Repeat;
    };
    samplerDesc.wrapS = translateWrap(key.cms, key.maskS);
    samplerDesc.wrapT = translateWrap(key.cmt, key.maskT);
    samplerDesc.maxAnisotropy = m_config.anisotropicLevel;
    samplerDesc.lodBias = m_config.lodBias;
    gpuTex->SetSampler(samplerDesc);

    // Create cache entry
    m_lruList.push_front(key);
    CacheEntry cacheEntry;
    cacheEntry.texture.key = key;
    cacheEntry.texture.gpuTexture = std::move(gpuTex);
    cacheEntry.texture.realWidth = gpuWidth;
    cacheEntry.texture.realHeight = gpuHeight;
    cacheEntry.texture.sizeBytes = gpuWidth * gpuHeight * 4 * desc.mipLevels;
    cacheEntry.texture.lastUsedFrame = m_frameCounter;
    cacheEntry.texture.useCount = 1;
    cacheEntry.texture.scaleS = static_cast<float>(key.width) / gpuWidth;
    cacheEntry.texture.scaleT = static_cast<float>(key.height) / gpuHeight;
    cacheEntry.lruIterator = m_lruList.begin();

    m_stats.currentEntries++;
    m_stats.currentBytes += cacheEntry.texture.sizeBytes;
    m_stats.uploads++;
    if (m_stats.currentBytes > m_stats.peakBytes)
        m_stats.peakBytes = m_stats.currentBytes;

    auto result = m_cache.emplace(key, std::move(cacheEntry));
    return &result.first->second.texture;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  INVALIDATION
// ═══════════════════════════════════════════════════════════════════════════════

void TextureCache::InvalidateRange(uint32_t addressStart, uint32_t sizeBytes) {
    uint32_t addressEnd = addressStart + sizeBytes;

    // Remove all textures whose RDRAM address overlaps the modified range
    auto it = m_cache.begin();
    while (it != m_cache.end()) {
        const auto& key = it->second.texture.key;
        uint32_t texEnd = key.address + CalculateTMEMSize(key);

        if (key.address < addressEnd && texEnd > addressStart) {
            // Overlap — invalidate (defer GPU resource destruction)
            m_stats.currentBytes -= it->second.texture.sizeBytes;
            m_stats.currentEntries--;
            m_stats.evictions++;
            m_lruList.erase(it->second.lruIterator);
            Retire(std::move(it->second.texture.gpuTexture));
            it = m_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void TextureCache::Retire(std::unique_ptr<ITexture> tex) {
    if (!tex) return;
    Zombie z;
    z.gpuTex = std::move(tex);
    z.retireSerial = m_renderer ? m_renderer->FrameSerial() : 0;
    m_zombies.push_back(std::move(z));
}

void TextureCache::InvalidateAll() {
    m_stats.evictions += m_stats.currentEntries;
    m_stats.currentEntries = 0;
    m_stats.currentBytes = 0;
    // Same rule as EvictLRU: the GPU may still be executing frames that sample these
    // textures, so they wait in the zombie queue instead of being destroyed here
    // (clearing the cache directly destroyed them at once).
    for (auto& entry : m_cache) Retire(std::move(entry.second.texture.gpuTexture));
    m_cache.clear();
    m_lruList.clear();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  EVICTION
// ═══════════════════════════════════════════════════════════════════════════════

void TextureCache::EvictStale(uint64_t currentFrame) {
    // Evict textures not used in the last 300 frames (~5 seconds)
    const uint64_t staleThreshold = 300;

    while (!m_lruList.empty()) {
        TextureKey oldest = m_lruList.back();
        auto cacheIt = m_cache.find(oldest);
        if (cacheIt != m_cache.end()) {
            if (currentFrame - cacheIt->second.texture.lastUsedFrame > staleThreshold) {
                m_stats.currentBytes -= cacheIt->second.texture.sizeBytes;
                m_stats.currentEntries--;
                m_stats.evictions++;
                Retire(std::move(cacheIt->second.texture.gpuTexture));
                m_cache.erase(cacheIt);
                m_lruList.pop_back();
            } else {
                // Since this is an LRU list, if the oldest is not stale, no newer item is.
                break;
            }
        } else {
            // Cleanup dangling entry
            m_lruList.pop_back();
        }
    }
}

void TextureCache::EvictLRU() {
    if (m_lruList.empty()) return;

    TextureKey oldest = m_lruList.back();
    m_lruList.pop_back();

    auto it = m_cache.find(oldest);
    if (it != m_cache.end()) {
        m_stats.currentBytes -= it->second.texture.sizeBytes;
        m_stats.currentEntries--;
        m_stats.evictions++;
        // Defer GPU resource destruction (see Retire).
        Retire(std::move(it->second.texture.gpuTexture));
        m_cache.erase(it);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  TEXTURE DECODING — DISPATCH
// ═══════════════════════════════════════════════════════════════════════════════

static inline uint8_t ReadTextureByte(const uint8_t* row, uint32_t byteOffset,
                                      uint32_t y, bool fromTMEM) {
    // DWordInterleave at load time models the N64 TMEM write interleave.
    // This read-time XOR models the TMEM read de-interleave: on odd rows,
    // swap the two 32-bit halves of each 64-bit word (XOR byte offset by 4).
    // Both operations are needed together (matching GLideN64's i=(ty&1)<<1 pattern).
    if (fromTMEM && (y & 1))
        byteOffset ^= 4;
    return row[byteOffset];
}

// Compute the row pointer for a given Y coordinate, wrapping the TMEM
// address at the correct bank boundary.  N64 TMEM is split into two 2KB
// banks: non-CI textures (no TLUT) can use the full 4KB (wrapMask = 0x1FF
// QWords), while CI textures with a TLUT only get the lower 2KB (wrapMask
// = 0xFF QWords) since the upper half holds the palette.
static inline const uint8_t* GetWrappedRow(const uint8_t* tmemBase,
                                            uint32_t tmemAddrQW,
                                            uint32_t lineQW, uint32_t y,
                                            uint32_t wrapMaskQW) {
    uint32_t rowQW = (tmemAddrQW + lineQW * y) & wrapMaskQW;
    return tmemBase + rowQW * 8;
}

void TextureCache::DecodeTexture(const TextureKey& key, const uint8_t* data,
                                   std::vector<uint32_t>& outPixels, bool fromTMEM) {
    uint32_t count = key.width * key.height;
    outPixels.resize(count);
    m_stats.conversions++;

    uint32_t lineBytes = key.line * 8; // N64 tile line is in 64-bit words
    if (lineBytes == 0 && key.width > 0) {
        uint32_t bitsPerPixel = 4u << (static_cast<uint32_t>(key.size) & 3u);
        lineBytes = (static_cast<uint32_t>(key.width) * bitsPerPixel + 7u) / 8u;
        lineBytes = (lineBytes + 7u) & ~7u;
    }

    // When data points to the full TMEM base, offset to this tile's start.
    // key.tmemOffset is the tile's tmemAddr (in 64-bit words). The tile's
    // sl/tl crop origin is NOT applied here: the shader already subtracts it
    // via uTexOffset0 (tileCoord = tc*scale - GetSL()), so shifting the decode
    // by sl too would double-count it and sample past the loaded data.
    const uint8_t* tileData = data + (static_cast<uint32_t>(key.tmemOffset) * 8);

    // TMEM row wrapping: N64 hardware wraps TMEM addresses at bank
    // boundaries.  CI textures with TLUT use only the lower 2KB (palette
    // in upper 2KB), so wrap at 256 QWords.  All others use the full 4KB
    // (512 QWords).  RGBA32 uses split-bank addressing (handled separately
    // by DecodeRGBA32_TMEM).  When not reading from TMEM, pass 0 to
    // disable wrapping.
    uint32_t wrapMaskQW = 0; // 0 = no wrapping
    if (fromTMEM) {
        bool hasTLUT = (key.tlutType != 0) || key.format == N64TexFormat::CI;
        wrapMaskQW = hasTLUT ? 0xFF : 0x1FF;
    }

    // Real N64 hardware: when a TLUT mode is active (otherModeH.textureLUT !=
    // G_TT_NONE) and the tile is 4-bit or 8-bit, the RDP ALWAYS decodes the
    // texel data as color-indexed with a palette lookup, regardless of the
    // tile's own format field. Confirmed against two independent authoritative
    // sources: angrylion-rdp-plus's cycle-accurate tile derivatives compute a
    // *different* effective format ("tlutswitch") whenever TLUT is enabled,
    // and GLideN64's texture-load dispatch table routes every non-YUV format
    // through the CI decoder at 4/8-bit once a TLUT mode is active (its own
    // comment: "Banjo-Kazooie uses this, doesn't make sense, but it works").
    // Tiles whose format field already says CI were unaffected by this gap
    // (decoding CI either way); tiles set up with some other format while
    // TLUT is active decoded as raw RGBA/IA/I -- misreading palette-index
    // bytes as pixel data -- which reads as noise/garbage.
    N64TexFormat effectiveFormat = key.format;
    if (key.tlutType != 0 && key.format != N64TexFormat::YUV &&
        (key.size == N64TexSize::Bits4 || key.size == N64TexSize::Bits8)) {
        effectiveFormat = N64TexFormat::CI;
    }

    switch (effectiveFormat) {
        case N64TexFormat::RGBA:
            if (key.size == N64TexSize::Bits16)
                DecodeRGBA16(tileData, outPixels.data(), key.width, key.height, lineBytes, fromTMEM, data, wrapMaskQW);
            else if (key.size == N64TexSize::Bits32) {
                if (fromTMEM)
                    DecodeRGBA32_TMEM(data, key.tmemOffset, key.line, outPixels.data(), key.width, key.height);
                else
                    DecodeRGBA32(tileData, outPixels.data(), key.width, key.height, lineBytes * 2, false);
            }
            break;

        case N64TexFormat::IA:
            if (key.size == N64TexSize::Bits16)
                DecodeIA16(tileData, outPixels.data(), key.width, key.height, lineBytes, fromTMEM, data, wrapMaskQW);
            else if (key.size == N64TexSize::Bits8)
                DecodeIA8(tileData, outPixels.data(), key.width, key.height, lineBytes, fromTMEM, data, wrapMaskQW);
            else if (key.size == N64TexSize::Bits4)
                DecodeIA4(tileData, outPixels.data(), key.width, key.height, lineBytes, fromTMEM, data, wrapMaskQW);
            break;

        case N64TexFormat::I:
            if (key.size == N64TexSize::Bits16)
                // I16 is not a native N64 format; treat as IA16 (intensity+alpha, 8-bit each).
                // Some games use format=I size=16b for the coverage-framebuffer copy path.
                DecodeIA16(tileData, outPixels.data(), key.width, key.height, lineBytes, fromTMEM, data, wrapMaskQW);
            else if (key.size == N64TexSize::Bits8)
                DecodeI8(tileData, outPixels.data(), key.width, key.height, lineBytes, fromTMEM, data, wrapMaskQW);
            else if (key.size == N64TexSize::Bits4)
                DecodeI4(tileData, outPixels.data(), key.width, key.height, lineBytes, fromTMEM, data, wrapMaskQW);
            break;

        case N64TexFormat::CI:
            // CI textures: tileData is the index stream; data is the full TMEM base
            // (palette lives at the upper TMEM half, data[TMEM_PALETTE_BASE+...])
            {
                uint8_t pal = static_cast<uint8_t>(key.palette);
                bool isIA = (key.tlutType == 3);  // G_TT_IA16

                if (key.size == N64TexSize::Bits8)
                    DecodeCI8(tileData, data, outPixels.data(), key.width, key.height, lineBytes, fromTMEM, isIA, wrapMaskQW);
                else if (key.size == N64TexSize::Bits4)
                    DecodeCI4(tileData, data, pal, outPixels.data(), key.width, key.height, lineBytes, fromTMEM, isIA, wrapMaskQW);
            }
            break;

        case N64TexFormat::YUV:
            DecodeYUV(tileData, outPixels.data(), key.width, key.height, lineBytes, fromTMEM, data, wrapMaskQW);
            break;

        default:
            std::memset(outPixels.data(), 0xFF, count * 4);
            break;
    }

}

// ═══════════════════════════════════════════════════════════════════════════════
//  FORMAT-SPECIFIC DECODERS
// ═══════════════════════════════════════════════════════════════════════════════

// Convert N64 RGBA5551 to RGBA8888
static inline uint32_t RGBA5551toRGBA8(uint16_t c) {
    uint8_t r = ((c >> 11) & 0x1F) << 3;
    uint8_t g = ((c >>  6) & 0x1F) << 3;
    uint8_t b = ((c >>  1) & 0x1F) << 3;
    uint8_t a = (c & 1) ? 0xFF : 0x00;
    // Replicate high bits into low bits for proper 5→8 expansion
    r |= r >> 5;
    g |= g >> 5;
    b |= b >> 5;
    return (a << 24) | (b << 16) | (g << 8) | r;
}

// Convert N64 IA88 (8-bit intensity + 8-bit alpha) to RGBA8888
static inline uint32_t IA88toRGBA8(uint16_t c) {
    uint8_t i = static_cast<uint8_t>(c >> 8);
    uint8_t a = static_cast<uint8_t>(c & 0xFF);
    return (a << 24) | (i << 16) | (i << 8) | i;
}

void TextureCache::DecodeRGBA16(const uint8_t* src, uint32_t* dst,
                                  uint32_t width, uint32_t height, uint32_t line, bool fromTMEM,
                                  const uint8_t* tmemBase, uint32_t wrapMaskQW) {
    const uint32_t tmemOff = (wrapMaskQW > 0 && tmemBase) ? static_cast<uint32_t>((src - tmemBase) >> 3) : 0;
    const uint32_t lineQW = line >> 3;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = (wrapMaskQW > 0 && tmemBase)
            ? GetWrappedRow(tmemBase, tmemOff, lineQW, y, wrapMaskQW)
            : src + y * line;
        for (uint32_t x = 0; x < width; x++) {
            uint16_t pixel = (static_cast<uint16_t>(ReadTextureByte(row, x * 2, y, fromTMEM)) << 8) |
                             ReadTextureByte(row, x * 2 + 1, y, fromTMEM);
            dst[y * width + x] = RGBA5551toRGBA8(pixel);
        }
    }
}

void TextureCache::DecodeRGBA32(const uint8_t* src, uint32_t* dst,
                                  uint32_t width, uint32_t height, uint32_t lineBytes, bool fromTMEM) {
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = src + y * lineBytes;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t r = ReadTextureByte(row, x * 4 + 0, y, fromTMEM);
            uint8_t g = ReadTextureByte(row, x * 4 + 1, y, fromTMEM);
            uint8_t b = ReadTextureByte(row, x * 4 + 2, y, fromTMEM);
            uint8_t a = ReadTextureByte(row, x * 4 + 3, y, fromTMEM);
            dst[y * width + x] = (a << 24) | (b << 16) | (g << 8) | r;
        }
    }
}

void TextureCache::DecodeRGBA32_TMEM(const uint8_t* tmemBase, uint16_t tmemAddr,
                                      uint16_t tileLine, uint32_t* dst,
                                      uint32_t width, uint32_t height) {
    // Split-bank RGBA32 decode matching GLideN64's _getTextureDestData for 32b
    const uint16_t* tmem16 = reinterpret_cast<const uint16_t*>(tmemBase);
    const uint32_t tbase = static_cast<uint32_t>(tmemAddr) << 2;  // in u16 units

    auto swapword = [](uint16_t v) -> uint16_t {
        return static_cast<uint16_t>((v << 8) | (v >> 8));
    };

    // Compute line32 stride in u16 units (matching GLideN64)
    int wid_64 = static_cast<int>(width) << 2;
    if (wid_64 & 15) wid_64 += 16;
    wid_64 &= 0xFFFFFFF0;
    wid_64 >>= 3;
    int line32 = static_cast<int>(tileLine) << 1;
    line32 = (line32 - wid_64) << 3;
    if (wid_64 < 1) wid_64 = 1;
    int w = wid_64 << 1;
    line32 = w + (line32 >> 2);

    for (uint32_t y = 0; y < height; y++) {
        uint32_t tline = tbase + static_cast<uint32_t>(line32) * y;
        uint32_t xorval = (y & 1) ? 3u : 1u;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t taddr = ((tline + x) ^ xorval) & 0x3ff;
            uint16_t gr = swapword(tmem16[taddr]);
            uint16_t ab = swapword(tmem16[taddr | 0x400]);
            dst[y * width + x] = (static_cast<uint32_t>(ab) << 16) | gr;
        }
    }
}

void TextureCache::DecodeIA16(const uint8_t* src, uint32_t* dst,
                                uint32_t width, uint32_t height, uint32_t line, bool fromTMEM,
                                const uint8_t* tmemBase, uint32_t wrapMaskQW) {
    const uint32_t tmemOff = (wrapMaskQW > 0 && tmemBase) ? static_cast<uint32_t>((src - tmemBase) >> 3) : 0;
    const uint32_t lineQW = line >> 3;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = (wrapMaskQW > 0 && tmemBase)
            ? GetWrappedRow(tmemBase, tmemOff, lineQW, y, wrapMaskQW)
            : src + y * line;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t intensity = ReadTextureByte(row, x * 2 + 0, y, fromTMEM);
            uint8_t alpha = ReadTextureByte(row, x * 2 + 1, y, fromTMEM);
            dst[y * width + x] = (alpha << 24) | (intensity << 16) | (intensity << 8) | intensity;
        }
    }
}

void TextureCache::DecodeIA8(const uint8_t* src, uint32_t* dst,
                               uint32_t width, uint32_t height, uint32_t line, bool fromTMEM,
                               const uint8_t* tmemBase, uint32_t wrapMaskQW) {
    const uint32_t tmemOff = (wrapMaskQW > 0 && tmemBase) ? static_cast<uint32_t>((src - tmemBase) >> 3) : 0;
    const uint32_t lineQW = line >> 3;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = (wrapMaskQW > 0 && tmemBase)
            ? GetWrappedRow(tmemBase, tmemOff, lineQW, y, wrapMaskQW)
            : src + y * line;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t val = ReadTextureByte(row, x, y, fromTMEM);
            uint8_t intensity = (val >> 4) & 0xF;
            uint8_t alpha = val & 0xF;
            intensity = (intensity << 4) | intensity; // 4→8 bit
            alpha = (alpha << 4) | alpha;
            dst[y * width + x] = (alpha << 24) | (intensity << 16) | (intensity << 8) | intensity;
        }
    }
}

void TextureCache::DecodeIA4(const uint8_t* src, uint32_t* dst,
                               uint32_t width, uint32_t height, uint32_t line, bool fromTMEM,
                               const uint8_t* tmemBase, uint32_t wrapMaskQW) {
    const uint32_t tmemOff = (wrapMaskQW > 0 && tmemBase) ? static_cast<uint32_t>((src - tmemBase) >> 3) : 0;
    const uint32_t lineQW = line >> 3;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = (wrapMaskQW > 0 && tmemBase)
            ? GetWrappedRow(tmemBase, tmemOff, lineQW, y, wrapMaskQW)
            : src + y * line;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t byte = ReadTextureByte(row, x / 2, y, fromTMEM);
            uint8_t val = (x & 1) ? (byte & 0xF) : ((byte >> 4) & 0xF);

            uint8_t intensity = (val >> 1) & 0x7;
            uint8_t alpha = val & 0x1;
            intensity = (intensity << 5) | (intensity << 2) | (intensity >> 1);
            alpha = alpha ? 0xFF : 0x00;
            dst[y * width + x] = (alpha << 24) | (intensity << 16) | (intensity << 8) | intensity;
        }
    }
}

void TextureCache::DecodeI8(const uint8_t* src, uint32_t* dst,
                              uint32_t width, uint32_t height, uint32_t line, bool fromTMEM,
                              const uint8_t* tmemBase, uint32_t wrapMaskQW) {
    const uint32_t tmemOff = (wrapMaskQW > 0 && tmemBase) ? static_cast<uint32_t>((src - tmemBase) >> 3) : 0;
    const uint32_t lineQW = line >> 3;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = (wrapMaskQW > 0 && tmemBase)
            ? GetWrappedRow(tmemBase, tmemOff, lineQW, y, wrapMaskQW)
            : src + y * line;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t i = ReadTextureByte(row, x, y, fromTMEM);
            dst[y * width + x] = (i << 24) | (i << 16) | (i << 8) | i;
        }
    }
}

void TextureCache::DecodeI4(const uint8_t* src, uint32_t* dst,
                              uint32_t width, uint32_t height, uint32_t line, bool fromTMEM,
                              const uint8_t* tmemBase, uint32_t wrapMaskQW) {
    const uint32_t tmemOff = (wrapMaskQW > 0 && tmemBase) ? static_cast<uint32_t>((src - tmemBase) >> 3) : 0;
    const uint32_t lineQW = line >> 3;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = (wrapMaskQW > 0 && tmemBase)
            ? GetWrappedRow(tmemBase, tmemOff, lineQW, y, wrapMaskQW)
            : src + y * line;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t byte = ReadTextureByte(row, x / 2, y, fromTMEM);
            uint8_t val = (x & 1) ? (byte & 0xF) : ((byte >> 4) & 0xF);
            uint8_t i = (val << 4) | val;
            dst[y * width + x] = (i << 24) | (i << 16) | (i << 8) | i;
        }
    }
}

void TextureCache::DecodeCI8(const uint8_t* src, const uint8_t* tmemBase, uint32_t* dst,
                               uint32_t width, uint32_t height, uint32_t line, bool fromTMEM, bool isIA,
                               uint32_t wrapMaskQW) {
    if (!tmemBase) return;

    const uint32_t tmemOff = (wrapMaskQW > 0) ? static_cast<uint32_t>((src - tmemBase) >> 3) : 0;
    const uint32_t lineQW = line >> 3;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = (wrapMaskQW > 0)
            ? GetWrappedRow(tmemBase, tmemOff, lineQW, y, wrapMaskQW)
            : src + y * line;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t idx = ReadTextureByte(row, x, y, fromTMEM);
            // CI8: palette at TMEM_PALETTE_BASE, 8-byte stride (GLideN64 TLUT layout)
            uint32_t addr = TMEM_PALETTE_BASE + static_cast<uint32_t>(idx) * 8;
            uint16_t color = (static_cast<uint16_t>(tmemBase[addr]) << 8) | tmemBase[addr + 1];
            dst[y * width + x] = isIA ? IA88toRGBA8(color) : RGBA5551toRGBA8(color);
        }
    }
}

void TextureCache::DecodeCI4(const uint8_t* src, const uint8_t* tmemBase, uint8_t palette,
                               uint32_t* dst, uint32_t width, uint32_t height, uint32_t line, bool fromTMEM, bool isIA,
                               uint32_t wrapMaskQW) {
    if (!tmemBase) return;

    const uint32_t tmemOff = (wrapMaskQW > 0) ? static_cast<uint32_t>((src - tmemBase) >> 3) : 0;
    const uint32_t lineQW = line >> 3;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = (wrapMaskQW > 0)
            ? GetWrappedRow(tmemBase, tmemOff, lineQW, y, wrapMaskQW)
            : src + y * line;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t byte = ReadTextureByte(row, x / 2, y, fromTMEM);
            uint8_t idx = (x & 1) ? (byte & 0xF) : ((byte >> 4) & 0xF);

            // CI4: palette at TMEM_PALETTE_BASE + palette * 0x80, 8-byte stride (GLideN64 TLUT layout)
            uint32_t addr = TMEM_PALETTE_BASE + static_cast<uint32_t>(palette) * 0x80 +
                            static_cast<uint32_t>(idx) * 8;
            uint16_t color = (static_cast<uint16_t>(tmemBase[addr]) << 8) | tmemBase[addr + 1];
            dst[y * width + x] = isIA ? IA88toRGBA8(color) : RGBA5551toRGBA8(color);
        }
    }
}

void TextureCache::DecodeYUV(const uint8_t* src, uint32_t* dst,
                               uint32_t width, uint32_t height, uint32_t line, bool fromTMEM,
                               const uint8_t* tmemBase, uint32_t wrapMaskQW) {
    // YUV422: 2 pixels per 32 bits (Y0 U Y1 V)
    // Store raw Y/U/V in R/G/B channels — the shader performs the actual
    // YUV→RGB conversion using the N64's programmable K coefficients.
    const uint32_t tmemOff = (wrapMaskQW > 0 && tmemBase) ? static_cast<uint32_t>((src - tmemBase) >> 3) : 0;
    const uint32_t lineQW = line >> 3;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = (wrapMaskQW > 0 && tmemBase)
            ? GetWrappedRow(tmemBase, tmemOff, lineQW, y, wrapMaskQW)
            : src + y * line;
        for (uint32_t x = 0; x < width; x += 2) {
            uint8_t y0 = ReadTextureByte(row, x * 2 + 0, y, fromTMEM);
            uint8_t u  = ReadTextureByte(row, x * 2 + 1, y, fromTMEM);
            uint8_t y1 = ReadTextureByte(row, x * 2 + 2, y, fromTMEM);
            uint8_t v  = ReadTextureByte(row, x * 2 + 3, y, fromTMEM);

            // RGBA8 layout: R=Y, G=U(Cb), B=V(Cr), A=255
            dst[y * width + x + 0] = (0xFF << 24) | (v << 16) | (u << 8) | y0;
            if (x + 1 < width)
                dst[y * width + x + 1] = (0xFF << 24) | (v << 16) | (u << 8) | y1;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  UTILITY FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════════

uint32_t TextureCache::CalculateTextureCRC(const uint8_t* data, uint32_t sizeBytes) {
    // Simple CRC32 for texture content hashing
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < sizeBytes; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return ~crc;
}

uint32_t TextureCache::CalculateTMEMSize(const TextureKey& key) const {
    uint32_t bitsPerPixel = 0;
    switch (key.size) {
        case N64TexSize::Bits4:  bitsPerPixel = 4; break;
        case N64TexSize::Bits8:  bitsPerPixel = 8; break;
        case N64TexSize::Bits16: bitsPerPixel = 16; break;
        case N64TexSize::Bits32: bitsPerPixel = 32; break;
        default: bitsPerPixel = 16; break;
    }
    return (key.width * key.height * bitsPerPixel) / 8;
}

uint32_t TextureCache::NextPow2(uint32_t v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

uint32_t TextureCache::MaxMipLevels(uint32_t w, uint32_t h) {
    uint32_t dim = std::max(w, h);
    if (dim == 0) return 1;
    uint32_t levels = 1;
    while (dim > 1) { dim >>= 1; levels++; }
    return levels;
}

void TextureCache::SetHiResPack(std::shared_ptr<const HiResPack> pack) {
    m_hiResPack = std::move(pack);
    m_config.enableHiResTextures = HasHiResPack();
    // Pictures are several times larger than the textures they replace.
    if (m_config.enableHiResTextures)
        m_config.maxCacheBytes = std::max<uint64_t>(m_config.maxCacheBytes, 1024ull * 1024 * 1024);
    InvalidateAll();
}

void TextureCache::SetDumpFolder(const std::filesystem::path& folder, const std::string& gameName) {
    m_dumpFolder = folder;
    m_dumpGameName = gameName;
    m_dumped.clear();
}

void TextureCache::DumpTexture(const TextureKey& key, const std::vector<uint32_t>& pixels) {
    if (key.hiresTexCrc == 0 && key.hiresPalCrc == 0) return;
    const uint64_t identity = ((static_cast<uint64_t>(key.hiresPalCrc) << 32) | key.hiresTexCrc)
                            ^ (static_cast<uint64_t>(key.format) << 60) ^ (static_cast<uint64_t>(key.size) << 56);
    if (!m_dumped.insert(identity).second) return;

    // The picture covers the region the checksum was computed over, which can be smaller
    // than the decoded tile (clamped textures are padded out to the sampled size).
    const uint32_t w = (key.hiresWidth  != 0 && key.hiresWidth  <= key.width)  ? key.hiresWidth  : key.width;
    const uint32_t h = (key.hiresHeight != 0 && key.hiresHeight <= key.height) ? key.hiresHeight : key.height;
    if (w == 0 || h == 0 || pixels.size() < static_cast<size_t>(key.width) * key.height) return;

    char name[512];
    if (key.hiresPalCrc != 0) {
        std::snprintf(name, sizeof(name), "%s#%08X#%X#%X#%08X_all.png", m_dumpGameName.c_str(),
                      key.hiresTexCrc, static_cast<unsigned>(key.format), static_cast<unsigned>(key.size), key.hiresPalCrc);
    } else {
        std::snprintf(name, sizeof(name), "%s#%08X#%X#%X_all.png", m_dumpGameName.c_str(),
                      key.hiresTexCrc, static_cast<unsigned>(key.format), static_cast<unsigned>(key.size));
    }
    std::error_code ec;
    std::filesystem::create_directories(m_dumpFolder, ec);
    const std::filesystem::path file = m_dumpFolder / name;
    if (std::filesystem::exists(file, ec)) return;

    std::vector<uint32_t> cropped;
    const uint32_t* src = pixels.data();
    if (w != key.width || h != key.height) {
        cropped.resize(static_cast<size_t>(w) * h);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(&cropped[static_cast<size_t>(y) * w], &pixels[static_cast<size_t>(y) * key.width], w * sizeof(uint32_t));
        src = cropped.data();
    }
    Nova::WritePNG(file.string().c_str(), static_cast<int>(w), static_cast<int>(h), 4, src);
}

bool TextureCache::LoadHiResTexture(CachedTexture& entry, const TextureKey& key) {
    if (!HasHiResPack() || (key.hiresTexCrc == 0 && key.hiresPalCrc == 0)) return false;
    // A picture is drawn over the whole texture, so the region its checksum covered has to
    // be the whole texture; otherwise it would be stretched.
    if (key.hiresWidth != key.width || key.hiresHeight != key.height) return false;

    HiResId id;
    id.texCrc = key.hiresTexCrc;
    id.palCrc = key.hiresPalCrc;
    id.format = static_cast<uint8_t>(key.format);
    id.size   = static_cast<uint8_t>(key.size);
    const std::unique_ptr<HiResImage> image = m_hiResPack->Find(id);
    if (!image || image->width == 0 || image->height == 0) return false;

    TextureDesc desc;
    desc.width  = image->width;
    desc.height = image->height;
    desc.format = TextureFormat::RGBA8;
    desc.usage  = BufferUsage::Static;
    desc.mipLevels = m_config.enableMipmaps ? std::min<uint32_t>(5, MaxMipLevels(desc.width, desc.height)) : 1;

    auto gpuTex = m_renderer->CreateTexture(desc);
    if (!gpuTex) return false;
    gpuTex->Upload(image->rgba.data(), image->rgba.size());
    if (desc.mipLevels > 1) gpuTex->GenerateMipmaps();

    // The shader does clamping, mirroring and masking itself, on continuous coordinates
    // (n64ReadHiRes); repeat lets it read across the seam where a tile wraps.
    SamplerDesc sampler;
    sampler.minFilter = desc.mipLevels > 1 ? SamplerFilter::LinearMipLinear : SamplerFilter::Linear;
    sampler.magFilter = SamplerFilter::Linear;
    sampler.wrapS = TextureWrap::Repeat;
    sampler.wrapT = TextureWrap::Repeat;
    sampler.maxAnisotropy = std::max(m_config.anisotropicLevel, 4.0f);
    sampler.lodBias = m_config.lodBias;
    gpuTex->SetSampler(sampler);

    entry.key = key;
    entry.gpuTexture = std::move(gpuTex);
    entry.realWidth  = image->width;
    entry.realHeight = image->height;
    entry.sizeBytes  = image->width * image->height * 4 * desc.mipLevels;
    entry.isHiRes    = true;
    entry.hiresWidth  = key.hiresWidth  ? key.hiresWidth  : key.width;
    entry.hiresHeight = key.hiresHeight ? key.hiresHeight : key.height;
    entry.scaleS = 1.0f;
    entry.scaleT = 1.0f;

    m_stats.hiResLoaded++;
    return true;
}

}}} // namespace Nova64::Video::Tex
