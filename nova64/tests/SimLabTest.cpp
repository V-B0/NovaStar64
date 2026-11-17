/**
 * @file SimLabTest.cpp
 * @brief Tests of the Simulator workbench model (UI/SimLab): connecting, the rules a cable must obey, validation,
 *        link lights and saving; and of the file names and safe writes shared by the per-game files
 *        (UI/GameFiles). No window or graphics needed.
 */
#include "../UI/GameFiles.hpp"
#include "../UI/SimLab.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace NovaUI::sim;

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::printf("FAILED %s:%d  %s\n", __FILE__, __LINE__, #condition);                                         \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

bool HasProblem(const std::vector<Problem>& problems, Severity severity, const char* word) {
    for (const Problem& p : problems)
        if (p.severity == severity && p.text.find(word) != std::string::npos) return true;
    return false;
}

void DefaultBenchIsReady() {
    const Lab lab = Lab::Default();
    CHECK(lab.devices.size() == 4);
    CHECK(lab.cables.size() == 3);
    CHECK(lab.Check(true).empty());
    CHECK(lab.PlayerOf(lab.ControllerOnPort(0)) == 0);

    // A game file that is missing or not a ROM is an error on the cartridge
    CHECK(HasProblem(lab.Check(false), Severity::Error, "not an N64 ROM"));
    for (const Cable& c : lab.cables) CHECK(lab.State(c, true) == LinkState::Up);
}

void CablesObeyThePorts() {
    Lab lab = Lab::Default();
    const int console = lab.First(DeviceKind::Console);
    const int tv = lab.First(DeviceKind::Tv);
    const int controller = lab.First(DeviceKind::Controller);

    CHECK(!lab.Connect(console, kConsoleAv, tv, 0));         // both ports are taken
    CHECK(!lab.CanConnect(console, kConsoleAv, console, 1)); // a device cannot join itself
    CHECK(!lab.CanConnect(tv, 0, controller, 0));            // an AV port is not a controller cable
    CHECK(!lab.CanConnect(console, 99, controller, 0));      // a port that does not exist

    const int second = lab.Add(DeviceKind::Controller, ImVec2(0, 0));
    CHECK(second != 0);
    CHECK(lab.Connect(second, 0, console, kConsoleFirstController + 1));
    CHECK(lab.PlayerOf(second) == 1);
    CHECK(lab.ControllerOnPort(1) == second);
    CHECK(!lab.Connect(second, 0, console, kConsoleFirstController + 2)); // its cable is already in use
}

void LimitsOnTheBench() {
    Lab lab = Lab::Default();
    CHECK(lab.Add(DeviceKind::Console, ImVec2(0, 0)) == 0);
    CHECK(lab.Add(DeviceKind::Tv, ImVec2(0, 0)) == 0);
    CHECK(lab.Add(DeviceKind::Cartridge, ImVec2(0, 0)) == 0);
    int controllers = lab.Count(DeviceKind::Controller);
    while (lab.Add(DeviceKind::Controller, ImVec2(0, 0))) ++controllers;
    CHECK(controllers == 4);
}

void RumblePakBelongsToAPlayer() {
    Lab lab = Lab::Default();
    const int controller = lab.ControllerOnPort(0);
    const int pak = lab.Add(DeviceKind::RumblePak, ImVec2(0, 0));
    CHECK(!lab.PlayerHasRumblePak(0));
    CHECK(lab.Connect(pak, 0, controller, 1));
    CHECK(lab.PlayerHasRumblePak(0));
    CHECK(lab.PakOf(controller) == pak);

    // Taking the controller off the console leaves the pak waiting
    const Cable* controllerCable = lab.CableAt(controller, 0);
    CHECK(controllerCable != nullptr);
    lab.RemoveCable(controllerCable->id);
    CHECK(!lab.PlayerHasRumblePak(0));
    CHECK(lab.State(*lab.CableAt(pak, 0), true) == LinkState::Waiting);
    CHECK(HasProblem(lab.Check(true), Severity::Warning, "port 1"));
}

void RemovingADeviceRemovesItsCables() {
    Lab lab = Lab::Default();
    const size_t before = lab.cables.size();
    lab.Remove(lab.First(DeviceKind::Tv));
    CHECK(lab.cables.size() == before - 1);
    CHECK(HasProblem(lab.Check(true), Severity::Error, "No display"));
    lab.Remove(lab.First(DeviceKind::Console));
    CHECK(lab.cables.empty());
    CHECK(HasProblem(lab.Check(true), Severity::Error, "no console"));
}

void PowerMattersToTheLinkLights() {
    Lab lab = Lab::Default();
    lab.Find(lab.First(DeviceKind::Tv))->powered = false;
    CHECK(HasProblem(lab.Check(true), Severity::Error, "TV is switched off"));
    CHECK(lab.State(*lab.CableAt(lab.First(DeviceKind::Console), kConsoleAv), true) == LinkState::Down);

    lab.Find(lab.First(DeviceKind::Console))->powered = false;
    CHECK(HasProblem(lab.Check(true), Severity::Error, "console is switched off"));
    for (const Cable& c : lab.cables) CHECK(lab.State(c, true) == LinkState::Down);
}

void SavingAndLoading() {
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "novastar64-simlab-test";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    std::filesystem::current_path(folder);

    Lab lab = Lab::Default();
    const int pak = lab.Add(DeviceKind::RumblePak, ImVec2(300, 180));
    lab.Connect(pak, 0, lab.ControllerOnPort(0), 1);
    lab.Find(lab.First(DeviceKind::Tv))->powered = false;
    CHECK(lab.Save("roms/Some Game (USA).z64"));

    Lab loaded;
    CHECK(loaded.Load("roms/Some Game (USA).z64"));
    CHECK(loaded.devices.size() == lab.devices.size());
    CHECK(loaded.cables.size() == lab.cables.size());
    CHECK(loaded.PlayerHasRumblePak(0));
    CHECK(!loaded.Find(loaded.First(DeviceKind::Tv))->powered);
    CHECK(loaded.Find(pak)->pos.x == 300.0f);

    // A new device after loading gets an id nothing else uses
    const int added = loaded.Add(DeviceKind::Controller, ImVec2(0, 0));
    for (const Device& d : loaded.devices)
        if (d.id != added) CHECK(d.id != added);
    for (const Cable& c : loaded.cables) CHECK(c.id != added);

    CHECK(!Lab().Load("roms/Another Game.z64")); // no file: the lab is left alone
}

void ACorruptFileCannotBreakTheBench() {
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "novastar64-simlab-test-corrupt";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / "settings" / "games");
    std::filesystem::current_path(folder);
    {
        std::ofstream f(Lab::PathFor("roms/bad.z64"));
        f << "device 1 console 0 0 1\n"
             "device 1 tv 100 0 1\n"        // the id is already used
             "device 2 toaster 0 0 1\n"     // not a kind of device
             "device 3 tv 200 0 1\n"
             "device 4 console 0 0 1\n"     // a second console
             "device -5 controller 0 0 1\n" // a negative id
             "cable 10 1 0 3 0\n"
             "cable 11 1 0 3 0\n"           // both ports are taken now
             "cable 12 1 77 3 0\n"          // a port that does not exist
             "cable 13 1 1 9 0\n"           // a device that does not exist
             "cable 14 3 0 3 0\n"           // a device joined to itself
             "garbage line\n";
    }
    Lab lab;
    CHECK(lab.Load("roms/bad.z64"));
    CHECK(lab.devices.size() == 2);
    CHECK(lab.cables.size() == 1);
    CHECK(lab.Add(DeviceKind::Controller, ImVec2(0, 0)) > 10); // new ids are above every id that was kept
}

void GameKeysDoNotCollide() {
    using NovaUI::GameKey;
    CHECK(GameKey("roms/sm64.z64") == "sm64");                   // a plain name is kept as it is
    CHECK(GameKey("games/Star_Fox-64.n64") == "Star_Fox-64");
    CHECK(GameKey("Zelda (U).z64") != GameKey("Zelda [U].z64")); // the same letters, different names
    CHECK(GameKey("Zelda (U).z64").rfind("Zelda__U_-", 0) == 0); // still readable
    CHECK(GameKey("\xE3\x82\xBC\xE3\x83\xAB.z64") !=
          GameKey("\xE3\x83\x9E\xE3\x83\xAA.z64"));              // names in another script
    CHECK(!GameKey("").empty());
    CHECK(GameKey("a/b/c.z64") == GameKey("c.z64"));             // only the file name counts
}

void FilesAreWrittenWhole() {
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "novastar64-write-test";
    std::filesystem::remove_all(folder);
    std::filesystem::current_path(std::filesystem::temp_directory_path());
    const std::string path = (folder / "deeper" / "file.txt").string();

    CHECK(NovaUI::WriteFileAtomically(path, "first"));  // makes the folder
    CHECK(NovaUI::WriteFileAtomically(path, "second")); // replaces the file
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text == "second");
    CHECK(!std::filesystem::exists(path + ".tmp")); // nothing left behind

    // A folder in the way is a failure, and says so
    std::filesystem::create_directories(folder / "blocked.txt");
    CHECK(!NovaUI::WriteFileAtomically((folder / "blocked.txt").string(), "x"));
}

} // namespace

int main() {
    DefaultBenchIsReady();
    CablesObeyThePorts();
    LimitsOnTheBench();
    RumblePakBelongsToAPlayer();
    RemovingADeviceRemovesItsCables();
    PowerMattersToTheLinkLights();
    SavingAndLoading();
    ACorruptFileCannotBreakTheBench();
    GameKeysDoNotCollide();
    FilesAreWrittenWhole();
    if (g_failures == 0) std::printf("SimLab: all tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
