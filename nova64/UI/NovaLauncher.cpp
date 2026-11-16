/**
 * @file NovaLauncher.cpp
 * @brief Launcher window: main loop, window chrome and the Home tab.
 */
#include "NovaLauncher.hpp"
#include "LauncherInternal.hpp"
#include "NovaTheme.hpp"
#include "AppIcon.hpp"

#include <SDL.h>
#include <glad/gl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>
#include <imgui_node_editor.h>

#include "../NovaGraphics/Texture/ImageWrite.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace ed = ax::NodeEditor;
namespace fs = std::filesystem;

namespace NovaUI {

ed::EditorContext* g_NodeEditor = nullptr;

namespace {

// Uploads the embedded icon as a GL texture (0 if it couldn't be decoded).
GLuint UploadIconTexture() {
    const IconImage& icon = AppIconImage();
    if (icon.width == 0) return 0;
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, icon.width, icon.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, icon.rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

constexpr float kNavbarHeight = 40.0f;
constexpr float kWindowButton = 34.0f;

// The launcher is borderless, so it provides its own drag area (the top bar,
// clear of the File menu and window buttons) and resize edges.
extern "C" SDL_HitTestResult SDLCALL LauncherHitTest(SDL_Window* win, const SDL_Point* area, void*) {
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);

    if (!(SDL_GetWindowFlags(win) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
        constexpr int kEdge = 6;
        const bool left = area->x < kEdge, right = area->x >= w - kEdge;
        const bool top = area->y < kEdge, bottom = area->y >= h - kEdge;
        if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (left) return SDL_HITTEST_RESIZE_LEFT;
        if (right) return SDL_HITTEST_RESIZE_RIGHT;
        if (top) return SDL_HITTEST_RESIZE_TOP;
        if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
    }

    constexpr int kLeftReserve = 80;
    constexpr int kRightReserve = 140;
    if (area->y < kNavbarHeight && area->x > kLeftReserve && area->x < w - kRightReserve) return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}

Tab TabFromName(const std::string& name) {
    if (name == "games") return Tab::Games;
    if (name == "settings") return Tab::Settings;
    if (name == "scripts") return Tab::Scripts;
    if (name == "memory") return Tab::HexEditor;
    if (name == "achievements") return Tab::Achievements;
    if (name == "simulator") return Tab::Simulator;
    return Tab::Home;
}

} // namespace

// --- Construction ---
NovaLauncher::NovaLauncher() {
    // Unattended runs, for checking the launcher without a person at the keyboard:
    //   NOVA64_UI_SHOT=/tmp/ui.png NOVA64_UI_TAB=settings/controls
    //       render one tab, save a PNG after 20 frames and exit
    //   NOVA64_UI_ROM=roms/game.z64
    //       start a game as soon as the window is up
    //   NOVA64_UI_KEYS=600:Backspace,640:Left,700:Return
    //       press keys at the given launcher frames
    //   NOVA64_UI_OPEN=add | edit:<index>
    //       open the Add Game or Edit Game dialog as soon as the launcher starts
    //   NOVA64_UI_SHOT_AT=580,660,900
    //       with NOVA64_UI_SHOT, save <name>_<frame>.png at each frame and exit after the last
    if (const char* shot = std::getenv("NOVA64_UI_SHOT")) m_shotPath = shot;
    if (const char* tab = std::getenv("NOVA64_UI_TAB")) {
        std::string t(tab);
        const auto slash = t.find('/');
        m_activeTab = TabFromName(t.substr(0, slash));
        if (slash != std::string::npos) m_forcedSubTab = t.substr(slash + 1);
    }
    if (const char* rom = std::getenv("NOVA64_UI_ROM")) m_autoRom = rom;
    if (const char* frames = std::getenv("NOVA64_UI_SHOT_AT")) {
        for (const char* p = frames; *p;) {
            char* end = nullptr;
            const long frame = std::strtol(p, &end, 10);
            if (end == p) break;
            m_shotFrames.push_back(static_cast<int>(frame));
            p = (*end == ',') ? end + 1 : end;
        }
        std::sort(m_shotFrames.begin(), m_shotFrames.end());
    }
    if (const char* keys = std::getenv("NOVA64_UI_KEYS")) {
        const std::string list(keys);
        for (size_t start = 0; start < list.size();) {
            size_t comma = list.find(',', start);
            if (comma == std::string::npos) comma = list.size();
            const std::string item = list.substr(start, comma - start);
            const size_t colon = item.find(':');
            if (colon != std::string::npos) {
                const SDL_Scancode key = SDL_GetScancodeFromName(item.substr(colon + 1).c_str());
                if (key != SDL_SCANCODE_UNKNOWN)
                    m_autoKeys.emplace_back(std::atoi(item.substr(0, colon).c_str()), static_cast<int>(key));
            }
            start = comma + 1;
        }
    }

    LoadSettings();
    LoadLibrary();

    //   NOVA64_UI_OPEN=add | edit:<index>   open the Add Game or Edit Game dialog on start
    if (const char* open = std::getenv("NOVA64_UI_OPEN")) {
        if (std::strcmp(open, "add") == 0) OpenAddDialog();
        else if (std::strncmp(open, "edit:", 5) == 0) OpenEditDialog(std::atoi(open + 5));
    }
}

NovaLauncher::~NovaLauncher() { StopEmbedded(); }

// --- Window animation ---
void NovaLauncher::UpdateWindowAnimations(SDL_Window* window) {
    constexpr float kStep = 0.08f;
    switch (m_animState) {
    case AnimState::FadeIn:
        m_windowOpacity = std::min(1.0f, m_windowOpacity + kStep);
        if (m_windowOpacity >= 1.0f) m_animState = AnimState::Stable;
        SDL_SetWindowOpacity(window, m_windowOpacity);
        break;
    case AnimState::FadeOut:
        m_windowOpacity = std::max(0.0f, m_windowOpacity - kStep);
        if (m_windowOpacity <= 0.0f) m_shouldExit = true;
        SDL_SetWindowOpacity(window, m_windowOpacity);
        break;
    case AnimState::MinimizeOut:
        m_windowOpacity -= kStep * 1.5f;
        if (m_windowOpacity <= 0.0f) {
            SDL_MinimizeWindow(window);
            m_animState = AnimState::Stable;
            m_windowOpacity = 1.0f;
        }
        SDL_SetWindowOpacity(window, std::max(0.0f, m_windowOpacity));
        break;
    case AnimState::Stable: break;
    }
}

// --- Main loop ---
void NovaLauncher::Run() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return;
    NovaInput::Gamepads::Instance().Init();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);

    SDL_Window* window =
        SDL_CreateWindow("NovaStar64", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 768,
                         SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        SDL_Quit();
        return;
    }
    SDL_SetWindowMinimumSize(window, 800, 560);
    SDL_SetWindowHitTest(window, LauncherHitTest, nullptr);
    ApplyAppIcon(window);

    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, glContext);
    SDL_GL_SetSwapInterval(1);
    m_window = window;
    m_glContext = glContext;

    const bool capturing = !m_shotPath.empty();
    if (capturing) {
        m_windowOpacity = 1.0f;
        m_animState = AnimState::Stable;
    }
    SDL_SetWindowOpacity(window, m_windowOpacity);

    std::error_code ec;
    fs::create_directories("settings", ec);
    fs::create_directories("scripts", ec);
    fs::create_directories("saves", ec);

    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress))) {
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        m_window = nullptr;
        m_glContext = nullptr;
        SDL_Quit();
        return;
    }

    m_iconTex = UploadIconTexture();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = capturing ? nullptr : "settings/imgui.ini";
    ApplyWin95Theme();

    // Rasterize the font at the display's pixel density; otherwise text is
    // rendered at 1x and scaled up, which is blurry on Retina displays.
    int winW = 0, winH = 0, drawW = 0, drawH = 0;
    SDL_GetWindowSize(window, &winW, &winH);
    SDL_GL_GetDrawableSize(window, &drawW, &drawH);
    const float dpiScale = winW > 0 ? static_cast<float>(drawW) / static_cast<float>(winW) : 1.0f;
    if (dpiScale > 1.0f) {
        ImFontConfig fontConfig;
        fontConfig.SizePixels = 13.0f * dpiScale;
        io.Fonts->AddFontDefault(&fontConfig);
        io.FontGlobalScale = 1.0f / dpiScale;
    }

    ImGui_ImplSDL2_InitForOpenGL(window, glContext);
    ImGui_ImplOpenGL3_Init("#version 330");

    ed::Config config;
    config.SettingsFile = capturing ? nullptr : "settings/NodeEditor.json";
    g_NodeEditor = ed::CreateEditor(&config);
    ed::SetCurrentEditor(g_NodeEditor);
    ApplyNodeEditorTheme();
    ed::SetCurrentEditor(nullptr);

    RefreshScriptFiles();

    const int lastShotFrame = m_shotFrames.empty() ? 20 : m_shotFrames.back();
    while (!m_shouldExit) {
        // A scripted key is pressed on its frame and released two frames later.
        for (const auto& [frame, scancode] : m_autoKeys) {
            if (frame != m_frameCount && frame + 2 != m_frameCount) continue;
            SDL_Event key{};
            const bool down = (frame == m_frameCount);
            key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
            key.key.state = down ? SDL_PRESSED : SDL_RELEASED;
            key.key.windowID = SDL_GetWindowID(window);
            key.key.keysym.scancode = static_cast<SDL_Scancode>(scancode);
            key.key.keysym.sym = SDL_GetKeyFromScancode(key.key.keysym.scancode);
            SDL_PushEvent(&key);
        }
        if (m_frameCount == 1 && !m_autoRom.empty()) {
            std::string title;
            detail::ReadN64RomHeader(m_autoRom, &title);
            PlayRom(m_autoRom, title);
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            NovaInput::Gamepads::Instance().HandleEvent(event);
            if (HandlePadRebindEvent(event)) continue;
            if (HandleRebindEvent(event)) continue;
            if (event.type == SDL_KEYDOWN && m_emuRunning.load() &&
                HandleGameKey(event.key.keysym.scancode, event.key.repeat != 0))
                continue;
            if (event.type == SDL_CONTROLLERBUTTONDOWN && m_emuRunning.load() &&
                HandleGamePadButton(event.cbutton.button))
                continue;
            ImGui_ImplSDL2_ProcessEvent(&event);

            if (event.type == SDL_QUIT) m_shouldExit = true;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST &&
                m_launch.pauseOnFocusLoss && m_emuRunning.load() && !m_showPauseMenu && !m_rewindOpen)
                OpenPauseMenu();
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window))
                m_shouldExit = true;

            // Dropping a ROM adds it to the library and starts it.
            if (event.type == SDL_DROPFILE && event.drop.file) {
                const std::string dropped(event.drop.file);
                SDL_free(event.drop.file);
                std::string title;
                if (IsN64DDDisk(dropped) || detail::ReadN64RomHeader(dropped, &title)) {
                    AddToLibrary(dropped, title);
                    if (!m_emuRunning.load())
                        PlayRom(dropped, title.empty() ? fs::path(dropped).stem().string() : title);
                }
            }
        }

        UpdatePlayState();
        UpdateWindowAnimations(window);

        SDL_GL_MakeCurrent(window, glContext);
        // A controller moves through the launcher's menus, but not while the
        // game itself is reading it or a controller input is being assigned.
        const bool padNavigation = (!m_emuRunning.load() || m_showPauseMenu || m_rewindOpen) && m_padRebindTarget < 0;
        if (padNavigation) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        if (m_emuRunning.load()) {
            RenderGameView();
            RenderPauseMenu();
            RenderRewindBar();
            // The top bar needs a window of its own here: drawn outside any
            // window, ImGui puts it in a stray "Debug" window.
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, kNavbarHeight));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("##GameBar", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBackground);
            RenderTopNavbar(window);
            ImGui::End();
            ImGui::PopStyleVar();
        } else {
            RenderUI(window);
        }

        ImGui::Render();
        SDL_GL_GetDrawableSize(window, &drawW, &drawH);
        glViewport(0, 0, drawW, drawH);
        glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        const bool shotNow = capturing && (m_shotFrames.empty() ? m_frameCount == lastShotFrame
                                                                : std::binary_search(m_shotFrames.begin(),
                                                                                     m_shotFrames.end(), m_frameCount));
        if (shotNow) {
            const size_t row = static_cast<size_t>(drawW) * 4;
            std::vector<uint8_t> pixels(row * static_cast<size_t>(drawH));
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, drawW, drawH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            std::vector<uint8_t> image(pixels.size());
            for (int y = 0; y < drawH; ++y) // GL rows are bottom-up
                std::memcpy(&image[static_cast<size_t>(y) * row], &pixels[static_cast<size_t>(drawH - 1 - y) * row],
                            row);
            for (size_t i = 3; i < image.size(); i += 4) image[i] = 255;

            fs::path file(m_shotPath);
            if (!m_shotFrames.empty())
                file.replace_filename(file.stem().string() + "_" + std::to_string(m_frameCount) +
                                      file.extension().string());
            Nova::WritePNG(file.string().c_str(), drawW, drawH, 4, image.data());
        }
        if (capturing && m_frameCount >= lastShotFrame) m_shouldExit = true;
        ++m_frameCount;

        SDL_GL_SwapWindow(window);
    }

    StopEmbedded();

    ed::DestroyEditor(g_NodeEditor);
    g_NodeEditor = nullptr;
    ReleaseCoverTextures();
    if (m_iconTex) {
        const GLuint icon = m_iconTex;
        glDeleteTextures(1, &icon);
        m_iconTex = 0;
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    m_glContext = nullptr;
    m_window = nullptr;
    NovaInput::Gamepads::Instance().Shutdown();
    SDL_Quit();
}

// --- Layout ---
void NovaLauncher::RenderTopNavbar(SDL_Window* window) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.85f, 0.43f, 0.26f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0.85f, 0.43f, 0.26f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    // Taller frame padding makes the menu bar fill the whole strip instead of
    // its top half, which also centres the menu and buttons vertically.
    const ImVec2 normalPadding = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(normalPadding.x, (kNavbarHeight - ImGui::GetFontSize()) * 0.5f));
    ImGui::BeginChild("TopNavbar", ImVec2(0, kNavbarHeight), ImGuiChildFlags_None,
                      ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar);
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, normalPadding);
            // The bar pushes white text, which the menu popup would inherit.
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
            if (ImGui::MenuItem("Open ROM...")) OpenRomWithPicker();
            if (m_emuRunning.load() && ImGui::MenuItem("Stop Game")) StopEmbedded();
            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) m_animState = AnimState::FadeOut;
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }

        if (m_emuRunning.load()) {
            ImGui::Text("  %s%s", m_emuTitle.c_str(), m_turboMode ? "  [2x]" : "");
            if (m_launch.showFps && m_fps > 0.0f) {
                ImGui::SameLine();
                ImGui::Text("  %.0f fps", m_fps);
            }
            if (!m_showPauseMenu && !m_rewindOpen) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.75f), "   Esc: menu   Backspace: rewind");
            }
        }
        if (!m_statusText.empty() && SDL_GetTicks() < m_statusUntil) {
            ImGui::SameLine();
            ImGui::Text("   %s", m_statusText.c_str());
        }

        // Right-align the window buttons, counting the spacing ImGui puts
        // between them so the last one doesn't spill past the edge.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float buttonsWidth = kWindowButton * 3.0f + style.ItemSpacing.x * 2.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - buttonsWidth - style.WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.2f));
        if (ImGui::Button("_##min", ImVec2(kWindowButton, 0))) m_animState = AnimState::MinimizeOut;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Minimize");
        ImGui::SameLine();
        const bool maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
        if (ImGui::Button(maximized ? "[=]##max" : "[ ]##max", ImVec2(kWindowButton, 0))) {
            if (maximized) SDL_RestoreWindow(window);
            else SDL_MaximizeWindow(window);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(maximized ? "Restore" : "Maximize");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.1f, 0.1f, 0.8f));
        if (ImGui::Button("X##close", ImVec2(kWindowButton, 0))) m_animState = AnimState::FadeOut;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Quit");
        ImGui::PopStyleColor(3);
        ImGui::EndMenuBar();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

void NovaLauncher::RenderUI(SDL_Window* window) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("MainLayout", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    RenderTopNavbar(window);

    ImGui::Columns(2, "LayoutColumns", false);
    ImGui::SetColumnWidth(0, 190.0f);
    ImGui::BeginChild("Sidebar", ImVec2(0, 0), ImGuiChildFlags_Borders);
    RenderSidebar();
    ImGui::EndChild();
    ImGui::NextColumn();
    ImGui::BeginChild("Content", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    switch (m_activeTab) {
    case Tab::Home:         RenderHomeTab(); break;
    case Tab::Games:        RenderGamesTab(); break;
    case Tab::Settings:     RenderSettingsTab(); break;
    case Tab::Scripts:      RenderScriptsTab(); break;
    case Tab::HexEditor:    RenderHexEditorTab(); break;
    case Tab::Achievements: RenderAchievementsTab(); break;
    case Tab::Simulator:    RenderSimulatorTab(); break;
    }
    ImGui::EndChild();
    ImGui::Columns(1);
    ImGui::End();
}

void NovaLauncher::RenderSidebar() {
    if (m_iconTex) {
        constexpr float kIcon = 26.0f;
        ImGui::Image(static_cast<ImTextureID>(m_iconTex), ImVec2(kIcon, kIcon));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
    }
    ImGui::TextDisabled("NOVASTAR64");
    ImGui::Spacing();
    ImGui::Spacing();

    struct Entry {
        const char* label;
        Tab tab;
    };
    static constexpr Entry kEntries[] = {
        {"Home", Tab::Home},
        {"Games", Tab::Games},
        {"Settings", Tab::Settings},
        {"Scripts", Tab::Scripts},
        {"Memory Viewer", Tab::HexEditor},
        {"Achievements", Tab::Achievements},
        {"Simulator", Tab::Simulator},
    };
    for (const auto& e : kEntries) {
        if (ImGui::Selectable(e.label, m_activeTab == e.tab)) m_activeTab = e.tab;
    }
}

void NovaLauncher::RenderHomeTab() {
    if (m_iconTex) {
        constexpr float kIcon = 60.0f;
        ImGui::Image(static_cast<ImTextureID>(m_iconTex), ImVec2(kIcon, kIcon));
        ImGui::SameLine();
    }
    ImGui::BeginGroup();
    ImGui::SetWindowFontScale(1.5f);
    ImGui::Text("NovaStar64");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("Nintendo 64 emulator");
    ImGui::EndGroup();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Open ROM...", ImVec2(160, 36))) OpenRomWithPicker();
    ImGui::SameLine();
    if (ImGui::Button("Browse Games", ImVec2(160, 36))) m_activeTab = Tab::Games;
    ImGui::TextDisabled("You can also drag a ROM file onto this window.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const int games = static_cast<int>(m_library.size());
    ImGui::Text("%d game%s in your library", games, games == 1 ? "" : "s");

    ImGui::Spacing();
    ImGui::TextDisabled("While playing:");
    ImGui::BulletText("Esc opens the pause menu, with save states.");
    ImGui::BulletText("Backspace rewinds: pick a moment from the pictures along the bottom.");
    ImGui::BulletText("Tab switches to double speed and back.");
}

} // namespace NovaUI
