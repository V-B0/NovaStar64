/**
 * @file RomMemHandler.cpp
 * @brief Rom Mem Handler implementation.
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
    // Initialize ROM data span
    if (m_rom.IsLoaded()) {
        const uint8_t* data = m_rom.GetRomAddress();
        size_t size = m_rom.GetSize();
        // Since span requires non-null, ensure data is valid or size is 0
        if (data && size > 0) {
            m_rom_data = Common::Span<const uint8_t>(data, size);
            m_rom_mask = static_cast<uint32_t>(size - 1); // Simple mask for PoT sizes?
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

// Implement other interface methods (read8, read16, bytes...)
bool RomMemoryHandler::Read8(uint32_t address, uint8_t& value) {
    if (m_latch_active) {
        // Latching usually only returns 32-bit word, but for byte access:
        value = static_cast<uint8_t>(m_latch_value >> 24); // Return MSB? Or irrelevant?
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
    // Check latch? Usually bulk read ignores latch.

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

uint16_t RomMemoryHandler::read_rom_16(uint32_t address) const {
    uint32_t offset = address & 0x0FFFFFFF;
    if (offset + 2 <= m_rom_size) {
        // ROM data is in LE word order; use ^2 for halfword access
        return *reinterpret_cast<const uint16_t*>(&m_rom_data[offset ^ 2]);
    }
    return 0;
}

uint8_t RomMemoryHandler::read_rom_8(uint32_t address) const {
    uint32_t offset = address & 0x0FFFFFFF;
    if (offset < m_rom_size) {
        // ROM data is in LE word order; use ^3 for byte access
        return m_rom_data[offset ^ 3];
    }
    return 0;
}

bool RomMemoryHandler::apply_patch(uint32_t address, uint32_t value, uint32_t decay_ms) {
    if (!is_rom_address(address)) return false;

    // Add or update patch
    auto it = std::find_if(m_active_patches.begin(), m_active_patches.end(),
                           [address](const PatchEntry& p) { return p.address == address; });

    if (it != m_active_patches.end()) {
        it->value = value;
        // Update expiration if needed (using current time + decay_ms)
        // Ignoring decay logic for simple P64 emulation unless detailed.
    } else {
        m_active_patches.push_back({address, value, 0});
        // Sort for faster lookup if used in read
        std::sort(m_active_patches.begin(), m_active_patches.end());
    }

    m_stats.patch_applications++;
    return true;
}

bool RomMemoryHandler::remove_patch(uint32_t address) {
    auto it = std::remove_if(m_active_patches.begin(), m_active_patches.end(),
                             [address](const PatchEntry& p) { return p.address == address; });

    if (it != m_active_patches.end()) {
        m_active_patches.erase(it, m_active_patches.end());
        return true;
    }
    return false;
}

void RomMemoryHandler::clear_all_patches() { m_active_patches.clear(); }

std::vector<RomMemoryHandler::Patch> RomMemoryHandler::get_active_patches() const {
    std::vector<Patch> result;
    for (const auto& p : m_active_patches) {
        result.push_back({p.address, 0, p.value, p.expiration_time, true});
    }
    return result;
}

size_t RomMemoryHandler::get_active_patch_count() const { return m_active_patches.size(); }

void RomMemoryHandler::force_decay_all() {
    clear_all_patches();
    rom_write_decayed();
}

void RomMemoryHandler::reset() {
    rom_write_decayed();
    clear_all_patches();
    m_stats = {};
}

void RomMemoryHandler::on_system_reset() { reset(); } // Alias
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

std::string RomMemoryHandler::get_rom_info() const {
    std::stringstream ss;
    ss << "ROM Loaded: " << (m_rom_size / 1024 / 1024) << "MB"
       << "\nPatches: " << m_active_patches.size();
    return ss.str();
}

std::string RomMemoryHandler::get_memory_map() const { return "0x10000000 - 0x1FBFFFFF: Cartridge ROM"; }
std::string RomMemoryHandler::get_patch_info() const {
    std::stringstream ss;
    ss << "Active Patches: " << m_active_patches.size();
    return ss.str();
}

void RomMemoryHandler::set_write_callback(WriteCallback callback) { m_write_callback = callback; }

void RomMemoryHandler::log_rom_access(uint32_t address, uint32_t value, bool is_write) const {
    Log::debugf("ROM 0x%08X: %08X (%s)", address, value, is_write ? "W" : "R");
}
void RomMemoryHandler::log_patch_event(uint32_t address, uint32_t value, bool applied) const {
    Log::infof("Patch %s at 0x%08X: %08X", applied ? "Applied" : "Removed", address, value);
}
