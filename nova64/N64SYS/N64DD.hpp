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

    /**
     * @brief Save current disk image to file
     * @param filePath Optional path (uses original if not specified)
     * @throws DiskError on failure
     */
    void SaveDisk(const std::optional<fs::path>& filePath = std::nullopt);

    void UnloadDisk() noexcept;

    // Disk information
    bool IsLoaded() const noexcept { return m_Image && m_Image->IsLoaded(); }

    const DiskImage& GetImage() const;

    DataView GetSystemArea() const;

    // Buffer management
    uint32_t GetBufferAddress() const noexcept { return m_BufferAddress; }

    /**
     * @brief Set buffer address
     * @param address New buffer address
     */
    void SetBufferAddress(uint32_t address) noexcept { m_BufferAddress = address; }

private:
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

    // D64-specific data
    struct D64Data;
    std::unique_ptr<D64Data> m_D64Data;

    // File handling
    class DiskFile;
    std::unique_ptr<DiskFile> m_DiskFile;

    // Error state
    std::optional<DiskError> m_LastError;
};

namespace DiskUtils {} // namespace DiskUtils

} // namespace Disk
} // namespace N64
