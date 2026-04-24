/**
 * @file FRam.cpp
 * @brief N64 Flash RAM save implementation
 * 
 * Based on Project64's implementation with modern C++ improvements:
 * - Better error handling with detailed logging
 * - Proper RAII for file and memory management
 * - Type-safe mode handling
 * - Cleaner code organization
 * - Better byte-swapping logic
 */

#include "FRam.hpp"
#include "../MainSYS.hpp"
#include "../N64Rom.hpp"
#include "../Types.hpp"
#include "../../common/log.hpp"
#include "../../common/path.hpp"
#include <cstring>
#include <algorithm>
#include <filesystem>

extern N64::Rom::N64Rom* g_Rom;

// Flash RAM constants
constexpr size_t FLASHRAM_SIZE  = 128 * 1024;  // 128 KB total chip
constexpr size_t BLOCK_SIZE     = 128;         // 128 bytes per page (program unit)
constexpr size_t SECTOR_SIZE    = 16 * 1024;   // 16 KB sector (erase unit) — Macronix MX29L1100

CFlashRam::CFlashRam(bool ReadOnly)
    : m_FlashRamPointer(nullptr)
    , m_FlashFlag(FLASHRAM_MODE_NOPES)
    , m_FlashStatus(0)
    , m_FlashRAM_Offset(0)
    , m_ReadOnly(ReadOnly)
{
    Log::debugf("CFlashRam initialized (ReadOnly: %s)", ReadOnly ? "true" : "false");
}

CFlashRam::~CFlashRam()
{
    if (m_File.is_open())
    {
        m_File.close();
        Log::debug("Flash RAM file closed");
    }
    
    m_FlashRamPointer = nullptr;
}

// DMA from Flash RAM to RDRAM
void CFlashRam::DmaFromFlashram(uint8_t* dest, int32_t StartOffset, int32_t len)
{
    if (!dest)
    {
        Log::error("DmaFromFlashram: null destination pointer");
        return;
    }

    // BUG-FIX: any code path that returns early without writing to `dest`
    // leaves whatever was previously in RDRAM. MM's libultra interprets
    // that as the saved game state, and a zeroed/garbage Save Context
    // means "max hearts = 0" → Link instant-dies after the first
    // cutscene, plus saving appears to fail. Pre-fill the destination
    // with 0xFF (FlashRAM erased state) up front so every error path is
    // safe.
    if (len > 0) std::memset(dest, 0xFF, static_cast<size_t>(len));

    // Off-stack buffer (was 64 KB on the stack — fragile under deep PI
    // call stacks). Single-threaded access, so static is fine.
    static uint8_t FlipBuffer[0x10000];

    switch (m_FlashFlag)
    {
    case FLASHRAM_MODE_READ:
    {
        if (!m_File.is_open() && !LoadFlashram())
        {
            Log::error("DmaFromFlashram: failed to load Flash RAM");
            return;
        }

        if (len > static_cast<int32_t>(sizeof(FlipBuffer)))
        {
            Log::errorf("DmaFromFlashram: buffer overflow attempt (len: %d, max: %zu)", 
                       len, sizeof(FlipBuffer));
            len = sizeof(FlipBuffer);
        }

        if ((len & 3) != 0)
        {
            Log::warningf("DmaFromFlashram: unaligned length (%d bytes) — returning 0xFF", len);
            // dest is already pre-filled with 0xFF above.
            return;
        }

        std::memset(FlipBuffer, 0xFF, sizeof(FlipBuffer));

        // N64 FlashRAM hardware: READ and WRITE address the device very
        // differently and we MUST mirror that asymmetry here.
        //   * READ  (osFlashReadArray): the game issues PI DMA with
        //     deviceAddr = page_num * 128, so cart_addr is
        //     0x08000000 + (page_num * 128). The byte offset is encoded
        //     in the cart address itself, NOT in m_FlashRAM_Offset (the
        //     read-mode 0xF0 command does not touch the offset register).
        //     The caller already extracted this with `cart_addr & 0x1FFFF`
        //     and passes it as StartOffset.
        //   * WRITE (osFlashWriteArray): the game writes 0x4B|page to
        //     program m_FlashRAM_Offset, then DMAs to the fixed data
        //     port 0x08000000. DmaToFlashram seeks by m_FlashRAM_Offset.
        //
        // The previous "<<1" PJ64 helper shifted StartOffset assuming
        // dword units; that was the bug. Using m_FlashRAM_Offset here
        // (a previous attempt) is also wrong because it stays stale or
        // zero across pure-read sequences and softcrashes new-file
        // creation when MM reads back a freshly-written page.
        m_File.seek(StartOffset, FileSystem::SeekOrigin::Begin);
        size_t bytesRead = m_File.read(FlipBuffer, len);

        // Fill any bytes the read could not satisfy (short file / past EOF)
        // with 0xFF (FlashRAM erased state). The buffer was pre-filled with
        // 0xFF above, so this is mostly defensive.
        for (size_t count = bytesRead; count < static_cast<size_t>(len); count++)
        {
            FlipBuffer[count] = 0xFF;
        }

        // Byte-swap from file format to N64 format (big-endian 32-bit words)
        for (int32_t count = 0; count < len; count += 4)
        {
            *(uint32_t*)(dest + count) = *(uint32_t*)&FlipBuffer[count];
        }

        Log::infof("[FlashRAM] read %zu bytes from offset 0x%X", bytesRead, StartOffset);
        break;
    }

    case FLASHRAM_MODE_STATUS:
    {
        if (StartOffset != 0 && len != 8)
        {
            Log::warningf("DmaFromFlashram: unexpected STATUS read (offset: %d, len: %d)", 
                         StartOffset, len);
        }

        // Return status in proper byte order
        *((uint32_t*)&dest[0]) = static_cast<uint32_t>(m_FlashStatus >> 32);
        *((uint32_t*)&dest[4]) = static_cast<uint32_t>(m_FlashStatus & 0xFFFFFFFF);
        
        Log::debugf("DmaFromFlashram: read status 0x%016llX", m_FlashStatus);
        break;
    }

    default:
        Log::warningf("DmaFromFlashram: invalid mode (%d)", m_FlashFlag);
        break;
    }
}

// DMA from RDRAM to Flash RAM
void CFlashRam::DmaToFlashram(uint8_t* Source, int32_t StartOffset, int32_t len)
{
    if (!Source)
    {
        Log::error("DmaToFlashram: null source pointer");
        return;
    }

    switch (m_FlashFlag)
    {
    case FLASHRAM_MODE_WRITE:
    {
        if (m_ReadOnly)
        {
            Log::warning("DmaToFlashram: attempted write in read-only mode");
            return;
        }

        // Pre-fill with 0xFF (erased state). Real N64 FlashRAM is
        // page-oriented: every write programs a full 128-byte page, and
        // any cells not driven by the DMA stay in the erased (0xFF) state
        // they were left in by the preceding 0x78/0xD2 erase pass. The
        // previous memset(0) here would corrupt those cells to 0x00 and
        // break MM new-file creation (which relies on the trailing tail
        // of partially-filled pages reading back as 0xFF after save).
        std::memset(m_PageBuffer, 0xFF, sizeof(m_PageBuffer));

        // Copy from source (already in N64 byte order)
        std::memcpy(m_PageBuffer, Source, std::min(static_cast<size_t>(len), sizeof(m_PageBuffer)));

        Log::infof("[FlashRAM] DMA to page buffer (len %d)", len);
        break;
    }

    default:
        Log::warningf("DmaToFlashram: invalid mode (%d)", m_FlashFlag);
        break;
    }
}

// Read Flash RAM status register
uint32_t CFlashRam::ReadFromFlashStatus(uint32_t PAddr)
{
    switch (PAddr)
    {
    case 0x08000000:
        return static_cast<uint32_t>(m_FlashStatus >> 32);
    
    default:
        Log::warningf("ReadFromFlashStatus: unexpected address 0x%08X", PAddr);
        break;
    }
    
    return static_cast<uint32_t>(m_FlashStatus >> 32);
}

// Write Flash RAM command register
void CFlashRam::WriteToFlashCommand(uint32_t Value)
{
    Log::infof("[FlashRAM] command 0x%08X (mode=%d, off=0x%X)", Value, m_FlashFlag, m_FlashRAM_Offset);
    switch (Value & 0xFF000000)
    {
    case 0xD2000000:  // Execute command
        switch (m_FlashFlag)
        {
        case FLASHRAM_MODE_NOPES:
            break;
            
        case FLASHRAM_MODE_READ:
            break;
            
        case FLASHRAM_MODE_STATUS:
            break;
            
        case FLASHRAM_MODE_ERASE:
        {
            // FlashRAM erase command erases a 16KB sector (128 pages of 128 bytes).
            std::vector<uint8_t> EraseSector(SECTOR_SIZE, 0xFF);

            if (!m_File.is_open() && !LoadFlashram())
            {
                Log::error("WriteToFlashCommand: failed to load Flash RAM for erase");
                return;
            }

            if (!m_ReadOnly)
            {
                // Align to sector boundary (16KB)
                uint32_t sectorOffset = m_FlashRAM_Offset & ~(SECTOR_SIZE - 1);
                m_File.seek(sectorOffset, FileSystem::SeekOrigin::Begin);
                m_File.write(EraseSector.data(), SECTOR_SIZE);
                m_File.flush();
                Log::debugf("Flash RAM erase: 16KB sector at offset 0x%X", sectorOffset);
            }
            // Operation-complete status (matches PJ64/Mupen64Plus magic
            // constants) so the game's status poll after 0xD2 sees
            // "erase done" instead of staying in the prior mode.
            m_FlashStatus = 0x1111800800C2001E;
            break;
        }

        case FLASHRAM_MODE_WRITE:
            // Write is handled by DmaToFlashram
            if (!m_File.is_open() && !LoadFlashram())
            {
                Log::error("WriteToFlashCommand: failed to load Flash RAM for write");
                return;
            }
            // Operation-complete status for write so libultra's status
            // poll after 0xD2 sees "write done" rather than the read or
            // erase status that was set previously.
            m_FlashStatus = 0x1111800400C2001E;
            Log::debugf("Flash RAM write complete: offset 0x%X", m_FlashRAM_Offset);
            break;
            
        default:
            Log::errorf("WriteToFlashCommand: unknown flash flag %d (command: 0x%08X)", 
                       m_FlashFlag, Value);
        }
        m_FlashFlag = FLASHRAM_MODE_NOPES;
        break;

    case 0xE1000000:  // Set status mode
        // BUG-FIX: do NOT clobber m_FlashStatus here. Real hardware
        // preserves the status word across mode switches so the game
        // can poll the result of a previous 0xD2 (write/erase complete)
        // by issuing 0xE1 followed by a STATUS-mode DMA read. The
        // previous unconditional reset was eating our operation-complete
        // bits and causing libultra's save thread to stall.
        m_FlashFlag = FLASHRAM_MODE_STATUS;
        if (m_FlashStatus == 0)
            m_FlashStatus = 0x1111800100C2001E;  // initial idle status
        Log::debug("Flash RAM: status mode");
        break;

    case 0xF0000000:  // Set read mode
        m_FlashFlag = FLASHRAM_MODE_READ;
        m_FlashStatus = 0x11118004F0000000;
        Log::debug("Flash RAM: read mode");
        break;

    case 0x4B000000:  // Set offset
        m_FlashRAM_Offset = (Value & 0xFFFF) * BLOCK_SIZE;
        Log::debugf("Flash RAM: set offset 0x%X", m_FlashRAM_Offset);
        break;

    case 0x78000000:  // Set erase mode
        m_FlashFlag = FLASHRAM_MODE_ERASE;
        m_FlashStatus = 0x1111800800C2001E;
        Log::debug("Flash RAM: erase mode");
        break;

    case 0xB4000000:  // Set write mode
        m_FlashFlag = FLASHRAM_MODE_WRITE;
        Log::debug("Flash RAM: write mode");
        break;

    case 0xA5000000:  // Execute Page Program
        m_FlashRAM_Offset = (Value & 0xFFFF) * BLOCK_SIZE;
        
        if (m_FlashFlag == FLASHRAM_MODE_WRITE)
        {
            if (!m_File.is_open() && !LoadFlashram())
            {
                Log::error("WriteToFlashCommand: failed to load Flash RAM for write");
                return;
            }
            if (!m_ReadOnly)
            {
                m_File.seek(m_FlashRAM_Offset, FileSystem::SeekOrigin::Begin);
                size_t bytesWritten = m_File.write(m_PageBuffer, BLOCK_SIZE);
                m_File.flush();
                Log::infof("[FlashRAM] wrote %zu bytes to offset 0x%X", bytesWritten, m_FlashRAM_Offset);
            }
        }
        
        m_FlashStatus = 0x1111800400C2001E;
        Log::debugf("Flash RAM: execute page program at 0x%X", m_FlashRAM_Offset);
        break;

    case 0x00000000:  // No-op / idle — games write 0 to reset the command register
        break;

    default:
        Log::warningf("WriteToFlashCommand: unknown command 0x%08X", Value);
        break;
    }
}

// Load Flash RAM from file
bool CFlashRam::LoadFlashram()
{
    extern CN64System* g_System;
    
    if (!g_System)
    {
        Log::error("Cannot load Flash RAM: no active system");
        return false;
    }

    // Build save file path
    // Format: <SaveDir>/<GameName>.fla
    std::string savePath = "Saves";
    std::string gameName = "game";
    if (g_Rom && g_Rom->IsLoaded()) {
        gameName = g_Rom->GetImage().GetRomName();
        for (char& c : gameName) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' ||
                c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                c = '_';
        }
        while (!gameName.empty() && gameName.back() == ' ') gameName.pop_back();
    }
    std::string fileName = gameName + ".fla";
    std::string fullPath = savePath + "/" + fileName;

    // Create save directory if it doesn't exist
    std::filesystem::create_directories(savePath);

    // Try to open the file
    FileSystem::OpenMode openMode = m_ReadOnly ? 
        FileSystem::OpenMode::Read : 
        (FileSystem::OpenMode::Read | FileSystem::OpenMode::Write);

    if (!m_File.is_open())
    {
        try {
            m_File.open(fullPath, openMode);
            Log::infof("Loaded Flash RAM from: %s", fullPath.c_str());
        } catch (...) {
            // File doesn't exist yet - create it
            if (!m_ReadOnly)
            {
                try {
                    m_File.open(fullPath, 
                               FileSystem::OpenMode::Read | 
                               FileSystem::OpenMode::Write | 
                               FileSystem::OpenMode::Create);
                    
                    // Initialize Flash RAM to 0xFF (erased state)
                    uint8_t emptyPage[4096];
                    std::memset(emptyPage, 0xFF, sizeof(emptyPage));
                    
                    // Write empty pages to fill entire Flash RAM
                    for (size_t i = 0; i < FLASHRAM_SIZE / sizeof(emptyPage); i++)
                    {
                        m_File.write(emptyPage, sizeof(emptyPage));
                    }
                    
                    m_File.seek(0, FileSystem::SeekOrigin::Begin);
                    m_File.flush();
                    
                    Log::infof("Created new Flash RAM save file: %s (%zu KB)", 
                             fullPath.c_str(), FLASHRAM_SIZE / 1024);
                } catch (...) {
                    Log::errorf("Failed to create Flash RAM file: %s", fullPath.c_str());
                    return false;
                }
            }
            else
            {
                Log::warningf("Flash RAM file not found (read-only mode): %s", fullPath.c_str());
                return false;
            }
        }
    }

    return m_File.is_open();
}
