/**
 * @file LauncherSimulator.cpp
 * @brief Simulator page: a workbench for one game's virtual hardware, in the manner of a network simulator.
 *
 * Devices (console, TV, controllers, a Rumble Pak, the game's cartridge) are dragged from the palette onto the
 * workbench and joined with cables. A link light on each cable end says whether it works. Clicking a device shows
 * its options in the inspector; every option is one a game can override (see GameSetup.hpp), and a Rumble Pak in a
 * controller sets that player's pak. "Play" starts the game with exactly that setup, and Simulation mode steps
 * through the power-on sequence first, event by event, so a setup that will not work says why.
 */
#include "GameInfo.hpp"
#include "LauncherInternal.hpp"
#include "NovaLauncher.hpp"
#include "UiKit.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace NovaUI {

namespace {

using namespace sim;

constexpr float kPanelWidth = 372.0f;
constexpr float kPaletteHeight = 96.0f;
constexpr float kToolbarHeight = 44.0f;
constexpr float kGrid = 12.0f;

kit::Icon DeviceIcon(DeviceKind kind) {
    switch (kind) {
    case DeviceKind::Console:    return kit::Icon::Memory;
    case DeviceKind::Tv:         return kit::Icon::Display;
    case DeviceKind::Controller: return kit::Icon::Games;
    case DeviceKind::RumblePak:  return kit::Icon::Lightning;
    case DeviceKind::Cartridge:  return kit::Icon::Tag;
    }
    return kit::Icon::None;
}

ImVec2 Normal(Facing facing) {
    switch (facing) {
    case Facing::Left:   return ImVec2(-1.0f, 0.0f);
    case Facing::Right:  return ImVec2(1.0f, 0.0f);
    case Facing::Top:    return ImVec2(0.0f, -1.0f);
    case Facing::Bottom: return ImVec2(0.0f, 1.0f);
    }
    return ImVec2(0.0f, 0.0f);
}

ImVec2 Add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
ImVec2 Scale(ImVec2 a, float s) { return ImVec2(a.x * s, a.y * s); }

ImVec2 Bezier(const ImVec2 (&p)[4], float t) {
    const float u = 1.0f - t;
    const float a = u * u * u, b = 3.0f * u * u * t, c = 3.0f * u * t * t, d = t * t * t;
    return ImVec2(a * p[0].x + b * p[1].x + c * p[2].x + d * p[3].x, a * p[0].y + b * p[1].y + c * p[2].y + d * p[3].y);
}

float SnapToGrid(float v) { return std::round(v / kGrid) * kGrid; }

ImU32 Col(const ImVec4& c) { return ImGui::GetColorU32(c); }

const ImVec4& CableColor(PortType type) {
    switch (type) {
    case PortType::Av:         return kit::color::kStar;
    case PortType::Controller: return kit::color::kTextMuted;
    case PortType::Pak:        return kit::color::kAccentText;
    case PortType::Cartridge:  return kit::color::kTextDim;
    }
    return kit::color::kTextMuted;
}

const ImVec4& LinkColor(LinkState state) {
    switch (state) {
    case LinkState::Up:      return kit::color::kSuccess;
    case LinkState::Waiting: return kit::color::kStar;
    case LinkState::Down:    return kit::color::kDanger;
    }
    return kit::color::kDanger;
}

const ImVec4& SeverityColor(Severity severity) {
    return severity == Severity::Error     ? kit::color::kDanger
           : severity == Severity::Warning ? kit::color::kStar
                                           : kit::color::kTextMuted;
}

std::string ChoiceLabel(const char* id, const SettingsModel& model) {
    const OptionDef* option = FindOption(id);
    return option ? ValueText(*option, option->get(model)) : std::string();
}

// A cable between two ports, as the four points of a cubic curve in screen space.
struct CablePath {
    ImVec2 p[4];
};

CablePath MakePath(ImVec2 a, Facing fa, ImVec2 b, Facing fb) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float k = std::clamp(std::sqrt(dx * dx + dy * dy) * 0.5f, 36.0f, 140.0f);
    return CablePath{{a, Add(a, Scale(Normal(fa), k)), Add(b, Scale(Normal(fb), k)), b}};
}

float DistanceToPath(const CablePath& path, ImVec2 mouse) {
    float best = 1e9f;
    ImVec2 prev = path.p[0];
    for (int i = 1; i <= 24; ++i) {
        const ImVec2 next = Bezier(path.p, static_cast<float>(i) / 24.0f);
        const ImVec2 ab(next.x - prev.x, next.y - prev.y), ap(mouse.x - prev.x, mouse.y - prev.y);
        const float len2 = ab.x * ab.x + ab.y * ab.y;
        const float t = len2 > 0.0f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / len2, 0.0f, 1.0f) : 0.0f;
        const float dx = prev.x + ab.x * t - mouse.x, dy = prev.y + ab.y * t - mouse.y;
        best = std::min(best, std::sqrt(dx * dx + dy * dy));
        prev = next;
    }
    return best;
}

} // namespace

void NovaLauncher::OpenSimulator(int gameIndex) {
    if (gameIndex >= 0 && gameIndex < static_cast<int>(m_library.size())) m_simGame = gameIndex;
    m_simView.selDevice = 0;
    m_simView.selCable = 0;
    m_activeTab = Tab::Simulator;
}

void NovaLauncher::RenderSimulatorTab() {
    using namespace kit;
    const Fonts& fonts = GetFonts();
    ImGuiIO& io = ImGui::GetIO();

    if (m_library.empty()) {
        BeginColumn(1000.0f);
        PageTitle("Simulator", "Wire up a Nintendo 64 for a game, check it, and play it.");
        BeginCard("##simEmpty", ImVec2(0, 0), true, ImVec2(space::k8, space::k6));
        ImGui::TextUnformatted("Add a game to the library first.");
        ImGui::Dummy(ImVec2(0.0f, space::k2));
        if (Button("Add game", ButtonStyle::Primary, Icon::Plus, ImVec2(150.0f, 0.0f))) OpenAddDialog();
        EndCard();
        EndColumn();
        return;
    }

    if (m_simGame < 0 || m_simGame >= static_cast<int>(m_library.size()))
        m_simGame = (m_selectedGame >= 0 && m_selectedGame < static_cast<int>(m_library.size())) ? m_selectedGame : 0;
    RomMetadata& rom = m_library[static_cast<size_t>(m_simGame)];
    if (m_simLoaded != m_simGame) {
        m_simSetup.Load(rom.path);
        m_simRom = detail::ReadRomInfo(rom.path);
        if (!m_simLab.Load(rom.path)) m_simLab = Lab::Default();
        m_simView = View();
        m_simLoaded = m_simGame;
    }
    View& view = m_simView;
    Lab& lab = m_simLab;
    const GameInfo* info = FindGameInfo(rom.name);

    // For screenshots: NOVA64_UI_TAB=simulator/simulation or simulator/simulation:N
    bool jumpToStep = false;
    int forcedStep = -1;
    if (m_frameCount < 3 && m_forcedSubTab.compare(0, 10, "simulation") == 0) {
        view.simulation = true;
        jumpToStep = true;
        forcedStep = m_forcedSubTab.size() > 11 ? std::atoi(m_forcedSubTab.c_str() + 11) : 99;
    }

    // The global values, and what this game will actually use
    int noChip = 0;
    int globalPaks[4] = {0, 0, 0, 0};
    SettingsModel global{&m_launch, &m_gfx, &noChip, globalPaks};
    LaunchOptions effLaunch = m_launch;
    Nova64::Video::PluginConfig effGfx = m_gfx;
    int effChip = 0;
    int effPaks[4] = {0, 0, 0, 0};
    SettingsModel effective{&effLaunch, &effGfx, &effChip, effPaks};
    m_simSetup.Apply(effective);

    std::error_code ec;
    const bool romExists = fs::exists(rom.path, ec);
    const bool isDisk = IsN64DDDisk(rom.path);
    const bool cartridgeOk = romExists && (m_simRom.valid || isDisk);

    // Whatever changes on the workbench is saved at once, together with the pak each player has
    auto save = [&] {
        lab.Save(rom.path);
        for (int p = 0; p < 4; ++p) {
            const std::string id = "accessory" + std::to_string(p + 1);
            if (lab.PlayerHasRumblePak(p)) m_simSetup.Set(id, 2);
            else m_simSetup.Clear(id);
        }
        m_simSetup.Save(rom.path);
    };

    // --- What is wrong, or worth knowing ---
    std::vector<Problem> problems = lab.Check(cartridgeOk);
    if (const OptionDef* backend = FindOption("backendType"))
        if (const char* why = backend->choiceUnavailable ? backend->choiceUnavailable(effGfx.backendType) : nullptr)
            problems.push_back(
                {Severity::Error,
                 std::string(effGfx.backendType == 1 ? "Vulkan" : "OpenGL") + " can't be used here: " + why,
                 lab.First(DeviceKind::Tv)});
    if (effGfx.renderingMode == Nova64::Video::RenderingMode::LLE && effLaunch.rspEngine == 1)
        problems.push_back({Severity::Warning, "Low-level graphics on the RSP interpreter will be very slow.",
                            lab.First(DeviceKind::Console)});
    if (effGfx.rayTracing && effGfx.renderingMode != Nova64::Video::RenderingMode::HLE)
        problems.push_back(
            {Severity::Warning, "Ray-traced lighting only works with high-level graphics.", lab.First(DeviceKind::Tv)});
    if (effGfx.rayTracing && effGfx.backendType != 1)
        problems.push_back(
            {Severity::Warning, "Ray-traced lighting needs the Vulkan driver.", lab.First(DeviceKind::Tv)});
    if (m_simRom.valid && m_simRom.pal)
        problems.push_back({Severity::Info, "A PAL game: it runs at 50 Hz, so full speed is 50 frames a second.",
                            lab.First(DeviceKind::Cartridge)});
    if (info && info->needsExpansionPak)
        problems.push_back({Severity::Info,
                            std::string(info->title) + " needs the Expansion Pak. The console always has it (8 MB).",
                            lab.First(DeviceKind::Console)});
    if (effChip != 0)
        problems.push_back({Severity::Info,
                            std::string("The save chip is forced to ") + (effChip == 1 ? "SRAM" : "FlashRAM") + ".",
                            lab.First(DeviceKind::Cartridge)});
    int errors = 0;
    for (const Problem& p : problems)
        if (p.severity == Severity::Error) ++errors;

    // --- Words for what each device shows ---
    const std::string regionText = m_simRom.valid
                                       ? std::string(m_simRom.region) + (m_simRom.pal ? ", PAL 50 Hz" : ", NTSC 60 Hz")
                                       : (isDisk ? "64DD disk" : "Not an N64 ROM");
    const std::string chipText = effChip == 1 ? "SRAM" : effChip == 2 ? "FlashRAM" : "detected from the game";
    const std::string resText = ChoiceLabel("internalResScale", effective);

    // --- The power-on sequence of Simulation mode ---
    auto buildSteps = [&] {
        std::vector<Step> steps;
        const int console = lab.First(DeviceKind::Console);
        const int tv = lab.First(DeviceKind::Tv);
        const int cartridge = lab.First(DeviceKind::Cartridge);
        auto add = [&](int from, int to, const Cable* cable, std::string text, Severity result) {
            Step step;
            step.from = from;
            step.to = to;
            step.cable = cable ? cable->id : 0;
            step.text = std::move(text);
            step.result = result;
            steps.push_back(std::move(step));
        };
        if (!console) {
            add(0, 0, nullptr, "There is no console on the workbench.", Severity::Error);
            return steps;
        }
        if (!lab.Find(console)->powered) {
            add(console, 0, nullptr, "The console is switched off, so nothing happens.", Severity::Error);
            return steps;
        }
        add(console, 0, nullptr,
            "Power on. The VR4300 CPU and the RCP start, with 8 MB of RDRAM (the Expansion Pak is fitted).",
            Severity::Info);
        const Cable* cartCable = lab.CableAt(console, kConsoleCartridge);
        if (!cartridge || !cartCable) {
            add(console, cartridge, nullptr, "The cartridge slot is empty. There is nothing to boot.", Severity::Error);
            return steps;
        }
        if (!cartridgeOk) {
            add(cartridge, console, cartCable, "The game's file is missing, or it is not an N64 ROM.", Severity::Error);
            return steps;
        }
        add(cartridge, console, cartCable,
            "Cartridge read: " + (m_simRom.valid ? m_simRom.title + ", " + m_simRom.format : rom.name) + ".",
            Severity::Info);
        add(console, 0, nullptr,
            "Region " + regionText + ": the video clock and frame pacing follow it. Save chip: " + chipText + ".",
            Severity::Info);
        for (int p = 0; p < 4; ++p) {
            const int controller = lab.ControllerOnPort(p);
            if (!controller) {
                if (p == 0)
                    add(console, 0, nullptr, "No controller in port 1. Most games wait for player 1.",
                        Severity::Warning);
                continue;
            }
            const bool connected = NovaInput::Gamepads::Instance().Connected(p);
            std::string text = "Controller " + std::to_string(p + 1) + " detected (" +
                               (connected ? "a game controller"
                                : p == 0  ? "the keyboard"
                                          : "no device is attached to this player") +
                               ")";
            if (lab.PakOf(controller)) text += ", with a Rumble Pak in its slot";
            add(controller, console, lab.CableAt(controller, 0), text + ".",
                connected || p == 0 ? Severity::Info : Severity::Warning);
        }
        const Cable* av = lab.CableAt(console, kConsoleAv);
        if (!tv || !av) {
            add(console, tv, nullptr, "The AV output goes nowhere: no TV is connected, so there is no picture.",
                Severity::Error);
            return steps;
        }
        if (!lab.Find(tv)->powered) {
            add(console, tv, av, "The video signal reaches a TV that is switched off.", Severity::Error);
            return steps;
        }
        add(console, tv, av,
            "Video signal: " + resText + ", " +
                (effGfx.aspectRatio == Nova64::Video::AspectRatio::Widescreen16_9 ? "16:9" : "4:3") + ", drawn by " +
                ChoiceLabel("backendType", effective) +
                (effLaunch.displayMode == 1 ? " fullscreen" : " in the launcher window") + ".",
            Severity::Info);
        add(tv, 0, nullptr, "Picture on screen. Ready: press Play.", Severity::Info);
        return steps;
    };
    if (view.simulation && (view.steps.empty() || jumpToStep)) {
        view.steps = buildSteps();
        if (jumpToStep) view.step = std::min(forcedStep, static_cast<int>(view.steps.size()) - 1);
        view.stepStart = ImGui::GetTime();
    }

    // === The toolbar ===
    const float top = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX(), top + 7.0f));
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::BeginCombo("##simGame", rom.name.c_str())) {
        for (int i = 0; i < static_cast<int>(m_library.size()); ++i)
            if (ImGui::Selectable(m_library[static_cast<size_t>(i)].name.c_str(), i == m_simGame)) m_simGame = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine(0.0f, space::k4);
    {
        static const char* const kModes[] = {"Realtime", "Simulation"};
        int mode = view.simulation ? 1 : 0;
        ImGui::SetCursorPosY(top + 6.0f);
        if (Segmented("##simMode", kModes, 2, &mode)) {
            view.simulation = (mode == 1);
            if (view.simulation) {
                view.steps = buildSteps();
                view.step = -1;
                view.autoplay = false;
            }
        }
    }
    const float right = ImGui::GetContentRegionMax().x;
    ImGui::SetCursorPos(ImVec2(right - 130.0f, top));
    ImGui::BeginDisabled(errors > 0);
    if (Button("Play", ButtonStyle::Primary, Icon::Play, ImVec2(130.0f, kToolbarHeight))) PlayRom(rom.path, rom.name);
    ImGui::EndDisabled();
    if (errors > 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Fix the problems on the workbench first.");
    ImGui::SetCursorPos(ImVec2(right - 130.0f - space::k2 - 88.0f, top + 6.0f));
    if (Button("Reset", ButtonStyle::Ghost, Icon::None, ImVec2(88.0f, 32.0f))) {
        lab = Lab::Default();
        view.selDevice = view.selCable = 0;
        view.fitPending = true;
        save();
    }
    ImGui::SetCursorPos(ImVec2(right - 130.0f - space::k2 * 2.0f - 88.0f - 64.0f, top + 6.0f));
    if (Button("Fit", ButtonStyle::Ghost, Icon::None, ImVec2(64.0f, 32.0f))) view.fitPending = true;
    ImGui::SetCursorPosY(top + kToolbarHeight + space::k3);

    // === Workbench, palette and panel ===
    const float bodyHeight = std::max(300.0f, ImGui::GetContentRegionAvail().y - 2.0f);
    const float canvasWidth = ImGui::GetContentRegionAvail().x - kPanelWidth - space::k4;
    const float canvasHeight = bodyHeight - kPaletteHeight - space::k3;

    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col(color::kSidebar));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(color::kHairline));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##workbench", ImVec2(canvasWidth, canvasHeight),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::GetContentRegionAvail();
        ImDrawList* draw = ImGui::GetWindowDrawList();

        // "Fit" puts the devices in the middle of the view
        if (view.fitPending && !lab.devices.empty()) {
            ImVec2 lo(1e9f, 1e9f), hi(-1e9f, -1e9f);
            for (const Device& d : lab.devices) {
                lo = ImVec2(std::min(lo.x, d.pos.x), std::min(lo.y, d.pos.y));
                hi = ImVec2(std::max(hi.x, d.pos.x + Def(d.kind).size.x), std::max(hi.y, d.pos.y + Def(d.kind).size.y));
            }
            // The legend takes the top and the status the bottom of the view
            const float usable = size.y - 34.0f - 50.0f;
            view.pan = ImVec2(std::round((size.x - (hi.x - lo.x)) * 0.5f - lo.x),
                              std::round(34.0f + (usable - (hi.y - lo.y)) * 0.5f - lo.y));
            view.fitPending = false;
        }
        auto toScreen = [&](ImVec2 w) { return ImVec2(origin.x + view.pan.x + w.x, origin.y + view.pan.y + w.y); };

        // The grid
        {
            const float step = kGrid * 2.0f;
            const float startX = std::fmod(view.pan.x, step) - step, startY = std::fmod(view.pan.y, step) - step;
            for (float y = startY; y < size.y + step; y += step)
                for (float x = startX; x < size.x + step; x += step)
                    draw->AddRectFilled(ImVec2(origin.x + x, origin.y + y),
                                        ImVec2(origin.x + x + 2.0f, origin.y + y + 2.0f), Col(Rgb(0xFFEADB, 0.07f)));
        }

        // The pointer
        NavButton("##surface", size);
        const bool surfaceHovered = ImGui::IsItemHovered();
        const bool surfaceActive = ImGui::IsItemActive();
        const bool surfaceActivated = ImGui::IsItemActivated();
        const bool surfaceDeactivated = ImGui::IsItemDeactivated();
        const bool surfaceFocused = ImGui::IsItemFocused();
        const ImVec2 mouse = io.MousePos;
        const ImVec2 world(mouse.x - origin.x - view.pan.x, mouse.y - origin.y - view.pan.y);

        // What is under the pointer: a port, a device, or a cable
        struct Hit {
            int device = 0;
            int port = -1;
            int cable = 0;
            bool power = false;
        };
        auto powerCenter = [&](const Device& d) {
            return ImVec2(d.pos.x + Def(d.kind).size.x - 20.0f, d.pos.y + 20.0f);
        };
        auto cablePath = [&](const Cable& c) {
            const Device& a = *lab.Find(c.a);
            const Device& b = *lab.Find(c.b);
            return MakePath(toScreen(lab.PortPos(a, c.portA)), Def(a.kind).ports[static_cast<size_t>(c.portA)].facing,
                            toScreen(lab.PortPos(b, c.portB)), Def(b.kind).ports[static_cast<size_t>(c.portB)].facing);
        };
        auto hitTest = [&]() {
            Hit hit;
            for (auto it = lab.devices.rbegin(); it != lab.devices.rend(); ++it) {
                const DeviceDef& def = Def(it->kind);
                for (int port = 0; port < static_cast<int>(def.ports.size()); ++port) {
                    const ImVec2 at = lab.PortPos(*it, port);
                    if (std::hypot(world.x - at.x, world.y - at.y) <= 11.0f) {
                        hit.device = it->id;
                        hit.port = port;
                        return hit;
                    }
                }
            }
            for (auto it = lab.devices.rbegin(); it != lab.devices.rend(); ++it) {
                const DeviceDef& def = Def(it->kind);
                if (it->kind == DeviceKind::Console || it->kind == DeviceKind::Tv) {
                    const ImVec2 pc = powerCenter(*it);
                    if (std::hypot(world.x - pc.x, world.y - pc.y) <= 12.0f) {
                        hit.device = it->id;
                        hit.power = true;
                        return hit;
                    }
                }
                if (world.x >= it->pos.x && world.x <= it->pos.x + def.size.x && world.y >= it->pos.y &&
                    world.y <= it->pos.y + def.size.y) {
                    hit.device = it->id;
                    return hit;
                }
            }
            float best = 8.0f;
            for (const Cable& c : lab.cables) {
                const float d = DistanceToPath(cablePath(c), mouse);
                if (d < best) {
                    best = d;
                    hit.cable = c.id;
                }
            }
            return hit;
        };
        const Hit hit = (surfaceHovered || surfaceActive) ? hitTest() : Hit{};

        // Dropping a device from the palette
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SIM_DEVICE")) {
                const DeviceKind kind = *static_cast<const DeviceKind*>(payload->Data);
                const DeviceDef& def = Def(kind);
                const int id = lab.Add(
                    kind, ImVec2(SnapToGrid(world.x - def.size.x * 0.5f), SnapToGrid(world.y - def.size.y * 0.5f)));
                if (id) {
                    view.selDevice = id;
                    view.selCable = 0;
                    save();
                }
            }
            ImGui::EndDragDropTarget();
        }

        // Pressing, dragging, releasing
        if (surfaceActivated && io.MouseDown[0]) {
            view.pressAt = mouse;
            view.moved = false;
            view.pickedCable = 0;
            if (hit.power) {
                if (Device* d = lab.Find(hit.device)) {
                    d->powered = !d->powered;
                    view.selDevice = d->id;
                    view.selCable = 0;
                    save();
                }
                view.drag = View::Drag::None;
            } else if (hit.port >= 0) {
                if (const Cable* existing = lab.CableAt(hit.device, hit.port)) {
                    view.drag = View::Drag::PickUp;
                    view.pickedCable = existing->id;
                } else {
                    view.drag = View::Drag::Cable;
                }
                view.dragFromDevice = hit.device;
                view.dragFromPort = hit.port;
                view.selDevice = hit.device;
                view.selCable = 0;
            } else if (hit.device) {
                view.drag = View::Drag::Device;
                view.dragDevice = hit.device;
                view.selDevice = hit.device;
                view.selCable = 0;
                const Device* d = lab.Find(hit.device);
                view.dragGrab = ImVec2(world.x - d->pos.x, world.y - d->pos.y);
            } else if (hit.cable) {
                view.selCable = hit.cable;
                view.selDevice = 0;
                view.drag = View::Drag::None;
            } else {
                view.selDevice = view.selCable = 0;
                view.drag = View::Drag::Pan;
            }
        }
        if (surfaceActive && io.MouseDown[0]) {
            if (std::hypot(mouse.x - view.pressAt.x, mouse.y - view.pressAt.y) > 5.0f) view.moved = true;
            switch (view.drag) {
            case View::Drag::Pan: view.pan = ImVec2(view.pan.x + io.MouseDelta.x, view.pan.y + io.MouseDelta.y); break;
            case View::Drag::Device:
                if (view.moved)
                    if (Device* d = lab.Find(view.dragDevice))
                        d->pos = ImVec2(world.x - view.dragGrab.x, world.y - view.dragGrab.y);
                break;
            case View::Drag::PickUp:
                if (view.moved) {
                    // Lift the cable off this port and carry its other end
                    if (const Cable* c = lab.CableAt(view.dragFromDevice, view.dragFromPort)) {
                        const bool fromIsA = (c->a == view.dragFromDevice);
                        const int otherDevice = fromIsA ? c->b : c->a;
                        const int otherPort = fromIsA ? c->portB : c->portA;
                        lab.RemoveCable(c->id);
                        view.dragFromDevice = otherDevice;
                        view.dragFromPort = otherPort;
                        view.drag = View::Drag::Cable;
                    }
                }
                break;
            default: break;
            }
        }
        if (surfaceDeactivated) {
            if (view.drag == View::Drag::Cable) {
                const Hit drop = hitTest();
                const bool lifted = view.pickedCable != 0;
                if (drop.port >= 0 && lab.Connect(view.dragFromDevice, view.dragFromPort, drop.device, drop.port))
                    save();
                else if (lifted) save(); // a cable was lifted off and dropped nowhere: it is gone
            } else if (view.drag == View::Drag::Device && view.moved) {
                if (Device* d = lab.Find(view.dragDevice)) d->pos = ImVec2(SnapToGrid(d->pos.x), SnapToGrid(d->pos.y));
                save();
            } else if (view.drag == View::Drag::PickUp && !view.moved) {
                view.selCable = view.pickedCable;
            }
            view.drag = View::Drag::None;
            view.pickedCable = 0;
        }
        if (surfaceHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))
            view.pan = ImVec2(view.pan.x + io.MouseDelta.x, view.pan.y + io.MouseDelta.y);

        // Right click: a menu for what is under the pointer
        if (surfaceHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && (hit.device || hit.cable)) {
            view.contextDevice = hit.device;
            view.contextCable = hit.device ? 0 : hit.cable;
            if (hit.device) view.selDevice = hit.device;
            ImGui::OpenPopup("##simContext");
        }
        if (surfaceHovered || surfaceFocused) {
            if ((ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) &&
                (view.selDevice || view.selCable)) {
                if (view.selDevice) lab.Remove(view.selDevice);
                else lab.RemoveCable(view.selCable);
                view.selDevice = view.selCable = 0;
                save();
            }
        }

        // The current event of the power-on sequence lights up what it touches
        const Step* current = (view.simulation && view.step >= 0 && view.step < static_cast<int>(view.steps.size()))
                                  ? &view.steps[static_cast<size_t>(view.step)]
                                  : nullptr;

        // --- Cables ---
        for (const Cable& c : lab.cables) {
            const CablePath path = cablePath(c);
            const PortType type = Def(lab.Find(c.a)->kind).ports[static_cast<size_t>(c.portA)].type;
            const bool selected = (view.selCable == c.id);
            const bool lit = current && current->cable == c.id;
            draw->AddBezierCubic(path.p[0], path.p[1], path.p[2], path.p[3], Col(color::kSidebar), 6.0f);
            draw->AddBezierCubic(path.p[0], path.p[1], path.p[2], path.p[3],
                                 Col(selected || lit ? color::kAccentHover : CableColor(type)),
                                 selected || lit ? 3.5f : 2.5f);
        }

        // --- Devices ---
        for (const Device& d : lab.devices) {
            const DeviceDef& def = Def(d.kind);
            const ImVec2 min = toScreen(d.pos);
            const ImVec2 max(min.x + def.size.x, min.y + def.size.y);
            const bool selected = (view.selDevice == d.id);
            const bool lit = current && (current->from == d.id || current->to == d.id);
            const bool hovered = hit.device == d.id && !hit.power && surfaceHovered && view.drag == View::Drag::None;
            draw->AddRectFilled(min, max, Col(hovered ? color::kHover : color::kRaised), radius::kCard);
            draw->AddRect(min, max, Col(selected || lit ? color::kAccentHover : color::kHairline), radius::kCard, 0,
                          selected || lit ? 2.0f : 1.0f);
            DrawIcon(draw, DeviceIcon(d.kind), ImVec2(min.x + 22.0f, min.y + 22.0f), 20.0f,
                     Col(selected || lit ? color::kAccentText : color::kTextMuted));
            PutText(draw, fonts.label, ImVec2(min.x + 40.0f, min.y + 12.0f), Col(color::kText), def.name);

            std::string l1, l2, l3;
            switch (d.kind) {
            case DeviceKind::Console:
                l1 = "CPU: " + ChoiceLabel("cpuCore", effective);
                l2 = "RSP: " + ChoiceLabel("rspEngine", effective);
                l3 = "8 MB  \xC2\xB7  " + ChoiceLabel("speed", effective);
                break;
            case DeviceKind::Tv:
                l1 = std::string(effGfx.backendType == 1 ? "Vulkan" : "OpenGL") + "  \xC2\xB7  " + resText;
                l2 = effGfx.aspectRatio == Nova64::Video::AspectRatio::Widescreen16_9 ? "16:9" : "4:3";
                l3 = effGfx.renderingMode == Nova64::Video::RenderingMode::LLE ? "low-level graphics"
                                                                               : "high-level graphics";
                break;
            case DeviceKind::Controller: {
                const int player = lab.PlayerOf(d.id);
                l1 = player >= 0 ? "Player " + std::to_string(player + 1) : "Not plugged in";
                l2 = player >= 0 ? (NovaInput::Gamepads::Instance().Connected(player) ? "Game controller"
                                    : player == 0                                     ? "Keyboard"
                                                                                      : "No device")
                                 : "";
                l3 = lab.PakOf(d.id) ? "Rumble Pak" : "";
                break;
            }
            case DeviceKind::RumblePak:
                l1 = "Force feedback";
                if (const Cable* c = lab.CableAt(d.id, 0)) {
                    const int player = lab.PlayerOf(c->a == d.id ? c->b : c->a);
                    l2 = player >= 0 ? "Player " + std::to_string(player + 1) : "";
                }
                break;
            case DeviceKind::Cartridge:
                l1 = rom.name;
                l2 = m_simRom.valid
                         ? std::string(m_simRom.region) + (m_simRom.pal ? "  \xC2\xB7  50 Hz" : "  \xC2\xB7  60 Hz")
                         : (isDisk ? "64DD disk" : "Not a ROM");
                l3 = "Saves: " + chipText;
                break;
            }
            const std::string lines[3] = {l1, l2, l3};
            float y = min.y + 40.0f;
            for (const std::string& line : lines) {
                if (line.empty()) continue;
                PutText(draw, fonts.small, ImVec2(min.x + 12.0f, y), Col(color::kTextMuted),
                        Ellipsize(fonts.small, line, def.size.x - 24.0f).c_str());
                y += 17.0f;
            }
            if (d.kind == DeviceKind::Console || d.kind == DeviceKind::Tv) {
                const ImVec2 pc = toScreen(powerCenter(d));
                const bool powerHover = hit.power && hit.device == d.id && surfaceHovered;
                draw->AddCircleFilled(pc, 11.0f, Col(powerHover ? color::kHover : color::kCard));
                draw->AddCircle(pc, 11.0f, Col(d.powered ? color::kSuccess : color::kHairline), 0, 1.5f);
                DrawIcon(draw, Icon::Lightning, pc, 14.0f, Col(d.powered ? color::kSuccess : color::kTextDim));
            }
        }

        // --- Ports and link lights ---
        const bool carrying = view.drag == View::Drag::Cable;
        for (const Device& d : lab.devices) {
            const DeviceDef& def = Def(d.kind);
            for (int port = 0; port < static_cast<int>(def.ports.size()); ++port) {
                const PortDef& pd = def.ports[static_cast<size_t>(port)];
                const ImVec2 at = toScreen(lab.PortPos(d, port));
                const Cable* cable = lab.CableAt(d.id, port);
                const bool hoverPort = hit.device == d.id && hit.port == port && surfaceHovered;
                const bool target = carrying && lab.CanConnect(view.dragFromDevice, view.dragFromPort, d.id, port);
                draw->AddCircleFilled(at, 6.0f, Col(cable ? CableColor(pd.type) : color::kCard));
                draw->AddCircle(at, 6.0f,
                                Col(target      ? color::kSuccess
                                    : hoverPort ? color::kAccentHover
                                                : color::kHairline),
                                0, target || hoverPort ? 2.0f : 1.5f);
                if (target) draw->AddCircle(at, 10.0f, Col(color::kSuccess), 0, 1.5f);
                if (cable)
                    draw->AddCircleFilled(Add(at, Scale(Normal(pd.facing), 14.0f)), 4.5f,
                                          Col(LinkColor(lab.State(*cable, cartridgeOk))));
            }
        }
        if (surfaceHovered && hit.port >= 0 && view.drag == View::Drag::None) {
            const Device* d = lab.Find(hit.device);
            ImGui::SetTooltip("%s: %s", Def(d->kind).name, Def(d->kind).ports[static_cast<size_t>(hit.port)].name);
        }

        // The cable being pulled
        if (carrying) {
            if (const Device* from = lab.Find(view.dragFromDevice)) {
                const PortDef& pd = Def(from->kind).ports[static_cast<size_t>(view.dragFromPort)];
                const ImVec2 start = toScreen(lab.PortPos(*from, view.dragFromPort));
                const ImVec2 toMouse(mouse.x - start.x, mouse.y - start.y);
                const float k =
                    std::clamp(std::sqrt(toMouse.x * toMouse.x + toMouse.y * toMouse.y) * 0.5f, 24.0f, 120.0f);
                draw->AddBezierCubic(start, Add(start, Scale(Normal(pd.facing), k)),
                                     ImVec2(mouse.x - toMouse.x * 0.25f, mouse.y - toMouse.y * 0.25f), mouse,
                                     Col(CableColor(pd.type)), 2.5f);
            }
        }

        // The envelope that travels the cable during Simulation
        if (current && current->cable) {
            for (const Cable& c : lab.cables) {
                if (c.id != current->cable) continue;
                const CablePath path = cablePath(c);
                float t = std::clamp(static_cast<float>((ImGui::GetTime() - view.stepStart) / 0.9), 0.0f, 1.0f);
                if (c.a != current->from) t = 1.0f - t;
                const ImVec2 at = Bezier(path.p, t);
                draw->AddRectFilled(ImVec2(at.x - 9.0f, at.y - 6.5f), ImVec2(at.x + 9.0f, at.y + 6.5f),
                                    Col(color::kAccentHover), 3.0f);
                draw->AddLine(ImVec2(at.x - 8.0f, at.y - 5.0f), ImVec2(at.x, at.y + 1.0f), Col(color::kOnAccent), 1.5f);
                draw->AddLine(ImVec2(at.x, at.y + 1.0f), ImVec2(at.x + 8.0f, at.y - 5.0f), Col(color::kOnAccent), 1.5f);
            }
        }

        // What is wrong, in the bottom left corner: errors and warnings first
        {
            std::vector<const Problem*> shown;
            for (const Problem& p : problems)
                if (p.severity != Severity::Info && shown.size() < 3) shown.push_back(&p);
            float y = origin.y + size.y - space::k3;
            if (shown.empty()) {
                const char* ready = "Ready to play";
                const ImVec2 ts = Measure(fonts.small, ready);
                const ImVec2 a(origin.x + space::k3, y - 26.0f);
                draw->AddRectFilled(a, ImVec2(a.x + ts.x + 40.0f, y), Col(color::kRaised), 13.0f);
                draw->AddRect(a, ImVec2(a.x + ts.x + 40.0f, y), Col(color::kHairline), 13.0f);
                DrawIcon(draw, Icon::Check, ImVec2(a.x + 16.0f, a.y + 13.0f), 16.0f, Col(color::kSuccess));
                PutText(draw, fonts.small, ImVec2(a.x + 30.0f, a.y + 13.0f - ts.y * 0.5f), Col(color::kText), ready);
            }
            for (auto it = shown.rbegin(); it != shown.rend(); ++it) {
                const Problem& p = **it;
                const std::string text = Ellipsize(fonts.small, p.text, size.x - 80.0f);
                const ImVec2 ts = Measure(fonts.small, text.c_str());
                const ImVec2 a(origin.x + space::k3, y - 26.0f), b(a.x + ts.x + 40.0f, y);
                draw->AddRectFilled(a, b, Col(color::kRaised), 13.0f);
                draw->AddRect(a, b, Col(color::kHairline), 13.0f);
                DrawIcon(draw, Icon::Warning, ImVec2(a.x + 16.0f, a.y + 13.0f), 16.0f, Col(SeverityColor(p.severity)));
                PutText(draw, fonts.small, ImVec2(a.x + 30.0f, a.y + 13.0f - ts.y * 0.5f), Col(color::kText),
                        text.c_str());
                y -= 32.0f;
            }
        }

        PutText(draw, fonts.caption, ImVec2(origin.x + space::k3, origin.y + space::k3), Col(color::kTextDim),
                "DRAG A PORT TO CONNECT  \xC2\xB7  DEL REMOVES");
    }

    // The right-click menu
    if (ImGui::BeginPopup("##simContext")) {
        if (const Device* d = lab.Find(view.contextDevice)) {
            if (d->kind == DeviceKind::Console || d->kind == DeviceKind::Tv) {
                if (ImGui::MenuItem(d->powered ? "Switch off" : "Switch on")) {
                    lab.Find(view.contextDevice)->powered = !d->powered;
                    save();
                }
            }
            if (ImGui::MenuItem("Delete")) {
                lab.Remove(view.contextDevice);
                view.selDevice = 0;
                save();
            }
        } else if (view.contextCable && ImGui::MenuItem("Delete cable")) {
            lab.RemoveCable(view.contextCable);
            view.selCable = 0;
            save();
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    // --- The palette ---
    BeginCard("##palette", ImVec2(canvasWidth, kPaletteHeight), false, ImVec2(space::k4, space::k3));
    {
        SectionLabel("Devices");
        ImGui::Dummy(ImVec2(0.0f, space::k1));
        static const DeviceKind kKinds[] = {DeviceKind::Console, DeviceKind::Tv, DeviceKind::Controller,
                                            DeviceKind::RumblePak, DeviceKind::Cartridge};
        for (size_t i = 0; i < 5; ++i) {
            const DeviceKind kind = kKinds[i];
            const DeviceDef& def = Def(kind);
            const bool full = lab.Count(kind) >= def.maxCount;
            if (i > 0) ImGui::SameLine(0.0f, space::k2);
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 itemSize(std::floor((canvasWidth - space::k4 * 2.0f - space::k2 * 4.0f) / 5.0f), 40.0f);
            ImGui::BeginDisabled(full);
            const bool clicked = NavButton("##device", itemSize);
            const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            const bool focused = ImGui::IsItemFocused() && io.NavVisible;
            if (!full && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload("SIM_DEVICE", &kind, sizeof(DeviceKind));
                ImGui::TextUnformatted(def.name);
                ImGui::EndDragDropSource();
            }
            ImGui::EndDisabled();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 max(min.x + itemSize.x, min.y + itemSize.y);
            draw->AddRectFilled(min, max, Col(hovered && !full ? color::kHover : color::kRaised), radius::kControl);
            draw->AddRect(min, max, Col(focused ? color::kAccentHover : color::kHairline), radius::kControl, 0,
                          focused ? 2.0f : 1.0f);
            DrawIcon(draw, DeviceIcon(kind), ImVec2(min.x + 20.0f, min.y + 20.0f), 20.0f,
                     Col(full ? color::kTextDim : color::kAccentText));
            const char* label = kind == DeviceKind::Console ? "Console" : def.name;
            PutText(
                draw, fonts.small, ImVec2(min.x + 38.0f, min.y + (itemSize.y - Measure(fonts.small, label).y) * 0.5f),
                Col(full ? color::kTextDim : color::kText), Ellipsize(fonts.small, label, itemSize.x - 44.0f).c_str());
            if (hovered && full) ImGui::SetTooltip("Already on the workbench.");
            if (clicked && !full) {
                // Put it in view, on the free spot nearest the bottom middle
                ImVec2 spot(SnapToGrid(-view.pan.x + 24.0f), SnapToGrid(-view.pan.y + 44.0f));
                float best = 1e9f;
                for (float y = 40.0f; y <= canvasHeight - def.size.y - 52.0f; y += 12.0f) {
                    for (float x = 12.0f; x <= canvasWidth - def.size.x - 12.0f; x += 12.0f) {
                        const ImVec2 world(x - view.pan.x, y - view.pan.y);
                        bool free = true;
                        for (const Device& other : lab.devices) {
                            const ImVec2 os = Def(other.kind).size;
                            if (world.x < other.pos.x + os.x + 16.0f && world.x + def.size.x + 16.0f > other.pos.x &&
                                world.y < other.pos.y + os.y + 16.0f && world.y + def.size.y + 16.0f > other.pos.y)
                                free = false;
                        }
                        const float dist = std::hypot(x + def.size.x * 0.5f - canvasWidth * 0.5f,
                                                      y + def.size.y * 0.5f - canvasHeight * 0.8f);
                        if (free && dist < best) {
                            best = dist;
                            spot = ImVec2(SnapToGrid(world.x), SnapToGrid(world.y));
                        }
                    }
                }
                if (const int id = lab.Add(kind, spot)) {
                    view.selDevice = id;
                    view.selCable = 0;
                    save();
                }
            }
            ImGui::PopID();
        }
    }
    EndCard();
    ImGui::EndGroup();

    // === The panel on the right ===
    ImGui::SameLine(0.0f, space::k4);
    BeginCard("##simPanel", ImVec2(kPanelWidth, bodyHeight), false, ImVec2(space::k4, space::k4));
    if (view.simulation) {
        SectionTitle("Power-on sequence");
        Muted("What the console does when it starts, one event at a time.");
        ImGui::Dummy(ImVec2(0.0f, space::k2));
        const bool atEnd = view.step + 1 >= static_cast<int>(view.steps.size());
        if (Button(view.autoplay ? "Pause" : "Play steps", ButtonStyle::Secondary,
                   view.autoplay ? Icon::Pause : Icon::Play, ImVec2(128.0f, 36.0f))) {
            view.autoplay = !view.autoplay;
            if (view.autoplay && atEnd) view.step = -1;
            view.stepStart = ImGui::GetTime();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(atEnd);
        if (Button("Step", ButtonStyle::Secondary, Icon::FastForward, ImVec2(88.0f, 36.0f))) {
            ++view.step;
            view.stepStart = ImGui::GetTime();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (Button("Reset", ButtonStyle::Ghost, Icon::None, ImVec2(80.0f, 36.0f))) {
            view.steps = buildSteps();
            view.step = -1;
            view.autoplay = false;
        }
        if (view.autoplay && ImGui::GetTime() - view.stepStart > 1.3) {
            if (!atEnd) {
                ++view.step;
                view.stepStart = ImGui::GetTime();
            } else {
                view.autoplay = false;
            }
        }
        ImGui::Dummy(ImVec2(0.0f, space::k3));
        ImGui::BeginChild("##events", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, space::k1));
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        for (int i = 0; i < static_cast<int>(view.steps.size()); ++i) {
            const Step& step = view.steps[static_cast<size_t>(i)];
            const bool reached = i <= view.step;
            const std::string text = ClampLines(fonts.small, step.text, rowWidth - 64.0f, 3);
            const ImVec2 ts = Measure(fonts.small, text.c_str());
            const float rowHeight = std::max(40.0f, ts.y + space::k3 * 2.0f);
            ImGui::PushID(i);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const bool clicked = NavButton("##event", ImVec2(rowWidth, rowHeight));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (clicked) {
                view.step = i;
                view.stepStart = ImGui::GetTime();
                view.autoplay = false;
            }
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 max(min.x + rowWidth, min.y + rowHeight);
            if (i == view.step) draw->AddRectFilled(min, max, Col(color::kAccentSoft), radius::kControl);
            else if (hovered) draw->AddRectFilled(min, max, Col(color::kHover), radius::kControl);
            char number[8];
            std::snprintf(number, sizeof(number), "%d", i + 1);
            PutText(draw, fonts.caption, ImVec2(min.x + space::k3, min.y + (rowHeight - 12.0f) * 0.5f),
                    Col(reached ? color::kTextMuted : color::kTextDim), number);
            PutText(draw, fonts.small, ImVec2(min.x + 36.0f, min.y + (rowHeight - ts.y) * 0.5f),
                    Col(reached ? color::kText : color::kTextDim), text.c_str());
            if (reached) {
                const ImVec4& tint = step.result == Severity::Error     ? color::kDanger
                                     : step.result == Severity::Warning ? color::kStar
                                                                        : color::kSuccess;
                DrawIcon(draw, step.result == Severity::Info ? Icon::Check : Icon::Warning,
                         ImVec2(max.x - 16.0f, min.y + rowHeight * 0.5f), 16.0f, Col(tint));
            }
        }
        if (view.steps.empty()) Muted("Press Reset to build the sequence.");
        ImGui::PopStyleVar();
        ImGui::EndChild();
    } else {
        // --- The inspector ---
        const Device* selected = lab.Find(view.selDevice);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::BeginCombo("##simDevice", selected ? Def(selected->kind).name : "Workbench")) {
            if (ImGui::Selectable("Workbench", !selected)) view.selDevice = 0;
            for (const Device& d : lab.devices) {
                std::string label = Def(d.kind).name;
                if (d.kind == DeviceKind::Controller) {
                    const int player = lab.PlayerOf(d.id);
                    label += player >= 0 ? " " + std::to_string(player + 1) : " (not plugged in)";
                }
                if (ImGui::Selectable(label.c_str(), d.id == view.selDevice)) view.selDevice = d.id;
            }
            ImGui::EndCombo();
        }
        ImGui::Dummy(ImVec2(0.0f, space::k2));
        ImGui::BeginChild("##inspectorBody", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
        RowsReset();

        // One option: the value Settings gives, with a button to give this game its own; or its own, with a way back
        auto optionRow = [&](const char* id) {
            const OptionDef* found = FindOption(id);
            if (!found) return;
            const OptionDef& option = *found;
            const bool custom = m_simSetup.Has(option.id);
            const int globalValue = option.get(global);
            int value = custom ? m_simSetup.Get(option.id, globalValue) : globalValue;
            const char* blocked = option.unavailable ? option.unavailable(effective) : nullptr;
            const char* warning = option.warning ? option.warning(effective) : nullptr;
            const char* text = blocked ? blocked : warning ? warning : option.help;
            bool changed = false;
            ImGui::PushID(option.id);
            ImGui::BeginDisabled(blocked != nullptr);
            ImGui::PushFont(fonts.label);
            ImGui::TextUnformatted(option.title);
            ImGui::PopFont();
            if (custom) {
                ImGui::SameLine(0.0f, space::k2);
                const ImVec2 ts = Measure(fonts.caption, "CUSTOM");
                const ImVec2 a = ImGui::GetCursorScreenPos();
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(ImVec2(a.x, a.y + 1.0f), ImVec2(a.x + ts.x + space::k3, a.y + ts.y + 7.0f),
                                    Col(color::kAccentSoft), 5.0f);
                PutText(draw, fonts.caption, ImVec2(a.x + space::k3 * 0.5f, a.y + 4.0f), Col(color::kAccentText),
                        "CUSTOM");
                ImGui::Dummy(ImVec2(ts.x + space::k3, ts.y));
            }
            HintText(text);
            ImGui::Dummy(ImVec2(0.0f, space::k1));
            if (!custom) {
                const std::string current = ValueText(option, globalValue);
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, color::kTextMuted);
                ImGui::TextUnformatted(current.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine(ImGui::GetContentRegionMax().x - 104.0f);
                if (Button("Customize", ButtonStyle::Secondary, Icon::None, ImVec2(104.0f, 30.0f))) {
                    m_simSetup.Set(option.id, globalValue);
                    changed = true;
                }
            } else {
                if (detail::DrawOptionControl(option, &value, globalValue)) {
                    m_simSetup.Set(option.id, value);
                    changed = true;
                }
                ImGui::SameLine(0.0f, space::k2);
                if (Button("Reset", ButtonStyle::Ghost, Icon::None, ImVec2(56.0f, 30.0f))) {
                    m_simSetup.Clear(option.id);
                    changed = true;
                }
            }
            ImGui::EndDisabled();
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0.0f, space::k4));
            if (changed) m_simSetup.Save(rom.path);
        };
        auto powerRow = [&](Device& d) {
            bool on = d.powered;
            ImGui::AlignTextToFramePadding();
            ImGui::PushFont(fonts.label);
            ImGui::TextUnformatted("Power");
            ImGui::PopFont();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 44.0f);
            if (Toggle("##power", &on)) {
                d.powered = on;
                save();
            }
            ImGui::Dummy(ImVec2(0.0f, space::k3));
        };

        if (!selected) {
            SectionTitle("Workbench");
            Muted("This is the hardware for %s. Click a device to see and change its options. Whatever you do not "
                  "change follows Settings.",
                  rom.name.c_str());
            ImGui::Dummy(ImVec2(0.0f, space::k3));
            if (problems.empty()) {
                Muted("Nothing to fix. This game is ready to play.");
            } else {
                SectionLabel("Check");
                ImGui::Dummy(ImVec2(0.0f, space::k1));
                for (const Problem& p : problems) {
                    const ImVec2 at = ImGui::GetCursorScreenPos();
                    DrawIcon(ImGui::GetWindowDrawList(), p.severity == Severity::Info ? Icon::Info : Icon::Warning,
                             ImVec2(at.x + 8.0f, at.y + 9.0f), 16.0f, Col(SeverityColor(p.severity)));
                    ImGui::SetCursorScreenPos(ImVec2(at.x + 26.0f, at.y));
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                    ImGui::TextUnformatted(p.text.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::Dummy(ImVec2(0.0f, space::k1));
                }
            }
            if (!m_simSetup.Empty()) {
                ImGui::Dummy(ImVec2(0.0f, space::k4));
                if (Button("Reset all changes", ButtonStyle::Ghost, Icon::None, ImVec2(0.0f, 36.0f))) {
                    m_simSetup.ClearAll();
                    save();
                }
            }
        } else {
            Device& device = *lab.Find(selected->id);
            SectionTitle(Def(device.kind).name);
            switch (device.kind) {
            case DeviceKind::Console:
                Muted("The CPU, the RSP and how fast the game runs. It always has the Expansion Pak, 8 MB in all.");
                ImGui::Dummy(ImVec2(0.0f, space::k3));
                powerRow(device);
                for (const char* id : {"speed", "cpuCore", "rspEngine", "rewind"}) optionRow(id);
                break;
            case DeviceKind::Tv:
                Muted("The picture: driver, resolution and accuracy.");
                ImGui::Dummy(ImVec2(0.0f, space::k3));
                powerRow(device);
                for (const char* id :
                     {"displayMode", "backendType", "internalResScale", "widescreen", "enableMipmaps",
                      "hiResTexEnabled", "rayTracing", "renderingMode", "fbReadAccess", "fbWriteAccess"})
                    optionRow(id);
                break;
            case DeviceKind::Cartridge: {
                Muted("The game itself: what its header says, and how it saves.");
                ImGui::Dummy(ImVec2(0.0f, space::k3));
                char line[96];
                InfoRow("Title", m_simRom.valid ? m_simRom.title.c_str() : "-");
                InfoRow("Game code",
                        m_simRom.valid ? (m_simRom.gameId + std::string(1, m_simRom.regionCode)).c_str() : "-");
                InfoRow("Region", regionText.c_str());
                InfoRow("Format", m_simRom.valid ? m_simRom.format : "-");
                std::snprintf(line, sizeof(line), "%.1f MB", static_cast<double>(m_simRom.size) / (1024.0 * 1024.0));
                InfoRow("Size", line);
                std::snprintf(line, sizeof(line), "%08X %08X", m_simRom.crc1, m_simRom.crc2);
                InfoRow("Checksums", m_simRom.valid ? line : "-");
                ImGui::Dummy(ImVec2(0.0f, space::k4));
                optionRow("saveChip");
                break;
            }
            case DeviceKind::Controller: {
                const int player = lab.PlayerOf(device.id);
                Muted("Plug its cable into a console port to make it a player. Which controller plays is chosen in "
                      "Settings.");
                ImGui::Dummy(ImVec2(0.0f, space::k3));
                InfoRow("Player", player >= 0 ? std::to_string(player + 1).c_str() : "not plugged in");
                InfoRow("Device", player >= 0 ? (NovaInput::Gamepads::Instance().Connected(player) ? "Game controller"
                                                 : player == 0                                     ? "Keyboard"
                                                                                                   : "None attached")
                                              : "-");
                InfoRow("Pak slot", lab.PakOf(device.id) ? "Rumble Pak" : "Empty");
                ImGui::Dummy(ImVec2(0.0f, space::k4));
                if (Button("Controller settings", ButtonStyle::Secondary, Icon::Settings, ImVec2(0.0f, 36.0f))) {
                    m_settingsPage = 3;
                    m_activeTab = Tab::Settings;
                }
                break;
            }
            case DeviceKind::RumblePak:
                Muted("A Rumble Pak in a controller's slot turns that player's rumble on for this game, even when "
                      "Settings says automatic. The motor needs a controller that can rumble.");
                ImGui::Dummy(ImVec2(0.0f, space::k3));
                InfoRow("In", lab.CableAt(device.id, 0) ? "a controller" : "nothing");
                break;
            }
        }
        ImGui::EndChild();
    }
    EndCard();
}

} // namespace NovaUI
