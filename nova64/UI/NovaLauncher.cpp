// ============================================================
// NovaLauncher.cpp — NovaStar64 UI Launcher
// Full rewrite with embedded emulator, pause menu, save states,
// input remapping, speed/rewind, Lua scripting, hex editor.
// ============================================================
#define _CRT_SECURE_NO_WARNINGS
#include "NovaLauncher.hpp"
#include "NovaTheme.hpp"

#include <SDL.h>
#include <glad/gl.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#include <imgui_node_editor.h>

#include <cstring>
#include <string>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <thread>
#include <chrono>
#include <fstream>

#include "../N64SYS/MainSYS.hpp"
#include "../NovaGraphics/Plugin/FrameCallback.hpp"

#ifndef _WIN32
#include <unistd.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

#include <cstdio>   // popen/pclose for the native file picker

namespace ed = ax::NodeEditor;

namespace {

// ── Native "open ROM" file picker ───────────────────────────────────────────
// Returns the chosen ROM path, or "" if the user cancelled.  Uses the OS-native
// dialog so it feels like a real Mac app (no in-app path typing required).
inline std::string PickRomFileNative() {
#if defined(__APPLE__)
    // AppleScript drives the native NSOpenPanel.  On cancel, osascript writes an
    // error to stderr (discarded) and prints nothing to stdout.
    // NOTE: no `of type {...}` filter — macOS matches that against UTIs, and the
    // bare extensions z64/n64/v64 have no registered UTI, so the ROMs would be
    // greyed out.  Allow any file and let the user pick the ROM.
    const char* cmd =
        "osascript -e 'POSIX path of (choose file with prompt \"Select an N64 ROM\")'"
        " 2>/dev/null";
    FILE* pipe = ::popen(cmd, "r");
    if (!pipe) return {};
    std::string out;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
    ::pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
#elif defined(__linux__)
    FILE* pipe = ::popen(
        "zenity --file-selection --title='Select an N64 ROM' 2>/dev/null", "r");
    if (!pipe) return {};
    std::string out;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
    ::pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
#else
    return {};
#endif
}

// ── Borderless-window drag region ───────────────────────────────────────────
// The launcher is a borderless window, so it has no native title bar to grab.
// Treat the middle of the 40px top navbar as a drag handle, leaving the left
// (File menu) and right (min/max/close buttons) interactive.
extern "C" SDL_HitTestResult SDLCALL LauncherHitTest(SDL_Window* win,
                                                     const SDL_Point* area,
                                                     void* /*data*/) {
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    constexpr int kNavbarH      = 40;
    constexpr int kLeftReserve  = 80;   // File menu
    constexpr int kRightReserve = 110;  // min / max / close buttons
    if (area->y >= 0 && area->y < kNavbarH &&
        area->x > kLeftReserve && area->x < (w - kRightReserve)) {
        return SDL_HITTEST_DRAGGABLE;
    }
    return SDL_HITTEST_NORMAL;
}
#ifdef _WIN32
inline bool EmuProcValid(const NovaUI::EmuInstance& e) { return e.pi.hProcess != nullptr; }
#else
// Absolute path to this executable, used to re-launch ourselves per ROM.
inline std::string SelfExePath() {
    char buf[4096] = {};
#if defined(__APPLE__)
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    return std::string(buf);
#else
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::string(buf);
#endif
}
inline bool EmuProcValid(const NovaUI::EmuInstance& e) { return e.pid > 0; }
inline void EmuKill(NovaUI::EmuInstance& e) {
    if (e.pid > 0) {
        ::kill(static_cast<pid_t>(e.pid), SIGTERM);
        ::waitpid(static_cast<pid_t>(e.pid), nullptr, 0);
        e.pid = -1;
    }
}
#endif
} // namespace

// ── Editor context (for Simulator tab) ───────────────────────────────────────
static ed::EditorContext* g_Context = nullptr;

// ── Rewind key name ───────────────────────────────────────────────────────────
const char* NovaUI::NovaLauncher::kRewindKeyName = "Backspace";

// ============================================================
// KeyBindings save / load (simple INI-style, no JSON dep)
// ============================================================
void NovaUI::KeyBindings::Save(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return;
    f << "A="        << (int)A        << "\n";
    f << "B="        << (int)B        << "\n";
    f << "Z="        << (int)Z        << "\n";
    f << "Start="    << (int)Start    << "\n";
    f << "DPadUp="   << (int)DPadUp   << "\n";
    f << "DPadDn="   << (int)DPadDn   << "\n";
    f << "DPadLt="   << (int)DPadLt   << "\n";
    f << "DPadRt="   << (int)DPadRt   << "\n";
    f << "L="        << (int)L        << "\n";
    f << "R="        << (int)R        << "\n";
    f << "CUp="      << (int)CUp      << "\n";
    f << "CDown="    << (int)CDown    << "\n";
    f << "CLeft="    << (int)CLeft    << "\n";
    f << "CRight="   << (int)CRight   << "\n";
    f << "StickUp="  << (int)StickUp  << "\n";
    f << "StickDn="  << (int)StickDn  << "\n";
    f << "StickLt="  << (int)StickLt  << "\n";
    f << "StickRt="  << (int)StickRt  << "\n";
}

void NovaUI::KeyBindings::Load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        int val = std::stoi(line.substr(eq + 1));
        if      (key == "A")       A       = val;
        else if (key == "B")       B       = val;
        else if (key == "Z")       Z       = val;
        else if (key == "Start")   Start   = val;
        else if (key == "DPadUp")  DPadUp  = val;
        else if (key == "DPadDn")  DPadDn  = val;
        else if (key == "DPadLt")  DPadLt  = val;
        else if (key == "DPadRt")  DPadRt  = val;
        else if (key == "L")       L       = val;
        else if (key == "R")       R       = val;
        else if (key == "CUp")     CUp     = val;
        else if (key == "CDown")   CDown   = val;
        else if (key == "CLeft")   CLeft   = val;
        else if (key == "CRight")  CRight  = val;
        else if (key == "StickUp") StickUp = val;
        else if (key == "StickDn") StickDn = val;
        else if (key == "StickLt") StickLt = val;
        else if (key == "StickRt") StickRt = val;
    }
}

// ============================================================
// NovaLauncher — constructor / destructor
// ============================================================
NovaUI::NovaLauncher::NovaLauncher()
    : m_selectedRom()
    , m_shouldExit(false)
    , m_activeTab(Tab::Home)
    , m_showEditModal(false)
    , m_showAddModal(false)
    , m_editingRomIndex(0)
    , m_ramAmount(4)
    , m_graphicsApi(0)
    , m_vsync(true)
    , m_internalResScale(1)
    , m_fullscreen(false)
    , m_targetFPS(60)
    , m_antiAliasing(0)
    , m_texFilter(1)
    , m_enableMipmaps(true)
    , m_multiThreading(true)
    , m_showFPS(true)
    , m_simMode(0)
    , m_simTool(SimTool::Select)
    , m_nextNodeId(10)
    , m_nextLinkId(100)
    , m_selectedCartIndex(0)
    , m_windowOpacity(0.0f)
    , m_animState(AnimState::FadeIn)
{
    m_library.push_back({"SM64.z64", "Super Mario 64", "", "A classic 3D platformer."});
    m_library.push_back({"OOT.z64", "Ocarina of Time", "", "Epic adventure."});
    m_library.push_back({"MM.z64", "Majora's Mask", "", "Dark and mysterious."});
    m_library.push_back({"FZeroDD.ndd", "F-Zero X Expansion Kit", "", "Nintendo 64DD Magneto-Optical Disk Expansion Packet."});

    memset(m_editNameBuffer, 0, sizeof(m_editNameBuffer));
    memset(m_editBoxArtBuffer, 0, sizeof(m_editBoxArtBuffer));
    memset(m_editNotesBuffer, 0, sizeof(m_editNotesBuffer));

    m_bindings.Load("settings/keybindings.ini");
}

NovaUI::NovaLauncher::~NovaLauncher() {
    StopEmbedded();
    for (auto& inst : m_instances) {
        if (inst.running && EmuProcValid(inst)) {
#ifdef _WIN32
            TerminateProcess(inst.pi.hProcess, 0);
            CloseHandle(inst.pi.hProcess);
            CloseHandle(inst.pi.hThread);
#else
            EmuKill(inst);
#endif
        }
    }
}

// ============================================================
// Helpers
// ============================================================
bool NovaUI::NovaLauncher::IsN64DDDisk(const std::string& path) {
    if (path.length() < 4) return false;
    std::string ext = path.substr(path.find_last_of('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return (ext == "ndd" || ext == "d64");
}

void NovaUI::NovaLauncher::RefreshScriptFiles() {
    m_scriptFiles.clear();
    namespace fs = std::filesystem;
    fs::create_directories("scripts");
    for (auto& e : fs::directory_iterator("scripts")) {
        if (e.path().extension() == ".lua")
            m_scriptFiles.push_back(e.path().string());
    }
}

// ============================================================
// Embedded emulator: launch + stop
// ============================================================
void NovaUI::NovaLauncher::LaunchEmbedded(const std::string& romPath) {
    StopEmbedded();

    // Copy ROM to roms/
    namespace fs = std::filesystem;
    fs::path src(romPath);
    fs::create_directories("roms");
    fs::path dst = fs::path("roms") / src.filename();
    if (fs::exists(src)) {
        std::error_code ec;
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (!ec) src = dst;
    }
    m_emuRomPath = src.string();

    // Setup save state
    m_saveState.SetRom(m_emuRomPath);

    // Init Lua engine (wire RDRAM after emulator starts — placeholder for now)
    m_lua.Init(nullptr, 0);

    // Register frame callback — pixel upload is now handled by UploadFromPixelBuf
    // in the launcher render loop. The callback only ticks Lua scripts.
    Nova64::Video::g_frameCallback = [this]() {
        m_lua.OnFrame();
    };

    // Set embedded mode flag BEFORE starting the thread.
    Nova64::Video::g_embeddedMode.store(true);
    Nova64::Video::g_emuPaused.store(false);
    Nova64::Video::g_speedMultiplier.store(1.0f);
    Nova64::Video::g_rewindActive.store(false);
    Nova64::Video::g_frameReady.store(false);
    m_turboMode = false;
    m_showPauseMenu = false;
    m_emuRunning.store(true);


    // Launch emulator on background thread
    std::string romStr = m_emuRomPath;
    m_emuThread = std::thread([this, romStr]() {
        CN64System::s_TargetFPS = m_targetFPS;
        CN64System::RunFileImage(romStr.c_str(), 0);
        m_emuRunning.store(false);
        Nova64::Video::g_embeddedMode.store(false);
        Nova64::Video::g_frameReady.store(false);
    });
}

void NovaUI::NovaLauncher::StopEmbedded() {
    if (m_emuRunning.load()) {
        // Must unpause BEFORE stopping — otherwise the emulator thread is
        // spin-waiting on g_emuPaused and CloseSystem() can never be reached.
        Nova64::Video::g_emuPaused.store(false);
        Nova64::Video::g_embeddedMode.store(false); // stop pixel readback too
        m_showPauseMenu = false;

        CN64System::CloseSystem();
        m_emuRunning.store(false);
    }
    if (m_emuThread.joinable())
        m_emuThread.join();

    Nova64::Video::g_frameCallback = nullptr;
    Nova64::Video::g_frameReady.store(false);
    m_turboMode = false;

    // Delete the game texture so we don't render a stale frame on the
    // next launcher screen.
    if (m_gameTex) {
        glDeleteTextures(1, &m_gameTex);
        m_gameTex = 0;
        m_gameTexW = m_gameTexH = 0;
    }
}


// Upload CPU pixels (written by VideoPlugin via glReadPixels) to our GL texture.
// Must be called from the LAUNCHER's GL thread (where m_gameTex lives).
void NovaUI::NovaLauncher::UploadFrameTex(uint32_t /*unused*/, int /*unused*/, int /*unused*/) {
    // Not used with pixel-buffer path — see UploadFromPixelBuf below.
}

void NovaUI::NovaLauncher::UploadFromPixelBuf() {
    if (!Nova64::Video::g_frameReady.load(std::memory_order_acquire)) return;

    std::lock_guard<std::mutex> lk(Nova64::Video::g_frameMutex);
    int w = Nova64::Video::g_frameW.load(std::memory_order_relaxed);
    int h = Nova64::Video::g_frameH.load(std::memory_order_relaxed);
    if (w <= 0 || h <= 0 || Nova64::Video::g_framePixels.empty()) return;

    if (m_gameTex == 0 || w != m_gameTexW || h != m_gameTexH) {
        if (m_gameTex) glDeleteTextures(1, &m_gameTex);
        glGenTextures(1, &m_gameTex);
        glBindTexture(GL_TEXTURE_2D, m_gameTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, Nova64::Video::g_framePixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        m_gameTexW = w;
        m_gameTexH = h;
    } else {
        glBindTexture(GL_TEXTURE_2D, m_gameTex);
        // glReadPixels returns bottom-up; flip via pixel-row trick at upload.
        // For now just upload as-is (will be upside-down — we flip the UV below).
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, Nova64::Video::g_framePixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    Nova64::Video::g_frameReady.store(false, std::memory_order_relaxed);
}

// ============================================================
// Render: game view — draws game texture as full-window BACKGROUND.
// Called BEFORE ImGui::NewFrame so it doesn't block input on overlay.
// ============================================================
void NovaUI::NovaLauncher::RenderGameView(SDL_Window* /*window*/) {
    // Upload new frame if VideoPlugin deposited one
    UploadFromPixelBuf();

    if (!m_gameTex) return;

    ImGuiIO& io = ImGui::GetIO();
    float winW = io.DisplaySize.x;
    float winH = io.DisplaySize.y;

    // Draw directly into the background draw list — no ImGui window needed,
    // so it never intercepts mouse/keyboard events.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // Compute letterboxed rect (4:3)
    float imgW = winW;
    float imgH = imgW * 3.0f / 4.0f;
    if (imgH > winH) { imgH = winH; imgW = imgH * 4.0f / 3.0f; }
    float ox = (winW - imgW) * 0.5f;
    float oy = (winH - imgH) * 0.5f;

    // glReadPixels is bottom-up: flip UV vertically to correct orientation
    dl->AddImage(
        (ImTextureID)(uintptr_t)m_gameTex,
        ImVec2(ox, oy + imgH),           // top-left  = bottom of texture
        ImVec2(ox + imgW, oy),            // bot-right = top of texture
        ImVec2(0, 0), ImVec2(1, 1)        // UV: normal (flip handled by vertex swap)
    );
}

// ============================================================
// Pause Menu — proper overlay, fully interactive
// ============================================================
void NovaUI::NovaLauncher::RenderPauseMenu() {
    if (!m_showPauseMenu) return;

    ImGuiIO& io = ImGui::GetIO();
    float winW = io.DisplaySize.x;
    float winH = io.DisplaySize.y;

    // Dim the game with a semi-transparent rect drawn behind the menu window
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0,0), ImVec2(winW, winH), IM_COL32(0,0,0,170));

    float pw = 580.0f, ph = 480.0f;
    ImGui::SetNextWindowPos(ImVec2((winW-pw)*0.5f, (winH-ph)*0.5f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(pw, ph), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::SetNextWindowFocus();  // Ensure this window receives keyboard/mouse
    ImGui::Begin("##PauseMenu", nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_NoMove       |
                 ImGuiWindowFlags_NoScrollbar);

    ImGui::SetWindowFontScale(1.4f);
    ImGui::TextColored(ImVec4(0.85f,0.43f,0.26f,1.0f), "  PAUSED");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::Spacing();

    // ── Save States ──────────────────────────────────────────
    ImGui::TextColored(ImVec4(0.7f,0.85f,1.0f,1.0f), "Save States");
    ImGui::Spacing();
    for (int i = 0; i < kSaveSlots; ++i) {
        const auto& slot = m_saveState.GetSlot(i);
        char label[64];
        snprintf(label, sizeof(label), "Slot %d: %s", i+1,
                 slot.hasData ? slot.TimestampStr().c_str() : "(empty)");

        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f,0.5f,0.2f,1.0f));
        if (ImGui::Button("Save", ImVec2(60,0))) {
            Nova64::Video::g_emuPaused.store(true);
            m_saveState.Save(i);
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, slot.hasData ? ImVec4(0.2f,0.3f,0.7f,1.0f) : ImVec4(0.3f,0.3f,0.3f,0.5f));
        if (ImGui::Button("Load", ImVec2(60,0)) && slot.hasData) {
            m_saveState.Load(i);
            m_showPauseMenu = false;
            Nova64::Video::g_emuPaused.store(false);
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ── Speed toggle ─────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.7f,0.85f,1.0f,1.0f), "Speed");
    ImGui::SameLine();
    if (ImGui::RadioButton("1x", !m_turboMode)) {
        m_turboMode = false;
        CN64System::s_TargetFPS = m_targetFPS;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("2x (TAB)", m_turboMode)) {
        m_turboMode = true;
        CN64System::s_TargetFPS = 0;
    }
    ImGui::TextDisabled("  Hold [%s] to rewind while playing", kRewindKeyName);

    ImGui::Spacing();
    ImGui::Separator();

    // ── Input Config (compact) ────────────────────────────────
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Input Configuration")) {
        RenderInputSettingsWidget();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Resume / Quit ─────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f,0.6f,0.1f,1.0f));
    if (ImGui::Button("Resume (ESC)", ImVec2(200,34))) {
        m_showPauseMenu = false;
        Nova64::Video::g_emuPaused.store(false);
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f,0.1f,0.1f,1.0f));
    if (ImGui::Button("Quit Game", ImVec2(200,34))) {
        m_showPauseMenu = false;
        StopEmbedded();
        m_activeTab = Tab::Home;
    }
    ImGui::PopStyleColor();

    ImGui::End();
}

// ============================================================
// Input settings widget (shared: pause menu + Settings tab)
// ============================================================
void NovaUI::NovaLauncher::RenderInputSettingsWidget() {
    struct Entry { const char* name; int KeyBindings::*field; };
    static const Entry entries[] = {
        {"A Button",    &KeyBindings::A},
        {"B Button",    &KeyBindings::B},
        {"Z Trigger",   &KeyBindings::Z},
        {"Start",       &KeyBindings::Start},
        {"D-Pad Up",    &KeyBindings::DPadUp},
        {"D-Pad Down",  &KeyBindings::DPadDn},
        {"D-Pad Left",  &KeyBindings::DPadLt},
        {"D-Pad Right", &KeyBindings::DPadRt},
        {"L Bumper",    &KeyBindings::L},
        {"R Bumper",    &KeyBindings::R},
        {"C-Up",        &KeyBindings::CUp},
        {"C-Down",      &KeyBindings::CDown},
        {"C-Left",      &KeyBindings::CLeft},
        {"C-Right",     &KeyBindings::CRight},
        {"Stick Up",    &KeyBindings::StickUp},
        {"Stick Down",  &KeyBindings::StickDn},
        {"Stick Left",  &KeyBindings::StickLt},
        {"Stick Right", &KeyBindings::StickRt},
    };
    constexpr int N = (int)(sizeof(entries)/sizeof(entries[0]));

    ImGui::BeginTable("InputTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg);
    ImGui::TableSetupColumn("Button",  ImGuiTableColumnFlags_WidthFixed, 120);
    ImGui::TableSetupColumn("Key",     ImGuiTableColumnFlags_WidthFixed, 120);
    ImGui::TableSetupColumn("Action",  ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (int i = 0; i < N; ++i) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(entries[i].name);

        ImGui::TableSetColumnIndex(1);
        int sc = m_bindings.*entries[i].field;
        const char* keyName = SDL_GetScancodeName((SDL_Scancode)sc);
        ImGui::TextUnformatted(keyName ? keyName : "?");

        ImGui::TableSetColumnIndex(2);
        ImGui::PushID(i);
        bool isRebinding = (m_rebindTarget == i);
        if (isRebinding) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f,0.43f,0.26f,1.0f));
            ImGui::Button("Press a key...", ImVec2(-1,0));
            ImGui::PopStyleColor();
            // Capture next keypress
            const uint8_t* ks = SDL_GetKeyboardState(nullptr);
            for (int sc2 = 0; sc2 < SDL_NUM_SCANCODES; ++sc2) {
                if (ks[sc2]) {
                    m_bindings.*entries[i].field = sc2;
                    m_rebindTarget = -1;
                    m_bindings.Save("settings/keybindings.ini");
                    break;
                }
            }
        } else {
            if (ImGui::Button("Rebind", ImVec2(-1,0)))
                m_rebindTarget = i;
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// ============================================================
// Window animation
// ============================================================
void NovaUI::NovaLauncher::UpdateWindowAnimations(SDL_Window* window) {
    const float animationSpeed = 0.08f;
    switch (m_animState) {
        case AnimState::FadeIn:
            m_windowOpacity += animationSpeed;
            if (m_windowOpacity >= 1.0f) { m_windowOpacity = 1.0f; m_animState = AnimState::Stable; }
            SDL_SetWindowOpacity(window, m_windowOpacity);
            break;
        case AnimState::FadeOut:
            m_windowOpacity -= animationSpeed;
            if (m_windowOpacity <= 0.0f) { m_windowOpacity = 0.0f; m_shouldExit = true; }
            SDL_SetWindowOpacity(window, m_windowOpacity);
            break;
        case AnimState::MinimizeOut:
            m_windowOpacity -= animationSpeed * 1.5f;
            if (m_windowOpacity <= 0.0f) {
                SDL_MinimizeWindow(window);
                m_animState = AnimState::Stable;
                m_windowOpacity = 1.0f;
                SDL_SetWindowOpacity(window, 1.0f);
            } else {
                SDL_SetWindowOpacity(window, m_windowOpacity);
            }
            break;
        default: break;
    }
}

// ============================================================
// Child-process instance management (Simulator mode)
// ============================================================
void NovaUI::NovaLauncher::LaunchEmulator(const SimNode& node) {
    if (node.cartPath[0] == '\0') return;
    const std::string name = (node.cartName[0] != '\0') ? node.cartName
                                                         : std::filesystem::path(node.cartPath).filename().string();
    LaunchProcess(node.cartPath, name);
}

// Copy the ROM into roms/ and re-launch this executable with it as a child
// process so the game gets its own window. The child runs the emulator on its
// main thread, which is required for window/surface creation (the in-launcher
// "embedded" path runs on a background thread and cannot create a window on
// macOS). The child inherits our environment and preloads MoltenVK itself.
void NovaUI::NovaLauncher::LaunchProcess(const std::string& romPath, const std::string& romName) {
    if (romPath.empty()) return;

    namespace fs = std::filesystem;
    fs::path srcPath(romPath);
    fs::create_directories("roms");
    fs::path destPath = fs::path("roms") / srcPath.filename();
    if (fs::exists(srcPath)) {
        std::error_code ec;
        fs::copy_file(srcPath, destPath, fs::copy_options::overwrite_existing, ec);
        if (!ec) srcPath = destPath;
    }

    std::string romStr = srcPath.string();
    const std::string name = !romName.empty() ? romName : srcPath.filename().string();

#ifdef _WIN32
    wchar_t exePathW[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
    std::wstring cmdLine = std::wstring(exePathW) + L" \"" +
                           std::wstring(romStr.begin(), romStr.end()) + L"\"";
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        EmuInstance inst;
        inst.romName = name;
        inst.romPath = romStr;
        inst.pi      = pi;
        inst.running = true;
        m_instances.push_back(std::move(inst));
    }
#else
    // POSIX: re-launch this executable with the ROM path as an argument.
    std::string exePath = SelfExePath();
    if (!exePath.empty()) {
        char* argv[] = { const_cast<char*>(exePath.c_str()),
                         const_cast<char*>(romStr.c_str()),
                         nullptr };
        pid_t childPid = -1;
        if (posix_spawn(&childPid, exePath.c_str(), nullptr, nullptr, argv, environ) == 0) {
            EmuInstance inst;
            inst.romName = name;
            inst.romPath = romStr;
            inst.pid     = static_cast<long>(childPid);
            inst.running = true;
            m_instances.push_back(std::move(inst));
        }
    }
#endif
}

void NovaUI::NovaLauncher::PollInstances() {
    for (auto& inst : m_instances) {
        if (!inst.running) continue;
#ifdef _WIN32
        DWORD exitCode = STILL_ACTIVE;
        if (GetExitCodeProcess(inst.pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
            inst.running = false;
            CloseHandle(inst.pi.hProcess);
            CloseHandle(inst.pi.hThread);
            inst.pi.hProcess = nullptr;
        }
#else
        if (inst.pid > 0) {
            int status = 0;
            pid_t r = ::waitpid(static_cast<pid_t>(inst.pid), &status, WNOHANG);
            if (r == static_cast<pid_t>(inst.pid) || r == -1) {
                inst.running = false;
                inst.pid = -1;
            }
        }
#endif
    }
    m_instances.erase(std::remove_if(m_instances.begin(), m_instances.end(),
        [](const EmuInstance& e){ return !e.running && !EmuProcValid(e); }),
        m_instances.end());
}

// ============================================================
// Main Run() loop
// ============================================================
std::string NovaUI::NovaLauncher::Run() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return "";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);

    SDL_Window* window = SDL_CreateWindow("NovaStar64 - Boot Menu",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 768,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS);
    if (!window) { SDL_Quit(); return ""; }

    // Borderless window has no native title bar — make the top navbar a drag
    // handle so the window can be moved.
    SDL_SetWindowHitTest(window, LauncherHitTest, nullptr);

    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, glContext);
    SDL_GL_SetSwapInterval(1);
    SDL_SetWindowOpacity(window, 0.0f);

    // Ensure settings / scripts dirs
    namespace fs = std::filesystem;
    fs::create_directories("settings");
    fs::create_directories("scripts");
    fs::create_directories("saves");

    if (!gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress)) {
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return "";
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "settings/imgui.ini";

    ApplyWin95Theme();
    ImGui_ImplSDL2_InitForOpenGL(window, glContext);
    ImGui_ImplOpenGL3_Init("#version 330");

    ed::Config config;
    config.SettingsFile = "settings/NodeEditor.json";
    g_Context = ed::CreateEditor(&config);

    RefreshScriptFiles();

    while (!m_shouldExit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);

            if (event.type == SDL_QUIT) {
                m_shouldExit = true;
            }
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window)) {
                m_shouldExit = true;
            }

            // Drag-and-drop a ROM onto the window: add it to the library and
            // boot it immediately.
            if (event.type == SDL_DROPFILE && event.drop.file) {
                std::string dropped(event.drop.file);
                SDL_free(event.drop.file);
                if (!dropped.empty()) {
                    std::string nm = std::filesystem::path(dropped).stem().string();
                    // De-dup: only add to the library if not already present.
                    bool known = false;
                    for (auto& r : m_library) if (r.path == dropped) { known = true; break; }
                    if (!known) m_library.push_back({dropped, nm, "", ""});
#ifdef __APPLE__
                    LaunchProcess(dropped, nm);   // separate window; renders via Vulkan/MoltenVK
#else
                    if (!m_emuRunning.load()) LaunchEmbedded(dropped);
#endif
                }
            }

            // ESC: toggle pause menu (only when game running embedded)
            if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                if (m_emuRunning.load()) {
                    m_showPauseMenu = !m_showPauseMenu;
                    Nova64::Video::g_emuPaused.store(m_showPauseMenu);
                }
            }

            // TAB: toggle 2x speed
            if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_TAB) {
                if (m_emuRunning.load()) {
                    m_turboMode = !m_turboMode;
                    CN64System::s_TargetFPS = m_turboMode ? 0 : m_targetFPS;
                }
            }

            // Backspace held: rewind
            if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_BACKSPACE) {
                if (m_emuRunning.load())
                    Nova64::Video::g_rewindActive.store(true);
            }
            if (event.type == SDL_KEYUP && event.key.keysym.scancode == SDL_SCANCODE_BACKSPACE) {
                Nova64::Video::g_rewindActive.store(false);
            }
        }

        PollInstances();
        UpdateWindowAnimations(window);

        // ── Render ─────────────────────────────────────────────
        SDL_GL_MakeCurrent(window, glContext);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // If game is running embedded, draw it first then overlay launcher UI
        if (m_emuRunning.load()) {
            RenderGameView(window);
            RenderPauseMenu();
            // Small always-on-top HUD strip (top bar only, no sidebar)
            RenderTopNavbar(window);
        } else {
            RenderUI(window);
        }

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    // Cleanup
    StopEmbedded();
    for (auto& inst : m_instances) {
        if (inst.running && EmuProcValid(inst)) {
#ifdef _WIN32
            TerminateProcess(inst.pi.hProcess, 0);
            CloseHandle(inst.pi.hProcess); CloseHandle(inst.pi.hThread);
#else
            EmuKill(inst);
#endif
        }
    }
    if (m_gameTex) { glDeleteTextures(1, &m_gameTex); m_gameTex = 0; }

    ed::DestroyEditor(g_Context); g_Context = nullptr;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return "";
}

// ============================================================
// Top navbar
// ============================================================
void NovaUI::NovaLauncher::RenderTopNavbar(SDL_Window* window) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg,   ImVec4(0.85f,0.43f,0.26f,1.0f));
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0.85f,0.43f,0.26f,1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text,      ImVec4(1.0f,1.0f,1.0f,1.0f));
    ImGui::BeginChild("TopNavbar", ImVec2(0, 40), false, ImGuiWindowFlags_MenuBar);
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Exit Emulator")) { m_animState = AnimState::FadeOut; }
            ImGui::EndMenu();
        }

        // Show running indicator if game is embedded
        if (m_emuRunning.load()) {
            ImGui::TextColored(ImVec4(0.2f,1.0f,0.2f,1.0f), "  [RUNNING: %s]",
                               std::filesystem::path(m_emuRomPath).stem().string().c_str());
            if (m_turboMode) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f,0.8f,0.2f,1.0f), "[2x]"); }
        }

        float btnW = 28.0f;
        float rightX = ImGui::GetWindowWidth() - btnW * 3 - 8;
        ImGui::SameLine(rightX);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1,1,1,0.2f));
        if (ImGui::Button(" _ ", ImVec2(btnW,0))) { m_animState = AnimState::MinimizeOut; }
        ImGui::SameLine();
        static bool maximized = false;
        if (ImGui::Button(maximized ? " [] " : " [=] ", ImVec2(btnW,0))) {
            if (maximized) { SDL_RestoreWindow(window); maximized = false; }
            else           { SDL_MaximizeWindow(window); maximized = true; }
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f,0.1f,0.1f,0.8f));
        if (ImGui::Button(" X ", ImVec2(btnW,0))) { m_animState = AnimState::FadeOut; }
        ImGui::PopStyleColor(3);
        ImGui::EndMenuBar();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(3);
}

// ============================================================
// Full UI (shown when no game is running)
// ============================================================
void NovaUI::NovaLauncher::RenderUI(SDL_Window* window) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("MainLayout", nullptr, flags);
    RenderTopNavbar(window);

    ImGui::Columns(2, "LayoutColumns", false);
    ImGui::SetColumnWidth(0, 200.0f);
    ImGui::BeginChild("Sidebar", ImVec2(0,0), true);
    RenderSidebar();
    ImGui::EndChild();
    ImGui::NextColumn();
    ImGui::BeginChild("Content", ImVec2(0,0), true, ImGuiWindowFlags_HorizontalScrollbar);
    switch (m_activeTab) {
        case Tab::Home:         RenderHomeTab();         break;
        case Tab::Games:        RenderGamesTab();        break;
        case Tab::Settings:     RenderSettingsTab();     break;
        case Tab::Scripts:      RenderScriptsTab();      break;
        case Tab::HexEditor:    RenderHexEditorTab();    break;
        case Tab::Achievements: RenderAchievementsTab(); break;
        case Tab::Simulator:    RenderSimulatorTab();    break;
    }
    ImGui::EndChild();
    ImGui::End();
}

// ============================================================
// Sidebar
// ============================================================
void NovaUI::NovaLauncher::RenderSidebar() {
    ImGui::TextDisabled("NOVASTAR64");
    ImGui::Spacing(); ImGui::Spacing();

    const char* tabs[] = {"Home Menu","Games","Settings","Scripts","Hex Editor","Achievements","Simulator"};
    Tab tabValues[]    = {Tab::Home, Tab::Games, Tab::Settings, Tab::Scripts,
                          Tab::HexEditor, Tab::Achievements, Tab::Simulator};
    for (int i = 0; i < 7; ++i) {
        if (ImGui::Selectable(tabs[i], m_activeTab == tabValues[i]))
            m_activeTab = tabValues[i];
    }
}

// ============================================================
// Home Tab
// ============================================================
void NovaUI::NovaLauncher::RenderHomeTab() {
    ImGui::SetWindowFontScale(1.5f);
    ImGui::Text("NovaStar64 Dash Center");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Text("Welcome to your retro low-level desktop subsystem development module.");
    ImGui::Separator(); ImGui::Spacing();

    ImGui::BeginChild("DashboardStatus", ImVec2(0,140), true);
    ImGui::TextColored(ImVec4(0.85f,0.43f,0.26f,1.0f), "System Status Monitor");
    ImGui::Text("-----------------------------------------");
    ImGui::Text("Library Managed ROMs: %d", (int)m_library.size());
    ImGui::Text("Active Emulator Instances: %d", (int)m_instances.size());
    if (m_emuRunning.load()) {
        ImGui::TextColored(ImVec4(0.2f,1.0f,0.2f,1.0f), "Embedded Game: %s %s",
                           std::filesystem::path(m_emuRomPath).stem().string().c_str(),
                           m_turboMode ? "[2x SPEED]" : "");
        ImGui::TextDisabled("  ESC = Pause | TAB = 2x Speed | Backspace = Rewind");
    }
    ImGui::Text("Hardware Simulation: %s", m_simMode == 0 ? "Realtime" : "Sandbox");
    ImGui::EndChild();

    RenderInstancesPanel();
}

// ============================================================
// Instance panel (child-process instances, Simulator mode)
// ============================================================
void NovaUI::NovaLauncher::RenderInstancesPanel() {
    ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.85f,0.43f,0.26f,1.0f), "Running Emulator Instances");
    if (m_instances.empty()) { ImGui::TextDisabled("  No active instances."); return; }

    for (size_t i = 0; i < m_instances.size(); ++i) {
        auto& inst = m_instances[i];
        ImGui::PushID((int)i);
        ImGui::BeginChild("##inst", ImVec2(0,60), true);
        ImGui::TextColored(inst.running ? ImVec4(0.2f,1.0f,0.2f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f),
                           inst.running ? "[RUNNING]" : "[STOPPED]");
        ImGui::SameLine(); ImGui::TextWrapped("%s", inst.romName.c_str());
        ImGui::TextDisabled("%s", inst.romPath.c_str());
        if (inst.running) {
            ImGui::SameLine(ImGui::GetWindowWidth() - 90);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f,0.1f,0.1f,1.0f));
            if (ImGui::Button("Force Stop", ImVec2(80,0))) {
#ifdef _WIN32
                TerminateProcess(inst.pi.hProcess, 1);
                CloseHandle(inst.pi.hProcess); CloseHandle(inst.pi.hThread);
                inst.pi.hProcess = nullptr;
#else
                EmuKill(inst);
#endif
                inst.running = false;
            }
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
}

// ============================================================
// Games Tab
// ============================================================
void NovaUI::NovaLauncher::RenderGamesTab() {
    ImGui::Text("Games Library Managed Files");
    ImGui::SameLine(ImGui::GetWindowWidth() - 50);
    if (ImGui::Button("+", ImVec2(30,0))) {
        m_showAddModal = true;
        memset(m_editNameBuffer,    0, sizeof(m_editNameBuffer));
        memset(m_editBoxArtBuffer,  0, sizeof(m_editBoxArtBuffer));
        memset(m_editNotesBuffer,   0, sizeof(m_editNotesBuffer));
    }
    ImGui::Separator(); ImGui::Spacing();

    ImGui::Columns(3, "LibraryGrid", false);
    for (size_t i = 0; i < m_library.size(); ++i) {
        auto& rom = m_library[i];
        ImGui::PushID((int)i);
        ImGui::BeginChild("GameCard", ImVec2(0,150), true);
        ImGui::TextWrapped("%s", rom.name.c_str());
        ImGui::TextDisabled("%s", rom.path.c_str());

#ifdef __APPLE__
        // macOS: the GL renderer needs 4.6 (macOS caps at 4.1), so the game
        // renders via Vulkan/MoltenVK in its own window (a background thread
        // cannot create the window/surface — embedded-in-launcher needs the
        // offscreen-Vulkan path which is not wired yet).
        if (ImGui::Button("Play", ImVec2(-1,0))) {
            LaunchProcess(rom.path, rom.name);
        }
#else
        // Launch embedded (default, non-simulator)
        if (ImGui::Button("Play (Embedded)", ImVec2(-1,0))) {
            LaunchEmbedded(rom.path);
        }
#endif

        if (ImGui::BeginPopupContextWindow("GameContext")) {
            if (ImGui::MenuItem("Edit ROM")) {
                m_showEditModal    = true;
                m_editingRomIndex  = (int)i;
                strncpy(m_editNameBuffer,   rom.name.c_str(), sizeof(m_editNameBuffer)-1);
                strncpy(m_editBoxArtBuffer, rom.path.c_str(), sizeof(m_editBoxArtBuffer)-1);
                strncpy(m_editNotesBuffer,  rom.notes.c_str(), sizeof(m_editNotesBuffer)-1);
            }
            if (ImGui::MenuItem("Delete ROM")) { m_library.erase(m_library.begin() + i); ImGui::EndPopup(); ImGui::EndChild(); ImGui::PopID(); break; }
            ImGui::EndPopup();
        }
        ImGui::EndChild();
        ImGui::PopID();
        ImGui::NextColumn();
    }
    ImGui::Columns(1);

    if (m_showAddModal) ImGui::OpenPopup("Add New ROM");
    if (ImGui::BeginPopupModal("Add New ROM", &m_showAddModal, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("ROM Path", m_editBoxArtBuffer, sizeof(m_editBoxArtBuffer));
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            std::string picked = PickRomFileNative();
            if (!picked.empty()) {
                std::strncpy(m_editBoxArtBuffer, picked.c_str(), sizeof(m_editBoxArtBuffer) - 1);
                m_editBoxArtBuffer[sizeof(m_editBoxArtBuffer) - 1] = '\0';
                if (m_editNameBuffer[0] == '\0') {
                    std::string nm = std::filesystem::path(picked).stem().string();
                    std::strncpy(m_editNameBuffer, nm.c_str(), sizeof(m_editNameBuffer) - 1);
                    m_editNameBuffer[sizeof(m_editNameBuffer) - 1] = '\0';
                }
            }
        }
        ImGui::InputText("Name",     m_editNameBuffer,   sizeof(m_editNameBuffer));
        ImGui::InputTextMultiline("Notes", m_editNotesBuffer, sizeof(m_editNotesBuffer), ImVec2(300,100));
        if (ImGui::Button("Add Game", ImVec2(120,0))) {
            m_library.push_back({m_editBoxArtBuffer, m_editNameBuffer, "", m_editNotesBuffer});
            m_showAddModal = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120,0))) m_showAddModal = false;
        ImGui::EndPopup();
    }
    if (m_showEditModal) ImGui::OpenPopup("Edit ROM");
    if (ImGui::BeginPopupModal("Edit ROM", &m_showEditModal, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("ROM Path", m_editBoxArtBuffer, sizeof(m_editBoxArtBuffer));
        ImGui::InputText("Name",     m_editNameBuffer,   sizeof(m_editNameBuffer));
        ImGui::InputTextMultiline("Notes", m_editNotesBuffer, sizeof(m_editNotesBuffer), ImVec2(300,100));
        if (ImGui::Button("Save")) {
            if (m_editingRomIndex >= 0 && m_editingRomIndex < (int)m_library.size()) {
                auto& rom = m_library[m_editingRomIndex];
                rom.path  = m_editBoxArtBuffer;
                rom.name  = m_editNameBuffer;
                rom.notes = m_editNotesBuffer;
            }
            m_showEditModal = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120,0))) m_showEditModal = false;
        ImGui::EndPopup();
    }
}

// ============================================================
// Settings Tab
// ============================================================
void NovaUI::NovaLauncher::RenderSettingsTab() {
    ImGui::Text("System Properties & Configurations");
    ImGui::TextDisabled("Altering parameters will directly affect new hardware instances.");
    ImGui::Separator(); ImGui::Spacing();

    if (ImGui::BeginTabBar("SettingsTabs")) {
        if (ImGui::BeginTabItem("Hardware Core")) {
            ImGui::Spacing();
            ImGui::Text("Onboard Console Memory Allocation:");
            ImGui::RadioButton("4 MB Base",    &m_ramAmount, 4);
            ImGui::RadioButton("8 MB Expanded",&m_ramAmount, 8);
            ImGui::Separator();
            ImGui::Checkbox("Multi-Threaded Dispatching", &m_multiThreading);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Video Settings")) {
            ImGui::Spacing();
            const char* backends[] = {"OpenGL 3.3 Core Profile","Vulkan Hardware Interface (STUB)"};
            ImGui::Combo("Graphics API Layer", &m_graphicsApi, backends, IM_ARRAYSIZE(backends));
            ImGui::SliderInt("Internal Scaling Target", &m_internalResScale, 1, 8, "%dx Native");
            ImGui::SliderInt("Target Frame Rate Caps",  &m_targetFPS,        30, 120, "%d FPS");
            ImGui::Checkbox("Enable Vertical Sync State",   &m_vsync);
            ImGui::Checkbox("Launch Fullscreen",            &m_fullscreen);
            ImGui::Checkbox("Display Active Frames Overlay",&m_showFPS);
            ImGui::Separator();
            const char* texFilters[] = {"Point Filtering","Bilinear Interpolation","Trilinear Cache Map"};
            ImGui::Combo("Texture Sample Processing", &m_texFilter, texFilters, IM_ARRAYSIZE(texFilters));
            ImGui::Checkbox("Pre-calculate Linear Texture Mipmaps", &m_enableMipmaps);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Input")) {
            ImGui::Spacing();
            ImGui::TextDisabled("Click 'Rebind' then press the new key. Changes save automatically.");
            ImGui::Spacing();
            RenderInputSettingsWidget();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ============================================================
// Scripts Tab (Lua)
// ============================================================
void NovaUI::NovaLauncher::RenderScriptsTab() {
    ImGui::Text("Lua Scripting Console");
    ImGui::TextDisabled("Place .lua files in the scripts/ directory.");
    ImGui::Separator(); ImGui::Spacing();

    if (ImGui::Button("Refresh Script List")) RefreshScriptFiles();
    ImGui::SameLine();
    if (ImGui::Button("Unload All Scripts")) m_lua.UnloadAll();
    ImGui::Separator();

    // Script list
    ImGui::BeginChild("ScriptList", ImVec2(0, 160), true);
    for (auto& f : m_scriptFiles) {
        std::string name = std::filesystem::path(f).filename().string();
        ImGui::PushID(f.c_str());
        if (ImGui::Button("Load")) {
            if (!m_lua.IsReady()) m_lua.Init(nullptr, 0);
            m_lua.LoadScript(f);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.7f,0.85f,1.0f,1.0f), "Console Output");
    ImGui::BeginChild("LuaLog", ImVec2(0,0), true, ImGuiWindowFlags_HorizontalScrollbar);
    const auto& log = m_lua.GetLog();
    for (auto& line : log) ImGui::TextUnformatted(line.c_str());
    if (!log.empty()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    if (ImGui::Button("Clear Log")) m_lua.ClearLog();
}

// ============================================================
// Hex Editor Tab
// ============================================================
void NovaUI::NovaLauncher::RenderHexEditorTab() {
    ImGui::Text("Memory Hex Editor");
    ImGui::TextDisabled("Browses N64 RDRAM. Only active while a game is running.");
    ImGui::Separator(); ImGui::Spacing();

    if (!m_emuRunning.load()) {
        ImGui::TextDisabled("  Start a game to browse memory.");
        return;
    }

    // For now we don't have a direct RDRAM pointer from the running thread —
    // expose via a static (VideoPlugin already has rdram from SystemInfo).
    // Placeholder: show a dummy 4 MB zero buffer until we wire the pointer.
    static std::vector<uint8_t> s_dummy;
    if (s_dummy.empty()) s_dummy.assign(4 * 1024 * 1024, 0);

    ImGui::BeginChild("HexEdRegion", ImVec2(0,0), false, ImGuiWindowFlags_HorizontalScrollbar);
    m_hexEditor.Render("##hex", s_dummy.data(), s_dummy.size(), 0x80000000u);
    ImGui::EndChild();
}

// ============================================================
// Achievements Tab
// ============================================================
void NovaUI::NovaLauncher::RenderAchievementsTab() {
    ImGui::Text("Achievements Tracker");
    ImGui::Separator();
    ImGui::TextDisabled("Connect your RetroAchievements account to track progress.");
}

// ============================================================
// Simulator helpers
// ============================================================
const char* NovaUI::NovaLauncher::NodeIcon(SimNodeKind k) {
    switch(k) {
        case SimNodeKind::N64Console:    return "[N64]";
        case SimNodeKind::TV:            return "[TV]";
        case SimNodeKind::N64DD:         return "[DD]";
        case SimNodeKind::Controller:    return "[CTL]";
        case SimNodeKind::ExpansionPak:  return "[EXP]";
        case SimNodeKind::Cartridge:     return "[CART]";
        case SimNodeKind::RumblePak:     return "[RMB]";
        case SimNodeKind::ControllerPak: return "[MEM]";
        case SimNodeKind::TransferPak:   return "[T-PAK]";
    }
    return "[?]";
}

const char* NovaUI::NovaLauncher::NodeLabel(SimNodeKind k) {
    switch(k) {
        case SimNodeKind::N64Console:    return "Console Core";
        case SimNodeKind::TV:            return "CRT Display Monitor";
        case SimNodeKind::N64DD:         return "64DD Coprocessor Drive";
        case SimNodeKind::Controller:    return "Control Pad Node";
        case SimNodeKind::ExpansionPak:  return "Expansion Pak RDRAM";
        case SimNodeKind::Cartridge:     return "ROM Media Pak";
        case SimNodeKind::RumblePak:     return "Rumble Force Pak";
        case SimNodeKind::ControllerPak: return "Controller Mem Pak";
        case SimNodeKind::TransferPak:   return "Transfer Pak";
        default:                         return "Accessory Module";
    }
}

void NovaUI::NovaLauncher::SpawnNode(SimNodeKind kind, int cartIndex) {
    SimNode n;
    n.id   = m_nextNodeId++;
    n.kind = kind;
    if (kind == SimNodeKind::Cartridge && cartIndex >= 0 && cartIndex < (int)m_library.size()) {
        strncpy(n.cartName, m_library[cartIndex].name.c_str(), sizeof(n.cartName)-1);
        strncpy(n.cartPath, m_library[cartIndex].path.c_str(), sizeof(n.cartPath)-1);
    }
    m_simNodes.push_back(n);
}

void NovaUI::NovaLauncher::RenderSimNode(SimNode& node) {
    ed::BeginNode(ed::NodeId(node.id));
    ImGui::Text("%s %s", NodeIcon(node.kind),
                node.kind == SimNodeKind::Cartridge ? node.cartName : NodeLabel(node.kind));
    ImGui::TextDisabled("---------------------------------");

    if (node.kind == SimNodeKind::N64Console) {
        for (int i = 1; i <= 4; ++i) {
            ed::BeginPin(ed::PinId(GetPinId(node.id, i)), ed::PinKind::Input);
            ImGui::Text("<- Input %d", i);
            ed::EndPin();
        }
        ImGui::SameLine(120);
        ed::BeginPin(ed::PinId(GetPinId(node.id, 0)), ed::PinKind::Output);
        ImGui::Text("AV Out ->");
        ed::EndPin();
    } else if (node.kind == SimNodeKind::TV) {
        ed::BeginPin(ed::PinId(GetPinId(node.id, 1)), ed::PinKind::Input);
        ImGui::Text("<- Coax AV In");
        ed::EndPin();
        ImGui::Checkbox("Display Power Status", &node.isPowered);
    } else if (node.kind == SimNodeKind::Controller) {
        ImGui::SameLine(50);
        ed::BeginPin(ed::PinId(GetPinId(node.id, 0)), ed::PinKind::Output);
        ImGui::Text("Interface ->");
        ed::EndPin();
    }

    if (node.kind == SimNodeKind::Cartridge || node.kind == SimNodeKind::ExpansionPak ||
        node.kind == SimNodeKind::TransferPak || node.kind == SimNodeKind::RumblePak ||
        node.kind == SimNodeKind::ControllerPak) {
        if (node.kind == SimNodeKind::Cartridge) ImGui::Text("Target: %s", node.cartPath);
        ImGui::Spacing();
        ImGui::Button("[Drag Hardware Module]");
        if (ImGui::BeginDragDropSource()) {
            int uid = node.id;
            ImGui::SetDragDropPayload("HW_NODE", &uid, sizeof(int));
            ImGui::Text("Moving: %s", NodeLabel(node.kind));
            ImGui::EndDragDropSource();
        }
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) node.showConfig = !node.showConfig;

    if (node.showConfig || node.kind == SimNodeKind::N64Console) {
        ImGui::Spacing();
        if (node.kind == SimNodeKind::N64Console) {
            ImGui::TextColored(ImVec4(0.8f,0.8f,0.3f,1.0f), "Core Memory: %d MB", node.ramMB);
            ImGui::Checkbox("System Master Toggle Switch", &node.isPowered);
            if (node.hasExpansionPak) {
                ImGui::TextColored(ImVec4(0.9f,0.2f,0.6f,1.0f), "[Expansion Pak Installed]");
                if (ImGui::Button("Eject Exp. Pak")) { node.hasExpansionPak = false; node.ramMB = 4; SpawnNode(SimNodeKind::ExpansionPak); }
            }
            if (node.cartPath[0] != '\0') {
                ImGui::TextColored(ImVec4(0.0f,1.0f,0.0f,1.0f), "[Mounted Game]: %s", node.cartName);
                if (ImGui::Button("Eject Game Disk")) { node.cartPath[0] = '\0'; }
            }
            if (node.isPowered && node.cartPath[0] != '\0') {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f,0.6f,0.1f,1.0f));
                if (ImGui::Button("RUN INSTANCE", ImVec2(-1,30))) LaunchEmulator(node);
                ImGui::PopStyleColor();
            }
        }
    }
    ed::EndNode();

    if (node.kind == SimNodeKind::N64Console) {
        ed::Suspend();
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("HW_NODE")) {
                int srcId = *(const int*)pl->Data;
                auto it = std::find_if(m_simNodes.begin(), m_simNodes.end(),
                    [srcId](const SimNode& n){ return n.id == srcId; });
                if (it != m_simNodes.end()) {
                    if (it->kind == SimNodeKind::Cartridge) {
                        if (IsN64DDDisk(it->cartPath)) {
                            node.has64DD = true;
                            strncpy(node.ddCartName, it->cartName, sizeof(node.ddCartName)-1);
                            strncpy(node.ddCartPath, it->cartPath, sizeof(node.ddCartPath)-1);
                        } else {
                            strncpy(node.cartName, it->cartName, sizeof(node.cartName)-1);
                            strncpy(node.cartPath, it->cartPath, sizeof(node.cartPath)-1);
                        }
                        m_simNodes.erase(it);
                    } else if (it->kind == SimNodeKind::ExpansionPak) {
                        node.hasExpansionPak = true; node.ramMB = 8;
                        m_simNodes.erase(it);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ed::Resume();
    }
}

// ============================================================
// Simulator Tab
// ============================================================
void NovaUI::NovaLauncher::RenderSimulatorTab() {
    ed::SetCurrentEditor(g_Context);
    ImGui::BeginChild("SimWorkspace", ImVec2(0,-90), false, ImGuiWindowFlags_HorizontalScrollbar);
    ed::Begin("Playground Editor");

    for (auto& node : m_simNodes) RenderSimNode(node);
    for (auto& lnk : m_simLinks)
        ed::Link(ed::LinkId(lnk.id), ed::PinId(lnk.fromPin), ed::PinId(lnk.toPin));

    if (ed::BeginCreate()) {
        ed::PinId fromPin, toPin;
        if (ed::QueryNewLink(&fromPin, &toPin)) {
            if (fromPin && toPin && IsInputPin((int)fromPin.Get()) != IsInputPin((int)toPin.Get())) {
                if (ed::AcceptNewItem(ImVec4(0,1,0,1), 2.0f)) {
                    SimLink lnk;
                    lnk.id      = m_nextLinkId++;
                    lnk.fromPin = IsInputPin((int)fromPin.Get()) ? (int)toPin.Get() : (int)fromPin.Get();
                    lnk.toPin   = IsInputPin((int)fromPin.Get()) ? (int)fromPin.Get() : (int)toPin.Get();
                    m_simLinks.push_back(lnk);
                }
            }
        }
    }
    ed::EndCreate();

    if (ed::BeginDelete()) {
        ed::LinkId delLink;
        while (ed::QueryDeletedLink(&delLink)) {
            if (ed::AcceptDeletedItem())
                m_simLinks.erase(std::remove_if(m_simLinks.begin(), m_simLinks.end(),
                    [&](const SimLink& l){ return l.id == (int)delLink.Get(); }), m_simLinks.end());
        }
    }
    ed::EndDelete();

    ed::End();
    ed::SetCurrentEditor(nullptr);
    ImGui::EndChild();

    ImGui::BeginChild("SimTools", ImVec2(0,0), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::Text("Mode: "); ImGui::SameLine();
    ImGui::RadioButton("Realtime", &m_simMode, 0); ImGui::SameLine();
    ImGui::RadioButton("Sandbox",  &m_simMode, 1); ImGui::SameLine();
    ImGui::Text(" | Spawn: "); ImGui::SameLine();
    if (ImGui::Button("[+] Console"))  SpawnNode(SimNodeKind::N64Console);   ImGui::SameLine();
    if (ImGui::Button("[+] Display"))  SpawnNode(SimNodeKind::TV);           ImGui::SameLine();
    if (ImGui::Button("[+] Controller"))SpawnNode(SimNodeKind::Controller);  ImGui::SameLine();
    if (ImGui::Button("[+] Exp.Pak"))  SpawnNode(SimNodeKind::ExpansionPak); ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f,0.6f,0.4f,1.0f));
    if (ImGui::Button("[ Spawn Cartridge... ]")) ImGui::OpenPopup("CartGridSelectorPopup");
    ImGui::PopStyleColor();

    if (ImGui::BeginPopup("CartGridSelectorPopup")) {
        ImGui::TextColored(ImVec4(0.85f,0.43f,0.26f,1.0f), "Available ROM Images:");
        ImGui::Separator();
        ImGui::Columns(2, "PopupGrid", false);
        for (size_t i = 0; i < m_library.size(); ++i) {
            ImGui::PushID((int)i);
            if (ImGui::Selectable(m_library[i].name.c_str(), false, ImGuiSelectableFlags_DontClosePopups, ImVec2(0,35))) {
                SpawnNode(SimNodeKind::Cartridge, (int)i);
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", m_library[i].path.c_str());
            ImGui::PopID();
            ImGui::NextColumn();
        }
        ImGui::Columns(1);
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    ImGui::Text(" | Target: "); ImGui::SameLine();

    bool signalValid = false; SimNode validNode;
    for (auto& node : m_simNodes) {
        if (node.kind == SimNodeKind::TV && node.isPowered) {
            for (auto& lnk : m_simLinks) {
                if (lnk.toPin == GetPinId(node.id, 1)) {
                    int srcId = GetNodeIdFromPin(lnk.fromPin);
                    for (auto& console : m_simNodes) {
                        if (console.id == srcId && console.kind == SimNodeKind::N64Console &&
                            console.isPowered && console.cartPath[0] != '\0') {
                            signalValid = true; validNode = console;
                        }
                    }
                }
            }
        }
    }

    if (signalValid) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f,0.7f,0.1f,1.0f));
        if (ImGui::Button("RUN EMULATION ON TARGET DISPLAY", ImVec2(260,0)))
            LaunchEmulator(validNode);
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("Signal Missing (Verify Console, Game, TV and Cables)");
    }

    ImGui::EndChild();
}
