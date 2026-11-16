/**
 * @file UnCachedSysMemory.cpp
 * @brief Uncached System Memory Handler - Save Memory (SRAM/FlashRAM) Interface
 * 
 * Handles uncached memory accesses to save memory regions.
 * Memory region: 0x08000000-0x0FFFFFFF (Cartridge Domain 2, Address 2)
 * 
 * This region is used for:
 * - SRAM access (0x08000000-0x08088000) - 512KB+32KB
 * - FlashRAM access (0x08000000-0x08010000) - 128KB
 * - Save chip auto-detection
 */

#include "UnCachedSysMemory.hpp"
#include "../MainSYS.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../Savetypes/SRam.hpp"
#include "../Savetypes/FRam.hpp"
#include "../../common/log.hpp"
#include <cstring>

// Memory region constants
namespace SaveMemory {
constexpr uint32_t REGION_BASE = 0x08000000; // Cartridge domain 2, address 2
constexpr uint32_t REGION_MASK = 0x1FFFFFFF; // Physical address mask

// SRAM constants
constexpr uint32_t SRAM_MAX_OFFSET = 0x88000;

// FlashRAM constants
constexpr uint32_t FLASHRAM_MAX_OFFSET = 0x10000;

// Auto-detection threshold
constexpr uint32_t AUTO_DETECT_THRESHOLD = 0x88000;
} // namespace SaveMemory

// ============================================================================
// Constructor/Destructor
// ============================================================================

UnCachedSysMemory::UnCachedSysMemory(CN64System& system, CRegisters& reg, Nova64::MipsMemoryVM& mmu,
                                     bool savesReadOnly) :
    m_System(system), m_Reg(reg), m_MMU(mmu), m_Sram(savesReadOnly), m_FlashRam(savesReadOnly),
    m_SavesReadOnly(savesReadOnly) {
    Log::infof("UnCachedSysMemory: Initialized (saves %s)", savesReadOnly ? "read-only" : "read-write");
}

// ============================================================================
// Read Operations
// ============================================================================

bool UnCachedSysMemory::Read32(uint32_t addr, uint32_t& val) {
    // Calculate offset from save memory base
    uint32_t offset = (addr & SaveMemory::REGION_MASK) - SaveMemory::REGION_BASE;

    // Auto-detect save chip type on first access
    if (m_System.m_SaveUsing == N64Types::SaveChipType::Auto) {
        AutoDetectSaveType(offset);
    }

    // Check if offset is within valid range
    if (offset > SaveMemory::SRAM_MAX_OFFSET) {
        // Beyond valid save memory range - return open bus
        val = ((offset & 0xFFFF) << 16) | (offset & 0xFFFF);
        Log::warningf("UnCachedSysMemory: Read beyond save range at offset 0x%X", offset);
        return true;
    }

    // Handle read based on save chip type
    N64Types::SaveChipType saveType = m_System.m_SaveUsing;

    switch (saveType) {
    case N64Types::SaveChipType::Sram: val = ReadFromSRAM(offset); break;

    case N64Types::SaveChipType::FlashRam: val = ReadFromFlashRAM(addr & SaveMemory::REGION_MASK); break;

    default:
        // Unknown save type - return open bus
        val = ((addr & 0xFFFF) << 16) | (addr & 0xFFFF);
        Log::warningf("UnCachedSysMemory: Read with unknown save type at 0x%08X", addr);
        break;
    }

    return true;
}

// ============================================================================
// Write Operations
// ============================================================================

bool UnCachedSysMemory::Write32(uint32_t addr, uint32_t val, uint32_t mask) {
    // Calculate offset from save memory base
    uint32_t offset = (addr & SaveMemory::REGION_MASK) - SaveMemory::REGION_BASE;

    // Auto-detect save chip type on first access
    if (m_System.m_SaveUsing == N64Types::SaveChipType::Auto) {
        AutoDetectSaveType(offset);
    }

    // Check if offset is within valid range
    if (offset > SaveMemory::FLASHRAM_MAX_OFFSET) {
        // Beyond FlashRAM range - ignore write
        return true;
    }

    // Check if saves are read-only
    if (m_SavesReadOnly) {
        Log::debugf("UnCachedSysMemory: Write blocked (read-only mode) at 0x%08X", addr);
        return true;
    }

    // Handle write based on save chip type
    N64Types::SaveChipType saveType = m_System.m_SaveUsing;

    switch (saveType) {
    case N64Types::SaveChipType::Sram:
        if (offset < SaveMemory::SRAM_MAX_OFFSET) {
            WriteToSRAM(offset, val, mask);
        }
        break;

    case N64Types::SaveChipType::FlashRam: WriteToFlashRAM(val); break;

    default: Log::warningf("UnCachedSysMemory: Write with unknown save type at 0x%08X", addr); break;
    }

    return true;
}

// ============================================================================
// DMA Operations
// ============================================================================

bool UnCachedSysMemory::DMARead() {
    // PI DMA: Read from save memory to RDRAM
    // PI_CART_ADDR -> PI_DRAM_ADDR

    uint32_t cartAddr = m_Reg.m_Peripheral_Interface[3];                    // PI_CART_ADDR_REG
    uint32_t dramAddr = m_Reg.m_Peripheral_Interface[0];                    // PI_DRAM_ADDR_REG
    uint32_t length = ((m_Reg.m_Peripheral_Interface[2]) & 0x00FFFFFF) + 1; // PI_RD_LEN_REG

    // Align length to 2 bytes
    if ((length & 1) != 0) {
        length += 1;
    }

    Log::debugf("UnCachedSysMemory: DMA Read - Cart[0x%08X] -> RDRAM[0x%08X], length=0x%X", cartAddr, dramAddr, length);

    uint32_t offset = (cartAddr & SaveMemory::REGION_MASK) - SaveMemory::REGION_BASE;

    // Auto-detect save chip type
    if (m_System.m_SaveUsing == N64Types::SaveChipType::Auto) {
        AutoDetectSaveType(offset);
    }

    uint8_t* rdram = m_MMU.Rdram();

    if (!rdram) {
        Log::errorf("UnCachedSysMemory: DMA read failed - RDRAM not initialized");
        CompleteDMA(true);
        return false;
    }

    // Perform DMA based on save type
    N64Types::SaveChipType saveType = m_System.m_SaveUsing;

    if (saveType == N64Types::SaveChipType::Sram) {
        m_Sram.DmaFromSram(rdram + dramAddr, offset, length);
    } else if (saveType == N64Types::SaveChipType::FlashRam) {
        m_FlashRam.DmaFromFlashram(rdram + dramAddr, offset, length);
    } else {
        Log::errorf("UnCachedSysMemory: DMA read failed - unknown save type");
        CompleteDMA(true);
        return false;
    }

    // Complete DMA with interrupt
    CompleteDMA(false);
    return true;
}

void UnCachedSysMemory::DMAWrite() {
    // PI DMA: Write from RDRAM to save memory
    // PI_DRAM_ADDR -> PI_CART_ADDR

    uint32_t cartAddr = m_Reg.m_Peripheral_Interface[3];                    // PI_CART_ADDR_REG
    uint32_t dramAddr = m_Reg.m_Peripheral_Interface[0];                    // PI_DRAM_ADDR_REG
    uint32_t length = ((m_Reg.m_Peripheral_Interface[2]) & 0x00FFFFFE) + 2; // PI_WR_LEN_REG

    Log::debugf("UnCachedSysMemory: DMA Write - RDRAM[0x%08X] -> Cart[0x%08X], length=0x%X", dramAddr, cartAddr,
                length);

    uint32_t offset = (cartAddr & SaveMemory::REGION_MASK) - SaveMemory::REGION_BASE;

    // Auto-detect save chip type
    if (m_System.m_SaveUsing == N64Types::SaveChipType::Auto) {
        AutoDetectSaveType(offset);
    }

    // Check if saves are read-only
    if (m_SavesReadOnly) {
        Log::warningf("UnCachedSysMemory: DMA write blocked (read-only mode)");
        CompleteDMA(false);
        return;
    }

    uint8_t* rdram = m_MMU.Rdram();

    if (!rdram) {
        Log::errorf("UnCachedSysMemory: DMA write failed - RDRAM not initialized");
        CompleteDMA(true);
        return;
    }

    // Perform DMA based on save type
    N64Types::SaveChipType saveType = m_System.m_SaveUsing;

    if (saveType == N64Types::SaveChipType::Sram) {
        m_Sram.DmaToSram(rdram + dramAddr, offset, length);
    } else if (saveType == N64Types::SaveChipType::FlashRam) {
        m_FlashRam.DmaToFlashram(rdram + dramAddr, offset, length);
    } else {
        Log::errorf("UnCachedSysMemory: DMA write failed - unknown save type");
        CompleteDMA(true);
        return;
    }

    // Complete DMA with interrupt
    CompleteDMA(false);
}

// ============================================================================
// Save Chip Type Detection
// ============================================================================

void UnCachedSysMemory::AutoDetectSaveType(uint32_t offset) {
    // Auto-detect save chip based on access pattern
    // - SRAM: typically accessed at offsets > 0x10000
    // - FlashRAM: typically accessed at offsets < 0x10000

    N64Types::SaveChipType detectedType;

    if (offset >= SaveMemory::AUTO_DETECT_THRESHOLD) {
        // Large offset suggests SRAM
        detectedType = N64Types::SaveChipType::Sram;
    } else {
        // Small offset suggests FlashRAM
        detectedType = N64Types::SaveChipType::FlashRam;
    }

    m_System.m_SaveUsing = detectedType;

    Log::infof("UnCachedSysMemory: Auto-detected save type: %s (offset=0x%X)",
               (detectedType == N64Types::SaveChipType::Sram) ? "SRAM" : "FlashRAM", offset);
}

// ============================================================================
// SRAM Access
// ============================================================================

uint32_t UnCachedSysMemory::ReadFromSRAM(uint32_t offset) {
    if (offset >= SaveMemory::SRAM_MAX_OFFSET) {
        Log::warningf("UnCachedSysMemory: SRAM read beyond range at offset 0x%X", offset);
        return ((offset & 0xFFFF) << 16) | (offset & 0xFFFF);
    }

    // Read 4 bytes from SRAM
    uint8_t data[4] = {0};
    m_Sram.DmaFromSram(data, offset, 4);

    // Convert to 32-bit value (little-endian byte order)
    uint32_t value = (data[3] << 24) | (data[2] << 16) | (data[1] << 8) | data[0];

    Log::debugf("UnCachedSysMemory: SRAM read at offset 0x%X = 0x%08X", offset, value);

    return value;
}

void UnCachedSysMemory::WriteToSRAM(uint32_t offset, uint32_t value, uint32_t mask) {
    if (offset >= SaveMemory::SRAM_MAX_OFFSET) {
        Log::warningf("UnCachedSysMemory: SRAM write beyond range at offset 0x%X", offset);
        return;
    }

    // Convert 32-bit value to bytes (little-endian)
    uint8_t data[4];
    data[0] = static_cast<uint8_t>(value & 0xFF);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data[3] = static_cast<uint8_t>((value >> 24) & 0xFF);

    // Apply mask if needed (for partial writes)
    if (mask != 0xFFFFFFFF) {
        uint8_t existingData[4];
        m_Sram.DmaFromSram(existingData, offset, 4);

        // Apply mask per byte
        for (int i = 0; i < 4; i++) {
            uint8_t byteMask = static_cast<uint8_t>((mask >> (i * 8)) & 0xFF);
            data[i] = (data[i] & byteMask) | (existingData[i] & ~byteMask);
        }
    }

    // Write 4 bytes to SRAM
    m_Sram.DmaToSram(data, offset, 4);

    Log::debugf("UnCachedSysMemory: SRAM write at offset 0x%X = 0x%08X", offset, value);
}

// ============================================================================
// FlashRAM Access
// ============================================================================

uint32_t UnCachedSysMemory::ReadFromFlashRAM(uint32_t addr) {
    // Read FlashRAM status register
    uint32_t value = m_FlashRam.ReadFromFlashStatus(addr);

    Log::debugf("UnCachedSysMemory: FlashRAM read at 0x%08X = 0x%08X", addr, value);

    return value;
}

void UnCachedSysMemory::WriteToFlashRAM(uint32_t value) {
    // Write FlashRAM command
    m_FlashRam.WriteToFlashCommand(value);

    Log::debugf("UnCachedSysMemory: FlashRAM command = 0x%08X", value);
}

// ============================================================================
// DMA Completion
// ============================================================================

void UnCachedSysMemory::CompleteDMA(bool error) {
    // Clear DMA busy flag
    m_Reg.m_Peripheral_Interface[0] &= ~0x01; // PI_STATUS_REG &= ~PI_STATUS_DMA_BUSY

    if (error) {
        // Set error flag
        m_Reg.m_Peripheral_Interface[0] |= 0x04; // PI_STATUS_REG |= PI_STATUS_ERROR
        Log::errorf("UnCachedSysMemory: DMA completed with error");
    } else {
        // Set interrupt flag
        m_Reg.m_Peripheral_Interface[0] |= 0x08; // PI_STATUS_REG |= PI_STATUS_INTERRUPT
    }

    // Trigger PI interrupt
    m_Reg.m_Mips_Interface[2] |= 0x10; // MI_INTR_REG |= MI_INTR_PI
    m_Reg.CheckInterrupts();
}

// ============================================================================
// Save Access
// ============================================================================

CSram& UnCachedSysMemory::GetSram() { return m_Sram; }

CFlashRam& UnCachedSysMemory::GetFlashRam() { return m_FlashRam; }
