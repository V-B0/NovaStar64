/**
 * @file HexEditor.cpp
 * @brief ImGui memory hex editor widget implementation.
 */
#include "HexEditor.hpp"
#include <imgui.h>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>

namespace NovaUI {

void HexEditor::Render(const char* label, uint8_t* mem, size_t size, uint32_t baseAddr) {
    if (!mem || size == 0) {
        ImGui::TextDisabled("No memory available.");
        return;
    }

    DrawToolbar(mem, size, baseAddr);
    ImGui::Separator();
    DrawRows(mem, size, baseAddr);
}

void HexEditor::DrawToolbar(uint8_t* mem, size_t size, uint32_t baseAddr) {
    // -- Go To Address --
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputText("##goto", m_gotoInput, sizeof(m_gotoInput),
                         ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
        uint32_t addr = 0;
        sscanf(m_gotoInput, "%x", &addr);
        if (addr >= baseAddr && (addr - baseAddr) < (uint32_t)size)
            m_cursorByte = addr - baseAddr;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Go To (hex addr)");

    ImGui::SameLine(0, 20);

    // -- Search bytes --
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("##search", m_searchBuf, sizeof(m_searchBuf));
    ImGui::SameLine();
    if (ImGui::Button("Find")) {
        // Parse space-separated hex bytes
        std::vector<uint8_t> pattern;
        char* p = m_searchBuf;
        while (*p) {
            while (*p == ' ') ++p;
            if (!*p) break;
            uint32_t b = 0;
            sscanf(p, "%2x", &b);
            pattern.push_back((uint8_t)b);
            while (*p && *p != ' ') ++p;
        }
        if (!pattern.empty()) {
            auto it = std::search(mem, mem + size, pattern.begin(), pattern.end());
            m_searchHit = (it != mem + size) ? (size_t)(it - mem) : SIZE_MAX;
            if (m_searchHit != SIZE_MAX) m_cursorByte = m_searchHit;
        }
    }

    ImGui::SameLine(0, 20);
    ImGui::Checkbox("Read Only", &ReadOnly);
    ImGui::SameLine();
    ImGui::Checkbox("ASCII", &ShowAscii);
}

void HexEditor::DrawRows(uint8_t* mem, size_t size, uint32_t baseAddr) {
    const float lineH   = ImGui::GetTextLineHeight();
    const int   bpr     = std::max(1, BytesPerRow);
    const size_t rows   = (size + bpr - 1) / bpr;

    // Scroll to cursor
    size_t cursorRow = m_cursorByte / bpr;

    ImGuiListClipper clipper;
    clipper.Begin((int)rows, lineH);
    while (clipper.Step()) {
        for (int rowIdx = clipper.DisplayStart; rowIdx < clipper.DisplayEnd; ++rowIdx) {
            size_t rowBase = (size_t)rowIdx * bpr;
            uint32_t dispAddr = baseAddr + (uint32_t)rowBase;

            // Address column
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 1.0f, 1.0f), "%08X:", dispAddr);
            ImGui::SameLine();

            // Hex bytes
            for (int col = 0; col < bpr; ++col) {
                size_t byteIdx = rowBase + col;
                if (byteIdx >= size) {
                    ImGui::TextDisabled("   ");
                } else {
                    uint8_t val = mem[byteIdx];
                    bool isCursor = (byteIdx == m_cursorByte);
                    bool isHit    = (m_searchHit != SIZE_MAX && byteIdx == m_searchHit);

                    if (col > 0) ImGui::SameLine(0, col % 8 == 0 ? 6 : 2);

                    char hexLabel[16];
                    snprintf(hexLabel, sizeof(hexLabel), "%02X##b%zu", val, byteIdx);

                    if (isCursor)
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.43f, 0.26f, 1.0f));
                    else if (isHit)
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));

                    if (isCursor || isHit) {
                        if (ImGui::SmallButton(hexLabel)) m_cursorByte = byteIdx;
                        ImGui::PopStyleColor();
                    } else {
                        if (ImGui::SmallButton(hexLabel)) m_cursorByte = byteIdx;
                    }
                }
            }

            // ASCII sidebar
            if (ShowAscii) {
                ImGui::SameLine(0, 12);
                ImGui::TextDisabled("|");
                ImGui::SameLine(0, 4);
                char ascii[64] = {};
                for (int col = 0; col < bpr && rowBase + col < size; ++col) {
                    uint8_t b = mem[rowBase + col];
                    ascii[col] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
                }
                ImGui::TextUnformatted(ascii);
            }
        }
    }

    // Keyboard editing when not read-only
    if (!ReadOnly && ImGui::IsWindowFocused()) {
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && m_cursorByte + 1 < size) ++m_cursorByte;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)  && m_cursorByte > 0) --m_cursorByte;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)  && m_cursorByte + bpr < size) m_cursorByte += bpr;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)    && m_cursorByte >= (size_t)bpr) m_cursorByte -= bpr;
    }

    // Tooltip on hover
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        if (m_cursorByte < size) {
            uint32_t addr32 = *(uint32_t*)(mem + (m_cursorByte & ~3));
            ImGui::SetTooltip("Addr: %08X  u8: %02X  u32@align: %08X",
                              baseAddr + (uint32_t)m_cursorByte,
                              mem[m_cursorByte],
                              addr32);
        }
    }
}

} // namespace NovaUI
