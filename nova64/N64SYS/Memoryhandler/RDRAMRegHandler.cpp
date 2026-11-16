/**
 * @file RDRAMRegHandler.cpp
 * @brief RDRAM device register handler (0x03F00000-0x03FFFFFF): per-device configuration, delay, mode and refresh registers.
 */
#include "RDRAMRegHandler.hpp"
#include "../MainSYS.hpp"
#include "../MIPSY/Register.hpp"
#include "../../common/log.hpp"
#include <iomanip>
#include <sstream>

// N64 RDRAM Register Base Addresses
// Range: 0x03F00000 - 0x03FFFFFF
constexpr uint32_t RDRAM_REGS_BASE = 0x03F00000;
constexpr uint32_t RDRAM_REGS_END = 0x03FFFFFF;

RDRAMRegistersHandler::RDRAMRegistersHandler(CRegisters& registers, const Config& config) :
    m_registers(registers), m_config(config) {
    m_device_count = std::min(config.device_count, RDRAM::MAX_DEVICES);
    m_expansion_pak_enabled = config.enable_expansion_pak;

    initialize_devices();
}

void RDRAMRegistersHandler::initialize_devices() {
    for (size_t i = 0; i < m_devices.size(); i++) {
        initialize_device(i);
    }
}

void RDRAMRegistersHandler::initialize_device(size_t index) {
    auto& device = m_devices[index];

    device.present = (index < m_device_count);
    device.device_id = static_cast<uint32_t>(index);
    device.type = RDRAM::DeviceType::RDRAM_16MBIT;

    // Defaults
    device.delay = 0x18181818;
    device.manufacturer = RDRAM::Manufacturer::NEC;
}

bool RDRAMRegistersHandler::is_device_register_address(uint32_t address) const {
    return (address >= RDRAM_REGS_BASE && address <= RDRAM_REGS_END);
}

std::pair<size_t, RDRAM::DeviceRegister> RDRAMRegistersHandler::decode_device_address(uint32_t address) const {
    uint32_t offset = address - RDRAM_REGS_BASE;
    size_t device_index = (offset >> 13) & 0x7; // Support up to 8 devices
    uint32_t reg_offset = offset & 0x3FC;

    RDRAM::DeviceRegister reg = RDRAM::DeviceRegister::CONFIG; // Default

    switch (reg_offset) {
    case 0x00: reg = RDRAM::DeviceRegister::CONFIG; break;
    case 0x04: reg = RDRAM::DeviceRegister::DEVICE_TYPE; break;
    case 0x08: reg = RDRAM::DeviceRegister::DEVICE_ID; break;
    case 0x0C: reg = RDRAM::DeviceRegister::DELAY; break;
    case 0x10: reg = RDRAM::DeviceRegister::MODE; break;
    case 0x14: reg = RDRAM::DeviceRegister::REF_INTERVAL; break;
    case 0x18: reg = RDRAM::DeviceRegister::REF_ROW; break;
    case 0x1C: reg = RDRAM::DeviceRegister::RAS_INTERVAL; break;
    case 0x20: reg = RDRAM::DeviceRegister::MIN_INTERVAL; break;
    case 0x24: reg = RDRAM::DeviceRegister::ADDR_SELECT; break;
    case 0x28: reg = RDRAM::DeviceRegister::DEVICE_MANUF; break;
    default:   reg = RDRAM::DeviceRegister::DEVICE_MANUF; break;
    }

    return {device_index, reg};
}

bool RDRAMRegistersHandler::Read32(uint32_t address, uint32_t& value) {
    if (!is_device_register_address(address)) return false;

    auto [idx, reg] = decode_device_address(address);

    if (idx >= m_devices.size()) {
        value = 0;
        return true;
    }

    value = read_device_register(idx, reg);

    m_stats.device_reads++;

    return true;
}

bool RDRAMRegistersHandler::Write32(uint32_t address, uint32_t value, uint32_t mask) {
    if (!is_device_register_address(address)) return false;

    auto [idx, reg] = decode_device_address(address);

    if (idx >= m_devices.size()) return true;

    write_device_register(idx, reg, value, mask);

    m_stats.device_writes++;
    return true;
}

uint32_t RDRAMRegistersHandler::read_device_register(size_t device_index, RDRAM::DeviceRegister reg) const {
    const auto& device = m_devices[device_index];
    uint32_t val = 0;

    switch (reg) {
    case RDRAM::DeviceRegister::CONFIG:      val = device.config; break;
    case RDRAM::DeviceRegister::DEVICE_TYPE: val = static_cast<uint32_t>(device.type); break;
    case RDRAM::DeviceRegister::DEVICE_ID:   val = device.device_id; break;
    case RDRAM::DeviceRegister::DELAY:       val = device.delay; break;
    case RDRAM::DeviceRegister::MODE:        val = device.mode; break;

    case RDRAM::DeviceRegister::REF_INTERVAL: val = device.refresh_interval; break;
    case RDRAM::DeviceRegister::REF_ROW:      val = device.refresh_row; break;
    case RDRAM::DeviceRegister::RAS_INTERVAL: val = device.ras_interval; break;
    case RDRAM::DeviceRegister::MIN_INTERVAL: val = device.min_interval; break;
    case RDRAM::DeviceRegister::ADDR_SELECT:  val = device.addr_select; break;
    case RDRAM::DeviceRegister::DEVICE_MANUF: val = static_cast<uint32_t>(device.manufacturer); break;
    default:                                  break;
    }

    if (reg == RDRAM::DeviceRegister::MODE) {
        // preserve P64 behavior for compatibility if desired
    }

    log_device_access(device_index, reg, val, false);
    return val;
}

void RDRAMRegistersHandler::write_device_register(size_t device_index, RDRAM::DeviceRegister reg, uint32_t value,
                                                  uint32_t mask) {
    auto& device = m_devices[device_index];

    auto update = [&](uint32_t& target) { target = (target & ~mask) | (value & mask); };

    switch (reg) {
    case RDRAM::DeviceRegister::CONFIG:      update(device.config); break;
    case RDRAM::DeviceRegister::DEVICE_TYPE: {
        uint32_t temp = static_cast<uint32_t>(device.type);
        update(temp);
        device.type = static_cast<RDRAM::DeviceType>(temp);
    } break;
    case RDRAM::DeviceRegister::DEVICE_ID: update(device.device_id); break;
    case RDRAM::DeviceRegister::DELAY:     update(device.delay); break;
    case RDRAM::DeviceRegister::MODE:
        update(device.mode);
        handle_mode_register_write(device_index, value, mask);
        break;
    case RDRAM::DeviceRegister::REF_INTERVAL: update(device.refresh_interval); break;
    case RDRAM::DeviceRegister::REF_ROW:      update(device.refresh_row); break;
    case RDRAM::DeviceRegister::RAS_INTERVAL: update(device.ras_interval); break;
    case RDRAM::DeviceRegister::MIN_INTERVAL: update(device.min_interval); break;
    case RDRAM::DeviceRegister::ADDR_SELECT:  update(device.addr_select); break;
    case RDRAM::DeviceRegister::DEVICE_MANUF: {
        uint32_t temp = static_cast<uint32_t>(device.manufacturer);
        update(temp);
        device.manufacturer = static_cast<RDRAM::Manufacturer>(temp);
    } break;
    default: break;
    }

    log_device_access(device_index, reg, value, true);
}

void RDRAMRegistersHandler::handle_mode_register_write(size_t device_index, uint32_t value, uint32_t mask) {
    update_device_power_state(device_index);
}

void RDRAMRegistersHandler::log_device_access(size_t device_index, RDRAM::DeviceRegister reg, uint32_t value,
                                              bool is_write) const {
    if (!m_config.enable_debug_logging) return;

    const char* op = is_write ? "Write" : "Read";
    const char* reg_name = "UNKNOWN";

    switch (reg) {
    case RDRAM::DeviceRegister::CONFIG:       reg_name = "CONFIG"; break;
    case RDRAM::DeviceRegister::DEVICE_TYPE:  reg_name = "DEVICE_TYPE"; break;
    case RDRAM::DeviceRegister::DEVICE_ID:    reg_name = "DEVICE_ID"; break;
    case RDRAM::DeviceRegister::DELAY:        reg_name = "DELAY"; break;
    case RDRAM::DeviceRegister::MODE:         reg_name = "MODE"; break;
    case RDRAM::DeviceRegister::REF_INTERVAL: reg_name = "REF_INTERVAL"; break;
    case RDRAM::DeviceRegister::REF_ROW:      reg_name = "REF_ROW"; break;
    case RDRAM::DeviceRegister::RAS_INTERVAL: reg_name = "RAS_INTERVAL"; break;
    case RDRAM::DeviceRegister::MIN_INTERVAL: reg_name = "MIN_INTERVAL"; break;
    case RDRAM::DeviceRegister::ADDR_SELECT:  reg_name = "ADDR_SELECT"; break;
    case RDRAM::DeviceRegister::DEVICE_MANUF: reg_name = "DEVICE_MANUF"; break;
    default:                                  break;
    }

    Log::debugf("RDRAMDev[%zu] %s %s: 0x%08X", device_index, op, reg_name, value);
}

void RDRAMRegistersHandler::update_device_power_state(size_t device_index) {
    auto& device = m_devices[device_index];
    bool self_refresh = (device.mode & static_cast<uint32_t>(RDRAM::DeviceMode::SELF_REFRESH));

    if (self_refresh) {
        enter_self_refresh(device_index);
    } else {
        exit_self_refresh(device_index);
    }
}

void RDRAMRegistersHandler::enter_self_refresh(size_t device_index) {
    if (m_config.emulate_power_states) {
        m_stats.power_state_changes++;
        Log::debugf("RDRAMDev[%zu] Entered Self Refresh", device_index);
    }
}

void RDRAMRegistersHandler::exit_self_refresh(size_t device_index) {
    if (m_config.emulate_power_states) {
        m_stats.power_state_changes++;
        Log::debugf("RDRAMDev[%zu] Exited Self Refresh", device_index);
    }
}

void RDRAMRegistersHandler::reset() {
    for (auto& dev : m_devices) dev = RDRAM::DeviceState();
    initialize_devices();
    reset_statistics();
}
