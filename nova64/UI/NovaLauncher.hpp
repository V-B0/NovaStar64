#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstring>
#include <functional>
#include <cstdint>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "../N64SYS/SaveState.hpp"
#include "../NovaLua/LuaEngine.hpp"
#include "HexEditor.hpp"

struct SDL_Window;
typedef void* SDL_GLContext;

namespace NovaUI {

enum class Tab {
    Home,
    Games,
    Settings,
    Scripts,
    HexEditor,
    Achievements,
    Simulator
};

struct RomMetadata {
    std::string path;
    std::string name;
    std::string boxArtPath;
    std::string notes;
};

// -----------------------------------------------
// Simulator node system
// -----------------------------------------------
enum class SimNodeKind {
    N64Console,
    TV,
    N64DD,
    Controller,
    ExpansionPak,
    Cartridge,
    RumblePak,
    ControllerPak,
    TransferPak
};

struct SimNode {
    int id;
    SimNodeKind kind;

    bool showConfig = false;
    bool isPowered  = false;

    int  ramMB           = 4;
    bool hasExpansionPak = false;
    bool has64DD         = false;
    char cartName[128]   = {};
    char cartPath[512]   = {};
    char ddCartName[128] = {};
    char ddCartPath[512] = {};

    int pakType = 0;
};

struct SimLink {
    int id;
    int fromPin;
    int toPin;
};

enum class SimTool { Select, Delete, Cable };

// Represents a single running emulator child process (Simulator mode only)
struct EmuInstance {
    std::string romName;
    std::string romPath;
#ifdef _WIN32
    PROCESS_INFORMATION pi{};
#else
    // POSIX child process id (-1 when none / reaped).
    long pid = -1;
#endif
    bool running = true;
};

// -----------------------------------------------
// Key bindings (configurable, saved to settings/)
// SDL_Scancode is enum : int; we store as int to avoid pulling SDL headers here.
// -----------------------------------------------
struct KeyBindings {
    int A       = 27;  // SDL_SCANCODE_X
    int B       = 29;  // SDL_SCANCODE_Z
    int Z       = 224; // SDL_SCANCODE_LCTRL
    int Start   = 40;  // SDL_SCANCODE_RETURN
    int DPadUp  = 23;  // SDL_SCANCODE_T
    int DPadDn  = 10;  // SDL_SCANCODE_G
    int DPadLt  = 9;   // SDL_SCANCODE_F
    int DPadRt  = 11;  // SDL_SCANCODE_H
    int L       = 225; // SDL_SCANCODE_LSHIFT
    int R       = 229; // SDL_SCANCODE_RSHIFT
    int CUp     = 82;  // SDL_SCANCODE_UP
    int CDown   = 81;  // SDL_SCANCODE_DOWN
    int CLeft   = 80;  // SDL_SCANCODE_LEFT
    int CRight  = 79;  // SDL_SCANCODE_RIGHT
    int StickUp = 26;  // SDL_SCANCODE_W
    int StickDn = 22;  // SDL_SCANCODE_S
    int StickLt = 4;   // SDL_SCANCODE_A
    int StickRt = 7;   // SDL_SCANCODE_D

    void Save(const std::string& path) const;
    void Load(const std::string& path);
};

// -----------------------------------------------
// Main Launcher class
// -----------------------------------------------
class NovaLauncher {
public:
    NovaLauncher();
    ~NovaLauncher();

    std::string Run();

private:
    // ---- Render Sections ----
    void RenderTopNavbar(SDL_Window* window);
    void RenderUI(SDL_Window* window);
    void RenderSidebar();
    void RenderHomeTab();
    void RenderGamesTab();
    void RenderSettingsTab();
    void RenderScriptsTab();
    void RenderHexEditorTab();
    void RenderAchievementsTab();
    void RenderSimulatorTab();
    void RenderInstancesPanel();
    void RenderInputSettingsWidget();

    // ---- Pause Menu (ESC) ----
    void RenderPauseMenu();

    // ---- Embedded Game ----
    void LaunchEmbedded(const std::string& romPath);
    void StopEmbedded();
    void RenderGameView(SDL_Window* window);

    // ---- Simulator / Child process ----
    void LaunchEmulator(const SimNode& node);
    // Launch a ROM as a separate child process (own window). On macOS this is
    // the path that actually renders: the child owns its main thread, so the
    // Vulkan/MoltenVK window/surface can be created (a background thread cannot).
    void LaunchProcess(const std::string& romPath, const std::string& romName);
    void PollInstances();

    // ---- Simulator helpers ----
    void SpawnNode(SimNodeKind kind, int cartIndex = -1);
    void RenderSimNode(SimNode& node);
    const char* NodeIcon(SimNodeKind k);
    const char* NodeLabel(SimNodeKind k);
    bool IsN64DDDisk(const std::string& path);

    int GetPinId(int nodeId, int pinIndex) const { return (nodeId << 4) | pinIndex; }
    int GetNodeIdFromPin(int pinId) const { return pinId >> 4; }
    bool IsInputPin(int pinId) const { return (pinId & 0xF) > 0; }

    // Window animation
    void UpdateWindowAnimations(SDL_Window* window);

    // GL texture for embedded game
    void UploadFrameTex(uint32_t srcTex, int w, int h);
    void UploadFromPixelBuf();   // uploads g_framePixels → m_gameTex (launcher GL thread)


    // ---- State ----
    std::string m_selectedRom;
    bool m_shouldExit;
    Tab m_activeTab;

    std::vector<RomMetadata> m_library;

    // ROM modals
    bool m_showEditModal;
    bool m_showAddModal;
    int  m_editingRomIndex;
    char m_editNameBuffer[128];
    char m_editBoxArtBuffer[256];
    char m_editNotesBuffer[1024];

    // Settings
    int  m_ramAmount;
    int  m_graphicsApi;
    bool m_vsync;
    int  m_internalResScale;
    bool m_fullscreen;
    int  m_targetFPS;
    int  m_antiAliasing;
    int  m_texFilter;
    bool m_enableMipmaps;
    bool m_multiThreading;
    bool m_showFPS;

    // Key bindings
    KeyBindings m_bindings;
    int m_rebindTarget = -1;   // -1 = not rebinding; >=0 = index of button being rebound

    // Simulator State
    int  m_simMode;
    SimTool m_simTool;
    int     m_nextNodeId;
    int     m_nextLinkId;
    int     m_selectedCartIndex;
    std::vector<SimNode> m_simNodes;
    std::vector<SimLink> m_simLinks;
    std::vector<EmuInstance> m_instances;

    // Window Animation
    float m_windowOpacity;
    enum class AnimState { FadeIn, Stable, FadeOut, MinimizeOut } m_animState;

    // ---- Embedded Emulator State ----
    std::thread          m_emuThread;
    std::atomic<bool>    m_emuRunning { false };
    std::string          m_emuRomPath;

    // Display texture (blitted to launcher window)
    uint32_t m_gameTex      = 0;   ///< GL texture ID updated each frame
    int      m_gameTexW     = 640;
    int      m_gameTexH     = 480;
    std::mutex m_gameTexMtx;

    // Pause menu
    bool m_showPauseMenu    = false;

    // Speed: 1x / 2x (TAB key)
    bool m_turboMode        = false;

    // Save state manager
    SaveStateManager m_saveState;

    // Lua scripting
    LuaEngine m_lua;
    std::vector<std::string> m_scriptFiles; ///< discovered in scripts/
    void RefreshScriptFiles();

    // Hex editor
    HexEditor m_hexEditor;

    // RetroAchievements
    std::string m_raUsername;
    std::string m_raPassword;

    // Rewind key display name
    static const char* kRewindKeyName;
};

} // namespace NovaUI