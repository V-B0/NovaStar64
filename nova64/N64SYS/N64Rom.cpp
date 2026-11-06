/**
 * @file N64Rom.cpp
 * @brief N64 ROM image loading, management, and metadata extraction implementation
 */

#include "N64Rom.hpp"
#include "../common/platform.hpp"
#include "../common/MemoryManagment.hpp"
#include "../common/log.hpp"
#include "../common/md5.hpp"

#include <fstream>
#include <cstring>
#include <algorithm>
#include <cctype>

namespace N64 {
namespace Rom {

static constexpr uint32_t READ_CHUNK_SIZE = 0x100000; // 1MB chunks for reading
static constexpr uint32_t BOOT_CODE_SIZE = 0x1000;

// ROM magic numbers for format detection
static constexpr uint32_t MAGIC_BIG_ENDIAN = 0x80371240;
static constexpr uint32_t MAGIC_BYTE_SWAPPED = 0x37804012;
static constexpr uint32_t MAGIC_LITTLE_ENDIAN = 0x40123780;
static constexpr uint32_t MAGIC_64DD_IPL = 0x80270740;
static constexpr uint32_t MAGIC_64DD_IPL_SWAPPED = 0x27800740;

// CIC chip CRC values
static constexpr uint64_t CIC_CRC_6101 = 0x000000D0027FDF31;
static constexpr uint64_t CIC_CRC_6101_ALT1 = 0x000000CFFB631223;
static constexpr uint64_t CIC_CRC_6101_IQUE = 0x000000C34B2826B8;
static constexpr uint64_t CIC_CRC_6101_IQUE_PM = 0x0000002F35CF0DE9;
static constexpr uint64_t CIC_CRC_6101_IQUE_SP = 0x000000C92ADFE50A;
static constexpr uint64_t CIC_CRC_6102 = 0x000000D057C85244;
static constexpr uint64_t CIC_CRC_6103 = 0x000000D6497E414B;
static constexpr uint64_t CIC_CRC_6105 = 0x0000011A49F60E96;
static constexpr uint64_t CIC_CRC_6106 = 0x000000D6D5BE5580;
static constexpr uint64_t CIC_CRC_5167 = 0x000001053BC19870;
static constexpr uint64_t CIC_CRC_8303 = 0x000000D2E53EF008;
static constexpr uint64_t CIC_CRC_8401 = 0x000000D2E53EF39F;
static constexpr uint64_t CIC_CRC_8501 = 0x000000D2E53E5DDA;
static constexpr uint64_t CIC_CRC_LIBDRAGON = 0x0000007c56242373;
static constexpr uint64_t CIC_CRC_5101_ALECK64 = 0x000000A5F80BF620;

// ============================================================================
// RomImage Implementation
// ============================================================================

bool RomImage::IsPal() const noexcept {
    switch (m_Country) {
        case CountryCode::Germany:
        case CountryCode::France:
        case CountryCode::Italy:
        case CountryCode::Europe:
        case CountryCode::Spain:
        case CountryCode::Australia:
            return true;
        default:
            return false;
    }
}

bool RomImage::IsNtsc() const noexcept {
    switch (m_Country) {
        case CountryCode::USA:
        case CountryCode::Japan:
            return true;
        default:
            return false;
    }
}

// ============================================================================
// N64Rom Implementation
// ============================================================================

N64Rom::N64Rom()
    : m_Image(std::make_unique<RomImage>())
{
}

N64Rom::~N64Rom() {
    UnloadRom();
}

void N64Rom::LoadRom(const fs::path& filePath, bool loadBootCodeOnly) {
    UnloadRom();
    
    if (!fs::exists(filePath)) {
        throw RomError(RomError::Type::FileNotFound, 
            "ROM file not found: " + filePath.string());
    }
    
    std::string ext = filePath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    // Check if it's a zip file
    if (ext == ".zip") {
        LoadFromZip(filePath, loadBootCodeOnly);
    } else {
        LoadFromFile(filePath, loadBootCodeOnly);
    }
    
    m_Image->m_FilePath = filePath;
    m_Image->m_FileName = filePath.filename().string();
    
    // Parse header and extract metadata
    ParseHeader();
    
    // Detect CIC chip
    CalculateCICChip();
    
    // Calculate MD5 if loading full ROM
    if (!loadBootCodeOnly) {
        CalculateMD5();
    }
    
    // Check if this is a 64DD IPL
    m_Image->m_IsDDIPL = (m_Image->m_CICChip == CICChip::CIC_NUS_8303 ||
                          m_Image->m_CICChip == CICChip::CIC_NUS_DDUS);
}

void N64Rom::LoadIPL(const fs::path& filePath, bool loadBootCodeOnly) {
    LoadRom(filePath, loadBootCodeOnly);
    
    // Verify it's actually a 64DD IPL
    if (!m_Image->m_IsDDIPL) {
        UnloadRom();
        throw RomError(RomError::Type::InvalidFormat, 
            "File is not a valid 64DD IPL ROM");
    }
}

void N64Rom::UnloadRom() noexcept {
    if (m_Image) {
        m_Image->m_Data.clear();
        m_Image->m_Format = RomFormat::Unknown;
        m_Image->m_Country = CountryCode::Unknown;
        m_Image->m_CICChip = CICChip::Unknown;
        m_Image->m_RomName.clear();
        m_Image->m_FileName.clear();
        m_Image->m_MD5.clear();
        m_Image->m_FilePath.clear();
        m_Image->m_IsDDIPL = false;
    }
}

constexpr bool N64Rom::IsValidRom(const std::array<uint8_t, 4>& testBytes) noexcept {
    uint32_t magic = (static_cast<uint32_t>(testBytes[0]) << 24) |
                     (static_cast<uint32_t>(testBytes[1]) << 16) |
                     (static_cast<uint32_t>(testBytes[2]) << 8) |
                     static_cast<uint32_t>(testBytes[3]);
    
    switch (magic) {
        case MAGIC_BIG_ENDIAN:
        case MAGIC_BYTE_SWAPPED:
        case MAGIC_LITTLE_ENDIAN:
        case MAGIC_64DD_IPL:
        case MAGIC_64DD_IPL_SWAPPED:
            return true;
        default:
            return false;
    }
}

const RomImage& N64Rom::GetImage() const {
    if (!m_Image || !m_Image->IsLoaded()) {
        throw RomError(RomError::Type::ReadError, "No ROM loaded");
    }
    return *m_Image;
}

RomFormat N64Rom::GetFormat() const {
    return m_Image ? m_Image->m_Format : RomFormat::Unknown;
}

size_t N64Rom::GetSize() const {
    return m_Image ? m_Image->m_Data.size() : 0;
}

DataView N64Rom::GetHeader() const {
    if (!IsLoaded() || m_Image->m_Data.size() < RomHeader::SIZE) {
        return DataView{nullptr, 0};
    }
    return DataView{m_Image->m_Data.data(), RomHeader::SIZE};
}

DataView N64Rom::GetBootCode() const {
    if (!IsLoaded() || m_Image->m_Data.size() < BOOT_CODE_SIZE) {
        return DataView{nullptr, 0};
    }
    return DataView{m_Image->m_Data.data() + RomHeader::BOOT_CODE_OFFSET, 
                    RomHeader::BOOT_CODE_SIZE};
}

const uint8_t* N64Rom::GetRomAddress() const {
    return m_Image ? m_Image->m_Data.data() : nullptr;
}

uint8_t* N64Rom::GetRomAddress() {
    return m_Image ? m_Image->m_Data.data() : nullptr;
}

CICChip N64Rom::DetectCICChip(const uint8_t* romData, uint64_t* crc) noexcept {
    if (!romData) {
        return CICChip::Unknown;
    }
    
    uint64_t calcCrc = 0;
    uint64_t crcAleck64 = 0;
    
    // Calculate CRC from boot code area (0x40 to 0x1000)
    for (int i = 0x40; i < 0x1000; i += 4) {
        if (i == 0xC00) {
            crcAleck64 = calcCrc; // From 0x40 to 0xC00 (Aleck64)
        }
        calcCrc += *reinterpret_cast<const uint32_t*>(romData + i);
    }
    
    if (crc != nullptr) {
        *crc = calcCrc;
    }
    
    // Match CRC to known CIC chips
    switch (calcCrc) {
        case CIC_CRC_6101:
        case CIC_CRC_6101_ALT1:
        case CIC_CRC_6101_IQUE:
        case CIC_CRC_6101_IQUE_PM:
        case CIC_CRC_6101_IQUE_SP:
            return CICChip::CIC_NUS_6101;
            
        case CIC_CRC_6102:
        case CIC_CRC_LIBDRAGON:
            return CICChip::CIC_NUS_6102;
            
        case CIC_CRC_6103:
            return CICChip::CIC_NUS_6103;
            
        case CIC_CRC_6105:
            return CICChip::CIC_NUS_6105;
            
        case CIC_CRC_6106:
            return CICChip::CIC_NUS_6106;
            
        case CIC_CRC_5167:
            return CICChip::CIC_NUS_5167;
            
        case CIC_CRC_8303:
        case CIC_CRC_8401:
            return CICChip::CIC_NUS_8303;
            
        case CIC_CRC_8501:
            return CICChip::CIC_NUS_DDUS;
            
        default:
            // Check for Aleck64 CIC
            if (crcAleck64 == CIC_CRC_5101_ALECK64) {
                if (crc != nullptr) {
                    *crc = crcAleck64;
                }
                return CICChip::CIC_NUS_5101_ALECK64;
            }
            
            Log::warningf("DetectCICChip: Unknown CIC CRC 0x%016llX (defaulting to 6102)", calcCrc);
            return CICChip::CIC_NUS_6102;
    }
}

void N64Rom::CleanRomName(std::string& romName, bool byteswap) {
    if (romName.empty()) {
        return;
    }
    
    // Ensure we have at least 20 characters to work with
    if (romName.size() < 20) {
        romName.resize(20, ' ');
    }
    
    // Byteswap if needed
    if (byteswap) {
        for (size_t i = 0; i + 3 < romName.size() && i < 20; i += 4) {
            std::swap(romName[i], romName[i + 3]);
            std::swap(romName[i + 1], romName[i + 2]);
        }
    }
    
    // Truncate at 20 characters
    if (romName.size() > 20) {
        romName.resize(20);
    }
    
    // Remove trailing spaces
    while (!romName.empty() && (romName.back() == ' ' || romName.back() == '\0')) {
        romName.pop_back();
    }
    
    // Replace special characters
    for (char& c : romName) {
        switch (c) {
            case '/':
            case '\\':
                c = '-';
                break;
            case ':':
                c = ';';
                break;
        }
    }
}

void N64Rom::SaveRomSettingID(bool temp) {
    // Save ROM settings to emulator configuration
    // This would typically save to a settings system
    // For now, this is a placeholder
}

void N64Rom::ClearRomSettingID() {
    // Clear any saved ROM settings
}

// ============================================================================
void N64Rom::DetectFormatAndByteSwap() {
    if (!m_Image || m_Image->m_Data.size() < 4) {
        return;
    }
    
    uint32_t magic = *reinterpret_cast<uint32_t*>(m_Image->m_Data.data());
    
    switch (magic) {
        case 0x40123780: // Big endian - native N64 format (read as LE: 0x80371240 -> 0x40123780)
            m_Image->m_Format = RomFormat::BigEndian;
            ByteSwapN64(); // Convert to host order (40 12 37 80)
            break;
            
        case 0x12408037: // Byte swapped (V64) (read as LE: 0x37804012 -> 0x12408037)
            m_Image->m_Format = RomFormat::ByteSwapped;
            ByteSwapV64(); // Convert to Big Endian (80 37 12 40)
            ByteSwapN64(); // Convert to host order (40 12 37 80)
            break;
            
        case 0x80371240: // Little endian (N64) (read as LE: 0x40123780 -> 0x80371240)
            m_Image->m_Format = RomFormat::LittleEndian;
            // No swap needed, already in host order (40 12 37 80)
            break;
            
        default:
            m_Image->m_Format = RomFormat::Unknown;
            break;
    }
}

void N64Rom::ParseHeader() {
    if (!IsLoaded() || m_Image->m_Data.size() < RomHeader::SIZE) {
        return;
    }
    
    const uint8_t* header = m_Image->m_Data.data();
    
    // ROM data is stored in host word order (each 32-bit word byte-reversed
    // from big-endian on little-endian hosts). Individual byte reads need
    // XOR 3 to convert big-endian byte offset to host-word-order offset.
    
    // Extract ROM name (20 bytes starting at offset 0x20)
    char name[21] = {0};
    for (int i = 0; i < static_cast<int>(RomHeader::NAME_LENGTH); i++) {
        name[i] = static_cast<char>(header[(RomHeader::OFFSET_NAME + i) ^ 3]);
    }
    m_Image->m_RomName = name;
    
    // Clean the ROM name
    CleanRomName(m_Image->m_RomName, false);
    
    // If name is empty, use filename
    if (m_Image->m_RomName.empty()) {
        m_Image->m_RomName = m_Image->m_FilePath.stem().string();
        CleanRomName(m_Image->m_RomName, false);
    }
    
    // Extract country code (single byte — use XOR 3 for byte access)
    m_Image->m_Country = static_cast<CountryCode>(header[RomHeader::OFFSET_COUNTRY_CODE ^ 3]);
}

void N64Rom::CalculateCICChip() {
    if (!IsLoaded() || m_Image->m_Data.size() < BOOT_CODE_SIZE) {
        m_Image->m_CICChip = CICChip::Unknown;
        return;
    }
    
    uint64_t crc = 0;
    m_Image->m_CICChip = DetectCICChip(m_Image->m_Data.data(), &crc);
}

void N64Rom::CalculateMD5() {
    if (!IsLoaded()) {
        m_Image->m_MD5.clear();
        return;
    }
    
    Common::MD5 md5(m_Image->m_Data.data(), m_Image->m_Data.size());
    m_Image->m_MD5 = md5.HexDigest();
}

bool N64Rom::AllocateImage(size_t size) {
    if (!m_Image) {
        m_Image = std::make_unique<RomImage>();
    }
    
    try {
        m_Image->m_Data.resize(size);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

void N64Rom::LoadFromFile(const fs::path& filePath, bool loadBootCodeOnly) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        throw RomError(RomError::Type::ReadError, 
            "Failed to open ROM file: " + filePath.string());
    }
    
    // Get file size
    file.seekg(0, std::ios::end);
    size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    
    // Read first 4 bytes to verify it's a valid ROM
    std::array<uint8_t, 4> testBytes;
    if (!file.read(reinterpret_cast<char*>(testBytes.data()), 4)) {
        throw RomError(RomError::Type::ReadError, "Failed to read ROM header");
    }
    
    if (!IsValidRomImage(testBytes.data())) {
        throw RomError(RomError::Type::InvalidFormat, 
            "Invalid ROM image file");
    }
    
    // Determine size to load
    size_t loadSize = loadBootCodeOnly ? BOOT_CODE_SIZE : fileSize;
    loadSize = std::min(loadSize, fileSize);
    
    // Allocate memory
    if (!AllocateImage(loadSize)) {
        throw RomError(RomError::Type::OutOfMemory, 
            "Failed to allocate memory for ROM");
    }
    
    // Copy the first 4 bytes we already read
    std::memcpy(m_Image->m_Data.data(), testBytes.data(), 4);
    
    // Read the rest of the ROM in chunks
    file.seekg(4);
    size_t totalRead = 4;
    
    while (totalRead < loadSize) {
        size_t toRead = std::min(static_cast<size_t>(READ_CHUNK_SIZE), loadSize - totalRead);
        if (!file.read(reinterpret_cast<char*>(m_Image->m_Data.data() + totalRead), toRead)) {
            if (file.gcount() == 0) {
                throw RomError(RomError::Type::ReadError, "Failed to read ROM data");
            }
            totalRead += static_cast<size_t>(file.gcount());
            break;
        }
        totalRead += toRead;
    }
    
    // Byte swap if needed
    DetectFormatAndByteSwap();
}

void N64Rom::LoadFromZip(const fs::path& filePath, bool loadBootCodeOnly) {
    // Basic zip support would go here
    // For now, throw an error indicating zip support is not implemented
    // In a full implementation, you would use a library like zlib/minizip
    throw RomError(RomError::Type::ZipError, 
        "ZIP file support not yet implemented. Extract the ROM first.");
}

void N64Rom::ByteSwapIfNeeded() {
    DetectFormatAndByteSwap();
}

void N64Rom::ByteSwapV64() {
    // V64 format: swap every 2 bytes
    uint8_t* data = m_Image->m_Data.data();
    size_t size = m_Image->m_Data.size();
    
    for (size_t i = 0; i < size; i += 2) {
        std::swap(data[i], data[i + 1]);
    }
}

void N64Rom::ByteSwapN64() {
    // N64/Little endian format: swap every 4 bytes
    uint8_t* data = m_Image->m_Data.data();
    size_t size = m_Image->m_Data.size();
    
    for (size_t i = 0; i < size; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
    }
}

bool N64Rom::IsValidRomImage(const uint8_t* test) noexcept {
    if (!test) {
        return false;
    }
    
    uint32_t magic = *reinterpret_cast<const uint32_t*>(test);
    
    switch (magic) {
        case 0x40123780: // Little endian
        case 0x12408037: // Byte swapped
        case 0x80371240: // Big endian
        case 0x40072780: // 64DD IPL
            return true;
        default:
            return false;
    }
}

// ============================================================================
// ROM CRC Calculation
// ============================================================================

void N64Rom::CalculateRomCrc() {
    if (!IsLoaded() || m_Image->m_Data.size() < 0x101000) {
        return;
    }
    
    uint32_t t0, t2, t3, t4, t5;
    uint32_t a0, a1, a2, a3;
    uint32_t s0;
    uint32_t v0, v1;
    uint32_t length = 0x00100000;
    
    // Select initial value based on CIC chip
    switch (m_Image->m_CICChip) {
        case CICChip::CIC_NUS_6101:
        case CICChip::CIC_NUS_6102:
            v0 = 0xF8CA4DDC;
            break;
        case CICChip::CIC_NUS_6103:
            v0 = 0xA3886759;
            break;
        case CICChip::CIC_NUS_6105:
            v0 = 0xDF26F436;
            break;
        case CICChip::CIC_NUS_6106:
            v0 = 0x1FEA617A;
            break;
        case CICChip::CIC_NUS_DDUS:
            length = 0x000A0000;
            v0 = 0x861AE3A7;
            break;
        case CICChip::CIC_NUS_8303:
            length = 0x000A0000;
            v0 = 0x8331D4CA;
            break;
        default:
            return;
    }
    
    v1 = 0;
    t0 = 0;
    t5 = 0x20;
    
    a3 = v0;
    t2 = v0;
    t3 = v0;
    s0 = v0;
    a2 = v0;
    t4 = v0;
    
    uint8_t* romData = m_Image->m_Data.data();
    
    for (t0 = 0; t0 < length; t0 += 4) {
        v0 = *reinterpret_cast<uint32_t*>(romData + t0 + 0x1000);
        
        v1 = a3 + v0;
        a1 = v1;
        
        if (v1 < a3) {
            if (m_Image->m_CICChip == CICChip::CIC_NUS_DDUS ||
                m_Image->m_CICChip == CICChip::CIC_NUS_8303) {
                t2 = t2 ^ t3;
            } else {
                t2 += 1;
            }
        }
        
        v1 = v0 & 0x001F;
        a0 = (v0 << v1) | (v0 >> (t5 - v1));
        
        a3 = a1;
        t3 = t3 ^ v0;
        s0 = s0 + a0;
        
        if (a2 < v0) {
            a2 = a3 ^ v0 ^ a2;
        } else {
            if (m_Image->m_CICChip == CICChip::CIC_NUS_8303) {
                a2 = a2 + a0;
            } else {
                a2 = a2 ^ a0;
            }
        }
        
        if (m_Image->m_CICChip == CICChip::CIC_NUS_6105) {
            t4 = (v0 ^ (*reinterpret_cast<uint32_t*>(romData + (0xFF & t0) + 0x750))) + t4;
        } else {
            t4 = (v0 ^ s0) + t4;
        }
    }
    
    if (m_Image->m_CICChip == CICChip::CIC_NUS_6103) {
        a3 = (a3 ^ t2) + t3;
        s0 = (s0 ^ a2) + t4;
    } else if (m_Image->m_CICChip == CICChip::CIC_NUS_6106) {
        a3 = 0xFFFFFFFF & (a3 * t2) + t3;
        s0 = 0xFFFFFFFF & (s0 * a2) + t4;
    } else {
        a3 = a3 ^ t2 ^ t3;
        s0 = s0 ^ a2 ^ t4;
    }
    
    // Write CRC to ROM header
    *reinterpret_cast<uint32_t*>(romData + 0x10) = a3;
    *reinterpret_cast<uint32_t*>(romData + 0x14) = s0;
}

} // namespace Rom
} // namespace N64
