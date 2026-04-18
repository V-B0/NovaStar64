/**
 * @file KeyboardInput.cpp
 * @brief Keyboard-to-N64-controller mapping implementation
 */

#include "KeyboardInput.hpp"
#include <SDL.h>

namespace NovaInput {

static constexpr int8_t ANALOG_MAX =  80;
static constexpr int8_t ANALOG_MIN = -80;

PIF::ControllerState PollKeyboard()
{
    PIF::ControllerState state{};
    state.plugged_in = true;

    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (!keys) return state;

    uint16_t buttons = 0;

    // A = X key
    if (keys[SDL_SCANCODE_X])        buttons |= N64Button::A;
    // B = Y or Z key (supports both QWERTY and QWERTZ layouts)
    if (keys[SDL_SCANCODE_Y] || keys[SDL_SCANCODE_Z])
                                     buttons |= N64Button::B;
    // Start = Enter/Return
    if (keys[SDL_SCANCODE_RETURN])   buttons |= N64Button::Start;
    // Z trigger = C key
    if (keys[SDL_SCANCODE_C])        buttons |= N64Button::Z;
    // L bumper = Left Shift
    if (keys[SDL_SCANCODE_LSHIFT])   buttons |= N64Button::L;
    // R bumper = Right Shift
    if (keys[SDL_SCANCODE_RSHIFT])   buttons |= N64Button::R;

    // Analog stick = WASD
    int8_t ax = 0, ay = 0;
    if (keys[SDL_SCANCODE_W])        ay =  ANALOG_MAX;
    if (keys[SDL_SCANCODE_S])        ay =  ANALOG_MIN;
    if (keys[SDL_SCANCODE_A])        ax =  ANALOG_MIN;
    if (keys[SDL_SCANCODE_D])        ax =  ANALOG_MAX;
    state.joystick_x = ax;
    state.joystick_y = ay;

    // D-Pad = TFGH (reachable from left hand near WASD)
    if (keys[SDL_SCANCODE_T])        buttons |= N64Button::DPadUp;
    if (keys[SDL_SCANCODE_G])        buttons |= N64Button::DPadDn;
    if (keys[SDL_SCANCODE_F])        buttons |= N64Button::DPadLt;
    if (keys[SDL_SCANCODE_H])        buttons |= N64Button::DPadRt;

    // C-buttons = Arrow keys
    if (keys[SDL_SCANCODE_UP])       buttons |= N64Button::CUp;
    if (keys[SDL_SCANCODE_DOWN])     buttons |= N64Button::CDown;
    if (keys[SDL_SCANCODE_LEFT])     buttons |= N64Button::CLeft;
    if (keys[SDL_SCANCODE_RIGHT])    buttons |= N64Button::CRight;

    state.buttons = buttons;

    return state;
}

} // namespace NovaInput
