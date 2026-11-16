/**
 * @file GamepadInput.cpp
 * @brief Game controllers as N64 controllers: bindings, analog stick, player
 *        slots and the Rumble Pak.
 */

#include "GamepadInput.hpp"
#include "KeyboardInput.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace NovaInput {

namespace {

using Kind = PadBinding::Kind;

PadBinding PadButton(SDL_GameControllerButton b) { return {Kind::Button, b}; }
PadBinding PadAxisPlus(SDL_GameControllerAxis a) { return {Kind::AxisPositive, a}; }
PadBinding PadAxisMinus(SDL_GameControllerAxis a) { return {Kind::AxisNegative, a}; }

struct DefaultPadBinding {
    PadBinding primary;
    PadBinding alternate;
};

// Indexed by N64Input. The N64's A and B sit below and left of each other,
// like a pad's south and west buttons; the C buttons go on the right stick
// and Z on the left trigger, where it sits under the N64's grip.
const DefaultPadBinding& DefaultBinding(size_t input) {
    static const DefaultPadBinding kDefaults[kN64InputCount] = {
        {PadButton(SDL_CONTROLLER_BUTTON_A), {}},                                 // A
        {PadButton(SDL_CONTROLLER_BUTTON_X), PadButton(SDL_CONTROLLER_BUTTON_B)}, // B
        {PadAxisPlus(SDL_CONTROLLER_AXIS_TRIGGERLEFT), {}},                       // Z
        {PadButton(SDL_CONTROLLER_BUTTON_START), {}},                             // Start
        {PadButton(SDL_CONTROLLER_BUTTON_DPAD_UP), {}},
        {PadButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN), {}},
        {PadButton(SDL_CONTROLLER_BUTTON_DPAD_LEFT), {}},
        {PadButton(SDL_CONTROLLER_BUTTON_DPAD_RIGHT), {}},
        {PadButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER), {}},                                             // L
        {PadButton(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER), PadAxisPlus(SDL_CONTROLLER_AXIS_TRIGGERRIGHT)}, // R
        {PadAxisMinus(SDL_CONTROLLER_AXIS_RIGHTY), {}},                                                  // C-Up
        {PadAxisPlus(SDL_CONTROLLER_AXIS_RIGHTY), {}},                                                   // C-Down
        {PadAxisMinus(SDL_CONTROLLER_AXIS_RIGHTX), {}},                                                  // C-Left
        {PadAxisPlus(SDL_CONTROLLER_AXIS_RIGHTX), {}},                                                   // C-Right
        {PadAxisMinus(SDL_CONTROLLER_AXIS_LEFTY), {}},                                                   // Stick Up
        {PadAxisPlus(SDL_CONTROLLER_AXIS_LEFTY), {}},                                                    // Stick Down
        {PadAxisMinus(SDL_CONTROLLER_AXIS_LEFTX), {}},                                                   // Stick Left
        {PadAxisPlus(SDL_CONTROLLER_AXIS_LEFTX), {}},                                                    // Stick Right
    };
    return kDefaults[input];
}

const char* ButtonLabel(int button) {
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:             return "A Button";
    case SDL_CONTROLLER_BUTTON_B:             return "B Button";
    case SDL_CONTROLLER_BUTTON_X:             return "X Button";
    case SDL_CONTROLLER_BUTTON_Y:             return "Y Button";
    case SDL_CONTROLLER_BUTTON_BACK:          return "Back";
    case SDL_CONTROLLER_BUTTON_GUIDE:         return "Guide";
    case SDL_CONTROLLER_BUTTON_START:         return "Start";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return "Left Stick Click";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return "Right Stick Click";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return "Left Bumper";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "Right Bumper";
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return "D-Pad Up";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return "D-Pad Down";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return "D-Pad Left";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return "D-Pad Right";
    case SDL_CONTROLLER_BUTTON_MISC1:         return "Share";
    case SDL_CONTROLLER_BUTTON_PADDLE1:       return "Paddle 1";
    case SDL_CONTROLLER_BUTTON_PADDLE2:       return "Paddle 2";
    case SDL_CONTROLLER_BUTTON_PADDLE3:       return "Paddle 3";
    case SDL_CONTROLLER_BUTTON_PADDLE4:       return "Paddle 4";
    case SDL_CONTROLLER_BUTTON_TOUCHPAD:      return "Touchpad";
    default:                                  return "Button ?";
    }
}

const char* AxisLabel(int axis, bool positive) {
    switch (axis) {
    case SDL_CONTROLLER_AXIS_LEFTX:        return positive ? "Left Stick Right" : "Left Stick Left";
    case SDL_CONTROLLER_AXIS_LEFTY:        return positive ? "Left Stick Down" : "Left Stick Up";
    case SDL_CONTROLLER_AXIS_RIGHTX:       return positive ? "Right Stick Right" : "Right Stick Left";
    case SDL_CONTROLLER_AXIS_RIGHTY:       return positive ? "Right Stick Down" : "Right Stick Up";
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:  return positive ? "Left Trigger" : "Left Trigger (released)";
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return positive ? "Right Trigger" : "Right Trigger (released)";
    default:                               return "Axis ?";
    }
}

std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// "none", an SDL button name ("a", "leftshoulder"), or an axis name with a
// direction ("+leftx", "-righty").
bool ParseBinding(const std::string& text, PadBinding& out) {
    if (text.empty()) return false;
    if (text == "none") {
        out = {};
        return true;
    }
    if (text[0] == '+' || text[0] == '-') {
        const SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(text.c_str() + 1);
        if (axis == SDL_CONTROLLER_AXIS_INVALID) return false;
        out = {text[0] == '+' ? Kind::AxisPositive : Kind::AxisNegative, axis};
        return true;
    }
    const SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(text.c_str());
    if (button == SDL_CONTROLLER_BUTTON_INVALID) return false;
    out = {Kind::Button, button};
    return true;
}

std::string FormatBinding(const PadBinding& b) {
    switch (b.kind) {
    case Kind::Button: {
        const char* name = SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(b.index));
        return name ? name : "none";
    }
    case Kind::AxisPositive:
    case Kind::AxisNegative: {
        const char* name = SDL_GameControllerGetStringForAxis(static_cast<SDL_GameControllerAxis>(b.index));
        if (!name) return "none";
        return std::string(b.kind == Kind::AxisPositive ? "+" : "-") + name;
    }
    default: return "none";
    }
}

const char* AccessoryName(Accessory a) {
    switch (a) {
    case Accessory::None:      return "none";
    case Accessory::RumblePak: return "rumble";
    default:                   return "automatic";
    }
}

// How far this binding is pressed, 0..1.
float Amount(SDL_GameController* controller, const PadBinding& b) {
    switch (b.kind) {
    case Kind::Button:
        return SDL_GameControllerGetButton(controller, static_cast<SDL_GameControllerButton>(b.index)) ? 1.0f : 0.0f;
    case Kind::AxisPositive:
    case Kind::AxisNegative: {
        float v =
            static_cast<float>(SDL_GameControllerGetAxis(controller, static_cast<SDL_GameControllerAxis>(b.index))) /
            32767.0f;
        if (b.kind == Kind::AxisNegative) v = -v;
        return std::clamp(v, 0.0f, 1.0f);
    }
    default: return 0.0f;
    }
}

long long FileStamp(const char* path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) return -1;
    return static_cast<long long>(time.time_since_epoch().count());
}

} // namespace

std::string PadBindingName(const PadBinding& binding) {
    switch (binding.kind) {
    case Kind::Button:       return ButtonLabel(binding.index);
    case Kind::AxisPositive: return AxisLabel(binding.index, true);
    case Kind::AxisNegative: return AxisLabel(binding.index, false);
    default:                 return "(none)";
    }
}

// ============================================================================
// ControllerSettings
// ============================================================================

void ControllerSettings::ResetBindings() {
    for (size_t i = 0; i < kN64InputCount; ++i) {
        primary[i] = DefaultBinding(i).primary;
        alternate[i] = DefaultBinding(i).alternate;
    }
}

bool ControllerSettings::LoadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) return false;

    std::string line;
    while (std::getline(file, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string value = Trim(line.substr(eq + 1));

        if (key == "Deadzone" || key == "Range") {
            char* end = nullptr;
            const long v = std::strtol(value.c_str(), &end, 10);
            if (end == value.c_str()) continue;
            if (key == "Deadzone") deadzone = static_cast<int>(std::clamp(v, 0L, 90L));
            else range = static_cast<int>(std::clamp(v, 10L, 150L));
            continue;
        }
        if (key.size() == 16 && key.compare(0, 6, "Player") == 0 && key.compare(7, 9, "Accessory") == 0) {
            const int player = key[6] - '1';
            if (player < 0 || player >= kMaxPlayers) continue;
            if (value == "none") accessory[player] = Accessory::None;
            else if (value == "rumble") accessory[player] = Accessory::RumblePak;
            else accessory[player] = Accessory::Automatic;
            continue;
        }
        if (key.size() == 11 && key.compare(0, 6, "Player") == 0 && key.compare(7, 4, "Guid") == 0) {
            const int player = key[6] - '1';
            if (player < 0 || player >= kMaxPlayers) continue;
            preferredGuid[player] = (value == "none") ? std::string() : value;
            continue;
        }
        for (size_t i = 0; i < kN64InputCount; ++i) {
            if (key != InputIniKey(static_cast<N64Input>(i))) continue;
            const auto comma = value.find(',');
            PadBinding first, second;
            if (!ParseBinding(Trim(value.substr(0, comma)), first)) break;
            if (comma != std::string::npos && !ParseBinding(Trim(value.substr(comma + 1)), second)) break;
            primary[i] = first;
            alternate[i] = second;
            break;
        }
    }
    return true;
}

bool ControllerSettings::SaveToFile(const std::string& path) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file) return false;

    file << "# Game controller settings\n"
         << "# N64 input = binding[, alternate]. A binding is an SDL button name (a, b, x, y,\n"
         << "# back, start, leftshoulder, dpup, ...), an axis with a direction (+leftx,\n"
         << "# -lefty, +triggerleft, ...), or none.\n"
         << "Deadzone = " << deadzone << "\n"
         << "Range = " << range << "\n";
    for (int p = 0; p < kMaxPlayers; ++p)
        file << "Player" << (p + 1) << "Accessory = " << AccessoryName(accessory[p]) << "\n";
    file << "# Which physical controller (by its stable ID) each player slot prefers.\n"
         << "# none takes whichever controller connects first, in whatever order the\n"
         << "# operating system reports them -- including a controller nobody is actually\n"
         << "# using, e.g. a stale Bluetooth pairing macOS still calls connected.\n";
    for (int p = 0; p < kMaxPlayers; ++p)
        file << "Player" << (p + 1) << "Guid = " << (preferredGuid[p].empty() ? "none" : preferredGuid[p]) << "\n";
    for (size_t i = 0; i < kN64InputCount; ++i) {
        file << InputIniKey(static_cast<N64Input>(i)) << " = " << FormatBinding(primary[i]);
        if (alternate[i].kind != Kind::None) file << ", " << FormatBinding(alternate[i]);
        file << "\n";
    }
    return static_cast<bool>(file);
}

// ============================================================================
// Stick
// ============================================================================

void StickToN64(float x, float y, int deadzonePercent, int rangePercent, int8_t& outX, int8_t& outY) {
    outX = outY = 0;
    const float magnitude = std::hypot(x, y);
    const float dead = static_cast<float>(std::clamp(deadzonePercent, 0, 90)) / 100.0f;
    if (magnitude <= dead || magnitude <= 0.0f) return;

    const float push = (std::min(magnitude, 1.0f) - dead) / (1.0f - dead);
    const float limit = std::min(127.0f, 80.0f * static_cast<float>(std::clamp(rangePercent, 10, 150)) / 100.0f);

    // Modern sticks have a round gate; the N64's is an octagon. Stretch the
    // circle to a square, then cut the corners so a full diagonal lands where
    // a real controller's does (about 69, 69).
    const float ux = x / magnitude;
    const float uy = y / magnitude;
    const float toSquare = 1.0f / std::max(std::abs(ux), std::abs(uy));
    float nx = ux * toSquare * push * limit;
    float ny = uy * toSquare * push * limit;
    const float corner = limit * 1.725f;
    const float sum = std::abs(nx) + std::abs(ny);
    if (sum > corner) {
        nx *= corner / sum;
        ny *= corner / sum;
    }
    outX = static_cast<int8_t>(std::lround(std::clamp(nx, -limit, limit)));
    outY = static_cast<int8_t>(std::lround(std::clamp(ny, -limit, limit)));
}

// ============================================================================
// Gamepads
// ============================================================================

Gamepads& Gamepads::Instance() {
    static Gamepads s_instance;
    return s_instance;
}

void Gamepads::Init() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) return;
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) return;
    m_initialized = true;
    // Controllers connected before this point; later ones arrive as events.
    for (int i = 0; i < SDL_NumJoysticks(); ++i) OpenLocked(i);
}

void Gamepads::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) return;
    for (auto& pad : m_pads) {
        if (pad.controller) SDL_GameControllerClose(pad.controller);
        pad = Pad{};
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    m_initialized = false;
}

void Gamepads::OpenLocked(int deviceIndex) {
    if (!SDL_IsGameController(deviceIndex)) return;
    const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(deviceIndex);
    for (const auto& pad : m_pads)
        if (pad.controller && pad.id == id) return; // already open

    SDL_JoystickGUID sdlGuid = SDL_JoystickGetDeviceGUID(deviceIndex);
    char guidBuf[33] = {};
    SDL_JoystickGetGUIDString(sdlGuid, guidBuf, sizeof(guidBuf));

    RefreshSettingsLocked();

    // This device's preferred slot, if it has one and the slot isn't already
    // held by this same device (can't happen here, but keeps the intent clear).
    int preferred = -1;
    for (int p = 0; p < kMaxPlayers; ++p) {
        if (!m_settings.preferredGuid[p].empty() && m_settings.preferredGuid[p] == guidBuf) {
            preferred = p;
            break;
        }
    }

    Pad* slot = nullptr;
    if (preferred >= 0) {
        slot = &m_pads[preferred];
        if (slot->controller) {
            // Someone else is sitting in this device's preferred slot (most
            // often: nothing was configured yet and slot 0 filled in
            // enumeration order). Move them out to a free slot rather than
            // leaving them stranded; if there truly is no free slot, four
            // controllers are already connected and this one has nowhere to
            // go regardless of preference.
            auto free = std::find_if(m_pads.begin(), m_pads.end(), [](const Pad& p) { return !p.controller; });
            if (free == m_pads.end()) return;
            *free = std::move(*slot);
            *slot = Pad{};
        }
    } else {
        auto free = std::find_if(m_pads.begin(), m_pads.end(), [](const Pad& p) { return !p.controller; });
        if (free == m_pads.end()) return; // four players already
        slot = &*free;
    }

    SDL_GameController* controller = SDL_GameControllerOpen(deviceIndex);
    if (!controller) return;
    slot->controller = controller;
    slot->id = id;
    const char* name = SDL_GameControllerName(controller);
    slot->name = (name && *name) ? name : "Game controller";
    slot->guid = guidBuf;
    slot->canRumble = SDL_GameControllerHasRumble(controller) == SDL_TRUE;
    slot->rumbling = false;
}

void Gamepads::HandleEvent(const SDL_Event& event) {
    if (event.type == SDL_CONTROLLERDEVICEADDED) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized) OpenLocked(event.cdevice.which);
    } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& pad : m_pads) {
            if (pad.controller && pad.id == event.cdevice.which) {
                SDL_GameControllerClose(pad.controller);
                pad = Pad{};
            }
        }
    }
}

bool Gamepads::Connected(int player) {
    if (player < 0 || player >= kMaxPlayers) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pads[player].controller != nullptr;
}

void Gamepads::RefreshSettingsLocked() {
    // Re-read the settings file at most once a second, so a change made in
    // the launcher applies to a game that is already running.
    const uint32_t now = SDL_GetTicks();
    if (m_settingsChecked && now - m_settingsCheckedAt < 1000) return;
    m_settingsChecked = true;
    m_settingsCheckedAt = now;

    const long long stamp = FileStamp(kControllerSettingsPath);
    if (stamp == m_settingsStamp) return;
    ControllerSettings fresh;
    if (stamp >= 0) fresh.LoadFromFile(kControllerSettingsPath);
    m_settings = fresh;
    m_settingsStamp = stamp;
}

void Gamepads::ReloadSettings() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_settingsChecked = false;
    m_settingsStamp = -2; // differs from any real stamp, and from "no file"
}

void Gamepads::Apply(int player, PIF::ControllerState& state) {
    if (player < 0 || player >= kMaxPlayers) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    SDL_GameController* controller = m_pads[player].controller;
    if (!controller) return;
    RefreshSettingsLocked();
    const ControllerSettings& s = m_settings;

    auto amount = [&](N64Input input) {
        const size_t i = static_cast<size_t>(input);
        return std::max(Amount(controller, s.primary[i]), Amount(controller, s.alternate[i]));
    };

    static const std::pair<N64Input, uint16_t> kButtons[] = {
        {N64Input::A, N64Button::A},
        {N64Input::B, N64Button::B},
        {N64Input::Z, N64Button::Z},
        {N64Input::Start, N64Button::Start},
        {N64Input::DPadUp, N64Button::DPadUp},
        {N64Input::DPadDown, N64Button::DPadDn},
        {N64Input::DPadLeft, N64Button::DPadLt},
        {N64Input::DPadRight, N64Button::DPadRt},
        {N64Input::L, N64Button::L},
        {N64Input::R, N64Button::R},
        {N64Input::CUp, N64Button::CUp},
        {N64Input::CDown, N64Button::CDown},
        {N64Input::CLeft, N64Button::CLeft},
        {N64Input::CRight, N64Button::CRight},
    };
    uint16_t buttons = 0;
    for (const auto& [input, bit] : kButtons)
        if (amount(input) >= 0.5f) buttons |= bit; // triggers and stick directions: half a push
    state.buttons |= buttons;

    const float x = amount(N64Input::StickRight) - amount(N64Input::StickLeft);
    const float y = amount(N64Input::StickUp) - amount(N64Input::StickDown);
    int8_t sx = 0, sy = 0;
    StickToN64(x, y, s.deadzone, s.range, sx, sy);
    if (std::abs(sx) > std::abs(state.joystick_x)) state.joystick_x = sx;
    if (std::abs(sy) > std::abs(state.joystick_y)) state.joystick_y = sy;
}

std::vector<ConnectedPad> Gamepads::List() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ConnectedPad> list;
    for (int p = 0; p < kMaxPlayers; ++p) {
        if (!m_pads[p].controller) continue;
        list.push_back({p, m_pads[p].name, m_pads[p].guid, m_pads[p].canRumble});
    }
    return list;
}

bool Gamepads::SwapPlayers(int playerA, int playerB) {
    if (playerA < 0 || playerA >= kMaxPlayers || playerB < 0 || playerB >= kMaxPlayers) return false;
    if (playerA == playerB) return true;
    std::lock_guard<std::mutex> lock(m_mutex);
    std::swap(m_pads[playerA], m_pads[playerB]);
    return true;
}

bool Gamepads::AnyButtonHeld() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& pad : m_pads) {
        if (!pad.controller) continue;
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
            if (SDL_GameControllerGetButton(pad.controller, static_cast<SDL_GameControllerButton>(b))) return true;
        if (SDL_GameControllerGetAxis(pad.controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000 ||
            SDL_GameControllerGetAxis(pad.controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000)
            return true;
    }
    return false;
}

bool Gamepads::HasRumblePak(int player) {
    if (player < 0 || player >= kMaxPlayers) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    RefreshSettingsLocked();
    switch (m_settings.accessory[player]) {
    case Accessory::None:      return false;
    case Accessory::RumblePak: return true;
    default:                   return m_pads[player].controller && m_pads[player].canRumble;
    }
}

void Gamepads::SetRumble(int player, bool on) {
    if (player < 0 || player >= kMaxPlayers) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    Pad& pad = m_pads[player];
    if (!pad.controller || !pad.canRumble) return;
    // Games switch the motor on and off many times a second. Send changes, and
    // re-send "on" a few times a second since SDL stops after the duration.
    const uint32_t now = SDL_GetTicks();
    if (on == pad.rumbling && (!on || now - pad.rumbleSentAt < 250)) return;
    SDL_GameControllerRumble(pad.controller, on ? 0xFFFF : 0, on ? 0xFFFF : 0, on ? 500 : 0);
    pad.rumbling = on;
    pad.rumbleSentAt = now;
}

void Gamepads::StopRumble() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& pad : m_pads) {
        if (pad.controller && pad.rumbling) {
            SDL_GameControllerRumble(pad.controller, 0, 0, 0);
            pad.rumbling = false;
        }
    }
}

} // namespace NovaInput
