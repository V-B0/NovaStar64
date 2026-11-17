/**
 * @file LauncherSimulator.cpp
 * @brief Simulator page: how one game will run, and what is different about it.
 *
 * A game starts with the global settings. Here a single game can override any option that makes sense per
 * game; the overrides are saved in settings/games/<rom name>.ini and applied when the game starts (see
 * PlayRom). The page shows the virtual hardware as a schematic (cartridge, console, display, controllers), lets
 * the inspector below change what belongs to the selected part, and lists what is wrong with the setup.
 */
#include "GameInfo.hpp"
#include "LauncherInternal.hpp"
#include "NovaLauncher.hpp"
#include "UiKit.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace NovaUI {

namespace {

enum Part { kCartridge = 0, kConsole, kDisplay, kControllers, kPartCount };

struct PartDef {
    const char* name;
    kit::Icon icon;
    const char* summary;
    std::vector<const char*> options;
};

const PartDef& GetPart(int part) {
    static const PartDef kParts[kPartCount] = {
        {"Cartridge", kit::Icon::Tag, "The game itself: what its header says, and how it saves.", {"saveChip"}},
        {"Console",
         kit::Icon::Memory,
         "The CPU, the RSP and how fast the game runs.",
         {"speed", "cpuCore", "rspEngine", "rewind"}},
        {"Display",
         kit::Icon::Display,
         "The picture: driver, resolution and accuracy.",
         {"displayMode", "backendType", "internalResScale", "widescreen", "enableMipmaps", "hiResTexEnabled",
          "rayTracing", "renderingMode", "fbReadAccess", "fbWriteAccess"}},
        {"Controllers", kit::Icon::Games, "What is plugged into the four controller ports.", {}},
    };
    return kParts[part];
}

enum class Severity { Info, Warning, Error };

struct Problem {
    Severity severity;
    std::string text;
};

const char* BackendLabel(int backend) { return backend == 1 ? "Vulkan" : backend == 0 ? "OpenGL" : "Direct3D 11"; }

// How many of a part's options this game overrides.
int CustomCount(const GameSetup& setup, const PartDef& part) {
    int count = 0;
    for (const char* id : part.options)
        if (setup.Has(id)) ++count;
    return count;
}

std::string ChoiceLabel(const char* id, const SettingsModel& model) {
    const OptionDef* option = FindOption(id);
    return option ? ValueText(*option, option->get(model)) : std::string();
}

} // namespace

void NovaLauncher::OpenSimulator(int gameIndex) {
    if (gameIndex >= 0 && gameIndex < static_cast<int>(m_library.size())) m_simGame = gameIndex;
    m_simPart = 0;
    m_activeTab = Tab::Simulator;
}

void NovaLauncher::RenderSimulatorTab() {
    using namespace kit;
    const Fonts& fonts = GetFonts();

    if (m_frameCount < 3) {
        static const char* const kPartNames[kPartCount] = {"cartridge", "console", "display", "controllers"};
        for (int i = 0; i < kPartCount; ++i)
            if (m_forcedSubTab == kPartNames[i]) m_simPart = i;
    }

    kit::BeginColumn(1000.0f);
    if (m_library.empty()) {
        PageTitle("Simulator", "See how a game will run, and change what is different about it.");
        BeginCard("##simEmpty", ImVec2(0, 0), true, ImVec2(space::k8, space::k6));
        ImGui::TextUnformatted("Add a game to the library first.");
        ImGui::Dummy(ImVec2(0.0f, space::k2));
        if (Button("Add game", ButtonStyle::Primary, Icon::Plus, ImVec2(150.0f, 0.0f))) OpenAddDialog();
        EndCard();
        kit::EndColumn();
        return;
    }
    if (m_simGame < 0 || m_simGame >= static_cast<int>(m_library.size()))
        m_simGame = (m_selectedGame >= 0 && m_selectedGame < static_cast<int>(m_library.size())) ? m_selectedGame : 0;
    RomMetadata& rom = m_library[static_cast<size_t>(m_simGame)];
    if (m_simLoaded != m_simGame) {
        m_simSetup.Load(rom.path);
        m_simRom = detail::ReadRomInfo(rom.path);
        m_simLoaded = m_simGame;
    }
    const GameInfo* info = FindGameInfo(rom.name);

    // The global values, and what this game will actually use
    int noChip = 0;
    SettingsModel global{&m_launch, &m_gfx, &noChip};
    LaunchOptions effLaunch = m_launch;
    Nova64::Video::PluginConfig effGfx = m_gfx;
    int effChip = 0;
    SettingsModel effective{&effLaunch, &effGfx, &effChip};
    m_simSetup.Apply(effective);

    std::error_code ec;
    const bool romExists = fs::exists(rom.path, ec);
    const bool isDisk = IsN64DDDisk(rom.path);

    // --- What is wrong, or worth knowing ---
    std::vector<Problem> problems;
    if (!romExists)
        problems.push_back(
            {Severity::Error, "The ROM file is missing: " + fs::path(rom.path).filename().string() + "."});
    else if (!m_simRom.valid && !isDisk)
        problems.push_back({Severity::Error, "This file does not look like an N64 ROM."});
    if (const OptionDef* backend = FindOption("backendType"))
        if (const char* why = backend->choiceUnavailable ? backend->choiceUnavailable(effGfx.backendType) : nullptr)
            problems.push_back(
                {Severity::Error, std::string(BackendLabel(effGfx.backendType)) + " can't be used here: " + why});
    if (effGfx.renderingMode == Nova64::Video::RenderingMode::LLE && effLaunch.rspEngine == 1)
        problems.push_back(
            {Severity::Warning, "Low-level graphics on the RSP interpreter will be very slow. Use the recompiler."});
    if (effGfx.rayTracing && effGfx.renderingMode != Nova64::Video::RenderingMode::HLE)
        problems.push_back(
            {Severity::Warning, "Ray-traced lighting only works with high-level graphics, so it will not be used."});
    if (effGfx.rayTracing && effGfx.backendType != 1)
        problems.push_back({Severity::Warning, "Ray-traced lighting needs the Vulkan driver, so it will not be used."});
    if (m_simRom.valid && m_simRom.pal)
        problems.push_back(
            {Severity::Info, "This is a PAL game. It runs at 50 Hz, so full speed is 50 frames a second."});
    if (info && info->needsExpansionPak)
        problems.push_back(
            {Severity::Info, std::string(info->title) +
                                 " needs the Expansion Pak. NovaStar64 always provides 8 MB, so nothing to do."});
    if (effLaunch.speed == 0)
        problems.push_back({Severity::Info,
                            "Unlimited speed removes the frame limit: the game runs as fast as this computer allows."});
    if (effChip != 0)
        problems.push_back({Severity::Info, std::string("The save chip is forced to ") +
                                                (effChip == 1 ? "SRAM" : "FlashRAM") + " for this game."});

    // --- Title and the Play button ---
    const float top = ImGui::GetCursorPosY();
    ImGui::BeginGroup();
    PageTitleText("Simulator",
                  "See how a game will run, and change what is different about it. Everything else follows Settings.");
    ImGui::EndGroup();
    ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionMax().x - 150.0f, top + space::k1));
    ImGui::BeginDisabled(!romExists);
    if (Button("Play", ButtonStyle::Primary, Icon::Play, ImVec2(150.0f, 44.0f))) PlayRom(rom.path, rom.name);
    ImGui::EndDisabled();
    ImGui::SetCursorPosY(top + 76.0f);

    // --- The games on the left ---
    constexpr float kListWidth = 232.0f;
    const float gap = space::k4;
    const float rightWidth = ImGui::GetContentRegionAvail().x - kListWidth - gap;
    const float columnTop = ImGui::GetCursorPosY();

    BeginCard("##simGames", ImVec2(kListWidth, 0), true, ImVec2(space::k2, space::k2));
    {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, space::k1));
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        for (int i = 0; i < static_cast<int>(m_library.size()); ++i) {
            const RomMetadata& other = m_library[static_cast<size_t>(i)];
            ImGui::PushID(i);
            constexpr float kRow = 56.0f;
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 max(min.x + rowWidth, min.y + kRow);
            const bool clicked = NavButton("##row", ImVec2(rowWidth, kRow));
            const bool hovered = ImGui::IsItemHovered();
            const bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
            if (clicked) {
                m_simGame = i;
                m_simPart = 0;
            }
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const bool selected = (i == m_simGame);
            if (selected) draw->AddRectFilled(min, max, ImGui::GetColorU32(color::kAccentSoft), radius::kControl);
            else if (hovered) draw->AddRectFilled(min, max, ImGui::GetColorU32(color::kHover), radius::kControl);
            if (selected)
                draw->AddRectFilled(ImVec2(min.x, min.y + 12.0f), ImVec2(min.x + 3.0f, max.y - 12.0f),
                                    ImGui::GetColorU32(color::kAccentHover), 1.5f);
            if (focused) draw->AddRect(min, max, ImGui::GetColorU32(color::kAccentHover), radius::kControl, 0, 2.0f);

            const ImVec2 thumbMin(min.x + space::k3, min.y + 10.0f), thumbMax(thumbMin.x + 44.0f, thumbMin.y + 36.0f);
            if (const CoverTexture* cover = GetCoverTexture(other.cover))
                DrawImageCover(draw, cover->texture, cover->width, cover->height, thumbMin, thumbMax, 6.0f);
            else DrawGradientFill(draw, thumbMin, thumbMax, PlaceholderColor(other.name), 6.0f, 0.3f);
            GameSetup probe;
            const bool custom =
                fs::exists(GameSetup::PathFor(other.path), ec) && probe.Load(other.path) && !probe.Empty();
            const float textX = thumbMax.x + space::k3;
            const float textW = max.x - textX - space::k2;
            PutText(draw, fonts.label, ImVec2(textX, min.y + 10.0f), ImGui::GetColorU32(color::kText),
                    Ellipsize(fonts.label, other.name, textW).c_str());
            PutText(draw, fonts.small, ImVec2(textX, min.y + 31.0f),
                    ImGui::GetColorU32(custom ? color::kAccentText : color::kTextDim),
                    custom ? "Custom setup" : "Default setup");
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
    }
    EndCard();

    // --- The setup on the right ---
    ImGui::SetCursorPos(ImVec2(kListWidth + gap, columnTop));
    ImGui::BeginGroup();

    // The schematic: cartridge -> console -> display, with the controllers plugged into the console
    char line[128];
    auto drawPart = [&](int part, ImVec2 min, ImVec2 size, const char* l1, const std::string& l2,
                        const std::string& l3) {
        const PartDef& def = GetPart(part);
        const ImVec2 max(min.x + size.x, min.y + size.y);
        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(part);
        if (NavButton("##part", size)) m_simPart = part;
        const bool hovered = ImGui::IsItemHovered();
        const bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
        ImGui::PopID();
        const bool selected = (m_simPart == part);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(min, max, ImGui::GetColorU32(hovered ? color::kHover : color::kRaised), radius::kCard);
        draw->AddRect(min, max, ImGui::GetColorU32(selected || focused ? color::kAccentHover : color::kHairline),
                      radius::kCard, 0, selected || focused ? 2.0f : 1.0f);
        DrawIcon(draw, def.icon, ImVec2(min.x + space::k4 + 10.0f, min.y + space::k4 + 10.0f), 20.0f,
                 ImGui::GetColorU32(selected ? color::kAccentText : color::kTextMuted));
        PutText(draw, fonts.label, ImVec2(min.x + space::k4 + 30.0f, min.y + space::k4 + 1.0f),
                ImGui::GetColorU32(color::kText), def.name);
        const int custom = CustomCount(m_simSetup, def);
        if (custom > 0) {
            char badge[16];
            std::snprintf(badge, sizeof(badge), "%d CUSTOM", custom);
            const ImVec2 text = Measure(fonts.caption, badge);
            const ImVec2 a(max.x - space::k4 - text.x - space::k3, min.y + space::k4 - 1.0f);
            draw->AddRectFilled(a, ImVec2(a.x + text.x + space::k3, a.y + text.y + 6.0f),
                                ImGui::GetColorU32(color::kAccentSoft), 5.0f);
            PutText(draw, fonts.caption, ImVec2(a.x + space::k3 * 0.5f, a.y + 3.0f),
                    ImGui::GetColorU32(color::kAccentText), badge);
        }
        const float textW = size.x - space::k4 * 2.0f;
        float y = min.y + space::k4 + 36.0f;
        PutText(draw, fonts.body, ImVec2(min.x + space::k4, y), ImGui::GetColorU32(color::kText),
                Ellipsize(fonts.body, l1, textW).c_str());
        y += 22.0f;
        PutText(draw, fonts.small, ImVec2(min.x + space::k4, y), ImGui::GetColorU32(color::kTextMuted),
                Ellipsize(fonts.small, l2, textW).c_str());
        y += 19.0f;
        PutText(draw, fonts.small, ImVec2(min.x + space::k4, y), ImGui::GetColorU32(color::kTextMuted),
                Ellipsize(fonts.small, l3, textW).c_str());
    };
    auto arrow = [&](ImVec2 from, ImVec2 to) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 color = ImGui::GetColorU32(color::kTextDim);
        draw->AddLine(from, ImVec2(to.x - 6.0f, to.y), color, 2.0f);
        draw->AddTriangleFilled(ImVec2(to.x, to.y), ImVec2(to.x - 8.0f, to.y - 5.0f), ImVec2(to.x - 8.0f, to.y + 5.0f),
                                color);
    };

    BeginCard("##schematic", ImVec2(rightWidth, 0), true, ImVec2(space::k4, space::k4));
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float inner = ImGui::GetContentRegionAvail().x;
        const float arrowGap = space::k8;
        const float boxW = std::floor((inner - arrowGap * 2.0f) / 3.0f);
        const float boxH = 124.0f;

        std::string cartL2 =
            m_simRom.valid ? std::string(m_simRom.region) + "  \xC2\xB7  " + (m_simRom.pal ? "PAL 50 Hz" : "NTSC 60 Hz")
                           : (isDisk ? "64DD disk" : "Not an N64 ROM");
        const char* chipName = effChip == 1 ? "SRAM" : effChip == 2 ? "FlashRAM" : "detected from the game";
        std::snprintf(line, sizeof(line), "Saves: %s", chipName);
        drawPart(kCartridge, origin, ImVec2(boxW, boxH), rom.name.c_str(), cartL2, line);

        std::snprintf(line, sizeof(line), "CPU: %s", ChoiceLabel("cpuCore", effective).c_str());
        const std::string consoleL2 = "RSP: " + ChoiceLabel("rspEngine", effective);
        const std::string consoleL3 = ChoiceLabel("speed", effective) + "  \xC2\xB7  8 MB RAM";
        drawPart(kConsole, ImVec2(origin.x + boxW + arrowGap, origin.y), ImVec2(boxW, boxH), line, consoleL2,
                 consoleL3);

        const std::string displayL1 =
            std::string(BackendLabel(effGfx.backendType)) + "  \xC2\xB7  " + ChoiceLabel("internalResScale", effective);
        const std::string displayL2 =
            (effGfx.aspectRatio == Nova64::Video::AspectRatio::Widescreen16_9 ? "16:9 widescreen" : "4:3") +
            std::string(effGfx.renderingMode == Nova64::Video::RenderingMode::LLE ? "  \xC2\xB7  low-level" : "");
        drawPart(kDisplay, ImVec2(origin.x + (boxW + arrowGap) * 2.0f, origin.y), ImVec2(boxW, boxH), displayL1.c_str(),
                 displayL2, ChoiceLabel("displayMode", effective));

        const float midY = origin.y + boxH * 0.5f;
        arrow(ImVec2(origin.x + boxW + 4.0f, midY), ImVec2(origin.x + boxW + arrowGap - 4.0f, midY));
        arrow(ImVec2(origin.x + boxW * 2.0f + arrowGap + 4.0f, midY),
              ImVec2(origin.x + (boxW + arrowGap) * 2.0f - 4.0f, midY));

        // The controllers hang under the console
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float barTop = origin.y + boxH + space::k6;
        const float consoleCenter = origin.x + boxW + arrowGap + boxW * 0.5f;
        draw->AddLine(ImVec2(consoleCenter, origin.y + boxH + 2.0f), ImVec2(consoleCenter, barTop - 2.0f),
                      ImGui::GetColorU32(color::kTextDim), 2.0f);
        const ImVec2 barMin(origin.x, barTop), barSize(inner, 60.0f);
        ImGui::SetCursorScreenPos(barMin);
        ImGui::PushID(kControllers);
        if (NavButton("##ports", barSize)) m_simPart = kControllers;
        const bool barHovered = ImGui::IsItemHovered();
        const bool barFocused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
        ImGui::PopID();
        const bool barSelected = (m_simPart == kControllers);
        const ImVec2 barMax(barMin.x + barSize.x, barMin.y + barSize.y);
        draw->AddRectFilled(barMin, barMax, ImGui::GetColorU32(barHovered ? color::kHover : color::kRaised),
                            radius::kCard);
        draw->AddRect(barMin, barMax,
                      ImGui::GetColorU32(barSelected || barFocused ? color::kAccentHover : color::kHairline),
                      radius::kCard, 0, barSelected || barFocused ? 2.0f : 1.0f);
        DrawIcon(draw, Icon::Games, ImVec2(barMin.x + space::k4 + 10.0f, barMin.y + barSize.y * 0.5f), 20.0f,
                 ImGui::GetColorU32(barSelected ? color::kAccentText : color::kTextMuted));
        PutText(
            draw, fonts.label,
            ImVec2(barMin.x + space::k4 + 30.0f, barMin.y + (barSize.y - Measure(fonts.label, "Controllers").y) * 0.5f),
            ImGui::GetColorU32(color::kText), "Controllers");
        const float slotsLeft = barMin.x + space::k4 + 30.0f + Measure(fonts.label, "Controllers").x + space::k6;
        const float slotW = std::floor((barMax.x - space::k4 - slotsLeft - space::k2 * 3.0f) / 4.0f);
        for (int p = 0; p < NovaInput::kMaxPlayers; ++p) {
            const bool connected = NovaInput::Gamepads::Instance().Connected(p);
            const char* device = connected ? "Controller" : (p == 0 ? "Keyboard" : "Empty");
            const ImVec2 a(slotsLeft + static_cast<float>(p) * (slotW + space::k2), barMin.y + 12.0f);
            const ImVec2 b(a.x + slotW, barMax.y - 12.0f);
            draw->AddRectFilled(a, b, ImGui::GetColorU32(color::kCard), radius::kControl);
            std::snprintf(line, sizeof(line), "%d", p + 1);
            PutText(draw, fonts.caption, ImVec2(a.x + space::k2, a.y + 4.0f), ImGui::GetColorU32(color::kTextDim),
                    line);
            PutText(draw, fonts.small, ImVec2(a.x + space::k2, a.y + 19.0f),
                    ImGui::GetColorU32(connected || p == 0 ? color::kText : color::kTextDim),
                    Ellipsize(fonts.small, device, slotW - space::k4).c_str());
        }
        // One line on whether this setup is fine, and a way back to Settings
        int errors = 0, warnings = 0, notes = 0;
        for (const Problem& problem : problems)
            (problem.severity == Severity::Error ? errors : problem.severity == Severity::Warning ? warnings : notes)++;
        const float stripTop = barTop + barSize.y + space::k3;
        const bool fine = errors == 0 && warnings == 0;
        DrawIcon(draw, fine ? Icon::Check : Icon::Warning, ImVec2(origin.x + 10.0f, stripTop + 12.0f), 20.0f,
                 ImGui::GetColorU32(errors     ? color::kDanger
                                    : warnings ? color::kStar
                                               : color::kSuccess));
        if (errors)
            std::snprintf(line, sizeof(line), "%d problem%s: this game will not start as set up.", errors,
                          errors == 1 ? "" : "s");
        else if (warnings)
            std::snprintf(line, sizeof(line), "%d warning%s. See Check below.", warnings, warnings == 1 ? "" : "s");
        else
            std::snprintf(line, sizeof(line), notes ? "Ready to play. %d note%s below." : "Ready to play.", notes,
                          notes == 1 ? "" : "s");
        PutText(draw, fonts.body, ImVec2(origin.x + 30.0f, stripTop + 2.0f), ImGui::GetColorU32(color::kText), line);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + inner - 170.0f, stripTop - 4.0f));
        if (!m_simSetup.Empty() && Button("Reset all changes", ButtonStyle::Ghost, Icon::None, ImVec2(170.0f, 32.0f))) {
            m_simSetup.ClearAll();
            m_simSetup.Save(rom.path);
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x, stripTop + 28.0f));
        ImGui::Dummy(ImVec2(inner, 1.0f));
    }
    EndCard();
    ImGui::Dummy(ImVec2(0.0f, space::k4));

    // --- The inspector: what belongs to the selected part ---
    const PartDef& part = GetPart(m_simPart);
    {
        ImGui::BeginGroup();
        SectionTitle(part.name);
        Muted("%s", part.summary);
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0.0f, space::k3));
    }

    auto overrideRow = [&](const OptionDef& option) {
        const bool custom = m_simSetup.Has(option.id);
        const int globalValue = option.get(global);
        const int shown = custom ? m_simSetup.Get(option.id, globalValue) : globalValue;
        const char* blocked = option.unavailable ? option.unavailable(effective) : nullptr;
        const char* warning = option.warning ? option.warning(effective) : nullptr;
        const char* text = blocked ? blocked : warning ? warning : option.help;
        const float controlWidth = detail::OptionControlWidth(option);
        bool changed = false;
        int value = shown;
        ImGui::BeginDisabled(blocked != nullptr);
        SettingRow(
            option.title, text, detail::kResetWidth + controlWidth, detail::OptionControlHeight(option),
            [&] {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                if (!custom) {
                    // The row follows Settings: show that value, and let the game take its own.
                    const std::string current = ValueText(option, globalValue);
                    const ImVec2 size = Measure(fonts.label, current.c_str());
                    PutText(ImGui::GetWindowDrawList(), fonts.label,
                            ImVec2(at.x + detail::kResetWidth,
                                   at.y + (detail::OptionControlHeight(option) - size.y) * 0.5f),
                            ImGui::GetColorU32(color::kTextDim), current.c_str());
                    ImGui::SetCursorScreenPos(ImVec2(at.x + detail::kResetWidth + controlWidth - 104.0f,
                                                     at.y + (detail::OptionControlHeight(option) - 32.0f) * 0.5f));
                    if (Button("Customize", ButtonStyle::Secondary, Icon::None, ImVec2(104.0f, 32.0f))) {
                        m_simSetup.Set(option.id, globalValue);
                        changed = true;
                        value = globalValue;
                    }
                    return false;
                }
                ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + (detail::OptionControlHeight(option) - 28.0f) * 0.5f));
                if (Button("Reset", ButtonStyle::Ghost, Icon::None, ImVec2(detail::kResetWidth - 8.0f, 28.0f))) {
                    m_simSetup.Clear(option.id);
                    changed = true;
                    return false;
                }
                ImGui::SetCursorScreenPos(ImVec2(at.x + detail::kResetWidth, at.y));
                if (detail::DrawOptionControl(option, &value, globalValue)) {
                    m_simSetup.Set(option.id, value);
                    changed = true;
                }
                return false;
            },
            custom ? "Custom" : nullptr);
        ImGui::EndDisabled();
        if (changed) m_simSetup.Save(rom.path);
    };

    if (m_simPart == kCartridge) {
        BeginSettings("##cartInfo", "Header");
        InfoRow("Title", m_simRom.valid ? m_simRom.title.c_str() : "-");
        InfoRow("Game code", m_simRom.valid ? (m_simRom.gameId + std::string(1, m_simRom.regionCode)).c_str() : "-");
        InfoRow("Region",
                m_simRom.valid
                    ? (std::string(m_simRom.region) + (m_simRom.pal ? ", PAL (50 Hz)" : ", NTSC (60 Hz)")).c_str()
                    : "-");
        InfoRow("Format", m_simRom.valid ? m_simRom.format : "-");
        std::snprintf(line, sizeof(line), "%.1f MB", static_cast<double>(m_simRom.size) / (1024.0 * 1024.0));
        InfoRow("Size", line);
        std::snprintf(line, sizeof(line), "%08X  %08X", m_simRom.crc1, m_simRom.crc2);
        InfoRow("Checksums", m_simRom.valid ? line : "-");
        EndSettings();
    }

    if (m_simPart == kControllers) {
        BeginSettings("##ports", nullptr);
        static const char* const kAccessory[] = {"Rumble Pak when the controller supports it", "None", "Rumble Pak"};
        for (int p = 0; p < NovaInput::kMaxPlayers; ++p) {
            const bool connected = NovaInput::Gamepads::Instance().Connected(p);
            std::snprintf(line, sizeof(line), "Player %d", p + 1);
            const char* device = connected ? "A game controller is connected."
                                 : p == 0  ? "No controller: the keyboard plays."
                                           : "Nothing is plugged in.";
            const std::string accessory = std::string("Accessory: ") +
                                          kAccessory[static_cast<int>(m_padSettings.accessory[static_cast<size_t>(p)])];
            SettingRow(line, device, 260.0f, 24.0f, [&] {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const ImVec2 size = Measure(fonts.small, accessory.c_str());
                PutText(ImGui::GetWindowDrawList(), fonts.small,
                        ImVec2(at.x + 260.0f - size.x, at.y + (24.0f - size.y) * 0.5f),
                        ImGui::GetColorU32(color::kTextMuted), accessory.c_str());
                return false;
            });
        }
        EndSettings();
        Muted("Controllers belong to the whole launcher, not to one game. They are set in Settings.");
        ImGui::Dummy(ImVec2(0.0f, space::k2));
        if (Button("Open controller settings", ButtonStyle::Secondary, Icon::Settings, ImVec2(0.0f, 36.0f))) {
            m_settingsPage = 3;
            m_activeTab = Tab::Settings;
        }
        ImGui::Dummy(ImVec2(0.0f, space::k4));
    } else {
        BeginSettings(part.name, m_simPart == kCartridge ? "Save chip" : nullptr);
        for (const char* id : part.options)
            if (const OptionDef* option = FindOption(id)) overrideRow(*option);
        EndSettings();
    }

    ImGui::PushFont(fonts.heading);
    ImGui::TextUnformatted("Check");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, space::k2));
    BeginCard("##problems", ImVec2(rightWidth, 0), true, ImVec2(space::k4, space::k3));
    if (problems.empty()) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        DrawIcon(ImGui::GetWindowDrawList(), Icon::Check, ImVec2(at.x + 10.0f, at.y + 12.0f), 20.0f,
                 ImGui::GetColorU32(color::kSuccess));
        ImGui::SetCursorScreenPos(ImVec2(at.x + 30.0f, at.y));
        ImGui::TextUnformatted("Nothing to fix. This game is ready to play.");
    }
    for (const Problem& problem : problems) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const Icon icon = problem.severity == Severity::Info ? Icon::Info : Icon::Warning;
        const ImVec4& tint = problem.severity == Severity::Error     ? color::kDanger
                             : problem.severity == Severity::Warning ? color::kStar
                                                                     : color::kTextMuted;
        DrawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(at.x + 10.0f, at.y + 10.0f), 20.0f, ImGui::GetColorU32(tint));
        ImGui::SetCursorScreenPos(ImVec2(at.x + 30.0f, at.y));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(problem.text.c_str());
        ImGui::PopTextWrapPos();
    }
    EndCard();
    ImGui::Dummy(ImVec2(0.0f, space::k6));

    ImGui::EndGroup();
    kit::EndColumn();
}

} // namespace NovaUI
