#pragma once
#include "../../common/log.hpp"
#include "Memoryhandler.hpp"
#include <cstdint>
#include <span>
#include <array>
#include <chrono>
#include <functional>  
#include <string>      
#include <memory>      

// Forward declarations
class CN64System;
class CRegisters;
class CMipsMemoryVM;
class UncachedRom;

namespace PI { 

    enum class Status : uint32_t {
        DMA_BUSY      = 0x01,
        IO_BUSY       = 0x02,
        ERROR         = 0x04,
        INTERRUPT     = 0x08
    };
    
    enum class Control : uint32_t {
        SET_RESET     = 0x01,
        CLEAR_INTR    = 0x02
    };
    
    // Register indices
    enum Register : size_t {
        PI_DRAM_ADDR = 0,
        PI_CART_ADDR,
        PI_RD_LEN,
        PI_WR_LEN,
        PI_STATUS,
        PI_BSD_DOM1_LAT,
        PI_DOMAIN1,
        PI_BSD_DOM1_PWD,
        PI_BSD_DOM1_PGS,
        PI_BSD_DOM1_RLS,
        PI_BSD_DOM2_LAT,
        PI_DOMAIN2,
        PI_BSD_DOM2_PWD,
        PI_BSD_DOM2_PGS,
        PI_BSD_DOM2_RLS,
        REGISTER_COUNT
    };
    
    // Bus Domain configurations
    struct BusDomain {
        uint32_t latency;
        uint32_t pulse_width;
        uint32_t page_size;
        uint32_t release;
    };
        
    struct DMAState {
        bool is_read;
        uint32_t dram_address;
        uint32_t cart_address;
        uint32_t length;
        uint32_t bytes_transferred;
        
        std::string to_string() const {
            return std::string(is_read ? "READ" : "WRITE") + 
                   " DMA: DRAM=0x" + 
                   std::to_string(dram_address) + 
                   " CART=0x" + 
                   std::to_string(cart_address) + 
                   " LEN=" + 
                   std::to_string(length) + 
                   " (" + 
                   std::to_string(bytes_transferred) + 
                   "/" + 
                   std::to_string(length) + 
                   ")";
        }
    };
    
    explicit PeripheralInterfaceHandler(
        CN64System& system,
        CMipsMemoryVM& mmu,
        CRegisters& registers,
        CEeprom& eeprom,
        UncachedRom& domain2_handler,
        const Config& config = {}
    );
    
    ~PeripheralInterfaceHandler() override;
    
    // MemoryHandler interface
    [[nodiscard]] bool Read32(uint32_t address, uint32_t& value) override;
    [[nodiscard]] bool Write32(uint32_t address, uint32_t value, uint32_t mask) override;
    
    // DMA control
    void process_dma_cycles(uint64_t current_cycle);
    void cancel_dma();
    
    // State management
    void reset();
    void load_state(std::istream& is);
    void save_state(std::ostream& os) const;
    
    // Debug information
    [[nodiscard]] std::string get_dma_status() const;
    [[nodiscard]] std::string get_register_dump() const;
    
    // Statistics
    struct Statistics {
        uint64_t dma_reads = 0;
        uint64_t dma_writes = 0;
        uint64_t bytes_read = 0;
        uint64_t bytes_written = 0;
        uint64_t dma_errors = 0;
        uint64_t interrupt_count = 0;
        
        // Timing
        uint64_t total_dma_cycles = 0;
        uint64_t max_dma_duration = 0;
        uint64_t min_dma_duration = UINT64_MAX;
        
        std::string to_string() const {
            return "DMA Reads: " + std::to_string(dma_reads) +
                   "\nDMA Writes: " + std::to_string(dma_writes) +
                   "\nBytes Read: " + std::to_string(bytes_read) +
                   "\nBytes Written: " + std::to_string(bytes_written) +
                   "\nDMA Errors: " + std::to_string(dma_errors) +
                   "\nInterrupts: " + std::to_string(interrupt_count);
        }
    };
    
    [[nodiscard]] const Statistics& get_statistics() const { return m_stats; }
    void reset_statistics() { m_stats = {}; }
    
    // Callbacks
    using SystemEventCallback = std::function<void()>;
    void set_reset_callback(SystemEventCallback callback);
    void set_game_state_loaded_callback(SystemEventCallback callback);
    
    // Additional public methods
    [[nodiscard]] bool is_dma_active() const { return m_dma_state.active; }
    [[nodiscard]] const DMAState& get_dma_state() const { return m_dma_state; }
    void force_dma_completion();
{ 
private:
    
    PeripheralInterfaceHandler() = delete;
    PeripheralInterfaceHandler(const PeripheralInterfaceHandler&) = delete;
    PeripheralInterfaceHandler(PeripheralInterfaceHandler&&) = delete;
    PeripheralInterfaceHandler& operator=(const PeripheralInterfaceHandler&) = delete;
    PeripheralInterfaceHandler& operator=(PeripheralInterfaceHandler&&) = delete;
    
    
    class Impl;
    std::unique_ptr<Impl> m_impl;
    
    
    CN64System& m_system;
    CMipsMemoryVM& m_mmu;
    CRegisters& m_registers;
    CEeprom& m_eeprom;
    UncachedRom& m_uncached_rom;
    Config m_config;
    
    // Registers
    std::array<uint32_t, PI::REGISTER_COUNT> m_registers{};
    PeripheralInterfaceReg m_pi_regs{m_registers};
    
    // DMA state
    DMAState m_dma_state;
    bool m_dma_used = false;
    bool m_first_dma = true;
    
    // Callbacks
    SystemEventCallback m_reset_callback;
    SystemEventCallback m_game_state_loaded_callback;
    
    // Statistics
    Statistics m_stats;
    
    // Timing
    uint64_t m_last_cycle_update = 0;
    
    // Log file (optional)
    std::unique_ptr<class FileSystem::File> m_log_file;
    
    // Internal methods
    void handle_dma_read();
    void handle_dma_write();
    void process_dma_transfer();
    void complete_dma_transfer();
    void trigger_pi_interrupt();
    void update_dma_state(uint64_t current_cycle);
    
    // Address validation
    [[nodiscard]] bool validate_dma_addresses(uint32_t dram_addr, 
                                              uint32_t cart_addr, 
                                              uint32_t length) const;
    
    // Timing calculations
    [[nodiscard]] uint64_t calculate_dma_duration(uint32_t length) const;
    [[nodiscard]] uint64_t get_current_cycle() const;
    
    // Register access helpers
    [[nodiscard]] uint32_t* get_register_ptr(uint32_t address);
    [[nodiscard]] const uint32_t* get_register_ptr(uint32_t address) const;
    
    // Status register handling
    void handle_status_register_write(uint32_t value);
    
    // Logging
    void log_message(const std::string& message) const;
    void log_dma_start(const DMAState& state) const;
    void log_dma_complete(const DMAState& state) const;
    void log_register_access(uint32_t address, uint32_t value, bool is_write) const;
    
    // System event handlers
    void on_system_reset();
    void on_game_state_loaded();
    void on_first_dma();
};
}
   