/**
 * @file HiResPack.hpp
 * @brief Texture packs in the Rice Video format, as used by Glide64 and GLideN64.
 *
 * A pack is a folder of PNG pictures (hires_texture/<game name>/, subfolders allowed)
 * whose file names identify the N64 texture each one replaces:
 *
 *     <game name>#<texture crc>#<format>#<size>_all.png            most textures
 *     <game name>#<texture crc>#<format>#<size>#<palette crc>_all.png   palette (CI) textures
 *
 * The texture crc is a checksum of the texel data as the game keeps it in RDRAM, the
 * palette crc one of the palette entries the texture actually uses, and format/size are
 * the N64's own codes (format 0 RGBA, 2 CI, 3 IA, 4 I; size 0 4-bit ... 3 32-bit).
 * `$` in place of either checksum matches any value. Pictures may also come as an
 * `_rgb` colour image plus an `_a` alpha image.
 *
 * The checksum algorithm is the one those emulators define, so packs made for them
 * work here unchanged. RiceCrc32 follows GLideN64's TxUtil.cpp (GPL-2.0,
 * https://github.com/gonetz/GLideN64) rather than being an independent design, since
 * matching the existing packs requires reproducing it exactly.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Nova64 {
namespace Video {
namespace Tex {

/// A replacement picture, straight (non-premultiplied) RGBA8.
struct HiResImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

/// The identity a pack file name carries: what it replaces.
struct HiResId {
    uint32_t texCrc = 0; ///< checksum of the texel data
    uint32_t palCrc = 0; ///< checksum of the palette entries used (CI textures), else 0
    uint8_t format = 0;  ///< N64 texture format
    uint8_t size = 0;    ///< N64 texel size

    bool Valid() const { return texCrc != 0 || palCrc != 0; }
};

/// Rice checksum of `height` rows of `width` texels of N64 texel size `size`
/// (0 = 4 bit ... 3 = 32 bit), `rowStride` bytes apart. Reads whole 32-bit words in the
/// byte order RDRAM keeps them in (which is what the packs' names were computed from).
uint32_t RiceCrc32(const uint8_t* src, int width, int height, int size, int rowStride);

/// The identity of a texture: its Rice checksum(s) plus format and size. `palette` is
/// the raw copy of the palette memory the texture reads (null for non-CI textures);
/// at least 8 bytes before it must be readable. Returns an id with Valid() == false
/// if the data doesn't lie inside the buffer.
HiResId ComputeHiResId(const uint8_t* rdram, size_t rdramSize, uint32_t address, int width, int height, int format,
                       int size, int rowStride, const uint8_t* palette);

class HiResPack {
public:
    /// Indexes every recognisable picture under `folder`. Returns false (and stays
    /// empty) if the folder is missing or holds none.
    bool Load(const std::filesystem::path& folder);
    bool Empty() const { return m_count == 0; }
    size_t Count() const { return m_count; }
    const std::filesystem::path& Folder() const { return m_folder; }

    /// Decodes the picture that replaces the texture with this identity; null if the
    /// pack has none.
    std::unique_ptr<HiResImage> Find(const HiResId& id) const;

private:
    struct Files {
        std::filesystem::path all, rgb, alpha;
    };
    struct Key {
        uint32_t value; ///< texture crc, palette crc, or both packed (see the maps)
        uint16_t formatSize;
        bool operator==(const Key& o) const { return value == o.value && formatSize == o.formatSize; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const { return (static_cast<size_t>(k.value) * 0x9E3779B1u) ^ k.formatSize; }
    };
    struct FullKey {
        uint32_t tex, pal;
        uint16_t formatSize;
        bool operator==(const FullKey& o) const { return tex == o.tex && pal == o.pal && formatSize == o.formatSize; }
    };
    struct FullKeyHash {
        size_t operator()(const FullKey& k) const {
            return ((static_cast<size_t>(k.tex) * 0x9E3779B1u) ^ (static_cast<size_t>(k.pal) * 0x85EBCA6Bu)) ^
                   k.formatSize;
        }
    };

    static std::unique_ptr<HiResImage> Decode(const Files& files);

    std::filesystem::path m_folder;
    size_t m_count = 0;
    std::unordered_map<FullKey, Files, FullKeyHash> m_byTexAndPal; // ...#<crc>#F#S#<palette crc>
    std::unordered_map<Key, Files, KeyHash> m_byTex;               // ...#<crc>#F#S  and  ...#<crc>#F#S#$
    std::unordered_map<Key, Files, KeyHash> m_byPal;               // ...#$#F#S#<palette crc>
};

} // namespace Tex
} // namespace Video
} // namespace Nova64
