/**
 * @file DDisk.cpp
 * @brief D Disk implementation.
 */
#include "DDisk.hpp"
#include <cstring>
#include <algorithm>
#include <cstdio>

namespace DiskDrive {

// Global variables definition
bool dd_write = false;
bool dd_reset_hold = false;
uint32_t dd_track_offset = 0;
uint32_t dd_zone = 0;
uint32_t dd_start_block = 0;
uint32_t dd_current = 0;

DiskController g_DiskController;

// ============================================================================
// DiskController Implementation
// ============================================================================

void DiskController::ExecuteCommand(DiskCommand command, uint32_t parameter) {
    // Reset transient status flags at start of new command
    status.command_complete = false;
    status.error = false;
    
    // Most commands require logic, some might set busy
    status.busy = true;

    switch (command) {
        case DiskCommand::Seek:
            // Parameter is typically the target track (or cylinder)
            if (SeekToTrack(parameter)) {
                status.seek_complete = true;
            } else {
                HandleError();
            }
            break;
            
        case DiskCommand::ReadSector:
            if (!status.motor_on) {
                HandleError();
                break;
            }
            current_mode = DiskMode::Reading;
            // Uses current_track/current_sector set by previous Seek commands
            if (ReadSector(current_track, current_sector)) {
               // Read success
            }
            break;
            
        case DiskCommand::WriteSector:
            if (!status.motor_on || status.write_protected) {
                HandleError();
                break;
            }
            current_mode = DiskMode::Writing;
            if (WriteSector(current_track, current_sector)) {
                // Write success
            }
            break;
            
        case DiskCommand::Reset:
            Reset();
            break;
            
        case DiskCommand::MotorOn:
            MotorOn();
            break;
            
        case DiskCommand::MotorOff:
            MotorOff();
            break;
            
        case DiskCommand::CheckDisk:
            // Just checks status, effectively
            UpdateStatus();
            break;
            
        case DiskCommand::GetStatus:
            // Return status (handled by caller reading status register)
            break;
            
        case DiskCommand::SetZone:
            SetZone(static_cast<DiskZone>(parameter));
            break;
            
        case DiskCommand::ReadBlock:
            if (!status.motor_on) {
                HandleError();
            } else {
                current_mode = DiskMode::Reading;
                ReadBlock(parameter);
            }
            break;
            
        case DiskCommand::WriteBlock:
            if (!status.motor_on || status.write_protected) {
                HandleError();
            } else {
                current_mode = DiskMode::Writing;
                WriteBlock(parameter);
            }
            break;
            
        default:
            HandleError();
            break;
    }
    
    status.busy = false;
    status.command_complete = true;
    
    // Revert mode to idle unless operation is ongoing (like DMA or continuous read)
    // For this simple implementation, we revert immediately
    current_mode = DiskMode::Idle;
}

void DiskController::Reset() {
    current_mode = DiskMode::Idle;
    current_zone = DiskZone::Zone0;
    
    // Reset status bits
    status = {};
    status.disk_present = true; // Assume disk is inserted for emulation
    status.motor_on = false;
    
    current_track = 0;
    current_sector = 0;
    current_block = 0;
    
    dma_start_address = 0;
    dma_length = 0;
    dma_active = false;
    
    std::memset(sector_buffer, 0, BUFFER_SIZE);
}

void DiskController::SetZone(DiskZone zone) {
    if (static_cast<uint8_t>(zone) < 16) {
        current_zone = zone;
    } 
}

bool DiskController::ReadSector(uint32_t track, uint32_t sector) {
    if (!IsValidTrackSector(current_zone, track, sector)) {
        HandleError();
        return false;
    }

    uint32_t offset = CalculatePhysicalOffset(current_zone, track, sector);

    if (disk_data && offset + BYTES_PER_SECTOR <= disk_data_size) {
        std::memcpy(sector_buffer, disk_data + offset, BYTES_PER_SECTOR);
    } else {
        // No disk image attached or offset out of range — zero-fill
        std::memset(sector_buffer, 0, BYTES_PER_SECTOR);
    }
    
    status.data_ready = true;
    return true;
}

bool DiskController::WriteSector(uint32_t track, uint32_t sector) {
    if (!IsValidTrackSector(current_zone, track, sector)) {
        HandleError();
        return false;
    }
    
    uint32_t offset = CalculatePhysicalOffset(current_zone, track, sector);

    if (disk_data_writable && offset + BYTES_PER_SECTOR <= disk_data_size) {
        std::memcpy(disk_data_writable + offset, sector_buffer, BYTES_PER_SECTOR);
    } else {
        HandleError();
        return false;
    }
    
    return true;
}

bool DiskController::ReadBlock(uint32_t block) {
    current_block = block;
    for (uint32_t s = 0; s < SECTORS_PER_BLOCK; ++s) {
        if (!ReadSector(current_track, s)) return false;
    }
    return true;
}

bool DiskController::WriteBlock(uint32_t block) {
    current_block = block;
    for (uint32_t s = 0; s < SECTORS_PER_BLOCK; ++s) {
        if (!WriteSector(current_track, s)) return false;
    }
    return true;
}

void DiskController::StartDMA(uint32_t address, uint32_t length, bool is_write) {
    dma_start_address = address;
    dma_length = length;
    dma_active = true;
    
    // In this implementation, we process DMA lazily or immediately
    ProcessDMA();
}

void DiskController::ProcessDMA() {
    if (!dma_active) return;
    
    // To Be Implemented: RDRAM access.
    // Simulation:
    // if (is_write) { memcpy(sector_buffer, RDRAM[dma_start_address], dma_length); }
    // else { memcpy(RDRAM[dma_start_address], sector_buffer, dma_length); }
    
    // For now, assume transfer completes instantly
    dma_active = false;
    dma_length = 0;
    status.command_complete = true;
}

bool DiskController::SeekToTrack(uint32_t track) {
    const auto& info = GetZoneInfo(current_zone);
    if (track >= info.tracks_in_zone) {
        return false;
    }
    current_track = track;
    return true;
}

bool DiskController::SeekToSector(uint32_t track, uint32_t sector) {
    if (!SeekToTrack(track)) return false;
    
    const auto& info = GetZoneInfo(current_zone);
    if (sector >= info.sectors_per_track) {
        return false;
    }
    current_sector = sector;
    return true;
}

// Helpers
const ZoneInfo& DiskController::GetZoneInfo(DiskZone zone) {
    return ZONE_TABLE[static_cast<uint8_t>(zone) & 0xF];
}

uint32_t DiskController::CalculatePhysicalOffset(DiskZone zone, uint32_t track, uint32_t sector) {
    return DiskGeometry::CalculateSectorOffset(zone, track, sector);
}

bool DiskController::IsValidTrackSector(DiskZone zone, uint32_t track, uint32_t sector) {
    if (static_cast<uint8_t>(zone) >= 16) return false;
    const auto& info = GetZoneInfo(zone);
    return (track < info.tracks_in_zone) && (sector < info.sectors_per_track);
}

// Internal Private Helpers
void DiskController::UpdateBufferPointers() {
    // Stub
}

bool DiskController::TransferSector(bool is_write) {
    if (is_write) return WriteSector(current_track, current_sector);
    return ReadSector(current_track, current_sector);
}

void DiskController::UpdateStatus() {
    // Logic to ensure status flags are consistent can go here
}

void DiskController::HandleError() {
    status.error = true;
    status.busy = false; // Stop if error
}

void DiskController::MotorOn() {
    status.motor_on = true;
    status.disk_present = true;
}

void DiskController::MotorOff() {
    status.motor_on = false;
}

void DiskController::ProcessGapSector() {
    // Stub
}

// ============================================================================
// Global Function Implementations
// ============================================================================

void DiskCommandExecute() {
    // Map global `dd_current` to command execution
    // Assuming `dd_current` holds the command ID
    g_DiskController.ExecuteCommand(static_cast<DiskCommand>(dd_current));
}

void DiskReset() {
    g_DiskController.Reset();
}

void DiskBMControl() {
    // Stub
}

void DiskGapSectorCheck() {
    // Stub
}

void DiskBMUpdate() {
   // Stub
}

bool DiskBMReadWrite(bool write) {
    if (write) {
        return g_DiskController.WriteSector(g_DiskController.GetCurrentTrack(), g_DiskController.GetCurrentSector());
    } else {
        return g_DiskController.ReadSector(g_DiskController.GetCurrentTrack(), g_DiskController.GetCurrentSector());
    }
}

void DiskDMACheck() {
    if (g_DiskController.IsDMAActive()) {
        g_DiskController.ProcessDMA();
    }
}

} // namespace DiskDrive
