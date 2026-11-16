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
constexpr uint16_t A = 0x8000;
constexpr uint16_t B = 0x4000;
constexpr uint16_t Z = 0x2000;
constexpr uint16_t Start = 0x1000;
constexpr uint16_t DPadUp = 0x0800;
constexpr uint16_t DPadDn = 0x0400;
constexpr uint16_t DPadLt = 0x0200;
constexpr uint16_t DPadRt = 0x0100;
// bits 7-6 reserved
constexpr uint16_t L = 0x0020;
constexpr uint16_t R = 0x0010;
constexpr uint16_t CUp = 0x0008;
constexpr uint16_t CDown = 0x0004;
constexpr uint16_t CLeft = 0x0002;
constexpr uint16_t CRight = 0x0001;
} // namespace N64Button

/**
 * @brief Poll the SDL keyboard state and return the N64 controller state for
 *        player 1, using the bindings in settings/keybindings.ini (defaults in
 *        KeyBindings.cpp). The analog stick is digital, at +/-80 magnitude.
 */
PIF::ControllerState PollKeyboard();

/**
 * @brief Debug-only scripted input for unattended runs. With NOVA64_AUTOINPUT
 *        (SM64-style menu masher) or NOVA64_AUTOINPUT_MM (walk, open the pause
 *        menu, cycle pages) set, replaces player 1's state; otherwise leaves it.
 *        Those two count controller reads.
 *
 *        NOVA64_AUTOINPUT_SCRIPT=<file> plays a timeline instead, keyed on video
 *        frames (`viFrame`), so it lines up with NOVA64_SHOT_* screenshot numbers.
 *        One line per span: `FROM TO BUTTONS [X Y] [pulse=PERIOD,ON]`, frames
 *        half-open [FROM, TO), BUTTONS joined with `+` (A B Z Start DUp DDown
 *        DLeft DRight L R CUp CDown CLeft CRight) or `-` for none, X and Y the
 *        stick in -80..80, `pulse` to press only for the first ON frames of every
 *        PERIOD. Spans overlap freely (buttons OR together); `#` starts a comment.
 */
void ApplyScriptedInput(PIF::ControllerState& state, uint64_t viFrame);

} // namespace NovaInput
