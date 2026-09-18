/**
 * @file LauncherTools.cpp
 * @brief Scripts, Memory Viewer and Achievements tabs.
 */
#include "NovaLauncher.hpp"

#include "../N64SYS/MainSYS.hpp"
#include "../N64SYS/MIPSY/Vmem.hpp"

#include <imgui.h>

#include <algorithm>
#include <filesystem>

extern CN64System* g_System;

namespace fs = std::filesystem;

namespace NovaUI {

void NovaLauncher::RefreshScriptFiles() {
    m_scriptFiles.clear();
    std::error_code ec;
    fs::create_directories("scripts", ec);
    for (auto it = fs::directory_iterator("scripts", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->path().extension() == ".lua") m_scriptFiles.push_back(it->path().string());
    }
    std::sort(m_scriptFiles.begin(), m_scriptFiles.end());
}

void NovaLauncher::RenderScriptsTab() {
    ImGui::Text("Lua Scripts");
    ImGui::TextDisabled("Put .lua files in the scripts/ folder.");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Refresh List")) RefreshScriptFiles();
    ImGui::SameLine();
    if (ImGui::Button("Unload All")) m_lua.UnloadAll();

    ImGui::BeginChild("ScriptList", ImVec2(0, 160), ImGuiChildFlags_Borders);
    if (m_scriptFiles.empty()) ImGui::TextDisabled("No scripts found.");
    for (const auto& file : m_scriptFiles) {
        ImGui::PushID(file.c_str());
        if (ImGui::Button("Load")) {
            if (!m_lua.IsReady()) m_lua.Init(nullptr, 0);
            m_lua.LoadScript(file);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(fs::path(file).filename().string().c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Text("Output");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        m_lua.ClearLog();
        m_shownLogLines = 0;
    }
    ImGui::BeginChild("LuaLog", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const auto& log = m_lua.GetLog();
    for (const auto& line : log) ImGui::TextUnformatted(line.c_str());
    // Follow new output, but leave the scroll position alone otherwise.
    if (log.size() != m_shownLogLines) {
        ImGui::SetScrollHereY(1.0f);
        m_shownLogLines = log.size();
    }
    ImGui::EndChild();
}

void NovaLauncher::RenderHexEditorTab() {
    ImGui::Text("Memory Viewer");
    ImGui::TextDisabled("Read-only view of the running game's RDRAM.");
    ImGui::Separator();
    ImGui::Spacing();
    Nova64::MipsMemoryVM* mem = (m_emuRunning.load() && g_System) ? g_System->GetMemory() : nullptr;
    const uint8_t* rdram = mem ? mem->Rdram() : nullptr;
    if (!rdram) {
        ImGui::TextDisabled("Start a game to browse its memory.");
        return;
    }
    // RDRAM is kept as host-order 32-bit words; show it in the N64's byte order.
    m_hexEditor.Render(rdram, mem->RdramSize(), 0x80000000u, true);
}

void NovaLauncher::RenderAchievementsTab() {
    ImGui::Text("Achievements");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("RetroAchievements support isn't available yet.");
}

} // namespace NovaUI
