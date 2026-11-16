/**
 * @file SIInterfaceHandler.cpp
 * @brief Serial Interface (SI) register handler (0x04800000-0x0480001C): DMA address registers, status and DMA requests to the PIF.
 */
#include "SIInterfaceHandler.hpp"
#include "../StateArchive.hpp"
#include "PifInterface.hpp"
#include "../MIPSY/Register.hpp" // For CRegisters definition
#include "../MIPSY/Mempak.hpp"
#include <iomanip>
#include <sstream>

// Constants for Address Decoding (0x04800000 range)
constexpr uint32_t SI_BASE_ADDR = 0x04800000;
constexpr uint32_t SI_END_ADDR = 0x0480001C; // P64 handles up to 0x18, + 4 bytes

// Implementation of internal PIMPL class not needed if we follow P64 simple logic,
// using m_impl for any complex state tracking if implied by header.
// For now, we will implement the logic directly in the class as per P64.
class SerialInterfaceHandler::Impl {};

SerialInterfaceHandler::SerialInterfaceHandler(Nova64::MipsMemoryVM& mmu, CRegisters& registers,
                                               PifRamHandler& pif_handler, const Config& config) :
    m_impl(std::make_unique<Impl>()), m_mmu(mmu), m_registers(registers), m_pif_handler(pif_handler), m_config(config),
    m_si_regs(registers.m_SerialInterface) // Link to global registers
{
    reset();
}

SerialInterfaceHandler::~SerialInterfaceHandler() = default;

void SerialInterfaceHandler::reset() {
    // Clear registers if needed? P64 doesn't explicit reset in constructor body
    // except calling parent constructor which zeros?
    // Registers are zeroed by CRegisters Reset usually.
    m_dma_state = {};
    reset_statistics();
}

bool SerialInterfaceHandler::Read32(uint32_t address, uint32_t& value) {
    // P64 Logic:
    // Only SI_STATUS_REG (0x18 offset) returns value. Others return 0?
    // Address masking: 0x1FFFFFFF

    uint32_t offset = address & 0x1FFFFFFF;

    switch (offset) {
    case 0x04800018:
        value = m_si_regs.status();
        m_stats.status_reads++;
        break;
    default: value = 0; break;
    }

    if (m_config.enable_dma_logging) {
        log_register_access(address, value, false);
    }

    return true;
}

bool SerialInterfaceHandler::Write32(uint32_t address, uint32_t value, uint32_t mask) {
    uint32_t offset = address & 0x1FFFFFFF;

    // Log before write (P64 behavior)
    if (m_config.enable_dma_logging) {
        log_register_access(address, value, true);
    }

    auto update = [mask, value](uint32_t& reg) { reg = (reg & ~mask) | (value & mask); };

    switch (offset) {
    case 0x04800000: // SI_DRAM_ADDR_REG
        update(m_si_regs.dram_addr());
        break;

    case 0x04800004: // SI_PIF_ADDR_RD64B_REG (DMA PIF->RAM)
        update(m_si_regs.pif_addr_read());
        handle_dma_read();
        break;

    case 0x04800010: // SI_PIF_ADDR_WR64B_REG (DMA RAM->PIF)
        update(m_si_regs.pif_addr_write());
        handle_dma_write();
        break;

    case 0x04800018: // SI_STATUS_REG
    {
        // Clear SI Interrupt in MI
        constexpr uint32_t MI_INTR_SI_MASK = 0x02;
        m_registers.m_Mips_Interface[2] &= ~MI_INTR_SI_MASK;

        m_si_regs.set_interrupt(false);

        m_registers.CheckInterrupts();

        m_stats.register_writes++;
    } break;

    default: break;
    }

    m_stats.register_writes++;
    return true;
}

// --- Implemented Helper Methods ---

void SerialInterfaceHandler::process_dma_cycles(uint64_t current_cycle) {
    if (m_dma_state.active && m_dma_state.is_complete(current_cycle)) {
        complete_dma_transfer();
    }
}

void SerialInterfaceHandler::cancel_dma() { m_dma_state.active = false; }

void SerialInterfaceHandler::force_dma_completion() {
    if (m_dma_state.active) {
        complete_dma_transfer();
    }
}

bool SerialInterfaceHandler::is_si_register_address(uint32_t address) const {
    return (address >= SI_BASE_ADDR && address <= SI_END_ADDR); // P64 does limited range
}

void SerialInterfaceHandler::save_state(std::ostream& os) const {
    // Basic register dump
    for (auto reg : m_registers.m_SerialInterface) {
        uint32_t val = reg;
        os.write(reinterpret_cast<const char*>(&val), sizeof(val));
    }
}

void SerialInterfaceHandler::load_state(std::istream& is) {
    for (size_t i = 0; i < m_registers.m_SerialInterface.size(); i++) {
        uint32_t val;
        is.read(reinterpret_cast<char*>(&val), sizeof(val));
        m_registers.m_SerialInterface[i] = val;
    }
}

std::string SerialInterfaceHandler::get_dma_status() const { return m_dma_state.active ? "Active" : "Idle"; }

std::string SerialInterfaceHandler::get_register_dump() const {
    std::stringstream ss;
    ss << "SI Registers:\n"
       << "DRAM Addr: " << std::hex << m_si_regs.dram_addr() << "\n"
       << "PIF Read: " << m_si_regs.pif_addr_read() << "\n"
       << "PIF Write: " << m_si_regs.pif_addr_write() << "\n"
       << "Status: " << m_si_regs.status() << "\n";
    return ss.str();
}

void SerialInterfaceHandler::on_system_reset() { reset(); }

void SerialInterfaceHandler::handle_dma_read() {
    // PIF -> DRAM
    if (!validate_dma_address(m_si_regs.dram_addr())) return;

    // Instant transfer simulation (P64 style)
    m_pif_handler.dma_read();
    m_stats.dma_reads++;

    // SI interrupt is delivered by the PIF handler's SiTimer (~900 cycles)
    // to match real hardware timing.  Do not also fire it here — that
    // causes a spurious double interrupt.

    log_dma_complete({true, true, m_si_regs.dram_addr(), 0});
}

void SerialInterfaceHandler::handle_dma_write() {
    // DRAM -> PIF
    if (!validate_dma_address(m_si_regs.dram_addr())) return;

    m_pif_handler.dma_write();
    m_stats.dma_writes++;

    // SI interrupt is delivered by the PIF handler's SiTimer (~900 cycles)
    // to match real hardware timing.  Do not also fire it here — that
    // causes a spurious double interrupt.

    log_dma_complete({true, false, m_si_regs.dram_addr(), 0});
}

void SerialInterfaceHandler::process_dma_transfer() {
    // DMA is executed instantly in handle_dma_read() and handle_dma_write().
}

void SerialInterfaceHandler::complete_dma_transfer() {
    m_dma_state.active = false;
    trigger_si_interrupt();
}

void SerialInterfaceHandler::trigger_si_interrupt() {
    constexpr uint32_t MI_INTR_SI_MASK = 0x02;
    m_registers.m_Mips_Interface[2] |= MI_INTR_SI_MASK;
    m_si_regs.set_interrupt(true);
    m_registers.CheckInterrupts();
}

bool SerialInterfaceHandler::validate_dma_address(uint32_t dram_addr) const {
    // Every address is accepted; no alignment is enforced.
    return true;
}

bool SerialInterfaceHandler::validate_pif_address(uint32_t pif_addr) const { return true; }

uint64_t SerialInterfaceHandler::calculate_dma_duration() const { return 0; }
uint64_t SerialInterfaceHandler::get_current_cycle() const { return 0; }

SI::Register SerialInterfaceHandler::address_to_register(uint32_t address) const {
    uint32_t offset = address & 0x1F;
    switch (offset) {
    case 0x00: return SI::Register::DRAM_ADDR;
    case 0x04: return SI::Register::PIF_ADDR_READ;
    case 0x10: return SI::Register::PIF_ADDR_WRITE;
    case 0x18: return SI::Register::STATUS;
    default:   return SI::Register::STATUS;
    }
}

void SerialInterfaceHandler::handle_status_register_write(uint32_t value, uint32_t mask) {
    // Interrupt clear logic is in Write32
}

bool SerialInterfaceHandler::read_from_pif(uint32_t pif_addr, uint8_t* buffer, size_t size) {
    for (size_t i = 0; i < size; i++) {
        (void)m_pif_handler.Read8(pif_addr + i, buffer[i]);
    }
    return true;
}

bool SerialInterfaceHandler::write_to_pif(uint32_t pif_addr, const uint8_t* buffer, size_t size) {
    for (size_t i = 0; i < size; i++) {
        (void)m_pif_handler.Write8(pif_addr + i, buffer[i]);
    }
    return true;
}

bool SerialInterfaceHandler::read_from_rdram(uint32_t dram_addr, uint8_t* buffer, size_t size) {
    // Not implemented: the PIF handler accesses RDRAM itself during DMA.
    return false;
}

bool SerialInterfaceHandler::write_to_rdram(uint32_t dram_addr, const uint8_t* buffer, size_t size) { return false; }

void SerialInterfaceHandler::log_dma_start(const DMAState& state) const {
    Log::debugf("DMA Start: %s Addr=0x%08X", state.is_read ? "Read" : "Write", state.dram_address);
}

void SerialInterfaceHandler::log_dma_complete(const DMAState& state) const { Log::debugf("DMA Complete"); }

void SerialInterfaceHandler::log_register_access(uint32_t address, uint32_t value, bool is_write) const {
    if (m_config.enable_dma_logging) {
        Log::debugf("SI %s 0x%08X: 0x%08X", is_write ? "Write" : "Read", address, value);
    }
}

void SerialInterfaceHandler::SerializeState(Nova64::StateArchive& ar) {
    // The SI registers themselves live in CRegisters.
    ar.Section("SI  ");
    ar.Value(m_dma_state);
}
