/**
 * @file RDRAMInterFaceHandler.cpp
 * @brief RDRAM Inter Face Handler implementation.
 */
#include "RDRAMInterFaceHandler.hpp"
#include "../StateArchive.hpp"
#include "../MainSYS.hpp"
#include "../MIPSY/Register.hpp"
#include "../../common/log.hpp"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <iomanip>

// Use simple register offsets for RDRAM interface registers (0x04700000 base)
namespace {
    // Base address for RDRAM Interface
    constexpr uint32_t RI_BASE_ADDRESS = 0x04700000;
    constexpr uint32_t RI_END_ADDRESS  = 0x0470001C;
}

// ============================================================================
// RDRAMInterfaceHandler Implementation
// ============================================================================

RDRAMInterfaceHandler::RDRAMInterfaceHandler(CRegisters& reg) : 
    m_reg(reg),
    m_pc(reg.m_PROGRAM_COUNTER),
    m_regs(m_register_data)
{
    reset();
}

void RDRAMInterfaceHandler::reset()
{
    m_register_data.fill(0);
    // Initialize RI registers to post-PIF boot values.
    // The boot code checks RI_SELECT_REG to determine if RDRAM is already initialized.
    // If RI_SELECT is 0, the boot code enters RDRAM init from scratch which requires
    // cycle-accurate RI controller emulation. Setting these values tells the boot code
    // RDRAM is already configured and ready.
    m_register_data[0] = 0x0E;        // RI_MODE_REG
    m_register_data[1] = 0x40;        // RI_CONFIG_REG  
    m_register_data[3] = 0x14;        // RI_SELECT_REG
    m_register_data[4] = 0x00063634;  // RI_REFRESH_REG
    m_initialized = true;
}

bool RDRAMInterfaceHandler::is_register_address(uint32_t address) const
{
    return (address >= RI_BASE_ADDRESS && address <= RI_END_ADDRESS);
}

bool RDRAMInterfaceHandler::Read32(uint32_t address, uint32_t& value)
{
    switch (address & 0x1FFFFFFF)
    {
    case 0x04700000: value = m_regs.mode(); break;
    case 0x04700004: value = m_regs.config(); break;
    case 0x04700008: value = m_regs.current_load(); break;
    case 0x0470000C: value = m_regs.select(); break;
    case 0x04700010: value = m_regs.refresh(); break;
    case 0x04700014: value = m_regs.latency(); break;
    case 0x04700018: value = m_regs.rerror(); break;
    case 0x0470001C: value = m_regs.werror(); break;
    default:
        value = 0;
        Log::debugf("RDRAMInterface: Read from unknown address 0x%08X", address);
        // Breakpoint equivalent?
        return true; 
    }

    return true;
}

bool RDRAMInterfaceHandler::Write32(uint32_t address, uint32_t value, uint32_t mask)
{
    // Update value helper
    auto update = [&](uint32_t& reg) {
        reg = (reg & ~mask) | (value & mask);
    };

    uint32_t offset = address & 0x1FFFFFFF;

    switch (offset)
    {
    case 0x04700000: update(m_regs.mode()); break;
    case 0x04700004: update(m_regs.config()); break;
    case 0x04700008: update(m_regs.current_load()); break;
    case 0x0470000C: update(m_regs.select()); break;
    case 0x04700010: update(m_regs.refresh()); break;
    case 0x04700014: update(m_regs.latency()); break;
    case 0x04700018: update(m_regs.rerror()); break;
    case 0x0470001C: update(m_regs.werror()); break;
    default:
        Log::debugf("RDRAMInterface: Write to unknown address 0x%08X", address);
        return true;
    }
    
    // Handle side effects
    if (offset == 0x04700000) {
        handle_mode_write(m_regs.mode(), mask);
    } else if (offset == 0x04700004) {
        handle_config_write(m_regs.config(), mask);
    }
    
    return true;
}

// Specific register handling bits
void RDRAMInterfaceHandler::handle_config_write(uint32_t value, uint32_t mask) 
{
    // RI_CONFIG: 0x04700004
    // Used by games to configure SDRAM type/size implicitly.
    // Logging changes can help identify memory mapping issues.
}

void RDRAMInterfaceHandler::handle_mode_write(uint32_t value, uint32_t mask) 
{
    // RI_MODE: 0x04700000
    // Bit 0: RESET - If set, reset RI interface?
    // Bit 1: STOP - Stop clock?
    if (value & 0x01) {
       // Reset requested
       // In a full hardware sim, we might clear other RI regs here
       Log::info("RDRAMInterface: RI_MODE Reset bit set");
    }
}

uint32_t RDRAMInterfaceHandler::calculate_refresh_cycles() const { 
    // RI_REFRESH: 0x04700010
    // Contains "Refresh Interval" count.
    return m_regs.refresh();
}

uint32_t RDRAMInterfaceHandler::calculate_ras_cycles() const { 
    return 0; 
}

uint32_t RDRAMInterfaceHandler::calculate_cas_cycles() const { 
    return 0; 
}

void RDRAMInterfaceHandler::log_register_access(uint32_t address, uint32_t value, bool is_write) const
{
}

std::string RDRAMInterfaceHandler::get_register_dump() const
{
    std::stringstream ss;
    ss << "RI Registers:" << std::endl;
    ss << "MODE: "         << std::hex << std::setw(8) << std::setfill('0') << m_regs.mode() << std::endl;
    ss << "CONFIG: "       << std::setw(8) << m_regs.config() << std::endl;
    ss << "CURRENT_LOAD: " << std::setw(8) << m_regs.current_load() << std::endl;
    ss << "SELECT: "       << std::setw(8) << m_regs.select() << std::endl;
    ss << "REFRESH: "      << std::setw(8) << m_regs.refresh() << std::endl;
    ss << "LATENCY: "      << std::setw(8) << m_regs.latency() << std::endl;
    ss << "RERROR: "       << std::setw(8) << m_regs.rerror() << std::endl;
    ss << "WERROR: "       << std::setw(8) << m_regs.werror() << std::endl;
    return ss.str();
}

RDRAM::Register RDRAMInterfaceHandler::address_to_register(uint32_t address) const {
    uint32_t offset = address & 0x1F;
    switch (offset) {
        case 0x00: return RDRAM::Register::MODE;
        case 0x04: return RDRAM::Register::CONFIG;
        case 0x08: return RDRAM::Register::CURRENT_LOAD;
        case 0x0C: return RDRAM::Register::SELECT;
        case 0x10: return RDRAM::Register::REFRESH;
        case 0x14: return RDRAM::Register::LATENCY;
        case 0x18: return RDRAM::Register::RERROR;
        case 0x1C: return RDRAM::Register::WERROR;
        default:   return RDRAM::Register::MODE;
    }
}

void RDRAMInterfaceHandler::SerializeState(Nova64::StateArchive& ar) {
    ar.Section("RI  ");
    ar.Value(m_register_data);
    ar.Value(m_initialized);
}
