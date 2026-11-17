#include "SimLab.hpp"
#include "GameFiles.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace NovaUI::sim {

const DeviceDef& Def(DeviceKind kind) {
    static const DeviceDef kDefs[] = {
        {DeviceKind::Console,
         "Nintendo 64",
         ImVec2(184.0f, 112.0f),
         1,
         {{PortType::Av, Facing::Right, 1.0f, 0.5f, "AV out"},
          {PortType::Controller, Facing::Bottom, 0.2f, 1.0f, "Controller 1"},
          {PortType::Controller, Facing::Bottom, 0.4f, 1.0f, "Controller 2"},
          {PortType::Controller, Facing::Bottom, 0.6f, 1.0f, "Controller 3"},
          {PortType::Controller, Facing::Bottom, 0.8f, 1.0f, "Controller 4"},
          {PortType::Cartridge, Facing::Left, 0.0f, 0.5f, "Cartridge slot"}}},
        {DeviceKind::Tv, "TV", ImVec2(144.0f, 112.0f), 1, {{PortType::Av, Facing::Left, 0.0f, 0.5f, "AV in"}}},
        {DeviceKind::Controller,
         "Controller",
         ImVec2(120.0f, 84.0f),
         4,
         {{PortType::Controller, Facing::Top, 0.5f, 0.0f, "Cable"},
          {PortType::Pak, Facing::Right, 1.0f, 0.5f, "Pak slot"}}},
        {DeviceKind::RumblePak,
         "Rumble Pak",
         ImVec2(108.0f, 76.0f),
         4,
         {{PortType::Pak, Facing::Left, 0.0f, 0.5f, "Connector"}}},
        {DeviceKind::Cartridge,
         "Cartridge",
         ImVec2(120.0f, 84.0f),
         1,
         {{PortType::Cartridge, Facing::Right, 1.0f, 0.5f, "Connector"}}},
    };
    return kDefs[static_cast<int>(kind)];
}

namespace {

/// Ids start at 1, and are kept well below the largest int so that the next one cannot overflow.
constexpr int kMaxId = 1000000000;
/// Devices further from the origin than this are put back at the edge; it keeps a hand-edited file from producing
/// a view that is billions of units wide.
constexpr float kMaxCoordinate = 100000.0f;

bool ValidId(int id) { return id > 0 && id < kMaxId; }

const char* KindName(DeviceKind kind) {
    switch (kind) {
    case DeviceKind::Console:    return "console";
    case DeviceKind::Tv:         return "tv";
    case DeviceKind::Controller: return "controller";
    case DeviceKind::RumblePak:  return "rumble";
    case DeviceKind::Cartridge:  return "cartridge";
    }
    return "console";
}

bool KindFromName(const std::string& name, DeviceKind* out) {
    for (int i = 0; i < 5; ++i) {
        if (name == KindName(static_cast<DeviceKind>(i))) {
            *out = static_cast<DeviceKind>(i);
            return true;
        }
    }
    return false;
}

} // namespace

Lab Lab::Default() {
    Lab lab;
    const int cartridge = lab.Add(DeviceKind::Cartridge, ImVec2(0.0f, 14.0f));
    const int console = lab.Add(DeviceKind::Console, ImVec2(184.0f, 0.0f));
    const int tv = lab.Add(DeviceKind::Tv, ImVec2(424.0f, 0.0f));
    const int controller = lab.Add(DeviceKind::Controller, ImVec2(160.0f, 176.0f));
    lab.Connect(console, kConsoleCartridge, cartridge, 0);
    lab.Connect(console, kConsoleAv, tv, 0);
    lab.Connect(console, kConsoleFirstController, controller, 0);
    return lab;
}

std::string Lab::PathFor(const std::string& romPath) {
    return (fs::path("settings") / "games" / (GameKey(romPath) + ".lab")).generic_string();
}

bool Lab::Load(const std::string& romPath) {
    std::ifstream f(PathFor(romPath));
    if (!f) return false;
    Lab lab;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        std::string word;
        if (!(in >> word) || word[0] == '#') continue;
        if (word == "device") {
            Device d;
            std::string kind;
            int powered = 1;
            if (!(in >> d.id >> kind >> d.pos.x >> d.pos.y) || !KindFromName(kind, &d.kind) || !ValidId(d.id)) continue;
            if (!(in >> powered)) powered = 1; // a missing or unreadable switch means on
            d.powered = powered != 0;
            if (!std::isfinite(d.pos.x) || !std::isfinite(d.pos.y)) continue;
            d.pos = ImVec2(std::clamp(d.pos.x, -kMaxCoordinate, kMaxCoordinate),
                           std::clamp(d.pos.y, -kMaxCoordinate, kMaxCoordinate));
            if (lab.IdInUse(d.id) || lab.Count(d.kind) >= Def(d.kind).maxCount) continue;
            lab.devices.push_back(d);
            lab.m_nextId = std::max(lab.m_nextId, d.id + 1);
        } else if (word == "cable") {
            Cable c;
            if (!(in >> c.id >> c.a >> c.portA >> c.b >> c.portB) || !ValidId(c.id) || lab.IdInUse(c.id)) continue;
            const Device* a = lab.Find(c.a);
            const Device* b = lab.Find(c.b);
            if (!a || !b || c.portA < 0 || c.portB < 0 || c.portA >= static_cast<int>(Def(a->kind).ports.size()) ||
                c.portB >= static_cast<int>(Def(b->kind).ports.size()) || !lab.CanConnect(c.a, c.portA, c.b, c.portB))
                continue;
            lab.cables.push_back(c);
            lab.m_nextId = std::max(lab.m_nextId, c.id + 1);
        }
    }
    if (lab.devices.empty()) return false;
    *this = std::move(lab);
    return true;
}

bool Lab::Save(const std::string& romPath) const {
    std::ostringstream f;
    f << "# The Simulator's workbench for this game.\n";
    for (const Device& d : devices)
        f << "device " << d.id << ' ' << KindName(d.kind) << ' ' << d.pos.x << ' ' << d.pos.y << ' '
          << (d.powered ? 1 : 0) << '\n';
    for (const Cable& c : cables)
        f << "cable " << c.id << ' ' << c.a << ' ' << c.portA << ' ' << c.b << ' ' << c.portB << '\n';
    return WriteFileAtomically(PathFor(romPath), f.str());
}

Device* Lab::Find(int id) {
    for (Device& d : devices)
        if (d.id == id) return &d;
    return nullptr;
}

const Device* Lab::Find(int id) const {
    for (const Device& d : devices)
        if (d.id == id) return &d;
    return nullptr;
}

int Lab::Count(DeviceKind kind) const {
    int n = 0;
    for (const Device& d : devices)
        if (d.kind == kind) ++n;
    return n;
}

bool Lab::IdInUse(int id) const {
    for (const Device& d : devices)
        if (d.id == id) return true;
    for (const Cable& c : cables)
        if (c.id == id) return true;
    return false;
}

int Lab::First(DeviceKind kind) const {
    for (const Device& d : devices)
        if (d.kind == kind) return d.id;
    return 0;
}

int Lab::Add(DeviceKind kind, ImVec2 pos) {
    if (Count(kind) >= Def(kind).maxCount) return 0;
    Device d;
    d.id = m_nextId++;
    d.kind = kind;
    d.pos = pos;
    devices.push_back(d);
    return d.id;
}

void Lab::Remove(int deviceId) {
    cables.erase(std::remove_if(cables.begin(), cables.end(),
                                [&](const Cable& c) { return c.a == deviceId || c.b == deviceId; }),
                 cables.end());
    devices.erase(std::remove_if(devices.begin(), devices.end(), [&](const Device& d) { return d.id == deviceId; }),
                  devices.end());
}

void Lab::RemoveCable(int cableId) {
    cables.erase(std::remove_if(cables.begin(), cables.end(), [&](const Cable& c) { return c.id == cableId; }),
                 cables.end());
}

const Cable* Lab::CableAt(int deviceId, int port) const {
    for (const Cable& c : cables)
        if ((c.a == deviceId && c.portA == port) || (c.b == deviceId && c.portB == port)) return &c;
    return nullptr;
}

bool Lab::CanConnect(int deviceA, int portA, int deviceB, int portB) const {
    const Device* a = Find(deviceA);
    const Device* b = Find(deviceB);
    if (!a || !b || deviceA == deviceB || a->kind == b->kind) return false;
    const auto& pa = Def(a->kind).ports;
    const auto& pb = Def(b->kind).ports;
    if (portA < 0 || portB < 0 || portA >= static_cast<int>(pa.size()) || portB >= static_cast<int>(pb.size()))
        return false;
    if (pa[static_cast<size_t>(portA)].type != pb[static_cast<size_t>(portB)].type) return false;
    return !CableAt(deviceA, portA) && !CableAt(deviceB, portB);
}

bool Lab::Connect(int deviceA, int portA, int deviceB, int portB) {
    if (!CanConnect(deviceA, portA, deviceB, portB)) return false;
    Cable c;
    c.id = m_nextId++;
    c.a = deviceA;
    c.portA = portA;
    c.b = deviceB;
    c.portB = portB;
    cables.push_back(c);
    return true;
}

ImVec2 Lab::PortPos(const Device& device, int port) const {
    const DeviceDef& def = Def(device.kind);
    const PortDef& p = def.ports[static_cast<size_t>(port)];
    return ImVec2(device.pos.x + def.size.x * p.fx, device.pos.y + def.size.y * p.fy);
}

int Lab::PlayerOf(int controllerId) const {
    const int console = First(DeviceKind::Console);
    const Cable* cable = CableAt(controllerId, 0);
    if (!cable || !console) return -1;
    const bool controllerIsA = (cable->a == controllerId);
    const int otherDevice = controllerIsA ? cable->b : cable->a;
    const int otherPort = controllerIsA ? cable->portB : cable->portA;
    if (otherDevice != console) return -1;
    return otherPort - kConsoleFirstController;
}

int Lab::ControllerOnPort(int player) const {
    const int console = First(DeviceKind::Console);
    if (!console || player < 0 || player > 3) return 0;
    const Cable* cable = CableAt(console, kConsoleFirstController + player);
    if (!cable) return 0;
    return cable->a == console ? cable->b : cable->a;
}

int Lab::PakOf(int controllerId) const {
    const Cable* cable = CableAt(controllerId, 1);
    if (!cable) return 0;
    return cable->a == controllerId ? cable->b : cable->a;
}

bool Lab::PlayerHasRumblePak(int player) const {
    const int controller = ControllerOnPort(player);
    return controller && PakOf(controller);
}

LinkState Lab::State(const Cable& cable, bool cartridgeOk) const {
    const Device* a = Find(cable.a);
    const Device* b = Find(cable.b);
    if (!a || !b) return LinkState::Down;
    const int console = First(DeviceKind::Console);
    const Device* consoleDevice = Find(console);
    for (const Device* d : {a, b})
        if ((d->kind == DeviceKind::Console || d->kind == DeviceKind::Tv) && !d->powered) return LinkState::Down;
    // A pak is only as live as the controller it sits in.
    if (a->kind == DeviceKind::RumblePak || b->kind == DeviceKind::RumblePak) {
        const int controller = (a->kind == DeviceKind::Controller) ? a->id : b->id;
        if (PlayerOf(controller) < 0) return LinkState::Waiting;
        return (consoleDevice && consoleDevice->powered) ? LinkState::Up : LinkState::Down;
    }
    if (a->kind == DeviceKind::Cartridge || b->kind == DeviceKind::Cartridge)
        return cartridgeOk ? LinkState::Up : LinkState::Down;
    return LinkState::Up;
}

std::vector<Problem> Lab::Check(bool cartridgeOk) const {
    std::vector<Problem> out;
    const int console = First(DeviceKind::Console);
    const Device* consoleDevice = Find(console);
    if (!console) {
        out.push_back({Severity::Error, "There is no console on the workbench. Drag a Nintendo 64 onto it.", 0});
        return out;
    }
    if (!consoleDevice->powered) out.push_back({Severity::Error, "The console is switched off.", console});

    const int cartridge = First(DeviceKind::Cartridge);
    const Cable* cartridgeCable = CableAt(console, kConsoleCartridge);
    if (!cartridge)
        out.push_back(
            {Severity::Error, "There is no cartridge. Drag the game's cartridge onto the workbench.", console});
    else if (!cartridgeCable)
        out.push_back({Severity::Error,
                       "The cartridge is not in the console. Connect it to the console's cartridge slot.", cartridge});
    else if (!cartridgeOk)
        out.push_back({Severity::Error, "The game's file is missing, or it is not an N64 ROM.", cartridge});

    const int tv = First(DeviceKind::Tv);
    const Cable* avCable = CableAt(console, kConsoleAv);
    if (!tv || !avCable)
        out.push_back({Severity::Error, "No display: connect the console's AV out to a TV.", tv ? tv : console});
    else if (!Find(tv)->powered)
        out.push_back({Severity::Error, "The TV is switched off, so there is no picture.", tv});

    if (!ControllerOnPort(0))
        out.push_back({Severity::Warning, "No controller in port 1. Almost every game expects player 1.", console});
    for (const Device& d : devices) {
        if (d.kind == DeviceKind::Controller && PlayerOf(d.id) < 0)
            out.push_back({Severity::Info, "A controller is not plugged into the console, so it does nothing.", d.id});
        if (d.kind == DeviceKind::RumblePak && !CableAt(d.id, 0))
            out.push_back({Severity::Info, "A Rumble Pak is not in a controller, so it does nothing.", d.id});
    }
    return out;
}

} // namespace NovaUI::sim
