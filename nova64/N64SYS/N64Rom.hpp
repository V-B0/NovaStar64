/**
 * @file N64Rom.hpp
 * @brief N64 ROM image loading, management, and metadata extraction
 * 
 * Handles loading ROM images from disk (including ZIP archives), 
 * extracting metadata (CIC chip, country code, etc.), and providing
 * access to the ROM data for the emulator.
 */

#pragma once

#include "Types.hpp"
#include <vector>
#include <memory>
#include <string>
#include <string_view>
#include <array>
#include <cstdint>
#include <optional>
#include <filesystem>

namespace fs = std::filesystem;

namespace N64 {
namespace Rom {

/**
 * @enum RomFormat
 * @brief Supported ROM image byte orderings
 */
enum class RomFormat : uint8_t {
    BigEndian,      ///< .z64 - Native N64 format (big endian)
    ByteSwapped,    ///< .v64 - Doctor V64 format (byte swapped)
    LittleEndian,   ///< .n64 - Little endian format
    Unknown
};

/**
 * @enum CountryCode
 * @brief Country/region codes for N64 ROMs
 */
enum class CountryCode : uint8_t {
    Germany     = 0x44,  ///< 'D' - Germany (PAL)
    USA         = 0x45,  ///< 'E' - USA (NTSC)
    France      = 0x46,  ///< 'F' - France (PAL)
    Italy       = 0x49,  ///< 'I' - Italy (PAL)
    Japan       = 0x4A,  ///< 'J' - Japan (NTSC)
    Europe      = 0x50,  ///< 'P' - Europe (PAL)
    Spain       = 0x53,  ///< 'S' - Spain (PAL)
    Australia   = 0x55,  ///< 'U' - Australia (PAL)
    Unknown     = 0x00
};

/**
 * @enum CICChip
 * @brief CIC (Copy protection) chip types
 */
enum class CICChip : uint8_t {
    CIC_NUS_6101,   ///< Star Fox 64, etc.
    CIC_NUS_6102,   ///< Most common
    CIC_NUS_6103,   ///< Banjo-Kazooie, Paper Mario, etc.
    CIC_NUS_6105,   ///< Ocarina of Time, Majora's Mask, etc.
    CIC_NUS_6106,   ///< F-Zero X, Yoshi's Story, etc.
    CIC_NUS_5167,   ///< 64DD conversion
    CIC_NUS_8303,   ///< 64DD retail
    CIC_NUS_DDUS,   ///< 64DD US
    Unknown
};

/**
 * @struct DataView
 * @brief A simple view into a contiguous range of bytes (C++17 alternative to std::span)
 */
struct DataView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    
    bool empty() const noexcept { return size == 0 || data == nullptr; }
    const uint8_t* begin() const noexcept { return data; }
    const uint8_t* end() const noexcept { return data + size; }
    const uint8_t& operator[](size_t index) const { return data[index]; }
};

/**
 * @struct RomHeader
 * @brief N64 ROM header structure (first 0x40 bytes)
 */
struct RomHeader {
    static constexpr uint32_t SIZE = 0x40;
    static constexpr uint32_t BOOT_CODE_OFFSET = 0x40;
    static constexpr uint32_t BOOT_CODE_SIZE = 0x1000 - 0x40;
    
    // Header field offsets
    static constexpr uint32_t OFFSET_PI_SETTINGS = 0x00;
    static constexpr uint32_t OFFSET_CLOCK_RATE = 0x04;
    static constexpr uint32_t OFFSET_PROGRAM_COUNTER = 0x08;
    static constexpr uint32_t OFFSET_RELEASE = 0x0C;
    static constexpr uint32_t OFFSET_CRC1 = 0x10;
    static constexpr uint32_t OFFSET_CRC2 = 0x14;
    static constexpr uint32_t OFFSET_NAME = 0x20;
    static constexpr uint32_t OFFSET_MANUFACTURER = 0x3B;
    static constexpr uint32_t OFFSET_CART_ID = 0x3C;
    static constexpr uint32_t OFFSET_COUNTRY_CODE = 0x3E;
    
    static constexpr uint32_t NAME_LENGTH = 20;
    
    // ROM validation magic bytes
    static constexpr std::array<uint8_t, 4> MAGIC_BIG_ENDIAN = {0x80, 0x37, 0x12, 0x40};
    static constexpr std::array<uint8_t, 4> MAGIC_BYTE_SWAPPED = {0x37, 0x80, 0x40, 0x12};
    static constexpr std::array<uint8_t, 4> MAGIC_LITTLE_ENDIAN = {0x40, 0x12, 0x37, 0x80};
};

/**
 * @class RomImage
 * @brief Represents a loaded ROM image with access methods
 */
class RomImage {
public:
    RomImage() = default;
    ~RomImage() = default;
    
    // Non-copyable, movable
    RomImage(const RomImage&) = delete;
    RomImage& operator=(const RomImage&) = delete;
    RomImage(RomImage&&) = default;
    RomImage& operator=(RomImage&&) = default;
    
    /**
     * @brief Get raw pointer to ROM data
     */
    const uint8_t* Data() const noexcept { return m_Data.data(); }
    uint8_t* Data() noexcept { return m_Data.data(); }
    
    /**
     * @brief Get size of ROM data
     */
    size_t Size() const noexcept { return m_Data.size(); }
    
    /**
     * @brief Check if ROM image is loaded
     */
    bool IsLoaded() const noexcept { return !m_Data.empty(); }
    
    /**
     * @brief Get ROM format
     */
    RomFormat GetFormat() const noexcept { return m_Format; }
    
    /**
     * @brief Get country code
     */
    CountryCode GetCountry() const noexcept { return m_Country; }
    
    /**
     * @brief Get CIC chip type
     */
    CICChip GetCICChip() const noexcept { return m_CICChip; }
    
    /**
     * @brief Get ROM name from header
     */
    const std::string& GetRomName() const noexcept { return m_RomName; }
    
    /**
     * @brief Get file name
     */
    const std::string& GetFileName() const noexcept { return m_FileName; }
    
    /**
     * @brief Get file path
     */
    const fs::path& GetFilePath() const noexcept { return m_FilePath; }
    
    /**
     * @brief Get MD5 hash
     */
    const std::string& GetMD5() const noexcept { return m_MD5; }
    
    /**
     * @brief Check if ROM is PAL region
     */
    bool IsPal() const noexcept;
    
    /**
     * @brief Check if ROM is NTSC region
     */
    bool IsNtsc() const noexcept;
    
    /**
     * @brief Check if ROM is 64DD IPL
     */
    bool IsDDIPL() const noexcept { return m_IsDDIPL; }
    
private:
    friend class N64Rom;
    
    std::vector<uint8_t> m_Data;
    RomFormat m_Format = RomFormat::Unknown;
    CountryCode m_Country = CountryCode::Unknown;
    CICChip m_CICChip = CICChip::Unknown;
    std::string m_RomName;
    std::string m_FileName;
    std::string m_MD5;
    fs::path m_FilePath;
    bool m_IsDDIPL = false;
};

/**
 * @class RomError
 * @brief Exception for ROM-related errors
 */
class RomError : public std::runtime_error {
public:
    enum class Type {
        FileNotFound,
        InvalidFormat,
        InvalidSize,
        ReadError,
        ChecksumError,
        OutOfMemory,
        ZipError
    };
    
    RomError(Type type, const std::string& message)
        : std::runtime_error(message)
        , m_Type(type)
    {}
    
    Type GetType() const noexcept { return m_Type; }
    
private:
    Type m_Type;
};

/**
 * @class N64Rom
 * @brief Main class for managing N64 ROM images
 */
class N64Rom final {
public:
    /**
     * @brief Construct an empty ROM manager
     */
    N64Rom();
    ~N64Rom();
    
    // Non-copyable, movable
    N64Rom(const N64Rom&) = delete;
    N64Rom& operator=(const N64Rom&) = delete;
    N64Rom(N64Rom&&) = default;
    N64Rom& operator=(N64Rom&&) = default;
    
    // ROM loading and management
    /**
     * @brief Load a ROM image from file
     * @param filePath Path to ROM image file
     * @param loadBootCodeOnly Only load the boot code section
     * @throws RomError on failure
     */
    void LoadRom(const fs::path& filePath, bool loadBootCodeOnly = false);
    
    /**
     * @brief Load a 64DD IPL ROM
     * @param filePath Path to IPL ROM file
     * @param loadBootCodeOnly Only load the boot code section
     * @throws RomError on failure
     */
    void LoadIPL(const fs::path& filePath, bool loadBootCodeOnly = false);
    
    /**
     * @brief Unload current ROM image
     */
    void UnloadRom() noexcept;
    
    /**
     * @brief Check if ROM data is valid
     * @param testBytes First 4 bytes of ROM data
     * @return true if valid ROM header
     */
    static constexpr bool IsValidRom(const std::array<uint8_t, 4>& testBytes) noexcept;
    
    // ROM information
    /**
     * @brief Check if ROM is loaded
     */
    bool IsLoaded() const noexcept { return m_Image && m_Image->IsLoaded(); }
    
    /**
     * @brief Get loaded ROM image
     */
    const RomImage& GetImage() const;
    
    /**
     * @brief Get ROM format
     */
    RomFormat GetFormat() const;
    
    /**
     * @brief Get ROM size
     */
    size_t GetSize() const;
    
    /**
     * @brief Get ROM header data
     */
    DataView GetHeader() const;
    
    /**
     * @brief Get boot code section
     */
    DataView GetBootCode() const;
    
    /**
     * @brief Get raw pointer to ROM data
     */
    const uint8_t* GetRomAddress() const;
    uint8_t* GetRomAddress();
    
    // CIC detection
    /**
     * @brief Detect CIC chip type from ROM data
     * @param romData Pointer to ROM data
     * @param crc Optional output for CIC CRC value
     * @return Detected CIC chip type
     */
    static CICChip DetectCICChip(const uint8_t* romData, uint64_t* crc = nullptr) noexcept;
    
    // ROM name utilities
    /**
     * @brief Clean ROM name string
     * @param romName Name string to clean
     * @param byteswap Whether to byteswap the name
     */
    static void CleanRomName(std::string& romName, bool byteswap = true);
    
    // Settings persistence
    void SaveRomSettingID(bool temp);
    void ClearRomSettingID();
    
private:
    // Internal helpers
    void DetectFormatAndByteSwap();
    void ParseHeader();
    void CalculateCICChip();
    void CalculateMD5();
    void CalculateRomCrc();
    
    bool AllocateImage(size_t size);
    void LoadFromFile(const fs::path& filePath, bool loadBootCodeOnly);
    void LoadFromZip(const fs::path& filePath, bool loadBootCodeOnly);
    void ByteSwapIfNeeded();
    void ByteSwapV64();
    void ByteSwapN64();
    
    static bool IsValidRomImage(const uint8_t* test) noexcept;
    
    // Member variables
    std::unique_ptr<RomImage> m_Image;
};

} // namespace Rom
} // namespace N64