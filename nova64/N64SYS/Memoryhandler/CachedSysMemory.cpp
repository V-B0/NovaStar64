/**
 * @file CachedSysMemory.cpp
 * @brief 64DD Disk Interface (ASIC) Register Handler
 * 
 * Handles cached memory accesses to the 64DD disk drive interface registers.
 * Memory region: 0x05000000-0x05FFFFFF (Cartridge Domain 2, Address 1)
 * 
 * The 64DD uses 22 ASIC registers at 0x05000500-0x0500054C for disk operations.
 */

#include "CachedSysMemory.hpp"
#include "../MIPSY/Register.hpp"
#include "../N64Rom.hpp"
#include "../../common/log.hpp"
#include <algorithm>
#include <cstring>

// 64DD ASIC register addresses (relative to 0x05000000)
namespace ASIC {
    constexpr uint32_t DATA         = 0x05000500;  // Data register
    constexpr uint32_t MISC_REG     = 0x05000504;  // Miscellaneous register
    constexpr uint32_t STATUS       = 0x05000508;  // Status register / Command register (write)
    constexpr uint32_t CUR_TK       = 0x0500050C;  // Current track
    constexpr uint32_t BM_STATUS    = 0x05000510;  // Buffer manager status / Control (write)
    constexpr uint32_t ERR_SECTOR   = 0x05000514;  // Error sector
    constexpr uint32_t SEQ_STATUS   = 0x05000518;  // Sequencer status / Control (write)
    constexpr uint32_t CUR_SECTOR   = 0x0500051C;  // Current sector
    constexpr uint32_t HARD_RESET   = 0x05000520;  // Hard reset (write)
    constexpr uint32_t C1_S0        = 0x05000524;  // C1 correction status 0
    constexpr uint32_t HOST_SECBYTE = 0x05000528;  // Host sector byte
    constexpr uint32_t C1_S2        = 0x0500052C;  // C1 correction status 2
    constexpr uint32_t SEC_BYTE     = 0x05000530;  // Sector byte
    constexpr uint32_t C1_S4        = 0x05000534;  // C1 correction status 4
    constexpr uint32_t C1_S6        = 0x05000538;  // C1 correction status 6
    constexpr uint32_t CUR_ADDR     = 0x0500053C;  // Current address
    constexpr uint32_t ID_REG       = 0x05000540;  // ID register
    constexpr uint32_t TEST_REG     = 0x05000544;  // Test register
    constexpr uint32_t TEST_PIN_SEL = 0x05000548;  // Test pin select
    
    // Status register flags
    constexpr uint32_t STATUS_DATA_REQUEST  = 0x40000000;  // Data request
    constexpr uint32_t STATUS_C2_TRANSFER   = 0x10000000;  // C2 transfer complete
    constexpr uint32_t STATUS_BM_ERROR      = 0x08000000;  // Buffer manager error
    constexpr uint32_t STATUS_BM_INT        = 0x04000000;  // Buffer manager interrupt
    constexpr uint32_t STATUS_MECHA_INT     = 0x02000000;  // Mechanical interrupt
    constexpr uint32_t STATUS_DISK_PRESENT  = 0x00010000;  // Disk present
    constexpr uint32_t STATUS_BUSY          = 0x00001000;  // Busy
    constexpr uint32_t STATUS_READY         = 0x00000001;  // Ready
    
    // BM Control commands
    constexpr uint32_t BM_CTL_START         = 0x80000000;  // Start transfer
    constexpr uint32_t BM_CTL_MODE_WRITE    = 0x40000000;  // Write mode (vs read)
    constexpr uint32_t BM_CTL_RESET         = 0x10000000;  // Reset buffer manager
    constexpr uint32_t BM_CTL_BLOCK         = 0x02000000;  // Block transfer complete
    
    // Disk commands
    constexpr uint32_t CMD_SEEK             = 0x01;  // Seek to track
    constexpr uint32_t CMD_READ             = 0x02;  // Read sector
    constexpr uint32_t CMD_WRITE            = 0x03;  // Write sector
    constexpr uint32_t CMD_REZERO           = 0x06;  // Recalibrate (track 0)
    constexpr uint32_t CMD_SLEEP            = 0x07;  // Sleep mode
    constexpr uint32_t CMD_CLEAR_CHANGE     = 0x08;  // Clear disk change flag
    constexpr uint32_t CMD_CLEAR_RESET      = 0x09;  // Clear reset flag
    constexpr uint32_t CMD_READ_ID          = 0x0A;  // Read sector ID
}

/**
 * @class DiskInterfaceReg
 * @brief Wrapper for 64DD ASIC register array with named accessors
 */
class DiskInterfaceReg {
public:
    explicit DiskInterfaceReg(uint32_t* diskInterface) :
        ASIC_DATA(diskInterface[0]),
        ASIC_MISC_REG(diskInterface[1]),
        ASIC_STATUS(diskInterface[2]),
        ASIC_CMD(diskInterface[3]),
        ASIC_CUR_TK(diskInterface[4]),
        ASIC_BM_STATUS(diskInterface[5]),
        ASIC_BM_CTL(diskInterface[6]),
        ASIC_ERR_SECTOR(diskInterface[7]),
        ASIC_SEQ_STATUS(diskInterface[8]),
        ASIC_SEQ_CTL(diskInterface[9]),
        ASIC_CUR_SECTOR(diskInterface[10]),
        ASIC_HARD_RESET(diskInterface[11]),
        ASIC_C1_S0(diskInterface[12]),
        ASIC_HOST_SECBYTE(diskInterface[13]),
        ASIC_C1_S2(diskInterface[14]),
        ASIC_SEC_BYTE(diskInterface[15]),
        ASIC_C1_S4(diskInterface[16]),
        ASIC_C1_S6(diskInterface[17]),
        ASIC_CUR_ADDR(diskInterface[18]),
        ASIC_ID_REG(diskInterface[19]),
        ASIC_TEST_REG(diskInterface[20]),
        ASIC_TEST_PIN_SEL(diskInterface[21])
    {
    }

    // Register references
    uint32_t& ASIC_DATA;         // [0] Data register
    uint32_t& ASIC_MISC_REG;     // [1] Miscellaneous
    uint32_t& ASIC_STATUS;       // [2] Status (read) / CMD (write)
    uint32_t& ASIC_CMD;          // [3] Command register shadow
    uint32_t& ASIC_CUR_TK;       // [4] Current track
    uint32_t& ASIC_BM_STATUS;    // [5] Buffer manager status (read)
    uint32_t& ASIC_BM_CTL;       // [6] Buffer manager control (write)
    uint32_t& ASIC_ERR_SECTOR;   // [7] Error sector
    uint32_t& ASIC_SEQ_STATUS;   // [8] Sequencer status (read)
    uint32_t& ASIC_SEQ_CTL;      // [9] Sequencer control (write)
    uint32_t& ASIC_CUR_SECTOR;   // [10] Current sector
    uint32_t& ASIC_HARD_RESET;   // [11] Hard reset
    uint32_t& ASIC_C1_S0;        // [12] C1 status 0
    uint32_t& ASIC_HOST_SECBYTE; // [13] Host sector byte
    uint32_t& ASIC_C1_S2;        // [14] C1 status 2
    uint32_t& ASIC_SEC_BYTE;     // [15] Sector byte
    uint32_t& ASIC_C1_S4;        // [16] C1 status 4
    uint32_t& ASIC_C1_S6;        // [17] C1 status 6
    uint32_t& ASIC_CUR_ADDR;     // [18] Current address
    uint32_t& ASIC_ID_REG;       // [19] ID register
    uint32_t& ASIC_TEST_REG;     // [20] Test register
    uint32_t& ASIC_TEST_PIN_SEL; // [21] Test pin select
};

CachedSysMemory::CachedSysMemory(CRegisters& reg, N64::Rom::N64Rom& ddRom) :
    m_Reg(reg),
    m_DDRom(ddRom),
    m_DiskInterface(reg.m_DiskInterface.data()),
    m_DiskEnabled(false),
    m_DiskPresent(false),
    m_CurrentTrack(0),
    m_CurrentSector(0),
    m_CommandActive(false),
    m_SequencerActive(false),
    m_SequencerRemaining(0),
    m_RDRAM(nullptr),
    m_RDRAMSize(0)
{
    // Check if this is a 64DD IPL ROM
    m_DiskEnabled = ddRom.IsLoaded(); // Temporary fix for IsDDIPL
    
    if (m_DiskEnabled) {
        Log::info("CachedSysMemory: 64DD disk interface enabled");
        InitializeDiskInterface();
    }
    
    // Initialize C2 buffer (32KB sector cache)
    m_C2Buffer.fill(0);
}

// ============================================================================
// Initialization
// ============================================================================

void CachedSysMemory::InitializeDiskInterface()
{
    DiskInterfaceReg regs(m_DiskInterface);
    
    // Initialize ASIC registers to power-on state
    regs.ASIC_DATA = 0;
    regs.ASIC_MISC_REG = 0;
    regs.ASIC_STATUS = ASIC::STATUS_READY;  // Ready, not busy
    regs.ASIC_CMD = 0;
    regs.ASIC_CUR_TK = 0;
    regs.ASIC_BM_STATUS = 0;
    regs.ASIC_BM_CTL = 0;
    regs.ASIC_ERR_SECTOR = 0;
    regs.ASIC_SEQ_STATUS = 0;
    regs.ASIC_SEQ_CTL = 0;
    regs.ASIC_CUR_SECTOR = 0;
    regs.ASIC_HARD_RESET = 0;
    regs.ASIC_C1_S0 = 0;
    regs.ASIC_HOST_SECBYTE = 0;
    regs.ASIC_C1_S2 = 0;
    regs.ASIC_SEC_BYTE = 0;
    regs.ASIC_C1_S4 = 0;
    regs.ASIC_C1_S6 = 0;
    regs.ASIC_CUR_ADDR = 0;
    regs.ASIC_ID_REG = 0x00040000;  // ASIC version
    regs.ASIC_TEST_REG = 0;
    regs.ASIC_TEST_PIN_SEL = 0;
    
    m_CurrentTrack = 0;
    m_CurrentSector = 0;
    m_CommandActive = false;
    m_DiskPresent = (m_DDRom.GetSize() > 0);  // Disk present if ROM loaded
    
    if (m_DiskPresent) {
        regs.ASIC_STATUS |= ASIC::STATUS_DISK_PRESENT;
    }
}

// ============================================================================
// Read Operations
// ============================================================================

bool CachedSysMemory::Read32(uint32_t addr, uint32_t& val)
{
    // Check if 64DD is enabled
    if (!m_DiskEnabled) {
        // Return open bus value
        val = (addr << 16) | (addr & 0xFFFF);
        return true;
    }
    
    DiskInterfaceReg regs(m_DiskInterface);
    
    // Decode ASIC register address
    switch (addr & 0x1FFFFFFF) {
        case ASIC::DATA:
            val = regs.ASIC_DATA;
            break;
            
        case ASIC::MISC_REG:
            val = regs.ASIC_MISC_REG;
            break;
            
        case ASIC::STATUS:
            val = regs.ASIC_STATUS;
            // Reading status may update disk position (gap sector detection)
            UpdateDiskPosition();
            break;
            
        case ASIC::CUR_TK:
            val = regs.ASIC_CUR_TK;
            break;
            
        case ASIC::BM_STATUS:
            val = regs.ASIC_BM_STATUS;
            break;
            
        case ASIC::ERR_SECTOR:
            val = regs.ASIC_ERR_SECTOR;
            break;
            
        case ASIC::SEQ_STATUS:
            val = regs.ASIC_SEQ_STATUS;
            break;
            
        case ASIC::CUR_SECTOR:
            val = regs.ASIC_CUR_SECTOR;
            break;
            
        case ASIC::HARD_RESET:
            val = regs.ASIC_HARD_RESET;
            break;
            
        case ASIC::C1_S0:
            val = regs.ASIC_C1_S0;
            break;
            
        case ASIC::HOST_SECBYTE:
            val = regs.ASIC_HOST_SECBYTE;
            break;
            
        case ASIC::C1_S2:
            val = regs.ASIC_C1_S2;
            break;
            
        case ASIC::SEC_BYTE:
            val = regs.ASIC_SEC_BYTE;
            break;
            
        case ASIC::C1_S4:
            val = regs.ASIC_C1_S4;
            break;
            
        case ASIC::C1_S6:
            val = regs.ASIC_C1_S6;
            break;
            
        case ASIC::CUR_ADDR:
            val = regs.ASIC_CUR_ADDR;
            break;
            
        case ASIC::ID_REG:
            val = regs.ASIC_ID_REG;
            break;
            
        case ASIC::TEST_REG:
            val = regs.ASIC_TEST_REG;
            break;
            
        case ASIC::TEST_PIN_SEL:
            val = regs.ASIC_TEST_PIN_SEL;
            break;
            
        default:
            // Unhandled register - return open bus
            val = (addr << 16) | (addr & 0xFFFF);
            Log::warningf("CachedSysMemory: Read from unhandled ASIC register 0x%08X", addr);
            break;
    }
    
    return true;
}

// ============================================================================
// Write Operations
// ============================================================================

bool CachedSysMemory::Write32(uint32_t addr, uint32_t val, uint32_t mask)
{
    // Check if 64DD is enabled
    if (!m_DiskEnabled) {
        return true;  // Silently ignore writes when disabled
    }
    
    DiskInterfaceReg regs(m_DiskInterface);
    
    // Decode ASIC register address
    switch (addr & 0x1FFFFFFF) {
        case ASIC::DATA:
            regs.ASIC_DATA = (regs.ASIC_DATA & ~mask) | (val & mask);
            break;
            
        case ASIC::STATUS:
            // Write to status register = command register
            regs.ASIC_CMD = (regs.ASIC_CMD & ~mask) | (val & mask);
            ProcessDiskCommand();
            break;
            
        case ASIC::BM_STATUS:
            // Write to BM status = BM control
            regs.ASIC_BM_CTL = (regs.ASIC_BM_CTL & ~mask) | (val & mask);
            ProcessBufferManagerControl();
            break;
            
        case ASIC::SEQ_STATUS:
            // Write to sequencer status = sequencer control
            regs.ASIC_SEQ_CTL = (regs.ASIC_SEQ_CTL & ~mask) | (val & mask);
            ProcessSequencerControl();
            break;
            
        case ASIC::HARD_RESET:
            // Hard reset trigger
            PerformHardReset();
            break;
            
        case ASIC::HOST_SECBYTE:
            regs.ASIC_HOST_SECBYTE = (regs.ASIC_HOST_SECBYTE & ~mask) | (val & mask);
            break;
            
        case ASIC::SEC_BYTE:
            regs.ASIC_SEC_BYTE = (regs.ASIC_SEC_BYTE & ~mask) | (val & mask);
            break;
            
        case ASIC::TEST_PIN_SEL:
            regs.ASIC_TEST_PIN_SEL = (regs.ASIC_TEST_PIN_SEL & ~mask) | (val & mask);
            break;
            
        default:
            Log::warningf("CachedSysMemory: Write to unhandled ASIC register 0x%08X = 0x%08X", addr, val);
            break;
    }
    
    return true;
}

// ============================================================================
// Disk Command Processing
// ============================================================================

void CachedSysMemory::ProcessDiskCommand()
{
    DiskInterfaceReg regs(m_DiskInterface);
    uint32_t cmd = regs.ASIC_CMD & 0xFF;
    
    // Mark as busy
    regs.ASIC_STATUS |= ASIC::STATUS_BUSY;
    m_CommandActive = true;
    
    switch (cmd) {
        case ASIC::CMD_SEEK:
            CommandSeek();
            break;
            
        case ASIC::CMD_READ:
            CommandRead();
            break;
            
        case ASIC::CMD_WRITE:
            CommandWrite();
            break;
            
        case ASIC::CMD_REZERO:
            CommandRezero();
            break;
            
        case ASIC::CMD_SLEEP:
            CommandSleep();
            break;
            
        case ASIC::CMD_CLEAR_CHANGE:
            CommandClearChange(); 
            break; 
            
        case ASIC::CMD_CLEAR_RESET: 
            CommandClearReset(); 
            break; 
            
        case ASIC::CMD_READ_ID: 
            CommandReadID(); 
            break; 
            
        default: 
            Log::warningf("CachedSysMemory: Unknown disk command 0x%02X", cmd); 
            CompleteCommand(true); // Complete with error 
            break; 
    } 
} 

void CachedSysMemory::CommandSeek() 
{ 
    DiskInterfaceReg regs(m_DiskInterface); 
    uint32_t targetTrack = (regs.ASIC_CMD >> 16) & 0xFFF; 
    
    Log::debugf("64DD: Seek to track %u", targetTrack); 
    
    // Update current track 
    m_CurrentTrack = targetTrack; 
    regs.ASIC_CUR_TK = targetTrack; 
    
    // Seek complete 
    CompleteCommand(false); 
} 

void CachedSysMemory::CommandRead() 
{ 
    DiskInterfaceReg regs(m_DiskInterface);
    
    // Set data request flag - data is ready for buffer manager transfer
    regs.ASIC_STATUS |= ASIC::STATUS_DATA_REQUEST;
    
    // Command completes - buffer manager will handle DMA to RDRAM
    CompleteCommand(false);
}

void CachedSysMemory::CommandWrite()
{
    DiskInterfaceReg regs(m_DiskInterface);
    
    Log::debugf("64DD: Write sector at track %u, sector %u",
        m_CurrentTrack, m_CurrentSector);
    
    // Clear C2 buffer to prepare for incoming write data from RDRAM
    m_C2Buffer.fill(0);
    
    // Set data request flag - ready to receive data from buffer manager
    regs.ASIC_STATUS |= ASIC::STATUS_DATA_REQUEST;
    
    // Command completes - buffer manager will handle DMA from RDRAM and flush
    CompleteCommand(false);
}

void CachedSysMemory::CommandRezero()
{
    Log::debugf("64DD: Rezero (seek to track 0)");
    
    m_CurrentTrack = 0;
    m_CurrentSector = 0;
    
    DiskInterfaceReg regs(m_DiskInterface);
    regs.ASIC_CUR_TK = 0;
    regs.ASIC_CUR_SECTOR = 0;
    
    CompleteCommand(false);
}

void CachedSysMemory::CommandSleep()
{
    Log::debugf("64DD: Sleep mode");
    CompleteCommand(false);
}

void CachedSysMemory::CommandClearChange()
{
    Log::debugf("64DD: Clear disk change flag");
    CompleteCommand(false);
}

void CachedSysMemory::CommandClearReset()
{
    Log::debugf("64DD: Clear reset flag");
    CompleteCommand(false);
}

void CachedSysMemory::CommandReadID()
{
    DiskInterfaceReg regs(m_DiskInterface);
    
    Log::debugf("64DD: Read ID");
    
    // Return current sector ID
    regs.ASIC_CUR_SECTOR = m_CurrentSector;
    
    CompleteCommand(false);
}

void CachedSysMemory::CompleteCommand(bool error)
{
    DiskInterfaceReg regs(m_DiskInterface);
    
    // Clear busy flag
    regs.ASIC_STATUS &= ~ASIC::STATUS_BUSY;
    m_CommandActive = false;
    
    if (error) {
        // Set error flag
        regs.ASIC_BM_STATUS |= ASIC::STATUS_BM_ERROR;
    }
    
    // Generate mechanical interrupt
    regs.ASIC_STATUS |= ASIC::STATUS_MECHA_INT;
    
    // Trigger MI interrupt (disk uses PI interrupt line)
    m_Reg.m_Peripheral_Interface[0] |= 0x08;  // PI_STATUS_DMA_BUSY | PI_STATUS_INTERRUPT
    m_Reg.m_Mips_Interface[2] |= 0x10;  // MI_INTR_REG |= MI_INTR_PI
    m_Reg.CheckInterrupts();
}

// ============================================================================
// Buffer Manager Control
// ============================================================================

void CachedSysMemory::ProcessBufferManagerControl()
{
    DiskInterfaceReg regs(m_DiskInterface);
    uint32_t bmCtl = regs.ASIC_BM_CTL;
    
    if (bmCtl & ASIC::BM_CTL_RESET) {
        // Reset buffer manager
        Log::debugf("64DD: Buffer manager reset");
        regs.ASIC_BM_STATUS = 0;
        regs.ASIC_BM_CTL &= ~ASIC::BM_CTL_RESET;
        return;
    }
    
    if (bmCtl & ASIC::BM_CTL_START) {
        // Start DMA transfer
        bool isWrite = (bmCtl & ASIC::BM_CTL_MODE_WRITE) != 0;
        
        Log::debugf("64DD: Start buffer manager %s",
            isWrite ? "write" : "read");
        
        // Get DMA parameters from ASIC registers
        uint32_t dmaAddress = regs.ASIC_CUR_ADDR & 0x00FFFFFF;  // 24-bit RDRAM address
        uint32_t sectorByte = regs.ASIC_SEC_BYTE & 0xFF;
        uint32_t hostSecByte = regs.ASIC_HOST_SECBYTE & 0xFF;
        
        // Calculate transfer length (typically one block = 256 bytes)
        uint32_t blockLength = (bmCtl >> 16) & 0x7F;  // Block count in control register
        if (blockLength == 0) blockLength = 1;
        uint32_t transferLength = blockLength * 256;
        
        // Perform DMA transfer
        if (isWrite) {
            // DMA Write: RDRAM -> C2 buffer -> Disk
            PerformDMAWrite(dmaAddress, transferLength, sectorByte);
        } else {
            // DMA Read: Disk -> C2 buffer -> RDRAM
            PerformDMARead(dmaAddress, transferLength, sectorByte);
        }
        
        // Mark transfer complete
        regs.ASIC_STATUS |= ASIC::STATUS_C2_TRANSFER;
        regs.ASIC_BM_STATUS |= ASIC::BM_CTL_BLOCK;
        
        // Generate BM interrupt
        regs.ASIC_STATUS |= ASIC::STATUS_BM_INT;
        
        // Trigger interrupt to CPU
        m_Reg.m_Peripheral_Interface[0] |= 0x08;
        m_Reg.m_Mips_Interface[2] |= 0x10;
        m_Reg.CheckInterrupts();
        
        // Clear start flag
        regs.ASIC_BM_CTL &= ~ASIC::BM_CTL_START;
    }
}

// ============================================================================
// Sequencer Control
// ============================================================================

void CachedSysMemory::ProcessSequencerControl()
{
    DiskInterfaceReg regs(m_DiskInterface);
    uint32_t seqCtl = regs.ASIC_SEQ_CTL;
    
    // Sequencer control for multi-sector operations
    // Bits: [15:0] = sector count, [16] = enable, [17] = auto-increment
    
    if (seqCtl & 0x00010000) {  // Sequencer enabled
        uint32_t sectorCount = seqCtl & 0xFFFF;
        bool autoIncrement = (seqCtl & 0x00020000) != 0;
        
        Log::debugf("64DD: Sequencer start - %u sectors, auto=%d",
            sectorCount, autoIncrement);
        
        // Process multi-sector operation
        if (sectorCount > 0) {
            // Update sequencer status
            regs.ASIC_SEQ_STATUS = sectorCount;  // Remaining sectors
            
            // Start first sector operation
            // The sequencer automatically triggers read/write for each sector
            // and advances the sector counter
            
            if (autoIncrement) {
                // Auto-increment current sector after each operation
                m_SequencerActive = true;
                m_SequencerRemaining = sectorCount;
            }
        }
        
        // Clear enable bit
        regs.ASIC_SEQ_CTL &= ~0x00010000;
    }
}

// ============================================================================
// Disk Position Management
// ============================================================================

void CachedSysMemory::UpdateDiskPosition()
{
    // Simulate disk rotation - update current sector
    // The disk rotates continuously, and reading status updates the sector counter
    
    if (!m_DiskPresent || !m_DiskEnabled) {
        return;
    }
    
    DiskInterfaceReg regs(m_DiskInterface);
    
    // Advance sector (simplified simulation)
    // Real hardware: sectors advance based on spindle motor rotation
    m_CurrentSector = (m_CurrentSector + 1) % 85;  // 85 sectors per track (approximate)
    regs.ASIC_CUR_SECTOR = m_CurrentSector;
}

// ============================================================================
// Reset Operations
// ============================================================================

void CachedSysMemory::PerformHardReset()
{
    Log::infof("64DD: Hard reset");
    
    if (m_DiskEnabled) {
        InitializeDiskInterface();
    }
}

// ============================================================================
// DMA Operations
// ============================================================================

void CachedSysMemory::SetRDRAM(uint8_t* rdram, uint32_t size)
{
    m_RDRAM = rdram;
    m_RDRAMSize = size;
}

void CachedSysMemory::PerformDMARead(uint32_t rdramAddr, uint32_t length, uint32_t sectorOffset)
{
    if (!m_RDRAM || m_RDRAMSize == 0) {
        Log::errorf("64DD: DMA read failed - RDRAM not initialized");
        return;
    }
    
    // Validate RDRAM address
    if (rdramAddr + length > m_RDRAMSize) {
        Log::errorf("64DD: DMA read failed - invalid RDRAM address 0x%08X (size 0x%X)",
            rdramAddr, length);
        return;
    }
    
    // Load current sector into C2 buffer if not already loaded
    LoadCurrentSectorToC2Buffer();
    
    // Copy from C2 buffer to RDRAM
    uint32_t bufferOffset = sectorOffset % 32768;  // C2 buffer is 32KB
    uint32_t bytesToCopy = (bufferOffset + length > 32768) ? (32768 - bufferOffset) : length;
    
    std::memcpy(m_RDRAM + rdramAddr, m_C2Buffer.data() + bufferOffset, bytesToCopy);
    
    Log::debugf("64DD: DMA read - RDRAM[0x%08X] <- C2[0x%04X] (%u bytes)",
        rdramAddr, bufferOffset, bytesToCopy);
}

void CachedSysMemory::PerformDMAWrite(uint32_t rdramAddr, uint32_t length, uint32_t sectorOffset)
{
    if (!m_RDRAM || m_RDRAMSize == 0) {
        Log::errorf("64DD: DMA write failed - RDRAM not initialized");
        return;
    }
    
    // Validate RDRAM address
    if (rdramAddr + length > m_RDRAMSize) {
        Log::errorf("64DD: DMA write failed - invalid RDRAM address 0x%08X (size 0x%X)",
            rdramAddr, length);
        return;
    }
    
    // Copy from RDRAM to C2 buffer
    uint32_t bufferOffset = sectorOffset % 32768;
    uint32_t bytesToCopy = (bufferOffset + length > 32768) ? (32768 - bufferOffset) : length;
    
    std::memcpy(m_C2Buffer.data() + bufferOffset, m_RDRAM + rdramAddr, bytesToCopy);
    
    Log::debugf("64DD: DMA write - RDRAM[0x%08X] -> C2[0x%04X] (%u bytes)",
        rdramAddr, bufferOffset, bytesToCopy);
    
    // Flush C2 buffer to disk image
    FlushC2BufferToSector();
}

void CachedSysMemory::LoadCurrentSectorToC2Buffer()
{
    // Calculate absolute sector from track/sector position
    // 64DD disk format: ~123MB total, variable sectors per zone
    // Using simplified fixed sector layout for sector calculation
    const uint32_t sectorsPerTrack = 85;
    uint32_t absoluteSector = (m_CurrentTrack * sectorsPerTrack) + m_CurrentSector;
    
    // Load sector data from disk image
    const uint8_t* diskData = m_DDRom.GetRomAddress();
    size_t diskSize = m_DDRom.GetSize();
    
    if (!diskData || diskSize == 0) {
        Log::warningf("64DD: No disk data available for sector %u", absoluteSector);
        m_C2Buffer.fill(0);
        return;
    }
    
    // Calculate sector offset in disk image
    // C2 buffer holds multiple 512-byte sectors (32KB = 64 sectors)
    const uint32_t sectorSize = 512;
    uint32_t offset = absoluteSector * sectorSize;
    
    if (offset >= diskSize) {
        // Sector beyond disk size
        Log::debugf("64DD: Sector %u beyond disk size (offset 0x%X, size 0x%zX)",
            absoluteSector, offset, diskSize);
        m_C2Buffer.fill(0);
        
        DiskInterfaceReg regs(m_DiskInterface);
        regs.ASIC_ERR_SECTOR = absoluteSector;
        return;
    }
    
    // Read up to 32KB starting from sector offset
    uint32_t bytesToRead = ((offset + 32768) > diskSize) ? static_cast<uint32_t>(diskSize - offset) : 32768;
    std::memcpy(m_C2Buffer.data(), diskData + offset, bytesToRead);
    
    // Zero-fill remaining buffer space
    if (bytesToRead < 32768) {
        std::memset(m_C2Buffer.data() + bytesToRead, 0, 32768 - bytesToRead);
    }
    
    Log::debugf("64DD: Loaded sector %u (track %u, sector %u) - %u bytes to C2 buffer",
        absoluteSector, m_CurrentTrack, m_CurrentSector, bytesToRead);
}

void CachedSysMemory::FlushC2BufferToSector()
{
    // Calculate absolute sector from track/sector position
    const uint32_t sectorsPerTrack = 85;
    uint32_t absoluteSector = (m_CurrentTrack * sectorsPerTrack) + m_CurrentSector;
    
    // Get writable pointer to disk image
    uint8_t* diskData = m_DDRom.GetRomAddress();
    size_t diskSize = m_DDRom.GetSize();
    
    if (!diskData || diskSize == 0) {
        Log::errorf("64DD: Cannot flush - no disk data available");
        DiskInterfaceReg regs(m_DiskInterface);
        regs.ASIC_STATUS |= ASIC::STATUS_BM_ERROR;
        return;
    }
    
    // Calculate sector offset in disk image
    const uint32_t sectorSize = 512;
    uint32_t offset = absoluteSector * sectorSize;
    
    if (offset >= diskSize) {
        Log::errorf("64DD: Cannot flush - sector %u beyond disk size", absoluteSector);
        DiskInterfaceReg regs(m_DiskInterface);
        regs.ASIC_ERR_SECTOR = absoluteSector;
        regs.ASIC_STATUS |= ASIC::STATUS_BM_ERROR;
        return;
    }
    
    // Write C2 buffer contents back to disk image (up to 32KB)
    uint32_t bytesToWrite = ((offset + 32768) > diskSize) ? static_cast<uint32_t>(diskSize - offset) : 32768;
    std::memcpy(diskData + offset, m_C2Buffer.data(), bytesToWrite);
    
    Log::debugf("64DD: Flushed C2 buffer to sector %u (track %u, sector %u) - %u bytes",
        absoluteSector, m_CurrentTrack, m_CurrentSector, bytesToWrite);
}
