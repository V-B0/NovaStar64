/**
 * @file GamepadInput.hpp
 * @brief Game controllers (SDL_GameController) as N64 controllers: bindings,
 *        analog stick, player slots and the Rumble Pak.
 */
#pragma once

#include "../N64SYS/Memoryhandler/PifInterface.hpp"
#include "KeyBindings.hpp"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct _SDL_GameController;
union SDL_Event;

namespace NovaInput {

constexpr int kMaxPlayers = 4;

constexpr const char* kControllerSettingsPath = "settings/controllers.ini";

/// One input on a game controller: a button, or one direction of an axis.
struct PadBinding {
    enum class Kind : uint8_t { None, Button, AxisPositive, AxisNegative };
    Kind kind = Kind::None;
    int index = 0; ///< SDL_GameControllerButton or SDL_GameControllerAxis

    bool operator==(const PadBinding& other) const {
        return kind == other.kind && (kind == Kind::None || index == other.index);
    }
    bool operator!=(const PadBinding& other) const { return !(*this == other); }
};

/// "A Button", "Left Stick Up", "(none)". Button names follow the Xbox layout.
std::string PadBindingName(const PadBinding& binding);

/// What sits in a player's controller slot.
enum class Accessory : uint8_t {
    Automatic, ///< a Rumble Pak when that player's controller can rumble
    None,
    RumblePak,
};

struct ControllerSettings {
    std::array<PadBinding, kN64InputCount> primary{};
    std::array<PadBinding, kN64InputCount> alternate{};
    int deadzone = 12; ///< percent of stick travel ignored around the centre
    int range = 100;   ///< percent; 100 reaches a real N64 stick's edge
    std::array<Accessory, kMaxPlayers> accessory{};
    /// SDL_JoystickGUID string of the controller that should get this player
    /// slot, empty for "whichever connects first". A device whose preferred
    /// slot is occupied by an unpreferred device bumps that device elsewhere.
    /// Needed because SDL enumerates in whatever order the OS reports devices
    /// — including a stale Bluetooth pairing macOS still calls "connected" —
    /// so without this, a controller nobody is touching can silently claim
    /// player 1 ahead of the one the player is actually holding.
    std::array<std::string, kMaxPlayers> preferredGuid{};

    ControllerSettings() { ResetBindings(); }
    void ResetBindings();

    /// Keeps the current value for anything missing or malformed.
    bool LoadFromFile(const std::string& path);
    bool SaveToFile(const std::string& path) const;
};

/// Converts a stick position (-1..1 per axis, +y up) into N64 stick values:
/// radial deadzone, then the N64's octagonal gate (80 straight, about 69 on
/// the diagonals at 100% range).
void StickToN64(float x, float y, int deadzonePercent, int rangePercent, int8_t& outX, int8_t& outY);

struct ConnectedPad {
    int player = 0;   ///< 0-3
    std::string name;
    std::string guid; ///< SDL_JoystickGUID string; stable across reconnects
    bool canRumble = false;
};

/// The connected game controllers. Controllers are opened and closed on the
/// thread that pumps SDL events; the emulator reads them from its own thread.
/// A new controller takes its ControllerSettings::preferredGuid slot if it has
/// one and that slot is free (bumping whatever non-preferred device is there),
/// otherwise the first free slot; it keeps that slot until it is disconnected.
class Gamepads {
public:
    static Gamepads& Instance();

    /// Starts SDL's game controller subsystem and opens connected controllers.
    /// Call on the thread that pumps SDL events.
    void Init();
    void Shutdown();
    /// Opens and closes controllers as they are connected and disconnected.
    void HandleEvent(const SDL_Event& event);

    [[nodiscard]] bool Connected(int player);
    /// Adds this player's controller to `state`: buttons are combined, and the
    /// stick keeps whichever push is larger on each axis.
    void Apply(int player, PIF::ControllerState& state);
    [[nodiscard]] std::vector<ConnectedPad> List();
    /// True while a button or trigger is held on any controller.
    [[nodiscard]] bool AnyButtonHeld();
    /// Exchanges which controller sits in each of these two player slots
    /// (immediate; does not touch settings). Returns false for an out-of-range
    /// player index.
    bool SwapPlayers(int playerA, int playerB);

    [[nodiscard]] bool HasRumblePak(int player);
    void SetRumble(int player, bool on);
    void StopRumble();

    /// Makes the next read pick up settings saved by the launcher.
    void ReloadSettings();

private:
    struct Pad {
        _SDL_GameController* controller = nullptr;
        int32_t id = -1; ///< SDL_JoystickID
        std::string name;
        std::string guid;
        bool canRumble = false;
        bool rumbling = false;
        uint32_t rumbleSentAt = 0; ///< SDL ticks
    };

    Gamepads() = default;
    void OpenLocked(int deviceIndex);
    void RefreshSettingsLocked();

    std::mutex m_mutex;
    bool m_initialized = false;
    std::array<Pad, kMaxPlayers> m_pads{};

    ControllerSettings m_settings;
    uint32_t m_settingsCheckedAt = 0;
    bool m_settingsChecked = false;
    long long m_settingsStamp = -1; ///< file time the settings were read at
};

} // namespace NovaInput
