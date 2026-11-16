/**
 * @file NovaLauncher.cpp
 * @brief Launcher window: main loop, window chrome and the Home tab.
 */
#include "NovaLauncher.hpp"
#include "LauncherInternal.hpp"
#include "NovaTheme.hpp"
#include "AppIcon.hpp"
#include "UiKit.hpp"

#include <SDL.h>
#include <glad/gl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>
#include <imgui_node_editor.h>

#include "../NovaGraphics/Texture/ImageWrite.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace ed = ax::NodeEditor;
namespace fs = std::filesystem;

#ifndef NOVA64_VERSION
#define NOVA64_VERSION "unknown"
#endif

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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, icon.width, icon.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, icon.rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

constexpr float kNavbarHeight = 44.0f;

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
    kit::ApplyTheme();

    // Rasterize the fonts at the display's pixel density; otherwise text is
    // rendered at 1x and scaled up, which is blurry on Retina displays.
    int winW = 0, winH = 0, drawW = 0, drawH = 0;
    SDL_GetWindowSize(window, &winW, &winH);
    SDL_GL_GetDrawableSize(window, &drawW, &drawH);
    const float dpiScale = winW > 0 ? static_cast<float>(drawW) / static_cast<float>(winW) : 1.0f;
    kit::LoadFonts(dpiScale);

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
        kit::OnNewFrame();
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
namespace {
constexpr float kBodyMargin = 12.0f;
} // namespace

void NovaLauncher::RenderTopNavbar(SDL_Window* window) {
    using namespace kit;
    const float p = Px(1.0f);
    const float windowButton = 23.0f * p;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("TopNavbar", ImVec2(0, kNavbarHeight), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // A line under the bar, in the panel colours
    {
        const ImVec2 origin = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(origin.x, origin.y + kNavbarHeight - p),
                                                  ImVec2(origin.x + ImGui::GetWindowWidth(), origin.y + kNavbarHeight),
                                                  ImGui::GetColorU32(color::kBrown));
    }

    // File menu
    ImGui::SetCursorPos(ImVec2(6.0f * p, 3.0f * p));
    if (Button("File", ButtonStyle::Ghost, Icon::None, ImVec2(30.0f * p, kNavbarHeight - 7.0f * p)))
        ImGui::OpenPopup("FileMenu");
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + p));
    if (ImGui::BeginPopup("FileMenu")) {
        if (ImGui::MenuItem("Open ROM...")) OpenRomWithPicker();
        if (m_emuRunning.load() && ImGui::MenuItem("Stop Game")) StopEmbedded();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) m_animState = AnimState::FadeOut;
        ImGui::EndPopup();
    }

    // What is running, and the latest status message
    float x = 46.0f * p;
    auto put = [&](const std::string& text, const ImVec4& col) {
        ImGui::SetCursorPos(ImVec2(x, (kNavbarHeight - ImGui::GetTextLineHeight()) * 0.5f));
        ImGui::TextColored(col, "%s", text.c_str());
        x += ImGui::CalcTextSize(text.c_str()).x + 8.0f * p;
    };
    if (m_emuRunning.load()) {
        put(m_emuTitle + (m_turboMode ? "  [2x]" : ""), color::kText);
        if (m_launch.showFps && m_fps > 0.0f) {
            char fps[32];
            std::snprintf(fps, sizeof(fps), "%.0f fps", m_fps);
            put(fps, color::kTextMuted);
        }
        if (!m_showPauseMenu && !m_rewindOpen) put("Esc: menu   Backspace: rewind", color::kTextMuted);
    }
    if (!m_statusText.empty() && SDL_GetTicks() < m_statusUntil) put(m_statusText, color::kPeach);

    // Window controls
    const ImVec2 buttonSize(windowButton, kNavbarHeight - p);
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - windowButton * 3.0f, 0.0f));
    if (WindowControl("##min", Icon::Minimize, false, "Minimize", buttonSize)) m_animState = AnimState::MinimizeOut;
    ImGui::SameLine(0.0f, 0.0f);
    const bool maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
    if (WindowControl("##max", maximized ? Icon::Restore : Icon::Maximize, false, maximized ? "Restore" : "Maximize",
                      buttonSize)) {
        if (maximized) SDL_RestoreWindow(window);
        else SDL_MaximizeWindow(window);
    }
    ImGui::SameLine(0.0f, 0.0f);
    if (WindowControl("##close", Icon::Close, true, "Quit", buttonSize)) m_animState = AnimState::FadeOut;

    ImGui::EndChild();
}

void NovaLauncher::RenderUI(SDL_Window* window) {
    using namespace kit;
    const float p = Px(1.0f);
    const float sidebarWidth = 104.0f * p;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("MainLayout", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    // Dither along the bottom of the window, behind everything
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    DrawDitherBand(ImGui::GetWindowDrawList(), ImVec2(0.0f, display.y - 40.0f * p), display, 10);

    RenderTopNavbar(window);

    const float bodyHeight = display.y - kNavbarHeight - kBodyMargin;
    ImGui::SetCursorPos(ImVec2(kBodyMargin, kNavbarHeight + p * 3.0f));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("Sidebar", ImVec2(sidebarWidth, bodyHeight - p * 3.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    RenderSidebar();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 6.0f * p);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * p, 12.0f * p));
    ImGui::BeginChild("Content", ImVec2(-kBodyMargin, bodyHeight - p * 3.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    const ImVec2 contentPos = ImGui::GetWindowPos();
    const ImVec2 contentSize = ImGui::GetWindowSize();
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
    // The panel is drawn after its contents are submitted but, as a child draws after its parent, it lies behind them.
    DrawFrame(ImGui::GetWindowDrawList(), contentPos,
              ImVec2(contentPos.x + contentSize.x, contentPos.y + contentSize.y), Frame::Brown,
              ImGui::GetColorU32(color::kPanel));
    ImGui::End();
}

void NovaLauncher::RenderSidebar() {
    using namespace kit;
    const float p = Px(1.0f);

    // The name, with the icon
    const float logo = 16.0f * p;
    const float top = ImGui::GetCursorPosY() + 3.0f * p;
    ImGui::SetCursorPos(ImVec2(5.0f * p, top));
    if (m_iconTex) {
        ImGui::Image(static_cast<ImTextureID>(m_iconTex), ImVec2(logo, logo));
        ImGui::SameLine(0.0f, 4.0f * p);
    }
    ImGui::SetCursorPosY(top + (logo - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextUnformatted("NovaStar64");
    ImGui::SetCursorPos(ImVec2(0.0f, top + logo + 8.0f * p));

    struct Entry {
        const char* label;
        Icon icon;
        Tab tab;
    };
    static constexpr Entry kMain[] = {
        {"Home", Icon::Home, Tab::Home},
        {"Games", Icon::Games, Tab::Games},
        {"Settings", Icon::Settings, Tab::Settings},
    };
    static constexpr Entry kTools[] = {
        {"Scripts", Icon::Scripts, Tab::Scripts},
        {"Memory", Icon::Memory, Tab::HexEditor},
        {"Trophies", Icon::Trophy, Tab::Achievements},
        {"Simulator", Icon::Simulator, Tab::Simulator},
    };
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 2.0f * p));
    for (const auto& e : kMain)
        if (NavItem(e.icon, e.label, m_activeTab == e.tab)) m_activeTab = e.tab;

    ImGui::Dummy(ImVec2(0.0f, 4.0f * p));
    ImGui::SetCursorPosX(5.0f * p);
    SectionLabel("TOOLS");
    for (const auto& e : kTools)
        if (NavItem(e.icon, e.label, m_activeTab == e.tab)) m_activeTab = e.tab;
    ImGui::PopStyleVar();

    ImGui::SetCursorPos(ImVec2(5.0f * p, ImGui::GetWindowHeight() - ImGui::GetTextLineHeight() - 14.0f * p));
    ImGui::PushStyleColor(ImGuiCol_Text, color::kTextDisabled);
    ImGui::Text("V%s", NOVA64_VERSION);
    ImGui::PopStyleColor();
}

void NovaLauncher::RenderHomeTab() {
    using namespace kit;
    const Fonts& fonts = GetFonts();
    const float p = Px(1.0f);

    if (m_iconTex) {
        ImGui::Image(static_cast<ImTextureID>(m_iconTex), ImVec2(32.0f * p, 32.0f * p));
        ImGui::SameLine(0.0f, 8.0f * p);
    }
    ImGui::BeginGroup();
    ImGui::PushFont(fonts.display);
    ImGui::TextUnformatted("NovaStar64");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, color::kTextMuted);
    ImGui::TextUnformatted("Nintendo 64 emulator");
    ImGui::PopStyleColor();
    ImGui::EndGroup();

    ImGui::Dummy(ImVec2(0.0f, 4.0f * p));
    if (Button("Open ROM...", ButtonStyle::Primary, Icon::Folder, ImVec2(0.0f, 22.0f * p))) OpenRomWithPicker();
    ImGui::SameLine();
    if (Button("Browse games", ButtonStyle::Secondary, Icon::Games, ImVec2(0.0f, 22.0f * p))) m_activeTab = Tab::Games;
    HintText("You can also drag a ROM file onto this window.");

    // The library: the first few games, ready to play
    ImGui::Dummy(ImVec2(0.0f, 6.0f * p));
    const int games = static_cast<int>(m_library.size());
    SectionLabel("YOUR LIBRARY");
    if (games == 0) {
        BeginCard("EmptyLibrary", ImVec2(0, 0), true, ImVec2(10.0f * p, 8.0f * p));
        ImGui::TextUnformatted("No games yet.");
        HintText("Open a ROM, or drop a file onto this window, and it appears here.");
        EndCard();
    } else {
        constexpr int kTiles = 4;
        const float gap = 6.0f * p;
        const float avail = ImGui::GetContentRegionAvail().x;
        const float tileWidth = std::floor(std::clamp((avail - gap * (kTiles - 1)) / kTiles, 56.0f * p, 110.0f * p));
        const int shown = std::min(games, kTiles);
        for (int i = 0; i < shown; ++i) {
            const auto& rom = m_library[i];
            if (i > 0) ImGui::SameLine(0.0f, gap);
            ImGui::PushID(i);
            ImGui::BeginGroup();
            std::error_code ec;
            const bool playable = fs::exists(rom.path, ec);
            if (DrawCover(rom.cover, rom.name, tileWidth) && playable) PlayRom(rom.path, rom.name);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tileWidth);
            ImGui::TextWrapped("%s", rom.name.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImGui::PopID();
        }
        if (games > shown) {
            ImGui::Dummy(ImVec2(0.0f, p));
            if (Button("View all games", ButtonStyle::Ghost, Icon::ChevronRight)) m_activeTab = Tab::Games;
        }
    }

    // What the keys do while a game runs
    ImGui::Dummy(ImVec2(0.0f, 8.0f * p));
    SectionLabel("WHILE PLAYING");
    struct Shortcut {
        const char* key;
        const char* does;
    };
    static constexpr Shortcut kShortcuts[] = {
        {"Esc", "Pause menu and save states."},
        {"Backspace", "Rewind to a moment you pick."},
        {"Tab", "Double speed on and off."},
    };
    for (const auto& sc : kShortcuts) {
        const float rowTop = ImGui::GetCursorPosY();
        Keycap(sc.key);
        const float capHeight = ImGui::GetItemRectSize().y;
        ImGui::SameLine(78.0f * p);
        ImGui::SetCursorPosY(rowTop + (capHeight - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::TextUnformatted(sc.does);
        ImGui::SetCursorPosY(rowTop + capHeight + 3.0f * p);
    }
}

} // namespace NovaUI
