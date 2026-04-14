/**
 * @file ImGuiDebugUI.cpp
 * @brief ImGui Debug UI — Nemu64-style debugger.
 */
// ══════════════════════════════════════════════════════════════════════════════
// NovaGraphics — ImGui Debug UI — Nemu64-style debugger
// ══════════════════════════════════════════════════════════════════════════════

#include "ImGuiDebugUI.hpp"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <SDL.h>
#include <cstdio>
#include <cstring>

// N64 system access
#include "../../N64SYS/MainSYS.hpp"
#include "../../N64SYS/MIPSY/Register.hpp"
#include "../../N64SYS/MIPSY/Vmem.hpp"
#include "../../N64SYS/MIPSY/R4300Instruction.hpp"

// Video plugin / RDP state access
#include "../Plugin/VideoPlugin.hpp"
#include "../Core/RDPProcessor.hpp"
#include "../Core/FramebufferManager.hpp"

namespace Nova64::Video {

using Nova64::MipsMemoryVM;

ImGuiDebugUI g_imguiDebugUI;

// ─── Init / Shutdown ─────────────────────────────────────────

void ImGuiDebugUI::Init(SDL_Window* window, void* glContext) {
    if (m_initialized) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 3.0f;
    style.FrameRounding  = 2.0f;
    style.Alpha          = 0.95f;
    style.ItemSpacing    = ImVec2(6, 3);
    style.FramePadding   = ImVec2(4, 2);

    ImGui_ImplSDL2_InitForOpenGL(window, glContext);
    ImGui_ImplOpenGL3_Init("#version 460");
    m_initialized = true;
}

void ImGuiDebugUI::Shutdown() {
    if (!m_initialized) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
}

bool ImGuiDebugUI::ProcessEvent(const SDL_Event& event) {
    if (!m_initialized) return false;
    ImGui_ImplSDL2_ProcessEvent(&event);
    return ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard;
}

void ImGuiDebugUI::CycleDebugOutput() {
    if (!m_videoPlugin) return;
    auto& rdp = m_videoPlugin->GetRDPProcessor();
    if (!rdp) return;
    rdp->CycleDebugOutputMode();
    static const char* names[] = { "Normal","Texel0","Shade","PrimColor","EnvColor","UV Coords","Depth","Alpha" };
    int mode = rdp->GetDebugOutputMode();
    fprintf(stderr, "[Debug] Shader output mode: %d (%s)\n", mode, names[mode]);
}

void ImGuiDebugUI::ToggleForceOpaque() {
    if (!m_videoPlugin) return;
    auto& rdp = m_videoPlugin->GetRDPProcessor();
    if (!rdp) return;
    rdp->ToggleDebugForceOpaque();
    fprintf(stderr, "[Debug] Force Opaque: %s\n", rdp->IsDebugForceOpaque() ? "ON" : "OFF");
}

void ImGuiDebugUI::ToggleDisableDepth() {
    if (!m_videoPlugin) return;
    auto& rdp = m_videoPlugin->GetRDPProcessor();
    if (!rdp) return;
    rdp->ToggleDebugDisableDepth();
    fprintf(stderr, "[Debug] Disable Depth: %s\n", rdp->IsDebugDisableDepth() ? "ON" : "OFF");
}

void ImGuiDebugUI::ToggleDisableCulling() {
    if (!m_videoPlugin) return;
    auto& rdp = m_videoPlugin->GetRDPProcessor();
    if (!rdp) return;
    rdp->ToggleDebugDisableCulling();
    fprintf(stderr, "[Debug] Disable Culling: %s\n", rdp->IsDebugDisableCulling() ? "ON" : "OFF");
}

void ImGuiDebugUI::ToggleMagenta() {
    if (!m_videoPlugin) return;
    auto& rdp = m_videoPlugin->GetRDPProcessor();
    if (!rdp) return;
    rdp->ToggleDebugMagenta();
    fprintf(stderr, "[Debug] Magenta Highlight: %s\n", rdp->IsDebugMagenta() ? "ON" : "OFF");
}

// ─── Render ──────────────────────────────────────────────────

void ImGuiDebugUI::Render() {
    if (!m_initialized || !m_visible) return;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    DrawCommandsPanel();
    DrawRegistersPanel();
    DrawMemoryPanel();
    DrawRDPStatePanel();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// ═══════════════════════════════════════════════════════════════
//  Commands Panel — CPU / RSP / RDP disassembly with controls
// ═══════════════════════════════════════════════════════════════

void ImGuiDebugUI::DrawCommandsPanel() {
    ImGui::SetNextWindowPos(ImVec2(580, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460, 420), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Commands", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    CRegisters* reg = nullptr;
    MipsMemoryVM* mem = nullptr;
    if (m_system) {
        reg = m_system->GetRegisters();
        mem = m_system->GetMemory();
    }

    if (ImGui::BeginTabBar("CmdTabs")) {
        if (ImGui::BeginTabItem("CPU")) {
            DrawCPUDisasm(reg, mem);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("RSP")) {
            DrawRSPDisasm(reg, mem);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("RDP")) {
            DrawRDPDisasm();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

void ImGuiDebugUI::DrawCPUDisasm(CRegisters* reg, MipsMemoryVM* mem) {
    if (!reg || !mem) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "System not initialized");
        return;
    }

    uint32_t pc = static_cast<uint32_t>(reg->m_PROGRAM_COUNTER);

    // Jump to PC / address input
    ImGui::Text("PC: %08X", pc);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputText("##jumpaddr", m_cpuJumpInput, sizeof(m_cpuJumpInput),
                     ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    if (ImGui::SmallButton("Jump to PC")) {
        m_cpuJumpAddr = pc;
        snprintf(m_cpuJumpInput, sizeof(m_cpuJumpInput), "%08X", pc);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Go##jump")) {
        unsigned long long parsed = 0;
        if (sscanf(m_cpuJumpInput, "%llx", &parsed) == 1)
            m_cpuJumpAddr = static_cast<uint32_t>(parsed);
    }

    ImGui::Separator();

    // Disassembly table
    uint32_t startAddr = m_cpuJumpAddr ? m_cpuJumpAddr : (pc - 0x40);
    if (startAddr == 0) startAddr = pc > 0x40 ? pc - 0x40 : 0x80000000;

    if (ImGui::BeginTable("##cpudisasm", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
            ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Parameters", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int i = 0; i < 64; ++i) {
            uint32_t addr = startAddr + i * 4;
            uint32_t word = 0;
            mem->MemoryValue32(addr, word);

            R4300iInstruction instr(addr, word);

            ImGui::TableNextRow();

            // Highlight current PC row
            if (addr == pc) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                    ImGui::GetColorU32(ImVec4(0.2f, 0.35f, 0.6f, 0.65f)));
            }

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%08X", addr);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(instr.GetName().c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(instr.GetParam().c_str());
        }
        ImGui::EndTable();
    }
}

void ImGuiDebugUI::DrawRSPDisasm(CRegisters* reg, MipsMemoryVM* mem) {
    if (!reg || !mem) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "System not initialized");
        return;
    }

    auto* sp = mem->SPRegisters();
    if (!sp) { ImGui::Text("No SP registers"); return; }

    uint32_t rspPC = sp->m_SPRegs[8]; // SP_PC
    ImGui::Text("RSP PC: %03X", rspPC & 0xFFF);
    ImGui::Separator();

    uint8_t* imem = sp->Imem();
    if (!imem) { ImGui::Text("No IMEM"); return; }

    uint32_t startAddr = (rspPC > 0x40) ? (rspPC - 0x40) : 0;
    startAddr &= 0xFFC;

    if (ImGui::BeginTable("##rspdisasm", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
            ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Parameters", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int i = 0; i < 64; ++i) {
            uint32_t addr = (startAddr + i * 4) & 0xFFF;
            uint32_t word = (imem[addr] << 24) | (imem[addr+1] << 16) |
                            (imem[addr+2] << 8) | imem[addr+3];

            // RSP uses same MIPS instruction encoding for scalar ops
            R4300iInstruction instr(0x04001000 | addr, word);

            ImGui::TableNextRow();
            if (addr == (rspPC & 0xFFF)) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                    ImGui::GetColorU32(ImVec4(0.2f, 0.35f, 0.6f, 0.65f)));
            }

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%03X", addr);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(instr.GetName().c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(instr.GetParam().c_str());
        }
        ImGui::EndTable();
    }
}

void ImGuiDebugUI::DrawRDPDisasm() {
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "(RDP command trace — future)");
}

// ═══════════════════════════════════════════════════════════════
//  Registers Panel — matches Nemu64 tab layout
// ═══════════════════════════════════════════════════════════════

void ImGuiDebugUI::DrawRegistersPanel() {
    ImGui::SetNextWindowPos(ImVec2(10, 440), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 330), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Registers", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    CRegisters* reg = nullptr;
    MipsMemoryVM* mem = nullptr;
    if (m_system) {
        reg = m_system->GetRegisters();
        mem = m_system->GetMemory();
    }

    if (!reg) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "System not initialized");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("RegTabs")) {
        if (ImGui::BeginTabItem("General Purpose")) { DrawGPRTab(reg); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("COP 0"))           { DrawCOP0Tab(reg); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("COP 1"))           { DrawCOP1Tab(reg); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("MI Registers"))     { DrawMIRegistersTab(reg); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("SP Registers"))     { DrawSPRegistersTab(reg); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("DP Registers"))     { DrawDPRegistersTab(reg); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("VI Registers"))     { DrawVIRegistersTab(reg); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("SI Registers"))     { DrawSIRegistersTab(reg); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("PI Registers"))     { DrawPIRegistersTab(reg); ImGui::EndTabItem(); }
        if (mem) {
            if (ImGui::BeginTabItem("RSP GP"))       { DrawRSPGPTab(mem); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("RSP Vector"))   { DrawRSPVectorTab(mem); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("RSP Hidden"))   { DrawRSPHiddenTab(mem); ImGui::EndTabItem(); }
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

// Helper: draw two columns of registers like Nemu64
static void DrawRegPair(const char* name1, uint64_t val1,
                         const char* name2, uint64_t val2) {
    ImGui::Text("%-6s: %08X - %08X",
        name1, (uint32_t)(val1 >> 32), (uint32_t)(val1 & 0xFFFFFFFF));
    ImGui::SameLine(280);
    ImGui::Text("%-6s: %08X - %08X",
        name2, (uint32_t)(val2 >> 32), (uint32_t)(val2 & 0xFFFFFFFF));
}

void ImGuiDebugUI::DrawGPRTab(CRegisters* reg) {
    static const char* names[32] = {
        "R0",  "AT",  "V0",  "V1",  "A0",  "A1",  "A2",  "A3",
        "T0",  "T1",  "T2",  "T3",  "T4",  "T5",  "T6",  "T7",
        "S0",  "S1",  "S2",  "S3",  "S4",  "S5",  "S6",  "S7",
        "T8",  "T9",  "K0",  "K1",  "GP",  "SP",  "FP",  "RA"
    };

    ImGui::BeginChild("##gpr", ImVec2(0, 0), ImGuiChildFlags_None);

    // Two-column layout: Reg 0-15 left, Reg 16-31 right
    for (int i = 0; i < 16; ++i) {
        char lbl1[32], lbl2[32];
        snprintf(lbl1, sizeof(lbl1), "Reg %d - %s", i, names[i]);
        snprintf(lbl2, sizeof(lbl2), "Reg %d - %s", i + 16, names[i + 16]);
        DrawRegPair(lbl1, reg->m_GPR[i], lbl2, reg->m_GPR[i + 16]);
    }

    ImGui::Separator();
    ImGui::Text("HI: %08X - %08X", (uint32_t)(reg->m_HI >> 32), (uint32_t)(reg->m_HI & 0xFFFFFFFF));
    ImGui::SameLine(280);
    ImGui::Text("LO: %08X - %08X", (uint32_t)(reg->m_LO >> 32), (uint32_t)(reg->m_LO & 0xFFFFFFFF));

    ImGui::EndChild();
}

void ImGuiDebugUI::DrawCOP0Tab(CRegisters* reg) {
    static const char* cop0Names[32] = {
        "Index",    "Random",   "EntryLo0", "EntryLo1",
        "Context",  "PageMask", "Wired",    "Reg7",
        "BadVAddr", "Count",    "EntryHi",  "Compare",
        "Status",   "Cause",    "EPC",      "PRId",
        "Config",   "LLAddr",   "WatchLo",  "WatchHi",
        "XContext", "Reg21",    "Reg22",    "Reg23",
        "Reg24",    "Reg25",    "ECC",      "CacheErr",
        "TagLo",    "TagHi",    "ErrEPC",   "Reg31"
    };

    // COP0 register indices that exist in the enum
    static const uint8_t cop0Indices[] = {
        0,1,2,3,4,5,6,8,9,10,11,12,13,14,15,16,17,18,19,20,28,29,30
    };

    ImGui::BeginChild("##cop0", ImVec2(0, 0), ImGuiChildFlags_None);

    for (int idx : cop0Indices) {
        auto cop0Reg = static_cast<CRegisters::COP0Reg>(idx);
        uint64_t val = reg->Cop0_MF(cop0Reg);
        ImGui::Text("%-10s [%2d]: %016llX",
            cop0Names[idx], idx, (unsigned long long)val);
    }

    ImGui::EndChild();
}

void ImGuiDebugUI::DrawCOP1Tab(CRegisters* reg) {
    ImGui::BeginChild("##cop1", ImVec2(0, 0), ImGuiChildFlags_None);

    // Show FPRs as both hex and float
    for (int i = 0; i < 16; ++i) {
        float f1 = 0, f2 = 0;
        std::memcpy(&f1, &reg->m_FPR[i], sizeof(float));
        std::memcpy(&f2, &reg->m_FPR[i + 16], sizeof(float));
        ImGui::Text("F%-2d: %016llX (%.6g)", i,
            (unsigned long long)reg->m_FPR[i], f1);
        ImGui::SameLine(290);
        ImGui::Text("F%-2d: %016llX (%.6g)", i + 16,
            (unsigned long long)reg->m_FPR[i + 16], f2);
    }

    ImGui::Separator();
    ImGui::Text("FCSR (FPCR[31]): %08X", reg->m_FPCR[31]);

    ImGui::EndChild();
}

void ImGuiDebugUI::DrawMIRegistersTab(CRegisters* reg) {
    static const char* names[] = { "MI_MODE", "MI_VERSION", "MI_INTR", "MI_INTR_MASK" };
    for (int i = 0; i < 4; ++i)
        ImGui::Text("%-16s: %08X", names[i], reg->m_Mips_Interface[i]);
}

void ImGuiDebugUI::DrawSPRegistersTab(CRegisters* reg) {
    static const char* names[] = {
        "SP_MEM_ADDR", "SP_DRAM_ADDR", "SP_RD_LEN", "SP_WR_LEN",
        "SP_STATUS", "SP_DMA_FULL", "SP_DMA_BUSY", "SP_SEMAPHORE",
        "SP_PC", "SP_IBIST"
    };
    for (int i = 0; i < 10; ++i)
        ImGui::Text("%-16s: %08X", names[i], reg->m_SigProcessor_Interface[i]);
}

void ImGuiDebugUI::DrawDPRegistersTab(CRegisters* reg) {
    static const char* names[] = {
        "DPC_START", "DPC_END", "DPC_CURRENT", "DPC_STATUS",
        "DPC_CLOCK", "DPC_BUFBUSY", "DPC_PIPEBUSY", "DPC_TMEM",
        "DPC_REG8", "DPC_REG9"
    };
    for (int i = 0; i < 10; ++i)
        ImGui::Text("%-16s: %08X", names[i], reg->m_Display_ControlReg[i]);
}

void ImGuiDebugUI::DrawVIRegistersTab(CRegisters* reg) {
    static const char* names[] = {
        "VI_STATUS", "VI_ORIGIN", "VI_WIDTH", "VI_INTR",
        "VI_CURRENT", "VI_BURST", "VI_V_SYNC", "VI_H_SYNC",
        "VI_LEAP", "VI_H_START", "VI_V_START", "VI_V_BURST",
        "VI_X_SCALE", "VI_Y_SCALE"
    };
    for (int i = 0; i < 14; ++i)
        ImGui::Text("%-16s: %08X", names[i], reg->m_Video_Interface[i]);
}

void ImGuiDebugUI::DrawSIRegistersTab(CRegisters* reg) {
    static const char* names[] = {
        "SI_DRAM_ADDR", "SI_PIF_AD_RD64B", "SI_PIF_AD_WR64B", "SI_STATUS"
    };
    for (int i = 0; i < 4; ++i)
        ImGui::Text("%-16s: %08X", names[i], reg->m_SerialInterface[i]);
}

void ImGuiDebugUI::DrawPIRegistersTab(CRegisters* reg) {
    static const char* names[] = {
        "PI_DRAM_ADDR", "PI_CART_ADDR", "PI_RD_LEN", "PI_WR_LEN",
        "PI_STATUS", "PI_BSD_DOM1_LAT", "PI_BSD_DOM1_PWD", "PI_BSD_DOM1_PGS",
        "PI_BSD_DOM1_RLS", "PI_BSD_DOM2_LAT", "PI_BSD_DOM2_PWD", "PI_BSD_DOM2_PGS",
        "PI_BSD_DOM2_RLS"
    };
    for (int i = 0; i < 13; ++i)
        ImGui::Text("%-16s: %08X", names[i], reg->m_Peripheral_Interface[i]);
}

void ImGuiDebugUI::DrawRSPGPTab(MipsMemoryVM* mem) {
    auto* sp = mem->SPRegisters();
    if (!sp) { ImGui::Text("No SP registers"); return; }

    // RSP DMEM starts at offset 0, first 128 bytes are often GP regs
    // But we need the actual RSP register file — which lives in the RSP processor,
    // not in the memory handler. Show SP DMEM content for now.
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1),
        "RSP GP registers (from RSP processor)");
    ImGui::Separator();

    // SP register state accessible via m_SPRegs
    static const char* names[] = {
        "SP_MEM_ADDR", "SP_DRAM_ADDR", "SP_RD_LEN", "SP_WR_LEN",
        "SP_STATUS", "SP_DMA_FULL", "SP_DMA_BUSY", "SP_SEMAPHORE",
        "SP_PC", "SP_IBIST"
    };
    for (int i = 0; i < 10; ++i)
        ImGui::Text("%-16s: %08X", names[i], sp->m_SPRegs[i]);
}

void ImGuiDebugUI::DrawRSPVectorTab(MipsMemoryVM* mem) {
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1),
        "RSP Vector Register display (requires RSP processor link)");
}

void ImGuiDebugUI::DrawRSPHiddenTab(MipsMemoryVM* mem) {
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1),
        "RSP Hidden Register display (requires RSP processor link)");
}

// ═══════════════════════════════════════════════════════════════
//  Memory Panel — hex dump with address + hex + ASCII
// ═══════════════════════════════════════════════════════════════

void ImGuiDebugUI::DrawMemoryPanel() {
    ImGui::SetNextWindowPos(ImVec2(1050, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(480, 760), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Memory", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    MipsMemoryVM* mem = m_system ? m_system->GetMemory() : nullptr;
    if (!mem) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "System not initialized");
        ImGui::End();
        return;
    }

    // Address input bar
    ImGui::Text("Address:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    if (ImGui::InputText("##memaddr", m_memAddrInput, sizeof(m_memAddrInput),
            ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
        unsigned long long parsed = 0;
        if (sscanf(m_memAddrInput, "%llx", &parsed) == 1)
            m_memViewAddr = static_cast<uint32_t>(parsed);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Hex", &m_memShowHex);
    ImGui::SameLine();
    ImGui::Checkbox("ASCII", &m_memShowAscii);

    ImGui::Separator();

    // Memory hex dump
    ImGui::BeginChild("##memdump", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));

    const int rowsToShow = 64;
    const int bytesPerRow = m_memBytesPerRow;

    for (int row = 0; row < rowsToShow; ++row) {
        uint32_t rowAddr = m_memViewAddr + row * bytesPerRow;

        // Address column
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1), "%08X:", rowAddr);

        // Read bytes for this row
        uint8_t bytes[16] = {};
        for (int b = 0; b < bytesPerRow; ++b) {
            mem->MemoryValue8(rowAddr + b, bytes[b]);
        }

        // Hex column
        if (m_memShowHex) {
            ImGui::SameLine();
            for (int b = 0; b < bytesPerRow; ++b) {
                if (b > 0 && (b % 4 == 0)) ImGui::SameLine(0, 6);
                else ImGui::SameLine(0, 2);
                ImGui::Text("%02X", bytes[b]);
            }
        }

        // ASCII column
        if (m_memShowAscii) {
            ImGui::SameLine(0, 12);
            char ascii[17] = {};
            for (int b = 0; b < bytesPerRow; ++b)
                ascii[b] = (bytes[b] >= 32 && bytes[b] < 127) ? (char)bytes[b] : '.';
            ascii[bytesPerRow] = 0;
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1), "%s", ascii);
        }
    }

    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::End();
}

// ═══════════════════════════════════════════════════════════════
//  RDP State Panel — live RDP/rendering state tracing
// ═══════════════════════════════════════════════════════════════

void ImGuiDebugUI::DrawRDPStatePanel() {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("RDP State", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (!m_videoPlugin) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "VideoPlugin not connected");
        ImGui::End();
        return;
    }

    auto& rdpPtr = m_videoPlugin->GetRDPProcessor();
    if (!rdpPtr) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "RDP Processor not initialized");
        ImGui::End();
        return;
    }

    const auto& rdpState = rdpPtr->GetRDPState();
    const auto& stats    = rdpPtr->GetStats();

    // Debug output mode selector
    {
        int mode = rdpPtr->GetDebugOutputMode();
        ImGui::TextColored(ImVec4(1, 1, 0.5f, 1), "Shader Debug (F8 to cycle):");
        ImGui::SameLine();
        if (ImGui::Combo("##debugMode", &mode,
                "Normal\0Texel0\0Shade\0PrimColor\0EnvColor\0UV Coords\0Depth\0Alpha\0\0")) {
            rdpPtr->SetDebugOutputMode(mode);
        }
        ImGui::Separator();
    }

    if (ImGui::BeginTabBar("RDPTabs")) {

        // ─── Other Mode ───────────────────────────────────────────
        if (ImGui::BeginTabItem("Other Mode")) {
            uint32_t otherH = rdpState.otherModeH;
            uint32_t otherL = rdpState.otherModeL;

            ImGui::Text("otherModeH: %08X", otherH);
            ImGui::Text("otherModeL: %08X", otherL);
            ImGui::Separator();

            // Cycle type
            uint32_t cycleType = (otherH >> 20) & 3;
            static const char* cycleNames[] = { "1-Cycle", "2-Cycle", "Copy", "Fill" };
            ImGui::Text("Cycle Type:  %s (%d)", cycleNames[cycleType], cycleType);

            // Texture filter
            uint32_t texFilter = (otherH >> 12) & 3;
            static const char* filterNames[] = { "3-Point", "Average(std)", "Bilinear", "Reserved" };
            ImGui::Text("Tex Filter:  %s (%d)", filterNames[texFilter], texFilter);

            // Texture LOD
            ImGui::Text("Tex LOD:     %s", (otherH & 0x00010000) ? "ON" : "off");

            // Texture Detail
            uint32_t tdMode = (otherH >> 17) & 3;
            static const char* detailNames[] = { "Clamp", "Sharpen", "Detail", "Reserved" };
            ImGui::Text("Tex Detail:  %s (%d)", detailNames[tdMode], tdMode);

            // RGB/Alpha dither
            uint32_t rgbDither = (otherH >> 6) & 3;
            uint32_t alphaDither = (otherH >> 4) & 3;
            static const char* rgbDitherNames[] = { "MagicSq", "Bayer", "Noise", "None" };
            static const char* alphaDitherNames[] = { "Pattern", "~Pattern", "Noise", "None" };
            ImGui::Text("RGB Dither:  %s  Alpha Dither: %s", rgbDitherNames[rgbDither], alphaDitherNames[alphaDither]);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1), "otherModeL flags:");

            // otherModeL bit flags
            bool aaEn = (otherL & 0x0008) != 0;
            bool zCmp = (otherL & 0x0010) != 0;
            bool zUpd = (otherL & 0x0020) != 0;
            bool zSrc = (otherL & 0x0004) != 0;
            bool clrOnCvg = (otherL & 0x0080) != 0;
            bool cvgXAlpha = (otherL & 0x1000) != 0;
            bool alphaCvgSel = (otherL & 0x2000) != 0;
            bool forceBl = (otherL & 0x4000) != 0;
            uint32_t cvgDst = (otherL >> 8) & 3;
            uint32_t zMode = (otherL >> 10) & 3;
            uint32_t alphaCompare = otherL & 3;

            static const char* cvgDstNames[] = { "Clamp", "Wrap", "Full/Zap", "Save" };
            static const char* zModeNames[] = { "Opaque", "Interpen", "Transparent", "Decal" };
            static const char* alphaCompNames[] = { "None", "Threshold", "??", "Dither" };

            ImGui::Text("  AA_EN=%d  Z_CMP=%d  Z_UPD=%d  Z_SRC=%s",
                aaEn, zCmp, zUpd, zSrc ? "Prim" : "Pixel");
            ImGui::Text("  CLR_ON_CVG=%d  CVG_X_ALPHA=%d  ALPHA_CVG_SEL=%d  FORCE_BL=%d",
                clrOnCvg, cvgXAlpha, alphaCvgSel, forceBl);
            ImGui::Text("  CVG_DST=%s  Z_MODE=%s  ALPHA_CMP=%s",
                cvgDstNames[cvgDst], zModeNames[zMode], alphaCompNames[alphaCompare]);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1), "Blender Mux:");
            uint32_t p0  = (otherL >> 30) & 3;
            uint32_t a0  = (otherL >> 26) & 3;
            uint32_t m0  = (otherL >> 22) & 3;
            uint32_t b0  = (otherL >> 18) & 3;
            uint32_t p1  = (otherL >> 28) & 3;
            uint32_t a1  = (otherL >> 24) & 3;
            uint32_t m1  = (otherL >> 20) & 3;
            uint32_t b1  = (otherL >> 16) & 3;
            static const char* pNames[] = { "CC", "Mem", "Blend", "Fog" };
            static const char* aNames[] = { "CCAlpha", "FogAlpha", "ShadeAlpha", "Zero" };
            static const char* bNames[] = { "1-A", "MemAlpha", "One", "Zero" };
            ImGui::Text("  Cyc0: P=%s A=%s M=%s B=%s", pNames[p0], aNames[a0], pNames[m0], bNames[b0]);
            ImGui::Text("  Cyc1: P=%s A=%s M=%s B=%s", pNames[p1], aNames[a1], pNames[m1], bNames[b1]);

            ImGui::EndTabItem();
        }

        // ─── Combiner ─────────────────────────────────────────────
        if (ImGui::BeginTabItem("Combiner")) {
            uint64_t mux = rdpState.combineMode.Encode();
            ImGui::Text("Combine Mux: %016llX", (unsigned long long)mux);
            ImGui::Separator();

            uint32_t hi = static_cast<uint32_t>(mux >> 32);
            uint32_t lo = static_cast<uint32_t>(mux & 0xFFFFFFFF);

            static const char* rgbA[] = { "Comb","Tex0","Tex1","Prim","Shade","Env","1","Noise",
                                           "0","0","0","0","0","0","0","0" };
            static const char* rgbB[] = { "Comb","Tex0","Tex1","Prim","Shade","Env","CKey","K4",
                                           "0","0","0","0","0","0","0","0" };
            static const char* rgbC[] = { "Comb","Tex0","Tex1","Prim","Shade","Env","Scale","CombA",
                                           "Tex0A","Tex1A","PrimA","ShadeA","EnvA","LOD","PrimLOD","K5",
                                           "0","0","0","0","0","0","0","0",
                                           "0","0","0","0","0","0","0","0" };
            static const char* rgbD[] = { "Comb","Tex0","Tex1","Prim","Shade","Env","1","0" };
            static const char* alpABD[] = { "Comb","Tex0","Tex1","Prim","Shade","Env","1","0" };
            static const char* alpC[] = { "LOD","Tex0","Tex1","Prim","Shade","Env","PrimLOD","0" };

            auto showCycle = [&](int cycle) {
                int aRGB, bRGB, cRGB, dRGB, aA, bA, cA, dA;
                if (cycle == 0) {
                    aRGB = (hi >> 20) & 0xF;  cRGB = (hi >> 15) & 0x1F;
                    aA   = (hi >> 12) & 0x7;  cA   = (hi >> 9)  & 0x7;
                    bRGB = (lo >> 28) & 0xF;  dRGB = (lo >> 15) & 0x7;
                    bA   = (lo >> 12) & 0x7;  dA   = (lo >> 9)  & 0x7;
                } else {
                    aRGB = (hi >> 5)  & 0xF;  cRGB = (hi >> 0)  & 0x1F;
                    bRGB = (lo >> 24) & 0xF;
                    aA   = (lo >> 21) & 0x7;  cA   = (lo >> 18) & 0x7;
                    dRGB = (lo >> 6)  & 0x7;
                    bA   = (lo >> 3)  & 0x7;  dA   = (lo >> 0)  & 0x7;
                }
                ImGui::Text("  Cycle %d RGB: (%s - %s) * %s + %s",
                    cycle, rgbA[aRGB & 0xF], rgbB[bRGB & 0xF],
                    rgbC[cRGB & 0x1F], rgbD[dRGB & 0x7]);
                ImGui::Text("  Cycle %d A:   (%s - %s) * %s + %s",
                    cycle, alpABD[aA & 0x7], alpABD[bA & 0x7],
                    alpC[cA & 0x7], alpABD[dA & 0x7]);
            };

            ImGui::TextColored(ImVec4(1, 1, 0.7f, 1), "Cycle 0:");
            showCycle(0);
            ImGui::TextColored(ImVec4(1, 1, 0.7f, 1), "Cycle 1:");
            showCycle(1);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1), "Colors:");
            auto showColor = [](const char* name, const RDPColor& c) {
                ImGui::Text("  %-12s: R=%3d G=%3d B=%3d A=%3d",
                    name, c.r, c.g, c.b, c.a);
            };
            showColor("PrimColor", rdpState.primColor);
            showColor("EnvColor", rdpState.envColor);
            showColor("FogColor", rdpState.fogColor);
            showColor("BlendColor", rdpState.blendColor);
            showColor("FillColor", rdpState.fillColor);
            ImGui::Text("  PrimLODFrac:   %.4f", rdpState.primLODFrac / 255.0f);

            ImGui::EndTabItem();
        }

        // ─── Tiles / Textures ─────────────────────────────────────
        if (ImGui::BeginTabItem("Tiles")) {
            static const char* fmtNames[] = { "RGBA","YUV","CI","IA","I","?","?","?" };
            static const char* sizeNames[] = { "4b","8b","16b","32b" };
            for (int t = 0; t < 8; ++t) {
                const auto& tile = rdpState.tiles[t];
                bool open = ImGui::TreeNode((void*)(intptr_t)t, "Tile %d: %s %s  %dx%d  TMEM=%03X",
                    t, fmtNames[tile.format & 7], sizeNames[tile.size & 3],
                    tile.GetWidth(), tile.GetHeight(), tile.tmemAddr);
                if (open) {
                    ImGui::Text("  Line=%d  Palette=%d", tile.line, tile.palette);
                    ImGui::Text("  SL=%.2f  TL=%.2f  SH=%.2f  TH=%.2f",
                        tile.GetSL(), tile.GetTL(), tile.GetSH(), tile.GetTH());
                    ImGui::Text("  ClampS=%d MaskS=%d ShiftS=%d",
                        tile.clampS, tile.maskS, tile.shiftS);
                    ImGui::Text("  ClampT=%d MaskT=%d ShiftT=%d",
                        tile.clampT, tile.maskT, tile.shiftT);
                    ImGui::TreePop();
                }
            }
            ImGui::Separator();
            ImGui::Text("TextureImage: addr=%08X fmt=%d size=%d width=%d",
                rdpState.textureImage.address, rdpState.textureImage.format,
                rdpState.textureImage.size, rdpState.textureImage.width);

            ImGui::EndTabItem();
        }

        // ─── Scissor / Geometry ───────────────────────────────────
        if (ImGui::BeginTabItem("Geometry")) {
            ImGui::Text("GeometryMode: %08X", rdpState.geometry.geometryMode);
            ImGui::Separator();

            uint32_t gm = rdpState.geometry.geometryMode;
            ImGui::Text("  G_ZBUFFER:       %d", (gm & 0x00000001) != 0);
            ImGui::Text("  G_SHADE:         %d", (gm & 0x00000004) != 0);
            ImGui::Text("  G_CULL_FRONT:    %d", (gm & 0x00000200) != 0);
            ImGui::Text("  G_CULL_BACK:     %d", (gm & 0x00000400) != 0);
            ImGui::Text("  G_FOG:           %d", (gm & 0x00010000) != 0);
            ImGui::Text("  G_LIGHTING:      %d", (gm & 0x00020000) != 0);
            ImGui::Text("  G_TEXTURE_GEN:   %d", (gm & 0x00040000) != 0);
            ImGui::Text("  G_TEXTURE_ENABLE:%d", (gm & 0x00000000) != 0); // varies by ucode
            ImGui::Text("  G_SHADING_SMOOTH:%d", (gm & 0x00200000) != 0);

            ImGui::Separator();
            ImGui::Text("Scissor: XH=%d YH=%d XL=%d YL=%d",
                rdpState.scissor.xh >> 2, rdpState.scissor.yh >> 2,
                rdpState.scissor.xl >> 2, rdpState.scissor.yl >> 2);

            ImGui::Separator();
            ImGui::Text("ColorImage: addr=%08X fmt=%d size=%d width=%d",
                rdpState.colorImage.address, rdpState.colorImage.format,
                rdpState.colorImage.size, rdpState.colorImage.width);
            ImGui::Text("DepthImage: addr=%08X", rdpState.zbufferImage.address);

            ImGui::EndTabItem();
        }

        // ─── Stats ────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Stats")) {
            ImGui::TextColored(ImVec4(1, 1, 0.7f, 1), "Per Frame:");
            ImGui::Text("  Triangles:  %u", stats.frameTriangles);
            ImGui::Text("  Rects:      %u", stats.frameRects);
            ImGui::Text("  Tex Loads:  %u", stats.frameLoads);
            ImGui::Text("  Draw Calls: %u", stats.frameDrawCalls);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(1, 1, 0.7f, 1), "Cumulative:");
            ImGui::Text("  Commands:     %llu", (unsigned long long)stats.commandsProcessed);
            ImGui::Text("  Triangles:    %llu", (unsigned long long)stats.trianglesDrawn);
            ImGui::Text("  Rects:        %llu", (unsigned long long)stats.rectsDrawn);
            ImGui::Text("  Fill Rects:   %llu", (unsigned long long)stats.fillRectsDrawn);
            ImGui::Text("  Texture Loads:%llu", (unsigned long long)stats.textureLoads);
            ImGui::Text("  Batches:      %llu", (unsigned long long)stats.batchesFlushed);
            ImGui::Text("  State Changes:%llu", (unsigned long long)stats.stateChanges);
            ImGui::Text("  Display Lists:%llu", (unsigned long long)stats.displayListsProcessed);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(1, 1, 0.7f, 1), "Sync Counts:");
            ImGui::Text("  SyncFull: %llu  SyncPipe: %llu",
                (unsigned long long)stats.syncFullCount,
                (unsigned long long)stats.syncPipeCount);
            ImGui::Text("  SyncTile: %llu  SyncLoad: %llu",
                (unsigned long long)stats.syncTileCount,
                (unsigned long long)stats.syncLoadCount);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(1, 1, 0.7f, 1), "Framebuffers:");
            const auto& fbStats = g_fbManager.GetStats();
            ImGui::Text("  Created: %llu  Reused: %llu",
                (unsigned long long)fbStats.framebuffersCreated,
                (unsigned long long)fbStats.framebuffersReused);
            ImGui::Text("  Blits: %llu  Copies GPU→RDRAM: %llu",
                (unsigned long long)fbStats.blitsToScreen,
                (unsigned long long)fbStats.gpuToRdramCopies);

            ImGui::EndTabItem();
        }

        // ─── Frame Stats ──────────────────────────────────────────
        if (ImGui::BeginTabItem("Frame")) {
            const auto& fs = m_videoPlugin->GetFrameStats();
            ImGui::Text("Frame Time: %.2f ms", fs.frameTimeMs);
            ImGui::Text("FPS:        %.1f", fs.fps);
            ImGui::Text("Swap Time:  %.2f ms", fs.swapTimeMs);
            ImGui::Text("VI Count:   %llu", (unsigned long long)fs.viInterrupts);

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

} // namespace Nova64::Video
