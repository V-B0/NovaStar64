/**
 * @file SIInterfaceHandler.hpp
 * @brief Serial Interface (SI) handler declarations: register indices, status flags and SerialInterfaceHandler.
 */
#pragma once
#include "MIPSYinterfacehandler.hpp"
#include "Memoryhandler.hpp"
#include "../../common/log.hpp"
#include "../../common/Span.hpp"
#include <cstdint>
#include <array>
#include <string>
#include <memory>

class PifRamHandler;
namespace Nova64 {
class MipsMemoryVM;
class StateArchive;
} // namespace Nova64
class CRegisters;
class Logger;

namespace SI {
// Status register flags
enum class Status : uint32_t {
    DMA_BUSY = 0x0001,  // DMA transfer in progress
    READ_BUSY = 0x0002, // PIF read busy
    DMA_ERROR = 0x0008, // DMA error occurred
    INTERRUPT = 0x1000  // SI interrupt pending
};

// Register indices for m_SerialInterface array
enum class Register : uint32_t {
    DRAM_ADDR = 0,      // RDRAM address (0x00)
    PIF_ADDR_READ = 1,  // PIF read address (0x04)
    PIF_ADDR_WRITE = 2, // PIF write address (0x10)
    STATUS = 3,         // Status register (0x18)
    REGISTER_COUNT = 4
};

// DMA transfer size (fixed by hardware)
static constexpr size_t DMA_BLOCK_SIZE = 64; // 64 bytes

// Helper functions
constexpr bool is_status_set(uint32_t reg, Status flag) { return (reg & static_cast<uint32_t>(flag)) != 0; }

constexpr uint32_t set_status(uint32_t reg, Status flag) { return reg | static_cast<uint32_t>(flag); }

constexpr uint32_t clear_status(uint32_t reg, Status flag) { return reg & ~static_cast<uint32_t>(flag); }
} // namespace SI

class SerialInterfaceReg {
public:
    // Accessor methods
    uint32_t& dram_addr() { return m_registers[static_cast<size_t>(SI::Register::DRAM_ADDR)]; }
    uint32_t& pif_addr_read() { return m_registers[static_cast<size_t>(SI::Register::PIF_ADDR_READ)]; }
    uint32_t& pif_addr_write() { return m_registers[static_cast<size_t>(SI::Register::PIF_ADDR_WRITE)]; }
    uint32_t& status() { return m_registers[static_cast<size_t>(SI::Register::STATUS)]; }

    // Const accessors
    uint32_t dram_addr() const { return m_registers[static_cast<size_t>(SI::Register::DRAM_ADDR)]; }
    uint32_t pif_addr_read() const { return m_registers[static_cast<size_t>(SI::Register::PIF_ADDR_READ)]; }
    uint32_t pif_addr_write() const { return m_registers[static_cast<size_t>(SI::Register::PIF_ADDR_WRITE)]; }
    uint32_t status() const { return m_registers[static_cast<size_t>(SI::Register::STATUS)]; }

    bool is_dma_busy() const { return SI::is_status_set(status(), SI::Status::DMA_BUSY); }

    bool has_interrupt() const { return SI::is_status_set(status(), SI::Status::INTERRUPT); }

    void set_dma_busy(bool busy) {
        if (busy) {
            status() = SI::set_status(status(), SI::Status::DMA_BUSY);
        } else {
            status() = SI::clear_status(status(), SI::Status::DMA_BUSY);
        }
    }

    void set_interrupt(bool interrupt) {
        if (interrupt) {
            status() = SI::set_status(status(), SI::Status::INTERRUPT);
        } else {
            status() = SI::clear_status(status(), SI::Status::INTERRUPT);
        }
    }

    void set_dma_error(bool error) {
        if (error) {
            status() = SI::set_status(status(), SI::Status::DMA_ERROR);
        } else {
            status() = SI::clear_status(status(), SI::Status::DMA_ERROR);
        }
    }

public:
    explicit SerialInterfaceReg(Common::Span<uint32_t, static_cast<size_t>(SI::Register::REGISTER_COUNT)> registers) :
        m_registers(registers) {}

private:
    Common::Span<uint32_t, static_cast<size_t>(SI::Register::REGISTER_COUNT)> m_registers;

    SerialInterfaceReg() = delete;
    SerialInterfaceReg(const SerialInterfaceReg&) = delete;
    SerialInterfaceReg& operator=(const SerialInterfaceReg&) = delete;
};

class SerialInterfaceHandler final : public MemoryHandler {
public:
    struct Config {
        Config() noexcept {} // explicit ctor: avoids Clang NSDMI-in-default-arg error
        bool enable_dma_logging = false;
        bool emulate_dma_timing = true;
        bool enable_interrupt_debug = false;
        uint32_t dma_delay_cycles = 2000;
    };

    struct DMAState {
        bool active = false;
        bool is_read = false;
        uint32_t dram_address = 0;
        uint32_t pif_address = 0;
        size_t bytes_transferred = 0;
        uint64_t start_cycle = 0;
        uint64_t end_cycle = 0;

        bool is_complete(uint64_t current_cycle) const { return active && current_cycle >= end_cycle; }
    };

    explicit SerialInterfaceHandler(Nova64::MipsMemoryVM& mmu, CRegisters& registers, PifRamHandler& pif_handler,
                                    const Config& config = Config());

    ~SerialInterfaceHandler() override;

    [[nodiscard]] bool Read32(uint32_t address, uint32_t& value) override;
    [[nodiscard]] bool Write32(uint32_t address, uint32_t value, uint32_t mask) override;

    // DMA control
    void process_dma_cycles(uint64_t current_cycle);
    void cancel_dma();

    // State management
    void reset();
    void save_state(std::ostream& os) const;
    void load_state(std::istream& is);

    // Debug information
    [[nodiscard]] std::string get_dma_status() const;
    [[nodiscard]] std::string get_register_dump() const;

    // Statistics
    struct Statistics {
        uint64_t dma_reads = 0;
        uint64_t dma_writes = 0;
        uint64_t bytes_transferred = 0;
        uint64_t dma_errors = 0;
        uint64_t interrupts_triggered = 0;
        uint64_t status_reads = 0;
        uint64_t register_writes = 0;

        // Timing
        uint64_t total_dma_cycles = 0;
        uint64_t max_dma_duration = 0;
        uint64_t min_dma_duration = UINT64_MAX;
    };

    [[nodiscard]] const Statistics& get_statistics() const { return m_stats; }
    void reset_statistics() { m_stats = {}; }

    // Control
    [[nodiscard]] bool is_dma_active() const { return m_dma_state.active; }
    void force_dma_completion();
    /// Save states and rewind (see StateArchive.hpp).
    void SerializeState(Nova64::StateArchive& ar);

private:
    SerialInterfaceHandler() = delete;
    SerialInterfaceHandler(const SerialInterfaceHandler&) = delete;
    SerialInterfaceHandler(SerialInterfaceHandler&&) = delete;
    SerialInterfaceHandler& operator=(const SerialInterfaceHandler&) = delete;
    SerialInterfaceHandler& operator=(SerialInterfaceHandler&&) = delete;

    // Internal implementation (PIMPL)
    class Impl;
    std::unique_ptr<Impl> m_impl;

    CRegisters& m_registers;
    PifRamHandler& m_pif_handler;
    Config m_config;

    // SI registers
    SerialInterfaceReg m_si_regs;

    // DMA state
    DMAState m_dma_state;

    // Statistics
    Statistics m_stats;

    void handle_dma_read();
    void handle_dma_write();
    void complete_dma_transfer();
    void trigger_si_interrupt();

    // Address validation
    [[nodiscard]] bool validate_dma_address(uint32_t dram_addr) const;

    // Timing calculations
    [[nodiscard]] uint64_t calculate_dma_duration() const;
    [[nodiscard]] uint64_t get_current_cycle() const;

    void handle_status_register_write(uint32_t value, uint32_t mask);

    // Logging
    void log_dma_start(const DMAState& state) const;
    void log_dma_complete(const DMAState& state) const;
    void log_register_access(uint32_t address, uint32_t value, bool is_write) const;

    // System event handlers
    void on_system_reset();
};
