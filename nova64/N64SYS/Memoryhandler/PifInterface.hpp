/**
 * @file PifInterface.hpp
 * @brief PIF (Peripheral Interface) and JoyBus handler declarations: controller state, CIC types, PIF RAM command processing and the Controller Pak.
 */
#pragma once
#include "Memoryhandler.hpp"
#include "../../common/log.hpp"
#include "../../common/Span.hpp"
#include "../Savetypes/Eeprom.hpp"
#include <cstdint>
#include <array>
#include <string>
#include <memory>
#include <functional>

class CN64System;
namespace Nova64 {
class MipsMemoryVM;
class StateArchive;
} // namespace Nova64
class CRegisters;

namespace PIF {
static constexpr size_t PIF_ROM_SIZE = 0x7C0; // 1984 bytes
static constexpr size_t PIF_RAM_SIZE = 0x40;  // 64 bytes

// Memory addresses
static constexpr uint32_t PIF_ROM_START = 0x1FC00000;
static constexpr uint32_t PIF_ROM_END = 0x1FC007BF;
static constexpr uint32_t PIF_RAM_START = 0x1FC007C0;
static constexpr uint32_t PIF_RAM_END = 0x1FC007FF;

// CIC types
enum class CicType : int8_t {
    Unknown = -1,
    NUS_6101 = 1,
    NUS_6102,
    NUS_6103,
    NUS_6104,
    NUS_6105, // Most common (Super Mario 64)
    NUS_6106,
    NUS_5167,
    NUS_8303,
    NUS_8501,
    NUS_8401,
    NUS_5101,
    MiniIpl3
};

// Controller commands
enum class ControllerCommand : uint8_t {
    ReadButtons = 0x01,
    ReadAccessory = 0x02,
    WriteAccessory = 0x03,
    ReadEeprom = 0x04,
    WriteEeprom = 0x05,
    Reset = 0xFF
};

// Controller state
struct ControllerState {
    uint16_t buttons;
    int8_t joystick_x;
    int8_t joystick_y;
    bool plugged_in;
    bool accessory_present; // Rumble Pak, Transfer Pak, etc.

    constexpr ControllerState() :
        buttons(0), joystick_x(0), joystick_y(0), plugged_in(false), accessory_present(false) {}
};

// PIF status flags
enum class Status : uint8_t { Ready = 0x80, Error = 0x40, Busy = 0x20 };
} // namespace PIF

class PifRamHandler final : public MemoryHandler {
public:
    struct Config {
        Config() noexcept {} // explicit ctor: avoids Clang NSDMI-in-default-arg error
        bool saves_read_only = false;
        bool enable_debug_logging = false;
        bool enable_cic_emulation = true;
        bool enable_controller_pak = true;
        std::array<std::string, 4> controller_profiles;
        PIF::CicType cic_type = PIF::CicType::NUS_6105;
    };

    explicit PifRamHandler(CN64System& system, Nova64::MipsMemoryVM& mmu, CRegisters& registers, CEeprom& Eeprom,
                           const Config& config = Config());

    ~PifRamHandler() noexcept override;

    // MemoryHandler interface
    [[nodiscard]] bool Read32(uint32_t address, uint32_t& value) override;
    [[nodiscard]] bool Write32(uint32_t address, uint32_t value, uint32_t mask) override;

    [[nodiscard]] bool Read8(uint32_t address, uint8_t& value) override;
    [[nodiscard]] bool Write8(uint32_t address, uint8_t value) override;

    // DMA operations
    void dma_read();
    void dma_write();

    // State management
    void reset();
    void soft_reset();
    void set_cic_type(PIF::CicType type) {
        m_cic_type = type;
        m_config.cic_type = type;
    }
    void save_state(std::ostream& os) const;
    void load_state(std::istream& is);
    /// Save states and rewind (see StateArchive.hpp).
    void SerializeState(Nova64::StateArchive& ar);

    // Controller management
    void set_controller_state(size_t port, const PIF::ControllerState& state);
    [[nodiscard]] PIF::ControllerState get_controller_state(size_t port) const;
    void set_controller_pak_data(size_t port, const std::vector<uint8_t>& data);
    [[nodiscard]] std::vector<uint8_t> get_controller_pak_data(size_t port) const;

    // EEPROM access
    [[nodiscard]] std::vector<uint8_t> read_eeprom() const;
    void write_eeprom(const std::vector<uint8_t>& data);

    // Debug information
    [[nodiscard]] std::string get_pif_status() const;
    [[nodiscard]] std::string dump_ram() const;
    [[nodiscard]] std::string dump_controllers() const;

    // Statistics
    struct Statistics {
        uint64_t pif_reads = 0;
        uint64_t pif_writes = 0;
        uint64_t controller_polls = 0;
        uint64_t eeprom_reads = 0;
        uint64_t eeprom_writes = 0;
        uint64_t cic_challenges = 0;
        uint64_t dma_operations = 0;
    };

    [[nodiscard]] const Statistics& get_statistics() const { return m_stats; }
    void reset_statistics() { m_stats = {}; }

    // Callbacks
    using SystemResetCallback = std::function<void()>;
    void set_system_reset_callback(SystemResetCallback callback);

private:
    PifRamHandler() = delete;
    PifRamHandler(const PifRamHandler&) = delete;
    PifRamHandler(PifRamHandler&&) = delete;
    PifRamHandler& operator=(const PifRamHandler&) = delete;
    PifRamHandler& operator=(PifRamHandler&&) = delete;

    // Internal implementation (PIMPL)
    class Impl;
    std::unique_ptr<Impl> m_impl;

    CN64System& m_system;
    Nova64::MipsMemoryVM& m_mmu;
    CRegisters& m_registers;
    Config m_config;

    // PIF memory
    std::array<uint8_t, PIF::PIF_ROM_SIZE> m_pif_rom{};
    std::array<uint8_t, PIF::PIF_RAM_SIZE> m_pif_ram{};

    // Controller states
    std::array<PIF::ControllerState, 4> m_controllers{};

    // EEPROM (can be 4K or 16K)
    CEeprom& m_eeprom;

    // CIC state
    PIF::CicType m_cic_type;
    struct CicState {
        std::array<uint8_t, 64> seed;
        std::array<uint8_t, 64> challenge;
        std::array<uint8_t, 64> response;
        uint32_t counter;
        bool authenticated;
    } m_cic_state;

    // Statistics
    Statistics m_stats;

    // Callbacks
    SystemResetCallback m_system_reset_callback;

    // Timing
    uint64_t m_last_update_cycle = 0;

    // Internal methods
    // PIF command processing  (P64-style)
    void process_pif_commands(); // Legacy wrapper -> calls pif_control_write()
    void pif_control_read();     // P64 ControlRead  — before DMA_READ
    void pif_control_write();    // P64 ControlWrite — after DMA_WRITE

    // Controller command handlers  (P64-style, pointer to TX byte)
    void ReadControllerCommand(int channel, uint8_t* Command);
    void ProcessControllerCommand(int channel, uint8_t* Command);

    // The accessory in a controller's slot: a Rumble Pak when the controller
    // settings give that player one, otherwise the Controller Pak if present.
    [[nodiscard]] bool PakPresent(int channel) const;
    void PakRead(int channel, uint32_t address, uint8_t* data);
    void PakWrite(int channel, uint32_t address, const uint8_t* data);

    // Declared but not defined.
    void process_controller_command(size_t port, Common::Span<uint8_t> command);
    void process_eeprom_command(Common::Span<uint8_t> command);

    // CIC security
    void CicNus6105(const char* Challenge, char* Response, int Length);
    void cic_nus_6105_challenge(Common::Span<const uint8_t> challenge, Common::Span<uint8_t> response);
    void cic_nus_6102_challenge(Common::Span<const uint8_t> challenge, Common::Span<uint8_t> response);
    void cic_nus_6106_challenge(Common::Span<const uint8_t> challenge, Common::Span<uint8_t> response);
    [[nodiscard]] bool validate_cic() const;

    // Controller input handling
    void update_controller_inputs();

    // Controller Pak (memory card) emulation
    class ControllerPak {
    public:
        ControllerPak();

        bool read(uint32_t address, uint8_t& value);
        bool write(uint32_t address, uint8_t value);
        [[nodiscard]] std::vector<uint8_t> get_data() const;
        void set_data(const std::vector<uint8_t>& data);
        [[nodiscard]] bool is_present() const { return m_present; }
        void set_present(bool present) { m_present = present; }

    private:
        std::array<uint8_t, 0x8000> m_data{}; // 32KB controller pak
        bool m_present = false;
        [[maybe_unused]] bool m_formatted = false;
        [[maybe_unused]] uint16_t m_crc;
    };

    std::array<ControllerPak, 4> m_controller_paks;

    // Helper methods
    [[nodiscard]] bool is_pif_address(uint32_t address) const;
    [[nodiscard]] uint32_t pif_address_to_index(uint32_t address) const;
    void log_pif_command(const std::string& description, Common::Span<const uint8_t> data);

    // Byte swapping (N64 is big-endian)
    static constexpr uint32_t swap_bytes_32(uint32_t value) noexcept {
        return ((value & 0x000000FF) << 24) | ((value & 0x0000FF00) << 8) | ((value & 0x00FF0000) >> 8) |
               ((value & 0xFF000000) >> 24);
    }

    static constexpr uint16_t swap_bytes_16(uint16_t value) noexcept {
        return ((value & 0x00FF) << 8) | ((value & 0xFF00) >> 8);
    }

    // System event handlers
    void on_system_reset();
};
