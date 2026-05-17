/**
 * @file TextureCache.hpp
 * @brief Texture Cache.
 */
// SPDX-License-Identifier: MIT
// NovaGraphics - Texture Cache
// N64 TMEM-to-GPU texture management, hash-based deduplication, LRU eviction

#pragma once

#include "../Core/N64Types.hpp"
#include "../Renderer/Renderer.hpp"
#include "../Common/Logger.hpp"

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <memory>
#include <string>
#include <functional>
#include <array>
#include <list>
#include <deque>

namespace Nova64 { namespace Video { namespace Tex {

// ═══════════════════════════════════════════════════════════════════════════════
//  N64 TEXTURE FORMAT DEFINITIONS
// ═══════════════════════════════════════════════════════════════════════════════

enum class N64TexFormat : uint8_t {
    RGBA = 0,   // 16b or 32b RGBA
    YUV  = 1,   // YUV (FMV: Jet Force Gemini, etc.)
    CI   = 2,   // Color-indexed (palette lookup)
    IA   = 3,   // Intensity + Alpha
    I    = 4,   // Intensity only
    Count = 5
};

enum class N64TexSize : uint8_t {
    Bits4  = 0,
    Bits8  = 1,
    Bits16 = 2,
    Bits32 = 3,
    Count  = 4
};

// ═══════════════════════════════════════════════════════════════════════════════
//  TEXTURE HASH / KEY
// ═══════════════════════════════════════════════════════════════════════════════

struct TextureKey {
    uint32_t address;       // RDRAM address
    uint32_t palette;       // TLUT palette address (for CI textures)
    uint16_t width;
    uint16_t height;
    N64TexFormat format;
    N64TexSize size;
    uint8_t  cms;           // Clamp/Mirror/Wrap S
    uint8_t  cmt;           // Clamp/Mirror/Wrap T
    uint8_t  maskS;
    uint8_t  maskT;
    uint8_t  shiftS;
    uint8_t  shiftT;
    uint16_t line;          // Tile line size (bytes per row / 8)
    uint16_t tmemOffset;    // Tile base offset in TMEM (in 64-bit words = tmemAddr)
    uint8_t  tlutType;      // 0=none, 2=RGBA16, 3=IA16 (from otherModeH bits 15:14)
    uint8_t  mipLevels;     // Number of mip levels in tile chain (0 = base only)
    uint64_t crc;           // Content hash for change detection

    bool operator==(const TextureKey& other) const {
        return address == other.address &&
               palette == other.palette &&
               width == other.width &&
               height == other.height &&
               format == other.format &&
               size == other.size &&
               cms == other.cms &&
               cmt == other.cmt &&
               maskS == other.maskS &&
               maskT == other.maskT &&
               shiftS == other.shiftS &&
               shiftT == other.shiftT &&
               line == other.line &&
               tmemOffset == other.tmemOffset &&
               tlutType == other.tlutType &&
               mipLevels == other.mipLevels &&
               crc == other.crc;
    }
};

struct TextureKeyHash {
    size_t operator()(const TextureKey& k) const {
        size_t h = std::hash<uint32_t>{}(k.address);
        h ^= std::hash<uint32_t>{}(k.palette) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint64_t>{}(k.crc) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint16_t>{}(k.width) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint16_t>{}(k.height) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(k.format)) << 16;
        h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(k.size)) << 20;
        h ^= std::hash<uint8_t>{}(k.cms) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>{}(k.cmt) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>{}(k.maskS) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>{}(k.maskT) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>{}(k.shiftS) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>{}(k.shiftT) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint16_t>{}(k.line) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint16_t>{}(k.tmemOffset) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>{}(k.tlutType) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>{}(k.mipLevels) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  CACHED TEXTURE ENTRY
// ═══════════════════════════════════════════════════════════════════════════════

struct CachedTexture {
    TextureKey key;
    std::unique_ptr<ITexture> gpuTexture;
    uint32_t realWidth = 0;      // Actual texture width (may be power of 2)
    uint32_t realHeight = 0;     // Actual texture height
    uint32_t sizeBytes = 0;      // GPU memory used
    uint64_t lastUsedFrame = 0;  // LRU tracking
    uint32_t useCount = 0;       // Frequency tracking
    bool isHiRes = false;        // Hi-res texture pack replacement
    bool needsUpdate = false;    // Content changed in RDRAM

    // Texture coordinate scale (realWidth may differ from N64 width)
    float scaleS = 1.0f;
    float scaleT = 1.0f;
};

// ═══════════════════════════════════════════════════════════════════════════════
//  TEXTURE CACHE STATISTICS
// ═══════════════════════════════════════════════════════════════════════════════

struct TextureCacheStats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t evictions = 0;
    uint64_t uploads = 0;
    uint64_t conversions = 0;
    uint32_t currentEntries = 0;
    uint64_t currentBytes = 0;
    uint64_t peakBytes = 0;
    uint32_t hiResLoaded = 0;

    float GetHitRate() const {
        uint64_t total = hits + misses;
        return total > 0 ? static_cast<float>(hits) / total : 0.0f;
    }

    void Reset() {
        hits = misses = evictions = uploads = conversions = 0;
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  TEXTURE CACHE CONFIGURATION
// ═══════════════════════════════════════════════════════════════════════════════

struct TextureCacheConfig {
    uint64_t maxCacheBytes = 256 * 1024 * 1024;  // 256MB max GPU texture memory
    uint32_t maxEntries = 8192;                    // Max cached textures
    bool enableMipmaps = true;
    bool enableHiResTextures = false;
    bool enable3PointFilter = true;
    bool forceLinearFilter = false;
    bool force16bpp = false;                       // Force RGBA4444 for memory savings
    float anisotropicLevel = 1.0f;                 // 1.0 = off, up to 16.0
    float lodBias = 0.0f;                          // GL_TEXTURE_LOD_BIAS, set from game profile
    std::string hiResTexturePath;                  // Path to hi-res texture packs
};

// ═══════════════════════════════════════════════════════════════════════════════
//  TEXTURE CACHE
// ═══════════════════════════════════════════════════════════════════════════════

class TextureCache {
public:
    TextureCache();
    ~TextureCache();

    /// Initialize with renderer and config
    bool Initialize(IRenderer* renderer, const TextureCacheConfig& config);
    void Shutdown();

    /// Look up or create a texture from N64 state
    CachedTexture* GetTexture(const TextureKey& key, const uint8_t* rdram, const uint8_t* tmem);

    /// Mip tile descriptor for tile-chain mipmapping
    struct MipTileInfo {
        uint16_t tmemAddr;  // TMEM word address
        uint16_t line;      // Stride in 64-bit words
        uint16_t width;
        uint16_t height;
        uint8_t  format;
        uint8_t  size;
        uint8_t  palette;
    };

    /// Look up or create a mipmapped texture with per-level N64 tile data
    CachedTexture* GetTexture(const TextureKey& key, const uint8_t* rdram,
                              const uint8_t* tmem, const MipTileInfo* mipTiles, int mipCount);

    /// Invalidate textures that might have been modified by CPU writes
    void InvalidateRange(uint32_t addressStart, uint32_t sizeBytes);

    /// Invalidate entire cache (scene change, etc.)
    void InvalidateAll();

    /// Evict old textures to stay within memory budget
    void EvictStale(uint64_t currentFrame);

    /// Frame boundary — update stats, advance frame counter
    void BeginFrame();

    /// Get statistics
    const TextureCacheStats& GetStats() const { return m_stats; }

    /// Get cache config (for debug display)
    const TextureCacheConfig& GetConfig() const { return m_config; }

    /// Update config at runtime
    void SetConfig(const TextureCacheConfig& config);

private:
    /// Decode N64 texture data into RGBA8888
    void DecodeTexture(const TextureKey& key, const uint8_t* tmem,
                        std::vector<uint32_t>& outPixels, bool fromTMEM);

    /// Decode specific N64 formats
    void DecodeRGBA16(const uint8_t* src, uint32_t* dst, uint32_t width, uint32_t height, uint32_t line, bool fromTMEM, const uint8_t* tmemBase = nullptr, uint32_t wrapMaskQW = 0);
    void DecodeRGBA32(const uint8_t* src, uint32_t* dst, uint32_t width, uint32_t height, uint32_t lineBytes, bool fromTMEM);
    void DecodeRGBA32_TMEM(const uint8_t* tmemBase, uint16_t tmemAddr, uint16_t tileLine,
                           uint32_t* dst, uint32_t width, uint32_t height);
    void DecodeIA16(const uint8_t* src, uint32_t* dst, uint32_t width, uint32_t height, uint32_t line, bool fromTMEM, const uint8_t* tmemBase = nullptr, uint32_t wrapMaskQW = 0);
    void DecodeIA8(const uint8_t* src, uint32_t* dst, uint32_t width, uint32_t height, uint32_t line, bool fromTMEM, const uint8_t* tmemBase = nullptr, uint32_t wrapMaskQW = 0);
    void DecodeIA4(const uint8_t* src, uint32_t* dst, uint32_t width, uint32_t height, uint32_t line, bool fromTMEM, const uint8_t* tmemBase = nullptr, uint32_t wrapMaskQW = 0);
    void DecodeI8(const uint8_t* src, uint32_t* dst, uint32_t width, uint32_t height, uint32_t line, bool fromTMEM, const uint8_t* tmemBase = nullptr, uint32_t wrapMaskQW = 0);
    void DecodeI4(const uint8_t* src, uint32_t* dst, uint32_t width, uint32_t height, uint32_t line, bool fromTMEM, const uint8_t* tmemBase = nullptr, uint32_t wrapMaskQW = 0);
    void DecodeCI8(const uint8_t* src, const uint8_t* tmemBase, uint32_t* dst,
                    uint32_t width, uint32_t height, uint32_t line, bool fromTMEM, bool isIA = false, uint32_t wrapMaskQW = 0);
    void DecodeCI4(const uint8_t* src, const uint8_t* tmemBase, uint8_t palette,
                    uint32_t* dst, uint32_t width, uint32_t height, uint32_t line, bool fromTMEM, bool isIA = false, uint32_t wrapMaskQW = 0);
    void DecodeYUV(const uint8_t* src, uint32_t* dst, uint32_t width, uint32_t height, uint32_t line, bool fromTMEM, const uint8_t* tmemBase = nullptr, uint32_t wrapMaskQW = 0);

    /// Calculate CRC for texture data in RDRAM/TMEM
    uint32_t CalculateTextureCRC(const uint8_t* data, uint32_t sizeBytes);

    /// Calculate texture size in TMEM bytes
    uint32_t CalculateTMEMSize(const TextureKey& key) const;

    /// Get the next power of 2 >= value
    static uint32_t NextPow2(uint32_t v);

    /// LRU eviction — remove least recently used texture
    void EvictLRU();

    /// Load hi-res texture replacement
    bool LoadHiResTexture(CachedTexture& entry, const TextureKey& key);

    IRenderer* m_renderer = nullptr;
    TextureCacheConfig m_config;
    TextureCacheStats m_stats;
    uint64_t m_frameCounter = 0;

    // LRU list: front = most recently used, back = least recently used
    using LRUList = std::list<TextureKey>;
    LRUList m_lruList;

    struct CacheEntry {
        CachedTexture texture;
        LRUList::iterator lruIterator;
    };

    std::unordered_map<TextureKey, CacheEntry, TextureKeyHash> m_cache;

    // Deferred-destruction zombie queue.
    // Textures evicted from the cache are held here for kZombieFrameDelay frames
    // before their GPU resources are actually freed. This ensures any in-flight
    // GPU command buffers that still reference the texture's VkImageView have
    // completed before the view is destroyed (avoids GPU fault / TDR).
    static constexpr uint64_t kZombieFrameDelay = 3; // kFramesInFlight + 1
    struct Zombie {
        std::unique_ptr<ITexture> gpuTex;
        uint64_t safeDestroyFrame; // m_frameCounter value at which it's safe to destroy
    };
    std::deque<Zombie> m_zombies;
};

}}} // namespace Nova64::Video::Tex
