/**
 * @file KeyBindings.hpp
 * @brief Keyboard-to-N64 controller bindings, shared by the launcher (which
 *        edits them) and the PIF (which reads them). Game controller bindings
 *        are in GamepadInput.hpp.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace NovaInput {

enum class N64Input : uint8_t {
    A, B, Z, Start,
    DPadUp, DPadDown, DPadLeft, DPadRight,
    L, R,
    CUp, CDown, CLeft, CRight,
    StickUp, StickDown, StickLeft, StickRight,
    Count
};

constexpr size_t kN64InputCount = static_cast<size_t>(N64Input::Count);

constexpr const char* kKeyBindingsPath = "settings/keybindings.ini";

/// SDL scancodes; 0 means unbound.
struct KeyBinding {
    int primary   = 0;
    int alternate = 0;
};

const char* InputLabel(N64Input input);   ///< e.g. "A Button"
const char* InputIniKey(N64Input input);  ///< e.g. "A", the key in settings files

struct KeyBindings {
    std::array<KeyBinding, kN64InputCount> keys;

    KeyBindings() { ResetToDefaults(); }
    void ResetToDefaults();

    KeyBinding&       operator[](N64Input i)       { return keys[static_cast<size_t>(i)]; }
    const KeyBinding& operator[](N64Input i) const { return keys[static_cast<size_t>(i)]; }

    /// Keeps the default for any entry that is missing or malformed.
    bool LoadFromFile(const std::string& path);
    bool SaveToFile(const std::string& path) const;
};

} // namespace NovaInput
