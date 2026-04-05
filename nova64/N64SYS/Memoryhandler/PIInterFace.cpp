/**
 * @file PIInterFace.cpp
 * @brief Peripheral Interface (PI) Handler Implementation
 * 
 * Handles the Peripheral Interface which controls:
 * - ROM/cartridge access via DMA
 * - Save memory (SRAM/FlashRAM) DMA
 * - 64DD disk DMA transfers
 * - Bus timing configuration
 * 
 * Memory region: 0x04600000-0x04600034
 * 
 * Register map:
 * 0x04600000 - PI_DRAM_ADDR_REG - RDRAM address for DMA
 * 0x04600004 - PI_CART_ADDR_REG - Cartridge address for DMA
 * 0x04600008 - PI_RD_LEN_REG - DMA read length (RDRAM <- Cart)
 * 0x0460000C - PI_WR_LEN_REG - DMA write length (RDRAM -> Cart)
 * 0x04600010 - PI_STATUS_REG - Status and control
 * 0x04600014-0x04600020 - Domain 1 timing (ROM)
 * 0x04600024-0x04600030 - Domain 2 timing (Save memory)
 */

#include "PIInterFace.hpp"
#include "../MainSYS.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../MIPSY/DDisk.hpp"
#include "../MIPSY/Systiming.hpp"
#include "../N64Rom.hpp"
#include "../N64DD.hpp"
#include "UnCachedRom.hpp"
#include "../../common/log.hpp"
#include "../../common/MemoryManagment.hpp"
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <cstdio>

namespace PI {

// ============================================================================
// PI Status Register Bits
// ============================================================================

constexpr uint32_t STATUS_DMA_BUSY  = 0x01;
constexpr uint32_t STATUS_IO_BUSY   = 0x02;
constexpr uint32_t STATUS_ERROR     = 0x04;
constexpr uint32_t STATUS_INTERRUPT = 0x08;

// PI Control Bits (write-only)
constexpr uint32_t CTRL_SET_RESET   = 0x01;
constexpr uint32_t CTRL_CLR_INTR    = 0x02;

// MI Interrupt bits
constexpr uint32_t MI_INTR_PI       = 0x10;

// ============================================================================
// Memory Regions
// ============================================================================

constexpr uint32_t ROM_START        = 0x10000000;
constexpr uint32_t ROM_END          = 0x1FBFFFFF;

constexpr uint32_t DD_C2_BUFFER_START = 0x05000000;
constexpr uint32_t DD_C2_BUFFER_END   = 0x050003FF;

constexpr uint32_t DD_USER_SECTOR_START = 0x05000400;
constexpr uint32_t DD_USER_SECTOR_END   = 0x050004FF;

constexpr uint32_t DD_MSEQ_START    = 0x05000580;
constexpr uint32_t DD_MSEQ_END      = 0x050005BF;

constexpr uint32_t DD_IPL_ROM_START = 0x06000000;
constexpr uint32_t DD_IPL_ROM_END   = 0x063FFFFF;

constexpr uint32_t SRAM_START       = 0x08000000;
constexpr uint32_t SRAM_END         = 0x08088000;

constexpr uint32_t EVERDRIVE_START  = 0x1F800000;
constexpr uint32_t EVERDRIVE_END    = 0x1F810000;

PeripheralInterfaceHandler::PeripheralInterfaceHandler(
    CN64System& system,
    Nova64::MipsMemoryVM& mmu,
    CRegisters& registers,
    CEeprom& eeprom,
    UncachedRom& uncached_rom,
    const Config& config) :
    m_system(system),
    m_mmu(mmu),
    m_registers(registers),
    m_eeprom(eeprom),
    m_uncached_rom(uncached_rom),
    m_config(config),
    m_pi_regs(m_pi_registers_array)
{
    Log::infof("PIInterface: Initialized");
    
    // Initialize default bus timing
    reset();
}

PeripheralInterfaceHandler::~PeripheralInterfaceHandler()
{
    Log::infof("PIInterface: Destroyed");
}

// ============================================================================
// Read Operations
// ============================================================================

bool PeripheralInterfaceHandler::Read32(uint32_t addr, uint32_t& val)
{
    switch (addr & 0x1FFFFFFF) {
        case 0x04600000: val = m_pi_regs.PI_DRAM_ADDR_REG; break;
        case 0x04600004: val = m_pi_regs.PI_CART_ADDR_REG; break;
        case 0x04600008: val = m_pi_regs.PI_RD_LEN_REG; break;
        case 0x0460000C: val = m_pi_regs.PI_WR_LEN_REG; break;
        case 0x04600010: val = m_pi_regs.PI_STATUS_REG; break;
        case 0x04600014: val = m_pi_regs.PI_DOMAIN1_REG; break;
        case 0x04600018: val = m_pi_regs.PI_BSD_DOM1_PWD_REG; break;
        case 0x0460001C: val = m_pi_regs.PI_BSD_DOM1_PGS_REG; break;
        case 0x04600020: val = m_pi_regs.PI_BSD_DOM1_RLS_REG; break;
        case 0x04600024: val = m_pi_regs.PI_DOMAIN2_REG; break;
        case 0x04600028: val = m_pi_regs.PI_BSD_DOM2_PWD_REG; break;
        case 0x0460002C: val = m_pi_regs.PI_BSD_DOM2_PGS_REG; break;
        case 0x04600030: val = m_pi_regs.PI_BSD_DOM2_RLS_REG; break;
        
        default:
            val = 0;
            Log::warningf("PIInterface: Read from unmapped register 0x%08X", addr);
            return false;
    }
    
    Log::debugf("PIInterface: Read register[0x%08X] = 0x%08X", addr, val);
    return true;
}

// ============================================================================
// Write Operations
// ============================================================================

bool PeripheralInterfaceHandler::Write32(uint32_t addr, uint32_t val, uint32_t mask)
{
    uint32_t maskedVal = val & mask;
    
    Log::debugf("PIInterface: Write register[0x%08X] = 0x%08X (mask=0x%08X)", 
        addr, val, mask);
    
    switch (addr & 0x1FFFFFFF) {
        case 0x04600000:  // PI_DRAM_ADDR_REG
            m_pi_regs.PI_DRAM_ADDR_REG = (m_pi_regs.PI_DRAM_ADDR_REG & ~mask) | maskedVal;
            m_pi_regs.PI_DRAM_ADDR_REG &= 0x00FFFFFE;  // 24-bit, 2-byte aligned
            Log::debugf("PIInterface: PI_DRAM_ADDR = 0x%08X", m_pi_regs.PI_DRAM_ADDR_REG);
            break;
            
        case 0x04600004:  // PI_CART_ADDR_REG
            m_pi_regs.PI_CART_ADDR_REG = (m_pi_regs.PI_CART_ADDR_REG & ~mask) | maskedVal;
            // Most games use aligned DMA, but some don't
            if (!m_config.allow_unaligned_dma) {
                m_pi_regs.PI_CART_ADDR_REG &= 0xFFFFFFFE;
            }
            Log::debugf("PIInterface: PI_CART_ADDR = 0x%08X", m_pi_regs.PI_CART_ADDR_REG);
            break;
            
        case 0x04600008:  // PI_RD_LEN_REG - PI reads from DRAM (RDRAM -> Cart)
            Log::debugf("PIInterface: PI_RD_LEN written = 0x%08X -> HandleDMAWrite(RDRAM->Cart)", maskedVal);
            m_pi_regs.PI_RD_LEN_REG = (m_pi_regs.PI_RD_LEN_REG & ~mask) | maskedVal;
            m_pi_regs.PI_RD_LEN_REG &= 0x00FFFFFF;  // 24-bit
            HandleDMAWrite();
            break;
            
        case 0x0460000C:  // PI_WR_LEN_REG - PI writes to DRAM (Cart -> RDRAM)
            Log::debugf("PIInterface: PI_WR_LEN written = 0x%08X -> HandleDMARead(Cart->RDRAM)", maskedVal);
            m_pi_regs.PI_WR_LEN_REG = (m_pi_regs.PI_WR_LEN_REG & ~mask) | maskedVal;
            m_pi_regs.PI_WR_LEN_REG &= 0x00FFFFFF;  // 24-bit
            HandleDMARead();
            break;
            
        case 0x04600010:  // PI_STATUS_REG (control register)
            ProcessStatusWrite(maskedVal);
            break;
            
        case 0x04600014:  // PI_DOMAIN1_REG
            m_pi_regs.PI_DOMAIN1_REG = (m_pi_regs.PI_DOMAIN1_REG & ~mask) | maskedVal;
            m_pi_regs.PI_DOMAIN1_REG &= 0xFF;
            break;
            
        case 0x04600018:  // PI_BSD_DOM1_PWD_REG
            m_pi_regs.PI_BSD_DOM1_PWD_REG = (m_pi_regs.PI_BSD_DOM1_PWD_REG & ~mask) | maskedVal;
            m_pi_regs.PI_BSD_DOM1_PWD_REG &= 0xFF;
            break;
            
        case 0x0460001C:  // PI_BSD_DOM1_PGS_REG
            m_pi_regs.PI_BSD_DOM1_PGS_REG = (m_pi_regs.PI_BSD_DOM1_PGS_REG & ~mask) | maskedVal;
            m_pi_regs.PI_BSD_DOM1_PGS_REG &= 0xFF;
            break;
            
        case 0x04600020:  // PI_BSD_DOM1_RLS_REG
            m_pi_regs.PI_BSD_DOM1_RLS_REG = (m_pi_regs.PI_BSD_DOM1_RLS_REG & ~mask) | maskedVal;
            m_pi_regs.PI_BSD_DOM1_RLS_REG &= 0xFF;
            break;
            
        case 0x04600024:  // PI_DOMAIN2_REG
            m_pi_regs.PI_DOMAIN2_REG = (m_pi_regs.PI_DOMAIN2_REG & ~mask) | maskedVal;
            m_pi_regs.PI_DOMAIN2_REG &= 0xFF;
            break;
            
        case 0x04600028:  // PI_BSD_DOM2_PWD_REG
            m_pi_regs.PI_BSD_DOM2_PWD_REG = (m_pi_regs.PI_BSD_DOM2_PWD_REG & ~mask) | maskedVal;
            m_pi_regs.PI_BSD_DOM2_PWD_REG &= 0xFF;
            break;
            
        case 0x0460002C:  // PI_BSD_DOM2_PGS_REG
            m_pi_regs.PI_BSD_DOM2_PGS_REG = (m_pi_regs.PI_BSD_DOM2_PGS_REG & ~mask) | maskedVal;
            m_pi_regs.PI_BSD_DOM2_PGS_REG &= 0xFF;
            break;
            
        case 0x04600030:  // PI_BSD_DOM2_RLS_REG
            m_pi_regs.PI_BSD_DOM2_RLS_REG = (m_pi_regs.PI_BSD_DOM2_RLS_REG & ~mask) | maskedVal;
            m_pi_regs.PI_BSD_DOM2_RLS_REG &= 0xFF;
            break;
            
        default:
            Log::warningf("PIInterface: Write to unmapped register 0x%08X = 0x%08X", 
                addr, val);
            return false;
    }
    
    return true;
}

// ============================================================================
// Status Register Processing
// ============================================================================

void PeripheralInterfaceHandler::ProcessStatusWrite(uint32_t val)
{
    if (val & CTRL_CLR_INTR) {
        // Clear PI interrupt
        m_registers.m_Mips_Interface[2] &= ~MI_INTR_PI;
        m_pi_regs.PI_STATUS_REG &= ~STATUS_INTERRUPT;
        m_registers.CheckInterrupts();
        
        Log::debugf("PIInterface: Clear PI interrupt");
    }
    
    if (val & CTRL_SET_RESET) {
        // Reset PI (not commonly used)
        Log::debugf("PIInterface: PI reset requested");
    }
}

// ============================================================================
// DMA Read (Cart -> RDRAM)
// ============================================================================

void PeripheralInterfaceHandler::HandleDMARead()
{
    if (!m_dma_used) {
        m_dma_used = true;
        OnFirstDMA();
    }

    uint32_t dramAddr = m_pi_regs.PI_DRAM_ADDR_REG;
    uint32_t cartAddr = m_pi_regs.PI_CART_ADDR_REG;
    uint32_t length = (m_pi_regs.PI_WR_LEN_REG + 1) & ~1;  // Use PI_WR_LEN (Cart->RDRAM)
    
    Log::debugf("PIInterface: DMA Read - RDRAM=0x%08X Cart=0x%08X Length=0x%X",
        dramAddr, cartAddr, length);
    
    m_stats.dma_reads++;
    
    // Validate RDRAM bounds
    if (dramAddr + length > m_mmu.RdramSize()) {
        Log::errorf("PIInterface: DMA Read out of RDRAM bounds");
        m_stats.dma_errors++;
        CompleteDMA();
        return;
    }
    
    // Handle 64DD C2 buffer
    if (cartAddr >= DD_C2_BUFFER_START && cartAddr <= DD_C2_BUFFER_END) {
        Log::debugf("PIInterface: DMA Read from 64DD C2 buffer");
        uint32_t cycles = (length * 63) / 25;
        m_pi_regs.PI_STATUS_REG |= STATUS_DMA_BUSY;
        m_system.GetSystemTimer()->set_timer(N64System::TimerType::DDPiTimer, cycles, false);
        CompleteDMA();
        return;
    }
    
    // Handle 64DD user sector
    if (cartAddr >= DD_USER_SECTOR_START && cartAddr <= DD_USER_SECTOR_END) {
        Log::debugf("PIInterface: DMA Read from 64DD user sector");
        uint8_t* rdram = m_mmu.Rdram();
        N64Disk* disk = m_system.GetDisk();
        
        if (disk && disk->IsLoaded()) {
            const uint8_t* diskBuf = disk->GetImage().Data();
            for (uint32_t i = 0; i < length; i++) {
                rdram[((dramAddr + i) ^ 3)] = diskBuf[cartAddr - DD_USER_SECTOR_START + i];
            }
            m_stats.bytes_read += length;
        }
        
        uint32_t cycles = (length * 63) / 25;
        m_pi_regs.PI_STATUS_REG |= STATUS_DMA_BUSY;
        m_system.GetSystemTimer()->set_timer(N64System::TimerType::DDPiTimer, cycles, false);
        CompleteDMA();
        return;
    }
    
    // Handle 64DD MSEQ
    if (cartAddr >= DD_MSEQ_START && cartAddr <= DD_MSEQ_END) {
        Log::debugf("PIInterface: DMA Read from 64DD MSEQ (ignored)");
        CompleteDMA();
        return;
    }
    
    // Handle EverDrive registers
    if (cartAddr >= EVERDRIVE_START && cartAddr < EVERDRIVE_END) {
        Log::debugf("PIInterface: DMA Read from EverDrive registers (ignored)");
        CompleteDMA();
        return;
    }
    
    // Handle Standard ROM
    if (cartAddr >= ROM_START && cartAddr <= ROM_END) {
        PerformStandardDMARead(dramAddr, cartAddr, length);
        return;
    }
    
    // Handle 64DD IPL ROM
    if (cartAddr >= DD_IPL_ROM_START && cartAddr <= DD_IPL_ROM_END) {
        PerformDDIPLDMARead(dramAddr, cartAddr, length);
        return;
    }
    
    // Handle SRAM/FlashRAM
    if (cartAddr >= SRAM_START && cartAddr < SRAM_END) {
        if (m_uncached_rom.DMARead(dramAddr, cartAddr, length)) {
            m_stats.bytes_read += length;
            CompleteDMA();  // Fire PI interrupt (UncachedRom only does data transfer)
            return;
        }
    }
    
    Log::warningf("PIInterface: DMA Read from unhandled address 0x%08X", cartAddr);
    CompleteDMA();
}

// ============================================================================
// DMA Write (RDRAM -> Cart)
// ============================================================================

void PeripheralInterfaceHandler::HandleDMAWrite()
{
    if (!m_dma_used) {
        m_dma_used = true;
        OnFirstDMA();
    }
    
    uint32_t dramAddr = m_pi_regs.PI_DRAM_ADDR_REG & 0x7FFFFE;
    uint32_t cartAddr = m_pi_regs.PI_CART_ADDR_REG;
    uint32_t length = (m_pi_regs.PI_RD_LEN_REG + 1);  // Use PI_RD_LEN (RDRAM->Cart)
    
    Log::debugf("PIInterface: DMA Write - RDRAM=0x%08X Cart=0x%08X Length=0x%X",
        dramAddr, cartAddr, length);
    
    m_stats.dma_writes++;
    
    // Handle 64DD MSEQ
    if (cartAddr >= DD_MSEQ_START && cartAddr <= DD_MSEQ_END) {
        Log::debugf("PIInterface: DMA Write to 64DD MSEQ (ignored)");
        CompleteDMA();
        return;
    }
    
    // Handle SRAM/FlashRAM DMA
    if (cartAddr >= SRAM_START && cartAddr <= SRAM_END) {
        if (m_uncached_rom.DMAWrite(dramAddr, cartAddr, length)) {
            m_stats.bytes_written += length;
            CompleteDMA();  // Fire PI interrupt (UncachedRom only does data transfer)
            return;
        }
    }
    
    // Handle ROM Writes (RDRAM -> ROM) - e.g. for patching or 64DD
    if (cartAddr >= ROM_START && cartAddr <= ROM_END && m_config.allow_rom_writes) {
        Log::infof("PIInterface: DMA Write to ROM area");
        
        N64Rom* rom = m_system.GetRom();
        if (!rom) {
            Log::errorf("PIInterface: No ROM available for DMA write");
            CompleteDMA();
            return;
        }
        
        uint8_t* romData = rom->GetRomAddress();
        uint32_t romSize = static_cast<uint32_t>(rom->GetSize());
        uint8_t* rdram = m_mmu.Rdram();
        
        uint32_t romOffset = cartAddr - ROM_START;
        uint32_t copyLen = static_cast<uint32_t>(std::min(static_cast<size_t>(length), static_cast<size_t>(romSize - romOffset)));
        
        // Copy from RDRAM to ROM
        for (uint32_t i = 0; i < copyLen; i++) {
            romData[((romOffset + i) ^ 3)] = rdram[((dramAddr + i) ^ 3)];
        }
        
        m_stats.bytes_written += copyLen;
        CompleteDMA();
        return;
    }
    
    // Handle 64DD buffers/sectors
    if (cartAddr >= DD_C2_BUFFER_START && cartAddr <= DD_USER_SECTOR_END) {
        Log::debugf("PIInterface: DMA Write to 64DD buffer");
        // 64DD timing: ~63 cycles per 25 bytes
        uint32_t cycles = (length * 63) / 25;
        m_pi_regs.PI_STATUS_REG |= STATUS_DMA_BUSY;
        m_system.GetSystemTimer()->set_timer(N64System::TimerType::DDPiTimer, cycles, false);
        CompleteDMA();
        return;
    }
    
    Log::warningf("PIInterface: DMA Write to unhandled address 0x%08X", cartAddr);
    CompleteDMA();
}

// ============================================================================
// Standard DMA Read Implementation (Cart -> RDRAM)
// ============================================================================

void PeripheralInterfaceHandler::PerformStandardDMARead(uint32_t dramAddr, uint32_t cartAddr, uint32_t length)
{
    uint8_t* rdram = m_mmu.Rdram();
    uint32_t rdramSize = m_mmu.RdramSize();
    
    // Save original DRAM start address for recompiler cache invalidation.
    // dramAddr is modified during the copy loop below, so we need to
    // remember where the transfer started.
    const uint32_t dramStart = dramAddr;
    
    // Complex block-based DMA algorithm matching N64 hardware behavior
    m_pi_regs.PI_WR_LEN_REG = length <= 8 ? 0x7F - (dramAddr & 7) : 0x7F;
    
    uint8_t block[128];
    bool firstBlock = true;
    uint32_t transferLen = 0;
    int32_t maxBlockSize = 128;
    
    while (length > 0) {
        int32_t blockAlign = dramAddr & 7;
        int32_t blockSize = maxBlockSize - blockAlign;
        int32_t blockLen = blockSize;
        
        if (length < static_cast<uint32_t>(blockLen)) {
            blockLen = length;
        }
        
        int32_t endOfRow = 0x800 - (dramAddr & 0x7FF);
        if (endOfRow < blockLen) {
            blockLen = endOfRow;
        }
        
        length -= blockLen;
        if (length < 0) {
            length = 0;
        }
        
        int32_t readLen = (blockLen + 1) & ~1;
        ReadBlock(cartAddr, block, readLen);
        cartAddr += readLen;
        
        if (firstBlock) {
            if (blockLen == blockSize - 1) {
                blockLen += 1;
            }
            blockLen = blockLen - blockAlign;
            if (blockLen < 0) {
                blockLen = 0;
            }
        } else {
            blockLen = readLen;
        }
        
        if ((dramAddr + blockLen) >= rdramSize) {
            blockLen = rdramSize - dramAddr;
            if (blockLen < 0) {
                blockLen = 0;
            }
        }
        
        for (int32_t i = 0; i < blockLen; i++) {
            rdram[(dramAddr + i) ^ 3] = block[i];
        }
        
        dramAddr = (dramAddr + blockLen + 7) & ~7;
        transferLen += (blockLen + 7) & ~7;
        maxBlockSize = endOfRow < 8 ? 128 - blockAlign : 128;
        firstBlock = false;
    }
    
    m_stats.bytes_read += transferLen;

    // Invalidate any recompiler blocks that overlap this DMA target range
    // so stale compiled code is discarded after ROM-to-RDRAM copies.
    // Use the saved start address, NOT dramAddr (which has been advanced
    // past the end of the transfer by the copy loop above).
    m_system.NotifyDMAToRDRAM(dramStart, transferLen);

    // Complete DMA immediately — data is already copied to RDRAM above.
    // Many N64 emulators complete PI DMA instantly; delayed timing via PiTimer
    // requires careful timer bookkeeping that can cause DMA_BUSY to hang.
    // Timer-based completion could be revisited once the timer system
    // properly handles mid-period timer insertions.
    CompleteDMA();
}

// ============================================================================
// 64DD IPL ROM DMA Read
// ============================================================================

void PeripheralInterfaceHandler::PerformDDIPLDMARead(uint32_t dramAddr, uint32_t cartAddr, uint32_t length)
{
    Log::debugf("PIInterface: DMA Read from 64DD IPL ROM");
    
    N64Rom* ddRom = m_system.GetDDRom();
    if (!ddRom) {
        Log::warningf("PIInterface: No 64DD ROM available");
        CompleteDMA();
        return;
    }
    
    uint8_t* rdram = m_mmu.Rdram();
    uint8_t* romData = ddRom->GetRomAddress();
    uint32_t romSize = static_cast<uint32_t>(ddRom->GetSize());
    uint32_t romOffset = cartAddr - DD_IPL_ROM_START;
    
    uint32_t copyLen = static_cast<uint32_t>(std::min(static_cast<size_t>(length), static_cast<size_t>(romSize - romOffset)));
    for (uint32_t i = 0; i < copyLen; i++) {
        // ROM data is in LE word order; ^3 on ROM read converts to BE byte stream,
        // ^3 on RDRAM write converts back to LE word order
        rdram[((dramAddr + i) ^ 3)] = romData[(romOffset + i) ^ 3];
    }
    
    m_stats.bytes_read += copyLen;
    
    // 64DD IPL ROM has fast access, immediate interrupt
    m_pi_regs.PI_STATUS_REG |= STATUS_INTERRUPT;
    Log::debugf("PIInterface: 64DD IPL ROM DMA complete (%u bytes)", copyLen);
    
    CompleteDMA();
}

// ============================================================================
// Read Block Helper
// ============================================================================

void PeripheralInterfaceHandler::ReadBlock(uint32_t addr, uint8_t* block, uint32_t blockLen)
{
    N64Rom* rom = m_system.GetRom();
    N64Rom* ddRom = m_system.GetDDRom();
    
    if (addr >= ROM_START && addr <= ROM_END && rom) {
        // ROM data is in host (LE) word order after DetectFormatAndByteSwap.
        // DMA sends bytes in N64 big-endian order, so we use ^3 to convert
        // from LE word order back to BE byte order.
        // The RDRAM write (^3) then converts back to LE word order.
        uint8_t* romData = rom->GetRomAddress();
        uint32_t romSize = static_cast<uint32_t>(rom->GetSize());
        uint32_t romOffset = addr - ROM_START;
        
        for (uint32_t i = 0; i < blockLen; i++) {
            uint32_t pos = romOffset + i;
            block[i] = pos < romSize ? romData[pos ^ 3] : 0;
        }
    }
    else if (addr >= DD_C2_BUFFER_START && addr <= DD_C2_BUFFER_END) {
        // 64DD C2 buffer - return zeros
        std::memset(block, 0, blockLen);
    }
    else if (addr >= DD_USER_SECTOR_START && addr <= DD_USER_SECTOR_END) {
        // 64DD user sector
        N64Disk* disk = m_system.GetDisk();
        if (disk && disk->IsLoaded()) {
            const uint8_t* diskBuf = disk->GetImage().Data();
            uint32_t diskOffset = addr - DD_USER_SECTOR_START;
            for (uint32_t i = 0; i < blockLen; i++) {
                block[i] = diskBuf[(diskOffset + i) ^ 3];
            }
        } else {
            std::memset(block, 0, blockLen);
        }
    }
    else if (addr >= DD_IPL_ROM_START && addr <= DD_IPL_ROM_END && ddRom) {
        // 64DD IPL ROM - also in LE word order, use ^3 for BE byte stream
        uint8_t* romData = ddRom->GetRomAddress();
        uint32_t romSize = static_cast<uint32_t>(ddRom->GetSize());
        uint32_t romOffset = addr - DD_IPL_ROM_START;
        
        for (uint32_t i = 0; i < blockLen; i++) {
            uint32_t pos = romOffset + i;
            block[i] = pos < romSize ? romData[pos ^ 3] : 0;
        }
    }
    else {
        Log::warningf("PIInterface: ReadBlock from unhandled address 0x%08X", addr);
        std::memset(block, 0, blockLen);
    }
}

// ============================================================================
// DMA Completion
// ============================================================================

void PeripheralInterfaceHandler::CompleteDMA()
{
    m_pi_regs.PI_STATUS_REG &= ~STATUS_DMA_BUSY;
    m_pi_regs.PI_STATUS_REG |= STATUS_INTERRUPT;
    
    // Set MI interrupt
    m_registers.m_Mips_Interface[2] |= MI_INTR_PI;
    m_registers.CheckInterrupts();
    
    m_stats.interrupt_count++;
    
    Log::debugf("PIInterface: DMA Complete");
}

// ============================================================================
// First DMA Handler (CIC chip initialization)
// ============================================================================

void PeripheralInterfaceHandler::OnFirstDMA()
{
    Log::infof("PIInterface: First DMA - Initializing CIC chip emulation");
    
    N64Rom* rom = m_system.GetRom();
    if (!rom) {
        return;
    }
    
    // Get offset based on CIC chip
    uint32_t offset = 0x0318;  // Default for most CIC chips
    
    switch (rom->GetImage().GetCICChip()) {
        case N64::Rom::CICChip::CIC_NUS_6105:
            offset = 0x03F0;
            break;
        default:
            offset = 0x0318;
            break;
    }
    
    // Write RDRAM size to boot location
    uint32_t rdramSize = m_mmu.RdramSize();
    m_mmu.SW_Memory(0xFFFFFFFF80000000ULL + offset, rdramSize);
    
    Log::debugf("PIInterface: Wrote RDRAM size (0x%08X) to offset 0x%X", 
        rdramSize, offset);
}

// ============================================================================
// Reset
// ============================================================================

void PeripheralInterfaceHandler::reset()
{
    Log::infof("PIInterface: Reset");
    
    // Clear all registers
    // Clear PI registers
    m_pi_registers_array.fill(0);
    
    // Set initial values
    m_pi_regs.PI_RD_LEN_REG = 0x7F;
    m_pi_regs.PI_WR_LEN_REG = 0x7F;
    m_pi_regs.PI_STATUS_REG = 0;
    
    // Default bus timing
    m_pi_regs.PI_DOMAIN1_REG = 0x40;  // ROM domain
    m_pi_regs.PI_DOMAIN2_REG = 0x40;  // SRAM domain
    
    m_dma_used = false;
    m_first_dma = true;
}

} // namespace PI
