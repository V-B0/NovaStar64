/**
 * @file KeyboardInput.hpp
 * @brief Keyboard-to-N64-controller mapping
 *
 * Maps SDL keyboard state to N64 controller buttons and analog stick.
 */
#pragma once

#include "../N64SYS/Memoryhandler/PifInterface.hpp"
#include <cstdint>

namespace NovaInput {

// N64 controller button bitmask (matches PIF hardware format)
namespace N64Button {
    constexpr uint16_t A       = 0x8000;
    constexpr uint16_t B       = 0x4000;
    constexpr uint16_t Z       = 0x2000;
    constexpr uint16_t Start   = 0x1000;
    constexpr uint16_t DPadUp  = 0x0800;
    constexpr uint16_t DPadDn  = 0x0400;
    constexpr uint16_t DPadLt  = 0x0200;
    constexpr uint16_t DPadRt  = 0x0100;
    // bits 7-6 reserved
    constexpr uint16_t L       = 0x0020;
    constexpr uint16_t R       = 0x0010;
    constexpr uint16_t CUp     = 0x0008;
    constexpr uint16_t CDown   = 0x0004;
    constexpr uint16_t CLeft   = 0x0002;
    constexpr uint16_t CRight  = 0x0001;
}

/**
 * @brief Poll the SDL keyboard state and return the N64 controller state
 *        for player 1 using the built-in key mapping.
 *
 * Key mapping:
 *   A         = X
 *   B         = Y or Z (keyboard)
 *   Start     = Enter/Return
 *   Z trigger = C
 *   L bumper  = Left Shift
 *   R bumper  = Right Shift
 *   Analog    = W/A/S/D (digital, +/-80 magnitude)
 *   D-Pad     = T/F/G/H
 *   C-buttons = Arrow keys
 */
PIF::ControllerState PollKeyboard();

} // namespace NovaInput
