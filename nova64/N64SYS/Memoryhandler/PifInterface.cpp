/**
 * @file PifInterface.cpp
 * @brief PIF (Peripheral Interface) and JoyBus Handler Implementation
 */

#include "PifInterface.hpp"
#include "../StateArchive.hpp"
#include "../MainSYS.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Systiming.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../Savetypes/Eeprom.hpp"
#include "../../common/log.hpp"
#include "SIInterfaceHandler.hpp"
#include "../../NovaInput/KeyboardInput.hpp"
#include "../../NovaInput/GamepadInput.hpp"
#include <cstring>
#include <cstdio>
#include <algorithm>

// Use MainSYS accessor
// extern CN64System* g_System; 

namespace {
    // Helper to log byte arrays
    std::string bytes_to_string(Common::Span<const uint8_t> data) {
        std::string result;
        char buffer[4];
        for (uint8_t byte : data) {
            snprintf(buffer, sizeof(buffer), "%02X ", byte);
            result += buffer;
        }
        return result;
    }
}

// Implement the PIMPL class if we were strictly following it, 
// but since members are in the header, we'll use them directly.
class PifRamHandler::Impl {
    // Placeholder if needed
};

PifRamHandler::PifRamHandler(
    CN64System& system,
    Nova64::MipsMemoryVM& mmu,
    CRegisters& registers,
    CEeprom& eeprom,
    const Config& config
) : 
    m_system(system),
    m_mmu(mmu),
    m_registers(registers),
    m_eeprom(eeprom),
    m_config(config),
    m_cic_type(config.cic_type)
{
    m_stats = {};
    reset();
    
    // Controller 0 (Player 1) defaults to plugged in — required for OoT boot
    m_controllers[0].plugged_in = true;
}

PifRamHandler::~PifRamHandler() = default;

// ============================================================================
// Memory Handler Implementation
// ============================================================================

bool PifRamHandler::Read32(uint32_t address, uint32_t& value)
{
    address &= 0x1FFFFFFF;
    
    // PIF ROM (0x1FC00000 - 0x1FC007BF)
    // The boot ROM is usually handled by CachedRom/UncachedRom, but PIF RAM handles requests here?
    // The provided code says: if (Address < 0x1FC007C0) Value = 0;
    
    if (address < PIF::PIF_RAM_START) {
        // Accessing PIF ROM
        value = 0; 
        return true;
    }
    
    // PIF RAM (0x1FC007C0 - 0x1FC007FF)
    if (address < 0x1FC00800) {
        uint32_t offset = address - PIF::PIF_RAM_START;
        // Read 32 bits from byte array. N64 is Big Endian. Host is likely Little Endian.
        // m_pif_ram is array<uint8_t>.
        // We construct the word.
        uint32_t word = (m_pif_ram[offset] << 24) | 
                        (m_pif_ram[offset+1] << 16) | 
                        (m_pif_ram[offset+2] << 8) | 
                        (m_pif_ram[offset+3]);
                        
        value = word;
        return true;
    }
    
    return false;
}

bool PifRamHandler::Write32(uint32_t address, uint32_t value, uint32_t mask)
{
    address &= 0x1FFFFFFF;
    
    if (address < PIF::PIF_RAM_START) {
        // Write to ROM ignored
        return true;
    }
    
    // PIF RAM
    if (address < 0x1FC00800) {
        uint32_t offset = address - PIF::PIF_RAM_START;
        
        // Handle masking
        // Get current value
        uint32_t current = (m_pif_ram[offset] << 24) | 
                           (m_pif_ram[offset+1] << 16) | 
                           (m_pif_ram[offset+2] << 8) | 
                           (m_pif_ram[offset+3]);
                           
        // Update value
        uint32_t newValue = (current & ~mask) | (value & mask);
        
        // Write back bytes
        m_pif_ram[offset]   = (newValue >> 24) & 0xFF;
        m_pif_ram[offset+1] = (newValue >> 16) & 0xFF;
        m_pif_ram[offset+2] = (newValue >> 8)  & 0xFF;
        m_pif_ram[offset+3] = (newValue)       & 0xFF;
        
        // Check for control write (last word)
        if (address == 0x1FC007FC) {
            process_pif_commands();
        }
        return true;
    }
    
    return false;
}

bool PifRamHandler::Read8(uint32_t address, uint8_t& value)
{
    // Similar logic for 8-bit access
    address &= 0x1FFFFFFF;
    if (address >= PIF::PIF_RAM_START && address < 0x1FC00800) {
        value = m_pif_ram[address - PIF::PIF_RAM_START];
        return true;
    }
    return false;
}

bool PifRamHandler::Write8(uint32_t address, uint8_t value)
{
    address &= 0x1FFFFFFF;
    if (address >= PIF::PIF_RAM_START && address < 0x1FC00800) {
        m_pif_ram[address - PIF::PIF_RAM_START] = value;
        if (address == 0x1FC007FF) { // Last byte written
             process_pif_commands();
        }
        return true;
    }
    return false;
}

// ============================================================================
// DMA Operations
// ============================================================================

void PifRamHandler::dma_read()
{
    // RDRAM <- PIF_RAM  (P64: PifRamHandler::DMA_READ)
    uint8_t* rdram = m_mmu.Rdram();
    uint32_t & si_dram_addr = m_registers.m_SerialInterface[static_cast<int>(SI::Register::DRAM_ADDR)];

    if ((int32_t)si_dram_addr > (int32_t)m_mmu.RdramSize()) {
        Log::errorf("SI DMA Read: SI_DRAM_ADDR_REG not in RDRAM space: 0x%08X", si_dram_addr);
        return;
    }

    // Process controller read commands BEFORE copying to RDRAM (P64: ControlRead)
    pif_control_read();

    si_dram_addr &= 0xFFFFFFF8;

    if ((int32_t)si_dram_addr < 0) {
        int32_t rdram_pos = (int32_t)si_dram_addr;
        for (int i = 0; i < 0x40; i++, rdram_pos++) {
            if (rdram_pos < 0) continue;
            rdram[rdram_pos ^ 3] = m_pif_ram[i];
        }
    } else {
        for (int i = 0; i < 64; i++) {
            rdram[(si_dram_addr + i) ^ 3] = m_pif_ram[i];
        }
    }

    Nova64::WatchDMA(si_dram_addr, 64, "SI");

    m_stats.dma_operations++;

    // SI DMA timing — schedule interrupt. Timer period must account for CountPerOp
    // since NextTimer decrements by CountPerOp per instruction (matching P64).
    const uint32_t SI_DMA_DELAY = 900;
    if (m_system.GetSystemTimer()) {
        // Spread completion slightly across successive transfers, as the real SI
        // does, but derive the offset from the transfer count rather than rand()
        // so a given run is reproducible.
        const uint32_t jitter = static_cast<uint32_t>(m_stats.dma_operations) & 0x3F;
        m_system.GetSystemTimer()->set_timer(N64System::TimerType::SiTimer, SI_DMA_DELAY + jitter, false);
    } else {
        m_registers.m_Mips_Interface[2] |= 0x02;
        m_registers.m_SerialInterface[static_cast<int>(SI::Register::STATUS)] |= 0x1000;
        m_registers.CheckInterrupts();
    }
}

void PifRamHandler::dma_write()
{
    // PIF_RAM <- RDRAM  (P64: PifRamHandler::DMA_WRITE)
    uint8_t* rdram = m_mmu.Rdram();
    uint32_t & si_dram_addr = m_registers.m_SerialInterface[static_cast<int>(SI::Register::DRAM_ADDR)];

    if ((int32_t)si_dram_addr > (int32_t)m_mmu.RdramSize()) {
        Log::errorf("SI DMA Write: SI_DRAM_ADDR_REG not in RDRAM space: 0x%08X", si_dram_addr);
        return;
    }

    si_dram_addr &= 0xFFFFFFF8;

    if ((int32_t)si_dram_addr < 0) {
        int32_t rdram_pos = (int32_t)si_dram_addr;
        for (int i = 0; i < 0x40; i++, rdram_pos++) {
            if (rdram_pos < 0) { m_pif_ram[i] = 0; continue; }
            m_pif_ram[i] = rdram[rdram_pos ^ 3];
        }
    } else {
        for (int i = 0; i < 64; i++) {
            m_pif_ram[i] = rdram[(si_dram_addr + i) ^ 3];
        }
    }

    // Process commands after copying from RDRAM (P64: ControlWrite)
    pif_control_write();

    m_stats.dma_operations++;

    // SI DMA timing — schedule interrupt. Timer period must account for CountPerOp
    // since NextTimer decrements by CountPerOp per instruction (matching P64).
    const uint32_t SI_DMA_DELAY = 900;
    if (m_system.GetSystemTimer()) {
        m_system.GetSystemTimer()->set_timer(N64System::TimerType::SiTimer, SI_DMA_DELAY, false);
    } else {
        m_registers.m_Mips_Interface[2] |= 0x02;
        m_registers.m_SerialInterface[static_cast<int>(SI::Register::STATUS)] |= 0x1000;
        m_registers.CheckInterrupts();
    }
}

// ============================================================================
// Command Processing
// ============================================================================

// ============================================================================
// PIF ControlRead — called before DMA_READ  (matches P64's ControlRead)
// Processes read-type commands (0x01 ReadButtons) to fill response data
// ============================================================================
void PifRamHandler::pif_control_read()
{
    if (m_pif_ram[0x3F] == 0x02) return;

    // Player 1 is the keyboard plus the first game controller. Players 2-4 are
    // plugged in while a controller is connected for them.
    auto& gamepads = NovaInput::Gamepads::Instance();
    m_controllers[0] = NovaInput::PollKeyboard();
    gamepads.Apply(0, m_controllers[0]);
    for (int player = 1; player < 4; ++player) {
        PIF::ControllerState pad{};
        pad.plugged_in = gamepads.Connected(player);
        if (pad.plugged_in) gamepads.Apply(player, pad);
        m_controllers[player] = pad;
    }
    NovaInput::ApplyScriptedInput(m_controllers[0]);

    int channel = 0;
    for (int cur = 0; cur < 0x40; )
    {
        switch (m_pif_ram[cur])
        {
        case 0x00:
            channel++;
            if (channel > 5) cur = 0x40;
            else cur++;
            break;
        case 0xFD: cur = 0x40; break;
        case 0xFE: cur = 0x40; break;
        case 0xFF:
        case 0xB4:
        case 0x56:
        case 0xB8:
            cur++;
            break;
        default:
            if ((m_pif_ram[cur] & 0xC0) == 0)
            {
                if (channel < 4)
                {
                    ReadControllerCommand(channel, &m_pif_ram[cur]);
                }
                cur += m_pif_ram[cur] + (m_pif_ram[cur + 1] & 0x3F) + 2;
                channel++;
            }
            else
            {
                cur = 0x40;
            }
            break;
        }
    }
}

// ============================================================================
// Legacy wrapper — called from Write32 when 0x1FC007FC is written
// ============================================================================
void PifRamHandler::process_pif_commands()
{
    pif_control_write();
}

// ============================================================================
// PIF ControlWrite — called after DMA_WRITE  (matches P64's ControlWrite)
// Processes system control commands (CIC, etc.) and JoyBus write commands
// ============================================================================
void PifRamHandler::pif_control_write()
{
    constexpr int CHALLENGE_LENGTH = 0x20;

    // Handle control byte (byte 0x3F) — system commands
    if (m_pif_ram[0x3F] > 0x01)
    {
        static int pifCtrlCount = 0;
        if (pifCtrlCount++ < 20) {
            Log::infof("[PIF_CTRL] control=0x%02X (call #%d)", m_pif_ram[0x3F], pifCtrlCount);
        }
        switch (m_pif_ram[0x3F])
        {
        case 0x02: // CIC Challenge  (P64: case 0x02 in ControlWrite)
        {
            // Expand 15 bytes at PIF[48..62] into 30 challenge nibbles
            char challenge[30], response[30];
            for (int i = 0; i < 15; i++)
            {
                challenge[i * 2]     = (m_pif_ram[48 + i] >> 4) & 0x0F;
                challenge[i * 2 + 1] =  m_pif_ram[48 + i]        & 0x0F;
            }

            // Run the CIC-NUS-6105 algorithm (CHALLENGE_LENGTH - 2 = 30 nibbles)
            CicNus6105(challenge, response, CHALLENGE_LENGTH - 2);

            // Pack response back — P64 uses memcpy of little-endian uint64_t
            m_pif_ram[46] = 0;
            m_pif_ram[47] = 0;

            uint64_t ResponseValue = 0;
            for (int z = 8; z > 0; z--)
            {
                ResponseValue = (ResponseValue << 8) |
                    ((response[(z - 1) * 2] << 4) + response[(z - 1) * 2 + 1]);
            }
            std::memcpy(&m_pif_ram[48], &ResponseValue, sizeof(uint64_t));

            ResponseValue = 0;
            for (int z = 7; z > 0; z--)
            {
                ResponseValue = (ResponseValue << 8) |
                    ((response[((z + 8) - 1) * 2] << 4) + response[((z + 8) - 1) * 2 + 1]);
            }
            std::memcpy(&m_pif_ram[56], &ResponseValue, sizeof(uint64_t));
            break;
        }
        case 0x08: // Terminate boot / SI interrupt
            m_pif_ram[0x3F] = 0;
            m_registers.m_Mips_Interface[2] |= 0x02;  // MI_INTR_SI
            m_registers.m_SerialInterface[static_cast<int>(SI::Register::STATUS)] |= 0x1000;
            m_registers.CheckInterrupts();
            break;
        case 0x10:
            std::memset(m_pif_rom.data(), 0, m_pif_rom.size());
            break;
        case 0x30:
            m_pif_ram[0x3F] = 0x80;
            break;
        case 0xC0:
            std::memset(m_pif_ram.data(), 0, m_pif_ram.size());
            break;
        default:
            Log::warningf("Unknown PIF Control: %02X", m_pif_ram[0x3F]);
            break;
        }
        return;
    }

    // Process JoyBus channel commands  (P64: ControlWrite main loop)
    int channel = 0;
    for (int cur = 0; cur < 0x40; )
    {
        switch (m_pif_ram[cur])
        {
        case 0x00:
            channel++;
            if (channel > 5) cur = 0x40;
            else cur++;
            break;
        case 0xFD: cur = 0x40; break;
        case 0xFE: cur = 0x40; break;
        case 0xFF:
        case 0xB4:
        case 0x56:
        case 0xB8:
            cur++;
            break;
        default:
            if ((m_pif_ram[cur] & 0xC0) == 0)
            {
                if (channel < 4)
                {
                    ProcessControllerCommand(channel, &m_pif_ram[cur]);
                }
                else if (channel == 4)
                {
                    m_eeprom.EepromCommand(&m_pif_ram[cur]);
                }
                cur += m_pif_ram[cur] + (m_pif_ram[cur + 1] & 0x3F) + 2;
                channel++;
            }
            else
            {
                cur = 0x40;
            }
            break;
        }
    }
    m_pif_ram[0x3F] = 0;
}

// ============================================================================
// Controller Processing
// ============================================================================

// [Deleted duplicate process_controller_command]


void PifRamHandler::process_eeprom_command(Common::Span<uint8_t> command)
{
    // EEPROM is usually on channel 4
    if (command.empty()) return;
    
    // The CEeprom::EepromCommand method expects a pointer to the start of the JoyBus block 
    // (including TX/RX lengths) to parse commands correctly.
    // 'command' span starts at the Command byte (Offset 2).
    // We recover the pointer to the header (Offset 0).
    
    uint8_t* pRawCmd = command.data() - 2;
    
    // Boundary check
    if (pRawCmd >= m_pif_ram.data() && pRawCmd < (m_pif_ram.data() + m_pif_ram.size())) {
        m_eeprom.EepromCommand(pRawCmd);
    } else {
        Log::error("PifRamHandler: EEPROM command pointer out of PIF RAM bounds");
    }
}

// ============================================================================
// Callbacks & Updates
// ============================================================================

void PifRamHandler::reset()
{
    std::memset(m_pif_ram.data(), 0, m_pif_ram.size());

    // Write CIC seed bytes into PIF RAM (offsets 36-39)
    // These identify the CIC chip to the game's security check.
    // Format: [36]=0x00, [37]=seed_hi, [38]=seed_lo, [39]=0x3F
    switch (m_cic_type)
    {
    case PIF::CicType::NUS_6101:
        m_pif_ram[36] = 0x00; m_pif_ram[37] = 0x04; m_pif_ram[38] = 0x3F; m_pif_ram[39] = 0x3F;
        break;
    case PIF::CicType::NUS_6102:
        m_pif_ram[36] = 0x00; m_pif_ram[37] = 0x02; m_pif_ram[38] = 0x3F; m_pif_ram[39] = 0x3F;
        break;
    case PIF::CicType::NUS_6103:
        m_pif_ram[36] = 0x00; m_pif_ram[37] = 0x02; m_pif_ram[38] = 0x78; m_pif_ram[39] = 0x3F;
        break;
    case PIF::CicType::NUS_6105:
        m_pif_ram[36] = 0x00; m_pif_ram[37] = 0x02; m_pif_ram[38] = 0x91; m_pif_ram[39] = 0x3F;
        break;
    case PIF::CicType::NUS_6106:
        m_pif_ram[36] = 0x00; m_pif_ram[37] = 0x02; m_pif_ram[38] = 0x85; m_pif_ram[39] = 0x3F;
        break;
    default:
        m_pif_ram[36] = 0x00; m_pif_ram[37] = 0x02; m_pif_ram[38] = 0x3F; m_pif_ram[39] = 0x3F;
        break;
    }

    Log::infof("PIF Reset: CIC seed bytes = %02X %02X %02X %02X (CIC type %d)",
               m_pif_ram[36], m_pif_ram[37], m_pif_ram[38], m_pif_ram[39],
               static_cast<int>(m_cic_type));
}

// ============================================================================
// CIC-NUS-6105 Challenge/Response  (exact copy of P64's CicNus6105)
// ============================================================================
void PifRamHandler::CicNus6105(const char* Challenge, char* Response, int Length)
{
    static const char lut0[0x10] = {
        0x4, 0x7, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1,
        0xC, 0xF, 0x8, 0xF, 0x6, 0x3, 0x6, 0x9,
    };
    static const char lut1[0x10] = {
        0x4, 0x1, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1,
        0xC, 0x9, 0x8, 0x5, 0x6, 0x3, 0xC, 0x9,
    };
    const char* lut = lut0;
    char Key = 0xB;

    for (int i = 0; i < Length; i++)
    {
        Response[i] = (Key + 5 * Challenge[i]) & 0xF;
        Key = lut[(int)Response[i]];
        int Sgn = (Response[i] >> 3) & 0x1;
        int Mag = ((Sgn == 1) ? ~Response[i] : Response[i]) & 0x7;
        int Mod = (Mag % 3 == 1) ? Sgn : 1 - Sgn;
        if (lut == lut1 && (Response[i] == 0x1 || Response[i] == 0x9))
            Mod = 1;
        if (lut == lut1 && (Response[i] == 0xB || Response[i] == 0xE))
            Mod = 0;
        lut = (Mod == 1) ? lut1 : lut0;
    }
}

// Keep old method as wrapper for backward compatibility
void PifRamHandler::cic_nus_6105_challenge(Common::Span<const uint8_t> challenge, Common::Span<uint8_t> response)
{
    size_t len = std::min(challenge.size(), response.size());
    if (len > 30) len = 30;
    // Convert to char arrays
    char ch[30] = {}, rsp[30] = {};
    for (size_t i = 0; i < len; i++) ch[i] = (char)challenge[i];
    CicNus6105(ch, rsp, (int)len);
    for (size_t i = 0; i < len; i++) response[i] = (uint8_t)rsp[i];
}

// ============================================================================
// Accessors & Stubs
// ============================================================================

void PifRamHandler::set_controller_state(size_t port, const PIF::ControllerState& state) {
    if (port < m_controllers.size()) m_controllers[port] = state;
}

PIF::ControllerState PifRamHandler::get_controller_state(size_t port) const {
    if (port < m_controllers.size()) return m_controllers[port];
    return {};
}

std::string PifRamHandler::get_pif_status() const { return "Active"; }
std::string PifRamHandler::dump_ram() const { return bytes_to_string(m_pif_ram); }
std::string PifRamHandler::dump_controllers() const { return "Connected"; }
void PifRamHandler::soft_reset() {}
void PifRamHandler::save_state(std::ostream& os) const {}
void PifRamHandler::load_state(std::istream& is) {}

void PifRamHandler::set_controller_pak_data(size_t port, const std::vector<uint8_t>& data) {
    if (port < m_controller_paks.size()) {
        m_controller_paks[port].set_data(data);
    }
}

std::vector<uint8_t> PifRamHandler::get_controller_pak_data(size_t port) const {
    if (port < m_controller_paks.size()) {
        return m_controller_paks[port].get_data();
    }
    return {};
}

std::vector<uint8_t> PifRamHandler::read_eeprom() const { return {}; }
void PifRamHandler::write_eeprom(const std::vector<uint8_t>& data) {}
void PifRamHandler::set_system_reset_callback(SystemResetCallback callback) { m_system_reset_callback = callback; }
bool PifRamHandler::validate_cic() const { return true; }

bool PifRamHandler::is_pif_address(uint32_t address) const { return address >= PIF::PIF_RAM_START && address <= PIF::PIF_RAM_END; }
uint32_t PifRamHandler::pif_address_to_index(uint32_t address) const { return address - PIF::PIF_RAM_START; }
void PifRamHandler::log_pif_command(const std::string& description, Common::Span<const uint8_t> data) {}
void PifRamHandler::on_system_reset() {}

// ============================================================================
// Controller Pak Implementation
// ============================================================================

namespace {
    uint8_t calculate_mempak_crc(const uint8_t* data) {
        uint8_t crc = 0;
        for (int i = 0; i <= 32; i++) {
            for (int mask = 0x80; mask >= 1; mask >>= 1) {
                uint8_t xor_val = (crc & 0x80) ? 0x85 : 0x00;
                crc <<= 1;
                if (i < 32) {
                    if (data[i] & mask) {
                        crc |= 1;
                    }
                }
                crc ^= xor_val;
            }
        }
        return crc;
    }
}

PifRamHandler::ControllerPak::ControllerPak() {
    m_data.fill(0);
    m_present = false; // No controller pak by default
}

bool PifRamHandler::ControllerPak::read(uint32_t address, uint8_t& value) {
    if (address < m_data.size()) {
        value = m_data[address];
        return true;
    }
    return false;
}

bool PifRamHandler::ControllerPak::write(uint32_t address, uint8_t value) {
    if (address < m_data.size()) {
        m_data[address] = value;
        return true;
    }
    return false;
}

std::vector<uint8_t> PifRamHandler::ControllerPak::get_data() const {
    return std::vector<uint8_t>(m_data.begin(), m_data.end());
}

void PifRamHandler::ControllerPak::set_data(const std::vector<uint8_t>& data) {
    size_t copy_len = std::min(data.size(), m_data.size());
    std::copy(data.begin(), data.begin() + copy_len, m_data.begin());
}

bool PifRamHandler::PakPresent(int channel) const {
    if (channel < 0 || channel >= 4) return false;
    return NovaInput::Gamepads::Instance().HasRumblePak(channel) || m_controller_paks[channel].is_present();
}

// Rumble Pak behaviour as in mupen64plus: the pak identifies itself by reading
// 0x80 in 0x8000-0x8FFF, and a write to 0xC000 turns the motor on (non-zero)
// or off (zero).
void PifRamHandler::PakRead(int channel, uint32_t address, uint8_t* data) {
    if (NovaInput::Gamepads::Instance().HasRumblePak(channel)) {
        std::fill(data, data + 32, static_cast<uint8_t>((address >= 0x8000 && address < 0x9000) ? 0x80 : 0x00));
        return;
    }
    for (int i = 0; i < 32; i++)
        m_controller_paks[channel].read(address + i, data[i]);
}

void PifRamHandler::PakWrite(int channel, uint32_t address, const uint8_t* data) {
    auto& gamepads = NovaInput::Gamepads::Instance();
    if (gamepads.HasRumblePak(channel)) {
        if (address >= 0xC000 && address < 0xD000) gamepads.SetRumble(channel, data[0] != 0);
        return;
    }
    for (int i = 0; i < 32; i++)
        m_controller_paks[channel].write(address + i, data[i]);
}

// Controller helpers
// ============================================================================
// ReadControllerCommand — called during ControlRead (DMA_READ path)
// Command format: [TX_LEN] [RX_LEN] [CMD] [TX_DATA...] [RX_DATA...]
// Matches P64's ReadControllerCommand
// ============================================================================
void PifRamHandler::ReadControllerCommand(int channel, uint8_t* Command)
{
    switch (Command[2])
    {
    case 0x01: // Read buttons
        if (m_controllers[channel].plugged_in)
        {
            const auto& state = m_controllers[channel];
            // PIF data is big-endian: buttons_hi, buttons_lo, joystick_x, joystick_y
            Command[3] = (state.buttons >> 8) & 0xFF;
            Command[4] = state.buttons & 0xFF;
            Command[5] = static_cast<uint8_t>(state.joystick_x);
            Command[6] = static_cast<uint8_t>(state.joystick_y);
        }
        break;
    case 0x02: // Read from controller pak
        if (m_controllers[channel].plugged_in)
        {
            if (PakPresent(channel)) {
                uint32_t address = (Command[3] << 8) | (Command[4] & 0xE0);
                uint8_t* data = &Command[5];
                PakRead(channel, address, data);
                Command[0x25] = calculate_mempak_crc(data);
            } else {
                // No accessory present — set "no response" error flag
                Command[1] |= 0x80;
            }
        }
        break;
    case 0x03: // Write controller pak
        if (m_controllers[channel].plugged_in)
        {
            if (PakPresent(channel)) {
                uint32_t address = (Command[3] << 8) | (Command[4] & 0xE0);
                uint8_t* data = &Command[5];
                PakWrite(channel, address, data);
                Command[0x25] = calculate_mempak_crc(data);
            } else {
                Command[1] |= 0x80;
            }
        }
        break;
    }
}

// ============================================================================
// ProcessControllerCommand — called during ControlWrite (DMA_WRITE path)
// Handles controller identification, button reads, and pak access
// Matches P64's ProcessControllerCommand
// ============================================================================
void PifRamHandler::ProcessControllerCommand(int channel, uint8_t* Command)
{
    switch (Command[2])
    {
    case 0x00: // Check / info
    case 0xFF: // Reset and check
        if ((Command[1] & 0x80) != 0)
            break;
        if (channel > 0 && channel < 4)
            m_controllers[channel].plugged_in = NovaInput::Gamepads::Instance().Connected(channel);
        if (m_controllers[channel].plugged_in)
        {
            // N64 Controller identifier: 0x0500, status byte
            Command[3] = 0x05;
            Command[4] = 0x00;
            Command[5] = PakPresent(channel) ? 0x01 : 0x00;
        }
        else
        {
            // Not present — set error flag on RX byte
            Command[1] |= 0x80;
        }
        break;

    case 0x01: // Read buttons
        if (m_controllers[channel].plugged_in == false)
        {
            Command[1] |= 0x80;
        }
        break;

    case 0x02: // Read from controller pak
        if (m_controllers[channel].plugged_in)
        {
            if (PakPresent(channel)) {
                uint32_t address = (Command[3] << 8) | (Command[4] & 0xE0);
                uint8_t* data = &Command[5];
                PakRead(channel, address, data);
                Command[0x25] = calculate_mempak_crc(data);
            } else {
                // No accessory present — set "no response" error flag
                Command[1] |= 0x80;
            }
        }
        else
        {
            Command[1] |= 0x80;
        }
        break;

    case 0x03: // Write controller pak
        if (m_controllers[channel].plugged_in)
        {
            if (PakPresent(channel)) {
                uint32_t address = (Command[3] << 8) | (Command[4] & 0xE0);
                uint8_t* data = &Command[5];
                PakWrite(channel, address, data);
                Command[0x25] = calculate_mempak_crc(data);
            } else {
                Command[1] |= 0x80;
            }
        }
        else
        {
            Command[1] |= 0x80;
        }
        break;

    default:
        Log::warningf("PIF: Unknown controller command 0x%02X on ch%d", Command[2], channel);
        break;
    }
}

void PifRamHandler::SerializeState(Nova64::StateArchive& ar) {
    // Controller Pak contents are save data and stay as they are.
    ar.Section("PIF ");
    ar.Value(m_pif_ram);
    ar.Value(m_cic_state);
    // The SI DMA completion jitter is derived from this count, so it is timing
    // state, not just a statistic.
    ar.Value(m_stats.dma_operations);
}
