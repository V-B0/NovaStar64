/**
 * @file RDRAMRegHandler.hpp
 * @brief RDRAM device register handler declarations: device state, register accessors and RDRAMRegistersHandler.
 */
#pragma once
#include "Memoryhandler.hpp"
#include "../../common/log.hpp"
#include "../../common/Span.hpp"
#include <cstdint>
#include <array>
#include <string>

class CRegisters;

namespace RDRAM {

enum class DeviceRegister : uint32_t {
    CONFIG = 0x00,
    DEVICE_TYPE = 0x04,
    DEVICE_ID = 0x08,
    DELAY = 0x0C,
    MODE = 0x10,
    REF_INTERVAL = 0x14,
    REF_ROW = 0x18,
    RAS_INTERVAL = 0x1C,
    MIN_INTERVAL = 0x20,
    ADDR_SELECT = 0x24,
    DEVICE_MANUF = 0x28,
    REGISTER_COUNT = 11
};

static constexpr size_t MAX_DEVICES = 8;

enum class DeviceType : uint32_t {
    RDRAM_4MBIT = 0x00,
    RDRAM_8MBIT = 0x01,
    RDRAM_16MBIT = 0x02,
    RDRAM_32MBIT = 0x03,
    RDRAM_64MBIT = 0x04,
    RDRAM_128MBIT = 0x05,
    RDRAM_256MBIT = 0x06
};

enum class DeviceMode : uint32_t {
    SELF_REFRESH = 0x00000001,
    POWER_DOWN = 0x00000002,
    CLOCK_SUSPEND = 0x00000004,
    DLL_RESET = 0x00000008,
    DLL_BYPASS = 0x00000010,
    DLL_LOCK = 0x00000020,
    DLL_UNLOCK = 0x00000040
};

enum class Manufacturer : uint32_t {
    UNKNOWN = 0x00,
    RAMBUS = 0x01,
    TOSHIBA = 0x02,
    NEC = 0x03,
    SAMSUNG = 0x04,
    MICRON = 0x05,
    INFINEON = 0x06,
    MOSYS = 0x07
};

struct DeviceState {
    uint32_t config;
    DeviceType type;
    uint32_t device_id;
    uint32_t delay;
    uint32_t mode;
    uint32_t refresh_interval;
    uint32_t refresh_row;
    uint32_t ras_interval;
    uint32_t min_interval;
    uint32_t addr_select;
    Manufacturer manufacturer;
    bool present;

    constexpr DeviceState() :
        config(0), type(DeviceType::RDRAM_16MBIT), device_id(0), delay(0), mode(0), refresh_interval(0), refresh_row(0),
        ras_interval(0), min_interval(0), addr_select(0), manufacturer(Manufacturer::RAMBUS), present(false) {}
};
} // namespace RDRAM

class RDRAMDeviceRegisters {
public:
    explicit RDRAMDeviceRegisters(Common::Span<uint32_t> registers) : m_registers(registers) {}

    // Accessor methods
    uint32_t& config() { return m_registers[0]; }
    uint32_t& device_type() { return m_registers[1]; }
    uint32_t& device_id() { return m_registers[2]; }
    uint32_t& delay() { return m_registers[3]; }
    uint32_t& mode() { return m_registers[4]; }
    uint32_t& refresh_interval() { return m_registers[5]; }
    uint32_t& refresh_row() { return m_registers[6]; }
    uint32_t& ras_interval() { return m_registers[7]; }
    uint32_t& min_interval() { return m_registers[8]; }
    uint32_t& addr_select() { return m_registers[9]; }
    uint32_t& device_manuf() { return m_registers[10]; }

    // Const accessors
    uint32_t config() const { return m_registers[0]; }
    uint32_t device_type() const { return m_registers[1]; }
    uint32_t device_id() const { return m_registers[2]; }
    uint32_t delay() const { return m_registers[3]; }
    uint32_t mode() const { return m_registers[4]; }
    uint32_t refresh_interval() const { return m_registers[5]; }
    uint32_t refresh_row() const { return m_registers[6]; }
    uint32_t ras_interval() const { return m_registers[7]; }
    uint32_t min_interval() const { return m_registers[8]; }
    uint32_t addr_select() const { return m_registers[9]; }
    uint32_t device_manuf() const { return m_registers[10]; }

    RDRAM::DeviceType get_device_type_enum() const { return static_cast<RDRAM::DeviceType>(device_type()); }

    RDRAM::Manufacturer get_manufacturer_enum() const { return static_cast<RDRAM::Manufacturer>(device_manuf()); }

    bool is_self_refresh() const { return (mode() & static_cast<uint32_t>(RDRAM::DeviceMode::SELF_REFRESH)) != 0; }

    bool is_power_down() const { return (mode() & static_cast<uint32_t>(RDRAM::DeviceMode::POWER_DOWN)) != 0; }

private:
    Common::Span<uint32_t> m_registers;
};

class RDRAMRegistersHandler final : public MemoryHandler {
public:
    struct Config {
        Config() noexcept {} // explicit ctor: avoids Clang NSDMI-in-default-arg error
        size_t device_count = 4;
        bool enable_expansion_pak = true;
        bool enable_debug_logging = false;
        bool emulate_power_states = true;
    };

    explicit RDRAMRegistersHandler(CRegisters& registers, const Config& config = Config());

    ~RDRAMRegistersHandler() override = default;

    // MemoryHandler interface
    [[nodiscard]] bool Read32(uint32_t address, uint32_t& value) override;
    [[nodiscard]] bool Write32(uint32_t address, uint32_t value, uint32_t mask) override;

    // Device management
    [[nodiscard]] size_t get_device_count() const { return m_device_count; }
    [[nodiscard]] const RDRAM::DeviceState& get_device_state(size_t index) const;
    void set_device_present(size_t index, bool present);

    // Expansion Pak support
    void enable_expansion_pak(bool enable);
    [[nodiscard]] bool is_expansion_pak_enabled() const { return m_expansion_pak_enabled; }

    // State management
    void reset();
    void save_state(std::ostream& os) const;
    void load_state(std::istream& is);

    // Debug information
    [[nodiscard]] std::string get_device_info(size_t index) const;
    [[nodiscard]] std::string get_all_devices_info() const;
    [[nodiscard]] std::string get_memory_configuration() const;

    // Statistics
    struct Statistics {
        uint64_t device_reads = 0;
        uint64_t device_writes = 0;
        uint64_t power_state_changes = 0;
        uint64_t refresh_commands = 0;
    };

    [[nodiscard]] const Statistics& get_statistics() const { return m_stats; }
    void reset_statistics() { m_stats = {}; }

private:
    RDRAMRegistersHandler() = delete;
    RDRAMRegistersHandler(const RDRAMRegistersHandler&) = delete;
    RDRAMRegistersHandler(RDRAMRegistersHandler&&) = delete;
    RDRAMRegistersHandler& operator=(const RDRAMRegistersHandler&) = delete;
    RDRAMRegistersHandler& operator=(RDRAMRegistersHandler&&) = delete;

    CRegisters& m_registers;
    Config m_config;

    // Device states
    std::array<RDRAM::DeviceState, RDRAM::MAX_DEVICES> m_devices;
    size_t m_device_count;

    // Expansion Pak state
    bool m_expansion_pak_enabled;

    // Statistics
    Statistics m_stats;

    // Timing
    uint64_t m_last_refresh_cycle = 0;

    [[nodiscard]] bool is_device_register_address(uint32_t address) const;
    [[nodiscard]] std::pair<size_t, RDRAM::DeviceRegister> decode_device_address(uint32_t address) const;

    void initialize_devices();
    void initialize_device(size_t index);

    [[nodiscard]] uint32_t read_device_register(size_t device_index, RDRAM::DeviceRegister reg) const;
    void write_device_register(size_t device_index, RDRAM::DeviceRegister reg, uint32_t value, uint32_t mask);

    // Special register handling
    void handle_mode_register_write(size_t device_index, uint32_t value, uint32_t mask);
    void handle_config_register_write(size_t device_index, uint32_t value, uint32_t mask);

    // Power management
    void update_device_power_state(size_t device_index);
    void enter_self_refresh(size_t device_index);
    void exit_self_refresh(size_t device_index);
    void enter_power_down(size_t device_index);
    void exit_power_down(size_t device_index);

    // Timing calculations
    [[nodiscard]] uint32_t calculate_device_delay(size_t device_index) const;
    [[nodiscard]] uint32_t calculate_refresh_rate(size_t device_index) const;

    void log_device_access(size_t device_index, RDRAM::DeviceRegister reg, uint32_t value, bool is_write) const;
    void log_power_state_change(size_t device_index, const std::string& old_state, const std::string& new_state) const;
};
