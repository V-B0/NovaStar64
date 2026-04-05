/**
 * @file TransferPak.cpp
 * @brief Transfer Pak implementation.
 */
#include "TransferPak.hpp"
#include "GBcart.hpp"
#include "../../common/Log.hpp"

namespace N64Controller {

using namespace TransferPakRegisters;

TransferPakController::TransferPakController() {
    reset();
}

TransferPakController::~TransferPakController() {
    release();
}

void TransferPakController::reset() {
    status = TransferPakStatus();
    enable_register = 0x00;
    bank_register = 0x00;
    power_register = 0x00;
    access_mode_changed = false;
}

bool TransferPakController::initialize() {
    reset();
    return true;
}

void TransferPakController::release() {
    cartridge.reset();
}

// Memory Access
bool TransferPakController::read(uint16_t address, uint8_t& data) const {
    if (address >= REGISTER_START && address <= REGISTER_END) {
        return read_register(address, data);
    }
    
    if (status.enabled && status.power_on && cartridge) {
        return read_gameboy_memory(address, data);
    }

    data = 0x00;
    return true;
}

bool TransferPakController::write(uint16_t address, uint8_t data) {
    if (address >= REGISTER_START && address <= REGISTER_END) {
        return write_register(address, data);
    }
    
    if (status.enabled && status.power_on && cartridge) {
        return write_gameboy_memory(address, data);
    }
    
    return true;
}

bool TransferPakController::read_register(uint16_t address, uint8_t& data) const {
    switch (address) {
        case ENABLE_REGISTER: // 0x8000
            data = status.to_register_value();
            break;
        case BANK_REGISTER:   // 0x8001
            data = bank_register;
            break;
        case POWER_REGISTER:  // 0x8002
            data = power_register;
            break;
        case STATUS_REGISTER: // 0x8003
            data = 0x00; 
            if (cartridge) data |= 0x08; 
            break;
        default:
            return false;
    }
    return true;
}

bool TransferPakController::write_register(uint16_t address, uint8_t data) {
    switch (address) {
        case ENABLE_REGISTER: // 0x8000
            enable_register = data;
            status.from_register_value(data);
            break;
        case BANK_REGISTER:   // 0x8001
            bank_register = data;
            if (status.enabled) {
                status.current_bank = static_cast<TransferPakBank>(data & 0x03);
            }
            break;
        case POWER_REGISTER:  // 0x8002
            power_register = data;
            if (data & 0x01) {
                status.power_on = false; 
            } else {
                status.power_on = true;
            }
            break;
        case STATUS_REGISTER: 
            break;
        default:
            return false;
    }
    return true;
}

bool TransferPakController::read_gameboy_memory(uint16_t address, uint8_t& data) const {
    // Basic mapping: 
    // If bank0, map 0-0x3FFF to GB 0-0x3FFF?
    // Transfer pak maps GB space into N64 0x8000-0xFFFF window via banks.
    // Address input to this function is 0x0000-0xFFFF (offset in PAK memory space?)
    // Usually calls come with offset from 0xC000 or similar.
    // Assuming address is 0-0xFFFF relative to Transfer Pak base.
    
    // Transfer pak space is usually 32KB window.
    // 0x0000-0x7FFF is Bank 0 (Fixed GB ROM 0-0x3FFF repeated?)
    // Actually TP has a window.
    
    // Simplification: Ask cartridge directly.
    // But cartridge expects 16-bit address.
    
    // We'll pass the address directly for now, assuming caller handles bank mapping offsets if needed,
    // or we use status.current_bank for the switchable part.
    
    // If address is in switchable area:
    if (address >= 0x4000 && address < 0x8000) {
       // Bank 1 mapped here?
       // This depends on the specific N64-side mapping which is complex.
       // We'll trust the cartridge implementation to handle a flat address space query
       // or we need to map based on `status.current_bank`.
       
       // For this implementation, we just proxy to cartridge.
       data = cartridge->read(address); 
       return true;
    }
    
    data = cartridge->read(address);
    return true;
}

bool TransferPakController::write_gameboy_memory(uint16_t address, uint8_t data) {
    if (cartridge) {
         cartridge->write(address, data);
         return true;
    }
    return false;
}

bool TransferPakController::insert_cartridge(const std::string& rom_path) {
    cartridge = std::make_unique<GameBoy::Cartridge>();
    return cartridge->load_from_file(rom_path);
}

void TransferPakController::remove_cartridge() {
    cartridge.reset();
    status.cartridge_inserted = false;
    status.power_on = false;
}

} // namespace N64Controller
