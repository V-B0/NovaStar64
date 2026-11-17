#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <imgui.h>

#include "../N64SYS/SaveState.hpp"
#include "../NovaGraphics/Plugin/PluginConfig.hpp"
#include "../NovaInput/GamepadInput.hpp"
#include "../NovaInput/KeyBindings.hpp"
#include "../NovaLua/LuaEngine.hpp"
#include "GameSetup.hpp"
#include "HexEditor.hpp"
#include "RomInfo.hpp"
#include "SimLab.hpp"

struct SDL_Window;
union SDL_Event;

namespace NovaUI {

enum class Tab { Home, Library, Recent, Favorites, Settings, About, Scripts, HexEditor, Achievements, Simulator };

struct RomMetadata {
    std::string path;
    std::string name;
    std::string notes;
    std::string
        cover; ///< picture shown on the game's card; a path relative to the data folder ("covers/..."), or empty
    bool favorite = false;
    int64_t lastPlayed = 0; ///< seconds since 1970, or 0 if never started from the launcher
    int plays = 0;
};

/// Launcher options that aren't graphics settings (settings/launcher.ini).
struct LaunchOptions {
    int speed = 60;      ///< NTSC-equivalent: 30, 60, 120, or 0 = unlimited
    int displayMode = 0; ///< 0 = play in the launcher window as it is, 1 = go fullscreen while playing
    int cpuCore = 0;     ///< 0 = platform default, 1 = interpreter, 2 = recompiler
    int rspEngine =
        0; ///< RSP microcode: 0 = recompiler (faster; the interpreter where there isn't one), 1 = interpreter
    bool rewind = true;
    bool pauseOnFocusLoss = false; ///< pause when another application becomes active
    bool showFps = false;
    int rumbleShake = 25;          ///< how far the window shakes when a game rumbles, 0-100 (0 = not at all)
    int uiScale = 100;             ///< the size of the whole interface, in percent
    int volume = 100;              ///< 0-100
    bool muted = false;

    void Load(const std::string& path);
    /// Returns false if the file could not be written.
    bool Save(const std::string& path) const;
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
    /// The bar across the top: the brand and status in the launcher, the game's title and hints while playing.
    void RenderTitleBar(SDL_Window* window, bool inGame);
    void RenderUI(SDL_Window* window);
    void RenderSidebar();
    void RenderHintBar();
    void RenderHomeTab();
    /// Returns true when the game was removed from the library while it was drawn.
    bool RenderHeroCard(int index);
    void RenderEmptyHero();
    /// The library as a grid of covers: all games, the recently played ones, or the favourites.
    void RenderLibraryPage(Tab tab);
    void RenderGameDialogs();
    void RenderAboutTab();
    void RenderSettingsTab();
    /// One option as a row. Returns true when its value changed.
    bool RenderOptionRow(const OptionDef& option, const SettingsModel& model);
    void RenderInputSettingsWidget();
    void RenderControllerSettingsWidget();
    void RenderScriptsTab();
    void RenderHexEditorTab();
    void RenderAchievementsTab();
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
    void OpenAddDialog();
    void OpenEditDialog(int index);
    void AddToLibrary(const std::string& path, const std::string& name, const std::string& notes = {},
                      const std::string& cover = {});
    void OpenRomWithPicker();
    /// Counts a start of the game: moves it to the top of "Recently Played" and selects it.
    void RecordPlay(const std::string& path);
    /// Saves the last frame of the game that was running as the picture shown on its Home card.
    void SaveGameShot();
    /// Indices into m_library, most recently played first (only games that were played).
    std::vector<int> RecentGames() const;
    int FindGame(const std::string& path) const;
    void RemoveGame(int index);

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
    void StopEmbedded();
    /// Per-frame bookkeeping: a game that ended, a pending resume, new messages.
    void UpdatePlayState();
    /// Shakes the window while a game rumbles (or the game picture when the window cannot move).
    void UpdateShake(SDL_Window* window);
    /// Stops the shaking and puts the window back where it was. Called before the window changes mode.
    void EndShake(SDL_Window* window);
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

    // ---- Simulator: one game's setup ----
    void RenderSimulatorTab();
    /// Opens the Simulator on a game, to see how it will run and what is different about it.
    void OpenSimulator(int gameIndex);
    static bool IsN64DDDisk(const std::string& path);

    // ---- State ----
    bool m_shouldExit = false;
    Tab m_activeTab = Tab::Home;
    SDL_Window* m_window = nullptr;
    void* m_glContext = nullptr; ///< the launcher's SDL_GLContext, while Run() is active

    std::vector<RomMetadata> m_library;
    std::vector<std::string> m_removedPaths; ///< games in roms/ that were removed from the library: not added again
    /// Whether a file is there, looked at again every two seconds (a stat per card per frame is too many).
    bool FileExistsCached(const std::string& path);
    std::unordered_map<std::string, std::pair<bool, double>> m_existsCache;
    int m_coverLoadsThisFrame = 0; ///< pictures loaded so far this frame; a few per frame keeps a big library smooth
    bool m_showAddModal = false;
    bool m_showEditModal = false;
    int m_editingRomIndex = -1;
    int m_selectedGame = -1;      ///< index into m_library shown on the Home card, or -1 for the most recent one
    char m_searchBuffer[96] = {}; ///< the filter on the library pages
    int m_unusedSaveChip = 0;     ///< the settings model needs somewhere to point for the per-game-only save chip
    int m_settingsPage = 0;       ///< the Settings tab: 0 Emulation, 1 Video, 2 Audio, 3 Controls
    char m_editPathBuffer[512] = {};
    char m_editNameBuffer[128] = {};
    char m_editNotesBuffer[1024] = {};
    char m_editCoverBuffer[512] = {};
    std::string m_modalError;

    /// A cover picture uploaded to the GPU, cached by file path.
    struct CoverTexture {
        uint32_t texture = 0;
        int width = 0;
        int height = 0;
        bool failed = false; ///< the file couldn't be read; tried again after a few seconds
        double failedAt = 0.0;
    };
    std::unordered_map<std::string, CoverTexture> m_coverTextures;
    /// The texture for a cover picture, loading it on first use; null if there is none.
    const CoverTexture* GetCoverTexture(const std::string& path);
    /// Forget a cached picture (its file was replaced) or all of them (before the GL context goes).
    void ForgetCoverTexture(const std::string& path);
    void ReleaseCoverTextures();
    /// Draws a game's cover, or a placeholder, as one item of the given size. Returns true if clicked.
    bool DrawCover(const std::string& coverPath, const std::string& title, ImVec2 size, bool selected = false,
                   float rounding = 14.0f);

    Nova64::Video::PluginConfig m_gfx;
    LaunchOptions m_launch;
    LaunchOptions m_running;     ///< m_launch with the running game's own setup applied
    NovaInput::KeyBindings m_bindings;
    int m_rebindTarget = -1;     ///< N64Input index being changed, or -1
    NovaInput::ControllerSettings m_padSettings;
    int m_padRebindTarget = -1;  ///< N64Input index waiting for a controller input, or -1

    int m_simGame = -1;          ///< the game shown in the Simulator (index into m_library)
    sim::Lab m_simLab;           ///< the workbench of that game
    sim::View m_simView;
    int m_simLoaded = -1;        ///< the game m_simSetup and m_simRom were read for
    bool m_simRomExists = false; ///< whether that game's file is there, looked at about once a second
    GameSetup m_simSetup;        ///< that game's overrides
    detail::RomInfo m_simRom;    ///< what its ROM header says

    float m_windowOpacity = 0.0f;
    enum class AnimState { FadeIn, Stable, FadeOut, MinimizeOut } m_animState = AnimState::FadeIn;

    // ---- The game running inside the launcher ----
    std::thread m_emuThread;
    std::atomic<bool> m_emuRunning{false};
    std::atomic<bool> m_emuStartFailed{false};
    std::string m_emuRomPath;
    std::string m_emuTitle;
    uint32_t m_iconTex = 0; ///< the NovaStar icon, drawn in the title bar
    uint32_t m_gameTex = 0;
    int m_gameTexW = 0;
    int m_gameTexH = 0;
    bool m_turboMode = false;
    bool m_resumePending = false;
    bool m_wentFullscreen = false;

    bool m_showPauseMenu = false;
    bool m_focusPauseMenu = false;
    int m_overwriteSlot = -1; ///< slot waiting for "replace?" confirmation
    bool m_openOverwrite = false;

    // The rewind bar lists m_rewindPoints followed by a "Now" entry.
    bool m_rewindOpen = false;
    bool m_rewindFromPauseMenu = false;
    bool m_rewindScrollToSelection = false;
    int m_rewindSelection = 0;
    float m_rewindWheel = 0.0f;
    std::vector<Nova64::RewindPoint> m_rewindPoints;
    uint64_t m_stateRevision = 0;
    ThumbnailTextures m_thumbnails;

    // Frame rate shown in the top bar, measured in video fields per second.
    float m_fps = 0.0f;
    uint64_t m_fpsLastField = 0;
    uint32_t m_fpsLastTick = 0;

    std::string m_statusText;
    uint32_t m_statusUntil = 0; ///< SDL tick count
    uint64_t m_statusSequence = 0;

    LuaEngine m_lua;
    std::vector<std::string> m_scriptFiles;
    size_t m_shownLogLines = 0;
    HexEditor m_hexEditor;

    // ---- Unattended runs (NOVA64_UI_*, see the constructor) ----
    std::string m_shotPath;
    std::vector<int> m_shotFrames;
    std::string m_forcedSubTab;
    std::string m_autoRom;
    std::vector<std::pair<int, int>> m_autoKeys;  ///< launcher frame, SDL scancode
    std::vector<std::pair<int, int>> m_autoSaves; ///< launcher frame, save slot (0-3)
    struct AutoMouse {
        int frame;
        float x, y;
        char action; ///< 'm' move, 'd' button down, 'u' button up
    };
    std::vector<AutoMouse> m_autoMouse;
    int m_frameCount = 0;
    std::vector<std::pair<int, int>>
        m_autoRumble;            ///< launcher frame, how many frames the motor runs (NOVA64_UI_RUMBLE)

    float m_appliedScale = 1.0f; ///< the interface scale the fonts were made for (1 = 100%)

    // ---- Rumble shake ----
    bool m_shaking = false;
    int m_shakeBaseX = 0, m_shakeBaseY = 0; ///< where the window was when the shaking began
    int m_shakeLastX = 0, m_shakeLastY = 0; ///< where the last shake put it
    uint32_t m_shakeUntil = 0;              ///< SDL tick at which the shaking has died down
    uint32_t m_shakeSeed = 12345;
    ImVec2 m_shakeOffset{0.0f, 0.0f};       ///< in the interface's units: how far the game picture is moved
};

} // namespace NovaUI
