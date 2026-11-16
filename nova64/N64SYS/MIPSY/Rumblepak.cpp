/**
 * @file Rumblepak.cpp
 * @brief Rumble Pak model: a single motor register that reads back its on/off state.
 */
#include "Rumblepak.hpp"
#include "../../common/log.hpp"

static bool s_RumbleActive = false;

void Rumblepak::ReadFrom(uint32_t addr, uint8_t& data) {
    // The motor register at 0xC000 reads back its on/off state; every other address reads 0.
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
        }
    }
}
