/**
 * @file HiResPack.cpp
 * @brief Texture packs in the Rice Video format. See HiResPack.hpp.
 */
#include "HiResPack.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace Nova64 {
namespace Video {
namespace Tex {

// --- Checksum ---

uint32_t RiceCrc32(const uint8_t* src, int width, int height, int size, int rowStride) {
    uint32_t crc = 0;
    const uint32_t bytesPerLine = static_cast<uint32_t>(width) << size >> 1;
    int y = height - 1;
    do {
        uint32_t word = 0;
        int x = static_cast<int>(bytesPerLine) - 4;
        do {
            std::memcpy(&word, src + x, sizeof(word));
            word ^= static_cast<uint32_t>(x);
            crc = (crc << 4) + ((crc >> 28) & 15);
            crc += word;
            x -= 4;
        } while (x >= 0);
        word ^= static_cast<uint32_t>(y);
        crc += word;
        src += rowStride;
        --y;
    } while (y >= 0);
    return crc;
}

namespace {

// Largest palette index a CI texture uses. Reads bytes in memory order, as the
// reference does, so texels are visited in the byte order RDRAM keeps them in.
uint32_t MaxIndex8(const uint8_t* src, int width, int height, int rowStride) {
    uint8_t best = 0;
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = src + static_cast<ptrdiff_t>(rowStride) * y;
        for (int x = 0; x < width; ++x) {
            best = std::max(best, row[x]);
            if (best == 0xFF) return 0xFF;
        }
    }
    return best;
}

uint32_t MaxIndex4(const uint8_t* src, int width, int height, int rowStride) {
    uint8_t best = 0;
    const int bytes = width >> 1;
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = src + static_cast<ptrdiff_t>(rowStride) * y;
        for (int x = 0; x < bytes; ++x) {
            best = std::max<uint8_t>(best, row[x] >> 4);
            best = std::max<uint8_t>(best, row[x] & 0xF);
            if (best == 0xF) return 0xF;
        }
    }
    return best;
}

} // namespace

HiResId ComputeHiResId(const uint8_t* rdram, size_t rdramSize, uint32_t address, int width, int height, int format,
                       int size, int rowStride, const uint8_t* palette) {
    HiResId id;
    id.format = static_cast<uint8_t>(format);
    id.size = static_cast<uint8_t>(size);
    if (!rdram || width <= 0 || height <= 0 || size < 0 || size > 3 || rowStride < 0) return id;

    // The checksum reads whole words, starting a word before the end of each row's
    // bytes, and (for rows narrower than 4 bytes) a little before the row itself.
    const int64_t bytesPerLine = (static_cast<int64_t>(width) << size) >> 1;
    const int64_t first = static_cast<int64_t>(address) + std::min<int64_t>(0, bytesPerLine - 4);
    const int64_t last = static_cast<int64_t>(address) + static_cast<int64_t>(height - 1) * rowStride +
                         std::max<int64_t>(bytesPerLine, 4);
    if (first < 0 || last > static_cast<int64_t>(rdramSize)) return id;
    const uint8_t* src = rdram + address;

    if (palette && size <= 1) {
        // Palette (CI) texture: the texture checksum covers the index data, the palette
        // checksum only the entries those indices can reach.
        uint32_t texCrc, maxIndex;
        if (size == 1) {
            texCrc = RiceCrc32(src, width, height, 1, rowStride);
            maxIndex = MaxIndex8(src, width, height, rowStride);
        } else {
            texCrc = RiceCrc32(src, width, height, 0, rowStride);
            maxIndex = MaxIndex4(src, width, height, rowStride);
        }
        // Palette entries are 16 bits and the checksum reads a word at a time, so an odd
        // number of entries makes it reach two bytes before the palette (the previous
        // palette's last entry, for CI4). The caller keeps readable memory there.
        const uint32_t palCrc = RiceCrc32(palette, static_cast<int>(maxIndex) + 1, 1, 2, (size == 1) ? 512 : 32);
        if (texCrc != 0 || palCrc != 0) {
            id.texCrc = texCrc;
            id.palCrc = palCrc;
            return id;
        }
    }
    id.texCrc = RiceCrc32(src, width, height, size, rowStride);
    return id;
}

// --- Pack index ---

namespace {

enum class Kind { All, Rgb, Alpha };

struct ParsedName {
    bool texWildcard = false;
    bool palWildcard = false;
    bool hasPalette = false;
    uint32_t tex = 0, pal = 0;
    uint8_t format = 0, size = 0;
    Kind kind = Kind::All;
};

bool IsHex(const std::string& s, size_t digits) {
    return s.size() == digits && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::vector<std::string> SplitOn(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t pos; (pos = s.find(sep, start)) != std::string::npos; start = pos + 1)
        out.push_back(s.substr(start, pos - start));
    out.push_back(s.substr(start));
    return out;
}

// "SUPER MARIO 64#8A848688#3#1_all" -> checksums, format, size and which picture it is.
bool ParseName(const std::string& stem, ParsedName& out) {
    const size_t underscore = stem.rfind('_');
    if (underscore == std::string::npos) return false;
    const std::string suffix = stem.substr(underscore + 1);
    if (suffix == "all" || suffix == "ciByRGBA" || suffix == "allciByRGBA") out.kind = Kind::All;
    else if (suffix == "rgb") out.kind = Kind::Rgb;
    else if (suffix == "a") out.kind = Kind::Alpha;
    else return false;

    const auto tok = SplitOn(stem.substr(0, underscore), '#');
    const size_t n = tok.size();
    auto crcOrWildcard = [](const std::string& s) { return s == "$" || IsHex(s, 8); };

    if (n >= 5 && crcOrWildcard(tok[n - 4]) && IsHex(tok[n - 3], 1) && IsHex(tok[n - 2], 1) &&
        crcOrWildcard(tok[n - 1])) {
        out.hasPalette = true;
        out.texWildcard = tok[n - 4] == "$";
        out.palWildcard = tok[n - 1] == "$";
        if (out.texWildcard && out.palWildcard) return false;
        if (!out.texWildcard) out.tex = static_cast<uint32_t>(std::stoul(tok[n - 4], nullptr, 16));
        if (!out.palWildcard) out.pal = static_cast<uint32_t>(std::stoul(tok[n - 1], nullptr, 16));
        out.format = static_cast<uint8_t>(std::stoul(tok[n - 3], nullptr, 16));
        out.size = static_cast<uint8_t>(std::stoul(tok[n - 2], nullptr, 16));
        return true;
    }
    if (n >= 4 && IsHex(tok[n - 3], 8) && IsHex(tok[n - 2], 1) && IsHex(tok[n - 1], 1)) {
        out.tex = static_cast<uint32_t>(std::stoul(tok[n - 3], nullptr, 16));
        out.format = static_cast<uint8_t>(std::stoul(tok[n - 2], nullptr, 16));
        out.size = static_cast<uint8_t>(std::stoul(tok[n - 1], nullptr, 16));
        return true;
    }
    return false;
}

bool HasPngExtension(const fs::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".png";
}

std::unique_ptr<HiResImage> LoadPng(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return nullptr;
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return nullptr;
    int w = 0, h = 0, channels = 0;
    uint8_t* pixels = stbi_load_from_memory(reinterpret_cast<const uint8_t*>(bytes.data()),
                                            static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return nullptr;
    }
    auto image = std::make_unique<HiResImage>();
    image->width = static_cast<uint32_t>(w);
    image->height = static_cast<uint32_t>(h);
    image->rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
    stbi_image_free(pixels);
    return image;
}

} // namespace

bool HiResPack::Load(const fs::path& folder) {
    m_byTexAndPal.clear();
    m_byTex.clear();
    m_byPal.clear();
    m_count = 0;
    m_folder = folder;

    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return false;

    auto assign = [](Files& files, Kind kind, const fs::path& path) {
        (kind == Kind::All ? files.all : kind == Kind::Rgb ? files.rgb : files.alpha) = path;
    };
    for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || !HasPngExtension(it->path())) continue;
        ParsedName name;
        if (!ParseName(it->path().stem().string(), name)) continue;

        const uint16_t formatSize = static_cast<uint16_t>((name.format << 8) | name.size);
        if (name.hasPalette && !name.texWildcard && !name.palWildcard)
            assign(m_byTexAndPal[{name.tex, name.pal, formatSize}], name.kind, it->path());
        else if (name.hasPalette && name.texWildcard) assign(m_byPal[{name.pal, formatSize}], name.kind, it->path());
        else assign(m_byTex[{name.tex, formatSize}], name.kind, it->path());
    }
    m_count = m_byTexAndPal.size() + m_byTex.size() + m_byPal.size();
    return m_count != 0;
}

std::unique_ptr<HiResImage> HiResPack::Decode(const Files& files) {
    if (!files.all.empty()) return LoadPng(files.all);
    if (files.rgb.empty()) return nullptr;
    auto image = LoadPng(files.rgb);
    if (!image) return nullptr;
    if (!files.alpha.empty()) {
        // The alpha picture is a greyscale image; its brightness is the alpha.
        if (auto alpha = LoadPng(files.alpha);
            alpha && alpha->width == image->width && alpha->height == image->height) {
            for (size_t i = 0; i < static_cast<size_t>(image->width) * image->height; ++i)
                image->rgba[i * 4 + 3] = alpha->rgba[i * 4];
        }
    }
    return image;
}

std::unique_ptr<HiResImage> HiResPack::Find(const HiResId& id) const {
    const uint16_t formatSize = static_cast<uint16_t>((id.format << 8) | id.size);
    if (id.palCrc != 0) {
        if (auto it = m_byTexAndPal.find({id.texCrc, id.palCrc, formatSize}); it != m_byTexAndPal.end())
            if (auto image = Decode(it->second)) return image;
        if (auto it = m_byPal.find({id.palCrc, formatSize}); it != m_byPal.end())
            if (auto image = Decode(it->second)) return image;
    }
    if (auto it = m_byTex.find({id.texCrc, formatSize}); it != m_byTex.end()) return Decode(it->second);
    return nullptr;
}

} // namespace Tex
} // namespace Video
} // namespace Nova64
