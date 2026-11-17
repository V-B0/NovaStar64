/**
 * @file NovaLauncher.cpp
 * @brief Launcher window: main loop, title bar, sidebar, and the Home and About pages.
 */
#include "NovaLauncher.hpp"
#include "LauncherInternal.hpp"
#include "NovaTheme.hpp"
#include "AppIcon.hpp"
#include "GameInfo.hpp"
#include "UiKit.hpp"

#include <SDL.h>
#include <glad/gl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>
#include <imgui_node_editor.h>

#include "../NovaGraphics/Texture/ImageWrite.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, icon.width, icon.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, icon.rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

constexpr float kTitleBarHeight = 76.0f;
constexpr float kSidebarWidth = 248.0f;
constexpr float kHintBarHeight = 56.0f;
constexpr float kPagePadding = 14.0f;
constexpr float kRightMargin = 28.0f;

/// How tall the draggable top bar is right now (it is shorter while a game runs).
std::atomic<int> g_dragHeight{static_cast<int>(kTitleBarHeight)};

// The launcher is borderless, so it provides its own drag area (the top bar,
// clear of the window buttons) and resize edges.
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

    constexpr int kRightReserve = 150;
    if (area->y < g_dragHeight.load() && area->x < w - kRightReserve) return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}

Tab TabFromName(const std::string& name) {
    if (name == "library" || name == "games") return Tab::Library;
    if (name == "recent") return Tab::Recent;
    if (name == "favorites") return Tab::Favorites;
    if (name == "about") return Tab::About;
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
    //       render one tab (home, library, recent, favorites, settings, about, scripts, memory, achievements,
    //       simulator), save a PNG after 20 frames and exit
    //   NOVA64_UI_ROM=roms/game.z64
    //       start a game as soon as the window is up
    //   NOVA64_UI_KEYS=600:Backspace,640:Left,700:Return
    //       press keys at the given launcher frames
    //   NOVA64_UI_SAVE=300:0,600:1
    //       save a state into a slot (0-3) at the given launcher frames
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

    if (const char* saves = std::getenv("NOVA64_UI_SAVE")) {
        const std::string list(saves);
        for (size_t start = 0; start < list.size();) {
            size_t comma = list.find(',', start);
            if (comma == std::string::npos) comma = list.size();
            const std::string item = list.substr(start, comma - start);
            const size_t colon = item.find(':');
            if (colon != std::string::npos)
                m_autoSaves.emplace_back(std::atoi(item.substr(0, colon).c_str()),
                                         std::atoi(item.substr(colon + 1).c_str()));
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
        SDL_CreateWindow("NovaStar64", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                         SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        SDL_Quit();
        return;
    }
    SDL_SetWindowMinimumSize(window, 960, 600);
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
        for (const auto& [frame, slot] : m_autoSaves)
            if (frame == m_frameCount && m_emuRunning.load() && slot >= 0 && slot < Nova64::kSaveSlots)
                Nova64::StateService::Instance().RequestSave(slot);
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
            g_dragHeight.store(static_cast<int>(detail::kGameBarHeight));
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, detail::kGameBarHeight));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("##GameBar", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
            RenderTitleBar(window, true);
            ImGui::End();
            ImGui::PopStyleVar();
        } else {
            RenderUI(window);
        }

        ImGui::Render();
        SDL_GL_GetDrawableSize(window, &drawW, &drawH);
        glViewport(0, 0, drawW, drawH);
        glClearColor(0.031f, 0.027f, 0.094f, 1.0f);
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

const char* BackendName(int type) {
    switch (type) {
    case 0:  return "OpenGL";
    case 1:  return "Vulkan";
    case 2:  return "Direct3D 11";
    default: return "Unknown";
    }
}

ImU32 Col(const ImVec4& c) { return ImGui::GetColorU32(c); }

} // namespace

void NovaLauncher::RenderTitleBar(SDL_Window* window, bool inGame) {
    using namespace kit;
    const Fonts& fonts = GetFonts();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const float width = ImGui::GetWindowWidth();
    const float height = inGame ? detail::kGameBarHeight : kTitleBarHeight;
    const float controlWidth = 46.0f, controlHeight = 36.0f;
    const float controlsLeft = width - 12.0f - controlWidth * 3.0f;

    if (inGame) {
        draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), Col(Rgb(0x080718, 0.96f)));
        draw->AddLine(ImVec2(origin.x, origin.y + height - 1.0f), ImVec2(origin.x + width, origin.y + height - 1.0f),
                      Col(color::kGlassBorder));
    }

    // The brand
    const float logo = inGame ? 28.0f : 46.0f;
    const float logoX = inGame ? 16.0f : 28.0f;
    const float logoY = (height - logo) * 0.5f;
    if (m_iconTex) {
        draw->AddImageRounded(static_cast<ImTextureID>(m_iconTex), ImVec2(origin.x + logoX, origin.y + logoY),
                              ImVec2(origin.x + logoX + logo, origin.y + logoY + logo), ImVec2(0, 0), ImVec2(1, 1),
                              IM_COL32_WHITE, logo * 0.24f);
    }
    float x = logoX + logo + (inGame ? 12.0f : 16.0f);
    if (inGame) {
        const std::string title = Ellipsize(fonts.label, m_emuTitle + (m_turboMode ? "   2x" : ""), width * 0.35f);
        const ImVec2 size = Measure(fonts.label, title.c_str());
        PutText(draw, fonts.label, ImVec2(origin.x + x, origin.y + (height - size.y) * 0.5f), Col(color::kText),
                title.c_str());
        x += size.x + 18.0f;
        auto put = [&](const std::string& text, const ImVec4& col) {
            const ImVec2 sz = Measure(fonts.small, text.c_str());
            PutText(draw, fonts.small, ImVec2(origin.x + x, origin.y + (height - sz.y) * 0.5f), Col(col), text.c_str());
            x += sz.x + 16.0f;
        };
        if (m_launch.showFps && m_fps > 0.0f) {
            char fps[32];
            std::snprintf(fps, sizeof(fps), "%.0f fps", m_fps);
            put(fps, color::kTextMuted);
        }
        if (!m_showPauseMenu && !m_rewindOpen && x + 260.0f < controlsLeft)
            put("Esc: menu     Backspace: rewind     Tab: 2x", color::kTextDim);
    } else {
        const ImVec2 name = Measure(fonts.heading, "NovaStar64");
        const ImVec2 tag = Measure(fonts.small, "Nintendo 64 emulator");
        const float block = name.y + tag.y - 2.0f;
        const float top = origin.y + (height - block) * 0.5f;
        PutText(draw, fonts.heading, ImVec2(origin.x + x, top), Col(color::kText), "NovaStar64");
        PutText(draw, fonts.small, ImVec2(origin.x + x, top + name.y - 2.0f), Col(color::kTextMuted),
                "Nintendo 64 emulator");
    }

    // What is going on, on the right: the graphics backend, the input device and the size of the library
    float right = controlsLeft - 24.0f;
    if (!inGame) {
        std::vector<std::string> parts;
        parts.emplace_back(BackendName(m_gfx.backendType));
        parts.emplace_back(NovaInput::Gamepads::Instance().Connected(0) ? "Controller" : "Keyboard");
        char games[32];
        std::snprintf(games, sizeof(games), "%d game%s", static_cast<int>(m_library.size()),
                      m_library.size() == 1 ? "" : "s");
        parts.emplace_back(games);
        for (size_t i = parts.size(); i-- > 0;) {
            const ImVec2 sz = Measure(fonts.label, parts[i].c_str());
            right -= sz.x;
            PutText(draw, fonts.label, ImVec2(origin.x + right, origin.y + (height - sz.y) * 0.5f),
                    Col(i == 0 ? color::kText : color::kTextMuted), parts[i].c_str());
            if (i > 0) {
                right -= 16.0f;
                draw->AddLine(ImVec2(origin.x + right, origin.y + height * 0.5f - 9.0f),
                              ImVec2(origin.x + right, origin.y + height * 0.5f + 9.0f), Col(Rgb(0xFFFFFF, 0.22f)));
                right -= 16.0f;
            }
        }
    }

    // The latest message (a save, a rewind, an error), in a pill in the middle of the bar
    if (!m_statusText.empty() && SDL_GetTicks() < m_statusUntil) {
        const float maxText = std::max(120.0f, right - 360.0f);
        const std::string text = Ellipsize(fonts.label, m_statusText, maxText);
        const ImVec2 sz = Measure(fonts.label, text.c_str());
        const float pillW = sz.x + 36.0f, pillH = 34.0f;
        const float pillX =
            std::clamp(width * 0.5f - pillW * 0.5f, x + 8.0f, std::max(x + 8.0f, right - pillW - 16.0f));
        const ImVec2 a(origin.x + pillX, origin.y + (height - pillH) * 0.5f), b(a.x + pillW, a.y + pillH);
        draw->AddRectFilled(a, b, Col(Rgb(0x5B4DE3, 0.42f)), pillH * 0.5f);
        draw->AddRect(a, b, Col(Rgb(0xFFFFFF, 0.22f)), pillH * 0.5f);
        PutText(draw, fonts.label, ImVec2(a.x + 18.0f, a.y + (pillH - sz.y) * 0.5f), Col(color::kText), text.c_str());
    }

    // Window controls
    const ImVec2 buttonSize(controlWidth, controlHeight);
    ImGui::SetCursorPos(ImVec2(controlsLeft, (height - controlHeight) * 0.5f));
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
}

void NovaLauncher::RenderUI(SDL_Window* window) {
    using namespace kit;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    g_dragHeight.store(static_cast<int>(kTitleBarHeight));

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("MainLayout", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    DrawBackground(ImGui::GetWindowDrawList(), ImVec2(0, 0), display);
    RenderTitleBar(window, false);

    const float bodyTop = kTitleBarHeight;
    const float bodyHeight = display.y - kTitleBarHeight - kHintBarHeight;

    // Navigation
    ImGui::SetCursorPos(ImVec2(20.0f, bodyTop + 6.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("Sidebar", ImVec2(kSidebarWidth - 32.0f, bodyHeight - 6.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    RenderSidebar();
    ImGui::EndChild();

    // The page
    ImGui::SetCursorPos(ImVec2(kSidebarWidth - kPagePadding, bodyTop));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPagePadding, kPagePadding));
    ImGui::BeginChild("Content",
                      ImVec2(display.x - kSidebarWidth + kPagePadding - (kRightMargin - kPagePadding), bodyHeight),
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    switch (m_activeTab) {
    case Tab::Home:         RenderHomeTab(); break;
    case Tab::Library:
    case Tab::Recent:
    case Tab::Favorites:    RenderLibraryPage(m_activeTab); break;
    case Tab::Settings:     RenderSettingsTab(); break;
    case Tab::About:        RenderAboutTab(); break;
    case Tab::Scripts:      RenderScriptsTab(); break;
    case Tab::HexEditor:    RenderHexEditorTab(); break;
    case Tab::Achievements: RenderAchievementsTab(); break;
    case Tab::Simulator:    RenderSimulatorTab(); break;
    }
    ImGui::EndChild();

    RenderHintBar();
    RenderGameDialogs();
    ImGui::End();
}

void NovaLauncher::RenderSidebar() {
    using namespace kit;
    struct Entry {
        const char* label;
        Icon icon;
        Tab tab;
    };
    static constexpr Entry kMain[] = {
        {"Home", Icon::Home, Tab::Home},
        {"Library", Icon::Games, Tab::Library},
        {"Recent", Icon::Recent, Tab::Recent},
        {"Favorites", Icon::Star, Tab::Favorites},
        {"Settings", Icon::Settings, Tab::Settings},
        {"About", Icon::Info, Tab::About},
    };
    static constexpr Entry kTools[] = {
        {"Scripts", Icon::Scripts, Tab::Scripts},
        {"Memory", Icon::Memory, Tab::HexEditor},
        {"Trophies", Icon::Trophy, Tab::Achievements},
        {"Simulator", Icon::Simulator, Tab::Simulator},
    };
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 6.0f));
    for (const auto& e : kMain)
        if (NavItem(e.icon, e.label, m_activeTab == e.tab)) m_activeTab = e.tab;

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f);
    SectionLabel("TOOLS");
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 4.0f));
    for (const auto& e : kTools)
        if (NavItem(e.icon, e.label, m_activeTab == e.tab, true)) m_activeTab = e.tab;
    ImGui::PopStyleVar(2);
}

void NovaLauncher::RenderHintBar() {
    using namespace kit;
    const Fonts& fonts = GetFonts();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float top = display.y - kHintBarHeight;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(0, top), display, Col(Rgb(0x05040F, 0.55f)));
    draw->AddLine(ImVec2(0, top), ImVec2(display.x, top), Col(Rgb(0xFFFFFF, 0.07f)));

    ImGui::SetCursorPos(ImVec2(kSidebarWidth, top + (kHintBarHeight - 26.0f) * 0.5f));
    ControllerHint("A", "Select");
    ImGui::SameLine(0.0f, 28.0f);
    ControllerHint("B", "Back");
    ImGui::SameLine(0.0f, 28.0f);
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        DrawIcon(draw, Icon::Download, ImVec2(at.x + 11.0f, at.y + 13.0f), 20.0f, Col(color::kTextMuted));
        PutText(draw, fonts.small, ImVec2(at.x + 28.0f, at.y + 13.0f - Measure(fonts.small, "Drop").y * 0.5f),
                Col(color::kTextMuted), "Drop a ROM on the window to add it");
    }

    char version[64];
    std::snprintf(version, sizeof(version), "NovaStar64  v%s", NOVA64_VERSION);
    const ImVec2 sz = Measure(fonts.small, version);
    PutText(draw, fonts.small, ImVec2(display.x - kRightMargin - sz.x, top + (kHintBarHeight - sz.y) * 0.5f),
            Col(color::kTextDim), version);
}

// --- Home ---
void NovaLauncher::RenderEmptyHero() {
    using namespace kit;
    const Fonts& fonts = GetFonts();
    const float height = 300.0f;
    BeginCard("EmptyHero", ImVec2(0, height), false, ImVec2(36.0f, 30.0f), 24.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float ringX = origin.x + 90.0f, ringY = origin.y + 112.0f;
    draw->AddCircleFilled(ImVec2(ringX, ringY), 72.0f, Col(Rgb(0x5B4DE3, 0.22f)));
    draw->AddCircle(ImVec2(ringX, ringY), 72.0f, Col(Rgb(0x8E82FF, 0.55f)), 0, 2.0f);
    DrawIcon(draw, Icon::Games, ImVec2(ringX, ringY), 64.0f, Col(color::kText));

    const float textX = origin.x + 210.0f;
    const float textW = std::max(240.0f, width - 220.0f);
    PutText(draw, fonts.title, ImVec2(textX, origin.y + 28.0f), Col(color::kText), "Add your first game");
    const std::string body =
        ClampLines(fonts.body,
                   "Drop a ROM file onto this window, or pick one from your computer. NovaStar64 keeps "
                   "a copy in its roms folder and remembers what you played.",
                   textW, 3);
    PutText(draw, fonts.body, ImVec2(textX, origin.y + 82.0f), Col(color::kTextMuted), body.c_str());
    ImGui::SetCursorScreenPos(ImVec2(textX, origin.y + 170.0f));
    if (Button("Open ROM...", ButtonStyle::Primary, Icon::Folder, ImVec2(170.0f, 0.0f))) OpenRomWithPicker();
    ImGui::SameLine();
    if (Button("Add game", ButtonStyle::Secondary, Icon::Plus, ImVec2(150.0f, 0.0f))) OpenAddDialog();
    EndCard();
}

void NovaLauncher::RenderHeroCard(int index) {
    using namespace kit;
    const Fonts& fonts = GetFonts();
    RomMetadata& rom = m_library[static_cast<size_t>(index)];
    const GameInfo* info = FindGameInfo(rom.name);

    std::error_code ec;
    const bool exists = fs::exists(rom.path, ec);
    const std::string shotPath = detail::ShotPathFor(rom.path);
    const CoverTexture* shot = fs::exists(shotPath, ec) ? GetCoverTexture(shotPath) : nullptr;

    const float contentHeight = 232.0f;
    const float padX = 24.0f, padY = 22.0f;
    BeginCard("Hero", ImVec2(0, contentHeight + padY * 2.0f), false, ImVec2(padX, padY), 24.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float innerW = ImGui::GetContentRegionAvail().x;
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // Three columns: the box, the text, and a screenshot of the last time it was played
    const float coverW = std::clamp(innerW * 0.30f, 200.0f, 310.0f);
    const float coverH = std::min(contentHeight, std::floor(coverW * 0.75f));
    const float gap = 28.0f;
    float shotW = shot ? std::clamp(innerW * 0.30f, 240.0f, 340.0f) : 0.0f;
    if (innerW - coverW - shotW - gap * (shot ? 2.0f : 1.0f) < 300.0f) shotW = 0.0f;
    const float textW = innerW - coverW - gap - (shotW > 0.0f ? shotW + gap : 0.0f);

    // The box
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + (contentHeight - coverH) * 0.5f));
    ImGui::PushID("heroCover");
    if (DrawCover(rom.cover, rom.name, ImVec2(coverW, coverH), false, 16.0f) && exists) PlayRom(rom.path, rom.name);
    ImGui::PopID();

    // The text
    const float textX = origin.x + coverW + gap;
    float y = origin.y;
    const std::string title = ClampLines(fonts.title, rom.name, textW, 2);
    const ImVec2 titleSize = Measure(fonts.title, title.c_str());
    PutText(draw, fonts.title, ImVec2(textX, y), Col(color::kText), title.c_str());
    y += titleSize.y + 8.0f;

    ImGui::SetCursorScreenPos(ImVec2(textX, y));
    ImGui::BeginGroup();
    char buffer[64];
    if (info) {
        Chip(info->publisher, Icon::Publisher);
        ImGui::SameLine(0.0f, 18.0f);
        std::snprintf(buffer, sizeof(buffer), "%d", info->year);
        Chip(buffer, Icon::Calendar);
        ImGui::SameLine(0.0f, 18.0f);
        Chip(info->status, std::strcmp(info->status, "Playable") == 0 ? Icon::Check : Icon::Info,
             std::strcmp(info->status, "Playable") == 0 ? Col(color::kSuccess) : 0);
    } else {
        Chip(fs::path(rom.path).filename().string().c_str(), Icon::Folder);
    }
    if (rom.plays > 0) {
        ImGui::SameLine(0.0f, 18.0f);
        std::snprintf(buffer, sizeof(buffer), "Played %d time%s", rom.plays, rom.plays == 1 ? "" : "s");
        Chip(buffer, Icon::Recent);
    }
    if (!exists) {
        ImGui::SameLine(0.0f, 18.0f);
        Chip("File not found", Icon::Warning, Col(color::kDanger));
    }
    ImGui::EndGroup();
    y += 34.0f;

    std::string about = !rom.notes.empty() ? rom.notes : info ? info->blurb : "";
    if (about.empty()) about = "No description yet. Use the menu next to Play to add notes to this game.";
    const std::string blurb = ClampLines(fonts.body, about, textW, 3);
    PutText(draw, fonts.body, ImVec2(textX, y + 4.0f), Col(color::kTextMuted), blurb.c_str());

    // Actions, along the bottom
    ImGui::SetCursorScreenPos(ImVec2(textX, origin.y + contentHeight - 44.0f));
    ImGui::BeginDisabled(!exists);
    if (Button("Play", ButtonStyle::Primary, Icon::Play, ImVec2(150.0f, 0.0f))) PlayRom(rom.path, rom.name);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (Button(rom.favorite ? "Favorited" : "Favorite", ButtonStyle::Secondary, Icon::Heart, ImVec2(0.0f, 0.0f))) {
        rom.favorite = !rom.favorite;
        SaveLibrary();
    }
    ImGui::SameLine();
    if (IconButton("##heroMore", Icon::More, ButtonStyle::Secondary, "More")) ImGui::OpenPopup("##heroMenu");
    if (ImGui::BeginPopup("##heroMenu")) {
        if (ImGui::MenuItem("Edit details...")) OpenEditDialog(index);
        if (ImGui::MenuItem("Remove from library")) {
            ImGui::EndPopup();
            EndCard();
            RemoveGame(index);
            return;
        }
        ImGui::EndPopup();
    }

    // The screenshot
    if (shotW > 0.0f) {
        const ImVec2 a(origin.x + innerW - shotW, origin.y), b(a.x + shotW, a.y + contentHeight);
        DrawImageCover(draw, shot->texture, shot->width, shot->height, a, b, 16.0f);
        draw->AddRect(a, b, Col(Rgb(0xFFFFFF, 0.14f)), 16.0f);
    }
    EndCard();
}

void NovaLauncher::RenderHomeTab() {
    using namespace kit;
    const Fonts& fonts = GetFonts();
    const int count = static_cast<int>(m_library.size());

    // Which game the big card shows: the one picked, else the one played last, else the first
    const std::vector<int> recent = RecentGames();
    int hero = (m_selectedGame >= 0 && m_selectedGame < count) ? m_selectedGame : -1;
    if (hero < 0) hero = !recent.empty() ? recent.front() : (count > 0 ? 0 : -1);

    if (hero < 0) {
        RenderEmptyHero();
        return;
    }
    RenderHeroCard(hero);

    // A row of games below it
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    const bool playedAny = !recent.empty();
    std::vector<int> row = recent;
    if (!playedAny)
        for (int i = 0; i < count; ++i) row.push_back(i);

    const float rowTop = ImGui::GetCursorPosY();
    SectionTitle(playedAny ? "Recently Played" : "Your Library");
    const float seeAllWidth = 118.0f;
    ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionMax().x - seeAllWidth, rowTop - 2.0f));
    if (Button("See All", ButtonStyle::Ghost, Icon::ArrowRight, ImVec2(seeAllWidth, 36.0f), true))
        m_activeTab = playedAny ? Tab::Recent : Tab::Library;
    ImGui::SetCursorPosY(rowTop + 44.0f);

    const float gap = 18.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int perRow = std::clamp(static_cast<int>((avail + gap) / (168.0f + gap)), 2, 7);
    const float tileW = std::floor((avail - gap * (perRow - 1)) / perRow);
    const float tileH = std::floor(tileW * 0.75f);
    const int shown = std::min(static_cast<int>(row.size()), perRow);
    for (int n = 0; n < shown; ++n) {
        const int i = row[static_cast<size_t>(n)];
        const RomMetadata& rom = m_library[static_cast<size_t>(i)];
        if (n > 0) ImGui::SameLine(0.0f, gap);
        ImGui::PushID(i);
        ImGui::BeginGroup();
        if (DrawCover(rom.cover, rom.name, ImVec2(tileW, tileH), i == hero, 14.0f)) m_selectedGame = i;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) PlayRom(rom.path, rom.name);
        const std::string label = Ellipsize(fonts.label, rom.name, tileW);
        ImGui::PushStyleColor(ImGuiCol_Text, i == hero ? color::kText : color::kTextMuted);
        ImGui::PushFont(fonts.label);
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
}

// --- About ---
void NovaLauncher::RenderAboutTab() {
    using namespace kit;
    PageTitle("About", "An open-source Nintendo 64 emulator.");

    BeginCard("AboutVersion");
    ImGui::PushFont(GetFonts().heading);
    ImGui::Text("NovaStar64  %s", NOVA64_VERSION);
    ImGui::PopFont();
    HintText("Copyright (C) 2026 Varga B\xC3\xA1lint. Licensed under the GNU General Public License, version 2.");
    HintText("Began as a fork of Project64, whose copyright and licence it carries. Not affiliated with or endorsed by "
             "Nintendo. Nintendo 64 and the names of games are trademarks of their owners.");
    EndCard();

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    BeginCard("AboutKeys");
    SectionTitle("While a game runs");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    struct Shortcut {
        const char* key;
        const char* does;
    };
    static constexpr Shortcut kShortcuts[] = {
        {"Esc", "Pause menu, with save states and the controls."},
        {"Backspace", "Rewind: pick a moment from the last few minutes."},
        {"Tab", "Double speed on and off."},
    };
    for (const auto& sc : kShortcuts) {
        const float rowTop = ImGui::GetCursorPosY();
        Keycap(sc.key);
        const float capHeight = ImGui::GetItemRectSize().y;
        ImGui::SameLine(130.0f);
        ImGui::SetCursorPosY(rowTop + (capHeight - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::TextUnformatted(sc.does);
        ImGui::SetCursorPosY(rowTop + capHeight + 8.0f);
    }
    EndCard();

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    BeginCard("AboutBuilt");
    SectionTitle("Made with");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    HintText("SDL2, Dear ImGui, asmjit, Lua, nlohmann/json, stb, glslang, rcheevos, MoltenVK and Berkeley SoftFloat. "
             "Text is set in Inter and icons are Phosphor. Each keeps its own licence; see the README.");
    EndCard();
}

} // namespace NovaUI
