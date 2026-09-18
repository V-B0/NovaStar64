/**
 * @file LauncherPlay.cpp
 * @brief The game running inside the launcher: starting and stopping it, the
 *        game view, the pause menu with its save states, and the rewind bar.
 */
#include "NovaLauncher.hpp"
#include "LauncherInternal.hpp"

#include <SDL.h>
#include <glad/gl.h>
#include <imgui.h>

#include "../N64SYS/MainSYS.hpp"
#include "../N64SYS/MIPSY/Vmem.hpp"
#include "../NovaGraphics/Plugin/FrameCallback.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

extern CN64System* g_System;

namespace fs = std::filesystem;

namespace NovaUI {

namespace {

constexpr float kNavbarHeight     = 40.0f;
constexpr float kRewindThumbW     = 144.0f;
constexpr float kRewindThumbH     = 108.0f;
constexpr float kRewindBarHeight  = 208.0f;
constexpr ImU32 kAccent           = IM_COL32(217, 110, 66, 255);   // the top bar's orange

bool AnyKeyHeld() {
    int count = 0;
    const Uint8* keys = SDL_GetKeyboardState(&count);
    for (int i = 0; i < count; ++i) {
        if (keys[i]) return true;
    }
    return false;
}

GLuint CreateTexture(int width, int height, const uint8_t* rgba) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

void DeleteTexture(uint32_t texture) {
    GLuint name = texture;
    glDeleteTextures(1, &name);
}

// Draws a thumbnail as one item, or a dim box with `emptyLabel` when there is none.
void DrawThumbnail(ThumbnailTextures& textures, const std::shared_ptr<const Nova64::StateThumbnail>& thumbnail,
                   ImVec2 size, const char* emptyLabel) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    if (uint32_t texture = textures.Get(thumbnail)) {
        ImGui::Image(static_cast<ImTextureID>(texture), size);
        return;
    }
    ImGui::Dummy(size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(0, 0, 0, 50));
    if (emptyLabel && *emptyLabel) {
        const ImVec2 text = ImGui::CalcTextSize(emptyLabel);
        draw->AddText(ImVec2(pos.x + (size.x - text.x) * 0.5f, pos.y + (size.y - text.y) * 0.5f),
                      ImGui::GetColorU32(ImGuiCol_TextDisabled), emptyLabel);
    }
}

} // namespace

// ============================================================
// Thumbnail textures
// ============================================================
uint32_t ThumbnailTextures::Get(const std::shared_ptr<const Nova64::StateThumbnail>& thumbnail) {
    if (!thumbnail || thumbnail->width <= 0 || thumbnail->height <= 0 ||
        thumbnail->rgba.size() < static_cast<size_t>(thumbnail->width) * static_cast<size_t>(thumbnail->height) * 4) {
        return 0;
    }
    if (auto it = m_entries.find(thumbnail.get()); it != m_entries.end()) {
        if (it->second.source.lock() == thumbnail) return it->second.texture;
        DeleteTexture(it->second.texture);   // a newer thumbnail reused the address
        m_entries.erase(it);
    }
    Entry entry;
    entry.source  = thumbnail;
    entry.texture = CreateTexture(thumbnail->width, thumbnail->height, thumbnail->rgba.data());
    m_entries.emplace(thumbnail.get(), entry);
    return entry.texture;
}

void ThumbnailTextures::DropUnused() {
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->second.source.expired()) {
            DeleteTexture(it->second.texture);
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
}

void ThumbnailTextures::Clear() {
    for (const auto& [key, entry] : m_entries) DeleteTexture(entry.texture);
    m_entries.clear();
}

// ============================================================
// Starting and stopping
// ============================================================
void NovaLauncher::PlayRom(const std::string& path, const std::string& name) {
    StopEmbedded();
    if (path.empty()) return;

    m_emuRomPath = detail::CopyIntoRomsFolder(path);
    m_emuTitle   = name.empty() ? fs::path(m_emuRomPath).stem().string() : name;
    Nova64::StateService::Instance().BeginSession(m_emuRomPath);
    m_stateRevision = 0;

    // The machine doesn't exist yet, so RDRAM is bound on the first frame.
    m_lua.Init(nullptr, 0);
    Nova64::Video::g_frameCallback = [this]() {
        if (g_System) {
            if (auto* mem = g_System->GetMemory()) m_lua.BindMemory(mem->Rdram(), mem->RdramSize());
        }
        m_lua.OnFrame();
    };

    // "Automatic" must restore whatever the platform default was.
    static const bool kDefaultUseInterpreter = CN64System::s_UseInterpreter;
    CN64System::s_UseInterpreter = (m_launch.cpuCore == 0) ? kDefaultUseInterpreter : (m_launch.cpuCore == 1);

    CN64System::ClearStopRequest();
    Nova64::Video::g_embeddedMode.store(true);
    Nova64::Video::g_emuPaused.store(false);
    Nova64::Video::g_frameReady.store(false);
    m_turboMode = m_showPauseMenu = m_rewindOpen = m_resumePending = false;
    m_emuStartFailed.store(false);
    m_emuRunning.store(true);

    if (m_launch.displayMode == 1 && m_window && !(SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN)) {
        m_wentFullscreen = SDL_SetWindowFullscreen(m_window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0;
    }

    const std::string rom = m_emuRomPath;
    const int speed = m_launch.speed;
    m_emuThread = std::thread([this, rom, speed]() {
        CN64System::s_TargetFPS = speed;
        if (!CN64System::RunFileImage(rom.c_str(), 0)) m_emuStartFailed.store(true);
        m_emuRunning.store(false);
    });
}

void NovaLauncher::LaunchEmulator(const SimNode& node) {
    if (node.cartPath[0] == '\0') return;
    PlayRom(node.cartPath, node.cartName[0] ? node.cartName : fs::path(node.cartPath).stem().string());
}

void NovaLauncher::StopEmbedded() {
    NovaInput::Gamepads::Instance().StopRumble();
    if (m_emuThread.joinable()) {
        // A paused game waits inside the CPU loop; un-pause it so it can stop.
        Nova64::Video::g_emuPaused.store(false);
        CN64System::RequestStop();
        m_emuThread.join();
        CN64System::CloseSystem();
        // Shutting down the game's renderer can leave no GL context current.
        if (m_window && m_glContext) SDL_GL_MakeCurrent(m_window, static_cast<SDL_GLContext>(m_glContext));
        // The video plugin rewrites this file when it shuts down.
        m_gfx.LoadFromFile(detail::kGraphicsPath);
    }

    Nova64::StateService::Instance().EndSession();
    Nova64::Video::g_embeddedMode.store(false);
    Nova64::Video::g_emuPaused.store(false);
    Nova64::Video::g_frameCallback = nullptr;
    Nova64::Video::g_frameReady.store(false);
    m_emuRunning.store(false);
    m_turboMode = m_showPauseMenu = m_rewindOpen = m_resumePending = false;
    m_overwriteSlot = -1;
    m_rewindPoints.clear();

    // No GL calls once Run() has destroyed the context.
    if (m_glContext) {
        m_thumbnails.Clear();
        if (m_gameTex) DeleteTexture(m_gameTex);
    }
    m_gameTex = 0;
    m_gameTexW = m_gameTexH = 0;

    if (m_wentFullscreen && m_window) SDL_SetWindowFullscreen(m_window, 0);
    m_wentFullscreen = false;
}

void NovaLauncher::UpdatePlayState() {
    if (m_emuThread.joinable() && !m_emuRunning.load()) {
        const bool failed = m_emuStartFailed.load();
        StopEmbedded();
        ShowStatus(failed ? "Couldn't start that game." : "The game stopped.");
    }

    if (m_resumePending && !AnyKeyHeld() && !NovaInput::Gamepads::Instance().AnyButtonHeld()) {
        m_resumePending = false;
        Nova64::Video::g_emuPaused.store(false);
    }

    uint64_t sequence = 0;
    const std::string message = Nova64::StateService::Instance().LastMessage(sequence);
    if (sequence != m_statusSequence) {
        m_statusSequence = sequence;
        if (!message.empty()) ShowStatus(message);
    }

    if (m_emuRunning.load()) {
        SyncStateLists();
        // Frame rate: video fields per wall-clock second, sampled once a second.
        const uint32_t now = SDL_GetTicks();
        if (now - m_fpsLastTick >= 1000) {
            const uint64_t fields = g_System ? g_System->GetCurrentFrameCount() : 0;
            if (m_fpsLastTick != 0 && fields >= m_fpsLastField)
                m_fps = static_cast<float>(fields - m_fpsLastField) * 1000.0f / static_cast<float>(now - m_fpsLastTick);
            m_fpsLastField = fields;
            m_fpsLastTick  = now;
        }
    } else {
        m_fps = 0.0f;
        m_fpsLastTick = 0;
    }
}

void NovaLauncher::ShowStatus(const std::string& text) {
    m_statusText  = text;
    m_statusUntil = SDL_GetTicks() + 4000;
}

// ============================================================
// Pause and rewind
// ============================================================
bool NovaLauncher::HandleGameKey(int scancode, bool repeat) {
    if (m_rewindOpen) {
        switch (scancode) {
            case SDL_SCANCODE_LEFT:  MoveRewindSelection(-1); return true;
            case SDL_SCANCODE_RIGHT: MoveRewindSelection(+1); return true;
            case SDL_SCANCODE_HOME:  MoveRewindSelection(-static_cast<int>(m_rewindPoints.size()) - 1); return true;
            case SDL_SCANCODE_END:   MoveRewindSelection(static_cast<int>(m_rewindPoints.size()) + 1); return true;
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_KP_ENTER:
                if (!repeat) CloseRewind(true);
                return true;
            case SDL_SCANCODE_ESCAPE:
            case SDL_SCANCODE_BACKSPACE:
                if (!repeat) CloseRewind(false);
                return true;
            default:
                return false;
        }
    }
    if (repeat) return false;

    switch (scancode) {
        case SDL_SCANCODE_ESCAPE:
            if (m_showPauseMenu) ClosePauseMenu();
            else                 OpenPauseMenu();
            return true;
        case SDL_SCANCODE_BACKSPACE:
            if (m_launch.rewind) OpenRewind();
            else ShowStatus("Rewind is turned off in Settings > Emulation.");
            return true;
        case SDL_SCANCODE_TAB:
            if (m_showPauseMenu) return false;
            SetTurbo(!m_turboMode);
            return true;
        default:
            return false;
    }
}

bool NovaLauncher::HandleGamePadButton(int button) {
    // In the rewind strip the D-pad picks a picture, the bumpers jump to the
    // ends, A carries on from the picture and B backs out. Nothing reaches the
    // paused game.
    if (m_rewindOpen) {
        switch (button) {
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return HandleGameKey(SDL_SCANCODE_LEFT, false);
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return HandleGameKey(SDL_SCANCODE_RIGHT, false);
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return HandleGameKey(SDL_SCANCODE_HOME, false);
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return HandleGameKey(SDL_SCANCODE_END, false);
            case SDL_CONTROLLER_BUTTON_A:             return HandleGameKey(SDL_SCANCODE_RETURN, false);
            case SDL_CONTROLLER_BUTTON_B:
            case SDL_CONTROLLER_BUTTON_BACK:          return HandleGameKey(SDL_SCANCODE_ESCAPE, false);
            default:                                  return true;
        }
    }
    // Back (View, Select, Share) or Guide opens and closes the pause menu, like Esc.
    if (button == SDL_CONTROLLER_BUTTON_BACK || button == SDL_CONTROLLER_BUTTON_GUIDE)
        return HandleGameKey(SDL_SCANCODE_ESCAPE, false);
    // B leaves the pause menu, unless it is asking whether to replace a save.
    if (m_showPauseMenu && button == SDL_CONTROLLER_BUTTON_B && m_overwriteSlot < 0) {
        ClosePauseMenu();
        return true;
    }
    return false;
}

void NovaLauncher::OpenPauseMenu() {
    m_showPauseMenu  = true;
    m_focusPauseMenu = true;
    m_resumePending  = false;
    Nova64::Video::g_emuPaused.store(true);
    NovaInput::Gamepads::Instance().StopRumble();
}

void NovaLauncher::ClosePauseMenu() {
    m_showPauseMenu = false;
    m_overwriteSlot = -1;
    ResumeGame();
}

void NovaLauncher::ResumeGame() {
    m_resumePending = true;
}

void NovaLauncher::OpenRewind() {
    if (!m_emuRunning.load()) return;
    m_rewindFromPauseMenu = m_showPauseMenu;
    m_showPauseMenu = false;
    m_rewindOpen    = true;
    m_resumePending = false;
    Nova64::Video::g_emuPaused.store(true);
    NovaInput::Gamepads::Instance().StopRumble();

    SyncStateLists();
    m_rewindSelection         = static_cast<int>(m_rewindPoints.size());   // "Now"
    m_rewindScrollToSelection = true;
    m_rewindWheel             = 0.0f;
}

void NovaLauncher::CloseRewind(bool jumpToSelection) {
    const bool jump = jumpToSelection && m_rewindSelection >= 0 &&
                      m_rewindSelection < static_cast<int>(m_rewindPoints.size());
    if (jump) Nova64::StateService::Instance().RequestRewind(m_rewindPoints[m_rewindSelection].id);
    m_rewindOpen = false;

    // Backing out of a rewind opened from the pause menu returns to the menu.
    if (!jump && m_rewindFromPauseMenu && !jumpToSelection) {
        m_showPauseMenu  = true;
        m_focusPauseMenu = true;
        return;
    }
    ResumeGame();
}

void NovaLauncher::MoveRewindSelection(int delta) {
    const int now = static_cast<int>(m_rewindPoints.size());
    m_rewindSelection = std::clamp(m_rewindSelection + delta, 0, now);
    m_rewindScrollToSelection = true;
}

void NovaLauncher::SyncStateLists() {
    const auto& service = Nova64::StateService::Instance();
    const uint64_t revision = service.Revision();
    if (revision == m_stateRevision) return;
    m_stateRevision = revision;

    // Keep the same point selected, or "Now" if that was selected.
    const bool atNow = m_rewindSelection >= static_cast<int>(m_rewindPoints.size());
    const uint64_t selectedId = (!atNow && m_rewindSelection >= 0) ? m_rewindPoints[m_rewindSelection].id : 0;
    m_rewindPoints = service.RewindPoints();
    m_rewindSelection = static_cast<int>(m_rewindPoints.size());
    if (!atNow) {
        const auto it = std::find_if(m_rewindPoints.begin(), m_rewindPoints.end(),
                                     [selectedId](const Nova64::RewindPoint& p) { return p.id == selectedId; });
        if (it != m_rewindPoints.end()) m_rewindSelection = static_cast<int>(it - m_rewindPoints.begin());
    }
    m_thumbnails.DropUnused();
}

// ============================================================
// Drawing
// ============================================================

// Uploads the pixels the video plugin handed over into the launcher's texture.
void NovaLauncher::UploadFromPixelBuf() {
    if (!Nova64::Video::g_frameReady.load(std::memory_order_acquire)) return;

    std::lock_guard<std::mutex> lk(Nova64::Video::g_frameMutex);
    const int w = Nova64::Video::g_frameW.load(std::memory_order_relaxed);
    const int h = Nova64::Video::g_frameH.load(std::memory_order_relaxed);
    if (w <= 0 || h <= 0 || Nova64::Video::g_framePixels.size() < static_cast<size_t>(w) * static_cast<size_t>(h) * 4) return;

    if (m_gameTex == 0 || w != m_gameTexW || h != m_gameTexH) {
        if (m_gameTex) DeleteTexture(m_gameTex);
        m_gameTex  = CreateTexture(w, h, Nova64::Video::g_framePixels.data());
        m_gameTexW = w;
        m_gameTexH = h;
    } else {
        glBindTexture(GL_TEXTURE_2D, m_gameTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, Nova64::Video::g_framePixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    Nova64::Video::g_frameReady.store(false, std::memory_order_relaxed);
}

// Draws the game behind the UI, letterboxed between the top bar and the rewind
// bar. Uses the background draw list, so it never takes input from the overlay.
void NovaLauncher::RenderGameView() {
    UploadFromPixelBuf();

    uint32_t texture = m_gameTex;
    float aspect = (m_gameTexH > 0) ? static_cast<float>(m_gameTexW) / static_cast<float>(m_gameTexH) : 4.0f / 3.0f;
    // While picking a rewind point, show that point's picture.
    if (m_rewindOpen && m_rewindSelection >= 0 && m_rewindSelection < static_cast<int>(m_rewindPoints.size())) {
        const auto& thumbnail = m_rewindPoints[m_rewindSelection].thumbnail;
        if (uint32_t tex = m_thumbnails.Get(thumbnail)) {
            texture = tex;
            aspect  = static_cast<float>(thumbnail->width) / static_cast<float>(thumbnail->height);
        }
    }
    if (!texture) return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float top    = kNavbarHeight;
    const float height = std::max(1.0f, display.y - top - (m_rewindOpen ? kRewindBarHeight : 0.0f));
    float imgW = display.x;
    float imgH = imgW / aspect;
    if (imgH > height) {
        imgH = height;
        imgW = imgH * aspect;
    }
    const float ox = (display.x - imgW) * 0.5f;
    const float oy = top + (height - imgH) * 0.5f;
    ImGui::GetBackgroundDrawList()->AddImage(static_cast<ImTextureID>(texture), ImVec2(ox, oy), ImVec2(ox + imgW, oy + imgH));
}

void NovaLauncher::RenderPauseMenu() {
    if (!m_showPauseMenu) return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 170));

    // Fixed width, height fitted to the contents, centred below the top bar.
    const float pw   = std::min(640.0f, display.x - 40.0f);
    const float maxH = display.y - kNavbarHeight - 40.0f;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, kNavbarHeight + (display.y - kNavbarHeight) * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(pw, 0.0f), ImVec2(pw, maxH));
    ImGui::SetNextWindowBgAlpha(0.96f);
    if (m_focusPauseMenu) {
        ImGui::SetNextWindowFocus();
        m_focusPauseMenu = false;
    }
    ImGui::Begin("##PauseMenu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::SetWindowFontScale(1.4f);
    ImGui::TextColored(ImVec4(0.85f, 0.43f, 0.26f, 1.0f), "Paused");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine();
    ImGui::TextDisabled("  %s", m_emuTitle.c_str());
    ImGui::Separator();
    ImGui::Spacing();

    bool quit = false;
    if (ImGui::Button("Resume (Esc)", ImVec2(180, 34))) ClosePauseMenu();
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_launch.rewind);
    if (ImGui::Button("Rewind (Backspace)", ImVec2(180, 34))) OpenRewind();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Quit Game", ImVec2(180, 34))) quit = true;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    RenderSaveSlots();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Speed");
    ImGui::SameLine();
    if (ImGui::RadioButton("Normal", !m_turboMode)) SetTurbo(false);
    ImGui::SameLine();
    ImGui::BeginDisabled(m_launch.speed == 0);
    if (ImGui::RadioButton("Double (Tab)", m_turboMode)) SetTurbo(true);
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Controls")) {
        // The binding table is tall; it scrolls inside the menu instead of stretching it.
        ImGui::BeginChild("##Controls", ImVec2(0, std::min(300.0f, maxH * 0.5f)), ImGuiChildFlags_Borders);
        RenderInputSettingsWidget();
        ImGui::EndChild();
    }
    ImGui::End();

    if (quit) {
        StopEmbedded();
        m_activeTab = Tab::Home;
    }
}

void NovaLauncher::RenderSaveSlots() {
    auto& service = Nova64::StateService::Instance();
    const bool busy = service.RequestPending();

    ImGui::Text("Save states");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", busy ? "  working..." : "  kept in the saves folder");
    ImGui::Spacing();

    constexpr ImVec2 kThumb(96.0f, 72.0f);
    if (ImGui::BeginTable("##SaveSlots", 2, ImGuiTableFlags_SizingStretchSame)) {
        for (int i = 0; i < Nova64::kSaveSlots; ++i) {
            if (i % 2 == 0) ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const Nova64::SaveSlotInfo slot = service.Slot(i);

            ImGui::PushID(i);
            DrawThumbnail(m_thumbnails, slot.thumbnail, kThumb, slot.exists ? "" : "Empty");
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::Text("Slot %d", i + 1);
            ImGui::TextDisabled("%s", slot.SavedAtText().c_str());
            ImGui::BeginDisabled(busy);
            if (ImGui::Button("Save", ImVec2(64, 0))) {
                if (slot.exists) {
                    m_overwriteSlot = i;
                    m_openOverwrite = true;
                } else {
                    service.RequestSave(i);
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!slot.loadable);
            if (ImGui::Button("Load", ImVec2(64, 0))) {
                service.RequestLoad(i);
                ClosePauseMenu();
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::EndGroup();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (m_openOverwrite) {
        ImGui::OpenPopup("Replace save state?");
        m_openOverwrite = false;
    }
    if (ImGui::BeginPopupModal("Replace save state?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Slot %d already has a save state. Replace it?", m_overwriteSlot + 1);
        ImGui::Spacing();
        if (ImGui::Button("Replace", ImVec2(110, 0))) {
            if (m_overwriteSlot >= 0) service.RequestSave(m_overwriteSlot);
            m_overwriteSlot = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0))) {
            m_overwriteSlot = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void NovaLauncher::RenderRewindBar() {
    if (!m_rewindOpen) return;

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    const auto& service = Nova64::StateService::Instance();
    const int count = static_cast<int>(m_rewindPoints.size());

    // The mouse wheel steps through the pictures; rolling up goes further back.
    m_rewindWheel += io.MouseWheel;
    while (m_rewindWheel >= 1.0f)  { MoveRewindSelection(-1); m_rewindWheel -= 1.0f; }
    while (m_rewindWheel <= -1.0f) { MoveRewindSelection(+1); m_rewindWheel += 1.0f; }

    ImGui::SetNextWindowPos(ImVec2(0, display.y - kRewindBarHeight), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(display.x, kRewindBarHeight), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::Begin("##RewindBar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::TextColored(ImVec4(0.85f, 0.43f, 0.26f, 1.0f), "Rewind");
    ImGui::SameLine();
    ImGui::TextDisabled("Scroll or press Left/Right to pick a moment. Enter plays from there, Esc cancels.");

    bool play = false, cancel = false;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttonsWidth = 150.0f + 90.0f + style.ItemSpacing.x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - buttonsWidth - style.WindowPadding.x));
    ImGui::BeginDisabled(m_rewindSelection >= count);
    if (ImGui::Button("Play from here", ImVec2(150, 0))) play = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90, 0))) cancel = true;

    ImGui::BeginChild("##RewindStrip", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (count == 0) {
        ImGui::TextDisabled("No rewind points yet. One is kept every %d seconds of play.",
                            Nova64::StateService::kRewindIntervalSeconds);
    }
    const float fps = static_cast<float>(std::max(1, service.FieldsPerSecond()));
    const uint64_t now = std::max<uint64_t>(service.CurrentFrame(), count ? m_rewindPoints.back().frame : 0);
    const ImVec2 thumbSize(kRewindThumbW, kRewindThumbH);
    for (int i = 0; i <= count; ++i) {
        const bool isNow = (i == count);
        const bool selected = (i == m_rewindSelection);

        ImGui::PushID(i);
        ImGui::BeginGroup();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        DrawThumbnail(m_thumbnails, isNow ? nullptr : m_rewindPoints[i].thumbnail, thumbSize, isNow ? "Now" : "");
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            m_rewindSelection = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) play = true;
        }
        ImGui::GetWindowDrawList()->AddRect(pos, ImVec2(pos.x + thumbSize.x, pos.y + thumbSize.y),
                                            selected ? kAccent : IM_COL32(255, 255, 255, 60), 0.0f, 0,
                                            selected ? 4.0f : 1.0f);

        char label[32];
        if (isNow) {
            std::snprintf(label, sizeof(label), "Now");
        } else {
            const float seconds = static_cast<float>(now - std::min(now, m_rewindPoints[i].frame)) / fps;
            std::snprintf(label, sizeof(label), seconds < 1.0f ? "just now" : "%.0f s ago", seconds);
        }
        const float labelW = ImGui::CalcTextSize(label).x;
        ImGui::SetCursorScreenPos(ImVec2(pos.x + (thumbSize.x - labelW) * 0.5f, pos.y + thumbSize.y + 4.0f));
        if (selected) ImGui::TextUnformatted(label);
        else          ImGui::TextDisabled("%s", label);
        ImGui::EndGroup();

        if (selected && m_rewindScrollToSelection) {
            ImGui::SetScrollHereX(0.5f);
            m_rewindScrollToSelection = false;
        }
        ImGui::PopID();
        if (!isNow) ImGui::SameLine();
    }
    ImGui::EndChild();
    ImGui::End();

    if (play && m_rewindSelection < count) CloseRewind(true);
    else if (cancel)                       CloseRewind(false);
}

} // namespace NovaUI
