// ══════════════════════════════════════════════════════════════════════════════
// NovaGraphics — ImGui Debug UI  (Nemu64-style debugger)
// Panels: Commands (CPU/RSP/RDP disasm), Registers (all tabs), Memory viewer
// ══════════════════════════════════════════════════════════════════════════════

#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <mutex>

struct SDL_Window;
union SDL_Event;

// Forward declarations — avoid pulling in heavy N64 system headers
class CN64System;
class CRegisters;
namespace Nova64 { class MipsMemoryVM; }
namespace Nova64::Video { class VideoPlugin; }

namespace Nova64::Video {

using Nova64::MipsMemoryVM;

class ImGuiDebugUI {
public:
    void Init(SDL_Window* window, void* glContext);
    void Shutdown();
    bool ProcessEvent(const SDL_Event& event);
    void Render();

    void Toggle() { m_visible = !m_visible; }
    bool IsVisible() const { return m_visible; }

    // Bind the N64 system so we can read registers/memory live
    void SetSystem(CN64System* sys) { m_system = sys; }

    // Bind the video plugin so we can read RDP/rendering state live
    void SetVideoPlugin(VideoPlugin* vp) { m_videoPlugin = vp; }

private:
    bool m_visible     = false;
    bool m_initialized = false;

    CN64System*  m_system      = nullptr;
    VideoPlugin* m_videoPlugin = nullptr;

    // ─── Panel draw methods ──────────────────────────────────────
    void DrawCommandsPanel();
    void DrawRegistersPanel();
    void DrawMemoryPanel();
    void DrawRDPStatePanel();

    // Disassembly helpers
    void DrawCPUDisasm(CRegisters* reg, MipsMemoryVM* mem);
    void DrawRSPDisasm(CRegisters* reg, MipsMemoryVM* mem);
    void DrawRDPDisasm();

    // Register tab helpers
    void DrawGPRTab(CRegisters* reg);
    void DrawCOP0Tab(CRegisters* reg);
    void DrawCOP1Tab(CRegisters* reg);
    void DrawMIRegistersTab(CRegisters* reg);
    void DrawSPRegistersTab(CRegisters* reg);
    void DrawDPRegistersTab(CRegisters* reg);
    void DrawVIRegistersTab(CRegisters* reg);
    void DrawSIRegistersTab(CRegisters* reg);
    void DrawPIRegistersTab(CRegisters* reg);
    void DrawRSPGPTab(MipsMemoryVM* mem);
    void DrawRSPVectorTab(MipsMemoryVM* mem);
    void DrawRSPHiddenTab(MipsMemoryVM* mem);

    // Memory panel state
    uint32_t m_memViewAddr   = 0x80000000;
    char     m_memAddrInput[16] = "80000000";
    bool     m_memShowHex    = true;
    bool     m_memShowAscii  = true;
    int      m_memBytesPerRow = 16;

    // Commands panel state
    uint32_t m_cpuJumpAddr   = 0;
    char     m_cpuJumpInput[16] = "";
    int      m_cmdTab        = 0;  // 0=CPU, 1=RSP, 2=RDP
};

extern ImGuiDebugUI g_imguiDebugUI;

} // namespace Nova64::Video
