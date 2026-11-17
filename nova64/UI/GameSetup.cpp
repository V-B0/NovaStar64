#include "GameSetup.hpp"
#include "GameFiles.hpp"
#include "NovaLauncher.hpp"

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace Nova64::Video::VK {
bool IsVulkanAvailable();
}

namespace NovaUI {

float FitInterfaceScale(int percent, int displayIndex) {
    float scale = static_cast<float>(percent) / 100.0f;
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(std::max(0, displayIndex), &usable) == 0 && usable.w > 0 && usable.h > 0)
        scale = std::min(scale, std::max(1.0f, std::min(static_cast<float>(usable.w) / kMinWindowWidth,
                                                        static_cast<float>(usable.h) / kMinWindowHeight)));
    return scale;
}

namespace {

using Nova64::Video::AspectRatio;
using Nova64::Video::RenderingMode;

const char* DriverUnavailable(int backend) {
    static const bool kVulkan = Nova64::Video::VK::IsVulkanAvailable();
    if (backend == 1) return kVulkan ? nullptr : "No Vulkan driver was found.";
#ifdef __APPLE__
    return "macOS only supports OpenGL up to 4.1; this renderer needs 4.6.";
#else
    return nullptr;
#endif
}

const char* DriverWarning(const SettingsModel& m) {
    const char* why = DriverUnavailable(m.gfx->backendType);
    if (!why) return nullptr;
    return m.gfx->backendType == 1 ? "Vulkan isn't available here; games won't start until you pick another driver."
                                   : "OpenGL isn't available here; games won't start until you pick another driver.";
}

// The interface is laid out for a window of at least 960x600 of its own units, so a size is only offered when that
// window fits on the screen.
const char* ScaleUnavailable(int percent) {
    return FitInterfaceScale(percent, 0) < static_cast<float>(percent) / 100.0f - 0.001f
               ? "This screen is too small for that size."
               : nullptr;
}

const char* VulkanUnavailable(const SettingsModel& m) {
    return m.gfx->backendType == 1 ? nullptr : "Needs the Vulkan driver.";
}

template <int Player> int GetAccessory(const SettingsModel& m) { return m.accessory ? m.accessory[Player] : 0; }

template <int Player> void SetAccessory(SettingsModel& m, int v) {
    if (m.accessory) m.accessory[Player] = v;
}

std::vector<OptionDef> BuildOptions() {
    std::vector<OptionDef> o;

    // --- Emulation ---
    {
        OptionDef d{"speed",     "Speed",       "How fast the game runs. Double speed is also on the Tab key.",
                    "emulation", "Performance", OptionKind::Choice};
        d.choices = {"Half speed", "Full speed", "Double speed", "Unlimited"};
        d.values = {30, 60, 120, 0};
        d.get = [](const SettingsModel& m) { return m.launch->speed; };
        d.set = [](SettingsModel& m, int v) { m.launch->speed = v; };
        o.push_back(d);
    }
    {
        OptionDef d{
            "cpuCore",   "CPU core",    "Automatic uses the recompiler on x86-64 and the interpreter on Apple Silicon.",
            "emulation", "Performance", OptionKind::Choice};
        d.choices = {"Automatic", "Interpreter", "Dynamic recompiler"};
        d.get = [](const SettingsModel& m) { return m.launch->cpuCore; };
        d.set = [](SettingsModel& m, int v) { m.launch->cpuCore = v; };
        o.push_back(d);
    }
    {
        OptionDef d{"rspEngine", "RSP engine",  "Runs the graphics and audio microcode. Both give identical results.",
                    "emulation", "Performance", OptionKind::Choice};
        d.choices = {"Recompiler (faster)", "Interpreter"};
        d.get = [](const SettingsModel& m) { return m.launch->rspEngine; };
        d.set = [](SettingsModel& m, int v) { m.launch->rspEngine = v; };
        o.push_back(d);
    }
    {
        OptionDef d{
            "rewind",    "Rewind",        "Keeps a picture every 2 seconds so Backspace can go back; about 1 MB each.",
            "emulation", "While playing", OptionKind::Toggle};
        d.applies = Applies::Now;
        d.get = [](const SettingsModel& m) { return m.launch->rewind ? 1 : 0; };
        d.set = [](SettingsModel& m, int v) { m.launch->rewind = v != 0; };
        o.push_back(d);
    }
    {
        OptionDef d{
            "pauseOnFocusLoss", "Pause in the background", "Pauses the game when another application becomes active.",
            "emulation",        "While playing",           OptionKind::Toggle};
        d.applies = Applies::Now;
        d.perGame = false;
        d.get = [](const SettingsModel& m) { return m.launch->pauseOnFocusLoss ? 1 : 0; };
        d.set = [](SettingsModel& m, int v) { m.launch->pauseOnFocusLoss = v != 0; };
        o.push_back(d);
    }
    {
        OptionDef d{"saveChip",
                    "Save chip",
                    "Which kind of cartridge save the game uses. Leave on automatic unless saves do not stick.",
                    "emulation",
                    "Cartridge",
                    OptionKind::Choice};
        d.choices = {"Detect from the game", "SRAM", "FlashRAM"};
        d.store = Store::None;
        d.globalSetting = false;
        d.get = [](const SettingsModel& m) { return m.saveChip ? *m.saveChip : 0; };
        d.set = [](SettingsModel& m, int v) {
            if (m.saveChip) *m.saveChip = v;
        };
        o.push_back(d);
    }

    // The pak in each player's controller. The Simulator sets these from what is plugged into the controllers.
    {
        static const char* const kTitles[4] = {"Player 1 pak", "Player 2 pak", "Player 3 pak", "Player 4 pak"};
        static const char* const kIds[4] = {"accessory1", "accessory2", "accessory3", "accessory4"};
        int (*const getters[4])(const SettingsModel&) = {GetAccessory<0>, GetAccessory<1>, GetAccessory<2>,
                                                         GetAccessory<3>};
        void (*const setters[4])(SettingsModel&, int) = {SetAccessory<0>, SetAccessory<1>, SetAccessory<2>,
                                                         SetAccessory<3>};
        for (int p = 0; p < 4; ++p) {
            OptionDef d{kIds[p],     kTitles[p],    "What is in this controller's pak slot.",
                        "emulation", "Controllers", OptionKind::Choice};
            d.choices = {"Automatic", "Nothing", "Rumble Pak"};
            d.store = Store::None;
            d.globalSetting = false;
            d.get = getters[p];
            d.set = setters[p];
            o.push_back(d);
        }
    }

    // --- Video ---
    {
        OptionDef d{"displayMode", "Play games", "Inside the launcher window, or fullscreen while a game runs.",
                    "video",       "Window",     OptionKind::Choice};
        d.choices = {"In the launcher window", "Fullscreen"};
        d.get = [](const SettingsModel& m) { return m.launch->displayMode; };
        d.set = [](SettingsModel& m, int v) { m.launch->displayMode = v; };
        o.push_back(d);
    }
    {
        OptionDef d{"uiScale", "Interface size", "Scales the text and everything around it together.",
                    "video",   "Window",         OptionKind::Choice};
        d.choices = {"Normal (100%)", "Large (125%)", "Larger (150%)", "Largest (200%)"};
        d.values = {100, 125, 150, 200};
        d.applies = Applies::Now;
        d.perGame = false;
        d.choiceUnavailable = ScaleUnavailable;
        d.get = [](const SettingsModel& m) { return m.launch->uiScale; };
        d.set = [](SettingsModel& m, int v) { m.launch->uiScale = v; };
        o.push_back(d);
    }
    {
        OptionDef d{"showFps", "Show frame rate", "Puts the frame rate in the bar above the game.",
                    "video",   "Window",          OptionKind::Toggle};
        d.applies = Applies::Now;
        d.perGame = false;
        d.get = [](const SettingsModel& m) { return m.launch->showFps ? 1 : 0; };
        d.set = [](SettingsModel& m, int v) { m.launch->showFps = v != 0; };
        o.push_back(d);
    }
    {
        OptionDef d{"backendType", "Graphics driver", "Vulkan is recommended. OpenGL is not available on macOS.",
                    "video",       "Picture",         OptionKind::Choice};
        d.choices = {"Vulkan", "OpenGL"};
        d.values = {1, 0};
        d.store = Store::Graphics;
        d.choiceUnavailable = DriverUnavailable;
        d.warning = DriverWarning;
        d.get = [](const SettingsModel& m) { return m.gfx->backendType; };
        d.set = [](SettingsModel& m, int v) { m.gfx->backendType = v; };
        o.push_back(d);
    }
    {
        OptionDef d{"internalResScale", "Resolution",      "Multiplies the console's native resolution.", "video",
                    "Picture",          OptionKind::Slider};
        d.min = 1;
        d.max = 8;
        d.format = "%dx";
        d.store = Store::Graphics;
        d.get = [](const SettingsModel& m) { return m.gfx->internalResScale; };
        d.set = [](SettingsModel& m, int v) { m.gfx->internalResScale = v; };
        o.push_back(d);
    }
    {
        OptionDef d{"widescreen", "Widescreen (16:9)", "Widens the camera. Some games show gaps at the edges.",
                    "video",      "Picture",           OptionKind::Toggle};
        d.store = Store::Graphics;
        d.get = [](const SettingsModel& m) { return m.gfx->aspectRatio == AspectRatio::Widescreen16_9 ? 1 : 0; };
        d.set = [](SettingsModel& m, int v) {
            m.gfx->aspectRatio = v ? AspectRatio::Widescreen16_9 : AspectRatio::Native4_3;
        };
        o.push_back(d);
    }
    {
        OptionDef d{"enableMipmaps", "Mipmapping", "Smoother textures in the distance.",
                    "video",         "Textures",   OptionKind::Toggle};
        d.store = Store::Graphics;
        d.get = [](const SettingsModel& m) { return m.gfx->enableMipmaps ? 1 : 0; };
        d.set = [](SettingsModel& m, int v) { m.gfx->enableMipmaps = v != 0; };
        o.push_back(d);
    }
    {
        OptionDef d{"hiResTexEnabled", "Texture packs", "Loads hires_texture/<game name>/ in the Rice format.",
                    "video",           "Textures",      OptionKind::Toggle};
        d.store = Store::Graphics;
        d.get = [](const SettingsModel& m) { return m.gfx->hiResTexEnabled ? 1 : 0; };
        d.set = [](SettingsModel& m, int v) { m.gfx->hiResTexEnabled = v != 0; };
        o.push_back(d);
    }
    {
        OptionDef d{
            "textureDump", "Save textures for a pack", "Writes texture_dump/<game name>/ while you play.", "video",
            "Textures",    OptionKind::Toggle};
        d.store = Store::Graphics;
        d.perGame = false;
        d.get = [](const SettingsModel& m) { return m.gfx->textureDump ? 1 : 0; };
        d.set = [](SettingsModel& m, int v) { m.gfx->textureDump = v != 0; };
        o.push_back(d);
    }
    {
        OptionDef d{"rayTracing", "Ray-traced lighting", "Vulkan with high-level graphics only.", "video",
                    "Picture",    OptionKind::Toggle};
        d.store = Store::Graphics;
        d.experimental = true;
        d.get = [](const SettingsModel& m) { return m.gfx->rayTracing ? 1 : 0; };
        d.set = [](SettingsModel& m, int v) { m.gfx->rayTracing = v != 0; };
        d.unavailable = VulkanUnavailable;
        o.push_back(d);
    }
    {
        OptionDef d{"renderingMode",
                    "Graphics emulation",
                    "Low-level runs the real RSP microcode. It is slower and still shows seams.",
                    "video",
                    "Accuracy",
                    OptionKind::Choice};
        d.choices = {"High-level (recommended)", "Low-level (experimental)"};
        d.values = {static_cast<int>(RenderingMode::HLE), static_cast<int>(RenderingMode::LLE)};
        d.store = Store::Graphics;
        d.get = [](const SettingsModel& m) { return static_cast<int>(m.gfx->renderingMode); };
        d.set = [](SettingsModel& m, int v) { m.gfx->renderingMode = static_cast<RenderingMode>(v); };
        o.push_back(d);
    }
    {
        OptionDef d{"fbReadAccess",
                    "Copy frames back to game memory",
                    "For games that read the screen, such as pause-menu backgrounds.",
                    "video",
                    "Accuracy",
                    OptionKind::Toggle};
        d.store = Store::Graphics;
        d.get = [](const SettingsModel& m) { return m.gfx->fbReadAccess ? 1 : 0; };
        d.set = [](SettingsModel& m, int v) { m.gfx->fbReadAccess = v != 0; };
        o.push_back(d);
    }
    {
        OptionDef d{"fbWriteAccess",
                    "Show frames the game draws itself",
                    "For games that draw the picture on the CPU.",
                    "video",
                    "Accuracy",
                    OptionKind::Toggle};
        d.store = Store::Graphics;
        d.get = [](const SettingsModel& m) { return m.gfx->fbWriteAccess ? 1 : 0; };
        d.set = [](SettingsModel& m, int v) { m.gfx->fbWriteAccess = v != 0; };
        o.push_back(d);
    }

    // --- Controls ---
    {
        OptionDef d{
            "rumbleShake",
            "Shake the window",
            "How far the window shakes when a game rumbles. Needs a Rumble Pak in the controller; see the Simulator.",
            "controls",
            "Rumble",
            OptionKind::Slider};
        d.min = 0;
        d.max = 100;
        d.format = "%d%%";
        d.applies = Applies::Now;
        d.get = [](const SettingsModel& m) { return m.launch->rumbleShake; };
        d.set = [](SettingsModel& m, int v) { m.launch->rumbleShake = v; };
        o.push_back(d);
    }

    // --- Audio ---
    {
        OptionDef d{"volume", "Volume", "Takes effect immediately, including in a game that is running.",
                    "audio",  "Output", OptionKind::Slider};
        d.min = 0;
        d.max = 100;
        d.format = "%d%%";
        d.applies = Applies::Now;
        d.perGame = false;
        d.get = [](const SettingsModel& m) { return m.launch->volume; };
        d.set = [](SettingsModel& m, int v) { m.launch->volume = v; };
        o.push_back(d);
    }
    {
        OptionDef d{"muted", "Mute",   "Silences the game without changing the volume.",
                    "audio", "Output", OptionKind::Toggle};
        d.applies = Applies::Now;
        d.perGame = false;
        d.get = [](const SettingsModel& m) { return m.launch->muted ? 1 : 0; };
        d.set = [](SettingsModel& m, int v) { m.launch->muted = v != 0; };
        o.push_back(d);
    }
    return o;
}

/// The value an override file may give an option, or false if the option cannot have it. A hand-edited file must not
/// be able to push a value the interface cannot show, or the emulator cannot use, into a game.
bool ValidValue(const OptionDef& option, long raw, int* out) {
    if (raw < -1000000000L || raw > 1000000000L) return false;
    const int value = static_cast<int>(raw);
    switch (option.kind) {
    case OptionKind::Toggle: *out = value != 0 ? 1 : 0; return true;
    case OptionKind::Slider: *out = std::clamp(value, option.min, option.max); return true;
    case OptionKind::Choice:
        if (option.values.empty() ? (value < 0 || value >= static_cast<int>(option.choices.size()))
                                  : std::find(option.values.begin(), option.values.end(), value) == option.values.end())
            return false;
        *out = value;
        return true;
    }
    return false;
}

} // namespace

const std::vector<OptionDef>& AllOptions() {
    static const std::vector<OptionDef> kOptions = BuildOptions();
    return kOptions;
}

const OptionDef* FindOption(const std::string& id) {
    for (const OptionDef& o : AllOptions())
        if (id == o.id) return &o;
    return nullptr;
}

int ChoiceIndex(const OptionDef& option, int value) {
    if (option.values.empty()) return std::clamp(value, 0, std::max(0, static_cast<int>(option.choices.size()) - 1));
    for (size_t i = 0; i < option.values.size(); ++i)
        if (option.values[i] == value) return static_cast<int>(i);
    return 0;
}

int ChoiceValue(const OptionDef& option, int index) {
    if (option.values.empty()) return index;
    return option.values[static_cast<size_t>(std::clamp(index, 0, static_cast<int>(option.values.size()) - 1))];
}

std::string ValueText(const OptionDef& option, int value) {
    switch (option.kind) {
    case OptionKind::Toggle: return value ? "On" : "Off";
    case OptionKind::Choice:
        return option.choices.empty() ? std::string() : option.choices[static_cast<size_t>(ChoiceIndex(option, value))];
    case OptionKind::Slider: {
        char text[32];
        std::snprintf(text, sizeof(text), option.format, value);
        return text;
    }
    }
    return {};
}

std::string GameSetup::PathFor(const std::string& romPath) {
    return (fs::path("settings") / "games" / (GameKey(romPath) + ".ini")).generic_string();
}

bool GameSetup::Load(const std::string& romPath) {
    m_values.clear();
    std::ifstream f(PathFor(romPath));
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        std::string key = line.substr(0, eq);
        key.erase(key.find_last_not_of(" \t") + 1);
        const OptionDef* option = FindOption(key);
        if (!option || !option->perGame) continue; // an old or hand-edited key: ignore it
        const std::string text = line.substr(eq + 1);
        char* end = nullptr;
        const long raw = std::strtol(text.c_str(), &end, 10);
        int value = 0;
        if (end == text.c_str() || !ValidValue(*option, raw, &value)) continue;
        m_values[key] = value;
    }
    return true;
}

bool GameSetup::Save(const std::string& romPath) const {
    const std::string path = PathFor(romPath);
    std::error_code ec;
    if (m_values.empty()) {
        fs::remove(path, ec);
        return !ec;
    }
    std::ostringstream f;
    f << "# Options this game overrides; anything not listed follows the global settings.\n";
    for (const auto& [id, value] : m_values) f << id << " = " << value << "\n";
    return WriteFileAtomically(path, f.str());
}

int GameSetup::Get(const std::string& id, int fallback) const {
    const auto it = m_values.find(id);
    return it == m_values.end() ? fallback : it->second;
}

void GameSetup::Apply(const SettingsModel& model) const {
    SettingsModel target = model;
    for (const auto& [id, value] : m_values)
        if (const OptionDef* option = FindOption(id)) option->set(target, value);
}

bool GameSetup::TouchesGraphics() const {
    for (const auto& [id, value] : m_values) {
        const OptionDef* option = FindOption(id);
        if (option && option->store == Store::Graphics) return true;
    }
    return false;
}

} // namespace NovaUI
