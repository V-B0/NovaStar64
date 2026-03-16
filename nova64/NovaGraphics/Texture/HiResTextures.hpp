// SPDX-License-Identifier: MIT
// NovaGraphics - Hi-Res Texture Loader
// Load high-resolution replacement textures from PNG/DDS/KTX packs

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>

namespace Nova64 { namespace Video { namespace Tex {

// ═══════════════════════════════════════════════════════════════════════════════
//  HI-RES TEXTURE FORMATS
// ═══════════════════════════════════════════════════════════════════════════════

enum class HiResFormat : uint8_t {
    PNG  = 0,    // Uncompressed RGBA (loaded via stb_image or similar)
    DDS  = 1,    // DirectDraw Surface (DXT/BC compressed)
    KTX  = 2,    // Khronos Texture (Vulkan-native format)
    BMP  = 3,    // Bitmap
    TGA  = 4,    // Targa
    Count
};

enum class HiResCompression : uint8_t {
    None    = 0, // Uncompressed RGBA
    DXT1    = 1, // BC1 (RGB, 1-bit alpha)
    DXT3    = 2, // BC2 (RGBA, 4-bit alpha)
    DXT5    = 3, // BC3 (RGBA, interpolated alpha)
    BC7     = 4, // BC7 (high-quality RGBA)
    Count
};

// ═══════════════════════════════════════════════════════════════════════════════
//  HI-RES TEXTURE ENTRY
// ═══════════════════════════════════════════════════════════════════════════════

struct HiResTextureInfo {
    uint32_t crc;               // Matches the N64 texture CRC
    uint32_t width;
    uint32_t height;
    uint32_t mipLevels;
    HiResFormat format;
    HiResCompression compression;
    uint32_t sizeBytes;         // Size of pixel data
    std::string filePath;       // Path to the file on disk
    bool hasAlpha;
};

struct HiResTextureData {
    HiResTextureInfo info;
    std::vector<uint8_t> pixels; // Loaded pixel data (RGBA8 or compressed)
    std::vector<std::vector<uint8_t>> mipData; // Per-mip data for DDS/KTX
};

// ═══════════════════════════════════════════════════════════════════════════════
//  HI-RES TEXTURE PACK
// ═══════════════════════════════════════════════════════════════════════════════

struct HiResTexturePack {
    std::string name;
    std::string path;           // Root directory of the pack
    std::string gameId;         // ROM CRC or game identifier
    uint32_t textureCount = 0;
    uint64_t totalSizeBytes = 0;
    bool isLoaded = false;
};

// ═══════════════════════════════════════════════════════════════════════════════
//  HI-RES TEXTURE CACHE STATS
// ═══════════════════════════════════════════════════════════════════════════════

struct HiResStats {
    uint32_t packsFound = 0;
    uint32_t texturesIndexed = 0;
    uint32_t texturesLoaded = 0;
    uint64_t memoryUsed = 0;
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;
};

// ═══════════════════════════════════════════════════════════════════════════════
//  HI-RES TEXTURE MANAGER
// ═══════════════════════════════════════════════════════════════════════════════

class HiResTextures {
public:
    HiResTextures();
    ~HiResTextures();

    /// Initialize with search paths
    bool Initialize(const std::string& searchPath, const std::string& gameId);
    void Shutdown();

    /// Scan directories for texture packs
    void ScanForPacks(const std::string& searchPath);

    /// Load/index a specific texture pack
    bool LoadPack(const std::string& packPath);

    /// Check if a hi-res replacement exists for a given CRC
    bool HasTexture(uint32_t crc) const;

    /// Load a hi-res texture by CRC
    /// Returns nullptr if not found
    std::unique_ptr<HiResTextureData> LoadTexture(uint32_t crc);

    /// Get info about a texture without loading pixel data
    const HiResTextureInfo* GetTextureInfo(uint32_t crc) const;

    /// Preload textures (background loading)
    void PreloadAll();

    /// Release all loaded pixel data (keep index)
    void ReleasePixelData();

    /// Get list of available packs
    const std::vector<HiResTexturePack>& GetPacks() const { return m_packs; }

    /// Statistics
    const HiResStats& GetStats() const { return m_stats; }

    /// Load image file into RGBA8 pixels (public for use by TextureCache)
    bool LoadPNG(const std::string& path, HiResTextureData& out);
    bool LoadBMP(const std::string& path, HiResTextureData& out);
    bool LoadTGA(const std::string& path, HiResTextureData& out);
    bool LoadDDS(const std::string& path, HiResTextureData& out);
    bool LoadKTX(const std::string& path, HiResTextureData& out);

private:
    /// Parse a CRC from a filename like "00112233_fmt.png"
    static bool ParseCRCFromFilename(const std::string& filename, uint32_t& outCRC);

    /// Detect format from file extension
    static HiResFormat DetectFormat(const std::string& path);

    /// Index a single file
    void IndexFile(const std::string& filePath, const std::string& relativePath);

    /// Index directory recursively
    void IndexDirectory(const std::string& dirPath, const std::string& relativePath);

    std::string m_gameId;
    std::string m_searchPath;
    std::vector<HiResTexturePack> m_packs;

    // CRC → texture info mapping
    std::unordered_map<uint32_t, HiResTextureInfo> m_textureIndex;

    HiResStats m_stats;
};

}}} // namespace Nova64::Video::Tex
