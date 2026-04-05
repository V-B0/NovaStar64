/**
 * @file N64DD.cpp
 * @brief N64 Disk Drive (DD) image handling and management implementation
 */

#include "N64DD.hpp"
#include "../common/platform.hpp"
#include "../common/MemoryManagment.hpp"
#include "../common/log.hpp"
#include "MIPSY/Register.hpp"

#include <fstream>
#include <cstring>
#include <algorithm>

namespace N64 {
namespace Disk {

static constexpr uint32_t MAME_FORMAT_SIZE = 0x0435B0C0;
static constexpr uint32_t SDK_FORMAT_SIZE = 0x03DEC800;
static constexpr uint32_t READ_CHUNK_SIZE = 0x100000; // 1MB chunks for reading
static constexpr uint32_t SECTORS_PER_BLOCK = 85;
static constexpr uint32_t SYSTEM_LBAS = 24;
static constexpr uint32_t DISKID_LBA = 14;
static constexpr uint32_t MAX_LBA = 0x10DB;
static constexpr uint32_t SIZE_LBA = MAX_LBA + 1;

// Country codes
static constexpr uint32_t DISK_COUNTRY_JPN = 0xE848D316;
static constexpr uint32_t DISK_COUNTRY_USA = 0x2263EE56;
static constexpr uint32_t DISK_COUNTRY_DEV = 0x00000000;

// Sector sizes by zone (physical zone 0-7)
static constexpr uint32_t SECTORSIZE_P[8] = {
    232, 216, 208, 192, 176, 160, 144, 128
};

// RAM start LBA by disk type (0-6)
static constexpr uint16_t RAM_START_LBA[7] = {
    0x5A2, 0x7C6, 0x9EA, 0xC0E, 0xE32, 0x1010, 0x10DC
};

// RAM sizes by disk type
static constexpr uint32_t RAM_SIZES[7] = {
    0x24A9DC0, 0x1C226C0, 0x1450F00, 0xD35680, 0x6CFD40, 0x1DA240, 0x0
};

// Virtual zone to LBA table [disk_type][vzone]
static constexpr uint16_t VZONE_LBA_TBL[7][16] = {
    {0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08B4, 0x09C6, 0x0AD8, 0x0BEA, 0x0CFC, 0x0E0E, 0x0F20, 0x1032, 0x10DC},
    {0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08B4, 0x09C6, 0x0AD8, 0x0BEA, 0x0CFC, 0x0E0E, 0x0F20, 0x1032, 0x10DC},
    {0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08B4, 0x09C6, 0x0AD8, 0x0BEA, 0x0CFC, 0x0E0E, 0x0F20, 0x1032, 0x10DC},
    {0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08B4, 0x09C6, 0x0AD8, 0x0BEA, 0x0CFC, 0x0E0E, 0x0F20, 0x1032, 0x10DC},
    {0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08B4, 0x09C6, 0x0AD8, 0x0BEA, 0x0CFC, 0x0E0E, 0x0F20, 0x1032, 0x10DC},
    {0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08B4, 0x09C6, 0x0AD8, 0x0BEA, 0x0CFC, 0x0E0E, 0x0F20, 0x1032, 0x10DC},
    {0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08B4, 0x09C6, 0x0AD8, 0x0BEA, 0x0CFC, 0x0E0E, 0x0F20, 0x1032, 0x10DC}
};

// Virtual zone to physical zone table [disk_type][vzone]
static constexpr uint8_t VZONE_PZONE_TBL[7][16] = {
    {0x0, 0x1, 0x2, 0x9, 0x8, 0x3, 0x4, 0x5, 0x6, 0x7, 0xF, 0xE, 0xD, 0xC, 0xB, 0xA},
    {0x0, 0x1, 0x2, 0x3, 0xA, 0x9, 0x8, 0x4, 0x5, 0x6, 0x7, 0xF, 0xE, 0xD, 0xC, 0xB},
    {0x0, 0x1, 0x2, 0x3, 0x4, 0xB, 0xA, 0x9, 0x8, 0x5, 0x6, 0x7, 0xF, 0xE, 0xD, 0xC},
    {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0xC, 0xB, 0xA, 0x9, 0x8, 0x6, 0x7, 0xF, 0xE, 0xD},
    {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8, 0x7, 0xF, 0xE},
    {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8, 0xF},
    {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0xF, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8}
};

// Start cylinder zone table [head][pzone]
static constexpr uint16_t SCYL_ZONE_TBL[2][16] = {
    {0x000, 0x09E, 0x13C, 0x1D1, 0x266, 0x2FB, 0x390, 0x425, 0x000, 0x09E, 0x13C, 0x1D1, 0x266, 0x2FB, 0x390, 0x425},
    {0x091, 0x12F, 0x1C4, 0x259, 0x2EE, 0x383, 0x418, 0x48A, 0x091, 0x12F, 0x1C4, 0x259, 0x2EE, 0x383, 0x418, 0x48A}
};

// Outer cylinder table
static constexpr uint16_t OUTERCYL_TBL[8] = {
    0x000, 0x09E, 0x13C, 0x1D1, 0x266, 0x2FB, 0x390, 0x425
};

// MAME format start offsets per zone
static constexpr uint32_t MAME_START_OFFSET[16] = {
    0x00000000, 0x005F15E0, 0x00B79D00, 0x010801A0, 0x01523EC0,
    0x019B8B60, 0x01DF3680, 0x021CA6E0, 0x02539320, 0x02AEBC40,
    0x03030FC0, 0x03509FC0, 0x03985500, 0x03D9C000, 0x04148700,
    0x04499880
};

// Block size calculation macro-like inline
inline uint32_t BLOCKSIZE(uint8_t zone) {
    return SECTORSIZE_P[zone & 7] * SECTORS_PER_BLOCK;
}

inline uint32_t TRACKSIZE(uint8_t zone) {
    return BLOCKSIZE(zone) * 2;
}

// ============================================================================
// D64Data structure for D64 format handling
// ============================================================================

struct N64Disk::D64Data {
    bool initialized = false;
    uint16_t romLbaEnd = 0;
    uint16_t ramLbaStart = 0;
    uint16_t ramLbaEnd = 0;
};

// ============================================================================
// DiskFile class for file I/O
// ============================================================================

class N64Disk::DiskFile {
public:
    DiskFile() = default;
    ~DiskFile() { Close(); }
    
    bool Open(const fs::path& path, std::ios_base::openmode mode) {
        m_Path = path;
        m_Stream.open(path, mode | std::ios::binary);
        return m_Stream.is_open();
    }
    
    void Close() {
        if (m_Stream.is_open()) {
            m_Stream.close();
        }
    }
    
    bool IsOpen() const { return m_Stream.is_open(); }
    
    bool Read(uint8_t* buffer, size_t size) {
        m_Stream.read(reinterpret_cast<char*>(buffer), size);
        return m_Stream.good() || m_Stream.eof();
    }
    
    bool Write(const uint8_t* buffer, size_t size) {
        m_Stream.write(reinterpret_cast<const char*>(buffer), size);
        return m_Stream.good();
    }
    
    bool Seek(std::streamoff offset, std::ios_base::seekdir dir = std::ios::beg) {
        m_Stream.seekg(offset, dir);
        m_Stream.seekp(offset, dir);
        return m_Stream.good();
    }
    
    void SeekToBegin() {
        Seek(0, std::ios::beg);
    }
    
    size_t GetLength() {
        auto current = m_Stream.tellg();
        m_Stream.seekg(0, std::ios::end);
        auto length = m_Stream.tellg();
        m_Stream.seekg(current);
        return static_cast<size_t>(length);
    }
    
    size_t Read(uint8_t* buffer, size_t offset, size_t size) {
        Seek(offset);
        m_Stream.read(reinterpret_cast<char*>(buffer), size);
        return static_cast<size_t>(m_Stream.gcount());
    }
    
private:
    std::fstream m_Stream;
    fs::path m_Path;
};

// ============================================================================
// N64Disk Implementation
// ============================================================================

N64Disk::N64Disk()
    : m_Image(std::make_unique<DiskImage>())
    , m_D64Data(std::make_unique<D64Data>())
    , m_DiskFile(std::make_unique<DiskFile>())
{
}

N64Disk::~N64Disk() {
    UnloadDisk();
}

void N64Disk::LoadDisk(const fs::path& filePath) {
    UnloadDisk();
    
    if (!fs::exists(filePath)) {
        throw DiskError(DiskError::Type::FileNotFound, 
            "Disk file not found: " + filePath.string());
    }
    
    // Try to load shadow file first (for save data)
    fs::path shadowPath = filePath;
    shadowPath.replace_extension(".ndr");
    
    bool isShadowDisk = false;
    if (fs::exists(shadowPath) && !AllocateAndLoadDiskImage(shadowPath)) {
        // Shadow file failed, try original
        UnloadDisk();
        if (!AllocateAndLoadDiskImage(filePath)) {
            throw DiskError(DiskError::Type::ReadError, 
                "Failed to load disk image: " + filePath.string());
        }
    } else if (fs::exists(shadowPath)) {
        isShadowDisk = true;
    } else {
        // No shadow file, load original
        if (!AllocateAndLoadDiskImage(filePath)) {
            throw DiskError(DiskError::Type::ReadError, 
                "Failed to load disk image: " + filePath.string());
        }
    }
    
    m_Image->m_FilePath = filePath;
    
    // Calculate disk identifier
    uint32_t crc1 = CalculateChecksum();
    uint32_t crc2 = ~crc1;
    
    // Extract ROM name from disk ID
    auto diskId = GetIdentifier();
    if (!diskId.empty() && diskId.size >= 4) {
        char romName[5] = {0};
        if (*reinterpret_cast<const uint32_t*>(diskId.data) != 0) {
            romName[0] = static_cast<char>(diskId.data[3]);
            romName[1] = static_cast<char>(diskId.data[2]);
            romName[2] = static_cast<char>(diskId.data[1]);
            romName[3] = static_cast<char>(diskId.data[0]);
        } else {
            // Generate from CRC
            std::snprintf(romName, sizeof(romName), "%04X", crc1 & 0xFFFF);
        }
        m_Image->m_RomName = romName;
    }
    
    // Set identifier string
    char identBuf[32];
    std::snprintf(identBuf, sizeof(identBuf), "%08X-%08X-C:%X", 
        crc1, crc2, diskId.empty() ? 0 : diskId.data[0]);
    m_Image->m_Identifier = identBuf;
    
    // Detect country code
    m_Image->m_Country = DetectCountryCode();
    
    // Generate LBA to physical table
    GenerateLBATable();
    
    // Initialize D64 data if needed
    InitializeD64Data();
    
    // Detect RAM address
    DetectRamArea();
    
    // Try to load RAM save file
    LoadDiskRAMImage();

    // No need to save if disk type is 6 (no RAM area)
    uint8_t diskType = static_cast<uint8_t>(GetDiskType());
    if (diskType == 6) {
        return;
    }
    
    fs::path savePath = filePath.empty() ? m_Image->m_FilePath : filePath;
    
    if (m_Image->m_Format == DiskFormat::MAME) {
        // Shadow file for MAME format
        fs::path mameShadowPath = savePath;
        mameShadowPath.replace_extension(".ndr");
        
        std::ofstream file(mameShadowPath, std::ios::binary);
        if (!file) {
            throw DiskError(DiskError::Type::WriteError, 
                "Failed to open shadow file: " + mameShadowPath.string());
        }
        
        ForceByteSwap();
        file.write(reinterpret_cast<const char*>(m_Image->m_Data.data()), 
                   m_Image->m_Data.size());
        ForceByteSwap(); // Swap back
        
        if (!file.good()) {
            throw DiskError(DiskError::Type::WriteError, "Failed to write shadow file");
        }
    } else {
        // RAM file for other formats
        if (m_RamOffset == 0 || m_Image->m_Data.size() <= m_RamOffset) {
            return;
        }
        
        fs::path ramPath = savePath;
        ramPath.replace_extension(".ram");
        
        std::ofstream file(ramPath, std::ios::binary);
        if (!file) {
            throw DiskError(DiskError::Type::WriteError, 
                "Failed to open RAM file: " + ramPath.string());
        }
        
        ForceByteSwap();
        file.write(reinterpret_cast<const char*>(GetRamSection().data), 
                   m_Image->m_Data.size() - m_RamOffset);
        ForceByteSwap(); // Swap back
        
        if (!file.good()) {
            throw DiskError(DiskError::Type::WriteError, "Failed to write RAM file");
        }
    }
}

void N64Disk::SwapDisk(const fs::path& filePath) {
    // Unload current disk before loading the new one.
    // ASIC_STATUS disk-presence bits are managed by CachedRom::SetDiskData
    // at the system level when the new disk data is linked.
    UnloadDisk();
    LoadDisk(filePath);
}

void N64Disk::UnloadDisk() noexcept {
    if (m_DiskFile) {
        m_DiskFile->Close();
    }
    
    if (m_Image) {
        m_Image->m_Data.clear();
        m_Image->m_Format = DiskFormat::Unknown;
        m_Image->m_Type = DiskType::Unknown;
        m_Image->m_Country = CountryCode::Unknown;
        m_Image->m_Identifier.clear();
        m_Image->m_RomName.clear();
        m_Image->m_FilePath.clear();
    }
    
    m_SystemAreaOffset = 0;
    m_IdentifierOffset = 0;
    m_RomOffset = 0;
    m_RamOffset = 0;
    m_BufferAddress = 0;
    m_LBAToPhysTable.clear();
    
    if (m_D64Data) {
        m_D64Data->initialized = false;
    }
    
    m_LastError.reset();
}

bool N64Disk::IsValidDisk(const uint8_t* data, size_t size) noexcept {
    if (!data || size < 0x20) {
        return false;
    }
    
    // Basic system data check (first 0x20 bytes is enough)
    
    // Disk type check
    if ((data[0x05] & 0xEF) > 6) {
        return false;
    }
    
    // IPL load block check
    uint16_t iplLoadBlk = (static_cast<uint16_t>(data[0x06]) << 8) | data[0x07];
    if (iplLoadBlk > 0x10C3 || iplLoadBlk == 0x0000) {
        return false;
    }
    
    // IPL load address check
    uint32_t iplLoadAddr = (static_cast<uint32_t>(data[0x1C]) << 24) |
                           (static_cast<uint32_t>(data[0x1D]) << 16) |
                           (static_cast<uint32_t>(data[0x1E]) << 8) |
                           data[0x1F];
    if (iplLoadAddr < 0x80000000 || iplLoadAddr >= 0x80800000) {
        return false;
    }
    
    // Country code check
    uint32_t countryCode = *reinterpret_cast<const uint32_t*>(data);
    if (countryCode == DISK_COUNTRY_JPN || 
        countryCode == DISK_COUNTRY_USA ||
        countryCode == DISK_COUNTRY_DEV) {
        return true;
    }
    
    return false;
}

const DiskImage& N64Disk::GetImage() const {
    if (!m_Image || !m_Image->IsLoaded()) {
        throw DiskError(DiskError::Type::ReadError, "No disk loaded");
    }
    return *m_Image;
}

DiskFormat N64Disk::GetFormat() const {
    return m_Image ? m_Image->m_Format : DiskFormat::Unknown;
}

size_t N64Disk::GetSize() const {
    return m_Image ? m_Image->m_Data.size() : 0;
}

DataView N64Disk::GetHeader() const {
    if (!IsLoaded()) {
        return DataView{nullptr, 0};
    }
    return DataView{m_Image->m_Data.data() + m_SystemAreaOffset, 0x40};
}

DataView N64Disk::GetIdentifier() const {
    if (!IsLoaded()) {
        return DataView{nullptr, 0};
    }
    return DataView{m_Image->m_Data.data() + m_IdentifierOffset, 0x20};
}

DataView N64Disk::GetRomSection() const {
    if (!IsLoaded() || m_RomOffset == 0) {
        return DataView{nullptr, 0};
    }
    size_t romSize = (m_RamOffset > m_RomOffset) ? 
        (m_RamOffset - m_RomOffset) : (m_Image->m_Data.size() - m_RomOffset);
    return DataView{m_Image->m_Data.data() + m_RomOffset, romSize};
}

DataView N64Disk::GetRamSection() const {
    if (!IsLoaded() || m_RamOffset == 0) {
        return DataView{nullptr, 0};
    }
    return DataView{m_Image->m_Data.data() + m_RamOffset, 
                    m_Image->m_Data.size() - m_RamOffset};
}

DataView N64Disk::GetSystemArea() const {
    if (!IsLoaded()) {
        return DataView{nullptr, 0};
    }
    return DataView{m_Image->m_Data.data() + m_SystemAreaOffset, 
                    BLOCKSIZE(0) * SYSTEM_LBAS};
}

uint32_t N64Disk::AddressToOffset(const DiskAddress& address) const {
    if (!IsLoaded() || !address.IsValid()) {
        throw DiskError(DiskError::Type::GeometryError, "Invalid disk address");
    }
    
    uint16_t head = address.head;
    uint16_t track = address.track;
    uint16_t block = address.block;
    uint16_t sector = address.sector;
    
    // Get sector size for this track
    uint8_t zone = GetZoneForTrack(track, head);
    uint32_t sectorSize = SECTORSIZE_P[zone];
    
    uint32_t offset = 0;
    
    if (m_Image->m_Format == DiskFormat::MAME) {
        // MAME format calculation
        uint32_t trOff = 0;
        uint16_t ddZone = 0;
        
        if (track >= 0x425) {
            ddZone = 7 + head;
            trOff = track - 0x425;
        } else if (track >= 0x390) {
            ddZone = 6 + head;
            trOff = track - 0x390;
        } else if (track >= 0x2FB) {
            ddZone = 5 + head;
            trOff = track - 0x2FB;
        } else if (track >= 0x266) {
            ddZone = 4 + head;
            trOff = track - 0x266;
        } else if (track >= 0x1D1) {
            ddZone = 3 + head;
            trOff = track - 0x1D1;
        } else if (track >= 0x13C) {
            ddZone = 2 + head;
            trOff = track - 0x13C;
        } else if (track >= 0x9E) {
            ddZone = 1 + head;
            trOff = track - 0x9E;
        } else {
            ddZone = 0 + head;
            trOff = track;
        }
        
        offset = MAME_START_OFFSET[ddZone] + 
                 trOff * TRACKSIZE(ddZone) + 
                 block * BLOCKSIZE(ddZone) + 
                 sector * sectorSize;
    } else if (m_Image->m_Format == DiskFormat::SDK) {
        // SDK format calculation
        uint16_t lba = AddressToLBA(address);
        offset = LBAToByte(0, lba) + sector * sectorSize;
    } else {
        // D64 format calculation
        uint16_t lba = AddressToLBA(address);
        uint16_t romLbaEnd = m_D64Data ? m_D64Data->romLbaEnd : static_cast<uint16_t>(0);
        uint16_t ramLbaStart = m_D64Data ? m_D64Data->ramLbaStart : static_cast<uint16_t>(0);
        uint16_t ramLbaEnd = m_D64Data ? m_D64Data->ramLbaEnd : static_cast<uint16_t>(0);
        
        if (lba < DISKID_LBA) {
            offset = m_SystemAreaOffset;
        } else if (lba < SYSTEM_LBAS) {
            offset = m_IdentifierOffset;
        } else if (lba <= (romLbaEnd + SYSTEM_LBAS)) {
            offset = 0x200 + LBAToByte(SYSTEM_LBAS, lba - SYSTEM_LBAS) + sector * sectorSize;
        } else if (((lba - SYSTEM_LBAS) <= ramLbaEnd) && ((lba - SYSTEM_LBAS) >= ramLbaStart)) {
            offset = 0x200 + LBAToByte(SYSTEM_LBAS, romLbaEnd + 1);
            offset += LBAToByte(ramLbaStart + SYSTEM_LBAS, lba - ramLbaStart - SYSTEM_LBAS) + sector * sectorSize;
        } else {
            throw DiskError(DiskError::Type::GeometryError, "Address out of range");
        }
    }
    
    if (offset >= m_Image->m_Data.size()) {
        throw DiskError(DiskError::Type::GeometryError, "Calculated offset out of range");
    }
    
    return offset;
}

DiskAddress N64Disk::LBAToAddress(uint32_t lba) const {
    if (!IsLoaded() || lba > MAX_LBA) {
        throw DiskError(DiskError::Type::GeometryError, "Invalid LBA");
    }
    
    if (lba >= m_LBAToPhysTable.size()) {
        throw DiskError(DiskError::Type::GeometryError, "LBA out of table range");
    }
    
    uint16_t physValue = m_LBAToPhysTable[lba];
    
    DiskAddress addr;
    addr.track = physValue & 0x0FFF;
    addr.head = (physValue >> 12) & 0x01;
    addr.block = (physValue >> 13) & 0x01;
    addr.sector = 0;
    
    return addr;
}

uint32_t N64Disk::AddressToLBA(const DiskAddress& address) const {
    if (!IsLoaded()) {
        throw DiskError(DiskError::Type::GeometryError, "No disk loaded");
    }
    
    uint16_t expectedValue = address.track | 
                             (address.head << 12) | 
                             (address.block << 13);
    
    for (uint32_t lba = 0; lba < m_LBAToPhysTable.size(); ++lba) {
        if (m_LBAToPhysTable[lba] == expectedValue) {
            return lba;
        }
    }
    
    throw DiskError(DiskError::Type::GeometryError, "Cannot convert address to LBA");
}

uint32_t N64Disk::CalculateChecksum() const {
    if (!IsLoaded()) {
        return 0;
    }
    
    // Custom CRC calculation (same as Project64)
    uint32_t crc = 0;
    auto romSection = GetRomSection();
    
    size_t checkSize = std::min(static_cast<size_t>(0x4D08), romSection.size);
    for (size_t i = 0; i < checkSize; i += 4) {
        crc += *reinterpret_cast<const uint32_t*>(romSection.data + i);
    }
    
    return crc;
}

bool N64Disk::VerifyIntegrity() const {
    if (!IsLoaded()) {
        return false;
    }
    
    // Verify system area
    auto sysArea = GetSystemArea();
    if (sysArea.empty()) {
        return false;
    }
    
    return IsValidDisk(sysArea.data, sysArea.size);
}

void N64Disk::SaveDiskSettings(bool temporary) {
    // Save disk settings to emulator configuration
    // This would typically save to a settings system
    // For now, just store the identification locally
}

void N64Disk::ClearDiskSettings() noexcept {
    // Clear any saved disk settings
}

const uint8_t* N64Disk::GetBuffer() const {
    if (!IsLoaded()) {
        return nullptr;
    }
    return m_Image->m_Data.data() + m_BufferAddress;
}

uint8_t* N64Disk::GetBuffer() {
    if (!IsLoaded()) {
        return nullptr;
    }
    return m_Image->m_Data.data() + m_BufferAddress;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

void N64Disk::DetectFormatAndGeometry() {
    if (!m_Image || m_Image->m_Data.empty()) {
        return;
    }
    
    size_t size = m_Image->m_Data.size();
    
    if (size == MAME_FORMAT_SIZE) {
        m_Image->m_Format = DiskFormat::MAME;
    } else if (size == SDK_FORMAT_SIZE) {
        m_Image->m_Format = DiskFormat::SDK;
    } else if (size > 0x4F08) {
        m_Image->m_Format = DiskFormat::D64;
    } else {
        m_Image->m_Format = DiskFormat::Unknown;
    }
}

void N64Disk::ParseHeader() {
    if (!IsLoaded()) {
        return;
    }
    
    auto sysData = GetSystemArea();
    if (sysData.empty()) {
        return;
    }
    
    // Extract disk type
    uint8_t diskType = sysData.data[5] & 0x0F;
    if (diskType <= 6) {
        m_Image->m_Type = DiskType::Retail;
    }
}

void N64Disk::DetectSystemArea() {
    if (!m_Image || m_Image->m_Data.empty()) {
        return;
    }
    
    if (m_Image->m_Format == DiskFormat::MAME || m_Image->m_Format == DiskFormat::SDK) {
        // MAME / SDK (system area can be handled identically)
        m_SystemAreaOffset = 0;
        m_IdentifierOffset = static_cast<uint32_t>(DISKID_LBA * 0x4D08);
        m_RomOffset = static_cast<uint32_t>(SYSTEM_LBAS * 0x4D08);
        
        // Handle system data - check for valid system blocks
        const uint16_t sysblocks[4] = {9, 8, 1, 0};
        bool isDevDisk = false;
        
        for (int i = 0; i < 4; i++) {
            if (IsSystemSectorValid(sysblocks[i] + 2, 0xC0)) {
                m_SystemAreaOffset = static_cast<uint32_t>((sysblocks[i] + 2) * 0x4D08);
                isDevDisk = true;
                break;
            }
        }
        
        if (!isDevDisk) {
            for (int i = 0; i < 4; i++) {
                if (IsSystemSectorValid(sysblocks[i], 0xE8)) {
                    m_SystemAreaOffset = static_cast<uint32_t>(sysblocks[i] * 0x4D08);
                    break;
                }
            }
        }
        
        // Handle disk ID

    for (int i = 2; i > 0; i--) {
        if (IsSystemSectorValid(DISKID_LBA + i, 0xE8)) {
            m_IdentifierOffset = static_cast<uint32_t>((DISKID_LBA + i) * 0x4D08);
            break;
        }
    }
        m_SystemAreaOffset = 0x000;
        m_IdentifierOffset = 0x100;
        m_RomOffset = 0x200;
    }
}

void N64Disk::DetectRamArea() {
    if (!IsLoaded()) {
        return;
    }
    
    uint8_t diskType = static_cast<uint8_t>(GetDiskType());
    
    if (m_Image->m_Format == DiskFormat::MAME) {
        // MAME format doesn't support RAM saves directly
        m_RamOffset = 0;
    } else if (m_Image->m_Format == DiskFormat::SDK) {
        m_RamOffset = LBAToByte(0, RAM_START_LBA[diskType]);
    } else { // D64 format
        auto sysData = GetSystemArea();
        if (!sysData.empty() && sysData.size >= 0xE2) {
            uint16_t romLbaEnd = (sysData.data[0xE0 ^ 2] << 8) | sysData.data[(0xE0 ^ 2) + 1];
            m_RamOffset = m_RomOffset + LBAToByte(SYSTEM_LBAS, romLbaEnd + 1);
        }
    }
}

void N64Disk::GenerateLBATable() {
    m_LBAToPhysTable.resize(SIZE_LBA);
    
    for (uint32_t lba = 0; lba < SIZE_LBA; ++lba) {
        m_LBAToPhysTable[lba] = LBAToPhys(lba);
    }
}

bool N64Disk::IsSystemSectorValid(uint32_t block, uint32_t sectorSize) const {
    if (!m_Image || m_Image->m_Data.empty()) {
        return false;
    }
    
    const uint8_t* data = m_Image->m_Data.data();
    size_t dataSize = m_Image->m_Data.size();
    
    uint32_t baseOffset = block * 0x4D08;
    if (baseOffset + SECTORS_PER_BLOCK * sectorSize > dataSize) {
        return false;
    }
    
    // Check if all sectors are identical
    for (uint32_t j = 1; j < SECTORS_PER_BLOCK; ++j) {
        for (uint32_t k = 0; k < sectorSize; ++k) {
            if (data[baseOffset + j * sectorSize + k] != data[baseOffset + k]) {
                return false;
            }
        }
    }
    
    if (block < DISKID_LBA) {
        // System format check
        if (data[baseOffset + 4] != 0x10) {
            return false;
        }
        
        // Disk format check
        if ((data[baseOffset + 5] & 0xF0) != 0x10) {
            return false;
        }
        
        // Check for 0xFFFFFFFF at offset 0x18
        if (*reinterpret_cast<const uint32_t*>(&data[baseOffset + 0x18]) != 0xFFFFFFFF) {
            return false;
        }
        
        // Check alternate track offsets
        uint8_t alt = ((block & 2) != 0) ? 0xA : 0xC;
        for (int i = 0; i < 16; ++i) {
            if (data[baseOffset + 8 + i] != ((i + 1) * alt)) {
                return false;
            }
        }
    }
    
    return true;
}

CountryCode N64Disk::DetectCountryCode() const {
    if (!IsLoaded()) {
        return CountryCode::Unknown;
    }
    
    auto sysData = GetSystemArea();
    if (sysData.empty() || sysData.size < 4) {
        return CountryCode::Unknown;
    }
    
    uint32_t code = *reinterpret_cast<const uint32_t*>(sysData.data);
    
    switch (code) {
        case DISK_COUNTRY_JPN:
            return CountryCode::Japan;
        case DISK_COUNTRY_USA:
            return CountryCode::USA;
        case DISK_COUNTRY_DEV:
            return CountryCode::Development;
        default:
            return CountryCode::Unknown;
    }
}

void N64Disk::InitializeD64Data() {
    if (!m_D64Data || m_Image->m_Format != DiskFormat::D64) {
        return;
    }
    
    auto sysData = GetSystemArea();
    if (sysData.empty() || sysData.size < 0xE6) {
        return;
    }
    
    // Read D64 specific data
    m_D64Data->romLbaEnd = (sysData.data[0xE0] << 8) | sysData.data[0xE1];
    m_D64Data->ramLbaStart = (sysData.data[0xE2] << 8) | sysData.data[0xE3];
    m_D64Data->ramLbaEnd = (sysData.data[0xE4] << 8) | sysData.data[0xE5];
    
    // Initialize system data for proper operation
    uint8_t* data = m_Image->m_Data.data();
    data[m_SystemAreaOffset + (4 ^ 3)] = 0x10;
    data[m_SystemAreaOffset + (5 ^ 3)] |= 0x10;
    
    uint8_t diskType = static_cast<uint8_t>(GetDiskType());
    if (diskType < 6) {
        // Expand RAM area
        uint16_t ramStart = RAM_START_LBA[diskType] - SYSTEM_LBAS;
        uint16_t ramEnd = MAX_LBA - SYSTEM_LBAS;
        data[m_SystemAreaOffset + (0xE2 ^ 2)] = (ramStart >> 8) & 0xFF;
        data[m_SystemAreaOffset + (0xE2 ^ 2) + 1] = ramStart & 0xFF;
        data[m_SystemAreaOffset + (0xE4 ^ 2)] = (ramEnd >> 8) & 0xFF;
        data[m_SystemAreaOffset + (0xE4 ^ 2) + 1] = ramEnd & 0xFF;
    } else {
        data[m_SystemAreaOffset + (0xE2 ^ 2)] = 0xFF;
        data[m_SystemAreaOffset + (0xE2 ^ 2) + 1] = 0xFF;
        data[m_SystemAreaOffset + (0xE4 ^ 2)] = 0xFF;
        data[m_SystemAreaOffset + (0xE4 ^ 2) + 1] = 0xFF;
    }
    
    m_D64Data->initialized = true;
}

void N64Disk::CleanupD64Data() {
    if (!m_D64Data || !m_D64Data->initialized || m_Image->m_Format != DiskFormat::D64) {
        return;
    }
    
    // Restore the original data
    uint8_t* data = m_Image->m_Data.data();
    data[m_SystemAreaOffset + (4 ^ 3)] = 0x00;
    data[m_SystemAreaOffset + (5 ^ 3)] &= 0x0F;
    
    m_D64Data->initialized = false;
}

void N64Disk::ByteSwapIfNeeded() {
    if (!IsLoaded()) {
        return;
    }
    
    auto sysData = GetSystemArea();
    if (sysData.size < 12) {
        return;
    }
    
    uint32_t check = *reinterpret_cast<const uint32_t*>(sysData.data + 8);
    
    switch (check) {
        case 0x281E140A:
        case 0x3024180C:
            // Needs byte swap
            ForceByteSwap();
            break;
        case 0x0A141E28:
        case 0x0C182430:
            // Already correct endian
            break;
        default:
            // Unknown format, try anyway
            break;
    }
}

void N64Disk::ForceByteSwap() {
    if (!IsLoaded()) {
        return;
    }
    
    uint8_t* data = m_Image->m_Data.data();
    size_t size = m_Image->m_Data.size();
    
    for (size_t i = 0; i < size; i += 4) {
        // Swap bytes 0 and 3
        data[i] ^= data[i + 3];
        data[i + 3] ^= data[i];
        data[i] ^= data[i + 3];
        
        // Swap bytes 1 and 2
        data[i + 1] ^= data[i + 2];
        data[i + 2] ^= data[i + 1];
        data[i + 1] ^= data[i + 2];
    }
}

void N64Disk::AllocateImage(size_t size) {
    if (!m_Image) {
        m_Image = std::make_unique<DiskImage>();
    }
    m_Image->m_Data.resize(size);
}

void N64Disk::AllocateHeader() {
    // Header is part of the main image in this implementation
}

bool N64Disk::AllocateAndLoadDiskImage(const fs::path& filePath) {
    if (!m_DiskFile) {
        m_DiskFile = std::make_unique<DiskFile>();
    }
    
    if (!m_DiskFile->Open(filePath, std::ios::in)) {
        return false;
    }
    
    size_t fileSize = m_DiskFile->GetLength();
    std::string ext = filePath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    // Validate disk by reading test data
    uint8_t testData[0x100];
    bool isValidDisk = false;
    
    const uint8_t blocks[8] = {0, 1, 2, 3, 8, 9, 10, 11};
    for (int i = 0; i < 8; ++i) {
        m_DiskFile->Seek(0x4D08 * blocks[i]);
        if (m_DiskFile->Read(testData, sizeof(testData)) != sizeof(testData)) {
            m_DiskFile->Close();
            return false;
        }
        
        isValidDisk = IsValidDisk(testData, sizeof(testData));
        if (isValidDisk) {
            break;
        }
    }
    
    if (!isValidDisk) {
        m_DiskFile->Close();
        return false;
    }
    
    // Determine format and allocate
    if ((fileSize == MAME_FORMAT_SIZE || fileSize == SDK_FORMAT_SIZE) &&
        (ext == ".ndr" || ext == ".ndd")) {
        
        if (fileSize == MAME_FORMAT_SIZE) {
            m_Image->m_Format = DiskFormat::MAME;
        } else {
            m_Image->m_Format = DiskFormat::SDK;
        }
        
        AllocateImage(fileSize);
        m_DiskFile->SeekToBegin();
        
        // Read in chunks
        size_t totalRead = 0;
        const size_t chunkSize = 1024 * 1024; 
        while (totalRead < fileSize) {
            size_t toRead = std::min(chunkSize, fileSize - totalRead);
            if (m_DiskFile->Read(m_Image->m_Data.data() + totalRead, toRead) != toRead) {
                m_DiskFile->Close();
                return false;
            }
            totalRead += toRead;
        }
        
        DetectSystemArea();
        ByteSwapIfNeeded();
        
    } else if (fileSize > 0x4F08 && (ext == ".d6r" || ext == ".d64")) {
        m_Image->m_Format = DiskFormat::D64;
        
        // Read header to get disk type info
        m_DiskFile->SeekToBegin();
        m_DiskFile->Read(testData, 0x100);
        
        uint8_t diskType = testData[5];
        uint16_t romLbaEnd = (testData[0xE0] << 8) | testData[0xE1];
        uint16_t ramLbaStart = (testData[0xE2] << 8) | testData[0xE3];
        uint16_t ramLbaEnd = (testData[0xE4] << 8) | testData[0xE5];
        
        // Validate D64 structure
        if ((romLbaEnd + SYSTEM_LBAS) >= DiskGeometry::RAM_START_LBAS[diskType] ||
            ((ramLbaStart + SYSTEM_LBAS) != DiskGeometry::RAM_START_LBAS[diskType] && ramLbaStart != 0xFFFF)) {
            m_DiskFile->Close();
            return false;
        }
        
        uint32_t romSize = LBAToByte(SYSTEM_LBAS, romLbaEnd + 1);
        uint32_t ramSize = 0;
        if (ramLbaStart != 0xFFFF && ramLbaEnd != 0xFFFF) {
            ramSize = LBAToByte(SYSTEM_LBAS + ramLbaStart, ramLbaEnd + 1 - ramLbaStart);
        }
        uint32_t fullRamSize = DiskGeometry::RAM_SIZES[diskType];
        
        if ((0x200 + romSize + ramSize) != fileSize) {
            m_DiskFile->Close();
            return false;
        }
        
        // Allocate with full RAM area
        AllocateImage(0x200 + romSize + fullRamSize);
        
        // Read file
        m_DiskFile->SeekToBegin();
        size_t totalRead = 0;
        const size_t chunkSize = 1024 * 1024;
        while (totalRead < fileSize) {
            size_t remaining = fileSize - totalRead;
            size_t toRead = (chunkSize < remaining) ? chunkSize : remaining;
            
            if (m_DiskFile->Read(m_Image->m_Data.data() + totalRead, toRead) != toRead) {
                m_DiskFile->Close();
                return false;
            }
            totalRead += toRead;
        }
        
        DetectSystemArea();
        ForceByteSwap();
        
    } else {
        m_DiskFile->Close();
        return false;
    }
    
    return true;
}

bool N64Disk::LoadDiskRAMImage() {
    if (!IsLoaded()) {
        return false;
    }
    
    // Don't load RAM if using shadow disk or MAME format
    if (m_Image->m_Format == DiskFormat::MAME || m_RamOffset == 0) {
        return true;
    }
    
    fs::path ramPath = m_Image->m_FilePath;
    ramPath.replace_extension(".ram");
    
    if (!fs::exists(ramPath)) {
        return true; // No RAM file is not an error
    }
    
    std::ifstream ramFile(ramPath, std::ios::binary);
    if (!ramFile) {
        return false;
    }
    
    ramFile.seekg(0, std::ios::end);
    size_t ramFileSize = static_cast<size_t>(ramFile.tellg());
    ramFile.seekg(0, std::ios::beg);
    
    if (ramFileSize != m_Image->m_Data.size() - m_RamOffset) {
        return false;
    }
    
    ForceByteSwap();
    ramFile.read(reinterpret_cast<char*>(m_Image->m_Data.data() + m_RamOffset), ramFileSize);
    ForceByteSwap();
    
    return ramFile.good();
}

DiskType N64Disk::GetDiskType() const {
    if (!IsLoaded()) {
        return DiskType::Unknown;
    }
    
    auto sysData = GetSystemArea();
    if (sysData.empty() || sysData.size < 6) {
        return DiskType::Unknown;
    }
    
    return static_cast<DiskType>(sysData.data[5 ^ 3] & 0x0F);
}

uint8_t N64Disk::GetZoneForTrack(uint16_t track, uint16_t head) const {
    if (track >= 0x425) return 7;
    else if (track >= 0x390) return 6;
    else if (track >= 0x2FB) return 5;
    else if (track >= 0x266) return 4;
    else if (track >= 0x1D1) return 3;
    else if (track >= 0x13C) return 2;
    else if (track >= 0x9E) return 1;
    else return 0;
}

uint32_t N64Disk::LBAToVZone(uint32_t lba) const {
    uint8_t diskType = static_cast<uint8_t>(GetDiskType());
    for (uint32_t vzone = 0; vzone < 16; ++vzone) {
        if (lba < VZONE_LBA_TBL[diskType][vzone]) {
            return vzone;
        }
    }
    return 0;
}

uint8_t N64Disk::VZoneToPZone(uint32_t vzone, uint8_t diskType) const {
    if (vzone >= 16 || diskType >= 7) {
        return 0;
    }
    return VZONE_PZONE_TBL[diskType][vzone];
}

uint32_t N64Disk::LBAToByte(uint32_t startLba, uint32_t numLbas) const {
    if (numLbas == 0) {
        return 0;
    }
    
    uint8_t diskType = static_cast<uint8_t>(GetDiskType());
    bool initFlag = true;
    uint32_t totalBytes = 0;
    uint32_t blockSize = 0;
    uint32_t vzone = 0, pzone = 0;
    uint32_t lba = startLba;
    
    for (; numLbas != 0; --numLbas) {
        if (initFlag || VZONE_LBA_TBL[diskType][vzone] == lba) {
            vzone = LBAToVZone(lba);
            pzone = VZoneToPZone(vzone, diskType);
            if (pzone > 7) {
                pzone -= 7;
            }
            blockSize = SECTORSIZE_P[pzone] * SECTORS_PER_BLOCK;
        }
        
        totalBytes += blockSize;
        ++lba;
        initFlag = false;
        
        if ((numLbas - 1) != 0 && lba > MAX_LBA) {
            return 0xFFFFFFFF;
        }
    }
    
    return totalBytes;
}

uint16_t N64Disk::LBAToPhys(uint32_t lba) const {
    auto sysData = GetSystemArea();
    if (sysData.empty()) {
        return 0xFFFF;
    }
    
    uint8_t diskType = static_cast<uint8_t>(GetDiskType());
    
    // Get block 0/1 on disk track
    uint8_t block = 1;
    if ((lba & 3) == 0 || (lba & 3) == 3) {
        block = 0;
    }
    
    // Get virtual and physical disk zones
    uint16_t vzone = static_cast<uint16_t>(LBAToVZone(lba));
    uint16_t pzone = VZoneToPZone(vzone, diskType);
    
    // Get disk head
    uint16_t head = (pzone > 7) ? 1 : 0;
    
    // Get disk zone
    uint16_t diskZone = pzone;
    if (diskZone > 7) {
        diskZone = pzone - 7;
    }
    
    // Get virtual zone LBA start
    uint16_t vzoneLba = 0;
    if (vzone != 0) {
        vzoneLba = VZONE_LBA_TBL[diskType][vzone - 1];
    }
    
    // Calculate physical track
    uint16_t track = static_cast<uint16_t>((lba - vzoneLba) >> 1);
    
    // Get the start track from current zone
    uint16_t trackZoneStart = SCYL_ZONE_TBL[0][pzone];
    if (head != 0) {
        track = static_cast<uint16_t>(-static_cast<int16_t>(track));
        trackZoneStart = OUTERCYL_TBL[diskZone - 1];
    }
    track += SCYL_ZONE_TBL[0][pzone];
    
    // Get defect track info
    uint16_t defectOffset = 0;
    if (pzone != 0) {
        defectOffset = sysData.data[(8 + pzone - 1) ^ 3];
    }
    
    uint16_t defectAmount = sysData.data[(8 + pzone) ^ 3] - defectOffset;
    
    // Skip defect tracks
    while (defectAmount != 0 && 
           (sysData.data[(0x20 + defectOffset) ^ 3] + trackZoneStart) <= track) {
        ++track;
        ++defectOffset;
        --defectAmount;
    }
    
    return track | (head << 12) | (block << 13);
}

// ============================================================================
// DiskUtils Implementation
// ============================================================================

namespace DiskUtils {

DiskFormat DetectFormat(const uint8_t* data, size_t size) noexcept {
    if (!data || size == 0) {
        return DiskFormat::Unknown;
    }
    
    if (size == MAME_FORMAT_SIZE) {
        return DiskFormat::MAME;
    } else if (size == SDK_FORMAT_SIZE) {
        return DiskFormat::SDK;
    } else if (size > 0x4F08) {
        return DiskFormat::D64;
    }
    
    return DiskFormat::Unknown;
}

size_t GetExpectedSize(DiskFormat format) noexcept {
    switch (format) {
        case DiskFormat::MAME:
            return MAME_FORMAT_SIZE;
        case DiskFormat::SDK:
            return SDK_FORMAT_SIZE;
        case DiskFormat::D64:
            return 0; // Variable size
        default:
            return 0;
    }
}

uint32_t CalculateCRC32(const uint8_t* data, size_t size) noexcept {
    if (!data || size == 0) {
        return 0;
    }
    
    // Simple CRC32 implementation
    uint32_t crc = 0xFFFFFFFF;
    static constexpr uint32_t polynomial = 0xEDB88320;
    
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ polynomial;
            } else {
                crc >>= 1;
            }
        }
    }
    
    return ~crc;
}

void ByteSwapDisk(uint8_t* data, size_t size, DiskFormat format) {
    if (!data || size < 4) {
        return;
    }
    
    for (size_t i = 0; i < size; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
    }
}

std::string ExtractRomName(const uint8_t* header) {
    if (!header) {
        return "";
    }
    
    char name[5] = {0};
    name[0] = static_cast<char>(header[3]);
    name[1] = static_cast<char>(header[2]);
    name[2] = static_cast<char>(header[1]);
    name[3] = static_cast<char>(header[0]);
    
    return std::string(name);
}

std::string ExtractIdentifier(const uint8_t* diskData, size_t size) {
    if (!diskData || size < 0x20) {
        return "";
    }
    
    // Calculate simple identifier from first 32 bytes
    uint32_t hash = 0;
    for (size_t i = 0; i < 0x20; ++i) {
        hash = (hash << 1) ^ diskData[i];
    }
    
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", hash);
    return std::string(buf);
}

} // namespace DiskUtils

} // namespace Disk
} // namespace N64

