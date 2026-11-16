/**
 * @file RomMemHandler.hpp
 * @brief ROM memory handler declarations: ROM regions, mirroring and access modes, write patches and the read latch.
 */
#pragma once
#include "Memoryhandler.hpp"
#include "../../common/log.hpp"
#include "../../common/Span.hpp"
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <functional>

class CRegisters;
namespace N64 {
namespace Rom {
class N64Rom;
}
} // namespace N64
class CN64System;

namespace ROM {
// ROM memory regions
enum class Region : uint32_t {
    GAME_ROM_START = 0x10000000,
    GAME_ROM_END = 0x1FBFFFFF,
    PIF_ROM_START = 0x1FC00000,
    PIF_ROM_END = 0x1FC007BF,
    DD_ROM_START = 0x06000000, // 64DD ROM
    DD_ROM_END = 0x06FFFFFF
};

// ROM mirroring modes
enum class MirrorMode {
    NONE,        // No mirroring, read 0 beyond ROM
    MIRROR_32MB, // Mirror every 32MB
    MIRROR_64MB, // Mirror every 64MB
    WRAP_AROUND  // Wrap around ROM size
};

// ROM access types
enum class AccessType {
    READ_ONLY,     // Standard ROM (most games)
    WRITE_THROUGH, // Writes allowed (development/debug)
    PATCHABLE,     // Writes create temporary patches
    DECAYING       // Writes decay over time (emulation quirk)
};
} // namespace ROM

class RomMemoryHandler final : public MemoryHandler {
public:
    struct Config {
        Config() noexcept {} // explicit ctor: avoids Clang NSDMI-in-default-arg error
        ROM::AccessType access_type = ROM::AccessType::READ_ONLY;
        ROM::MirrorMode mirror_mode = ROM::MirrorMode::NONE;
        bool enable_write_logging = false;
        bool enable_rom_decay = true;
        bool enable_cheat_engine = false;
        uint32_t decay_interval_ms = 1000; // ROM writes decay after 1 second
    };

    explicit RomMemoryHandler(CN64System& system, CRegisters& registers, N64::Rom::N64Rom& rom,
                              const Config& config = Config());

    ~RomMemoryHandler() noexcept override;

    // MemoryHandler interface
    [[nodiscard]] bool Read32(uint32_t address, uint32_t& value) override;
    [[nodiscard]] bool Write32(uint32_t address, uint32_t value, uint32_t mask) override;

    [[nodiscard]] bool Read8(uint32_t address, uint8_t& value) override;
    [[nodiscard]] bool Read16(uint32_t address, uint16_t& value);
    [[nodiscard]] bool ReadBytes(uint32_t address, void* buffer, size_t size);

    // ROM decay management
    void rom_write_decayed();

    // Cheat/patch system
    struct Patch {
        uint32_t address;
        uint32_t original_value;
        uint32_t patched_value;
        uint64_t decay_timestamp; // When patch expires (0 = permanent)
        bool enabled;
    };

    void clear_all_patches();

    // State management
    void reset();
    void save_state(std::ostream& os) const;
    void load_state(std::istream& is);

    // Statistics
    struct Statistics {
        uint64_t rom_reads = 0;
        uint64_t rom_writes = 0;
        uint64_t patch_applications = 0;
        uint64_t patch_decays = 0;
        uint64_t mirror_accesses = 0;
        uint64_t out_of_bounds_accesses = 0;
    };
    [[nodiscard]] const Statistics& get_statistics() const { return m_stats; }
    void reset_statistics() { m_stats = {}; }

    // Callbacks
    using WriteCallback = std::function<void(uint32_t address, uint32_t value)>;

private:
    RomMemoryHandler() = delete;
    RomMemoryHandler(const RomMemoryHandler&) = delete;
    RomMemoryHandler(RomMemoryHandler&&) = delete;
    RomMemoryHandler& operator=(const RomMemoryHandler&) = delete;
    RomMemoryHandler& operator=(RomMemoryHandler&&) = delete;

    CN64System& m_system;
    CRegisters& m_registers;
    N64::Rom::N64Rom& m_rom;
    Config m_config;

    // ROM data access
    Common::Span<const uint8_t> m_rom_data;
    size_t m_rom_size;
    uint32_t m_rom_mask; // For mirroring calculations

    // Temporary patches/writes
    struct PatchEntry {
        uint32_t address;
        uint32_t value;
        uint64_t expiration_time; // 0 = never expires
        bool operator<(const PatchEntry& other) const { return address < other.address; }
    };

    std::vector<PatchEntry> m_active_patches;
    std::vector<PatchEntry> m_decayed_patches; // For debugging

    // Statistics
    Statistics m_stats;

    // Callbacks
    WriteCallback m_write_callback;

    // Timing
    uint64_t m_last_decay_check = 0;

    // Latch emulation
    uint32_t m_latch_value = 0;
    bool m_latch_active = false;
    uint64_t m_latch_decay_time = 0;

    // Helper methods
    [[nodiscard]] bool is_rom_address(uint32_t address) const;
    [[nodiscard]] ROM::Region get_rom_region(uint32_t address) const;
    [[nodiscard]] uint32_t translate_rom_address(uint32_t address) const;
    [[nodiscard]] uint32_t apply_mirroring(uint32_t address) const;

    // Patch management
    [[nodiscard]] std::optional<uint32_t> get_patch_value(uint32_t address) const;
    void apply_patch_internal(uint32_t address, uint32_t value, uint64_t duration_ms);
    void remove_expired_patches(uint64_t current_time);

    // ROM read implementations
    [[nodiscard]] uint32_t read_rom_32(uint32_t address) const;
    [[nodiscard]] uint8_t read_rom_8(uint32_t address) const;

    // Endianness handling (N64 is big-endian)
    static constexpr uint32_t swap_bytes_32(uint32_t value) {
        return ((value & 0x000000FF) << 24) | ((value & 0x0000FF00) << 8) | ((value & 0x00FF0000) >> 8) |
               ((value & 0xFF000000) >> 24);
    }

    static constexpr uint16_t swap_bytes_16(uint16_t value) {
        return ((value & 0x00FF) << 8) | ((value & 0xFF00) >> 8);
    }

    // Logging
    void log_rom_access(uint32_t address, uint32_t value, bool is_write) const;

    // System event handlers
    void on_system_reset();
    void on_game_state_loaded();
};
