/**
 * @file UnCachedSysMemory.hpp
 * @brief Uncached System Memory Handler - Save Memory Interface
 * 
 * Handles uncached memory accesses to save memory (SRAM/FlashRAM).
 * Memory region: 0x08000000-0x0FFFFFFF (Cartridge Domain 2, Address 2)
 */

#pragma once
#include "Memoryhandler.hpp"
#include <cstdint>
#include "../Savetypes/SRam.hpp"
#include "../Savetypes/FRam.hpp"

// Forward declarations
class CN64System;
class CRegisters;
namespace Nova64 { class MipsMemoryVM; }

/**
 * @class UnCachedSysMemory
 * @brief Memory handler for uncached save memory region
 * 
 * Implements save memory (SRAM/FlashRAM) access including:
 * - SRAM read/write operations (0x08000000-0x08088000)
 * - FlashRAM status and command interface (0x08000000-0x08010000)
 * - PI DMA operations for save memory
 * - Automatic save chip type detection
 * - Read-only mode support
 */
class UnCachedSysMemory : public MemoryHandler
{
public:
    /**
     * @brief Constructor
     * @param system Reference to N64 system
     * @param reg Register interface
     * @param mmu Memory management unit
     * @param savesReadOnly If true, prevent writes to save memory
     */
    UnCachedSysMemory(CN64System& system, CRegisters& reg, Nova64::MipsMemoryVM& mmu, bool savesReadOnly);
    
    /**
     * @brief Read 32-bit value from save memory
     * @param addr Physical address (0x08000000-0x0FFFFFFF)
     * @param val Output value
     * @return true on success
     */
    bool Read32(uint32_t addr, uint32_t& val) override;
    
    /**
     * @brief Write 32-bit value to save memory
     * @param addr Physical address
     * @param val Value to write
     * @param mask Write mask
     * @return true on success
     */
    bool Write32(uint32_t addr, uint32_t val, uint32_t mask) override;
    
    /**
     * @brief Perform PI DMA read (save memory to RDRAM)
     * @return true on success
     */
    bool DMARead();
    
    /**
     * @brief Perform PI DMA write (RDRAM to save memory)
     */
    void DMAWrite();
    
    /**
     * @brief Get SRAM interface
     * @return Reference to SRAM handler
     */
    CSram& GetSram();
    
    /**
     * @brief Get FlashRAM interface
     * @return Reference to FlashRAM handler
     */
    CFlashRam& GetFlashRam();

private:
    // Save type detection
    void AutoDetectSaveType(uint32_t offset);
    
    // SRAM access
    uint32_t ReadFromSRAM(uint32_t offset);
    void WriteToSRAM(uint32_t offset, uint32_t value, uint32_t mask);
    
    // FlashRAM access
    uint32_t ReadFromFlashRAM(uint32_t addr);
    void WriteToFlashRAM(uint32_t value);
    
    // DMA completion
    void CompleteDMA(bool error);
    
    // Delete copy operations
    UnCachedSysMemory(const UnCachedSysMemory&) = delete;
    UnCachedSysMemory& operator=(const UnCachedSysMemory&) = delete;

    CN64System& m_System;        ///< N64 system reference
    CRegisters& m_Reg;           ///< Register interface
    Nova64::MipsMemoryVM& m_MMU;        ///< Memory management unit
    CSram m_Sram;                ///< SRAM handler
    CFlashRam m_FlashRam;        ///< FlashRAM handler
    bool m_SavesReadOnly;        ///< Read-only mode flag
};