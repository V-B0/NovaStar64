/**
 * @file GameSetup.hpp
 * @brief The launcher's options as one table, and the per-game setups that override them.
 *
 * The Settings page and a game's Setup page are both drawn from this table, so an option is described
 * once: its name, help text, kind of control, where it is stored, and whether a single game may override it.
 * A game's overrides live in settings/games/<rom name>.ini, one "id = value" line per overridden option.
 */

#pragma once

#include "../NovaGraphics/Plugin/PluginConfig.hpp"

#include <map>
#include <string>
#include <vector>

namespace NovaUI {

/// The interface is laid out for a window of at least this many of its own units.
constexpr float kMinWindowWidth = 960.0f;
constexpr float kMinWindowHeight = 600.0f;
/// The default window, in the same units.
constexpr float kDefaultWindowWidth = 1280.0f;
constexpr float kDefaultWindowHeight = 720.0f;

/// The interface scale to use for \p percent on a display: what was asked for, but never more than lets a window of
/// the smallest size fit on that display. Returns a factor (1 = 100%).
float FitInterfaceScale(int percent, int displayIndex);

struct LaunchOptions;

/// The values an option reads and writes. The pointers refer to the launcher's own settings for the Settings page,
/// or to a copy when a game's effective setup is worked out.
struct SettingsModel {
    LaunchOptions* launch = nullptr;
    Nova64::Video::PluginConfig* gfx = nullptr;
    int* saveChip = nullptr;  ///< 0 = detect from the game, 1 = SRAM, 2 = FlashRAM
    int* accessory = nullptr; ///< four values, one per player: 0 = automatic, 1 = nothing, 2 = Rumble Pak
};

enum class OptionKind { Toggle, Choice, Slider };

/// When a changed value reaches a running game.
enum class Applies { Now, NextGame };

/// Which settings file a global value is saved to.
enum class Store { Launcher, Graphics, None };

struct OptionDef {
    const char* id;                   ///< the key in a game's setup file
    const char* title;
    const char* help;                 ///< one sentence under the title
    const char* page;                 ///< "emulation", "video" or "audio"
    const char* group;                ///< heading of the group of rows it sits in
    OptionKind kind;
    std::vector<const char*> choices; ///< Choice: the labels
    std::vector<int> values;          ///< Choice: the value of each label; empty means 0, 1, 2...
    int min = 0;                      ///< Slider
    int max = 1;                      ///< Slider
    const char* format = "%d";        ///< Slider
    Store store = Store::Launcher;
    Applies applies = Applies::NextGame;
    bool perGame = true;       ///< a game may override it
    bool globalSetting = true; ///< shown on the Settings page (false: only in a game's setup)
    bool experimental = false;
    int (*get)(const SettingsModel&) = nullptr;
    void (*set)(SettingsModel&, int) = nullptr;
    /// Why the option cannot be used right now, or null when it can.
    const char* (*unavailable)(const SettingsModel&) = nullptr;
    /// For a Choice: why a value cannot be picked, or null when it can. Null for all of them.
    const char* (*choiceUnavailable)(int value) = nullptr;
    /// A problem with the current value, shown instead of the help text, or null.
    const char* (*warning)(const SettingsModel&) = nullptr;
};

/// Every option, in the order the pages show them.
const std::vector<OptionDef>& AllOptions();
const OptionDef* FindOption(const std::string& id);

/// The default value of an option (what a fresh install has).
int OptionDefault(const OptionDef& option);

/// The index of the Choice label whose value is `value` (0 if there is none).
int ChoiceIndex(const OptionDef& option, int value);
int ChoiceValue(const OptionDef& option, int index);
/// The text of an option's current value, as shown on a row ("Vulkan", "4x", "On").
std::string ValueText(const OptionDef& option, int value);

/// The options one game overrides.
class GameSetup {
public:
    /// settings/games/<rom name>.ini
    static std::string PathFor(const std::string& romPath);

    bool Load(const std::string& romPath);
    /// Writes the file, or removes it when nothing is overridden.
    bool Save(const std::string& romPath) const;

    bool Has(const std::string& id) const { return m_values.count(id) != 0; }
    int Get(const std::string& id, int fallback) const;
    void Set(const std::string& id, int value) { m_values[id] = value; }
    void Clear(const std::string& id) { m_values.erase(id); }
    void ClearAll() { m_values.clear(); }
    bool Empty() const { return m_values.empty(); }
    size_t Count() const { return m_values.size(); }

    /// Writes each override into the model.
    void Apply(const SettingsModel& model) const;
    /// True if any override is stored in the graphics settings file.
    bool TouchesGraphics() const;

private:
    std::map<std::string, int> m_values;
};

} // namespace NovaUI
