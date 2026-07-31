/**
 * @file UnCachedRom.hpp
 * @brief Un Cached Rom declarations and types.
 */
#pragma once
#include "Memoryhandler.hpp"
#include "../Savetypes/SRam.hpp"
#include "../Savetypes/FRam.hpp"

class CN64System;
namespace Nova64 { class MipsMemoryVM; }
class CRegisters;

class UncachedRom final : 
    public MemoryHandler
{
public:
    struct Config {
            Config() noexcept {}  // explicit ctor: avoids Clang NSDMI-in-default-arg error
        bool saves_read_only = false;
        bool enable_dma_logging = false;
        bool enable_save_auto_detect = true;
        std::string save_file_path;
    };
    
    explicit UncachedRom(
        CN64System& system,
        CRegisters& registers,
        Nova64::MipsMemoryVM& mmu,
        const Config& config = Config());
    ~UncachedRom() override = default;
    
    // MemoryHandler interface
    [[nodiscard]] bool Read32(uint32_t address, uint32_t& value) override;
    [[nodiscard]] bool Write32(uint32_t address, uint32_t value, uint32_t mask) override;
    
    // DMA operations
    [[nodiscard]] bool DMARead(uint32_t dramAddr, uint32_t cartAddr, uint32_t length);
    bool DMAWrite(uint32_t dramAddr, uint32_t cartAddr, uint32_t length);
    
    // Save memory access
    CSram& getSram() { return *m_sram; }
    const CSram& getSram() const { return *m_sram; }
    
    CFlashRam& getFlashRam() { return *m_flash_ram; }
    const CFlashRam& getFlashRam() const { return *m_flash_ram; }
    
    // Configuration
    void setSavesReadOnly(bool read_only) { m_config.saves_read_only = read_only; }
    bool isSavesReadOnly() const { return m_config.saves_read_only; }
    
    // State management
    void reset();
    bool saveState(std::ostream& os) const;
    bool loadState(std::istream& is);
    
    // Debug
    std::string dumpState() const;
    std::string getMemoryMap() const;

private:
    
    UncachedRom() = delete;
    UncachedRom(const UncachedRom&) = delete;
    UncachedRom(UncachedRom&&) = delete;
    UncachedRom& operator=(const UncachedRom&) = delete;
    UncachedRom& operator=(UncachedRom&&) = delete;
    
    
    CN64System& m_system;
    CRegisters& m_registers;
    Nova64::MipsMemoryVM& m_mmu;
    Config m_config;
    
    
    std::unique_ptr<CSram> m_sram;
    std::unique_ptr<CFlashRam> m_flash_ram;
    
    
    struct DMAState {
        bool is_active = false;
        bool is_write = false;          // true = RDRAM→Cart (save), false = Cart→RDRAM (load)
        uint32_t source_address = 0;
        uint32_t dest_address = 0;
        uint32_t length = 0;
        uint32_t current_offset = 0;
    } m_dma_state;
    
    
    enum class MemoryRegion {
        SRAM,
        FLASH_RAM,
        ROM,
        DD_ROM,
        DD_REGS,
        PI_REGS,
        UNMAPPED
    };
    
    MemoryRegion decodeRegion(uint32_t address) const;
    uint32_t getRegionOffset(uint32_t address, MemoryRegion region) const;
    
    bool handleSramAccess(uint32_t address, uint32_t& value, bool is_write);
    bool handleFlashRamAccess(uint32_t address, uint32_t& value, bool is_write);
    bool handlePiRegisters(uint32_t address, uint32_t& value, bool is_write);
    bool handleDDInterface(uint32_t address, uint32_t& value, bool is_write);
    
    
    void startDMARead(uint32_t source, uint32_t dest, uint32_t length);
    void startDMAWrite(uint32_t source, uint32_t dest, uint32_t length);
    void processDMATransfer();
    void completeDMA();
    
    
    uint64_t m_dma_start_cycle = 0;
    uint64_t m_dma_complete_cycle = 0;
    
    
    struct Stats {
        uint64_t read_count = 0;
        uint64_t write_count = 0;
        uint64_t sram_accesses = 0;
        uint64_t flashram_accesses = 0;
        uint64_t dma_operations = 0;
        uint64_t dma_bytes_transferred = 0;
    } m_stats;
    
    // Cart ROM write latch (VR4300 PI bus behavior)
    uint32_t m_rom_latch_value = 0;
    bool     m_rom_latch_active = false;
    int32_t  m_rom_latch_timer = 0;  // m_NextTimer snapshot at write time
    
public:
    [[nodiscard]] bool IsLatchActive() const noexcept { return m_rom_latch_active; }
};