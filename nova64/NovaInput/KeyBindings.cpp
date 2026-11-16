/**
 * @file KeyBindings.cpp
 * @brief Keyboard binding defaults and settings-file persistence.
 */
#include "KeyBindings.hpp"

#include <SDL.h>

#include <cstdlib>
#include <fstream>

namespace NovaInput {

namespace {

struct InputInfo {
    const char* label;
    const char* iniKey;
    int defaultPrimary;
    int defaultAlternate;
};

// Indexed by N64Input. B also answers to Y because scancodes are physical key
// positions: on a QWERTZ keyboard the key printed "Z" sits where US "Y" is.
constexpr InputInfo kInputs[kN64InputCount] = {
    {"A Button", "A", SDL_SCANCODE_X, 0},
    {"B Button", "B", SDL_SCANCODE_Z, SDL_SCANCODE_Y},
    {"Z Trigger", "Z", SDL_SCANCODE_C, 0},
    {"Start", "Start", SDL_SCANCODE_RETURN, 0},
    {"D-Pad Up", "DPadUp", SDL_SCANCODE_T, 0},
    {"D-Pad Down", "DPadDown", SDL_SCANCODE_G, 0},
    {"D-Pad Left", "DPadLeft", SDL_SCANCODE_F, 0},
    {"D-Pad Right", "DPadRight", SDL_SCANCODE_H, 0},
    {"L Button", "L", SDL_SCANCODE_LSHIFT, 0},
    {"R Button", "R", SDL_SCANCODE_RSHIFT, 0},
    {"C-Up", "CUp", SDL_SCANCODE_UP, 0},
    {"C-Down", "CDown", SDL_SCANCODE_DOWN, 0},
    {"C-Left", "CLeft", SDL_SCANCODE_LEFT, 0},
    {"C-Right", "CRight", SDL_SCANCODE_RIGHT, 0},
    {"Stick Up", "StickUp", SDL_SCANCODE_W, 0},
    {"Stick Down", "StickDown", SDL_SCANCODE_S, 0},
    {"Stick Left", "StickLeft", SDL_SCANCODE_A, 0},
    {"Stick Right", "StickRight", SDL_SCANCODE_D, 0},
};

bool ParseScancode(const std::string& text, int& out) {
    const char* begin = text.c_str();
    char* end = nullptr;
    long v = std::strtol(begin, &end, 10);
    if (end == begin || v < 0 || v >= SDL_NUM_SCANCODES) return false;
    out = static_cast<int>(v);
    return true;
}

std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

} // namespace

const char* InputLabel(N64Input input) {
    const auto i = static_cast<size_t>(input);
    return i < kN64InputCount ? kInputs[i].label : "?";
}

const char* InputIniKey(N64Input input) {
    const auto i = static_cast<size_t>(input);
    return i < kN64InputCount ? kInputs[i].iniKey : "?";
}

void KeyBindings::ResetToDefaults() {
    for (size_t i = 0; i < kN64InputCount; ++i) {
        keys[i].primary = kInputs[i].defaultPrimary;
        keys[i].alternate = kInputs[i].defaultAlternate;
    }
}

bool KeyBindings::LoadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) return false;

    std::string line;
    while (std::getline(file, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string value = Trim(line.substr(eq + 1));

        for (size_t i = 0; i < kN64InputCount; ++i) {
            if (key != kInputs[i].iniKey) continue;
            const auto comma = value.find(',');
            KeyBinding parsed;
            if (!ParseScancode(Trim(value.substr(0, comma)), parsed.primary)) break;
            if (comma != std::string::npos && !ParseScancode(Trim(value.substr(comma + 1)), parsed.alternate)) break;
            keys[i] = parsed;
            break;
        }
    }
    return true;
}

bool KeyBindings::SaveToFile(const std::string& path) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file) return false;

    file << "# N64 input = SDL scancode[, alternate scancode]\n";
    for (size_t i = 0; i < kN64InputCount; ++i) {
        file << kInputs[i].iniKey << " = " << keys[i].primary;
        if (keys[i].alternate) file << ", " << keys[i].alternate;
        file << "\n";
    }
    return static_cast<bool>(file);
}

} // namespace NovaInput
