#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <optional>
#include <functional>
#include <vector>
#include <array>

// Forward declaration of GameBoy cartridge class
namespace GameBoy {
    class Cartridge;
}

namespace N64Controller {

// Transfer Pak memory regions
enum class TransferPakRegion : uint8_t {
    Registers = 0x00,      // Transfer Pak control registers
    GameBoyROM = 0x80,     // Game Boy ROM area
    GameBoyRAM = 0xA0,     // Game Boy RAM area
    Unused = 0xFF          // Unused/Invalid area
};

// Transfer Pak access modes
enum class TransferPakAccessMode : uint8_t {
    NotInserted = 0x40,    // No cartridge inserted
    Mode0 = 0x80,          // Access mode 0 (16-bit mode)
    Mode1 = 0x89           // Access mode 1 (8-bit mode)
};

// Transfer Pak banks (0x0000-0x7FFF is fixed, 0x8000-0xFFFF is banked)
enum class TransferPakBank : uint8_t {
    Bank0 = 0,   // Fixed bank (0x0000-0x3FFF)
    Bank1 = 1,   // Switchable bank (0x4000-0x7FFF)
    Bank2 = 2,   // RAM bank (if present)
    Bank3 = 3    // Unused/Reserved
};

// Transfer Pak status
struct TransferPakStatus {
    bool enabled = false;               // Whether Transfer Pak is enabled
    bool cartridge_inserted = false;    // Whether a Game Boy cartridge is inserted
    bool power_on = false;              // Whether power is supplied to cartridge
    bool access_mode_changed = false;   // Whether access mode just changed
    TransferPakAccessMode access_mode = TransferPakAccessMode::NotInserted;
    TransferPakBank current_bank = TransferPakBank::Bank0;
    
    // Convert to byte representation for register reading
    uint8_t to_register_value() const {
        uint8_t value = static_cast<uint8_t>(access_mode);
        if (!enabled) value = 0x00;
        if (!cartridge_inserted) value = 0x40;
        if (!power_on) value &= ~0x80;  // Clear power bit if not powered
        return value;
    }
    
    // Update from register write
    void from_register_value(uint8_t value) {
        access_mode_changed = (value != static_cast<uint8_t>(access_mode));
        
        switch (value) {
            case 0x00:      // Disabled
                enabled = false;
                power_on = false;
                access_mode = TransferPakAccessMode::NotInserted;
                break;
            case 0x40:      // No cartridge
                enabled = true;
                power_on = false;
                cartridge_inserted = false;
                access_mode = TransferPakAccessMode::NotInserted;
                break;
            case 0x80:      // Mode 0, powered
                enabled = true;
                power_on = true;
                cartridge_inserted = true;
                access_mode = TransferPakAccessMode::Mode0;
                break;
            case 0x89:      // Mode 1, powered
                enabled = true;
                power_on = true;
                cartridge_inserted = true;
                access_mode = TransferPakAccessMode::Mode1;
                break;
            default:
                // Invalid mode, disable
                enabled = false;
                power_on = false;
                access_mode = TransferPakAccessMode::NotInserted;
                break;
        }
    }
};

// Transfer Pak register addresses
namespace TransferPakRegisters {
    constexpr uint16_t ENABLE_REGISTER = 0x8000;    // Transfer Pak enable/access mode
    constexpr uint16_t BANK_REGISTER = 0x8001;      // Current bank selection
    constexpr uint16_t POWER_REGISTER = 0x8002;     // Cartridge power control
    constexpr uint16_t STATUS_REGISTER = 0x8003;    // Status register (read-only)
    
    constexpr uint16_t REGISTER_START = 0x8000;
    constexpr uint16_t REGISTER_END = 0x8003;
    constexpr uint16_t REGISTER_SIZE = REGISTER_END - REGISTER_START + 1;
}

// Main Transfer Pak controller class
class TransferPakController {
public:
    // Constructor/Destructor
    TransferPakController();
    ~TransferPakController();
    
    // No copying
    TransferPakController(const TransferPakController&) = delete;
    TransferPakController& operator=(const TransferPakController&) = delete;
    
    // Allow moving
    TransferPakController(TransferPakController&&) noexcept = default;
    TransferPakController& operator=(TransferPakController&&) noexcept = default;
    
    // Initialization and cleanup
    bool initialize();
    void release();
    void reset();
    
    // Cartridge management
    bool insert_cartridge(const std::string& rom_path);
    bool insert_cartridge(std::unique_ptr<GameBoy::Cartridge> cartridge);
    void remove_cartridge();
    bool save_cartridge_ram();
    
    // Memory access
    bool read(uint16_t address, uint8_t& data) const;
    bool write(uint16_t address, uint8_t data);
    
    bool read_buffer(uint16_t address, uint8_t* buffer, size_t size) const;
    bool write_buffer(uint16_t address, const uint8_t* buffer, size_t size);
    
    // Status and information
    const TransferPakStatus& get_status() const { return status; }
    bool is_enabled() const { return status.enabled; }
    bool is_cartridge_inserted() const { return status.cartridge_inserted; }
    TransferPakAccessMode get_access_mode() const { return status.access_mode; }
    
    // Cartridge information
    std::optional<std::string> get_cartridge_title() const;
    bool has_cartridge_battery() const;
    size_t get_cartridge_rom_size() const;
    size_t get_cartridge_ram_size() const;
    
    // Bank operations
    void set_bank(uint8_t bank);
    uint8_t get_bank() const { return static_cast<uint8_t>(status.current_bank); }
    
    // Power management
    void set_power(bool power_on);
    bool get_power() const { return status.power_on; }
    
    // Save state support
    struct SaveState {
        TransferPakStatus status;
        uint8_t current_bank;
        // Cartridge state would be saved separately
    };
    
    SaveState save_state() const;
    void load_state(const SaveState& state);
    
private:
    // Internal helper methods
    TransferPakRegion get_region(uint16_t address) const;
    uint32_t translate_address(uint16_t address) const;
    
    bool read_register(uint16_t address, uint8_t& data) const;
    bool write_register(uint16_t address, uint8_t data);
    
    bool read_gameboy_memory(uint16_t address, uint8_t& data) const;
    bool write_gameboy_memory(uint16_t address, uint8_t data);
    
    void update_access_mode();
    
    // Internal state
    TransferPakStatus status;
    std::unique_ptr<GameBoy::Cartridge> cartridge;
    
    // Register storage
    uint8_t enable_register = 0x00;
    uint8_t bank_register = 0x00;
    uint8_t power_register = 0x00;
    
    // For Mode 1 (8-bit access) emulation
    bool access_mode_changed = false;
    
    // Statistics
    struct TransferPakStats {
        size_t read_count = 0;
        size_t write_count = 0;
        size_t bank_switches = 0;
        size_t mode_changes = 0;
    } stats;
};

// Multi-controller Transfer Pak manager
class TransferPakManager {
public:
    static constexpr size_t MAX_CONTROLLERS = 4;
    
    // Singleton access
    static TransferPakManager& instance();
    
    // Controller-specific operations
    bool initialize_controller(size_t controller_index);
    void release_controller(size_t controller_index);
    void release_all();
    
    // Cartridge management per controller
    bool insert_cartridge(size_t controller_index, const std::string& rom_path);
    void remove_cartridge(size_t controller_index);
    bool save_cartridge_ram(size_t controller_index);
    
    // Memory access per controller
    bool read(size_t controller_index, uint16_t address, uint8_t& data);
    bool write(size_t controller_index, uint16_t address, uint8_t data);
    
    // Status queries
    bool is_controller_enabled(size_t controller_index) const;
    bool is_cartridge_inserted(size_t controller_index) const;
    std::optional<TransferPakStatus> get_controller_status(size_t controller_index) const;
    
    // Controller access
    TransferPakController* get_controller(size_t controller_index);
    const TransferPakController* get_controller(size_t controller_index) const;
    
    // Global operations
    void reset_all();
    void save_all_ram();
    
private:
    TransferPakManager();
    ~TransferPakManager();
    
    // No copying
    TransferPakManager(const TransferPakManager&) = delete;
    TransferPakManager& operator=(const TransferPakManager&) = delete;
    
    std::array<std::unique_ptr<TransferPakController>, MAX_CONTROLLERS> controller;
};

// Utility functions for Transfer Pak operations
namespace TransferPakUtils {
    // Address validation and translation
    constexpr bool is_valid_address(uint16_t address) {
        return address <= 0xFFFF;  // Transfer Pak uses 64KB address space
    }
    
    constexpr bool is_register_address(uint16_t address) {
        return address >= TransferPakRegisters::REGISTER_START && 
               address <= TransferPakRegisters::REGISTER_END;
    }
    
    constexpr bool is_gameboy_rom_address(uint16_t address) {
        return (address >= 0x8000 && address <= 0xBFFF) ||  // Banked ROM
               (address >= 0x0000 && address <= 0x7FFF);    // Fixed ROM
    }
    
    constexpr bool is_gameboy_ram_address(uint16_t address) {
        return address >= 0xA000 && address <= 0xBFFF;      // Cartridge RAM
    }
    
    // Bank address translation
    constexpr uint32_t translate_rom_address(uint16_t address, uint8_t bank) {
        if (address >= 0x4000 && address <= 0x7FFF) {
            // Banked area (0x4000-0x7FFF)
            return (static_cast<uint32_t>(bank) * 0x4000) + (address - 0x4000);
        } else if (address >= 0x0000 && address <= 0x3FFF) {
            // Fixed area (0x0000-0x3FFF)
            return address;
        } else if (address >= 0x8000 && address <= 0xBFFF) {
            // Alternate banked area
            return (static_cast<uint32_t>(bank) * 0x4000) + (address - 0x8000);
        }
        return 0;
    }
    
    // Register access helpers
    constexpr uint8_t get_register_index(uint16_t address) {
        return address - TransferPakRegisters::REGISTER_START;
    }
    
    // Mode conversion
    constexpr bool is_mode_8bit(TransferPakAccessMode mode) {
        return mode == TransferPakAccessMode::Mode1;
    }
    
    constexpr bool is_mode_16bit(TransferPakAccessMode mode) {
        return mode == TransferPakAccessMode::Mode0;
    }
}

// Legacy C-style struct (for compatibility)
struct transferpak {
    unsigned int enabled;
    unsigned int bank;
    unsigned int access_mode;
    unsigned int access_mode_changed;
    
    // Original used struct gb_cart
    // We'll use a pointer to the modern class instead
    GameBoy::Cartridge* gb_cart;
    
    transferpak() : enabled(0), bank(0), access_mode(0), 
                    access_mode_changed(0), gb_cart(nullptr) {}
};

// Legacy static class (for compatibility)
class Transferpak {
public:
    // Static initialization and cleanup
    static void Release();
    static void Init();
    
    // Static memory access (operates on controller 0 by default)
    static void ReadFrom(uint16_t address, uint8_t* data);
    static void WriteTo(uint16_t address, uint8_t* data);
    
    // Extended interface for multiple controllers
    static void ReadFromController(uint8_t controller, uint16_t address, uint8_t* data);
    static void WriteToController(uint8_t controller, uint16_t address, uint8_t* data);
    
    // Cartridge management
    static bool InsertCartridge(const char* rom_path);
    static bool InsertCartridge(uint8_t controller, const char* rom_path);
    static void RemoveCartridge();
    static void RemoveCartridge(uint8_t controller);
    
    // Status queries
    static bool IsEnabled();
    static bool IsCartridgeInserted();
    
private:
    // No instantiation
    Transferpak() = delete;
    ~Transferpak() = delete;
    
    // No copying
    Transferpak(const Transferpak&) = delete;
    Transferpak& operator=(const Transferpak&) = delete;
    
    // Internal legacy state
    static transferpak legacy_state;
    
    // Modern controller (for migration)
    static std::unique_ptr<TransferPakController> modern_controller;
    
    // Manager for multiple controllers
    static TransferPakManager* manager;
};

} // namespace N64Controller

// Global Transfer Pak access
namespace GlobalTransferPak {
    // Get the Transfer Pak manager
    N64Controller::TransferPakManager& get_manager();
    
    // Quick access to controller 0
    N64Controller::TransferPakController& get_controller();
    N64Controller::TransferPakController& get_controller(size_t index);
    
    // Initialize all Transfer Paks
    bool initialize();
    
    // Cleanup all Transfer Paks
    void release();
}