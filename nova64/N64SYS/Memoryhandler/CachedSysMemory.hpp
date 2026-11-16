/**
 * @file CachedSysMemory.hpp
 * @brief 64DD Disk Interface (ASIC) Register Handler
 * 
 * Handles cached memory accesses to the 64DD disk drive interface.
 * Memory region: 0x05000000-0x05FFFFFF (Cartridge Domain 2, Address 1)
 */

#pragma once
#include "Memoryhandler.hpp"
#include <cstdint>
#include <array>

class CRegisters;
namespace N64 {
namespace Rom {
class N64Rom;
}
} // namespace N64

/**
 * @class CachedSysMemory
 * @brief Memory handler for 64DD disk interface ASIC registers
 * 
 * Implements the 64DD disk drive hardware interface including:
 * - ASIC register access (22 registers at 0x05000500-0x0500054C)
 * - Disk command processing (seek, read, write, etc.)
 * - Buffer manager control for DMA operations
 * - Sequencer control for multi-sector operations
 * - Disk position tracking and status reporting
 * - C2 buffer management (32KB sector cache)
 */
class CachedSysMemory : public MemoryHandler {
public:
    CachedSysMemory(CRegisters& reg, N64::Rom::N64Rom& ddRom);

    /**
     * @brief Read 32-bit value from ASIC register
     * @param addr Physical address (0x05000000-0x05FFFFFF)
     * @param val Output value
     * @return true on success
     */
    bool Read32(uint32_t addr, uint32_t& val) override;

    /**
     * @brief Write 32-bit value to ASIC register
     * @param addr Physical address
     * @param val Value to write
     * @param mask Write mask
     * @return true on success
     */
    bool Write32(uint32_t addr, uint32_t val, uint32_t mask) override;

private:
    // Initialization
    void InitializeDiskInterface();

    // Command processing
    void ProcessDiskCommand();
    void CommandSeek();
    void CommandRead();
    void CommandWrite();
    void CommandRezero();
    void CommandSleep();
    void CommandClearChange();
    void CommandClearReset();
    void CommandReadID();
    void CompleteCommand(bool error);

    // Buffer manager
    void ProcessBufferManagerControl();

    // Sequencer
    void ProcessSequencerControl();

    // Disk position
    void UpdateDiskPosition();

    // Reset
    void PerformHardReset();

    // DMA operations
    void PerformDMARead(uint32_t rdramAddr, uint32_t length, uint32_t sectorOffset);
    void PerformDMAWrite(uint32_t rdramAddr, uint32_t length, uint32_t sectorOffset);
    void LoadCurrentSectorToC2Buffer();
    void FlushC2BufferToSector();

    CRegisters& m_Reg;         ///< Register interface
    N64::Rom::N64Rom& m_DDRom; ///< 64DD ROM interface
    uint32_t* m_DiskInterface; ///< Pointer to 22 ASIC registers

    // Disk state
    bool m_DiskEnabled;       ///< 64DD hardware enabled (IPL ROM loaded)
    bool m_DiskPresent;       ///< Disk present in drive
    uint32_t m_CurrentTrack;  ///< Current head track position
    uint32_t m_CurrentSector; ///< Current sector under head
    bool m_CommandActive;     ///< Command in progress

    // Sequencer state
    bool m_SequencerActive;        ///< Multi-sector operation active
    uint32_t m_SequencerRemaining; ///< Remaining sectors in sequence

    // C2 Buffer (32KB sector cache for disk I/O)
    std::array<uint8_t, 32768> m_C2Buffer;

    // RDRAM access for DMA
    uint8_t* m_RDRAM;     ///< Pointer to RDRAM for DMA operations
    uint32_t m_RDRAMSize; ///< RDRAM size in bytes
};
