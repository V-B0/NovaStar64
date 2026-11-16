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

static constexpr uint32_t SECTORS_PER_BLOCK = 85;
static constexpr uint32_t SYSTEM_LBAS = 24;

// Sector sizes by zone (physical zone 0-7)
static constexpr uint32_t SECTORSIZE_P[8] = {232, 216, 208, 192, 176, 160, 144, 128};

inline uint32_t BLOCKSIZE(uint8_t zone) { return SECTORSIZE_P[zone & 7] * SECTORS_PER_BLOCK; }

inline uint32_t TRACKSIZE(uint8_t zone) { return BLOCKSIZE(zone) * 2; }

// --- D64Data structure for D64 format handling ---

struct N64Disk::D64Data {
    bool initialized = false;
    uint16_t romLbaEnd = 0;
    uint16_t ramLbaStart = 0;
    uint16_t ramLbaEnd = 0;
};

// --- DiskFile class for file I/O ---

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

    void SeekToBegin() { Seek(0, std::ios::beg); }

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

// --- N64Disk Implementation ---

N64Disk::N64Disk() :
    m_Image(std::make_unique<DiskImage>()), m_D64Data(std::make_unique<D64Data>()),
    m_DiskFile(std::make_unique<DiskFile>()) {}

N64Disk::~N64Disk() { UnloadDisk(); }

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

const DiskImage& N64Disk::GetImage() const {
    if (!m_Image || !m_Image->IsLoaded()) {
        throw DiskError(DiskError::Type::ReadError, "No disk loaded");
    }
    return *m_Image;
}

DiskFormat N64Disk::GetFormat() const { return m_Image ? m_Image->m_Format : DiskFormat::Unknown; }

size_t N64Disk::GetSize() const { return m_Image ? m_Image->m_Data.size() : 0; }

DataView N64Disk::GetIdentifier() const {
    if (!IsLoaded()) {
        return DataView{nullptr, 0};
    }
    return DataView{m_Image->m_Data.data() + m_IdentifierOffset, 0x20};
}

DataView N64Disk::GetSystemArea() const {
    if (!IsLoaded()) {
        return DataView{nullptr, 0};
    }
    return DataView{m_Image->m_Data.data() + m_SystemAreaOffset, BLOCKSIZE(0) * SYSTEM_LBAS};
}

// --- Private Helper Methods ---

void N64Disk::ParseHeader() {
    if (!IsLoaded()) {
        return;
    }

    auto sysData = GetSystemArea();
    if (sysData.empty()) {
        return;
    }

    uint8_t diskType = sysData.data[5] & 0x0F;
    if (diskType <= 6) {
        m_Image->m_Type = DiskType::Retail;
    }
}

void N64Disk::AllocateImage(size_t size) {
    if (!m_Image) {
        m_Image = std::make_unique<DiskImage>();
    }
    m_Image->m_Data.resize(size);
}

// --- DiskUtils Implementation ---

namespace DiskUtils {} // namespace DiskUtils

} // namespace Disk
} // namespace N64
