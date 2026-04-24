/**
 * @file TextureCache.cpp
 * @brief Texture Cache.
 */
// SPDX-License-Identifier: MIT
// NovaGraphics - Texture Cache Implementation
// N64 texture decoding, caching, and GPU upload

#include "TextureCache.hpp"
#include "HiResTextures.hpp"
#include "../Common/BitUtils.hpp"
#include "../Common/Logger.hpp"
#include "../Common/ColorSpace.hpp"
#include "../../common/log.hpp"

#include <cstring>
#include <algorithm>
#include <cassert>
#include <sstream>
#include <iomanip>
#include <filesystem>

namespace Nova64 { namespace Video { namespace Tex {

// ═══════════════════════════════════════════════════════════════════════════════
//  TEXTURE CACHE LIFECYCLE
// ═══════════════════════════════════════════════════════════════════════════════

TextureCache::TextureCache() = default;
TextureCache::~TextureCache() { Shutdown(); }

bool TextureCache::Initialize(IRenderer* renderer, const TextureCacheConfig& config) {
    m_renderer = renderer;
    m_config = config;
    m_frameCounter = 0;
    m_stats = {};
    m_cache.reserve(config.maxEntries);
    return true;
}

void TextureCache::Shutdown() {
    m_cache.clear();
    m_lruList.clear();
    m_stats = {};
    m_renderer = nullptr;
}

void TextureCache::BeginFrame() {
    m_frameCounter++;
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

CachedTexture* TextureCache::GetTexture(const TextureKey& key, const uint8_t* rdram,
                                          const uint8_t* tmem) {
    // Look up in cache
    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        // Cache hit — update LRU
        m_stats.hits++;
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

    // Calculate GPU texture dimensions (may round up to power of 2)
    uint32_t gpuWidth = key.width;
    uint32_t gpuHeight = key.height;

    // Some GPUs prefer power-of-2 textures for mipmapping
    if (m_config.enableMipmaps) {
        gpuWidth = NextPow2(key.width);
        gpuHeight = NextPow2(key.height);
    }

    // Create GPU texture
    TextureDesc desc;
    desc.width = gpuWidth;
    desc.height = gpuHeight;
    desc.format = TextureFormat::RGBA8;
    desc.usage = BufferUsage::Dynamic;
    desc.mipLevels = m_config.enableMipmaps ? 4 : 1;

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
    auto translateWrap = [](uint8_t mode) -> TextureWrap {
        if (mode & 0x2) return TextureWrap::ClampToEdge;
        if (mode & 0x1) return TextureWrap::MirroredRepeat;
        return TextureWrap::Repeat;
    };
    samplerDesc.wrapS = translateWrap(key.cms);
    samplerDesc.wrapT = translateWrap(key.cmt);
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

    // Determine actual mip level count: base + mipCount additional levels
    int totalLevels = 1 + std::min(mipCount, 7);

    uint32_t gpuWidth = key.width;
    uint32_t gpuHeight = key.height;
    if (m_config.enableMipmaps) {
        gpuWidth = NextPow2(key.width);
        gpuHeight = NextPow2(key.height);
    }

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
    auto translateWrap = [](uint8_t mode) -> TextureWrap {
        if (mode & 0x2) return TextureWrap::ClampToEdge;
        if (mode & 0x1) return TextureWrap::MirroredRepeat;
        return TextureWrap::Repeat;
    };
    samplerDesc.wrapS = translateWrap(key.cms);
    samplerDesc.wrapT = translateWrap(key.cmt);
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
            // Overlap — invalidate
            m_stats.currentBytes -= it->second.texture.sizeBytes;
            m_stats.currentEntries--;
            m_stats.evictions++;
            m_lruList.erase(it->second.lruIterator);
            it = m_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void TextureCache::InvalidateAll() {
    m_stats.evictions += m_stats.currentEntries;
    m_stats.currentEntries = 0;
    m_stats.currentBytes = 0;
    m_cache.clear();
    m_lruList.clear();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  EVICTION
// ═══════════════════════════════════════════════════════════════════════════════

void TextureCache::EvictStale(uint64_t currentFrame) {
    // Evict textures not used in the last 300 frames (~5 seconds)
    const uint64_t staleThreshold = 300;

    auto it = m_lruList.rbegin();
    while (it != m_lruList.rend()) {
        auto cacheIt = m_cache.find(*it);
        if (cacheIt != m_cache.end()) {
            if (currentFrame - cacheIt->second.texture.lastUsedFrame > staleThreshold) {
                m_stats.currentBytes -= cacheIt->second.texture.sizeBytes;
                m_stats.currentEntries--;
                m_stats.evictions++;
                auto toErase = std::next(it).base();
                m_cache.erase(cacheIt);
                m_lruList.erase(toErase);
                it = m_lruList.rbegin(); // Reset after modification
                continue;
            }
        }
        ++it;
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

    // When data points to the full TMEM base, offset to this tile's start.
    // key.tmemOffset is the tile's tmemAddr (in 64-bit words).
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

    switch (key.format) {
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
            if (key.size == N64TexSize::Bits8)
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

bool TextureCache::LoadHiResTexture(CachedTexture& entry, const TextureKey& key) {
    if (!m_config.enableHiResTextures || m_config.hiResTexturePath.empty())
        return false;

    // Look up hi-res texture by CRC (the standard pack naming convention)
    // Texture pack files: <packPath>/<CRC32>_<fmt>_<size>.png  or  <CRC32>.png
    // We search for common naming patterns: CRC32 hex string in filename

    // Build glob pattern from CRC
    std::ostringstream crcHex;
    crcHex << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << key.crc;
    std::string crcStr = crcHex.str();

    // Common hi-res texture pack file naming conventions
    static const char* extensions[] = { ".png", ".dds", ".bmp", ".tga", ".ktx" };
    namespace fs = std::filesystem;

    std::string foundPath;
    for (const auto& ext : extensions) {
        // Pattern 1: <CRC>.ext
        fs::path p1 = fs::path(m_config.hiResTexturePath) / (crcStr + ext);
        if (fs::exists(p1)) { foundPath = p1.string(); break; }

        // Pattern 2: <CRC>_all.ext (Rice-style packs)
        fs::path p2 = fs::path(m_config.hiResTexturePath) / (crcStr + "_all" + ext);
        if (fs::exists(p2)) { foundPath = p2.string(); break; }

        // Pattern 3: <CRC>_rgb.ext + <CRC>_a.ext (separate alpha)
        fs::path p3 = fs::path(m_config.hiResTexturePath) / (crcStr + "_rgb" + ext);
        if (fs::exists(p3)) { foundPath = p3.string(); break; }
    }

    if (foundPath.empty())
        return false;

    // Use HiResTextures loader (static utility)
    HiResTextureData hiResData;
    std::string extLower = fs::path(foundPath).extension().string();
    std::transform(extLower.begin(), extLower.end(), extLower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    bool loaded = false;
    HiResTextures hiResLoader;
    if (extLower == ".png") loaded = hiResLoader.LoadPNG(foundPath, hiResData);
    else if (extLower == ".bmp") loaded = hiResLoader.LoadBMP(foundPath, hiResData);
    else if (extLower == ".tga") loaded = hiResLoader.LoadTGA(foundPath, hiResData);
    else if (extLower == ".dds") loaded = hiResLoader.LoadDDS(foundPath, hiResData);
    else if (extLower == ".ktx") loaded = hiResLoader.LoadKTX(foundPath, hiResData);

    if (!loaded || hiResData.pixels.empty()) return false;

    uint32_t hiW = hiResData.info.width;
    uint32_t hiH = hiResData.info.height;

    // Create GPU texture from hi-res data
    TextureDesc desc;
    desc.width = hiW;
    desc.height = hiH;
    desc.format = TextureFormat::RGBA8;
    desc.usage = BufferUsage::Static;
    desc.mipLevels = m_config.enableMipmaps ? 4 : 1;

    auto gpuTex = m_renderer->CreateTexture(desc);
    if (!gpuTex) return false;

    gpuTex->Upload(hiResData.pixels.data(), hiResData.pixels.size());

    if (m_config.enableMipmaps && desc.mipLevels > 1)
        gpuTex->GenerateMipmaps();

    // Set sampler state (hi-res textures look best with linear filtering)
    SamplerDesc samplerDesc;
    samplerDesc.minFilter = SamplerFilter::LinearMipLinear;
    samplerDesc.magFilter = SamplerFilter::Linear;
    auto translateWrap = [](uint8_t mode) -> TextureWrap {
        if (mode & 0x2) return TextureWrap::ClampToEdge;
        if (mode & 0x1) return TextureWrap::MirroredRepeat;
        return TextureWrap::Repeat;
    };
    samplerDesc.wrapS = translateWrap(key.cms);
    samplerDesc.wrapT = translateWrap(key.cmt);
    samplerDesc.maxAnisotropy = std::max(m_config.anisotropicLevel, 4.0f);
    samplerDesc.lodBias = m_config.lodBias;
    gpuTex->SetSampler(samplerDesc);

    entry.key = key;
    entry.gpuTexture = std::move(gpuTex);
    entry.realWidth = hiW;
    entry.realHeight = hiH;
    entry.sizeBytes = hiW * hiH * 4 * desc.mipLevels;
    entry.isHiRes = true;
    entry.scaleS = static_cast<float>(key.width) / hiW;
    entry.scaleT = static_cast<float>(key.height) / hiH;

    m_stats.hiResLoaded++;
    LOG_DEBUG("HiRes texture loaded: {} ({}x{} -> {}x{})", foundPath, key.width, key.height, hiW, hiH);
    return true;
}

}}} // namespace Nova64::Video::Tex
