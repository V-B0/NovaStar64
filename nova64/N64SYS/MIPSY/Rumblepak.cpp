/**
 * @file Rumblepak.cpp
 * @brief Rumblepak implementation.
 */
#include "Rumblepak.hpp"
#include "../../common/log.hpp"

// Static state to track motor status
static bool s_RumbleActive = false;

void Rumblepak::ReadFrom(uint32_t addr, uint8_t& data) {
    // Rumble pak read usually returns checks
    // Address 0x8000 often used for identifying the pak
    // For now we assume a simple response logic or 0
    if (addr == 0xC000) {
        data = s_RumbleActive ? 0x01 : 0x00;
        return;
    }
    data = 0x00;
}

void Rumblepak::WriteTo(uint32_t addr, uint8_t data) {
    if (addr == 0xC000) {
        bool active = (data & 0x1) != 0;
        if (active != s_RumbleActive) {
            s_RumbleActive = active;
            // Optionally log info
        }
    }
}
