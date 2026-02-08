#pragma once

#include <cstdint>
#include <cstddef>

namespace DiskDrive {

// Disk drive operating modes
enum class DiskMode : uint8_t {
    Idle,
    Reading,
    Writing,
    Seeking,
    Error
};

// Disk zones (DD uses 16 zones)
enum class DiskZone : uint8_t {
    Zone0 = 0, Zone1, Zone2, Zone3, Zone4, Zone5, Zone6, Zone7,
    Zone8, Zone9, Zone10, Zone11, Zone12, Zone13, Zone14, Zone15
};

// Disk commands
enum class DiskCommand : uint8_t {
    Seek = 0x00,
    ReadSector = 0x01,
    WriteSector = 0x02,
    Reset = 0x03,
    MotorOn = 0x04,
    MotorOff = 0x05,
    CheckDisk = 0x06,
    GetStatus = 0x07,
    SetZone = 0x08,
    ReadBlock = 0x09,
    WriteBlock = 0x0A
};

// Disk status flags
struct DiskStatus {
    bool busy : 1;
    bool error : 1;
    bool motor_on : 1;
    bool disk_present : 1;
    bool write_protected : 1;
    bool seek_complete : 1;
    bool data_ready : 1;
    bool command_complete : 1;
    
    explicit operator uint8_t() const {
        return (busy << 0) | (error << 1) | (motor_on << 2) | 
               (disk_present << 3) | (write_protected << 4) |
               (seek_complete << 5) | (data_ready << 6) | 
               (command_complete << 7);
    }
};

// Disk geometry constants
constexpr std::size_t SECTORS_PER_BLOCK = 85;
constexpr std::size_t BLOCKS_PER_TRACK = 2;
constexpr std::size_t BYTES_PER_SECTOR = 128;  // 64DD uses 128-byte sectors
constexpr std::size_t SECTORS_PER_TRACK = SECTORS_PER_BLOCK * BLOCKS_PER_TRACK;
constexpr std::size_t TRACKS_PER_DISK = 3175;  // Standard 64DD disk

// Zone configuration data
struct ZoneInfo {
    uint32_t sectors_per_track;
    uint32_t tracks_in_zone;
    uint32_t start_offset;  
};

// Zone table for 64DD (16 zones)
constexpr ZoneInfo ZONE_TABLE[16] = {
    // Sectors/Track, Tracks in Zone, Start Offset (bytes)
    {232, 158, 0x00000000},       // Zone 0
    {216, 158, 0x005F15E0},       // Zone 1
    {208, 149, 0x00B79D00},       // Zone 2
    {192, 149, 0x010801A0},       // Zone 3
    {176, 149, 0x01523720},       // Zone 4
    {160, 149, 0x01963D80},       // Zone 5
    {144, 149, 0x01D414C0},       // Zone 6
    {128, 114, 0x020BBCE0},       // Zone 7
    {216, 158, 0x023196E0},       // Zone 8
    {208, 158, 0x028A1E00},       // Zone 9
    {192, 149, 0x02DF5DC0},       // Zone 10
    {176, 149, 0x03299340},       // Zone 11
    {160, 149, 0x036D99A0},       // Zone 12
    {144, 149, 0x03AB70E0},       // Zone 13
    {128, 149, 0x03E31900},       // Zone 14
    {112, 114, 0x04149200}        // Zone 15
};

// Disk controller state
class DiskController {
private:
    DiskMode current_mode = DiskMode::Idle;
    DiskZone current_zone = DiskZone::Zone0;
    DiskStatus status{};
    
    uint32_t current_track = 0;
    uint32_t current_sector = 0;
    uint32_t current_block = 0;
    
    // DMA state
    uint32_t dma_start_address = 0;
    uint32_t dma_length = 0;
    bool dma_active = false;
    
    // Buffer management
    static constexpr std::size_t BUFFER_SIZE = 0x2000;  // 8KB buffer
    uint8_t sector_buffer[BUFFER_SIZE];
    
public:
    // Core disk operations
    void ExecuteCommand(DiskCommand command, uint32_t parameter = 0);
    void Reset();
    void SetZone(DiskZone zone);
    
    // Buffer management operations
    bool ReadSector(uint32_t track, uint32_t sector);
    bool WriteSector(uint32_t track, uint32_t sector);
    
    // Block operations (multiple sectors)
    bool ReadBlock(uint32_t block);
    bool WriteBlock(uint32_t block);
    
    // DMA operations
    void StartDMA(uint32_t address, uint32_t length, bool is_write);
    void ProcessDMA();
    bool IsDMAActive() const { return dma_active; }
    
    // Status and information
    DiskStatus GetStatus() const { return status; }
    DiskMode GetMode() const { return current_mode; }
    DiskZone GetZone() const { return current_zone; }
    
    // Position information
    uint32_t GetCurrentTrack() const { return current_track; }
    uint32_t GetCurrentSector() const { return current_sector; }
    uint32_t GetCurrentBlock() const { return current_block; }
    
    // Seek operations
    bool SeekToTrack(uint32_t track);
    bool SeekToSector(uint32_t track, uint32_t sector);
    
    // Helper methods
    static const ZoneInfo& GetZoneInfo(DiskZone zone);
    static uint32_t CalculatePhysicalOffset(DiskZone zone, uint32_t track, uint32_t sector);
    static bool IsValidTrackSector(DiskZone zone, uint32_t track, uint32_t sector);
    
private:
    // Internal helper methods
    void UpdateBufferPointers();
    bool TransferSector(bool is_write);
    void UpdateStatus();
    void HandleError();
    
    // Motor control
    void MotorOn();
    void MotorOff();
    
    // Gap sector handling (for copy protection)
    void ProcessGapSector();
};

// Global disk controller instance (optional - could be managed elsewhere)
extern DiskController g_DiskController;


void DiskCommandExecute();
void DiskReset();
void DiskBMControl();
void DiskGapSectorCheck();
void DiskBMUpdate();
bool DiskBMReadWrite(bool write);
void DiskDMACheck();


extern bool dd_write;
extern bool dd_reset_hold;
extern uint32_t dd_track_offset;
extern uint32_t dd_zone;
extern uint32_t dd_start_block;
extern uint32_t dd_current;


namespace DiskGeometry {
    constexpr uint32_t GetZoneSectorSize(DiskZone zone) {
        return ZONE_TABLE[static_cast<uint8_t>(zone)].sectors_per_track;
    }
    
    constexpr uint32_t GetZoneTrackSize(DiskZone zone) {
        return ZONE_TABLE[static_cast<uint8_t>(zone)].tracks_in_zone;
    }
    
    constexpr uint32_t GetZoneStartOffset(DiskZone zone) {
        return ZONE_TABLE[static_cast<uint8_t>(zone)].start_offset;
    }
    
    constexpr uint32_t CalculateSectorOffset(DiskZone zone, uint32_t track, uint32_t sector) {
        return GetZoneStartOffset(zone) + 
               (track * GetZoneSectorSize(zone) * BYTES_PER_SECTOR) +
               (sector * BYTES_PER_SECTOR);
    }
    
    constexpr uint32_t TracksToCylinder(uint32_t track) {
        return track / 2;  // 64DD has 2 tracks per cylinder
    }
    
    constexpr uint32_t CylinderToTrack(uint32_t cylinder, uint32_t head) {
        return (cylinder * 2) + (head & 1);
    }
}

} 


//using DiskDrive::SECTORS_PER_BLOCK;
//using DiskDrive::BLOCKS_PER_TRACK;
//using DiskDrive::g_DiskController;