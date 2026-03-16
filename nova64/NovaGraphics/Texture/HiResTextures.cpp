// SPDX-License-Identifier: MIT
// NovaGraphics - Hi-Res Texture Loader Implementation
// Loads PNG, BMP, TGA, DDS texture replacements from disk

#include "HiResTextures.hpp"
#include "../Common/Logger.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <cctype>

#include "stb_image.h"

namespace fs = std::filesystem;

namespace Nova64 { namespace Video { namespace Tex {

// ═══════════════════════════════════════════════════════════════════════════════
//  LIFECYCLE
// ═══════════════════════════════════════════════════════════════════════════════

HiResTextures::HiResTextures() = default;
HiResTextures::~HiResTextures() { Shutdown(); }

bool HiResTextures::Initialize(const std::string& searchPath, const std::string& gameId) {
    m_searchPath = searchPath;
    m_gameId = gameId;
    m_stats = {};

    // Scan for texture packs matching this game
    ScanForPacks(searchPath);

    return true;
}

void HiResTextures::Shutdown() {
    m_textureIndex.clear();
    m_packs.clear();
    m_stats = {};
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PACK SCANNING
// ═══════════════════════════════════════════════════════════════════════════════

void HiResTextures::ScanForPacks(const std::string& searchPath) {
    if (searchPath.empty()) return;

    try {
        fs::path root(searchPath);
        if (!fs::exists(root) || !fs::is_directory(root)) return;

        // Look for subdirectories that match the game ID
        for (const auto& entry : fs::directory_iterator(root)) {
            if (!entry.is_directory()) continue;

            std::string dirName = entry.path().filename().string();

            // Check if directory name contains our game CRC
            bool isMatch = false;
            if (!m_gameId.empty()) {
                // Case-insensitive compare
                std::string lowerDir = dirName;
                std::string lowerId = m_gameId;
                std::transform(lowerDir.begin(), lowerDir.end(), lowerDir.begin(), ::tolower);
                std::transform(lowerId.begin(), lowerId.end(), lowerId.begin(), ::tolower);
                isMatch = (lowerDir.find(lowerId) != std::string::npos);
            }

            // Also accept "hires_textures" or "textures" directories
            std::string lowerDir = dirName;
            std::transform(lowerDir.begin(), lowerDir.end(), lowerDir.begin(), ::tolower);
            if (lowerDir == "hires_textures" || lowerDir == "textures" ||
                lowerDir == "hires" || isMatch) {

                HiResTexturePack pack;
                pack.name = dirName;
                pack.path = entry.path().string();
                pack.gameId = m_gameId;
                pack.isLoaded = false;
                m_packs.push_back(pack);
                m_stats.packsFound++;
            }
        }

        // Auto-load all found packs
        for (auto& pack : m_packs) {
            LoadPack(pack.path);
        }

    } catch (const std::exception&) {
        // Filesystem error — silently continue
    }
}

bool HiResTextures::LoadPack(const std::string& packPath) {
    try {
        fs::path root(packPath);
        if (!fs::exists(root)) return false;

        IndexDirectory(packPath, "");

        // Update pack info
        for (auto& pack : m_packs) {
            if (pack.path == packPath) {
                pack.isLoaded = true;
                pack.textureCount = static_cast<uint32_t>(m_textureIndex.size());
                break;
            }
        }

        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  INDEXING
// ═══════════════════════════════════════════════════════════════════════════════

void HiResTextures::IndexDirectory(const std::string& dirPath, const std::string& relativePath) {
    try {
        for (const auto& entry : fs::directory_iterator(dirPath)) {
            if (entry.is_directory()) {
                std::string subRel = relativePath.empty() ?
                    entry.path().filename().string() :
                    relativePath + "/" + entry.path().filename().string();
                IndexDirectory(entry.path().string(), subRel);
            } else if (entry.is_regular_file()) {
                IndexFile(entry.path().string(), entry.path().filename().string());
            }
        }
    } catch (const std::exception&) {
        // Skip inaccessible directories
    }
}

void HiResTextures::IndexFile(const std::string& filePath, const std::string& relativePath) {
    // Check file extension
    HiResFormat fmt = DetectFormat(filePath);
    if (fmt == HiResFormat::Count) return; // Unsupported format

    // Parse CRC from filename
    uint32_t crc = 0;
    std::string filename = fs::path(filePath).stem().string();
    if (!ParseCRCFromFilename(filename, crc)) return;

    // Create index entry
    HiResTextureInfo info;
    info.crc = crc;
    info.format = fmt;
    info.filePath = filePath;
    info.width = 0;   // Will be set when actually loaded
    info.height = 0;
    info.mipLevels = 1;
    info.compression = HiResCompression::None;
    info.sizeBytes = 0;
    info.hasAlpha = true;

    // Get file size
    try {
        info.sizeBytes = static_cast<uint32_t>(fs::file_size(filePath));
    } catch (...) {}

    m_textureIndex[crc] = info;
    m_stats.texturesIndexed++;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  LOOKUP & LOADING
// ═══════════════════════════════════════════════════════════════════════════════

bool HiResTextures::HasTexture(uint32_t crc) const {
    return m_textureIndex.find(crc) != m_textureIndex.end();
}

const HiResTextureInfo* HiResTextures::GetTextureInfo(uint32_t crc) const {
    auto it = m_textureIndex.find(crc);
    if (it != m_textureIndex.end()) return &it->second;
    return nullptr;
}

std::unique_ptr<HiResTextureData> HiResTextures::LoadTexture(uint32_t crc) {
    auto it = m_textureIndex.find(crc);
    if (it == m_textureIndex.end()) {
        m_stats.cacheMisses++;
        return nullptr;
    }

    m_stats.cacheHits++;
    const HiResTextureInfo& info = it->second;

    auto data = std::make_unique<HiResTextureData>();
    data->info = info;

    bool loaded = false;
    switch (info.format) {
        case HiResFormat::PNG: loaded = LoadPNG(info.filePath, *data); break;
        case HiResFormat::BMP: loaded = LoadBMP(info.filePath, *data); break;
        case HiResFormat::TGA: loaded = LoadTGA(info.filePath, *data); break;
        case HiResFormat::DDS: loaded = LoadDDS(info.filePath, *data); break;
        case HiResFormat::KTX: loaded = LoadKTX(info.filePath, *data); break;
        default: break;
    }

    if (!loaded) return nullptr;

    m_stats.texturesLoaded++;
    m_stats.memoryUsed += data->pixels.size();

    return data;
}

void HiResTextures::PreloadAll() {
    // Load all indexed textures into memory
    for (const auto& [crc, info] : m_textureIndex) {
        LoadTexture(crc); // Result discarded — just ensures loading
    }
}

void HiResTextures::ReleasePixelData() {
    m_stats.memoryUsed = 0;
    m_stats.texturesLoaded = 0;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  FORMAT DETECTION & PARSING
// ═══════════════════════════════════════════════════════════════════════════════

bool HiResTextures::ParseCRCFromFilename(const std::string& filename, uint32_t& outCRC) {
    // Expected formats:
    //   "AABBCCDD"           — just the CRC
    //   "AABBCCDD_fmt"       — CRC + fornat info
    //   "AABBCCDD_all"       — CRC + "all" suffix
    //   "gamename#AABBCCDD"  — Rice-style naming

    std::string hexStr;

    // Check for Rice-style "name#CRC" format
    size_t hashPos = filename.find('#');
    if (hashPos != std::string::npos && hashPos + 8 <= filename.size()) {
        hexStr = filename.substr(hashPos + 1, 8);
    } else {
        // Check for leading hex CRC (first 8 chars)
        if (filename.size() >= 8) {
            hexStr = filename.substr(0, 8);
        }
    }

    // Validate hex string
    if (hexStr.size() < 8) return false;
    hexStr = hexStr.substr(0, 8);

    for (char c : hexStr) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }

    // Parse hex to uint32_t
    outCRC = static_cast<uint32_t>(std::stoul(hexStr, nullptr, 16));
    return true;
}

HiResFormat HiResTextures::DetectFormat(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".png") return HiResFormat::PNG;
    if (ext == ".dds") return HiResFormat::DDS;
    if (ext == ".ktx" || ext == ".ktx2") return HiResFormat::KTX;
    if (ext == ".bmp") return HiResFormat::BMP;
    if (ext == ".tga") return HiResFormat::TGA;
    return HiResFormat::Count; // Unsupported
}

// ═══════════════════════════════════════════════════════════════════════════════
//  IMAGE LOADERS
// ═══════════════════════════════════════════════════════════════════════════════

// PNG loader using stb_image for full decode (deflate, filtering, interlace)
bool HiResTextures::LoadPNG(const std::string& path, HiResTextureData& out) {
    // Read entire file into memory (stb_image memory callback interface)
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    auto fileSize = file.tellg();
    if (fileSize <= 8) return false;
    file.seekg(0);

    std::vector<uint8_t> fileData(static_cast<size_t>(fileSize));
    file.read(reinterpret_cast<char*>(fileData.data()), fileSize);
    if (!file.good()) return false;

    int w = 0, h = 0, channels = 0;
    // Force 4 channels (RGBA) output for consistent handling
    stbi_uc* pixels = stbi_load_from_memory(fileData.data(),
                                             static_cast<int>(fileData.size()),
                                             &w, &h, &channels, 4);
    if (!pixels) {
        LOG_WARN("HiResTextures: Failed to decode PNG: {} - {}", path, stbi_failure_reason());
        return false;
    }

    out.info.width    = static_cast<uint32_t>(w);
    out.info.height   = static_cast<uint32_t>(h);
    out.info.hasAlpha = (channels == 4 || channels == 2); // RGBA or GrayA
    out.info.compression = HiResCompression::None;

    size_t dataSize = static_cast<size_t>(w) * h * 4;
    out.pixels.assign(pixels, pixels + dataSize);
    out.info.sizeBytes = static_cast<uint32_t>(dataSize);

    stbi_image_free(pixels);
    return true;
}

bool HiResTextures::LoadBMP(const std::string& path, HiResTextureData& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    // BMP header
    uint8_t bmpHeader[54];
    file.read(reinterpret_cast<char*>(bmpHeader), 54);

    if (bmpHeader[0] != 'B' || bmpHeader[1] != 'M') return false;

    uint32_t width  = *(uint32_t*)&bmpHeader[18];
    uint32_t height = *(uint32_t*)&bmpHeader[22];
    uint16_t bpp    = *(uint16_t*)&bmpHeader[28];
    uint32_t offset = *(uint32_t*)&bmpHeader[10];

    if (bpp != 24 && bpp != 32) return false;

    out.info.width = width;
    out.info.height = height;
    out.info.hasAlpha = (bpp == 32);

    // Read pixel data (BMP is bottom-up, BGR order)
    file.seekg(offset);
    uint32_t bytesPerPixel = bpp / 8;
    uint32_t rowStride = ((width * bytesPerPixel + 3) / 4) * 4; // Row alignment

    std::vector<uint8_t> bmpData(rowStride * height);
    file.read(reinterpret_cast<char*>(bmpData.data()), bmpData.size());

    out.pixels.resize(width * height * 4);

    for (uint32_t y = 0; y < height; y++) {
        // BMP is bottom-up
        uint32_t srcRow = height - 1 - y;
        const uint8_t* row = &bmpData[srcRow * rowStride];

        for (uint32_t x = 0; x < width; x++) {
            uint32_t dstIdx = (y * width + x) * 4;
            uint32_t srcIdx = x * bytesPerPixel;

            out.pixels[dstIdx + 0] = row[srcIdx + 2]; // R (BMP is BGR)
            out.pixels[dstIdx + 1] = row[srcIdx + 1]; // G
            out.pixels[dstIdx + 2] = row[srcIdx + 0]; // B
            out.pixels[dstIdx + 3] = (bpp == 32) ? row[srcIdx + 3] : 0xFF; // A
        }
    }

    out.info.sizeBytes = static_cast<uint32_t>(out.pixels.size());
    return true;
}

bool HiResTextures::LoadTGA(const std::string& path, HiResTextureData& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    // TGA header (18 bytes)
    uint8_t header[18];
    file.read(reinterpret_cast<char*>(header), 18);

    uint8_t imageType = header[2];
    uint16_t width  = header[12] | (header[13] << 8);
    uint16_t height = header[14] | (header[15] << 8);
    uint8_t bpp = header[16];
    uint8_t descriptor = header[17];

    // Support uncompressed true-color only (type 2)
    if (imageType != 2) return false;
    if (bpp != 24 && bpp != 32) return false;

    out.info.width = width;
    out.info.height = height;
    out.info.hasAlpha = (bpp == 32);

    // Skip image ID
    file.seekg(header[0], std::ios::cur);

    uint32_t bytesPerPixel = bpp / 8;
    std::vector<uint8_t> tgaData(width * height * bytesPerPixel);
    file.read(reinterpret_cast<char*>(tgaData.data()), tgaData.size());

    out.pixels.resize(width * height * 4);

    bool topToBottom = (descriptor & 0x20) != 0;

    for (uint32_t y = 0; y < height; y++) {
        uint32_t srcY = topToBottom ? y : (height - 1 - y);

        for (uint32_t x = 0; x < width; x++) {
            uint32_t srcIdx = (srcY * width + x) * bytesPerPixel;
            uint32_t dstIdx = (y * width + x) * 4;

            out.pixels[dstIdx + 0] = tgaData[srcIdx + 2]; // R (TGA is BGR)
            out.pixels[dstIdx + 1] = tgaData[srcIdx + 1]; // G
            out.pixels[dstIdx + 2] = tgaData[srcIdx + 0]; // B
            out.pixels[dstIdx + 3] = (bpp == 32) ? tgaData[srcIdx + 3] : 0xFF;
        }
    }

    out.info.sizeBytes = static_cast<uint32_t>(out.pixels.size());
    return true;
}

bool HiResTextures::LoadDDS(const std::string& path, HiResTextureData& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    // DDS magic number
    uint32_t magic;
    file.read(reinterpret_cast<char*>(&magic), 4);
    if (magic != 0x20534444) return false; // "DDS "

    // DDS_HEADER (124 bytes)
    struct DDSHeader {
        uint32_t size;
        uint32_t flags;
        uint32_t height;
        uint32_t width;
        uint32_t pitchOrLinearSize;
        uint32_t depth;
        uint32_t mipMapCount;
        uint32_t reserved1[11];
        // DDS_PIXELFORMAT
        uint32_t pfSize;
        uint32_t pfFlags;
        uint32_t pfFourCC;
        uint32_t pfRGBBitCount;
        uint32_t pfRBitMask;
        uint32_t pfGBitMask;
        uint32_t pfBBitMask;
        uint32_t pfABitMask;
        // Caps
        uint32_t caps;
        uint32_t caps2;
        uint32_t caps3;
        uint32_t caps4;
        uint32_t reserved2;
    };

    DDSHeader ddsHeader;
    file.read(reinterpret_cast<char*>(&ddsHeader), sizeof(DDSHeader));

    out.info.width = ddsHeader.width;
    out.info.height = ddsHeader.height;
    out.info.mipLevels = std::max(1u, ddsHeader.mipMapCount);

    // Detect compression format from FourCC
    const uint32_t FOURCC_DXT1 = 0x31545844; // "DXT1"
    const uint32_t FOURCC_DXT3 = 0x33545844; // "DXT3"
    const uint32_t FOURCC_DXT5 = 0x35545844; // "DXT5"

    if (ddsHeader.pfFlags & 0x4) { // DDPF_FOURCC
        if (ddsHeader.pfFourCC == FOURCC_DXT1) {
            out.info.compression = HiResCompression::DXT1;
        } else if (ddsHeader.pfFourCC == FOURCC_DXT3) {
            out.info.compression = HiResCompression::DXT3;
        } else if (ddsHeader.pfFourCC == FOURCC_DXT5) {
            out.info.compression = HiResCompression::DXT5;
        } else {
            return false; // Unsupported format
        }
    } else {
        out.info.compression = HiResCompression::None;
    }

    // Read all remaining data
    auto currentPos = file.tellg();
    file.seekg(0, std::ios::end);
    auto endPos = file.tellg();
    size_t dataSize = static_cast<size_t>(endPos - currentPos);
    file.seekg(currentPos);

    out.pixels.resize(dataSize);
    file.read(reinterpret_cast<char*>(out.pixels.data()), dataSize);
    out.info.sizeBytes = static_cast<uint32_t>(dataSize);

    return true;
}

bool HiResTextures::LoadKTX(const std::string& path, HiResTextureData& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    // KTX identifier (12 bytes)
    uint8_t identifier[12];
    file.read(reinterpret_cast<char*>(identifier), 12);

    // KTX1 magic: { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A }
    const uint8_t KTX_MAGIC[] = { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
    if (std::memcmp(identifier, KTX_MAGIC, 12) != 0) return false;

    // Read KTX header fields
    uint32_t endianness;
    file.read(reinterpret_cast<char*>(&endianness), 4);

    uint32_t glType, glTypeSize, glFormat, glInternalFormat, glBaseInternalFormat;
    file.read(reinterpret_cast<char*>(&glType), 4);
    file.read(reinterpret_cast<char*>(&glTypeSize), 4);
    file.read(reinterpret_cast<char*>(&glFormat), 4);
    file.read(reinterpret_cast<char*>(&glInternalFormat), 4);
    file.read(reinterpret_cast<char*>(&glBaseInternalFormat), 4);

    uint32_t pixelWidth, pixelHeight, pixelDepth;
    file.read(reinterpret_cast<char*>(&pixelWidth), 4);
    file.read(reinterpret_cast<char*>(&pixelHeight), 4);
    file.read(reinterpret_cast<char*>(&pixelDepth), 4);

    uint32_t numArrayElements, numFaces, numMipLevels, bytesOfKeyValueData;
    file.read(reinterpret_cast<char*>(&numArrayElements), 4);
    file.read(reinterpret_cast<char*>(&numFaces), 4);
    file.read(reinterpret_cast<char*>(&numMipLevels), 4);
    file.read(reinterpret_cast<char*>(&bytesOfKeyValueData), 4);

    out.info.width = pixelWidth;
    out.info.height = pixelHeight;
    out.info.mipLevels = std::max(1u, numMipLevels);

    // Skip key-value data
    file.seekg(bytesOfKeyValueData, std::ios::cur);

    // Read mip level data
    for (uint32_t mip = 0; mip < out.info.mipLevels; mip++) {
        uint32_t imageSize;
        file.read(reinterpret_cast<char*>(&imageSize), 4);

        std::vector<uint8_t> mipPixels(imageSize);
        file.read(reinterpret_cast<char*>(mipPixels.data()), imageSize);

        if (mip == 0) {
            out.pixels = std::move(mipPixels);
        } else {
            out.mipData.push_back(std::move(mipPixels));
        }

        // Align to 4 bytes
        uint32_t padding = (4 - (imageSize % 4)) % 4;
        file.seekg(padding, std::ios::cur);
    }

    out.info.sizeBytes = static_cast<uint32_t>(out.pixels.size());
    return true;
}

}}} // namespace Nova64::Video::Tex
