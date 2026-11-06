/**
 * @file SRam.cpp
 * @brief S Ram implementation.
 */
#include "SRam.hpp"
#include "../../common/log.hpp"
#include "../../common/MemoryManagment.hpp"
#include "../MainSYS.hpp"
#include "../N64Rom.hpp"
#include <cstring>
#include <algorithm>
#include <filesystem>

// SRAM constants
static constexpr uint32_t SRAM_SIZE = 32 * 1024; // 32 KB

// External globals
extern CN64System* g_System;
extern N64::Rom::N64Rom* g_Rom;

// Constructor - Initialize SRAM
CSram::CSram(bool ReadOnly)
    : m_ReadOnly(ReadOnly)
{
    Log::debugf("CSram constructor (ReadOnly: %s)", ReadOnly ? "true" : "false");
    LoadSram();
}

// Destructor - Close file if open
CSram::~CSram()
{
    Log::debug("CSram destructor");
    
    if (m_File.is_open())
    {
        Log::debug("CSram: Closing SRAM file");
        m_File.close();
    }
}

// DMA from SRAM to emulated memory
void CSram::DmaFromSram(uint8_t* dest, int32_t StartOffset, uint32_t len)
{
    if (!dest)
    {
        Log::debugf("CSram::DmaFromSram - NULL destination pointer");
        return;
    }

    if (StartOffset < 0)
    {
        Log::debugf("CSram::DmaFromSram - Negative offset: %d", StartOffset);
        return;
    }

    if (len == 0)
    {
        Log::debugf("CSram::DmaFromSram - Zero length requested");
        return;
    }

    // Clamp to SRAM size
    uint32_t offset = static_cast<uint32_t>(StartOffset);
    if (offset >= SRAM_SIZE)
    {
        Log::debugf("CSram::DmaFromSram - Offset %u beyond SRAM size %u", offset, SRAM_SIZE);
        std::memset(dest, 0x00, len);
        return;
    }

    // Adjust length if it would exceed SRAM bounds
    uint32_t availableBytes = SRAM_SIZE - offset;
    uint32_t readLen = std::min(len, availableBytes);
    
    Log::debugf("CSram::DmaFromSram - Reading %u bytes from offset %u", readLen, offset);

    if (!m_File.is_open())
    {
        Log::debugf("CSram::DmaFromSram - File not open, returning zeros");
        std::memset(dest, 0x00, len);
        return;
    }

    // Seek to offset and read
    if (!m_File.seek(offset, FileSystem::SeekOrigin::Begin))
    {
        Log::debugf("CSram::DmaFromSram - Seek failed to offset %u", offset);
        std::memset(dest, 0x00, len);
        return;
    }

    size_t bytesRead = m_File.read(dest, readLen);
    if (bytesRead != readLen)
    {
        Log::debugf("CSram::DmaFromSram - Read %zu bytes, expected %u", bytesRead, readLen);
        
        // Fill remainder with zeros if short read
        if (bytesRead < readLen)
        {
            std::memset(dest + bytesRead, 0x00, readLen - bytesRead);
        }
    }

    // If we clamped the length, fill the rest with zeros
    if (readLen < len)
    {
        Log::debugf("CSram::DmaFromSram - Filling %u remaining bytes with zeros", len - readLen);
        std::memset(dest + readLen, 0x00, len - readLen);
    }
}

// DMA to SRAM from emulated memory
void CSram::DmaToSram(uint8_t* Source, int32_t StartOffset, uint32_t len)
{
    if (!Source)
    {
        Log::debugf("CSram::DmaToSram - NULL source pointer");
        return;
    }

    if (m_ReadOnly)
    {
        Log::debugf("CSram::DmaToSram - Write attempted in read-only mode");
        return;
    }

    if (StartOffset < 0)
    {
        Log::debugf("CSram::DmaToSram - Negative offset: %d", StartOffset);
        return;
    }

    if (len == 0)
    {
        Log::debugf("CSram::DmaToSram - Zero length requested");
        return;
    }

    // Clamp to SRAM size
    uint32_t offset = static_cast<uint32_t>(StartOffset);
    if (offset >= SRAM_SIZE)
    {
        Log::debugf("CSram::DmaToSram - Offset %u beyond SRAM size %u", offset, SRAM_SIZE);
        return;
    }

    // Adjust length if it would exceed SRAM bounds
    uint32_t availableBytes = SRAM_SIZE - offset;
    uint32_t writeLen = std::min(len, availableBytes);
    
    Log::debugf("CSram::DmaToSram - Writing %u bytes to offset %u", writeLen, offset);

    if (!m_File.is_open())
    {
        Log::debugf("CSram::DmaToSram - File not open, write failed");
        return;
    }

    // Seek to offset and write
    if (!m_File.seek(offset, FileSystem::SeekOrigin::Begin))
    {
        Log::debugf("CSram::DmaToSram - Seek failed to offset %u", offset);
        return;
    }

    size_t bytesWritten = m_File.write(Source, writeLen);
    if (bytesWritten != writeLen)
    {
        Log::debugf("CSram::DmaToSram - Wrote %zu bytes, expected %u", bytesWritten, writeLen);
    }

    // Flush to ensure data is written to disk
    m_File.flush();

    if (writeLen < len)
    {
        Log::debugf("CSram::DmaToSram - Clamped write, %u bytes beyond SRAM boundary not written", len - writeLen);
    }
}

// Load or create SRAM file
bool CSram::LoadSram()
{
    Log::debug("CSram::LoadSram - Loading SRAM file");

    // Get save file path
    if (!g_System)
    {
        Log::debugf("CSram::LoadSram - No system instance");
        return false;
    }

    std::string savePath = "Saves";
    
    // Derive filename from ROM name
    std::string romName = "game";
    if (g_Rom && g_Rom->IsLoaded())
    {
        romName = g_Rom->GetImage().GetRomName();
        // Sanitize filename - remove invalid characters
        for (char& c : romName) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' || 
                c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                c = '_';
        }
        // Trim trailing spaces
        while (!romName.empty() && romName.back() == ' ')
            romName.pop_back();
    }
    std::string fileName = romName + ".sra";
    
    // Ensure save directory exists
    std::filesystem::create_directories(savePath);
    
    std::string fullPath = (std::filesystem::path(savePath) / fileName).string();

    Log::debugf("CSram::LoadSram - Save path: %s", fullPath.c_str());

    // Try to open existing file first
    FileSystem::OpenMode openMode = FileSystem::OpenMode::Read | FileSystem::OpenMode::Write;
    
    try {
        m_File.open(fullPath, openMode);
        
        // Check if file is correct size
        size_t fileSize = m_File.size();
        
        if (fileSize == SRAM_SIZE)
        {
            Log::debugf("CSram::LoadSram - Loaded existing SRAM file (%u bytes)", SRAM_SIZE);
            return true;
        }
        else
        {
            Log::debugf("CSram::LoadSram - SRAM file wrong size (%zu bytes, expected %u), recreating", 
                       fileSize, SRAM_SIZE);
            m_File.close();
        }
    } catch (...) {
        // File doesn't exist, will create below
    }

    // Create new SRAM file
    Log::debugf("CSram::LoadSram - Creating new SRAM file");
    
    openMode = FileSystem::OpenMode::Read | FileSystem::OpenMode::Write | FileSystem::OpenMode::Create;
    
    try {
        m_File.open(fullPath, openMode);
    } catch (...) {
        Log::debugf("CSram::LoadSram - Failed to create SRAM file: %s", fullPath.c_str());
        return false;
    }

    // Initialize SRAM to 0x00 (unlike EEPROM/FlashRAM which use 0xFF)
    // SRAM typically initializes to 0x00
    constexpr size_t BUFFER_SIZE = 1024;
    uint8_t buffer[BUFFER_SIZE];
    std::memset(buffer, 0x00, BUFFER_SIZE);

    size_t bytesWritten = 0;
    while (bytesWritten < SRAM_SIZE)
    {
        size_t toWrite = std::min(BUFFER_SIZE, SRAM_SIZE - bytesWritten);
        size_t written = m_File.write(buffer, toWrite);
        
        if (written != toWrite)
        {
            Log::debugf("CSram::LoadSram - Write error at offset %zu", bytesWritten);
            m_File.close();
            return false;
        }
        
        bytesWritten += written;
    }

    m_File.flush();
    
    // Seek back to beginning
    (void)m_File.seek(0, FileSystem::SeekOrigin::Begin);
    
    Log::debugf("CSram::LoadSram - Created new SRAM file (%u bytes initialized to 0x00)", SRAM_SIZE);
    return true;
}
