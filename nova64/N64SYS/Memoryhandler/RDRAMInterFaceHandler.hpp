#pragma once
#include "MemoryHandler.hpp"
#include "../../common/log.hpp"
#include <cstdint>
#include <span>
#include <array>

// Forward declaration
class CRegisters;

namespace RDRAM {
    // Register offsets from 0x03F00000
    enum class Register : uint32_t {
        CONFIG           = 0x00,
        DEVICE_ID        = 0x04,
        DELAY            = 0x08,
        MODE             = 0x0C,
        REF_INTERVAL     = 0x10,
        REF_ROW          = 0x14,
        RAS_INTERVAL     = 0x18,
        MIN_INTERVAL     = 0x1C,
        ADDR_SELECT      = 0x20,
        DEVICE_MANUF     = 0x24,
        REGISTER_COUNT   = 10
    };
    
    // RDRAM configuration flags (RD_CONFIG register)
    enum class ConfigFlags : uint32_t {
        ENABLE           = 0x00000001,
        ADDR_SKEW        = 0x00000002,
        COLUMN_SIZE_256  = 0x00000004,
        COLUMN_SIZE_512  = 0x00000008,
        COLUMN_SIZE_1024 = 0x0000000C,
        ROW_SIZE_8       = 0x00000010,
        ROW_SIZE_9       = 0x00000020,
        ROW_SIZE_10      = 0x00000030,
        BANK_OVERLAP     = 0x00000040,
        REFRESH_ENABLE   = 0x00000080
    };
    
    // Mode register flags (RD_MODE register)
    enum class ModeFlags : uint32_t {
        ACTIVE_POWER_DOWN = 0x00000001,
        STANDBY_POWER_DOWN = 0x00000002,
        SELF_REFRESH      = 0x00000004,
        CLOCK_SUSPEND     = 0x00000008,
        DLL_RESET         = 0x00000010,
        DLL_BYPASS        = 0x00000020,
        DLL_LOCK          = 0x00000040,
        DLL_UNLOCK        = 0x00000080
    };
    
    // Helper functions
    constexpr bool has_flag(uint32_t reg, ConfigFlags flag) {
        return (reg & static_cast<uint32_t>(flag)) != 0;
    }
    
    constexpr bool has_flag(uint32_t reg, ModeFlags flag) {
        return (reg & static_cast<uint32_t>(flag)) != 0;
    }
}

// Register interface class (optional)
class RDRAMInterfaceReg {
public:
    // Accessor methods
    uint32_t& config() { return m_registers[static_cast<size_t>(RDRAM::Register::CONFIG)]; }
    uint32_t& device_id() { return m_registers[static_cast<size_t>(RDRAM::Register::DEVICE_ID)]; }
    uint32_t& delay() { return m_registers[static_cast<size_t>(RDRAM::Register::DELAY)]; }
    uint32_t& mode() { return m_registers[static_cast<size_t>(RDRAM::Register::MODE)]; }
    uint32_t& ref_interval() { return m_registers[static_cast<size_t>(RDRAM::Register::REF_INTERVAL)]; }
    uint32_t& ref_row() { return m_registers[static_cast<size_t>(RDRAM::Register::REF_ROW)]; }
    uint32_t& ras_interval() { return m_registers[static_cast<size_t>(RDRAM::Register::RAS_INTERVAL)]; }
    uint32_t& min_interval() { return m_registers[static_cast<size_t>(RDRAM::Register::MIN_INTERVAL)]; }
    uint32_t& addr_select() { return m_registers[static_cast<size_t>(RDRAM::Register::ADDR_SELECT)]; }
    uint32_t& device_manuf() { return m_registers[static_cast<size_t>(RDRAM::Register::DEVICE_MANUF)]; }
    
    // Const accessors
    uint32_t config() const { return m_registers[static_cast<size_t>(RDRAM::Register::CONFIG)]; }
    uint32_t device_id() const { return m_registers[static_cast<size_t>(RDRAM::Register::DEVICE_ID)]; }
    uint32_t delay() const { return m_registers[static_cast<size_t>(RDRAM::Register::DELAY)]; }
    uint32_t mode() const { return m_registers[static_cast<size_t>(RDRAM::Register::MODE)]; }
    uint32_t ref_interval() const { return m_registers[static_cast<size_t>(RDRAM::Register::REF_INTERVAL)]; }
    uint32_t ref_row() const { return m_registers[static_cast<size_t>(RDRAM::Register::REF_ROW)]; }
    uint32_t ras_interval() const { return m_registers[static_cast<size_t>(RDRAM::Register::RAS_INTERVAL)]; }
    uint32_t min_interval() const { return m_registers[static_cast<size_t>(RDRAM::Register::MIN_INTERVAL)]; }
    uint32_t addr_select() const { return m_registers[static_cast<size_t>(RDRAM::Register::ADDR_SELECT)]; }
    uint32_t device_manuf() const { return m_registers[static_cast<size_t>(RDRAM::Register::DEVICE_MANUF)]; }
    
    // Helper methods
    bool is_enabled() const {
        return has_flag(config(), RDRAM::ConfigFlags::ENABLE);
    }
    
    bool is_self_refresh() const {
        return has_flag(mode(), RDRAM::ModeFlags::SELF_REFRESH);
    }
    
    uint32_t get_column_size() const {
        uint32_t col_bits = config() & 0x0C;
        switch (col_bits) {
            case 0x04: return 256;
            case 0x08: return 512;
            case 0x0C: return 1024;
            default: return 256;  // Default
        }
    }
    
    uint32_t get_row_size() const {
        uint32_t row_bits = config() & 0x30;
        switch (row_bits) {
            case 0x10: return 8;
            case 0x20: return 9;
            case 0x30: return 10;
            default: return 8;  // Default
        }
    }

protected:
    explicit RDRAMInterfaceReg(std::span<uint32_t, static_cast<size_t>(RDRAM::Register::REGISTER_COUNT)> registers)
        : m_registers(registers) {}

private:
    std::span<uint32_t, static_cast<size_t>(RDRAM::Register::REGISTER_COUNT)> m_registers;
    
    // Prevent copying
    RDRAMInterfaceReg() = delete;
    RDRAMInterfaceReg(const RDRAMInterfaceReg&) = delete;
    RDRAMInterfaceReg& operator=(const RDRAMInterfaceReg&) = delete;
};

// Main handler class
class RDRAMInterfaceHandler final : public MemoryHandler {
public:
    explicit RDRAMInterfaceHandler(CRegisters& reg);
    ~RDRAMInterfaceHandler() override = default;
    
    // MemoryHandler interface
    [[nodiscard]] bool Read32(uint32_t address, uint32_t& value) override;
    [[nodiscard]] bool Write32(uint32_t address, uint32_t value, uint32_t mask) override;
    
    // State management
    void reset();
    
    // Debug information
    [[nodiscard]] std::string get_register_dump() const;
    [[nodiscard]] bool is_initialized() const { return m_initialized; }
    
    // Get register access (for other components)
    const RDRAMInterfaceReg& registers() const { return m_regs; }
    
private:
    // Delete copy and move operations
    RDRAMInterfaceHandler() = delete;
    RDRAMInterfaceHandler(const RDRAMInterfaceHandler&) = delete;
    RDRAMInterfaceHandler(RDRAMInterfaceHandler&&) = delete;
    RDRAMInterfaceHandler& operator=(const RDRAMInterfaceHandler&) = delete;
    RDRAMInterfaceHandler& operator=(RDRAMInterfaceHandler&&) = delete;
    
    // Member variables
    CRegisters& m_reg;
    const uint64_t& m_pc;
    
    // RDRAM interface registers
    std::array<uint32_t, static_cast<size_t>(RDRAM::Register::REGISTER_COUNT)> m_register_data{};
    RDRAMInterfaceReg m_regs{m_register_data};
    
    // State
    bool m_initialized = false;
    
    // Helper methods
    [[nodiscard]] bool is_register_address(uint32_t address) const;
    [[nodiscard]] RDRAM::Register address_to_register(uint32_t address) const;
    
    // Register handling
    void handle_config_write(uint32_t value, uint32_t mask);
    void handle_mode_write(uint32_t value, uint32_t mask);
    
    // Timing calculations
    [[nodiscard]] uint32_t calculate_refresh_cycles() const;
    [[nodiscard]] uint32_t calculate_ras_cycles() const;
    [[nodiscard]] uint32_t calculate_cas_cycles() const;
    
    // Logging
    void log_register_access(uint32_t address, uint32_t value, bool is_write) const;
};