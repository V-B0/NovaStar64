/**
 * @file KeyboardInput.cpp
 * @brief Keyboard-to-N64-controller mapping implementation
 */

#include "KeyboardInput.hpp"
#include "KeyBindings.hpp"

#include <SDL.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace NovaInput {

namespace {

constexpr int8_t ANALOG_MAX =  80;
constexpr int8_t ANALOG_MIN = -80;

// Re-reads the bindings file at most once a second, so a rebind made in the
// launcher applies to a game window that is already open.
const KeyBindings& CurrentBindings() {
    static KeyBindings bindings;
    static std::filesystem::file_time_type loadedAt{};
    static bool haveFile = false;
    static Uint32 lastCheck = 0;
    static bool checkedOnce = false;

    const Uint32 now = SDL_GetTicks();
    if (checkedOnce && now - lastCheck < 1000) return bindings;
    checkedOnce = true;
    lastCheck = now;

    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(kKeyBindingsPath, ec);
    if (ec) return bindings;

    if (!haveFile || modified != loadedAt) {
        KeyBindings fresh;
        fresh.LoadFromFile(kKeyBindingsPath);
        bindings = fresh;
        loadedAt = modified;
        haveFile = true;
    }
    return bindings;
}

bool Held(const Uint8* keys, const KeyBinding& b) {
    return (b.primary > 0 && keys[b.primary]) || (b.alternate > 0 && keys[b.alternate]);
}

} // namespace

PIF::ControllerState PollKeyboard()
{
    PIF::ControllerState state{};
    state.plugged_in = true;

    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (!keys) return state;

    const KeyBindings& kb = CurrentBindings();
    auto held = [&](N64Input in) { return Held(keys, kb[in]); };

    uint16_t buttons = 0;
    if (held(N64Input::A))         buttons |= N64Button::A;
    if (held(N64Input::B))         buttons |= N64Button::B;
    if (held(N64Input::Z))         buttons |= N64Button::Z;
    if (held(N64Input::Start))     buttons |= N64Button::Start;
    if (held(N64Input::L))         buttons |= N64Button::L;
    if (held(N64Input::R))         buttons |= N64Button::R;
    if (held(N64Input::DPadUp))    buttons |= N64Button::DPadUp;
    if (held(N64Input::DPadDown))  buttons |= N64Button::DPadDn;
    if (held(N64Input::DPadLeft))  buttons |= N64Button::DPadLt;
    if (held(N64Input::DPadRight)) buttons |= N64Button::DPadRt;
    if (held(N64Input::CUp))       buttons |= N64Button::CUp;
    if (held(N64Input::CDown))     buttons |= N64Button::CDown;
    if (held(N64Input::CLeft))     buttons |= N64Button::CLeft;
    if (held(N64Input::CRight))    buttons |= N64Button::CRight;
    state.buttons = buttons;

    int8_t ax = 0, ay = 0;
    if (held(N64Input::StickUp))    ay = ANALOG_MAX;
    if (held(N64Input::StickDown))  ay = ANALOG_MIN;
    if (held(N64Input::StickLeft))  ax = ANALOG_MIN;
    if (held(N64Input::StickRight)) ax = ANALOG_MAX;
    state.joystick_x = ax;
    state.joystick_y = ay;

    return state;
}

void ApplyScriptedInput(PIF::ControllerState& state)
{
    // These scripts used to live in the DLL input plugin's GetControllerState,
    // which the PIF never calls (it reads PollKeyboard directly), so they had
    // no effect at all.
    static const bool s_mm      = std::getenv("NOVA64_AUTOINPUT_MM") != nullptr;
    static const bool s_generic = std::getenv("NOVA64_AUTOINPUT") != nullptr;
    if (!s_mm && !s_generic) return;

    static unsigned long s_count = 0;
    const unsigned long t = s_count++;
    uint16_t buttons = 0;
    int8_t x = 0, y = 0;

    if (s_mm) {
        // Reads spent pressing Start/A to get from boot into the game.
        static const unsigned long s_boot = []() {
            const char* v = std::getenv("NOVA64_AUTOINPUT_MM_BOOT");
            return v ? std::strtoul(v, nullptr, 10) : 1200ul;
        }();
        if (t < s_boot) {
            if ((t % 100) < 8)      buttons |= N64Button::Start;
            else if ((t % 40) < 8)  buttons |= N64Button::A;
        } else {
            // Walk forward, open the pause menu, cycle pages with R, close it.
            const unsigned long local = (t - s_boot) % 3000;
            if (local < 400) {
                y = 80;
                if ((local % 40) < 8) buttons |= N64Button::A;   // clear dialogue
            } else if (local < 420) {
                buttons |= N64Button::Start;
            } else if ((local >= 900 && local < 1100) || (local >= 1400 && local < 1600)) {
                // Turn the page: hold R and push the stick right, which moves
                // the cursor off the edge of the page and turns it as well.
                buttons |= N64Button::R;
                x = 80;
            } else if (local >= 1900 && local < 1920) {
                buttons |= N64Button::Start;
            }
        }
    } else {
        if (t >= 300 && t < 2400) {
            if ((t % 120) < 8)      buttons |= N64Button::Start;
            else if ((t % 60) < 8)  buttons |= N64Button::A;
        } else if (t >= 2400) {
            // Slow circular stick sweep with A taps and an occasional Start.
            const double angle = static_cast<double>(t) * 0.02;
            x = static_cast<int8_t>(80.0 * std::cos(angle));
            y = static_cast<int8_t>(80.0 * std::sin(angle));
            if ((t % 30) < 10)  buttons |= N64Button::A;
            if ((t % 600) < 8)  buttons |= N64Button::Start;
        }
    }

    state.buttons    = buttons;
    state.joystick_x = x;
    state.joystick_y = y;
}

} // namespace NovaInput
