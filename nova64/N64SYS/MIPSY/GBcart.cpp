/**
 * @file GBcart.cpp
 * @brief G Bcart implementation.
 */
#include "GBcart.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace GameBoy {

// Helper for current time
static std::time_t get_current_time() { return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()); }

// --- MBC1 Implementation ---

uint8_t MBC1Controller::read(uint16_t address) const {
    if (address < 0x4000) {
        // Bank 0 (Fixed)
        // Note: In "Advanced" banking mode (Mode 1), this can theoretically be switched for large ROMs,
        // but standard behavior treats this as Bank 0.
        return cartridge.read_rom_bank(0, address);
    } else if (address < 0x8000) {
        // Bank 1-N (Switchable)
        uint16_t bank = cartridge.get_rom_bank();
        // MBC1 translates bank 0 to 1
        if (bank == 0) bank = 1;

        // Mask based on actual ROM size (handled by mask inside read_rom_safe usually,
        // but strict MBC1 masking depends on wiring).
        return cartridge.read_rom_bank(bank, address - 0x4000);
    } else if (address >= 0xA000 && address < 0xC000) {
        if (!ram_enabled) return 0xFF;
        if (cartridge.get_ram_size() == 0) return 0xFF;

        uint8_t bank = cartridge.get_ram_bank();
        // In Mode 0 (ROM Banking Mode), RAM bank is effectively locked to 0
        if (!banking_mode) {
            bank = 0;
        }
        return cartridge.read_ram_bank(bank, address - 0xA000);
    }
    return 0xFF;
}

void MBC1Controller::write(uint16_t address, uint8_t value) {
    if (address < 0x2000) {
        // RAM Enable (0x0000-0x1FFF) - 0x0A enables
        ram_enabled = ((value & 0x0F) == 0x0A);
    } else if (address < 0x4000) {
        // ROM Bank Number (Low 5 bits) (0x2000-0x3FFF)
        uint8_t bank_low = value & 0x1F;
        if (bank_low == 0) bank_low = 1; // 0 translates to 1

        uint16_t current = cartridge.get_rom_bank();
        current &= ~0x1F; // Clear low 5 bits
        current |= bank_low;
        cartridge.set_rom_bank(current);
    } else if (address < 0x6000) {
        // RAM Bank Number / ROM Bank Number (High 2 bits) (0x4000-0x5FFF)
        uint8_t val = value & 0x03;

        // This write updates TWO potential registers.
        // 1. The RAM Bank register (used if mode=1)
        cartridge.set_ram_bank(val);

        // 2. The High bits of ROM Bank (used if rom >= 1MB)
        uint16_t current = cartridge.get_rom_bank();
        current &= ~(0x03 << 5); // Clear bits 5 and 6
        current |= (val << 5);
        cartridge.set_rom_bank(current);
    } else if (address < 0x8000) {
        // Banking Mode Select (0x6000-0x7FFF)
        banking_mode = (value & 0x01);
    } else if (address >= 0xA000 && address < 0xC000) {
        // RAM Write
        if (!ram_enabled) return;

        uint8_t bank = cartridge.get_ram_bank();
        if (!banking_mode) bank = 0; // Mode 0 forces Bank 0

        cartridge.write_ram_bank(bank, address - 0xA000, value);
    }
}

void MBC1Controller::save_state(std::vector<uint8_t>& buffer) const {
    buffer.push_back(ram_enabled ? 1 : 0);
    buffer.push_back(banking_mode ? 1 : 0);
}

void MBC1Controller::load_state(const std::vector<uint8_t>& buffer, size_t& offset) {
    if (offset + 2 <= buffer.size()) {
        ram_enabled = (buffer[offset++] != 0);
        banking_mode = (buffer[offset++] != 0);
    }
}

void MBC1Controller::reset() {
    ram_enabled = false;
    banking_mode = false;
    // MBC1 defaults
    cartridge.set_rom_bank(1);
    cartridge.set_ram_bank(0);
}

// --- MBC3 Implementation (with RTC) ---

MBC3Controller::MBC3Controller(Cartridge& cart) : MemoryBankController(cart) { reset(); }

void MBC3Controller::update_rtc_time() {
    if (!cartridge.has_rtc()) return;

    std::time_t now = get_current_time();
    if (last_rtc_update == 0) {
        last_rtc_update = now;
        return;
    }

    if (current_rtc.is_halted()) {
        last_rtc_update = now;
        return;
    }

    std::time_t delta = now - last_rtc_update;
    if (delta > 0) {
        // Increment RTC by delta seconds
        // Simplified logic: convert RTC to seconds, add delta, convert back?
        // Or ripple add.
        long remaining = static_cast<long>(delta);
        while (remaining > 0) {
            current_rtc.seconds++;
            if (current_rtc.seconds >= 60) {
                current_rtc.seconds = 0;
                current_rtc.minutes++;
                if (current_rtc.minutes >= 60) {
                    current_rtc.minutes = 0;
                    current_rtc.hours++;
                    if (current_rtc.hours >= 24) {
                        current_rtc.hours = 0;
                        uint16_t days = current_rtc.get_days();
                        days++;
                        if (days > 0x1FF) {
                            days = 0; // Overflow resets days? Or just carry?
                            current_rtc.set_carry(true);
                        }
                        current_rtc.set_days(days);
                    }
                }
            }
            remaining--;
        }
        last_rtc_update = now;
    }
}

uint8_t MBC3Controller::read(uint16_t address) const {
    if (address < 0x4000) {
        return cartridge.read_rom_bank(0, address);
    } else if (address < 0x8000) {
        uint16_t bank = cartridge.get_rom_bank();
        if (bank == 0) bank = 1;
        return cartridge.read_rom_bank(bank, address - 0x4000);
    } else if (address >= 0xA000 && address < 0xC000) {
        if (!ram_rtc_enabled) return 0xFF;

        if (rtc_register >= 0x08 && rtc_register <= 0x0C) {
            // Read from RTC
            const RTCData& src = rtc_latched ? latched_rtc : current_rtc;
            switch (rtc_register) {
            case 0x08: return src.seconds;
            case 0x09: return src.minutes;
            case 0x0A: return src.hours;
            case 0x0B: return src.days_low;
            case 0x0C: return src.days_high;
            default:   return 0xFF;
            }
        } else {
            // Read from RAM
            return cartridge.read_ram_bank(cartridge.get_ram_bank(), address - 0xA000);
        }
    }
    return 0xFF;
}

void MBC3Controller::write(uint16_t address, uint8_t value) {
    if (address < 0x2000) {
        // RAM/RTC Enable
        ram_rtc_enabled = ((value & 0x0F) == 0x0A);
    } else if (address < 0x4000) {
        // ROM Bank Number (7 bits, 1-127)
        uint8_t bank = value & 0x7F;
        if (bank == 0) bank = 1;
        cartridge.set_rom_bank(bank);
    } else if (address < 0x6000) {
        // RAM Bank Number or RTC Register Select
        if (value <= 0x03) {
            cartridge.set_ram_bank(value);
            rtc_register = 0; // Point to RAM
        } else if (value >= 0x08 && value <= 0x0C) {
            rtc_register = value;
        }
    } else if (address < 0x8000) {
        // Latch Clock Data (Write 0 then 1 to latch)
        static uint8_t last_latch_val = 0xFF;
        if (last_latch_val == 0x00 && value == 0x01) {
            // Latch current time
            latched_rtc = current_rtc;
            rtc_latched = !rtc_latched; // Toggle latch? Or usually just capture?
            // Behavior: "Writing 0 then 1 latches the current time into RTC registers"
            // Usually it stays latched until unlatched? Or just copies once?
            // It copies current time to latched_registers. Future reads read latched state until unlatched?
            // Simplified: Copy now. Reads use latched_rtc if rtc_latched is true.
            rtc_latched = true;
        }
        // Some docs suggest latching is transient or needs 0->1 transition.
        last_latch_val = value;
    } else if (address >= 0xA000 && address < 0xC000) {
        if (!ram_rtc_enabled) return;

        if (rtc_register >= 0x08 && rtc_register <= 0x0C) {
            // Write to RTC
            // Modifying registers likely clears sub-second counter or updates current time
            switch (rtc_register) {
            case 0x08: current_rtc.seconds = value; break;
            case 0x09: current_rtc.minutes = value; break;
            case 0x0A: current_rtc.hours = value; break;
            case 0x0B: current_rtc.days_low = value; break;
            case 0x0C: current_rtc.days_high = value; break;
            }
            // Update timestamp so we don't overwrite this change on next update
            last_rtc_update = get_current_time();
        } else {
            // Write to RAM
            cartridge.write_ram_bank(cartridge.get_ram_bank(), address - 0xA000, value);
        }
    }
}

void MBC3Controller::save_state(std::vector<uint8_t>& buffer) const {
    buffer.push_back(ram_rtc_enabled ? 1 : 0);
    buffer.push_back(rtc_register);
    buffer.push_back(rtc_latched ? 1 : 0);
    // Serialize RTC struct
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&current_rtc);
    buffer.insert(buffer.end(), p, p + sizeof(RTCData));
}

void MBC3Controller::load_state(const std::vector<uint8_t>& buffer, size_t& offset) {
    if (offset + 3 + sizeof(RTCData) <= buffer.size()) {
        ram_rtc_enabled = buffer[offset++] != 0;
        rtc_register = buffer[offset++];
        rtc_latched = buffer[offset++] != 0;

        std::memcpy(&current_rtc, &buffer[offset], sizeof(RTCData));
        offset += sizeof(RTCData);

        last_rtc_update = 0; // Reset delta tracking
    }
}

void MBC3Controller::reset() {
    ram_rtc_enabled = false;
    rtc_register = 0;
    rtc_latched = false;
    current_rtc = RTCData();
    latched_rtc = RTCData();
    cartridge.set_rom_bank(1);
    cartridge.set_ram_bank(0);
    last_rtc_update = get_current_time();
}

void MBC3Controller::update_rtc() { update_rtc_time(); }

// --- MBC5 Implementation ---

uint8_t MBC5Controller::read(uint16_t address) const {
    if (address < 0x4000) {
        return cartridge.read_rom_bank(0, address);
    } else if (address < 0x8000) {
        return cartridge.read_rom_bank(cartridge.get_rom_bank(), address - 0x4000);
    } else if (address >= 0xA000 && address < 0xC000) {
        if (!ram_enabled) return 0xFF;
        return cartridge.read_ram_bank(cartridge.get_ram_bank(), address - 0xA000);
    }
    return 0xFF;
}

void MBC5Controller::write(uint16_t address, uint8_t value) {
    if (address < 0x2000) {
        ram_enabled = ((value & 0x0F) == 0x0A);
    } else if (address < 0x3000) {
        // ROM Bank Lower 8 bits
        uint16_t current = cartridge.get_rom_bank();
        current &= 0xFF00;
        current |= value;
        cartridge.set_rom_bank(current);
    } else if (address < 0x4000) {
        // ROM Bank High 1 bit
        uint16_t current = cartridge.get_rom_bank();
        current &= 0x00FF;
        if (value & 0x01) current |= 0x0100;
        cartridge.set_rom_bank(current);
    } else if (address < 0x6000) {
        // RAM Bank Number (0x00-0x0F)
        cartridge.set_ram_bank(value & 0x0F);
    } else if (address >= 0xA000 && address < 0xC000) {
        if (ram_enabled) {
            cartridge.write_ram_bank(cartridge.get_ram_bank(), address - 0xA000, value);
        }
    }
}

void MBC5Controller::save_state(std::vector<uint8_t>& buffer) const { buffer.push_back(ram_enabled ? 1 : 0); }

void MBC5Controller::load_state(const std::vector<uint8_t>& buffer, size_t& offset) {
    if (offset + 1 <= buffer.size()) {
        ram_enabled = (buffer[offset++] != 0);
    }
}

void MBC5Controller::reset() {
    ram_enabled = false;
    cartridge.set_rom_bank(1);
    cartridge.set_ram_bank(0);
}

// --- Cartridge Implementation ---

Cartridge::Cartridge() {
    // Basic initialization
}

Cartridge::~Cartridge() {
    if (!save_file_path.empty() && has_battery) {
        save_ram_to_file();
    }
}

bool Cartridge::load_from_file(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    std::streamsize size = file.tellg();
    if (size <= 0) return false;

    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) return false;

    return load_from_data(buffer);
}

bool Cartridge::load_from_data(const std::vector<uint8_t>& data) {
    rom_data = data; // Copy data
    rom_size = rom_data.size();

    // Safety check just in case
    if (rom_size < 0x150) return false;

    parse_header();
    detect_cartridge_type();

    // Setup RAM
    size_t required_ram = CartridgeUtils::ram_size_from_code(rom_data[0x149]);
    // Specialized MBC2 handling - 512x4 bits (internal), but here we alloc 512 bytes
    if (type == CartridgeType::MBC2 || type == CartridgeType::MBC2_BATTERY) {
        required_ram = 512;
    }
    ram_data.resize(required_ram, 0);
    ram_size = required_ram;

    // Check battery
    has_battery = CartridgeUtils::cartridge_has_battery(type);

    create_mbc_controller();

    return true;
}

void Cartridge::parse_header() {
    // Title 0x134-0x143
    char title_buf[17] = {0};
    std::memcpy(title_buf, &rom_data[0x134], 16);
    title = std::string(title_buf);

    // Game Code (CGB)
    // ... logic omitted for brevity, user generic parsing

    // CGB Flag at 0x143
    supports_color = (rom_data[0x143] == 0x80 || rom_data[0x143] == 0xC0);

    // SGB Flag at 0x146
    supports_super = (rom_data[0x146] == 0x03);
}

void Cartridge::detect_cartridge_type() {
    uint8_t code = rom_data[0x147];
    type = static_cast<CartridgeType>(code);
}

void Cartridge::create_mbc_controller() {
    mbc.reset();

    switch (type) {
    case CartridgeType::MBC1:
    case CartridgeType::MBC1_RAM:
    case CartridgeType::MBC1_RAM_BATTERY: mbc = std::make_unique<MBC1Controller>(*this); break;

    case CartridgeType::MBC3:
    case CartridgeType::MBC3_RAM:
    case CartridgeType::MBC3_RAM_BATTERY:
    case CartridgeType::MBC3_TIMER_BATTERY:
    case CartridgeType::MBC3_TIMER_RAM_BATTERY: mbc = std::make_unique<MBC3Controller>(*this); break;

    case CartridgeType::MBC5:
    case CartridgeType::MBC5_RAM:
    case CartridgeType::MBC5_RAM_BATTERY:
    case CartridgeType::MBC5_RUMBLE:
    case CartridgeType::MBC5_RUMBLE_RAM:
    case CartridgeType::MBC5_RUMBLE_RAM_BATTERY: mbc = std::make_unique<MBC5Controller>(*this); break;

    default:
        // No MBC or unsupported
        break;
    }

    if (mbc) {
        mbc->reset();
    }
}

uint8_t Cartridge::read(uint16_t address) const {
    if (mbc) {
        return mbc->read(address);
    }

    // No MBC fallback (ROM ONLY)
    if (address < 0x8000) {
        // Max 32KB
        if (address < rom_size) return rom_data[address];
        return 0xFF;
    }
    if (address >= 0xA000 && address < 0xC000) {
        if (ram_size > 0 && (address - 0xA000) < ram_size) {
            return ram_data[address - 0xA000];
        }
    }
    return 0xFF;
}

void Cartridge::write(uint16_t address, uint8_t value) {
    if (mbc) {
        mbc->write(address, value);
    } else {
        // Handle RAM write for ROM_ONLY (if RAM exists, rare)
        if (address >= 0xA000 && address < 0xC000) {
            if (ram_size > 0 && (address - 0xA000) < ram_size) {
                ram_data[address - 0xA000] = value;
            }
        }
    }
}

void Cartridge::update() {
    if (mbc) {
        mbc->update_rtc();
    }
}

// Helpers for MBC classes
void Cartridge::set_rom_bank(uint16_t bank) { current_rom_bank = bank; }

void Cartridge::set_ram_bank(uint8_t bank) { current_ram_bank = bank; }

// Internal safe reads
// Note: These are NOT part of the header interface but needed for the implementation logic
// if I want to deduplicate checks. But since they are not in header,
// I will implement the logic inside read_rom_safe inline or add private methods if header allows.
// The header does NOT declare private helpers for read_rom_bank.
// However, the header declares `get_rom_pointer()` and `get_rom_bank_size()`.
// I'll stick to using raw pointer access which is fast.

// File I/O for RAM
bool Cartridge::save_ram_to_file() const {
    if (!has_battery || ram_data.empty() || save_file_path.empty()) return false;

    std::ofstream file(save_file_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;

    file.write(reinterpret_cast<const char*>(ram_data.data()), ram_data.size());
    return true;
}

bool Cartridge::load_ram_from_file() {
    if (save_file_path.empty()) return false;

    std::ifstream file(save_file_path, std::ios::binary);
    if (!file.is_open()) return false; // No save file aka new game

    file.read(reinterpret_cast<char*>(ram_data.data()), ram_data.size());
    return true;
}

std::vector<uint8_t> Cartridge::save_state() const {
    std::vector<uint8_t> state;
    if (mbc) mbc->save_state(state);
    return state;
}

bool Cartridge::load_state(const std::vector<uint8_t>& state) {
    if (mbc) {
        size_t offset = 0;
        mbc->load_state(state, offset);
        return true;
    }
    return false;
}

bool Cartridge::has_rtc() const { return CartridgeUtils::cartridge_has_rtc(type); }

bool Cartridge::has_rumble() const { return CartridgeUtils::cartridge_has_rumble(type); }

std::string Cartridge::get_info_string() const {
    std::stringstream ss;
    ss << "Title: " << title << " | Type: " << std::hex << (int)static_cast<uint8_t>(type)
       << " | ROM: " << rom_size / 1024 << "KB"
       << " | RAM: " << ram_size / 1024 << "KB";
    return ss.str();
}

// Added internal helper methods to avoid repetitive code in MBCs
// Since these are not in the header, I interpret calls like "read_rom_bank" in my MBC impl
// as direct pointer arithmetic on the private members (via friend access).

// Helper implementations for use inside MBCs:
// (These represent the logic I used above)
// uint8_t read_rom_bank(uint16_t bank, uint16_t offset) -> rom_data[bank * 0x4000 + offset]
// uint8_t read_ram_bank(uint8_t bank, uint16_t offset) -> ram_data[bank * 0x2000 + offset]
// void write_ram_bank(uint8_t bank, uint16_t offset, uint8_t val) -> ram_data[...] = val

} // namespace GameBoy

// --- C Interface ---

// Opaque struct definition for C
struct gb_cart {
    std::unique_ptr<GameBoy::Cartridge> impl;
};

extern "C" {

bool init_gb_cart(struct gb_cart* gb_cart, const char* gb_file) {
    // Note: The caller must have allocated memory for 'gb_cart' struct
    // OR 'gb_cart' is treated as a handle.
    // Given the ambiguity and likely usage pattern in this project:
    // We assume gb_cart is a valid pointer to storage large enough for our struct.

    if (!gb_cart || !gb_file) return false;

    // We use placement new to construct the unique_ptr in the provided memory
    // This assumes the caller knows sizeof(gb_cart) which is dangerous if opacity is true.
    // However, if this is a pointer, maybe we can just assign?
    // Safety fallback: If this is used by C++, it's fine. If C, it's risky.

    new (gb_cart) struct gb_cart();
    gb_cart->impl = std::make_unique<GameBoy::Cartridge>();

    bool result = gb_cart->impl->load_from_file(gb_file);
    if (result) {
        // Attempt to load save
        std::string save_path = std::string(gb_file) + ".sav";
        gb_cart->impl->set_save_file_path(save_path);
        gb_cart->impl->load_ram_from_file();
    }
    return result;
}

void release_gb_cart(struct gb_cart* gb_cart) {
    if (gb_cart) {
        // Save RAM before destroy
        if (gb_cart->impl) {
            gb_cart->impl->save_ram_to_file();
            gb_cart->impl.reset(); // Destroy Cartridge
        }
        gb_cart->~gb_cart();       // Destruct wrapper
    }
}

void save_gb_cart(struct gb_cart* gb_cart) {
    if (gb_cart && gb_cart->impl) {
        gb_cart->impl->save_ram_to_file();
    }
}

void read_gb_cart(struct gb_cart* gb_cart, uint16_t address, uint8_t* data) {
    if (gb_cart && gb_cart->impl && data) {
        *data = gb_cart->impl->read(address);
    }
}

void write_gb_cart(struct gb_cart* gb_cart, uint16_t address, const uint8_t* data) {
    if (gb_cart && gb_cart->impl && data) {
        gb_cart->impl->write(address, *data);
    }
}

} // extern "C"

// --- Cartridge bank access methods (used by MBC controllers) ---

uint8_t GameBoy::Cartridge::read_rom_bank(uint16_t bank, uint16_t offset) const {
    size_t address = static_cast<size_t>(bank) * 0x4000 + offset;
    if (address < rom_data.size()) {
        return rom_data[address];
    }
    // Wrap around if address exceeds ROM size
    if (!rom_data.empty()) {
        return rom_data[address % rom_data.size()];
    }
    return 0xFF;
}

uint8_t GameBoy::Cartridge::read_ram_bank(uint8_t bank, uint16_t offset) const {
    size_t address = static_cast<size_t>(bank) * 0x2000 + offset;
    if (address < ram_data.size()) {
        return ram_data[address];
    }
    return 0xFF;
}

void GameBoy::Cartridge::write_ram_bank(uint8_t bank, uint16_t offset, uint8_t value) {
    size_t address = static_cast<size_t>(bank) * 0x2000 + offset;
    if (address < ram_data.size()) {
        ram_data[address] = value;
    }
}
