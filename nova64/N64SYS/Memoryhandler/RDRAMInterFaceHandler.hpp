/**
 * @file RDRAMInterFaceHandler.hpp
 * @brief RDRAM Interface (RI) handler declarations: register offsets, flag enums and RDRAMInterfaceHandler.
 */
#pragma once
#include "Memoryhandler.hpp"
#include "../../common/log.hpp"
#include "../../common/Span.hpp"
#include <cstdint>
#include <array>

class CRegisters;
namespace Nova64 {
class StateArchive;
}

namespace RDRAM {
// Register offsets from 0x04700000 (RI - RDRAM Interface)
enum class Register : uint32_t {
    MODE = 0x00,
    CONFIG = 0x04,
    CURRENT_LOAD = 0x08,
    SELECT = 0x0C,
    REFRESH = 0x10, // Also COUNT
    LATENCY = 0x14,
    RERROR = 0x18,
    WERROR = 0x1C,
    REGISTER_COUNT = 8
};

// RDRAM configuration flags (RI_CONFIG)
enum class ConfigFlags : uint32_t { ENABLE = 0x00000001, RESET = 0x00000002 };

// Mode register flags (RI_MODE)
enum class ModeFlags : uint32_t {
    RESET = 0x00000001,
    STOP = 0x00000002,
};

constexpr bool has_flag(uint32_t reg, ConfigFlags flag) { return (reg & static_cast<uint32_t>(flag)) != 0; }

constexpr bool has_flag(uint32_t reg, ModeFlags flag) { return (reg & static_cast<uint32_t>(flag)) != 0; }
} // namespace RDRAM

// Register interface class
class RDRAMInterfaceReg {
public:
    // Accessor methods
    uint32_t& mode() { return m_registers[0]; }
    uint32_t& config() { return m_registers[1]; }
    uint32_t& current_load() { return m_registers[2]; }
    uint32_t& select() { return m_registers[3]; }
    uint32_t& refresh() { return m_registers[4]; }
    uint32_t& latency() { return m_registers[5]; }
    uint32_t& rerror() { return m_registers[6]; }
    uint32_t& werror() { return m_registers[7]; }

    // Const accessors
    uint32_t mode() const { return m_registers[0]; }
    uint32_t config() const { return m_registers[1]; }
    uint32_t current_load() const { return m_registers[2]; }
    uint32_t select() const { return m_registers[3]; }
    uint32_t refresh() const { return m_registers[4]; }
    uint32_t latency() const { return m_registers[5]; }
    uint32_t rerror() const { return m_registers[6]; }
    uint32_t werror() const { return m_registers[7]; }

    // Helper methods
    bool is_reset() const { return has_flag(mode(), RDRAM::ModeFlags::RESET); }

    explicit RDRAMInterfaceReg(Common::Span<uint32_t> registers) : m_registers(registers) {}

protected:
private:
    Common::Span<uint32_t> m_registers;

    // Prevent copying
    RDRAMInterfaceReg() = delete;
    RDRAMInterfaceReg(const RDRAMInterfaceReg&) = delete;
    RDRAMInterfaceReg& operator=(const RDRAMInterfaceReg&) = delete;
};

class RDRAMInterfaceHandler final : public MemoryHandler {
public:
    explicit RDRAMInterfaceHandler(CRegisters& reg);
    ~RDRAMInterfaceHandler() override = default;

    // MemoryHandler interface
    [[nodiscard]] bool Read32(uint32_t address, uint32_t& value) override;
    [[nodiscard]] bool Write32(uint32_t address, uint32_t value, uint32_t mask) override;

    // State management
    void reset();
    /// Save states and rewind (see StateArchive.hpp).
    void SerializeState(Nova64::StateArchive& ar);

    // Debug information
    [[nodiscard]] std::string get_register_dump() const;
    [[nodiscard]] bool is_initialized() const { return m_initialized; }

    // Get register access (for other components)
    const RDRAMInterfaceReg& registers() const { return m_regs; }

private:
    RDRAMInterfaceHandler() = delete;
    RDRAMInterfaceHandler(const RDRAMInterfaceHandler&) = delete;
    RDRAMInterfaceHandler(RDRAMInterfaceHandler&&) = delete;
    RDRAMInterfaceHandler& operator=(const RDRAMInterfaceHandler&) = delete;
    RDRAMInterfaceHandler& operator=(RDRAMInterfaceHandler&&) = delete;

    [[maybe_unused]] CRegisters& m_reg;
    [[maybe_unused]] const uint64_t& m_pc;

    // RDRAM interface registers
    std::array<uint32_t, static_cast<size_t>(RDRAM::Register::REGISTER_COUNT)> m_register_data{};
    RDRAMInterfaceReg m_regs{m_register_data};

    bool m_initialized = false;

    // Helper methods
    [[nodiscard]] bool is_register_address(uint32_t address) const;

    // Register handling
    void handle_config_write(uint32_t value, uint32_t mask);
    void handle_mode_write(uint32_t value, uint32_t mask);

    // Logging
    void log_register_access(uint32_t address, uint32_t value, bool is_write) const;
};
