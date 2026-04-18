/**
 * @file UnCachedRom.cpp
 * @brief Uncached ROM and save memory handler implementation
 */

#include "UnCachedRom.hpp"
#include "../MainSYS.hpp"
#include "../N64Rom.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../../common/log.hpp"
#include <cstring>
#include <sstream>

/**
 * @brief Constructor
 */
UncachedRom::UncachedRom(
    CN64System& system,
    CRegisters& registers,
    Nova64::MipsMemoryVM& mmu,
    const Config& config)
    : m_system(system)
    , m_registers(registers)
    , m_mmu(mmu)
    , m_config(config)
    , m_sram(std::make_unique<CSram>(config.saves_read_only))
    , m_flash_ram(std::make_unique<CFlashRam>(config.saves_read_only))
{
}

/**
 * @brief Read 32-bit value from uncached ROM region
 */
bool UncachedRom::Read32(uint32_t address, uint32_t& value)
{
    m_stats.read_count++;
    
    MemoryRegion region = decodeRegion(address);
    
    switch (region)
    {
    case MemoryRegion::SRAM:
        return handleSramAccess(address, value, false);
        
    case MemoryRegion::FLASH_RAM:
        return handleFlashRamAccess(address, value, false);
        
    case MemoryRegion::PI_REGS:
        return handlePiRegisters(address, value, false);
        
    case MemoryRegion::DD_REGS:
        return handleDDInterface(address, value, false);
        
    case MemoryRegion::ROM:
    case MemoryRegion::DD_ROM:
    {
        // Read actual ROM data
        N64Rom* rom = m_system.GetRom();
        if (rom && rom->IsLoaded()) {
            uint32_t offset = address & 0x0FFFFFFF;  // Strip ROM base (0x10000000)
            const uint8_t* romData = rom->GetRomAddress();
            uint32_t romSize = static_cast<uint32_t>(rom->GetSize());
            if (offset + 4 <= romSize) {
                // ROM data is in host (LE) word order after DetectFormatAndByteSwap
                value = *reinterpret_cast<const uint32_t*>(romData + offset);
                return true;
            }
        }
        // Fallback: open bus
        value = (address & 0xFFFF) | ((address & 0xFFFF) << 16);
        return true;
    }
        
    case MemoryRegion::UNMAPPED:
    default:
        value = 0;
        return false;
    }
}

/**
 * @brief Write 32-bit value to uncached ROM region
 */
bool UncachedRom::Write32(uint32_t address, uint32_t value, uint32_t mask)
{
    m_stats.write_count++;
    value &= mask;
    
    MemoryRegion region = decodeRegion(address);
    
    switch (region)
    {
    case MemoryRegion::SRAM:
        {
            uint32_t dummy = value;
            return handleSramAccess(address, dummy, true);
        }
    case MemoryRegion::FLASH_RAM:
        {
            uint32_t dummy = value;
            return handleFlashRamAccess(address, dummy, true);
        }
    case MemoryRegion::PI_REGS:
        {
            uint32_t dummy = value;
            return handlePiRegisters(address, dummy, true);
        }
    case MemoryRegion::DD_REGS:
        {
            uint32_t dummy = value;
            return handleDDInterface(address, dummy, true);
        }
    case MemoryRegion::ROM:
    case MemoryRegion::DD_ROM:
        return true;
    case MemoryRegion::UNMAPPED:
    default:
        return false;
    }
}

/**
 * @brief Perform DMA read operation
 */
bool UncachedRom::DMARead(uint32_t dramAddr, uint32_t cartAddr, uint32_t length)
{
    m_stats.dma_operations++;
    
    if (m_config.enable_dma_logging)
    {
        Log::debugf("UncachedRom::DMARead - Cart:0x%08X -> DRAM:0x%08X, Length:%u",
                   cartAddr, dramAddr, length);
    }
    
    startDMARead(cartAddr, dramAddr, length);
    processDMATransfer();
    
    return true;
}

/**
 * @brief Perform DMA write operation
 */
bool UncachedRom::DMAWrite(uint32_t dramAddr, uint32_t cartAddr, uint32_t length)
{
    m_stats.dma_operations++;
    
    if (m_config.enable_dma_logging)
    {
        Log::debugf("UncachedRom::DMAWrite - DRAM:0x%08X -> Cart:0x%08X, Length:%u",
                   dramAddr, cartAddr, length);
    }
    
    startDMAWrite(dramAddr, cartAddr, length);
    processDMATransfer();
    return true;
}

/**
 * @brief Reset handler state
 */
void UncachedRom::reset()
{
    m_dma_state = DMAState{};
    m_dma_start_cycle = 0;
    m_dma_complete_cycle = 0;
    m_stats = Stats{};
}

/**
 * @brief Save state to stream
 */
bool UncachedRom::saveState(std::ostream& os) const
{
    os.write(reinterpret_cast<const char*>(&m_dma_state), sizeof(m_dma_state));
    os.write(reinterpret_cast<const char*>(&m_dma_start_cycle), sizeof(m_dma_start_cycle));
    os.write(reinterpret_cast<const char*>(&m_dma_complete_cycle), sizeof(m_dma_complete_cycle));
    return os.good();
}

/**
 * @brief Load state from stream
 */
bool UncachedRom::loadState(std::istream& is)
{
    is.read(reinterpret_cast<char*>(&m_dma_state), sizeof(m_dma_state));
    is.read(reinterpret_cast<char*>(&m_dma_start_cycle), sizeof(m_dma_start_cycle));
    is.read(reinterpret_cast<char*>(&m_dma_complete_cycle), sizeof(m_dma_complete_cycle));
    return is.good();
}

/**
 * @brief Dump state for debugging
 */
std::string UncachedRom::dumpState() const
{
    std::ostringstream oss;
    oss << "UncachedRom State:\n";
    oss << "  DMA Active: " << (m_dma_state.is_active ? "Yes" : "No") << "\n";
    oss << "  Statistics:\n";
    oss << "    Reads: " << m_stats.read_count << "\n";
    oss << "    Writes: " << m_stats.write_count << "\n";
    oss << "    SRAM accesses: " << m_stats.sram_accesses << "\n";
    oss << "    FlashRAM accesses: " << m_stats.flashram_accesses << "\n";
    oss << "    DMA operations: " << m_stats.dma_operations << "\n";
    oss << "    DMA bytes: " << m_stats.dma_bytes_transferred << "\n";
    return oss.str();
}

/**
 * @brief Get memory map information
 */
std::string UncachedRom::getMemoryMap() const
{
    return "Uncached ROM: 0x10000000-0x1FBFFFFF\n"
           "  SRAM: 0x08000000-0x0800FFFF (via PI)\n"
           "  FlashRAM: 0x08000000-0x08010000 (via PI)\n";
}

// Private helper methods

UncachedRom::MemoryRegion UncachedRom::decodeRegion(uint32_t address) const
{
    if (address >= 0x10000000 && address < 0x1FC00000)
        return MemoryRegion::ROM;
    if (address >= 0x08000000 && address < 0x08020000)
    {
        // SRAM and FlashRAM share the same address range (0x08000000).
        // The FlashRAM command/status register is at 0x08000000 (same as SRAM data).
        // Key insight: SRAM games ONLY use DMA for save access, never I/O (Read32/Write32).
        // Any I/O access to this range is a FlashRAM command/status register access.
        // Always route I/O through FLASH_RAM to enable proper auto-detection,
        // even if processDMATransfer already guessed SRAM from an earlier DMA.
        return MemoryRegion::FLASH_RAM;
    }
    if (address >= 0x04600000 && address < 0x04700000)
        return MemoryRegion::PI_REGS;
    if (address >= 0x05000000 && address < 0x06000000)
        return MemoryRegion::DD_REGS;
    return MemoryRegion::UNMAPPED;
}

uint32_t UncachedRom::getRegionOffset(uint32_t address, MemoryRegion region) const
{
    switch (region)
    {
    case MemoryRegion::SRAM:
    case MemoryRegion::FLASH_RAM:
        return address & 0x1FFFF;
    case MemoryRegion::ROM:
        return address & 0x0FFFFFFF;
    case MemoryRegion::DD_ROM:
        return address & 0x03FFFFFF;
    case MemoryRegion::PI_REGS:
        return address & 0xFFFFF;
    case MemoryRegion::DD_REGS:
        return address & 0xFFFFFF;
    default:
        return 0;
    }
}

bool UncachedRom::handleSramAccess(uint32_t address, uint32_t& value, bool is_write)
{
    m_stats.sram_accesses++;
    if (!m_sram)
    {
        value = 0;
        return false;
    }
    value = 0;
    return !is_write;
}

bool UncachedRom::handleFlashRamAccess(uint32_t address, uint32_t& value, bool is_write)
{
    m_stats.flashram_accesses++;
    if (!m_flash_ram)
    {
        value = 0;
        return false;
    }
    
    // Auto-detect FlashRAM when a FlashRAM command/status access occurs.
    // Allow upgrading from SRAM too — an early DMA may have guessed SRAM,
    // but any I/O access to this range proves it's actually FlashRAM.
    if (m_system.m_SaveUsing != N64Types::SaveChipType::FlashRam)
    {
        m_system.m_SaveUsing = N64Types::SaveChipType::FlashRam;
        Log::infof("UncachedRom: Auto-detected FlashRAM (I/O access at 0x%08X)", address);
    }
    
    if (is_write)
    {
        m_flash_ram->WriteToFlashCommand(value);
        return true;
    }
    else
    {
        value = m_flash_ram->ReadFromFlashStatus(address);
        return true;
    }
}

bool UncachedRom::handlePiRegisters(uint32_t address, uint32_t& value, bool is_write)
{
    value = 0;
    return false;
}

bool UncachedRom::handleDDInterface(uint32_t address, uint32_t& value, bool is_write)
{
    value = 0;
    return false;
}

void UncachedRom::startDMARead(uint32_t source, uint32_t dest, uint32_t length)
{
    m_dma_state.is_active = true;
    m_dma_state.is_write = false;  // Cart → RDRAM (load)
    m_dma_state.source_address = source;
    m_dma_state.dest_address = dest;
    m_dma_state.length = length;
    m_dma_state.current_offset = 0;
}

void UncachedRom::startDMAWrite(uint32_t source, uint32_t dest, uint32_t length)
{
    m_dma_state.is_active = true;
    m_dma_state.is_write = true;  // RDRAM → Cart (save)
    m_dma_state.source_address = source;
    m_dma_state.dest_address = dest;
    m_dma_state.length = length;
    m_dma_state.current_offset = 0;
}

void UncachedRom::processDMATransfer()
{
    if (!m_dma_state.is_active)
        return;
    
    // For DMARead (Cart→RDRAM):  source=cartAddr, dest=dramAddr
    // For DMAWrite (RDRAM→Cart): source=dramAddr, dest=cartAddr
    uint32_t cart_addr, dram_addr;
    if (m_dma_state.is_write) {
        // RDRAM → Cart (save): source=dram, dest=cart
        dram_addr = m_dma_state.source_address;
        cart_addr = m_dma_state.dest_address;
    } else {
        // Cart → RDRAM (load): source=cart, dest=dram
        cart_addr = m_dma_state.source_address;
        dram_addr = m_dma_state.dest_address;
    }
    uint32_t length = m_dma_state.length;
    
    // Determine save handler based on game's save type
    // Both m_sram and m_flash_ram are always allocated; use m_SaveUsing to pick the right one.
    uint8_t* rdram = m_mmu.Rdram();
    bool handled = false;
    
    if (rdram && dram_addr < 0x00800000 && cart_addr >= 0x08000000 && cart_addr < 0x08020000)
    {
        auto saveType = m_system.m_SaveUsing;
        
        // Auto-detect: DMA without prior FlashRAM command writes defaults to SRAM.
        // FlashRAM games set m_SaveUsing via handleFlashRamAccess before DMA.
        if (saveType == N64Types::SaveChipType::Auto)
        {
            saveType = N64Types::SaveChipType::Sram;
            m_system.m_SaveUsing = saveType;
            Log::infof("UncachedRom: Auto-detected SRAM (DMA access without FlashRAM commands)");
        }
        
        if (saveType == N64Types::SaveChipType::FlashRam)
        {
            if (m_dma_state.is_write)
                m_flash_ram->DmaToFlashram(rdram + dram_addr, cart_addr & 0xFFFF, length);
            else
                m_flash_ram->DmaFromFlashram(rdram + dram_addr, cart_addr & 0xFFFF, length);
            m_stats.dma_bytes_transferred += length;
            handled = true;
        }
        else if (saveType == N64Types::SaveChipType::Sram)
        {
            if (m_dma_state.is_write)
                m_sram->DmaToSram(rdram + dram_addr, cart_addr & 0xFFFF, length);
            else
                m_sram->DmaFromSram(rdram + dram_addr, cart_addr & 0xFFFF, length);
            m_stats.dma_bytes_transferred += length;
            handled = true;
        }
    }
    
    completeDMA();
}

void UncachedRom::completeDMA()
{
    m_dma_state.is_active = false;
    m_dma_state.current_offset = 0;
    m_registers.m_Peripheral_Interface[PI::Register::PI_STATUS] &= ~0x3;
}