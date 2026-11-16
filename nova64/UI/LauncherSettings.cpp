/**
 * @file LauncherSettings.cpp
 * @brief Settings tab: launch options, graphics options and keyboard controls.
 */
#include "NovaLauncher.hpp"
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

// ============================================================
// LaunchOptions
// ============================================================
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
        else if (key == "volume") volume = v;
        else if (key == "muted") muted = v != 0;
    }
    if (speed != 0 && speed != 30 && speed != 60 && speed != 120) speed = 60;
    // Older files also used 2, exclusive fullscreen; any fullscreen is 1 now.
    displayMode = (displayMode == 0) ? 0 : 1;
    if (cpuCore < 0 || cpuCore > 2) cpuCore = 0;
    if (rspEngine < 0 || rspEngine > 1) rspEngine = 0;
    if (volume < 0 || volume > 100) volume = 100;
}

void LaunchOptions::Save(const std::string& path) const {
    std::ofstream f(path, std::ios::trunc);
    f << "speed = " << speed << "\n"
      << "displayMode = " << displayMode << "\n"
      << "cpuCore = " << cpuCore << "\n"
      << "rspEngine = " << rspEngine << "\n"
      << "rewind = " << (rewind ? 1 : 0) << "\n"
      << "pauseWhenInactive = " << (pauseOnFocusLoss ? 1 : 0) << "\n"
      << "showFps = " << (showFps ? 1 : 0) << "\n"
      << "volume = " << volume << "\n"
      << "muted = " << (muted ? 1 : 0) << "\n";
}

// ============================================================
// Load / save
// ============================================================
void NovaLauncher::LoadSettings() {
    m_gfx.LoadFromFile(detail::kGraphicsPath);
    m_launch.Load(detail::kLauncherPath);
    m_bindings.LoadFromFile(NovaInput::kKeyBindingsPath);
    m_padSettings.LoadFromFile(NovaInput::kControllerSettingsPath);
    ApplyLaunchOptions();
}

void NovaLauncher::SaveLaunchOptions() {
    std::error_code ec;
    std::filesystem::create_directories("settings", ec);
    m_launch.Save(detail::kLauncherPath);
    ApplyLaunchOptions();
}

void NovaLauncher::ApplyLaunchOptions() {
    Nova64::RSP::HLE::CAudioOutput::SetMasterVolume(static_cast<uint32_t>(m_launch.volume));
    Nova64::RSP::HLE::CAudioOutput::SetMuted(m_launch.muted);
    Nova64::StateService::Instance().SetRewindEnabled(m_launch.rewind);
}

void NovaLauncher::SaveGraphicsSettings() {
    // A running game rewrites this file when it exits, so start from what is on
    // disk and change only the fields this tab owns.
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
    onDisk.SaveToFile(detail::kGraphicsPath);
}

void NovaLauncher::SetTurbo(bool enabled) {
    m_turboMode = enabled;
    const int speed = (enabled && m_launch.speed != 0) ? m_launch.speed * 2 : m_launch.speed;
    CN64System::s_TargetFPS = speed;
    if (g_System) g_System->SetSpeed(speed);
}

// ============================================================
// Controls
// ============================================================
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
        ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 120);
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

    if (m_rebindTarget >= 0) ImGui::TextDisabled("Press the new key, or Esc to cancel.");
    if (ImGui::Button("Reset to defaults")) {
        m_bindings.ResetToDefaults();
        m_bindings.SaveToFile(NovaInput::kKeyBindingsPath);
        m_rebindTarget = -1;
    }
}

// ============================================================
// Settings tab
// ============================================================
namespace {

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", text);
}

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
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Accessory", ImGuiTableColumnFlags_WidthFixed, 150);
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
                    ImGui::SameLine();
                    ImGui::TextDisabled("(rumble)");
                }
            } else {
                ImGui::TextDisabled("%s", p == 0 ? "Keyboard only" : "Not connected");
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
    ImGui::TextDisabled("A new controller takes the first free player, in whatever order the operating\n"
                        "system reports them -- if the wrong one gets a slot (for example a stale\n"
                        "Bluetooth pairing macOS still calls connected), move it above; the choice is\n"
                        "remembered for that controller. Player 1 also uses the keyboard.\n"
                        "Automatic plugs in a Rumble Pak for a controller that can rumble. Games look\n"
                        "for it when they start. With a game running, Back or Guide opens the pause menu.");

    ImGui::Spacing();
    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt("Stick deadzone", &m_padSettings.deadzone, 0, 50, "%d%%");
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveControllerSettings();
    ImGui::SetNextItemWidth(220);
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
        ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 120);
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
    if (m_padRebindTarget >= 0)
        ImGui::TextDisabled("Press a button, or push a stick or trigger, on a controller. Esc cancels.");
    if (ImGui::Button("Reset controller settings")) {
        m_padSettings = ControllerSettings{};
        m_padRebindTarget = -1;
        SaveControllerSettings();
    }
}

void NovaLauncher::RenderSettingsTab() {
    ImGui::Text("Settings");
    ImGui::TextDisabled("Changes save immediately. Video and emulation settings apply to the next game you start.");
    ImGui::Separator();
    ImGui::Spacing();

    const bool forcing = m_frameCount < 3;
    auto forced = [&](const char* name) {
        return (forcing && m_forcedSubTab == name) ? ImGuiTabItemFlags_SetSelected : 0;
    };

    if (!ImGui::BeginTabBar("SettingsTabs")) return;

    if (ImGui::BeginTabItem("Emulation", nullptr, forced("emulation"))) {
        ImGui::Spacing();
        static constexpr int kSpeeds[] = {30, 60, 120, 0};
        static const char* kSpeedLabels[] = {"Half speed", "Full speed", "Double speed", "Unlimited"};
        int speedIndex = 1;
        for (int i = 0; i < 4; ++i)
            if (kSpeeds[i] == m_launch.speed) speedIndex = i;
        ImGui::SetNextItemWidth(220);
        if (ImGui::Combo("Speed", &speedIndex, kSpeedLabels, 4)) {
            m_launch.speed = kSpeeds[speedIndex];
            SaveLaunchOptions();
        }

        static const char* kCores[] = {"Automatic", "Interpreter", "Dynamic recompiler"};
        ImGui::SetNextItemWidth(220);
        if (ImGui::Combo("CPU core", &m_launch.cpuCore, kCores, 3)) SaveLaunchOptions();
        ImGui::TextDisabled("Automatic uses the recompiler on x86-64 and the interpreter on Apple Silicon,\n"
                            "where the recompiler is still incomplete.");

        static const char* kRsp[] = {"Recompiler (faster)", "Interpreter"};
        ImGui::SetNextItemWidth(220);
        if (ImGui::Combo("RSP engine", &m_launch.rspEngine, kRsp, 2)) SaveLaunchOptions();
        ImGui::TextDisabled("Runs the graphics and audio microcode when it isn't emulated at a higher level\n"
                            "(the LLE rendering mode). Both give identical results; applies to the next game.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Checkbox("Rewind", &m_launch.rewind)) SaveLaunchOptions();
        Hint("keeps a picture every 2 seconds for Backspace; about 1 MB each");
        if (ImGui::Checkbox("Pause when another application is active", &m_launch.pauseOnFocusLoss))
            SaveLaunchOptions();

        // Everything NovaStar64 keeps -- settings, games, saves, covers, texture packs
        // -- is in one folder, which for an installed copy is a hidden one.
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        static const std::filesystem::path dataFolder = std::filesystem::current_path();
        static char dataFolderText[1024] = {};
        static bool openFailed = false;
        if (dataFolderText[0] == '\0')
            std::snprintf(dataFolderText, sizeof(dataFolderText), "%s", dataFolder.string().c_str());
        ImGui::TextUnformatted("Data folder");
        // A read-only box rather than plain text: a long path scrolls instead of being
        // cut off, and can be selected and copied.
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##dataFolder", dataFolderText, sizeof(dataFolderText), ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button("Open data folder")) {
            openFailed = SDL_OpenURL(FolderUrl(dataFolder).c_str()) != 0;
            if (openFailed) SDL_SetClipboardText(dataFolder.string().c_str());
        }
        Hint(openFailed ? "couldn't open it; the path is on the clipboard"
                        : "settings, games, saves, covers and texture packs");
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Video", nullptr, forced("video"))) {
        ImGui::Spacing();
        static const char* kModes[] = {"In the launcher window", "Fullscreen"};
        ImGui::SetNextItemWidth(220);
        if (ImGui::Combo("Play games", &m_launch.displayMode, kModes, 2)) SaveLaunchOptions();

        // Graphics driver. OpenGL can't run on macOS: Apple stopped at OpenGL
        // 4.1, and this renderer needs 4.6.
        static const bool kVulkanAvailable = Nova64::Video::VK::IsVulkanAvailable();
#ifdef __APPLE__
        constexpr bool kOpenGLAvailable = false;
        constexpr const char* kOpenGLUnavailable = "macOS only supports OpenGL up to 4.1; this renderer needs 4.6.";
#else
        constexpr bool kOpenGLAvailable = true;
        constexpr const char* kOpenGLUnavailable = "";
#endif
        struct Driver {
            const char* name;
            int backend;
            bool available;
            const char* why;
        };
        const Driver kDrivers[] = {
            {"Vulkan", 1, kVulkanAvailable, "No Vulkan driver was found."},
            {"OpenGL", 0, kOpenGLAvailable, kOpenGLUnavailable},
        };
        const char* current = (m_gfx.backendType == 1) ? "Vulkan" : "OpenGL";
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("Graphics driver", current)) {
            for (const auto& d : kDrivers) {
                const ImGuiSelectableFlags flags = d.available ? 0 : ImGuiSelectableFlags_Disabled;
                if (ImGui::Selectable(d.name, m_gfx.backendType == d.backend, flags)) {
                    m_gfx.backendType = d.backend;
                    SaveGraphicsSettings();
                }
                if (!d.available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", d.why);
            }
            ImGui::EndCombo();
        }
        const bool selectedUnavailable = (m_gfx.backendType == 1) ? !kVulkanAvailable : !kOpenGLAvailable;
        if (selectedUnavailable) {
            ImGui::TextColored(ImVec4(0.75f, 0.25f, 0.15f, 1.0f),
                               "%s isn't available here; games won't start until you pick the other driver.", current);
        }
        {
            std::string running;
            {
                std::lock_guard<std::mutex> lock(Nova64::Video::g_frameMutex);
                running = Nova64::Video::g_rendererDescription;
            }
            if (!running.empty()) ImGui::TextDisabled("Last game ran on %s.", running.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt("Resolution scale", &m_gfx.internalResScale, 1, 8, "%dx")) SaveGraphicsSettings();

        bool widescreen = (m_gfx.aspectRatio == Nova64::Video::AspectRatio::Widescreen16_9);
        if (ImGui::Checkbox("Widescreen (16:9)", &widescreen)) {
            m_gfx.aspectRatio =
                widescreen ? Nova64::Video::AspectRatio::Widescreen16_9 : Nova64::Video::AspectRatio::Native4_3;
            SaveGraphicsSettings();
        }
        Hint("widens the camera; some games show gaps at the edges");

        if (ImGui::Checkbox("Mipmapping", &m_gfx.enableMipmaps)) SaveGraphicsSettings();
        Hint("smoother textures in the distance");

        if (ImGui::Checkbox("Texture packs", &m_gfx.hiResTexEnabled)) SaveGraphicsSettings();
        Hint("hires_texture/<game name>/, Rice format; takes effect when a game starts");

        if (ImGui::Checkbox("Save game textures for making a pack", &m_gfx.textureDump)) SaveGraphicsSettings();
        Hint("writes texture_dump/<game name>/; takes effect when a game starts");

        ImGui::BeginDisabled(m_gfx.backendType != 1);
        if (ImGui::Checkbox("Ray-traced lighting (experimental)", &m_gfx.rayTracing)) SaveGraphicsSettings();
        ImGui::EndDisabled();
        Hint("Vulkan with high-level graphics emulation; takes effect when a game starts");

        if (ImGui::Checkbox("Show frame rate while playing", &m_launch.showFps)) SaveLaunchOptions();

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Accuracy")) {
            static const char* kEmulation[] = {"High-level (recommended)", "Low-level (experimental)"};
            int emulation = (m_gfx.renderingMode == Nova64::Video::RenderingMode::HLE) ? 0 : 1;
            ImGui::SetNextItemWidth(220);
            if (ImGui::Combo("Graphics emulation", &emulation, kEmulation, 2)) {
                m_gfx.renderingMode =
                    (emulation == 0) ? Nova64::Video::RenderingMode::HLE : Nova64::Video::RenderingMode::LLE;
                SaveGraphicsSettings();
            }
            Hint("low-level runs the real RSP microcode; slower, and still shows seams");

            if (ImGui::Checkbox("Copy frames back to game memory", &m_gfx.fbReadAccess)) SaveGraphicsSettings();
            Hint("for games that read the screen, such as pause-menu backgrounds");
            if (ImGui::Checkbox("Show frames the game draws itself", &m_gfx.fbWriteAccess)) SaveGraphicsSettings();
            Hint("for games that draw the picture on the CPU");
            ImGui::TextDisabled("Some games turn these two on regardless, because they need them.");
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Audio", nullptr, forced("audio"))) {
        ImGui::Spacing();
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt("Volume", &m_launch.volume, 0, 100, "%d%%")) SaveLaunchOptions();
        if (ImGui::Checkbox("Mute", &m_launch.muted)) SaveLaunchOptions();
        ImGui::TextDisabled("Takes effect immediately, including in a game that's running.");
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Controls", nullptr, forced("controls"))) {
        ImGui::Spacing();
        ImGui::TextDisabled("Games that are already running pick up changes within a second.");
        ImGui::Spacing();
        ImGui::SeparatorText("Game controllers");
        RenderControllerSettingsWidget();
        ImGui::Spacing();
        ImGui::SeparatorText("Keyboard (player 1)");
        RenderInputSettingsWidget();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
}

} // namespace NovaUI
