/**
 * @file LauncherSettings.cpp
 * @brief Settings tab: launch options, graphics options and keyboard controls.
 */
#include "NovaLauncher.hpp"
#include "UiKit.hpp"
#include "GameFiles.hpp"
#include "LauncherInternal.hpp"

#include "../N64SYS/MainSYS.hpp"
#include "../NovaGraphics/Plugin/FrameCallback.hpp"
#include "../NovaInput/KeyboardInput.hpp"

#include <SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

extern CN64System* g_System;

namespace Nova64::Video::VK {
bool IsVulkanAvailable();
}

namespace NovaUI {

namespace {

bool ParseInt(const std::string& text, int& out) {
    const char* begin = text.c_str();
    char* end = nullptr;
    long v = std::strtol(begin, &end, 10);
    if (end == begin) return false;
    out = static_cast<int>(v);
    return true;
}

std::string KeyName(int scancode) {
    if (scancode <= 0) return "(none)";
    const auto sc = static_cast<SDL_Scancode>(scancode);
    // Key names follow the active keyboard layout; scancode names are US positions.
    const char* name = SDL_GetKeyName(SDL_GetKeyFromScancode(sc));
    if (!name || !*name) name = SDL_GetScancodeName(sc);
    return (name && *name) ? name : "?";
}

} // namespace

// --- LaunchOptions ---
void LaunchOptions::Load(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        key.erase(key.find_last_not_of(" \t") + 1);
        int v = 0;
        if (!ParseInt(line.substr(eq + 1), v)) continue;
        if (key == "speed") speed = v;
        else if (key == "displayMode") displayMode = v;
        else if (key == "cpuCore") cpuCore = v;
        else if (key == "rspEngine") rspEngine = v;
        else if (key == "rewind") rewind = v != 0;
        else if (key == "pauseWhenInactive") pauseOnFocusLoss = v != 0;
        else if (key == "showFps") showFps = v != 0;
        else if (key == "rumbleShake") rumbleShake = v;
        else if (key == "uiScale") uiScale = v;
        else if (key == "volume") volume = v;
        else if (key == "muted") muted = v != 0;
    }
    if (speed != 0 && speed != 30 && speed != 60 && speed != 120) speed = 60;
    // Older files also used 2, exclusive fullscreen; any fullscreen is 1 now.
    displayMode = (displayMode == 0) ? 0 : 1;
    if (cpuCore < 0 || cpuCore > 2) cpuCore = 0;
    if (rspEngine < 0 || rspEngine > 1) rspEngine = 0;
    if (volume < 0 || volume > 100) volume = 100;
    rumbleShake = std::clamp(rumbleShake, 0, 100);
    if (uiScale != 100 && uiScale != 125 && uiScale != 150 && uiScale != 200) uiScale = 100;
}

bool LaunchOptions::Save(const std::string& path) const {
    std::ostringstream f;
    f << "speed = " << speed << "\n"
      << "displayMode = " << displayMode << "\n"
      << "cpuCore = " << cpuCore << "\n"
      << "rspEngine = " << rspEngine << "\n"
      << "rewind = " << (rewind ? 1 : 0) << "\n"
      << "pauseWhenInactive = " << (pauseOnFocusLoss ? 1 : 0) << "\n"
      << "showFps = " << (showFps ? 1 : 0) << "\n"
      << "rumbleShake = " << rumbleShake << "\n"
      << "uiScale = " << uiScale << "\n"
      << "volume = " << volume << "\n"
      << "muted = " << (muted ? 1 : 0) << "\n";
    return WriteFileAtomically(path, f.str());
}

// --- Load / save ---
void NovaLauncher::LoadSettings() {
    m_gfx.LoadFromFile(detail::kGraphicsPath);
    m_launch.Load(detail::kLauncherPath);
    m_bindings.LoadFromFile(NovaInput::kKeyBindingsPath);
    m_padSettings.LoadFromFile(NovaInput::kControllerSettingsPath);
    ApplyLaunchOptions();
}

void NovaLauncher::SaveLaunchOptions() {
    if (!m_launch.Save(detail::kLauncherPath)) ShowStatus("Couldn't save the settings. Is the data folder writable?");
    ApplyLaunchOptions();
}

void NovaLauncher::ApplyLaunchOptions() {
    Nova64::RSP::HLE::CAudioOutput::SetMasterVolume(static_cast<uint32_t>(m_launch.volume));
    Nova64::RSP::HLE::CAudioOutput::SetMuted(m_launch.muted);
    Nova64::StateService::Instance().SetRewindEnabled(m_launch.rewind);
}

void NovaLauncher::SaveGraphicsSettings() {
    // Start from what is on disk and change only the fields the options own, so anything edited by hand survives.
    Nova64::Video::PluginConfig onDisk;
    onDisk.LoadFromFile(detail::kGraphicsPath);
    onDisk.internalResScale = m_gfx.internalResScale;
    onDisk.aspectRatio = m_gfx.aspectRatio;
    onDisk.rayTracing = m_gfx.rayTracing;
    onDisk.backendType = m_gfx.backendType;
    onDisk.enableMipmaps = m_gfx.enableMipmaps;
    onDisk.hiResTexEnabled = m_gfx.hiResTexEnabled;
    onDisk.textureDump = m_gfx.textureDump;
    onDisk.renderingMode = m_gfx.renderingMode;
    onDisk.fbReadAccess = m_gfx.fbReadAccess;
    onDisk.fbWriteAccess = m_gfx.fbWriteAccess;
    if (!onDisk.SaveToFile(detail::kGraphicsPath))
        ShowStatus("Couldn't save the settings. Is the data folder writable?");
}

void NovaLauncher::SetTurbo(bool enabled) {
    m_turboMode = enabled;
    const int speed = (enabled && m_running.speed != 0) ? m_running.speed * 2 : m_running.speed;
    CN64System::s_TargetFPS = speed;
    if (g_System) g_System->SetSpeed(speed);
}

// --- Controls ---
bool NovaLauncher::HandleRebindEvent(const SDL_Event& event) {
    if (m_rebindTarget < 0) return false;
    if (event.type != SDL_KEYDOWN && event.type != SDL_KEYUP) return false;

    if (event.type == SDL_KEYDOWN && !event.key.repeat) {
        const SDL_Scancode sc = event.key.keysym.scancode;
        if (sc != SDL_SCANCODE_ESCAPE) {
            auto& binding = m_bindings.keys[static_cast<size_t>(m_rebindTarget)];
            binding.primary = sc;
            binding.alternate = 0;
            m_bindings.SaveToFile(NovaInput::kKeyBindingsPath);
        }
        m_rebindTarget = -1;
    }
    // Swallow key events while waiting, so the key doesn't also trigger a
    // hotkey (Esc opens the pause menu) or keyboard navigation.
    return true;
}

void NovaLauncher::RenderInputSettingsWidget() {
    constexpr int kCount = static_cast<int>(NovaInput::kN64InputCount);
    const ImVec4 kWarn(0.75f, 0.25f, 0.15f, 1.0f);

    if (ImGui::BeginTable("InputTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthFixed, 154.0f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 168.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < kCount; ++i) {
            const auto input = static_cast<NovaInput::N64Input>(i);
            const auto& b = m_bindings[input];

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(NovaInput::InputLabel(input));

            ImGui::TableSetColumnIndex(1);
            std::string keys = KeyName(b.primary);
            if (b.alternate) keys += " or " + KeyName(b.alternate);
            ImGui::TextUnformatted(keys.c_str());
            for (int j = 0; j < kCount && b.primary > 0; ++j) {
                const auto& other = m_bindings.keys[static_cast<size_t>(j)];
                if (j != i && (other.primary == b.primary || other.alternate == b.primary)) {
                    ImGui::SameLine();
                    ImGui::TextColored(kWarn, "(also %s)", NovaInput::InputLabel(static_cast<NovaInput::N64Input>(j)));
                    break;
                }
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(i);
            if (m_rebindTarget == i) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                ImGui::Button("Press a key...", ImVec2(-1, 0));
                ImGui::PopStyleColor();
            } else if (ImGui::Button("Change", ImVec2(-1, 0))) {
                m_rebindTarget = i;
                m_padRebindTarget = -1;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (m_rebindTarget >= 0) kit::Muted("Press the new key, or Esc to cancel.");
    if (ImGui::Button("Reset to defaults")) {
        m_bindings.ResetToDefaults();
        m_bindings.SaveToFile(NovaInput::kKeyBindingsPath);
        m_rebindTarget = -1;
    }
}

// --- Settings tab ---
namespace {

void Hint(const char* text) { kit::Muted("%s", text); }

// A file:// URL for a folder, with everything outside the unreserved set escaped
// (the user data folder has a space in it on macOS, and URLs may not).
std::string FolderUrl(const std::filesystem::path& folder) {
    const std::string path = folder.generic_string(); // forward slashes on every platform
    std::string url = "file://";
    if (path.empty() || path[0] != '/') url += '/';   // C:/Users/... -> file:///C:/Users/...
    static const char* kHex = "0123456789ABCDEF";
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '.' || c == '_' || c == '~' || c == ':') {
            url += static_cast<char>(c);
        } else {
            url += '%';
            url += kHex[c >> 4];
            url += kHex[c & 15];
        }
    }
    return url;
}

} // namespace

void NovaLauncher::SaveControllerSettings() {
    m_padSettings.SaveToFile(NovaInput::kControllerSettingsPath);
    NovaInput::Gamepads::Instance().ReloadSettings();
}

bool NovaLauncher::HandlePadRebindEvent(const SDL_Event& event) {
    if (m_padRebindTarget < 0) return false;
    using Kind = NovaInput::PadBinding::Kind;
    NovaInput::PadBinding picked;
    switch (event.type) {
    case SDL_CONTROLLERBUTTONDOWN: picked = {Kind::Button, event.cbutton.button}; break;
    case SDL_CONTROLLERAXISMOTION:
        // Only a deliberate push: sticks and triggers report small movements constantly.
        if (std::abs(static_cast<int>(event.caxis.value)) < 20000) return true;
        picked = {event.caxis.value > 0 ? Kind::AxisPositive : Kind::AxisNegative, event.caxis.axis};
        break;
    case SDL_KEYDOWN:
        if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) m_padRebindTarget = -1;
        return true;
    case SDL_KEYUP:
    case SDL_CONTROLLERBUTTONUP: return true;
    default:                     return false;
    }
    const auto i = static_cast<size_t>(m_padRebindTarget);
    m_padSettings.primary[i] = picked;
    m_padSettings.alternate[i] = {};
    SaveControllerSettings();
    m_padRebindTarget = -1;
    return true;
}

void NovaLauncher::RenderControllerSettingsWidget() {
    using namespace NovaInput;
    auto& gamepads = Gamepads::Instance();
    const std::vector<ConnectedPad> pads = gamepads.List();

    static const char* kAccessories[] = {"Automatic", "Nothing", "Rumble Pak"};
    static const char* kPlayerNames[] = {"Player 1", "Player 2", "Player 3", "Player 4"};
    if (ImGui::BeginTable("PadPlayers", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthFixed, 60 * 1.4f);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 154.0f);
        ImGui::TableSetupColumn("Accessory", ImGuiTableColumnFlags_WidthFixed, 150 * 1.4f);
        ImGui::TableHeadersRow();
        for (int p = 0; p < kMaxPlayers; ++p) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", p + 1);

            ImGui::TableSetColumnIndex(1);
            const auto pad =
                std::find_if(pads.begin(), pads.end(), [p](const ConnectedPad& c) { return c.player == p; });
            const bool hasPad = pad != pads.end();
            if (hasPad) {
                ImGui::TextUnformatted(pad->name.c_str());
                if (pad->canRumble) {
                    kit::Muted("(rumble)");
                }
            } else {
                kit::Muted("%s", p == 0 ? "Keyboard only" : "Not connected");
            }

            // A controller's player is decided by connection order, which can
            // put a controller nobody is holding — a stale Bluetooth pairing,
            // say — ahead of the one actually in use. Move it, and the choice
            // is remembered by that controller's own ID for next time.
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(p);
            if (hasPad) {
                ImGui::SetNextItemWidth(-1);
                int current = p;
                if (ImGui::Combo("##moveto", &current, kPlayerNames, kMaxPlayers) && current != p) {
                    // Whatever already sits in the target slot (if anything)
                    // gets bumped into this row's slot by the swap below, so
                    // that's what this slot should prefer from now on.
                    std::string bumpedGuid;
                    for (const auto& other : pads)
                        if (other.player == current) {
                            bumpedGuid = other.guid;
                            break;
                        }
                    gamepads.SwapPlayers(p, current);
                    m_padSettings.preferredGuid[current] = pad->guid;
                    m_padSettings.preferredGuid[p] = bumpedGuid;
                    SaveControllerSettings();
                }
            }
            ImGui::PopID();

            ImGui::TableSetColumnIndex(3);
            ImGui::PushID(1000 + p);
            int accessory = static_cast<int>(m_padSettings.accessory[p]);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##accessory", &accessory, kAccessories, 3)) {
                m_padSettings.accessory[p] = static_cast<Accessory>(accessory);
                SaveControllerSettings();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    kit::Muted("A new controller takes the first free player, in whatever order the operating\n"
               "system reports them -- if the wrong one gets a slot (for example a stale\n"
               "Bluetooth pairing macOS still calls connected), move it above; the choice is\n"
               "remembered for that controller. Player 1 also uses the keyboard.\n"
               "Automatic plugs in a Rumble Pak for a controller that can rumble. Games look\n"
               "for it when they start. With a game running, Back or Guide opens the pause menu.");

    ImGui::Spacing();
    ImGui::SetNextItemWidth(220 * 1.4f);
    ImGui::SliderInt("Stick deadzone", &m_padSettings.deadzone, 0, 50, "%d%%");
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveControllerSettings();
    ImGui::SetNextItemWidth(220 * 1.4f);
    ImGui::SliderInt("Stick range", &m_padSettings.range, 50, 150, "%d%%");
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveControllerSettings();
    Hint("100% reaches the edge of a real N64 stick");

    if (!pads.empty()) {
        PIF::ControllerState state{};
        const int player = pads.front().player;
        gamepads.Apply(player, state);
        static const std::pair<uint16_t, const char*> kNames[] = {
            {N64Button::A, "A"},           {N64Button::B, "B"},
            {N64Button::Z, "Z"},           {N64Button::Start, "Start"},
            {N64Button::L, "L"},           {N64Button::R, "R"},
            {N64Button::DPadUp, "D-Up"},   {N64Button::DPadDn, "D-Down"},
            {N64Button::DPadLt, "D-Left"}, {N64Button::DPadRt, "D-Right"},
            {N64Button::CUp, "C-Up"},      {N64Button::CDown, "C-Down"},
            {N64Button::CLeft, "C-Left"},  {N64Button::CRight, "C-Right"},
        };
        std::string held;
        for (const auto& [bit, name] : kNames) {
            if (!(state.buttons & bit)) continue;
            if (!held.empty()) held += ' ';
            held += name;
        }
        ImGui::Text("Player %d now: stick %d, %d  %s", player + 1, state.joystick_x, state.joystick_y, held.c_str());
    }

    ImGui::Spacing();
    constexpr int kCount = static_cast<int>(kN64InputCount);
    if (ImGui::BeginTable("PadTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthFixed, 154.0f);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 168.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < kCount; ++i) {
            const auto input = static_cast<N64Input>(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(InputLabel(input));

            ImGui::TableSetColumnIndex(1);
            std::string text = PadBindingName(m_padSettings.primary[i]);
            if (m_padSettings.alternate[i].kind != PadBinding::Kind::None)
                text += " or " + PadBindingName(m_padSettings.alternate[i]);
            ImGui::TextUnformatted(text.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(1000 + i);
            if (m_padRebindTarget == i) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                ImGui::Button("Waiting...", ImVec2(-1, 0));
                ImGui::PopStyleColor();
            } else if (ImGui::Button("Change", ImVec2(-1, 0))) {
                m_padRebindTarget = i;
                m_rebindTarget = -1;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (m_padRebindTarget >= 0) kit::Muted("Press a button, or push a stick or trigger, on a controller. Esc cancels.");
    if (ImGui::Button("Reset controller settings")) {
        m_padSettings = ControllerSettings{};
        m_padRebindTarget = -1;
        SaveControllerSettings();
    }
}

namespace {

bool UseSegmented(const OptionDef& option) {
    if (option.kind != OptionKind::Choice || option.choices.size() > 3) return false;
    for (const char* label : option.choices)
        if (std::strlen(label) > 12) return false;
    return true;
}

/// The default value of every option: a fresh set of launcher options and graphics settings.
struct Defaults {
    LaunchOptions launch;
    Nova64::Video::PluginConfig gfx;
    int saveChip = 0;
    Defaults() { gfx.LoadDefaults(); }
    SettingsModel Model() { return SettingsModel{&launch, &gfx, &saveChip}; }
};

} // namespace

namespace detail {

float OptionControlWidth(const OptionDef& option) {
    switch (option.kind) {
    case OptionKind::Toggle: return 44.0f;
    case OptionKind::Slider: return 260.0f;
    case OptionKind::Choice:
        return UseSegmented(option)
                   ? kit::SegmentedWidth(option.choices.data(), static_cast<int>(option.choices.size()))
                   : 260.0f;
    }
    return 260.0f;
}

float OptionControlHeight(const OptionDef& option) { return option.kind == OptionKind::Toggle ? 24.0f : 32.0f; }

} // namespace detail

int OptionDefault(const OptionDef& option) {
    static Defaults defaults;
    return option.get(defaults.Model());
}

namespace detail {

// Draws the control for an option on its own. `value` is read and written in place; `defaultValue` marks the
// matching entry of a dropdown.
bool DrawOptionControl(const OptionDef& option, int* value, int defaultValue) {
    switch (option.kind) {
    case OptionKind::Toggle: {
        bool on = *value != 0;
        if (!kit::Toggle("##value", &on)) return false;
        *value = on ? 1 : 0;
        return true;
    }
    case OptionKind::Slider:
        return kit::Slider("##value", value, option.min, option.max, option.format, OptionControlWidth(option));
    case OptionKind::Choice: {
        int index = ChoiceIndex(option, *value);
        if (UseSegmented(option)) {
            if (!kit::Segmented("##value", option.choices.data(), static_cast<int>(option.choices.size()), &index))
                return false;
            *value = ChoiceValue(option, index);
            return true;
        }
        bool changed = false;
        ImGui::SetNextItemWidth(OptionControlWidth(option));
        if (ImGui::BeginCombo("##value", option.choices[static_cast<size_t>(index)])) {
            for (size_t i = 0; i < option.choices.size(); ++i) {
                const int candidate = ChoiceValue(option, static_cast<int>(i));
                const char* why = option.choiceUnavailable ? option.choiceUnavailable(candidate) : nullptr;
                const std::string label =
                    std::string(option.choices[i]) + (candidate == defaultValue ? "   (default)" : "");
                if (ImGui::Selectable(label.c_str(), static_cast<int>(i) == index,
                                      why ? ImGuiSelectableFlags_Disabled : 0)) {
                    *value = candidate;
                    changed = true;
                }
                if (why && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why);
            }
            ImGui::EndCombo();
        }
        return changed;
    }
    }
    return false;
}

} // namespace detail

bool NovaLauncher::RenderOptionRow(const OptionDef& option, const SettingsModel& model) {
    SettingsModel target = model;
    const char* blocked = option.unavailable ? option.unavailable(target) : nullptr;
    const char* warning = option.warning ? option.warning(target) : nullptr;
    const char* text = blocked ? blocked : warning ? warning : option.help;

    int value = option.get(target);
    const int defaultValue = OptionDefault(option);
    bool changed = false;
    ImGui::BeginDisabled(blocked != nullptr);
    kit::SettingRow(
        option.title, text, detail::kResetWidth + detail::OptionControlWidth(option),
        detail::OptionControlHeight(option),
        [&] {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (value != defaultValue) {
                ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + (detail::OptionControlHeight(option) - 28.0f) * 0.5f));
                if (kit::Button("Reset", kit::ButtonStyle::Ghost, kit::Icon::None,
                                ImVec2(detail::kResetWidth - 8.0f, 28.0f))) {
                    value = defaultValue;
                    changed = true;
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(at.x + detail::kResetWidth, at.y));
            if (detail::DrawOptionControl(option, &value, defaultValue)) changed = true;
            return changed;
        },
        option.experimental ? "Experimental" : nullptr);
    ImGui::EndDisabled();
    if (!changed) return false;
    option.set(target, value);
    return true;
}

void NovaLauncher::RenderSettingsTab() {
    using namespace kit;
    static const char* const kPages[] = {"Emulation", "Video", "Audio", "Controls"};
    static const char* const kPageIds[] = {"emulation", "video", "audio", "controls"};

    if (m_frameCount < 3 && !m_forcedSubTab.empty())
        for (int i = 0; i < 4; ++i)
            if (m_forcedSubTab == kPageIds[i]) m_settingsPage = i;

    BeginColumn(880.0f);
    PageTitleText("Settings", "Changes save at once and apply the next time a game starts. Volume, mute, rewind, "
                              "frame rate and pause apply immediately.");
    ImGui::Dummy(ImVec2(0.0f, space::k4));
    TabStrip("##settingsTabs", kPages, 4, &m_settingsPage);
    ImGui::Dummy(ImVec2(0.0f, space::k6));

    SettingsModel model{&m_launch, &m_gfx, &m_unusedSaveChip};
    auto persist = [&](const OptionDef& option) {
        if (option.store == Store::Launcher) SaveLaunchOptions();
        else if (option.store == Store::Graphics) SaveGraphicsSettings();
    };

    if (m_settingsPage == 3) {
        BeginSettings("Rumble", "Rumble");
        for (const OptionDef& option : AllOptions())
            if (option.globalSetting && std::strcmp(option.page, "controls") == 0 && RenderOptionRow(option, model))
                persist(option);
        EndSettings();
        BeginCard("##controllers", ImVec2(0, 0), true, ImVec2(space::k6, space::k4));
        SectionTitle("Game controllers");
        ImGui::Dummy(ImVec2(0.0f, space::k2));
        RenderControllerSettingsWidget();
        EndCard();
        ImGui::Dummy(ImVec2(0.0f, space::k4));
        BeginCard("##keyboard", ImVec2(0, 0), true, ImVec2(space::k6, space::k4));
        SectionTitle("Keyboard (player 1)");
        ImGui::Dummy(ImVec2(0.0f, space::k2));
        RenderInputSettingsWidget();
        EndCard();
        EndColumn();
        return;
    }

    // The options of the page, grouped in the order the table lists them
    const char* group = nullptr;
    bool open = false;
    for (const OptionDef& option : AllOptions()) {
        if (!option.globalSetting || std::strcmp(option.page, kPageIds[m_settingsPage]) != 0) continue;
        if (!group || std::strcmp(group, option.group) != 0) {
            if (open) EndSettings();
            group = option.group;
            BeginSettings(option.group, option.group);
            open = true;
        }
        if (RenderOptionRow(option, model)) persist(option);
    }
    if (open) EndSettings();

    if (m_settingsPage == 0) {
        // Everything NovaStar64 keeps lives in one folder.
        BeginSettings("Data", "Data");
        static const std::filesystem::path dataFolder = std::filesystem::current_path();
        static bool openFailed = false;
        const std::string shown = openFailed ? "Couldn't open it; the path is on the clipboard." : dataFolder.string();
        if (SettingRow("Data folder", shown.c_str(), 150.0f, 32.0f, [&] {
                return Button("Open folder", ButtonStyle::Secondary, Icon::Folder, ImVec2(150.0f, 32.0f));
            })) {
            openFailed = SDL_OpenURL(FolderUrl(dataFolder).c_str()) != 0;
            if (openFailed) SDL_SetClipboardText(dataFolder.string().c_str());
        }
        EndSettings();
    }
    if (m_settingsPage == 1) {
        std::string running;
        {
            std::lock_guard<std::mutex> lock(Nova64::Video::g_frameMutex);
            running = Nova64::Video::g_rendererDescription;
        }
        if (!running.empty()) Muted("Last game ran on %s.", running.c_str());
    }

    // One button per page puts its options back to their defaults; the controls are not touched.
    ImGui::Dummy(ImVec2(0.0f, space::k2));
    if (Button("Restore defaults", ButtonStyle::Ghost, Icon::None, ImVec2(0.0f, 36.0f)))
        ImGui::OpenPopup("Restore defaults?");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, radius::kCard);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(space::k6, space::k6));
    CenterNextModal();
    const bool confirming = ImGui::BeginPopupModal("Restore defaults?", nullptr, kModalFlags);
    ImGui::PopStyleVar(2);
    if (confirming) {
        ImGui::Text("Every %s setting goes back to its default.", kPages[m_settingsPage]);
        ImGui::Dummy(ImVec2(0.0f, space::k2));
        if (Button("Restore", ButtonStyle::Primary, Icon::None, ImVec2(120.0f, 40.0f))) {
            for (const OptionDef& option : AllOptions()) {
                if (!option.globalSetting || std::strcmp(option.page, kPageIds[m_settingsPage]) != 0) continue;
                SettingsModel target = model;
                option.set(target, OptionDefault(option));
                persist(option);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (Button("Cancel", ButtonStyle::Secondary, Icon::None, ImVec2(120.0f, 40.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    EndColumn();
}

} // namespace NovaUI
