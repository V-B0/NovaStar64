/**
 * @file GBcart.hpp
 * @brief G Bcart declarations and types.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <memory>
#include <string>
#include <ctime>
#include <array>
#include <functional>

namespace GameBoy {

// Forward declarations
class Cartridge;

// Cartridge types (MBC chips)
enum class CartridgeType : uint8_t {
    ROM_ONLY = 0x00,
    MBC1 = 0x01,
    MBC1_RAM = 0x02,
    MBC1_RAM_BATTERY = 0x03,
    MBC2 = 0x05,
    MBC2_BATTERY = 0x06,
    ROM_RAM = 0x08,
    ROM_RAM_BATTERY = 0x09,
    MMM01 = 0x0B,
    MMM01_RAM = 0x0C,
    MMM01_RAM_BATTERY = 0x0D,
    MBC3_TIMER_BATTERY = 0x0F,
    MBC3_TIMER_RAM_BATTERY = 0x10,
    MBC3 = 0x11,
    MBC3_RAM = 0x12,
    MBC3_RAM_BATTERY = 0x13,
    MBC5 = 0x19,
    MBC5_RAM = 0x1A,
    MBC5_RAM_BATTERY = 0x1B,
    MBC5_RUMBLE = 0x1C,
    MBC5_RUMBLE_RAM = 0x1D,
    MBC5_RUMBLE_RAM_BATTERY = 0x1E,
    MBC6 = 0x20,
    MBC7_SENSOR_RUMBLE_RAM_BATTERY = 0x22,
    POCKET_CAMERA = 0xFC,
    BANDAI_TAMA5 = 0xFD,
    HUC3 = 0xFE,
    HUC1_RAM_BATTERY = 0xFF
};

// ROM sizes in bytes (from cartridge header)
enum class ROMSize : uint8_t {
    SIZE_32KB = 0x00,    // No ROM banking
    SIZE_64KB = 0x01,    // 4 banks
    SIZE_128KB = 0x02,   // 8 banks
    SIZE_256KB = 0x03,   // 16 banks
    SIZE_512KB = 0x04,   // 32 banks
    SIZE_1MB = 0x05,     // 64 banks (only 63 used by MBC1)
    SIZE_2MB = 0x06,     // 128 banks
    SIZE_4MB = 0x07,     // 256 banks
    SIZE_8MB = 0x08,     // 512 banks
    SIZE_1_1MB = 0x52,   // 72 banks
    SIZE_1_2MB = 0x53,   // 80 banks
    SIZE_1_5MB = 0x54    // 96 banks
};

// RAM sizes in bytes (from cartridge header)
enum class RAMSize : uint8_t {
    NONE = 0x00,
    SIZE_2KB = 0x01,     // Unused
    SIZE_8KB = 0x02,     // 1 bank
    SIZE_32KB = 0x03,    // 4 banks of 8KB each
    SIZE_128KB = 0x04,   // 16 banks of 8KB each
    SIZE_64KB = 0x05     // 8 banks of 8KB each
};

// Real-Time Clock registers (for MBC3)
struct RTCData {
    uint8_t seconds = 0;
    uint8_t minutes = 0;
    uint8_t hours = 0;
    uint8_t days_low = 0;
    uint8_t days_high = 0;  // Bit 0: Day counter MSB, Bit 6: Halt, Bit 7: Day counter carry
    
    // Constructor
    RTCData() = default;
    
    // Get current days (9-bit value)
    uint16_t get_days() const {
        return static_cast<uint16_t>(days_low) | 
               ((static_cast<uint16_t>(days_high) & 0x01) << 8);
    }
    
    // Set days (9-bit value)
    void set_days(uint16_t days) {
        days_low = static_cast<uint8_t>(days & 0xFF);
        days_high = (days_high & 0xFE) | static_cast<uint8_t>((days >> 8) & 0x01);
    }
    
    // Check if RTC is halted
    bool is_halted() const {
        return (days_high & 0x40) != 0;
    }
    
    // Set halt flag
    void set_halted(bool halted) {
        if (halted) {
            days_high |= 0x40;
        } else {
            days_high &= ~0x40;
        }
    }
    
    // Check for day counter carry
    bool has_carry() const {
        return (days_high & 0x80) != 0;
    }
    
    // Set carry flag
    void set_carry(bool carry) {
        if (carry) {
            days_high |= 0x80;
        } else {
            days_high &= ~0x80;
        }
    }
};

// Base class for Memory Bank Controller implementations
class MemoryBankController {
protected:
    Cartridge& cartridge;
    
public:
    MemoryBankController(Cartridge& cart) : cartridge(cart) {}
    virtual ~MemoryBankController() = default;
    
    virtual uint8_t read(uint16_t address) const = 0;
    virtual void write(uint16_t address, uint8_t value) = 0;
    virtual void save_state(std::vector<uint8_t>& buffer) const = 0;
    virtual void load_state(const std::vector<uint8_t>& buffer, size_t& offset) = 0;
    
    virtual void reset() = 0;
    virtual void update_rtc() {}  // Only for MBC3 with RTC
};

// MBC1 implementation
class MBC1Controller : public MemoryBankController {
private:
    bool ram_enabled = false;
    bool banking_mode = false;  // false = ROM banking, true = RAM banking
    
public:
    using MemoryBankController::MemoryBankController;
    
    uint8_t read(uint16_t address) const override;
    void write(uint16_t address, uint8_t value) override;
    void save_state(std::vector<uint8_t>& buffer) const override;
    void load_state(const std::vector<uint8_t>& buffer, size_t& offset) override;
    void reset() override;
};

// MBC3 implementation (with RTC support)
class MBC3Controller : public MemoryBankController {
private:
    bool ram_rtc_enabled = false;
    uint8_t rtc_register = 0;
    RTCData current_rtc;
    RTCData latched_rtc;
    time_t last_rtc_update = 0;
    bool rtc_latched = false;
    
    void update_rtc_time();
    
public:
    MBC3Controller(Cartridge& cart);
    
    uint8_t read(uint16_t address) const override;
    void write(uint16_t address, uint8_t value) override;
    void save_state(std::vector<uint8_t>& buffer) const override;
    void load_state(const std::vector<uint8_t>& buffer, size_t& offset) override;
    void reset() override;
    void update_rtc() override;
};

// MBC5 implementation
class MBC5Controller : public MemoryBankController {
private:
    bool ram_enabled = false;
    
public:
    using MemoryBankController::MemoryBankController;
    
    uint8_t read(uint16_t address) const override;
    void write(uint16_t address, uint8_t value) override;
    void save_state(std::vector<uint8_t>& buffer) const override;
    void load_state(const std::vector<uint8_t>& buffer, size_t& offset) override;
    void reset() override;
};

// Main cartridge class
class Cartridge {
private:
    // ROM data
    std::vector<uint8_t> rom_data;
    size_t rom_size = 0;
    
    // RAM data (for save games)
    std::vector<uint8_t> ram_data;
    size_t ram_size = 0;
    
    // Banking state
    uint16_t current_rom_bank = 1;  // Bank 0 is always fixed
    uint8_t current_ram_bank = 0;
    
    // Cartridge metadata
    CartridgeType type = CartridgeType::ROM_ONLY;
    std::string title;
    std::string game_code;
    bool supports_color = false;
    bool supports_super = false;
    std::string licensee_code;
    
    // MBC controller
    std::unique_ptr<MemoryBankController> mbc;
    
    // Battery backup
    bool has_battery = false;
    std::string save_file_path;
    
    // Helper methods
    void detect_cartridge_type();
    void parse_header();
    void create_mbc_controller();
    size_t get_ram_bank_count() const;
    
public:
    // Constructor/Destructor
    Cartridge();
    ~Cartridge();
    
    // No copying
    Cartridge(const Cartridge&) = delete;
    Cartridge& operator=(const Cartridge&) = delete;
    
    // Loading and saving
    bool load_from_file(const std::string& file_path);
    bool load_from_data(const std::vector<uint8_t>& data);
    bool save_ram_to_file() const;
    bool load_ram_from_file();
    
    // Memory access
    uint8_t read(uint16_t address) const;
    void write(uint16_t address, uint8_t value);
    
    // State management
    std::vector<uint8_t> save_state() const;
    bool load_state(const std::vector<uint8_t>& state);
    
    // Update (for RTC, etc.)
    void update();
    
    // Information getters
    const std::string& get_title() const { return title; }
    CartridgeType get_type() const { return type; }
    size_t get_rom_size() const { return rom_size; }
    size_t get_ram_size() const { return ram_size; }
    bool has_rtc() const;
    bool has_rumble() const;
    
    // Battery functions
    void set_save_file_path(const std::string& path) { save_file_path = path; }
    bool has_battery_backup() const { return has_battery; }
    
    // Debug info
    std::string get_info_string() const;
    
private:
    // Friend classes for MBC controllers to access private members
    friend class MBC1Controller;
    friend class MBC3Controller;
    friend class MBC5Controller;
    
    // Direct access for MBC controllers
    uint8_t* get_rom_pointer() { return rom_data.data(); }
    const uint8_t* get_rom_pointer() const { return rom_data.data(); }
    size_t get_rom_bank_size() const { return 0x4000; }  // 16KB per bank
    
    uint8_t* get_ram_pointer() { return ram_data.data(); }
    const uint8_t* get_ram_pointer() const { return ram_data.data(); }
    size_t get_ram_bank_size() const { return 0x2000; }  // 8KB per bank
    
    void set_rom_bank(uint16_t bank);
    void set_ram_bank(uint8_t bank);
    uint16_t get_rom_bank() const { return current_rom_bank; }
    uint8_t get_ram_bank() const { return current_ram_bank; }

    uint8_t read_rom_bank(uint16_t bank, uint16_t offset) const;
    uint8_t read_ram_bank(uint8_t bank, uint16_t offset) const;
    void write_ram_bank(uint8_t bank, uint16_t offset, uint8_t value);
};

// Utility functions
namespace CartridgeUtils {
    constexpr size_t rom_size_from_code(uint8_t code) {
        switch (code) {
            case 0x00: return 32 * 1024;     // 32KB
            case 0x01: return 64 * 1024;     // 64KB
            case 0x02: return 128 * 1024;    // 128KB
            case 0x03: return 256 * 1024;    // 256KB
            case 0x04: return 512 * 1024;    // 512KB
            case 0x05: return 1024 * 1024;   // 1MB
            case 0x06: return 2048 * 1024;   // 2MB
            case 0x07: return 4096 * 1024;   // 4MB
            case 0x08: return 8192 * 1024;   // 8MB
            case 0x52: return 1152 * 1024;   // 1.125MB
            case 0x53: return 1280 * 1024;   // 1.25MB
            case 0x54: return 1536 * 1024;   // 1.5MB
            default: return 32 * 1024;       // Default to 32KB
        }
    }
    
    constexpr size_t ram_size_from_code(uint8_t code) {
        switch (code) {
            case 0x00: return 0;             // No RAM
            case 0x01: return 2 * 1024;      // 2KB
            case 0x02: return 8 * 1024;      // 8KB
            case 0x03: return 32 * 1024;     // 32KB
            case 0x04: return 128 * 1024;    // 128KB
            case 0x05: return 64 * 1024;     // 64KB
            default: return 0;               // Default to no RAM
        }
    }
    
    constexpr bool cartridge_has_battery(CartridgeType type) {
        return type == CartridgeType::MBC1_RAM_BATTERY ||
               type == CartridgeType::MBC2_BATTERY ||
               type == CartridgeType::ROM_RAM_BATTERY ||
               type == CartridgeType::MMM01_RAM_BATTERY ||
               type == CartridgeType::MBC3_TIMER_BATTERY ||
               type == CartridgeType::MBC3_TIMER_RAM_BATTERY ||
               type == CartridgeType::MBC3_RAM_BATTERY ||
               type == CartridgeType::MBC5_RAM_BATTERY ||
               type == CartridgeType::MBC5_RUMBLE_RAM_BATTERY ||
               type == CartridgeType::MBC7_SENSOR_RUMBLE_RAM_BATTERY ||
               type == CartridgeType::HUC1_RAM_BATTERY;
    }
    
    constexpr bool cartridge_has_rtc(CartridgeType type) {
        return type == CartridgeType::MBC3_TIMER_BATTERY ||
               type == CartridgeType::MBC3_TIMER_RAM_BATTERY;
    }
    
    constexpr bool cartridge_has_rumble(CartridgeType type) {
        return type == CartridgeType::MBC5_RUMBLE ||
               type == CartridgeType::MBC5_RUMBLE_RAM ||
               type == CartridgeType::MBC5_RUMBLE_RAM_BATTERY ||
               type == CartridgeType::MBC7_SENSOR_RUMBLE_RAM_BATTERY;
    }
}

} // namespace GameBoy

// Legacy C-style interface (for compatibility)
extern "C" {
    struct gb_cart;
    
    bool init_gb_cart(struct gb_cart* gb_cart, const char* gb_file);
    void release_gb_cart(struct gb_cart* gb_cart);
    void save_gb_cart(struct gb_cart* gb_cart);
    void read_gb_cart(struct gb_cart* gb_cart, uint16_t address, uint8_t* data);
    void write_gb_cart(struct gb_cart* gb_cart, uint16_t address, const uint8_t* data);
}