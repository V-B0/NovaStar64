/**
 * @file Eeprom.cpp
 * @brief N64 EEPROM save implementation with RTC support
 * 
 * Based on Project64's implementation with modern C++ improvements:
 * - Better error handling with exceptions
 * - Proper RAII for file management
 * - Structured logging using Log namespace
 * - Cleaner code organization
 * - Type-safe enums
 */

#include "Eeprom.hpp"
#include "../MainSYS.hpp"
#include "../Types.hpp"
#include "../../common/log.hpp"
#include "../../common/path.hpp"
#include <cstring>
#include <ctime>
#include <stdexcept>

// EEPROM command codes
enum class EepromCmd : uint8_t {
    Status      = 0x00,  // Check EEPROM status
    Read        = 0x04,  // Read from EEPROM
    Write       = 0x05,  // Write to EEPROM
    RTCStatus   = 0x06,  // RTC status query
    RTCRead     = 0x07,  // Read RTC block
    RTCWrite    = 0x08   // Write RTC (unimplemented)
};

// Convert integer to BCD (Binary-Coded Decimal)
static inline uint8_t IntToBCD(int32_t value)
{
    value %= 100;
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

// CEeprom constructor
CEeprom::CEeprom(bool ReadOnly)
    : m_ReadOnly(ReadOnly)
{
    // Initialize EEPROM to 0xFF (unprogrammed state)
    std::memset(m_EEPROM, 0xFF, sizeof(m_EEPROM));
    
    Log::debugf("CEeprom initialized (ReadOnly: %s)", ReadOnly ? "true" : "false");
}

// CEeprom destructor
CEeprom::~CEeprom()
{
    if (m_File.is_open())
    {
        m_File.close();
        Log::debug("EEPROM file closed");
    }
}

// Main EEPROM command processor
void CEeprom::EepromCommand(uint8_t* Command)
{
    if (!Command)
    {
        Log::error("EEPROM command called with null pointer");
        return;
    }

    // Auto-detect save type on first EEPROM access
    extern CN64System* g_System;
    if (g_System && g_System->m_SaveUsing == N64Types::SaveChipType::Auto)
    {
        g_System->m_SaveUsing = N64Types::SaveChipType::Eeprom4K;
        Log::info("Auto-detected save type: EEPROM 4K");
    }

    const EepromCmd cmd = static_cast<EepromCmd>(Command[2]);

    switch (cmd)
    {
    case EepromCmd::Status:
        HandleStatusCommand(Command);
        break;

    case EepromCmd::Read:
        HandleReadCommand(Command);
        break;

    case EepromCmd::Write:
        HandleWriteCommand(Command);
        break;

    case EepromCmd::RTCStatus:
        HandleRTCStatusCommand(Command);
        break;

    case EepromCmd::RTCRead:
        HandleRTCReadCommand(Command);
        break;

    case EepromCmd::RTCWrite:
        HandleRTCWriteCommand(Command);
        break;

    default:
        Log::warningf("Unknown EEPROM command: 0x%02X", Command[2]);
        ProcessingError(Command);
        break;
    }
}

// Handle EEPROM status check command
void CEeprom::HandleStatusCommand(uint8_t* Command)
{
    extern CN64System* g_System;
    // Verify we're using EEPROM save type
    if (g_System && 
        g_System->m_SaveUsing != N64Types::SaveChipType::Eeprom4K && 
        g_System->m_SaveUsing != N64Types::SaveChipType::Eeprom16K)
    {
        Command[1] |= 0x80;  // Set error bit
        return;
    }

    if (Command[1] != 3)
    {
        Command[1] |= 0x40;  // Invalid parameter
        if ((Command[1] & 3) > 0)
        {
            Command[3] = 0x00;
        }
        if ((Command[1] & 3) > 1)
        {
            // Return EEPROM size indicator
            Command[4] = (g_System->m_SaveUsing == N64Types::SaveChipType::Eeprom4K) ? 0x80 : 0xC0;
        }
        if ((Command[1] & 3) > 2)
        {
            Command[5] = 0x00;
        }
    }
    else
    {
        Command[3] = 0x00;
        Command[4] = (g_System->m_SaveUsing == N64Types::SaveChipType::Eeprom4K) ? 0x80 : 0xC0;
        Command[5] = 0x00;
    }
}

// Handle EEPROM read command
void CEeprom::HandleReadCommand(uint8_t* Command)
{
    // Validate command format
    if (Command[0] != 2)
    {
        Log::warningf("EEPROM read command: unexpected byte 0 = %d (expected 2)", Command[0]);
        ProcessingError(Command);
    }
    if (Command[1] != 8)
    {
        Log::warningf("EEPROM read command: unexpected byte 1 = %d (expected 8)", Command[1]);
        ProcessingError(Command);
    }

    const int32_t line = Command[3];
    ReadFrom(&Command[4], line);
    
    Log::debugf("EEPROM read: line %d", line);
}

// Handle EEPROM write command
void CEeprom::HandleWriteCommand(uint8_t* Command)
{
    // Validate command format
    if (Command[0] != 10)
    {
        Log::warningf("EEPROM write command: unexpected byte 0 = %d (expected 10)", Command[0]);
        ProcessingError(Command);
    }
    if (Command[1] != 1)
    {
        Log::warningf("EEPROM write command: unexpected byte 1 = %d (expected 1)", Command[1]);
        ProcessingError(Command);
    }

    const int32_t line = Command[3];
    WriteTo(&Command[4], line);
    
    Log::debugf("EEPROM write: line %d", line);
}

// Handle RTC status query
void CEeprom::HandleRTCStatusCommand(uint8_t* Command)
{
    // RTC is present
    Command[3] = 0x00;
    Command[4] = 0x10;  // RTC present flag
    Command[5] = 0x00;
    
    Log::debug("RTC status query");
}

// Handle RTC read command
void CEeprom::HandleRTCReadCommand(uint8_t* Command)
{
    const uint8_t block = Command[3];
    
    switch (block)
    {
    case 0: // Block number query
        Command[4] = 0x00;
        Command[5] = 0x02;  // 2 blocks available
        Command[12] = 0x00;
        Log::debug("RTC read: block number query");
        break;

    case 1: // Read RTC block 1 (unimplemented)
        Log::warning("RTC read block 1: not implemented");
        break;

    case 2: // Read RTC time
    {
        std::time_t currentTime = std::time(nullptr);
        std::tm* localTime = std::localtime(&currentTime);
        
        if (localTime)
        {
            Command[4] = IntToBCD(localTime->tm_sec);
            Command[5] = IntToBCD(localTime->tm_min);
            Command[6] = 0x80 + IntToBCD(localTime->tm_hour);  // 0x80 = 24-hour format
            Command[7] = IntToBCD(localTime->tm_mday);
            Command[8] = IntToBCD(localTime->tm_wday);
            Command[9] = IntToBCD(localTime->tm_mon + 1);  // tm_mon is 0-11
            Command[10] = IntToBCD(localTime->tm_year % 100);  // Last 2 digits
            Command[11] = IntToBCD(localTime->tm_year / 100);  // Century
            Command[12] = 0x00;  // Status byte
            
            Log::debugf("RTC read: %04d-%02d-%02d %02d:%02d:%02d",
                       localTime->tm_year + 1900,
                       localTime->tm_mon + 1,
                       localTime->tm_mday,
                       localTime->tm_hour,
                       localTime->tm_min,
                       localTime->tm_sec);
        }
        else
        {
            Log::error("Failed to get local time for RTC");
            std::memset(&Command[4], 0, 9);
        }
        break;
    }

    default:
        Log::warningf("RTC read: unknown block %d", block);
        break;
    }
}

// Handle RTC write command (unimplemented)
void CEeprom::HandleRTCWriteCommand(uint8_t* Command)
{
    Log::warning("RTC write command: not implemented");
    // RTC writes are not commonly used and not implemented
    (void)Command;
}

// Load EEPROM data from file
void CEeprom::LoadEeprom()
{
    extern CN64System* g_System;
    // Initialize to unprogrammed state
    std::memset(m_EEPROM, 0xFF, sizeof(m_EEPROM));

    if (!g_System)
    {
        Log::error("Cannot load EEPROM: no active system");
        return;
    }

    // Build save file path
    // Format: <SaveDir>/<GameName>.eep
    std::string savePath = "Saves";  // TODO: Get from settings
    std::string gameName = "game";    // TODO: Get from ROM
    std::string fileName = gameName + ".eep";
    std::string fullPath = savePath + "/" + fileName;

    // TODO: Create save directory if it doesn't exist
    // TODO: Handle unique save directories per game

    // Try to open the file
    FileSystem::OpenMode openMode = m_ReadOnly ? 
        FileSystem::OpenMode::Read : 
        (FileSystem::OpenMode::Read | FileSystem::OpenMode::Write);

    if (!m_File.is_open())
    {
        try {
            m_File.open(fullPath, openMode);
        } catch (...) {
            // File doesn't exist yet - create it
            if (!m_ReadOnly)
            {
                try {
                    m_File.open(fullPath, 
                               FileSystem::OpenMode::Read | 
                               FileSystem::OpenMode::Write | 
                               FileSystem::OpenMode::Create);
                    // Write initial unprogrammed state
                    m_File.write(m_EEPROM, sizeof(m_EEPROM));
                    m_File.seek(0, FileSystem::SeekOrigin::Begin);
                    Log::infof("Created new EEPROM save file: %s", fullPath.c_str());
                } catch (...) {
                    Log::errorf("Failed to create EEPROM file: %s", fullPath.c_str());
                    return;
                }
            }
            else
            {
                Log::warningf("EEPROM file not found (read-only mode): %s", fullPath.c_str());
                return;
            }
        }
    }
    
    if (m_File.is_open())
    {
        // Read existing EEPROM data
        size_t bytesRead = m_File.read(m_EEPROM, sizeof(m_EEPROM));
        
        if (bytesRead > 0)
        {
            Log::infof("Loaded EEPROM from: %s (%zu bytes)", fullPath.c_str(), bytesRead);
        }
        else
        {
            Log::warningf("EEPROM file is empty: %s", fullPath.c_str());
        }
    }
}

// Read 8 bytes from EEPROM at specified line
void CEeprom::ReadFrom(uint8_t* Buffer, int32_t line)
{
    if (!Buffer)
    {
        Log::error("ReadFrom called with null buffer");
        return;
    }

    // Ensure EEPROM is loaded
    if (!m_File.is_open())
    {
        LoadEeprom();
    }

    // Validate line number
    const int32_t maxLines = sizeof(m_EEPROM) / 8;
    if (line < 0 || line >= maxLines)
    {
        Log::errorf("EEPROM read: invalid line %d (max %d)", line, maxLines - 1);
        std::memset(Buffer, 0xFF, 8);
        return;
    }

    // Copy 8 bytes from EEPROM
    const int32_t offset = line * 8;
    std::memcpy(Buffer, &m_EEPROM[offset], 8);
}

// Write 8 bytes to EEPROM at specified line
void CEeprom::WriteTo(uint8_t* Buffer, int32_t line)
{
    if (!Buffer)
    {
        Log::error("WriteTo called with null buffer");
        return;
    }

    // Ensure EEPROM is loaded
    if (!m_File.is_open())
    {
        LoadEeprom();
    }

    // Validate line number
    const int32_t maxLines = sizeof(m_EEPROM) / 8;
    if (line < 0 || line >= maxLines)
    {
        Log::errorf("EEPROM write: invalid line %d (max %d)", line, maxLines - 1);
        return;
    }

    // Update EEPROM memory
    const int32_t offset = line * 8;
    std::memcpy(&m_EEPROM[offset], Buffer, 8);

    // Write to file if not read-only
    if (!m_ReadOnly && m_File.is_open())
    {
        m_File.seek(offset, FileSystem::SeekOrigin::Begin);
        size_t bytesWritten = m_File.write(Buffer, 8);
        
        if (bytesWritten != 8)
        {
            Log::errorf("EEPROM write failed: wrote %zu/8 bytes at line %d", bytesWritten, line);
        }
    }
}

// Handle processing errors
void CEeprom::ProcessingError(uint8_t* Command)
{
    if (Command)
    {
        Log::warningf("EEPROM command processing error: [%02X %02X %02X %02X]",
                     Command[0], Command[1], Command[2], Command[3]);
    }
    // Note: Project64 displays error dialog here, but we just log
}
