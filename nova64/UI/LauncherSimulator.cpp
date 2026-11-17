/**
 * @file LauncherSimulator.cpp
 * @brief Simulator page: a workbench for one game's virtual hardware, in the manner of a network simulator.
 *
 * Devices (console, TV, controllers, a Rumble Pak, the game's cartridge) are dragged from the palette onto the
 * workbench and joined with cables. A link light on each cable end says whether it works. Clicking a device shows
 * its options in the inspector; every option is one a game can override (see GameSetup.hpp), and a Rumble Pak in a
 * controller sets that player's pak. "Play" starts the game with exactly that setup, and Simulation mode steps
 * through the power-on sequence first, event by event, so a setup that will not work says why.
 *
 * The page is built from a few functions that each draw one part and share a `Bench`, everything one frame of the
 * page needs to know; NovaLauncher::RenderSimulatorTab only lays the parts out.
 */
#include "GameInfo.hpp"
#include "LauncherInternal.hpp"
#include "NovaLauncher.hpp"
#include "UiKit.hpp"

#include <imgui.h>

#include <algorithm>
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
using kit::Col;

constexpr float kPanelWidth = 372.0f;
constexpr float kPaletteHeight = 96.0f;
constexpr float kToolbarHeight = 44.0f;
constexpr float kGrid = 12.0f;
constexpr float kPortRadius = 6.0f;
constexpr float kPortHitRadius = 11.0f;
constexpr float kCableHitDistance = 8.0f;
constexpr float kDragThreshold = 5.0f;
constexpr double kEnvelopeSeconds = 0.9;
constexpr double kAutoplaySeconds = 1.3;

// --- Small helpers ---

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
float Distance(ImVec2 a, ImVec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }
float SnapToGrid(float v) { return std::round(v / kGrid) * kGrid; }

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

ImVec2 Bezier(const ImVec2 (&p)[4], float t) {
    const float u = 1.0f - t;
    const float a = u * u * u, b = 3.0f * u * u * t, c = 3.0f * u * t * t, d = t * t * t;
    return ImVec2(a * p[0].x + b * p[1].x + c * p[2].x + d * p[3].x, a * p[0].y + b * p[1].y + c * p[2].y + d * p[3].y);
}

// A cable between two ports, as the four points of a cubic curve in screen space.
struct CablePath {
    ImVec2 p[4];
};

CablePath MakePath(ImVec2 a, Facing fa, ImVec2 b, Facing fb) {
    const float k = std::clamp(Distance(a, b) * 0.5f, 36.0f, 140.0f);
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
        best = std::min(best, Distance(ImVec2(prev.x + ab.x * t, prev.y + ab.y * t), mouse));
        prev = next;
    }
    return best;
}

std::string ChoiceLabel(const char* id, const SettingsModel& model) {
    const OptionDef* option = FindOption(id);
    return option ? ValueText(*option, option->get(model)) : std::string();
}

// --- What one frame of the page knows ---

// Not copyable: `effective` points at the members before it.
struct Bench {
    Lab& lab;
    View& view;
    GameSetup& setup;
    const RomMetadata& game;
    const detail::RomInfo& rom;
    const GameInfo* info;
    bool romExists;
    bool isDisk;
    bool cartridgeOk;

    // The options as Settings has them, and as this game will use them
    int noChip = 0;
    int globalPaks[4] = {0, 0, 0, 0};
    SettingsModel global;
    LaunchOptions effLaunch;
    Nova64::Video::PluginConfig effGfx;
    int effChip = 0;
    int effPaks[4] = {0, 0, 0, 0};
    SettingsModel effective;

    // Words the devices and the sequence use
    std::string regionText, chipText, resolutionText;

    std::vector<Problem> problems;
    int errors = 0;

    Bench(Lab& lab_, View& view_, GameSetup& setup_, const RomMetadata& game_, const detail::RomInfo& rom_,
          LaunchOptions& launch, Nova64::Video::PluginConfig& gfx, bool isDisk_, bool romExists_) :
        lab(lab_), view(view_), setup(setup_), game(game_), rom(rom_), info(FindGameInfo(game_.name)),
        romExists(romExists_), isDisk(isDisk_), global{&launch, &gfx, &noChip, globalPaks}, effLaunch(launch),
        effGfx(gfx), effective{&effLaunch, &effGfx, &effChip, effPaks} {
        cartridgeOk = romExists && (rom.valid || isDisk);
        setup.Apply(effective);
        regionText = rom.valid ? std::string(rom.region) + (rom.pal ? ", PAL 50 Hz" : ", NTSC 60 Hz")
                               : (isDisk ? "64DD disk" : "Not an N64 ROM");
        chipText = effChip == 1 ? "SRAM" : effChip == 2 ? "FlashRAM" : "detected from the game";
        resolutionText = ChoiceLabel("internalResScale", effective);
        problems = CollectProblems();
        for (const Problem& p : problems)
            if (p.severity == Severity::Error) ++errors;
    }
    Bench(const Bench&) = delete;
    Bench& operator=(const Bench&) = delete;

    /// Writes the wiring and the options to disk, with the pak each player has.
    void Save() {
        bool ok = lab.Save(game.path);
        for (int p = 0; p < 4; ++p) {
            const std::string id = "accessory" + std::to_string(p + 1);
            if (lab.PlayerHasRumblePak(p)) setup.Set(id, 2);
            else setup.Clear(id);
        }
        ok = setup.Save(game.path) && ok;
        view.saveFailed = !ok;
    }

    /// The wiring is the truth about the paks: they match when each player's pak in the setup is the Rumble Pak
    /// that is plugged in, and nothing else.
    bool PaksMatchWiring() const {
        for (int p = 0; p < 4; ++p) {
            const std::string id = "accessory" + std::to_string(p + 1);
            const bool plugged = lab.PlayerHasRumblePak(p);
            if (setup.Has(id) != plugged || (plugged && setup.Get(id, 0) != 2)) return false;
        }
        return true;
    }

    /// Whether the game overrides any option besides the paks, which come from the wiring.
    bool HasOptionOverrides() const {
        for (const OptionDef& option : AllOptions())
            if (setup.Has(option.id) && std::strncmp(option.id, "accessory", 9) != 0) return true;
        return false;
    }

    std::vector<Problem> CollectProblems() const {
        std::vector<Problem> out = lab.Check(cartridgeOk);
        const int tv = lab.First(DeviceKind::Tv), console = lab.First(DeviceKind::Console),
                  cartridge = lab.First(DeviceKind::Cartridge);
        if (const OptionDef* backend = FindOption("backendType"))
            if (const char* why = backend->choiceUnavailable ? backend->choiceUnavailable(effGfx.backendType) : nullptr)
                out.push_back(
                    {Severity::Error,
                     std::string(effGfx.backendType == 1 ? "Vulkan" : "OpenGL") + " can't be used here: " + why, tv});
        if (effGfx.renderingMode == Nova64::Video::RenderingMode::LLE && effLaunch.rspEngine == 1)
            out.push_back({Severity::Warning, "Low-level graphics on the RSP interpreter will be very slow.", console});
        if (effGfx.rayTracing && effGfx.renderingMode != Nova64::Video::RenderingMode::HLE)
            out.push_back({Severity::Warning, "Ray-traced lighting only works with high-level graphics.", tv});
        if (effGfx.rayTracing && effGfx.backendType != 1)
            out.push_back({Severity::Warning, "Ray-traced lighting needs the Vulkan driver.", tv});
        if (rom.valid && rom.pal)
            out.push_back(
                {Severity::Info, "A PAL game: it runs at 50 Hz, so full speed is 50 frames a second.", cartridge});
        if (info && info->needsExpansionPak)
            out.push_back({Severity::Info,
                           std::string(info->title) + " needs the Expansion Pak. The console always has it (8 MB).",
                           console});
        if (effChip != 0)
            out.push_back({Severity::Info,
                           std::string("The save chip is forced to ") + (effChip == 1 ? "SRAM" : "FlashRAM") + ".",
                           cartridge});
        return out;
    }
};

/// The events of the power-on sequence, in order, stopping at the first one that fails.
std::vector<Step> BuildSteps(const Bench& b) {
    std::vector<Step> steps;
    const Lab& lab = b.lab;
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
    if (!b.cartridgeOk) {
        add(cartridge, console, cartCable, "The game's file is missing, or it is not an N64 ROM.", Severity::Error);
        return steps;
    }
    add(cartridge, console, cartCable,
        "Cartridge read: " + (b.rom.valid ? b.rom.title + ", " + b.rom.format : b.game.name) + ".", Severity::Info);
    add(console, 0, nullptr,
        "Region " + b.regionText + ": the video clock and frame pacing follow it. Save chip: " + b.chipText + ".",
        Severity::Info);

    for (int p = 0; p < 4; ++p) {
        const int controller = lab.ControllerOnPort(p);
        if (!controller) {
            if (p == 0)
                add(console, 0, nullptr, "No controller in port 1. Most games wait for player 1.", Severity::Warning);
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
        "Video signal: " + b.resolutionText + ", " +
            (b.effGfx.aspectRatio == Nova64::Video::AspectRatio::Widescreen16_9 ? "16:9" : "4:3") + ", drawn by " +
            ChoiceLabel("backendType", b.effective) +
            (b.effLaunch.displayMode == 1 ? " fullscreen" : " in the launcher window") + ".",
        Severity::Info);
    add(tv, 0, nullptr, "Picture on screen. Ready: press Play.", Severity::Info);
    return steps;
}

// --- The workbench canvas ---

/// Where the canvas is on the screen and how the workbench is placed in it.
struct Canvas {
    ImVec2 origin;
    ImVec2 size;
    ImVec2 pan;

    ImVec2 ToScreen(ImVec2 w) const { return ImVec2(origin.x + pan.x + w.x, origin.y + pan.y + w.y); }
    ImVec2 ToWorld(ImVec2 s) const { return ImVec2(s.x - origin.x - pan.x, s.y - origin.y - pan.y); }
};

ImVec2 PowerCenter(const Device& d) { return ImVec2(d.pos.x + Def(d.kind).size.x - 20.0f, d.pos.y + 20.0f); }
bool HasPowerSwitch(const Device& d) { return d.kind == DeviceKind::Console || d.kind == DeviceKind::Tv; }

CablePath PathOf(const Lab& lab, const Canvas& canvas, const Cable& c) {
    const Device& a = *lab.Find(c.a);
    const Device& b = *lab.Find(c.b);
    return MakePath(canvas.ToScreen(lab.PortPos(a, c.portA)), Def(a.kind).ports[static_cast<size_t>(c.portA)].facing,
                    canvas.ToScreen(lab.PortPos(b, c.portB)), Def(b.kind).ports[static_cast<size_t>(c.portB)].facing);
}

/// What is under the pointer: a port, a power switch, a device, or a cable.
struct Hit {
    int device = 0;
    int port = -1;
    int cable = 0;
    bool power = false;
};

Hit HitTest(const Lab& lab, const Canvas& canvas, ImVec2 mouse) {
    Hit hit;
    const ImVec2 world = canvas.ToWorld(mouse);
    for (auto it = lab.devices.rbegin(); it != lab.devices.rend(); ++it) {
        const DeviceDef& def = Def(it->kind);
        for (int port = 0; port < static_cast<int>(def.ports.size()); ++port) {
            if (Distance(world, lab.PortPos(*it, port)) <= kPortHitRadius) {
                hit.device = it->id;
                hit.port = port;
                return hit;
            }
        }
    }
    for (auto it = lab.devices.rbegin(); it != lab.devices.rend(); ++it) {
        const DeviceDef& def = Def(it->kind);
        if (HasPowerSwitch(*it) && Distance(world, PowerCenter(*it)) <= 12.0f) {
            hit.device = it->id;
            hit.power = true;
            return hit;
        }
        if (world.x >= it->pos.x && world.x <= it->pos.x + def.size.x && world.y >= it->pos.y &&
            world.y <= it->pos.y + def.size.y) {
            hit.device = it->id;
            return hit;
        }
    }
    float best = kCableHitDistance;
    for (const Cable& c : lab.cables) {
        const float d = DistanceToPath(PathOf(lab, canvas, c), mouse);
        if (d < best) {
            best = d;
            hit.cable = c.id;
        }
    }
    return hit;
}

/// Presses, drags and releases on the canvas: moving a device, pulling a cable, panning, switching a device on or off.
void HandlePointer(Bench& b, Canvas& canvas, const Hit& hit, bool activated, bool active, bool deactivated) {
    Lab& lab = b.lab;
    View& view = b.view;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const ImVec2 world = canvas.ToWorld(mouse);

    if (activated && io.MouseDown[0]) {
        view.pressAt = mouse;
        view.moved = false;
        view.pickedCable = 0;
        if (hit.power) {
            if (Device* d = lab.Find(hit.device)) {
                d->powered = !d->powered;
                view.selDevice = d->id;
                view.selCable = 0;
                b.Save();
            }
            view.drag = View::Drag::None;
        } else if (hit.port >= 0) {
            if (const Cable* existing = lab.CableAt(hit.device, hit.port)) {
                view.drag = View::Drag::PickUp; // lifts the cable off once the pointer has moved
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

    if (active && io.MouseDown[0]) {
        if (Distance(mouse, view.pressAt) > kDragThreshold) view.moved = true;
        switch (view.drag) {
        case View::Drag::Pan: canvas.pan = view.pan = Add(view.pan, io.MouseDelta); break;
        case View::Drag::Device:
            if (view.moved)
                if (Device* d = lab.Find(view.dragDevice))
                    d->pos = ImVec2(world.x - view.dragGrab.x, world.y - view.dragGrab.y);
            break;
        case View::Drag::PickUp:
            if (view.moved) {
                if (const Cable* c = lab.CableAt(view.dragFromDevice, view.dragFromPort)) {
                    const bool fromIsA = (c->a == view.dragFromDevice);
                    const int otherDevice = fromIsA ? c->b : c->a;
                    const int otherPort = fromIsA ? c->portB : c->portA;
                    lab.RemoveCable(c->id);
                    view.dragFromDevice = otherDevice; // carry the other end
                    view.dragFromPort = otherPort;
                    view.drag = View::Drag::Cable;
                }
            }
            break;
        default: break;
        }
    }

    if (deactivated) {
        if (view.drag == View::Drag::Cable) {
            const Hit drop = HitTest(lab, canvas, mouse);
            if (drop.port >= 0 && lab.Connect(view.dragFromDevice, view.dragFromPort, drop.device, drop.port)) b.Save();
            else if (view.pickedCable) b.Save(); // a cable was lifted off and dropped nowhere: it is gone
        } else if (view.drag == View::Drag::Device && view.moved) {
            if (Device* d = lab.Find(view.dragDevice)) d->pos = ImVec2(SnapToGrid(d->pos.x), SnapToGrid(d->pos.y));
            b.Save();
        } else if (view.drag == View::Drag::PickUp && !view.moved) {
            view.selCable = view.pickedCable;
            view.selDevice = 0; // Delete must remove the cable, not the device it was plugged into
        }
        view.drag = View::Drag::None;
        view.pickedCable = 0;
    }
}

void DrawGrid(ImDrawList* draw, const Canvas& canvas) {
    const float step = kGrid * 2.0f;
    const float startX = std::fmod(canvas.pan.x, step) - step, startY = std::fmod(canvas.pan.y, step) - step;
    for (float y = startY; y < canvas.size.y + step; y += step)
        for (float x = startX; x < canvas.size.x + step; x += step)
            draw->AddRectFilled(ImVec2(canvas.origin.x + x, canvas.origin.y + y),
                                ImVec2(canvas.origin.x + x + 2.0f, canvas.origin.y + y + 2.0f),
                                Col(kit::Rgb(0xFFEADB, 0.07f)));
}

void DrawCables(ImDrawList* draw, const Bench& b, const Canvas& canvas, const Step* current) {
    for (const Cable& c : b.lab.cables) {
        const CablePath path = PathOf(b.lab, canvas, c);
        const PortType type = Def(b.lab.Find(c.a)->kind).ports[static_cast<size_t>(c.portA)].type;
        const bool emphasised = (b.view.selCable == c.id) || (current && current->cable == c.id);
        draw->AddBezierCubic(path.p[0], path.p[1], path.p[2], path.p[3], Col(kit::color::kSidebar), 6.0f);
        draw->AddBezierCubic(path.p[0], path.p[1], path.p[2], path.p[3],
                             Col(emphasised ? kit::color::kAccentHover : CableColor(type)), emphasised ? 3.5f : 2.5f);
    }
}

/// The lines of facts a device shows under its name.
void DeviceFacts(const Bench& b, const Device& d, std::string (&lines)[3]) {
    const Lab& lab = b.lab;
    switch (d.kind) {
    case DeviceKind::Console:
        lines[0] = "CPU: " + ChoiceLabel("cpuCore", b.effective);
        lines[1] = "RSP: " + ChoiceLabel("rspEngine", b.effective);
        lines[2] = "8 MB  \xC2\xB7  " + ChoiceLabel("speed", b.effective);
        break;
    case DeviceKind::Tv:
        lines[0] = std::string(b.effGfx.backendType == 1 ? "Vulkan" : "OpenGL") + "  \xC2\xB7  " + b.resolutionText;
        lines[1] = b.effGfx.aspectRatio == Nova64::Video::AspectRatio::Widescreen16_9 ? "16:9" : "4:3";
        lines[2] =
            b.effGfx.renderingMode == Nova64::Video::RenderingMode::LLE ? "low-level graphics" : "high-level graphics";
        break;
    case DeviceKind::Controller: {
        const int player = lab.PlayerOf(d.id);
        lines[0] = player >= 0 ? "Player " + std::to_string(player + 1) : "Not plugged in";
        if (player >= 0)
            lines[1] = NovaInput::Gamepads::Instance().Connected(player) ? "Game controller"
                       : player == 0                                     ? "Keyboard"
                                                                         : "No device";
        if (lab.PakOf(d.id)) lines[2] = "Rumble Pak";
        break;
    }
    case DeviceKind::RumblePak:
        lines[0] = "Force feedback";
        if (const Cable* c = lab.CableAt(d.id, 0)) {
            const int player = lab.PlayerOf(c->a == d.id ? c->b : c->a);
            if (player >= 0) lines[1] = "Player " + std::to_string(player + 1);
        }
        break;
    case DeviceKind::Cartridge:
        lines[0] = b.game.name;
        lines[1] = b.rom.valid ? std::string(b.rom.region) + (b.rom.pal ? "  \xC2\xB7  50 Hz" : "  \xC2\xB7  60 Hz")
                               : (b.isDisk ? "64DD disk" : "Not a ROM");
        lines[2] = "Saves: " + b.chipText;
        break;
    }
}

void DrawDevices(ImDrawList* draw, const Bench& b, const Canvas& canvas, const Hit& hit, bool surfaceHovered,
                 const Step* current) {
    const kit::Fonts& fonts = kit::GetFonts();
    for (const Device& d : b.lab.devices) {
        const DeviceDef& def = Def(d.kind);
        const ImVec2 min = canvas.ToScreen(d.pos);
        const ImVec2 max(min.x + def.size.x, min.y + def.size.y);
        const bool selected = (b.view.selDevice == d.id);
        const bool lit = current && (current->from == d.id || current->to == d.id);
        const bool hovered = hit.device == d.id && !hit.power && surfaceHovered && b.view.drag == View::Drag::None;
        draw->AddRectFilled(min, max, Col(hovered ? kit::color::kHover : kit::color::kRaised), kit::radius::kCard);
        draw->AddRect(min, max, Col(selected || lit ? kit::color::kAccentHover : kit::color::kHairline),
                      kit::radius::kCard, 0, selected || lit ? 2.0f : 1.0f);
        kit::DrawIcon(draw, DeviceIcon(d.kind), ImVec2(min.x + 22.0f, min.y + 22.0f), 20.0f,
                      Col(selected || lit ? kit::color::kAccentText : kit::color::kTextMuted));
        kit::PutText(draw, fonts.label, ImVec2(min.x + 40.0f, min.y + 12.0f), Col(kit::color::kText), def.name);

        std::string lines[3];
        DeviceFacts(b, d, lines);
        float y = min.y + 40.0f;
        for (const std::string& line : lines) {
            if (line.empty()) continue;
            kit::PutText(draw, fonts.small, ImVec2(min.x + 12.0f, y), Col(kit::color::kTextMuted),
                         kit::Ellipsize(fonts.small, line, def.size.x - 24.0f).c_str());
            y += 17.0f;
        }

        if (HasPowerSwitch(d)) {
            const ImVec2 pc = canvas.ToScreen(PowerCenter(d));
            const bool powerHover = hit.power && hit.device == d.id && surfaceHovered;
            draw->AddCircleFilled(pc, 11.0f, Col(powerHover ? kit::color::kHover : kit::color::kCard));
            draw->AddCircle(pc, 11.0f, Col(d.powered ? kit::color::kSuccess : kit::color::kHairline), 0, 1.5f);
            kit::DrawIcon(draw, kit::Icon::Lightning, pc, 14.0f,
                          Col(d.powered ? kit::color::kSuccess : kit::color::kTextDim));
        }
    }
}

void DrawPorts(ImDrawList* draw, const Bench& b, const Canvas& canvas, const Hit& hit, bool surfaceHovered) {
    const Lab& lab = b.lab;
    const bool carrying = b.view.drag == View::Drag::Cable;
    for (const Device& d : lab.devices) {
        const DeviceDef& def = Def(d.kind);
        for (int port = 0; port < static_cast<int>(def.ports.size()); ++port) {
            const PortDef& pd = def.ports[static_cast<size_t>(port)];
            const ImVec2 at = canvas.ToScreen(lab.PortPos(d, port));
            const Cable* cable = lab.CableAt(d.id, port);
            const bool hoverPort = hit.device == d.id && hit.port == port && surfaceHovered;
            const bool target = carrying && lab.CanConnect(b.view.dragFromDevice, b.view.dragFromPort, d.id, port);
            draw->AddCircleFilled(at, kPortRadius, Col(cable ? CableColor(pd.type) : kit::color::kCard));
            draw->AddCircle(at, kPortRadius,
                            Col(target      ? kit::color::kSuccess
                                : hoverPort ? kit::color::kAccentHover
                                            : kit::color::kHairline),
                            0, target || hoverPort ? 2.0f : 1.5f);
            if (target) draw->AddCircle(at, kPortRadius + 4.0f, Col(kit::color::kSuccess), 0, 1.5f);
            if (cable)
                draw->AddCircleFilled(Add(at, Scale(Normal(pd.facing), 14.0f)), 4.5f,
                                      Col(LinkColor(lab.State(*cable, b.cartridgeOk))));
        }
    }
    if (surfaceHovered && hit.port >= 0 && b.view.drag == View::Drag::None) {
        // The device may have been deleted earlier in this frame
        if (const Device* d = lab.Find(hit.device))
            ImGui::SetTooltip("%s: %s", Def(d->kind).name, Def(d->kind).ports[static_cast<size_t>(hit.port)].name);
    }
}

/// The cable being pulled from a port to the pointer.
void DrawCarriedCable(ImDrawList* draw, const Bench& b, const Canvas& canvas) {
    if (b.view.drag != View::Drag::Cable) return;
    const Device* from = b.lab.Find(b.view.dragFromDevice);
    if (!from) return;
    const PortDef& pd = Def(from->kind).ports[static_cast<size_t>(b.view.dragFromPort)];
    const ImVec2 start = canvas.ToScreen(b.lab.PortPos(*from, b.view.dragFromPort));
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImVec2 toMouse(mouse.x - start.x, mouse.y - start.y);
    const float k = std::clamp(Distance(start, mouse) * 0.5f, 24.0f, 120.0f);
    draw->AddBezierCubic(start, Add(start, Scale(Normal(pd.facing), k)),
                         ImVec2(mouse.x - toMouse.x * 0.25f, mouse.y - toMouse.y * 0.25f), mouse,
                         Col(CableColor(pd.type)), 2.5f);
}

/// The envelope that travels along the cable the current event uses.
void DrawEnvelope(ImDrawList* draw, const Bench& b, const Canvas& canvas, const Step* current) {
    if (!current || !current->cable) return;
    for (const Cable& c : b.lab.cables) {
        if (c.id != current->cable) continue;
        const CablePath path = PathOf(b.lab, canvas, c);
        float t = std::clamp(static_cast<float>((ImGui::GetTime() - b.view.stepStart) / kEnvelopeSeconds), 0.0f, 1.0f);
        if (c.a != current->from) t = 1.0f - t;
        const ImVec2 at = Bezier(path.p, t);
        draw->AddRectFilled(ImVec2(at.x - 9.0f, at.y - 6.5f), ImVec2(at.x + 9.0f, at.y + 6.5f),
                            Col(kit::color::kAccentHover), 3.0f);
        draw->AddLine(ImVec2(at.x - 8.0f, at.y - 5.0f), ImVec2(at.x, at.y + 1.0f), Col(kit::color::kOnAccent), 1.5f);
        draw->AddLine(ImVec2(at.x, at.y + 1.0f), ImVec2(at.x + 8.0f, at.y - 5.0f), Col(kit::color::kOnAccent), 1.5f);
    }
}

/// What is wrong, in the bottom left corner: errors and warnings first, or a line saying all is well.
void DrawStatus(ImDrawList* draw, const Bench& b, const Canvas& canvas) {
    const kit::Fonts& fonts = kit::GetFonts();
    std::vector<const Problem*> shown;
    for (const Problem& p : b.problems)
        if (p.severity != Severity::Info && shown.size() < 3) shown.push_back(&p);
    float y = canvas.origin.y + canvas.size.y - kit::space::k3;

    auto pill = [&](const std::string& text, kit::Icon icon, const ImVec4& tint) {
        const ImVec2 ts = kit::Measure(fonts.small, text.c_str());
        const ImVec2 a(canvas.origin.x + kit::space::k3, y - 26.0f), end(a.x + ts.x + 40.0f, y);
        draw->AddRectFilled(a, end, Col(kit::color::kRaised), 13.0f);
        draw->AddRect(a, end, Col(kit::color::kHairline), 13.0f);
        kit::DrawIcon(draw, icon, ImVec2(a.x + 16.0f, a.y + 13.0f), 16.0f, Col(tint));
        kit::PutText(draw, fonts.small, ImVec2(a.x + 30.0f, a.y + 13.0f - ts.y * 0.5f), Col(kit::color::kText),
                     text.c_str());
        y -= 32.0f;
    };
    if (b.view.saveFailed)
        pill("Couldn't save this setup. Is the data folder writable?", kit::Icon::Warning, kit::color::kDanger);
    if (shown.empty()) pill("Ready to play", kit::Icon::Check, kit::color::kSuccess);
    for (auto it = shown.rbegin(); it != shown.rend(); ++it)
        pill(kit::Ellipsize(fonts.small, (*it)->text, canvas.size.x - 80.0f), kit::Icon::Warning,
             SeverityColor((*it)->severity));
}

/// The right-click menu of a device or a cable.
void DrawContextMenu(Bench& b) {
    Lab& lab = b.lab;
    View& view = b.view;
    if (!ImGui::BeginPopup("##simContext")) return;
    if (const Device* d = lab.Find(view.contextDevice)) {
        if (HasPowerSwitch(*d) && ImGui::MenuItem(d->powered ? "Switch off" : "Switch on")) {
            lab.Find(view.contextDevice)->powered = !d->powered;
            b.Save();
        }
        if (ImGui::MenuItem("Delete")) {
            lab.Remove(view.contextDevice);
            view.selDevice = 0;
            b.Save();
        }
    } else if (view.contextCable && ImGui::MenuItem("Delete cable")) {
        lab.RemoveCable(view.contextCable);
        view.selCable = 0;
        b.Save();
    }
    ImGui::EndPopup();
}

/// "Fit" puts the devices in the middle of the view, clear of the legend above and the status below.
void FitView(View& view, const Lab& lab, ImVec2 size) {
    if (!view.fitPending || lab.devices.empty()) return;
    ImVec2 lo(1e9f, 1e9f), hi(-1e9f, -1e9f);
    for (const Device& d : lab.devices) {
        lo = ImVec2(std::min(lo.x, d.pos.x), std::min(lo.y, d.pos.y));
        hi = ImVec2(std::max(hi.x, d.pos.x + Def(d.kind).size.x), std::max(hi.y, d.pos.y + Def(d.kind).size.y));
    }
    const float usable = size.y - 34.0f - 50.0f;
    // Centred when it fits, and from the left edge when it does not (the rest can be panned into view)
    const float left = std::max(12.0f, (size.x - (hi.x - lo.x)) * 0.5f);
    view.pan =
        ImVec2(std::round(left - lo.x), std::round(34.0f + std::max(0.0f, usable - (hi.y - lo.y)) * 0.5f - lo.y));
    view.fitPending = false;
}

/// Draws the workbench and handles the pointer on it. Call between BeginChild and EndChild of its window.
void DrawCanvas(Bench& b) {
    Lab& lab = b.lab;
    View& view = b.view;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();

    Canvas canvas{ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail(), view.pan};
    FitView(view, lab, canvas.size);
    canvas.pan = view.pan;
    DrawGrid(draw, canvas);

    // The pointer
    kit::NavButton("##surface", canvas.size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool activated = ImGui::IsItemActivated();
    const bool deactivated = ImGui::IsItemDeactivated();
    const bool focused = ImGui::IsItemFocused();
    const Hit hit = (hovered || active) ? HitTest(lab, canvas, io.MousePos) : Hit{};

    // Dropping a device from the palette
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SIM_DEVICE")) {
            const DeviceKind kind = *static_cast<const DeviceKind*>(payload->Data);
            const ImVec2 world = canvas.ToWorld(io.MousePos);
            const ImVec2 size = Def(kind).size;
            if (const int id =
                    lab.Add(kind, ImVec2(SnapToGrid(world.x - size.x * 0.5f), SnapToGrid(world.y - size.y * 0.5f)))) {
                view.selDevice = id;
                view.selCable = 0;
                b.Save();
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (!active && !deactivated && view.drag != View::Drag::None) { // the page was left, or the release was missed
        view.drag = View::Drag::None;
        view.pickedCable = 0;
    }
    HandlePointer(b, canvas, hit, activated, active, deactivated);
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))
        canvas.pan = view.pan = Add(view.pan, io.MouseDelta);

    // Right click opens a menu for what is under the pointer
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && (hit.device || hit.cable)) {
        view.contextDevice = hit.device;
        view.contextCable = hit.device ? 0 : hit.cable;
        if (hit.device) view.selDevice = hit.device;
        ImGui::OpenPopup("##simContext");
    }
    // Delete works with the pointer over the canvas or with it focused; Backspace only with the pointer over it
    if (((ImGui::IsKeyPressed(ImGuiKey_Delete) && (hovered || focused)) ||
         (ImGui::IsKeyPressed(ImGuiKey_Backspace) && hovered)) &&
        (view.selDevice || view.selCable)) {
        if (view.selDevice) lab.Remove(view.selDevice);
        else lab.RemoveCable(view.selCable);
        view.selDevice = view.selCable = 0;
        b.Save();
    }

    // The current event of the power-on sequence lights up what it touches
    const bool inSequence = view.simulation && view.step >= 0 && view.step < static_cast<int>(view.steps.size());
    const Step* current = inSequence ? &view.steps[static_cast<size_t>(view.step)] : nullptr;

    canvas.pan = view.pan;
    DrawCables(draw, b, canvas, current);
    DrawDevices(draw, b, canvas, hit, hovered, current);
    DrawPorts(draw, b, canvas, hit, hovered);
    DrawCarriedCable(draw, b, canvas);
    DrawEnvelope(draw, b, canvas, current);
    DrawStatus(draw, b, canvas);
    kit::PutText(draw, kit::GetFonts().caption,
                 ImVec2(canvas.origin.x + kit::space::k3, canvas.origin.y + kit::space::k3), Col(kit::color::kTextDim),
                 "DRAG A PORT TO CONNECT  \xC2\xB7  DEL REMOVES");
    DrawContextMenu(b);
}

// --- The palette ---

/// A device that fits on the free spot nearest the bottom middle of the view.
ImVec2 FreeSpot(const Lab& lab, const View& view, DeviceKind kind, ImVec2 canvasSize) {
    const ImVec2 size = Def(kind).size;
    ImVec2 spot(SnapToGrid(-view.pan.x + 24.0f), SnapToGrid(-view.pan.y + 44.0f));
    float best = 1e9f;
    for (float y = 40.0f; y <= canvasSize.y - size.y - 52.0f; y += kGrid) {
        for (float x = 12.0f; x <= canvasSize.x - size.x - 12.0f; x += kGrid) {
            const ImVec2 world(x - view.pan.x, y - view.pan.y);
            bool free = true;
            for (const Device& other : lab.devices) {
                const ImVec2 os = Def(other.kind).size;
                if (world.x < other.pos.x + os.x + 16.0f && world.x + size.x + 16.0f > other.pos.x &&
                    world.y < other.pos.y + os.y + 16.0f && world.y + size.y + 16.0f > other.pos.y)
                    free = false;
            }
            const float dist = Distance(ImVec2(x + size.x * 0.5f, y + size.y * 0.5f),
                                        ImVec2(canvasSize.x * 0.5f, canvasSize.y * 0.8f));
            if (free && dist < best) {
                best = dist;
                spot = ImVec2(SnapToGrid(world.x), SnapToGrid(world.y));
            }
        }
    }
    return spot;
}

/// The row of devices below the canvas: drag one onto the workbench, or click it to put it in view.
void DrawPalette(Bench& b, ImVec2 canvasSize) {
    const kit::Fonts& fonts = kit::GetFonts();
    const ImGuiIO& io = ImGui::GetIO();
    kit::SectionLabel("Devices");
    ImGui::Dummy(ImVec2(0.0f, kit::space::k1));
    static const DeviceKind kKinds[] = {DeviceKind::Console, DeviceKind::Tv, DeviceKind::Controller,
                                        DeviceKind::RumblePak, DeviceKind::Cartridge};
    const ImVec2 itemSize(std::floor((canvasSize.x - kit::space::k4 * 2.0f - kit::space::k2 * 4.0f) / 5.0f), 40.0f);
    for (size_t i = 0; i < 5; ++i) {
        const DeviceKind kind = kKinds[i];
        const DeviceDef& def = Def(kind);
        const bool full = b.lab.Count(kind) >= def.maxCount;
        if (i > 0) ImGui::SameLine(0.0f, kit::space::k2);
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(full);
        const bool clicked = kit::NavButton("##device", itemSize);
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
        draw->AddRectFilled(min, max, Col(hovered && !full ? kit::color::kHover : kit::color::kRaised),
                            kit::radius::kControl);
        draw->AddRect(min, max, Col(focused ? kit::color::kAccentHover : kit::color::kHairline), kit::radius::kControl,
                      0, focused ? 2.0f : 1.0f);
        kit::DrawIcon(draw, DeviceIcon(kind), ImVec2(min.x + 20.0f, min.y + 20.0f), 20.0f,
                      Col(full ? kit::color::kTextDim : kit::color::kAccentText));
        const char* label = kind == DeviceKind::Console ? "Console" : def.name;
        kit::PutText(draw, fonts.small,
                     ImVec2(min.x + 38.0f, min.y + (itemSize.y - kit::Measure(fonts.small, label).y) * 0.5f),
                     Col(full ? kit::color::kTextDim : kit::color::kText),
                     kit::Ellipsize(fonts.small, label, itemSize.x - 44.0f).c_str());
        if (hovered && full) ImGui::SetTooltip("Already on the workbench.");
        if (clicked && !full) {
            if (const int id = b.lab.Add(kind, FreeSpot(b.lab, b.view, kind, canvasSize))) {
                b.view.selDevice = id;
                b.view.selCable = 0;
                b.Save();
            }
        }
        ImGui::PopID();
    }
}

// --- The panel on the right ---

/// Simulation mode: the power-on sequence, with controls to play it or step through it.
void DrawSequence(Bench& b) {
    View& view = b.view;
    const kit::Fonts& fonts = kit::GetFonts();
    using namespace kit;

    SectionTitle("Power-on sequence");
    Muted("What the console does when it starts, one event at a time.");
    ImGui::Dummy(ImVec2(0.0f, space::k2));
    const bool atEnd = view.step + 1 >= static_cast<int>(view.steps.size());
    if (Button(view.autoplay ? "Pause###autoplay" : "Play steps###autoplay", ButtonStyle::Secondary,
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
        view.steps = BuildSteps(b);
        view.step = -1;
        view.autoplay = false;
    }
    if (view.autoplay && ImGui::GetTime() - view.stepStart > kAutoplaySeconds) {
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
        if (reached)
            DrawIcon(draw, step.result == Severity::Info ? Icon::Check : Icon::Warning,
                     ImVec2(max.x - 16.0f, min.y + rowHeight * 0.5f), 16.0f,
                     Col(step.result == Severity::Error     ? color::kDanger
                         : step.result == Severity::Warning ? color::kStar
                                                            : color::kSuccess));
    }
    if (view.steps.empty()) Muted("Press Reset to build the sequence.");
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

/// One option: the value Settings gives, with a button to give this game its own; or its own, with a way back.
void OptionRow(Bench& b, const char* id) {
    using namespace kit;
    const kit::Fonts& fonts = GetFonts();
    const OptionDef* found = FindOption(id);
    if (!found) return;
    const OptionDef& option = *found;
    const bool custom = b.setup.Has(option.id);
    const int globalValue = option.get(b.global);
    int value = custom ? b.setup.Get(option.id, globalValue) : globalValue;
    const char* blocked = option.unavailable ? option.unavailable(b.effective) : nullptr;
    const char* warning = option.warning ? option.warning(b.effective) : nullptr;
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
        PutText(draw, fonts.caption, ImVec2(a.x + space::k3 * 0.5f, a.y + 4.0f), Col(color::kAccentText), "CUSTOM");
        ImGui::Dummy(ImVec2(ts.x + space::k3, ts.y));
    }
    HintText(blocked ? blocked : warning ? warning : option.help);
    ImGui::Dummy(ImVec2(0.0f, space::k1));
    if (!custom) {
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, color::kTextMuted);
        ImGui::TextUnformatted(ValueText(option, globalValue).c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 104.0f);
        if (Button("Customize", ButtonStyle::Secondary, Icon::None, ImVec2(104.0f, 30.0f))) {
            b.setup.Set(option.id, globalValue);
            changed = true;
        }
    } else {
        if (detail::DrawOptionControl(option, &value, globalValue)) {
            b.setup.Set(option.id, value);
            changed = true;
        }
        ImGui::SameLine(0.0f, space::k2);
        if (Button("Reset", ButtonStyle::Ghost, Icon::None, ImVec2(56.0f, 30.0f))) {
            b.setup.Clear(option.id);
            changed = true;
        }
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, space::k4));
    if (changed) b.Save();
}

void PowerRow(Bench& b, Device& d) {
    using namespace kit;
    bool on = d.powered;
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(GetFonts().label);
    ImGui::TextUnformatted("Power");
    ImGui::PopFont();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 44.0f);
    if (Toggle("##power", &on)) {
        d.powered = on;
        b.Save();
    }
    ImGui::Dummy(ImVec2(0.0f, space::k3));
}

/// What is wrong with the workbench, as a list.
void DrawCheckList(const Bench& b) {
    using namespace kit;
    if (b.problems.empty()) {
        Muted("Nothing to fix. This game is ready to play.");
        return;
    }
    SectionLabel("Check");
    ImGui::Dummy(ImVec2(0.0f, space::k1));
    for (const Problem& p : b.problems) {
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

/// The facts of a cartridge: what its header says.
void CartridgeFacts(const Bench& b) {
    using namespace kit;
    const detail::RomInfo& rom = b.rom;
    char line[96];
    InfoRow("Title", rom.valid ? rom.title.c_str() : "-");
    InfoRow("Game code", rom.valid ? (rom.gameId + std::string(1, rom.regionCode)).c_str() : "-");
    InfoRow("Region", b.regionText.c_str());
    InfoRow("Format", rom.valid ? rom.format : "-");
    std::snprintf(line, sizeof(line), "%.1f MB", static_cast<double>(rom.size) / (1024.0 * 1024.0));
    InfoRow("Size", line);
    std::snprintf(line, sizeof(line), "%08X %08X", rom.crc1, rom.crc2);
    InfoRow("Checksums", rom.valid ? line : "-");
}

/// Realtime mode: the options of the selected device. Returns true when the page should open the controller settings.
bool DrawInspector(Bench& b) {
    using namespace kit;
    Lab& lab = b.lab;
    View& view = b.view;
    bool openControllerSettings = false;

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
            ImGui::PushID(d.id); // two paks, or two loose controllers, have the same label
            if (ImGui::Selectable(label.c_str(), d.id == view.selDevice)) view.selDevice = d.id;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::Dummy(ImVec2(0.0f, space::k2));

    ImGui::BeginChild("##inspectorBody", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    RowsReset();
    if (!selected) {
        SectionTitle("Workbench");
        Muted("This is the hardware for %s. Click a device to see and change its options. Whatever you do not change "
              "follows Settings.",
              b.game.name.c_str());
        ImGui::Dummy(ImVec2(0.0f, space::k3));
        DrawCheckList(b);
        if (b.HasOptionOverrides()) {
            ImGui::Dummy(ImVec2(0.0f, space::k4));
            if (Button("Reset all changes", ButtonStyle::Ghost, Icon::None, ImVec2(0.0f, 36.0f))) {
                b.setup.ClearAll();
                b.Save();
            }
        }
        ImGui::EndChild();
        return false;
    }

    Device& device = *lab.Find(selected->id);
    SectionTitle(Def(device.kind).name);
    switch (device.kind) {
    case DeviceKind::Console:
        Muted("The CPU, the RSP and how fast the game runs. It always has the Expansion Pak, 8 MB in all.");
        ImGui::Dummy(ImVec2(0.0f, space::k3));
        PowerRow(b, device);
        for (const char* id : {"speed", "cpuCore", "rspEngine", "rewind"}) OptionRow(b, id);
        break;
    case DeviceKind::Tv:
        Muted("The picture: driver, resolution and accuracy.");
        ImGui::Dummy(ImVec2(0.0f, space::k3));
        PowerRow(b, device);
        for (const char* id : {"displayMode", "backendType", "internalResScale", "widescreen", "enableMipmaps",
                               "hiResTexEnabled", "rayTracing", "renderingMode", "fbReadAccess", "fbWriteAccess"})
            OptionRow(b, id);
        break;
    case DeviceKind::Cartridge:
        Muted("The game itself: what its header says, and how it saves.");
        ImGui::Dummy(ImVec2(0.0f, space::k3));
        CartridgeFacts(b);
        ImGui::Dummy(ImVec2(0.0f, space::k4));
        OptionRow(b, "saveChip");
        break;
    case DeviceKind::Controller: {
        const int player = lab.PlayerOf(device.id);
        Muted("Plug its cable into a console port to make it a player. Which controller plays is chosen in Settings.");
        ImGui::Dummy(ImVec2(0.0f, space::k3));
        InfoRow("Player", player >= 0 ? std::to_string(player + 1).c_str() : "not plugged in");
        InfoRow("Device", player >= 0 ? (NovaInput::Gamepads::Instance().Connected(player) ? "Game controller"
                                         : player == 0                                     ? "Keyboard"
                                                                                           : "None attached")
                                      : "-");
        InfoRow("Pak slot", lab.PakOf(device.id) ? "Rumble Pak" : "Empty (follows Settings)");
        ImGui::Dummy(ImVec2(0.0f, space::k4));
        openControllerSettings =
            Button("Controller settings", ButtonStyle::Secondary, Icon::Settings, ImVec2(0.0f, 36.0f));
        break;
    }
    case DeviceKind::RumblePak:
        Muted("A Rumble Pak in a controller's slot turns that player's rumble on for this game, even when Settings "
              "says automatic. The window "
              "shakes when the game rumbles, and so does a controller that can.");
        ImGui::Dummy(ImVec2(0.0f, space::k3));
        InfoRow("In", lab.CableAt(device.id, 0) ? "a controller" : "nothing");
        ImGui::Dummy(ImVec2(0.0f, space::k4));
        OptionRow(b, "rumbleShake");
        break;
    }
    ImGui::EndChild();
    return openControllerSettings;
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

    // The game on the bench; its setup, header and wiring are read when it changes
    if (m_simGame < 0 || m_simGame >= static_cast<int>(m_library.size()))
        m_simGame = (m_selectedGame >= 0 && m_selectedGame < static_cast<int>(m_library.size())) ? m_selectedGame : 0;
    const RomMetadata& game = m_library[static_cast<size_t>(m_simGame)];
    const bool justLoaded = (m_simLoaded != m_simGame);
    if (justLoaded) {
        m_simSetup.Load(game.path);
        m_simRom = detail::ReadRomInfo(game.path);
        if (!m_simLab.Load(game.path)) m_simLab = Lab::Default();
        m_simView = View();
        m_simLoaded = m_simGame;
    }
    if (justLoaded || m_frameCount % 60 == 0) {
        std::error_code ec;
        m_simRomExists = fs::exists(game.path, ec); // a stat per frame would stall on a sleeping network drive
    }
    View& view = m_simView;
    Bench bench(m_simLab, view, m_simSetup, game, m_simRom, m_launch, m_gfx, IsN64DDDisk(game.path), m_simRomExists);
    if (justLoaded && !bench.PaksMatchWiring())
        bench.Save(); // the setup file and the wiring file may have drifted apart

    // For screenshots: NOVA64_UI_TAB=simulator/simulation or simulator/simulation:N
    const bool forcedSimulation = m_frameCount < 3 && m_forcedSubTab.compare(0, 10, "simulation") == 0;
    if (forcedSimulation) view.simulation = true;
    if (view.simulation) {
        // The sequence follows every edit of the wiring and the options, so it never describes a bench that is gone
        view.steps = BuildSteps(bench);
        if (forcedSimulation) {
            const int wanted = m_forcedSubTab.size() > 11 ? std::atoi(m_forcedSubTab.c_str() + 11) : 99;
            view.step = std::min(wanted, static_cast<int>(view.steps.size()) - 1);
            view.stepStart = ImGui::GetTime();
        }
        view.step = std::min(view.step, static_cast<int>(view.steps.size()) - 1);
    }

    // === The toolbar: the game, the mode, and what to do with it ===
    const float top = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX(), top + 7.0f));
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::BeginCombo("##simGame", game.name.c_str())) {
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
                view.steps = BuildSteps(bench);
                view.step = -1;
                view.autoplay = false;
            }
        }
    }
    const float right = ImGui::GetContentRegionMax().x;
    ImGui::SetCursorPos(ImVec2(right - 130.0f, top));
    ImGui::BeginDisabled(bench.errors > 0);
    if (Button("Play", ButtonStyle::Primary, Icon::Play, ImVec2(130.0f, kToolbarHeight))) PlayRom(game.path, game.name);
    ImGui::EndDisabled();
    if (bench.errors > 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Fix the problems on the workbench first.");
    ImGui::SetCursorPos(ImVec2(right - 130.0f - space::k2 - 120.0f, top + 6.0f));
    if (Button("Reset wiring", ButtonStyle::Ghost, Icon::None, ImVec2(120.0f, 32.0f))) {
        m_simLab = Lab::Default();
        view.selDevice = view.selCable = 0;
        view.fitPending = true;
        bench.Save();
    }
    ImGui::SetCursorPos(ImVec2(right - 130.0f - space::k2 * 2.0f - 120.0f - 64.0f, top + 6.0f));
    if (Button("Fit", ButtonStyle::Ghost, Icon::None, ImVec2(64.0f, 32.0f))) view.fitPending = true;
    ImGui::SetCursorPosY(top + kToolbarHeight + space::k3);

    // === The canvas and the palette on the left, the panel on the right ===
    const float bodyHeight = std::max(300.0f, ImGui::GetContentRegionAvail().y - 2.0f);
    const float panelWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.42f, 300.0f, kPanelWidth);
    const float canvasWidth = ImGui::GetContentRegionAvail().x - panelWidth - space::k4;
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
    DrawCanvas(bench);
    ImGui::EndChild();

    BeginCard("##palette", ImVec2(canvasWidth, kPaletteHeight), false, ImVec2(space::k4, space::k3));
    DrawPalette(bench, ImVec2(canvasWidth, canvasHeight));
    EndCard();
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, space::k4);
    BeginCard("##simPanel", ImVec2(panelWidth, bodyHeight), false, ImVec2(space::k4, space::k4));
    if (view.simulation) {
        DrawSequence(bench);
    } else if (DrawInspector(bench)) {
        m_settingsPage = 3;
        m_activeTab = Tab::Settings;
    }
    EndCard();
}

} // namespace NovaUI
