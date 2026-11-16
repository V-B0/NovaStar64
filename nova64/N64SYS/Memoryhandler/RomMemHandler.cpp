/**
 * @file RomMemHandler.cpp
 * @brief ROM memory handler: cartridge ROM reads (8, 16, 32-bit and bulk), the read latch and temporary ROM write patches.
 */
#include "RomMemHandler.hpp"
#include "../N64Rom.hpp"
#include "../MainSYS.hpp"
#include "../MIPSY/Register.hpp"
#include "PIInterFace.hpp"
#include <cstring>
#include <algorithm>
#include <sstream>

using namespace ROM;

RomMemoryHandler::RomMemoryHandler(CN64System& system, CRegisters& registers, N64::Rom::N64Rom& rom,
                                   const Config& config) :
    m_system(system), m_registers(registers), m_rom(rom), m_config(config) {
    if (m_rom.IsLoaded()) {
        const uint8_t* data = m_rom.GetRomAddress();
        size_t size = m_rom.GetSize();
        // Since span requires non-null, ensure data is valid or size is 0
        if (data && size > 0) {
            m_rom_data = Common::Span<const uint8_t>(data, size);
            m_rom_mask = static_cast<uint32_t>(size - 1); // size - 1; only a valid mask for power-of-two sizes
            m_rom_size = size;
        } else {
            m_rom_size = 0;
            m_rom_mask = 0;
        }
    } else {
        m_rom_size = 0;
        m_rom_mask = 0;
    }
}

RomMemoryHandler::~RomMemoryHandler() noexcept = default;

// --- Memory Handler Implementation ---

bool RomMemoryHandler::Read32(uint32_t address, uint32_t& value) {
    if (m_latch_active) {
        value = m_latch_value;
        rom_write_decayed();
        return true;
    }

    if (is_rom_address(address)) {
        if (address < 0x06000000) {
            uint32_t& cart_reg = m_registers.m_Peripheral_Interface[PI::Register::PI_CART_ADDR];
            cart_reg = (address + 4) & ~1;
        }

        value = read_rom_32(address);

        if (m_config.enable_write_logging && (address & 0x1FFFFFFF) < 0x40) {
            log_rom_access(address, value, false);
        }

        m_stats.rom_reads++;
        return true;
    }

    value = (address << 16) | (address & 0xFFFF);
    m_stats.out_of_bounds_accesses++;
    return true;
}

bool RomMemoryHandler::Write32(uint32_t address, uint32_t value, uint32_t mask) {
    if (!m_latch_active) {
        m_latch_active = true;
        m_latch_value = (value & mask);

        uint32_t& pi_status = m_registers.m_Peripheral_Interface[PI::Register::PI_STATUS];
        pi_status |= static_cast<uint32_t>(PI::Status::IO_BUSY);
    }

    m_stats.rom_writes++;
    return true;
}

void RomMemoryHandler::rom_write_decayed() {
    m_latch_active = false;

    uint32_t& pi_status = m_registers.m_Peripheral_Interface[PI::Register::PI_STATUS];
    pi_status &= ~static_cast<uint32_t>(PI::Status::IO_BUSY);
}

bool RomMemoryHandler::is_rom_address(uint32_t address) const {
    return (address >= static_cast<uint32_t>(Region::GAME_ROM_START));
}

uint32_t RomMemoryHandler::read_rom_32(uint32_t address) const {
    uint32_t offset = address & 0x0FFFFFFF;

    if (offset + 4 <= m_rom_size) {
        // ROM data is in host (LE) word order after DetectFormatAndByteSwap.
        // Read as native uint32_t to get the correct N64 value.
        return *reinterpret_cast<const uint32_t*>(&m_rom_data[offset]);
    }

    return 0;
}

bool RomMemoryHandler::Read8(uint32_t address, uint8_t& value) {
    if (m_latch_active) {
        // Latching usually only returns 32-bit word, but for byte access:
        value = static_cast<uint8_t>(m_latch_value >> 24); // the latched word's most significant byte
        // Behavior undefined in P64 snippet, but standard rules apply.
        rom_write_decayed();
        return true;
    }
    if (is_rom_address(address)) {
        value = read_rom_8(address);
        m_stats.rom_reads++;
        return true;
    }
    return false;
}

bool RomMemoryHandler::ReadBytes(uint32_t address, void* buffer, size_t size) {
    if (!buffer) return false;
    // Bulk reads ignore the latch.

    if (is_rom_address(address)) {
        uint32_t offset = address & 0x0FFFFFFF;
        if (offset + size <= m_rom_size) {
            std::memcpy(buffer, &m_rom_data[offset], size);
            m_stats.rom_reads += (size / 4) + 1;
            return true;
        }
    }
    return false;
}

uint8_t RomMemoryHandler::read_rom_8(uint32_t address) const {
    uint32_t offset = address & 0x0FFFFFFF;
    if (offset < m_rom_size) {
        // ROM data is in LE word order; use ^3 for byte access
        return m_rom_data[offset ^ 3];
    }
    return 0;
}

void RomMemoryHandler::clear_all_patches() { m_active_patches.clear(); }

void RomMemoryHandler::reset() {
    rom_write_decayed();
    clear_all_patches();
    m_stats = {};
}

void RomMemoryHandler::on_system_reset() { reset(); }
void RomMemoryHandler::on_game_state_loaded() { reset(); }

void RomMemoryHandler::save_state(std::ostream& os) const {
    os.write(reinterpret_cast<const char*>(&m_latch_active), sizeof(m_latch_active));
    os.write(reinterpret_cast<const char*>(&m_latch_value), sizeof(m_latch_value));

    size_t patch_count = m_active_patches.size();
    os.write(reinterpret_cast<const char*>(&patch_count), sizeof(patch_count));
    if (patch_count > 0) {
        os.write(reinterpret_cast<const char*>(m_active_patches.data()), sizeof(PatchEntry) * patch_count);
    }
}

void RomMemoryHandler::load_state(std::istream& is) {
    is.read(reinterpret_cast<char*>(&m_latch_active), sizeof(m_latch_active));
    is.read(reinterpret_cast<char*>(&m_latch_value), sizeof(m_latch_value));

    size_t patch_count = 0;
    is.read(reinterpret_cast<char*>(&patch_count), sizeof(patch_count));

    m_active_patches.resize(patch_count);
    if (patch_count > 0) {
        is.read(reinterpret_cast<char*>(m_active_patches.data()), sizeof(PatchEntry) * patch_count);
    }
}

void RomMemoryHandler::log_rom_access(uint32_t address, uint32_t value, bool is_write) const {
    Log::debugf("ROM 0x%08X: %08X (%s)", address, value, is_write ? "W" : "R");
}
