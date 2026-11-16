/**
 * @file N64DD.hpp
 * @brief N64 Disk Drive (DD) image handling and management
 * 
 * Handles loading, saving, and manipulation of N64 Disk Drive images
 * Supports multiple formats: MAME, SDK, D64
 */

#pragma once

#include "Types.hpp"
#include <vector>
#include <memory>
#include <string>
#include <array>
#include <cstdint>
#include <optional>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace N64 {
namespace Disk {

/**
 * @enum DiskFormat
 * @brief Supported disk image formats
 */
enum class DiskFormat : uint8_t {
    MAME, ///< MAME format (0x0435B0C0 bytes)
    SDK,  ///< SDK format (0x03DEC800 bytes)
    D64,  ///< D64 format
    Unknown
};

/**
 * @enum DiskType
 * @brief Disk type information
 */
enum class DiskType : uint8_t {
    Retail,      ///< Retail disk
    Development, ///< Development disk
    Shadow,      ///< Shadow disk (save data)
    Unknown
};

/**
 * @enum CountryCode
 * @brief Country/region codes for N64DD disks
 */
enum class CountryCode : uint8_t {
    Japan,       ///< Japanese region
    USA,         ///< North American region
    Development, ///< Development/debug
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
 * @struct DiskGeometry
 * @brief Physical geometry of the disk
 */
struct DiskGeometry {
    static constexpr uint16_t SECTORS_PER_BLOCK = 85;
    static constexpr uint16_t BLOCKS_PER_TRACK = 2;
    static constexpr uint32_t MAX_LBA = 0x10DB;
    static constexpr uint32_t SYSTEM_LBAS = 24;
    static constexpr uint32_t DISKID_LBA = 14;

    // Sector sizes by zone
    static constexpr std::array<uint32_t, 16> SECTOR_SIZES = {232, 216, 208, 192, 176, 160, 144, 128,
                                                              216, 208, 192, 176, 160, 144, 128, 112};

    // Tracks per zone
    static constexpr std::array<uint32_t, 16> ZONE_TRACKS = {158, 158, 149, 149, 149, 149, 149, 114,
                                                             158, 158, 149, 149, 149, 149, 149, 114};

    // RAM start LBAs by disk type
    static constexpr std::array<uint16_t, 7> RAM_START_LBAS = {0x5A2, 0x7C6, 0x9EA, 0xC0E, 0xE32, 0x1010, 0x10DC};

    // RAM sizes by disk type
    static constexpr std::array<uint32_t, 7> RAM_SIZES = {0x24A9DC0, 0x1C226C0, 0x1450F00, 0xD35680,
                                                          0x6CFD40,  0x1DA240,  0x0};

    // Country codes
    static constexpr uint32_t COUNTRY_JAPAN = 0xE848D316;
    static constexpr uint32_t COUNTRY_USA = 0x2263EE56;
    static constexpr uint32_t COUNTRY_DEV = 0x00000000;

    static constexpr uint32_t BlockSize(uint8_t zone) noexcept { return SECTOR_SIZES[zone] * SECTORS_PER_BLOCK; }

    static constexpr uint32_t TrackSize(uint8_t zone) noexcept { return BlockSize(zone) * BLOCKS_PER_TRACK; }

    static constexpr uint32_t ZoneSize(uint8_t zone) noexcept { return TrackSize(zone) * ZONE_TRACKS[zone]; }
};

/**
 * @struct DiskAddress
 * @brief Represents a physical disk address
 */
struct DiskAddress {
    uint16_t head = 0;
    uint16_t track = 0;
    uint16_t block = 0;
    uint16_t sector = 0;

    bool IsValid() const noexcept { return track < 0x425 && block < 2 && sector < 85; }

    std::string ToString() const {
        std::ostringstream oss;
        oss << "H:" << head << " T:" << track << " B:" << block << " S:" << sector;
        return oss.str();
    }
};

/**
 * @class DiskImage
 * @brief Represents a loaded disk image with access methods
 */
class DiskImage {
public:
    DiskImage() = default;
    ~DiskImage() = default;

    // Non-copyable, movable
    DiskImage(const DiskImage&) = delete;
    DiskImage& operator=(const DiskImage&) = delete;
    DiskImage(DiskImage&&) = default;
    DiskImage& operator=(DiskImage&&) = default;

    const uint8_t* Data() const noexcept { return m_Data.data(); }
    uint8_t* Data() noexcept { return m_Data.data(); }

    size_t Size() const noexcept { return m_Data.size(); }

    bool IsLoaded() const noexcept { return !m_Data.empty(); }

    DiskFormat GetFormat() const noexcept { return m_Format; }

    DiskType GetType() const noexcept { return m_Type; }

    CountryCode GetCountry() const noexcept { return m_Country; }

    const std::string& GetIdentifier() const noexcept { return m_Identifier; }

    const std::string& GetRomName() const noexcept { return m_RomName; }

    const fs::path& GetFilePath() const noexcept { return m_FilePath; }

private:
    friend class N64Disk;

    std::vector<uint8_t> m_Data;
    DiskFormat m_Format = DiskFormat::Unknown;
    DiskType m_Type = DiskType::Unknown;
    CountryCode m_Country = CountryCode::Unknown;
    std::string m_Identifier;
    std::string m_RomName;
    fs::path m_FilePath;
};

/**
 * @class DiskError
 * @brief Exception for disk-related errors
 */
class DiskError : public std::runtime_error {
public:
    enum class Type {
        FileNotFound,
        InvalidFormat,
        InvalidSize,
        ReadError,
        WriteError,
        ChecksumError,
        GeometryError,
        OutOfMemory
    };

    DiskError(Type type, const std::string& message) : std::runtime_error(message), m_Type(type) {}

    Type GetType() const noexcept { return m_Type; }

private:
    Type m_Type;
};

/**
 * @class N64Disk
 * @brief Main class for managing N64 Disk Drive images
 */
class N64Disk final {
public:
    N64Disk();
    ~N64Disk();

    // Non-copyable, non-movable (handles system resources)
    N64Disk(const N64Disk&) = delete;
    N64Disk& operator=(const N64Disk&) = delete;
    N64Disk(N64Disk&&) = delete;
    N64Disk& operator=(N64Disk&&) = delete;

    // Disk loading and management
    /**
     * @brief Load a disk image from file
     * @param filePath Path to disk image file
     * @throws DiskError on failure
     */
    void LoadDisk(const fs::path& filePath);

    /**
     * @brief Save current disk image to file
     * @param filePath Optional path (uses original if not specified)
     * @throws DiskError on failure
     */
    void SaveDisk(const std::optional<fs::path>& filePath = std::nullopt);

    /**
     * @brief Swap to a different disk image
     * @param filePath Path to new disk image
     * @throws DiskError on failure
     */
    void SwapDisk(const fs::path& filePath);

    void UnloadDisk() noexcept;

    DiskType GetDiskType() const;
    uint8_t GetZoneForTrack(uint16_t track, uint16_t head) const;
    uint32_t LBAToVZone(uint32_t lba) const;
    uint8_t VZoneToPZone(uint32_t vzone, uint8_t diskType) const;
    uint32_t LBAToByte(uint32_t startLba, uint32_t numLbas) const;
    uint16_t LBAToPhys(uint32_t lba) const;

    /**
     * @brief Check if a disk image is valid
     * @param data Pointer to disk data
     * @param size Size of disk data
     * @return true if valid disk image
     */
    static bool IsValidDisk(const uint8_t* data, size_t size) noexcept;

    // Disk information
    bool IsLoaded() const noexcept { return m_Image && m_Image->IsLoaded(); }

    const DiskImage& GetImage() const;

    DiskFormat GetFormat() const;

    /**
     * @brief Get disk size
     */
    size_t GetSize() const;

    /**
     * @brief Get disk header (64DD IPL header)
     */
    DataView GetHeader() const;

    DataView GetIdentifier() const;

    DataView GetRomSection() const;

    DataView GetRamSection() const;

    DataView GetSystemArea() const;

    // Disk operations
    /**
     * @brief Convert physical address to byte offset
     * @param address Physical disk address
     * @return Byte offset in disk image
     * @throws DiskError if address is invalid
     */
    uint32_t AddressToOffset(const DiskAddress& address) const;

    /**
     * @brief Convert LBA to physical address
     * @param lba Logical block address
     * @return Physical disk address
     * @throws DiskError if LBA is invalid
     */
    DiskAddress LBAToAddress(uint32_t lba) const;

    /**
     * @brief Convert physical address to LBA
     * @param address Physical disk address
     * @return Logical block address
     * @throws DiskError if address is invalid
     */
    uint32_t AddressToLBA(const DiskAddress& address) const;

    /**
     * @brief Calculate disk CRC32 checksum
     */
    uint32_t CalculateChecksum() const;

    /**
     * @brief Verify disk integrity
     * @return true if disk is valid and not corrupted
     */
    bool VerifyIntegrity() const;

    // Save management
    /**
     * @brief Save disk settings/identifier
     * @param temporary If true, save to temporary location
     */
    void SaveDiskSettings(bool temporary = false);

    void ClearDiskSettings() noexcept;

    // Buffer management
    uint32_t GetBufferAddress() const noexcept { return m_BufferAddress; }

    /**
     * @brief Set buffer address
     * @param address New buffer address
     */
    void SetBufferAddress(uint32_t address) noexcept { m_BufferAddress = address; }

    const uint8_t* GetBuffer() const;

    uint8_t* GetBuffer();

private:
    // Internal helpers
    void DetectFormatAndGeometry();
    void ParseHeader();
    void DetectSystemArea();
    void DetectRamArea();
    void GenerateLBATable();

    bool IsSystemSectorValid(uint32_t block, uint32_t sectorSize) const;
    CountryCode DetectCountryCode() const;

    void InitializeD64Data();
    void CleanupD64Data();

    // Byte swapping
    void ByteSwapIfNeeded();
    void ForceByteSwap();

    // Memory management
    void AllocateImage(size_t size);
    void AllocateHeader();

    // Conversion tables
    struct ConversionTables {
        static const std::array<std::array<uint16_t, 16>, 7>& GetVZoneLBATable();
        static const std::array<std::array<uint8_t, 16>, 7>& GetVZonePZoneTable();
        static const std::array<std::array<uint16_t, 8>, 2>& GetSCylZoneTable();
        static const std::array<uint16_t, 8>& GetOuterCylTable();
        static const std::array<uint32_t, 16>& GetMAMEStartOffsets();
    };

    std::unique_ptr<DiskImage> m_Image;

    // Disk geometry state
    uint32_t m_SystemAreaOffset = 0;
    uint32_t m_IdentifierOffset = 0;
    uint32_t m_RomOffset = 0;
    uint32_t m_RamOffset = 0;
    uint32_t m_BufferAddress = 0;

    // LBA conversion table
    std::vector<uint16_t> m_LBAToPhysTable;

    // Helper methods
    bool AllocateAndLoadDiskImage(const fs::path& filePath);
    bool LoadDiskRAMImage();

    // D64-specific data
    struct D64Data;
    std::unique_ptr<D64Data> m_D64Data;

    // File handling
    class DiskFile;
    std::unique_ptr<DiskFile> m_DiskFile;

    // Error state
    std::optional<DiskError> m_LastError;
};

namespace DiskUtils {
DiskFormat DetectFormat(const uint8_t* data, size_t size) noexcept;

size_t GetExpectedSize(DiskFormat format) noexcept;

uint32_t CalculateCRC32(const uint8_t* data, size_t size) noexcept;

void ByteSwapDisk(uint8_t* data, size_t size, DiskFormat format);

std::string ExtractRomName(const uint8_t* header);

std::string ExtractIdentifier(const uint8_t* diskData, size_t size);
} // namespace DiskUtils

} // namespace Disk
} // namespace N64
