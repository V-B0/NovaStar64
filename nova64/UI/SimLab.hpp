/**
 * @file SimLab.hpp
 * @brief The Simulator's workbench: virtual hardware (console, TV, controllers, paks, a cartridge), the cables
 *        between them, and the rules that say whether the setup would work.
 *
 * The workbench is a model of the real wiring of a Nintendo 64: the console's AV output goes to a TV, controllers
 * plug into its four ports, a Rumble Pak goes into a controller, and the cartridge goes into its slot. Each game
 * has its own workbench, saved in settings/games/<rom name>.lab. What the wiring and the devices' options mean for
 * emulation is in LauncherSimulator.cpp.
 */

#pragma once

#include <imgui.h>

#include <string>
#include <vector>

namespace NovaUI::sim {

enum class DeviceKind { Console, Tv, Controller, RumblePak, Cartridge };
enum class PortType { Av, Controller, Pak, Cartridge };
enum class Facing { Left, Right, Top, Bottom };

struct PortDef {
    PortType type;
    Facing facing; ///< the side of the device the cable leaves from
    float fx, fy;  ///< where the port sits, as a fraction of the device's size
    const char* name;
};

struct DeviceDef {
    DeviceKind kind;
    const char* name;
    ImVec2 size;
    int maxCount; ///< how many of them the workbench may hold
    std::vector<PortDef> ports;
};

const DeviceDef& Def(DeviceKind kind);

/// Console ports: 0 is the AV output, 1 to 4 are the controller ports, 5 is the cartridge slot (on its left).
constexpr int kConsoleAv = 0;
constexpr int kConsoleFirstController = 1;
constexpr int kConsoleCartridge = 5;

struct Device {
    int id = 0;
    DeviceKind kind = DeviceKind::Console;
    ImVec2 pos{0, 0};
    bool powered = true; ///< the console and the TV have a power switch
};

struct Cable {
    int id = 0;
    int a = 0, portA = 0; ///< one end: a device and the number of its port
    int b = 0, portB = 0; ///< the other end
};

/// What a cable's link light shows.
enum class LinkState { Up, Waiting, Down };

enum class Severity { Info, Warning, Error };

struct Problem {
    Severity severity = Severity::Info;
    std::string text;
    int device = 0; ///< the device to select to look at it, or 0
};

class Lab {
public:
    std::vector<Device> devices;
    std::vector<Cable> cables;

    /// A console with a cartridge, a TV and controller 1, wired up.
    static Lab Default();

    /// settings/games/<rom name>.lab
    static std::string PathFor(const std::string& romPath);
    /// Returns false (leaving the lab as it was) when there is no file.
    bool Load(const std::string& romPath);
    bool Save(const std::string& romPath) const;

    Device* Find(int id);
    const Device* Find(int id) const;
    int Count(DeviceKind kind) const;
    int First(DeviceKind kind) const;

    /// Puts a new device on the workbench; returns its id, or 0 when there are already as many as are allowed.
    int Add(DeviceKind kind, ImVec2 pos);
    /// Removes a device and the cables on it.
    void Remove(int deviceId);
    void RemoveCable(int cableId);

    const Cable* CableAt(int deviceId, int port) const;
    /// True if the two ports are of the same kind, on different devices, and both are free.
    bool CanConnect(int deviceA, int portA, int deviceB, int portB) const;
    /// Connects two ports; false if they cannot be connected.
    bool Connect(int deviceA, int portA, int deviceB, int portB);

    /// Where a port is, in workbench coordinates.
    ImVec2 PortPos(const Device& device, int port) const;

    /// The player (0 to 3) a controller is plugged in as, or -1 if it is not plugged into the console.
    int PlayerOf(int controllerId) const;
    /// The controller in a console port (0 to 3), or 0.
    int ControllerOnPort(int player) const;
    /// The pak in a controller's slot, or 0.
    int PakOf(int controllerId) const;
    /// The Rumble Pak is in the controller of this player.
    bool PlayerHasRumblePak(int player) const;

    LinkState State(const Cable& cable, bool cartridgeOk) const;

    /// What is wrong with the wiring and what to look at, most important first. `cartridgeOk` is false when the
    /// game's file is missing or is not a ROM.
    std::vector<Problem> Check(bool cartridgeOk) const;

private:
    int m_nextId = 1;
};

/// One event of the power-on sequence the Simulation mode plays.
struct Step {
    int from = 0;  ///< the device the event comes from
    int to = 0;    ///< the device it goes to, or 0 when it stays on `from`
    int cable = 0; ///< the cable it travels along, or 0
    std::string text;
    Severity result = Severity::Info;
};

/// What the workbench shows and what the pointer is doing, not saved.
struct View {
    ImVec2 pan{24.0f, 20.0f};
    int selDevice = 0;
    int selCable = 0;

    enum class Drag { None, Pan, Device, Cable, PickUp } drag = Drag::None;
    int dragDevice = 0;     ///< Device: the device being moved
    ImVec2 dragGrab{0, 0};  ///< Device: where on it the pointer took hold
    int dragFromDevice = 0; ///< Cable: the port the cable started from
    int dragFromPort = -1;
    int pickedCable = 0;    ///< PickUp: the cable that may be lifted off its port
    ImVec2 pressAt{0, 0};
    bool moved = false;

    int contextDevice = 0;
    int contextCable = 0;

    bool simulation = false; ///< Simulation mode: step through the power-on sequence
    std::vector<Step> steps;
    int step = -1;           ///< the event shown now, or -1 before the first
    double stepStart = 0.0;
    bool autoplay = false;

    bool fitPending = true;
};

} // namespace NovaUI::sim
