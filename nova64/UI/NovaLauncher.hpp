#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../N64SYS/SaveState.hpp"
#include "../NovaGraphics/Plugin/PluginConfig.hpp"
#include "../NovaInput/GamepadInput.hpp"
#include "../NovaInput/KeyBindings.hpp"
#include "../NovaLua/LuaEngine.hpp"
#include "HexEditor.hpp"

struct SDL_Window;
union SDL_Event;

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
    int id = 0;
    SimNodeKind kind = SimNodeKind::N64Console;

    bool showConfig = false;
    bool isPowered  = false;

    // New nodes are placed once, the first time they are drawn in the editor.
    bool  placed = false;
    float spawnX = 0.0f;
    float spawnY = 0.0f;

    int  ramMB           = 4;
    bool hasExpansionPak = false;
    bool has64DD         = false;
    char cartName[128]   = {};
    char cartPath[512]   = {};
    char ddCartName[128] = {};
    char ddCartPath[512] = {};
};

struct SimLink {
    int id;
    int fromPin;
    int toPin;
};

/// Launcher options that aren't graphics settings (settings/launcher.ini).
struct LaunchOptions {
    int  speed            = 60;    ///< NTSC-equivalent: 30, 60, 120, or 0 = unlimited
    int  displayMode      = 0;     ///< 0 = play in the launcher window as it is, 1 = go fullscreen while playing
    int  cpuCore          = 0;     ///< 0 = platform default, 1 = interpreter, 2 = recompiler
    bool rewind           = true;
    bool pauseOnFocusLoss = false; ///< pause when another application becomes active
    bool showFps          = false;
    int  volume           = 100;   ///< 0-100
    bool muted            = false;

    void Load(const std::string& path);
    void Save(const std::string& path) const;
};

/// GL textures made from state thumbnails, kept for as long as the thumbnail exists.
class ThumbnailTextures {
public:
    /// The texture for a thumbnail, made on first use; 0 if there is none.
    uint32_t Get(const std::shared_ptr<const Nova64::StateThumbnail>& thumbnail);
    /// Deletes the textures of thumbnails that no longer exist.
    void DropUnused();
    void Clear();

private:
    struct Entry {
        std::weak_ptr<const Nova64::StateThumbnail> source;
        uint32_t texture = 0;
    };
    std::unordered_map<const Nova64::StateThumbnail*, Entry> m_entries;
};

class NovaLauncher {
public:
    NovaLauncher();
    ~NovaLauncher();

    void Run();

private:
    // ---- Layout ----
    void RenderTopNavbar(SDL_Window* window);
    void RenderUI(SDL_Window* window);
    void RenderSidebar();
    void RenderHomeTab();
    void RenderGamesTab();
    void RenderSettingsTab();
    void RenderInputSettingsWidget();
    void RenderControllerSettingsWidget();
    void RenderScriptsTab();
    void RenderHexEditorTab();
    void RenderAchievementsTab();
    void RenderSimulatorTab();
    void UpdateWindowAnimations(SDL_Window* window);

    // ---- Input ----
    /// Captures the next key press while a binding is being changed.
    /// Returns true when the event was consumed.
    bool HandleRebindEvent(const SDL_Event& event);
    /// The same for a game controller input being assigned.
    bool HandlePadRebindEvent(const SDL_Event& event);

    // ---- Library ----
    void LoadLibrary();
    void SaveLibrary() const;
    void AddToLibrary(const std::string& path, const std::string& name, const std::string& notes = {});
    void OpenRomWithPicker();

    // ---- Settings ----
    void LoadSettings();
    void SaveGraphicsSettings();
    void SaveLaunchOptions();
    void SaveControllerSettings();
    /// Pushes the options that take effect immediately (volume, rewind) to the emulator.
    void ApplyLaunchOptions();
    void SetTurbo(bool enabled);

    // ---- Playing a game inside the launcher (LauncherPlay.cpp) ----
    void PlayRom(const std::string& path, const std::string& name);
    void LaunchEmulator(const SimNode& node);
    void StopEmbedded();
    /// Per-frame bookkeeping: a game that ended, a pending resume, new messages.
    void UpdatePlayState();
    /// Hotkeys while a game runs. Returns true when the key was used.
    bool HandleGameKey(int scancode, bool repeat);
    /// Controller buttons the launcher uses while a game runs (Back opens the
    /// pause menu). Returns true when the button was used.
    bool HandleGamePadButton(int button);
    void OpenPauseMenu();
    void ClosePauseMenu();
    /// Un-pauses once no key is held, so the key that closed a menu (Enter is
    /// Start by default) doesn't reach the game as well.
    void ResumeGame();
    void OpenRewind();
    void CloseRewind(bool jumpToSelection);
    void MoveRewindSelection(int delta);
    void SyncStateLists();
    void ShowStatus(const std::string& text);
    void UploadFromPixelBuf();
    void RenderGameView();
    void RenderPauseMenu();
    void RenderSaveSlots();
    void RenderRewindBar();

    // ---- Tools ----
    void RefreshScriptFiles();

    // ---- Simulator ----
    void SpawnNode(SimNodeKind kind, int cartIndex = -1);
    void RenderSimNode(SimNode& node);
    static const char* NodeIcon(SimNodeKind k);
    static const char* NodeLabel(SimNodeKind k);
    static bool IsN64DDDisk(const std::string& path);
    static int  GetPinId(int nodeId, int pinIndex) { return (nodeId << 4) | pinIndex; }
    static int  GetNodeIdFromPin(int pinId)        { return pinId >> 4; }
    static bool IsInputPin(int pinId)              { return (pinId & 0xF) > 0; }

    // ---- State ----
    bool        m_shouldExit = false;
    Tab         m_activeTab  = Tab::Home;
    SDL_Window* m_window     = nullptr;
    void*       m_glContext  = nullptr;   ///< the launcher's SDL_GLContext, while Run() is active

    std::vector<RomMetadata> m_library;
    bool m_showAddModal    = false;
    bool m_showEditModal   = false;
    int  m_editingRomIndex = -1;
    char m_editPathBuffer[512]   = {};
    char m_editNameBuffer[128]   = {};
    char m_editNotesBuffer[1024] = {};
    std::string m_modalError;

    Nova64::Video::PluginConfig m_gfx;
    LaunchOptions               m_launch;
    NovaInput::KeyBindings      m_bindings;
    int                         m_rebindTarget = -1;   ///< N64Input index being changed, or -1
    NovaInput::ControllerSettings m_padSettings;
    int                         m_padRebindTarget = -1;   ///< N64Input index waiting for a controller input, or -1

    int m_nextNodeId = 10;
    int m_nextLinkId = 100;
    std::vector<SimNode> m_simNodes;
    std::vector<SimLink> m_simLinks;

    float m_windowOpacity = 0.0f;
    enum class AnimState { FadeIn, Stable, FadeOut, MinimizeOut } m_animState = AnimState::FadeIn;

    // ---- The game running inside the launcher ----
    std::thread       m_emuThread;
    std::atomic<bool> m_emuRunning{false};
    std::atomic<bool> m_emuStartFailed{false};
    std::string       m_emuRomPath;
    std::string       m_emuTitle;
    uint32_t          m_gameTex  = 0;
    int               m_gameTexW = 0;
    int               m_gameTexH = 0;
    bool              m_turboMode      = false;
    bool              m_resumePending  = false;
    bool              m_wentFullscreen = false;

    bool m_showPauseMenu  = false;
    bool m_focusPauseMenu = false;
    int  m_overwriteSlot  = -1;      ///< slot waiting for "replace?" confirmation
    bool m_openOverwrite  = false;

    // The rewind bar lists m_rewindPoints followed by a "Now" entry.
    bool                             m_rewindOpen              = false;
    bool                             m_rewindFromPauseMenu     = false;
    bool                             m_rewindScrollToSelection = false;
    int                              m_rewindSelection         = 0;
    float                            m_rewindWheel             = 0.0f;
    std::vector<Nova64::RewindPoint> m_rewindPoints;
    uint64_t                         m_stateRevision = 0;
    ThumbnailTextures                m_thumbnails;

    // Frame rate shown in the top bar, measured in video fields per second.
    float    m_fps          = 0.0f;
    uint64_t m_fpsLastField = 0;
    uint32_t m_fpsLastTick  = 0;

    std::string m_statusText;
    uint32_t    m_statusUntil    = 0;   ///< SDL tick count
    uint64_t    m_statusSequence = 0;

    LuaEngine                m_lua;
    std::vector<std::string> m_scriptFiles;
    size_t                   m_shownLogLines = 0;
    HexEditor                m_hexEditor;

    // ---- Unattended runs (NOVA64_UI_*, see the constructor) ----
    std::string                      m_shotPath;
    std::vector<int>                 m_shotFrames;
    std::string                      m_forcedSubTab;
    std::string                      m_autoRom;
    std::vector<std::pair<int, int>> m_autoKeys;   ///< launcher frame, SDL scancode
    int                              m_frameCount = 0;
};

} // namespace NovaUI
