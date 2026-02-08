#pragma once
#include "MIPSYInterfaceHandler.hpp"
#include "Memoryhandler.hpp"
#include "../../common/log.hpp"
#include <cstdint>
#include <span>
#include <array>
#include <string>
#include <memory>


class PifRamHandler;
class CMipsMemoryVM;
class CRegisters;
class Logger;

namespace SI {
    // Status register flags
    enum class Status : uint32_t {
        DMA_BUSY   = 0x0001,  // DMA transfer in progress
        READ_BUSY  = 0x0002,  // PIF read busy
        DMA_ERROR  = 0x0008,  // DMA error occurred
        INTERRUPT  = 0x1000   // SI interrupt pending
    };
    
    // Register offsets (from 0x04800000)
    enum class Register : uint32_t {
        DRAM_ADDR      = 0x00,  // RDRAM address
        PIF_ADDR_READ  = 0x04,  // PIF read address (64-byte block)
        PIF_ADDR_WRITE = 0x08,  // PIF write address (64-byte block)
        STATUS         = 0x0C,  // Status register
        REGISTER_COUNT = 4
    };
    
    // DMA transfer size (fixed by hardware)
    static constexpr size_t DMA_BLOCK_SIZE = 64;  // 64 bytes
    
    // Helper functions
    constexpr bool is_status_set(uint32_t reg, Status flag) {
        return (reg & static_cast<uint32_t>(flag)) != 0;
    }
    
    constexpr uint32_t set_status(uint32_t reg, Status flag) {
        return reg | static_cast<uint32_t>(flag);
    }
    
    constexpr uint32_t clear_status(uint32_t reg, Status flag) {
        return reg & ~static_cast<uint32_t>(flag);
    }
}


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
    
    
    bool is_dma_busy() const {
        return SI::is_status_set(status(), SI::Status::DMA_BUSY);
    }
    
    bool has_interrupt() const {
        return SI::is_status_set(status(), SI::Status::INTERRUPT);
    }
    
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

protected:
    explicit SerialInterfaceReg(std::span<uint32_t, static_cast<size_t>(SI::Register::REGISTER_COUNT)> registers)
        : m_registers(registers) {}

private:
    std::span<uint32_t, static_cast<size_t>(SI::Register::REGISTER_COUNT)> m_registers;
    
    
    SerialInterfaceReg() = delete;
    SerialInterfaceReg(const SerialInterfaceReg&) = delete;
    SerialInterfaceReg& operator=(const SerialInterfaceReg&) = delete;
};


class SerialInterfaceHandler final : public MemoryHandler {
public:
    struct Config {
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
        
        bool is_complete(uint64_t current_cycle) const {
            return active && current_cycle >= end_cycle;
        }
    };
    
    explicit SerialInterfaceHandler(
        CMipsMemoryVM& mmu,
        CRegisters& registers,
        PifRamHandler& pif_handler,
        const Config& config = {}
    );
    
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

private:
    
    SerialInterfaceHandler() = delete;
    SerialInterfaceHandler(const SerialInterfaceHandler&) = delete;
    SerialInterfaceHandler(SerialInterfaceHandler&&) = delete;
    SerialInterfaceHandler& operator=(const SerialInterfaceHandler&) = delete;
    SerialInterfaceHandler& operator=(SerialInterfaceHandler&&) = delete;
    
    // Internal implementation (PIMPL)
    class Impl;
    std::unique_ptr<Impl> m_impl;
    
    // Member variables
    CMipsMemoryVM& m_mmu;
    CRegisters& m_registers;
    PifRamHandler& m_pif_handler;
    Config m_config;
    logging::Logger m_logger;
    
    // SI registers
    std::array<uint32_t, static_cast<size_t>(SI::Register::REGISTER_COUNT)> m_registers{};
    SerialInterfaceReg m_si_regs{m_registers};
    
    // DMA state
    DMAState m_dma_state;
    
    // Statistics
    Statistics m_stats;
    
    // Timing
    uint64_t m_last_cycle_update = 0;
    
    // Internal methods
    void handle_dma_read();      
    void handle_dma_write();     
    void process_dma_transfer();
    void complete_dma_transfer();
    void trigger_si_interrupt();
    
    // Address validation
    [[nodiscard]] bool validate_dma_address(uint32_t dram_addr) const;
    [[nodiscard]] bool validate_pif_address(uint32_t pif_addr) const;
    
    // Timing calculations
    [[nodiscard]] uint64_t calculate_dma_duration() const;
    [[nodiscard]] uint64_t get_current_cycle() const;
    
    // Register access helpers
    [[nodiscard]] bool is_si_register_address(uint32_t address) const;
    [[nodiscard]] SI::Register address_to_register(uint32_t address) const;
    void handle_status_register_write(uint32_t value, uint32_t mask);
    
    // PIF access helpers
    [[nodiscard]] bool read_from_pif(uint32_t pif_addr, uint8_t* buffer, size_t size);
    [[nodiscard]] bool write_to_pif(uint32_t pif_addr, const uint8_t* buffer, size_t size);
    
    // RDRAM access helpers
    [[nodiscard]] bool read_from_rdram(uint32_t dram_addr, uint8_t* buffer, size_t size);
    [[nodiscard]] bool write_to_rdram(uint32_t dram_addr, const uint8_t* buffer, size_t size);
    
    // Logging
    void log_dma_start(const DMAState& state) const;
    void log_dma_complete(const DMAState& state) const;
    void log_register_access(uint32_t address, uint32_t value, bool is_write) const;
    
    // System event handlers
    void on_system_reset();
};