/**
 * @file HexEditor.cpp
 * @brief Read-only ImGui memory viewer implementation.
 */
#include "HexEditor.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace NovaUI {

namespace {

uint8_t ByteAt(const uint8_t* mem, size_t size, size_t i, bool wordSwapped) {
    // Within a host-order word, big-endian byte n lives at host offset n ^ 3.
    if (wordSwapped && (i | 3) < size) return mem[i ^ 3];
    return mem[i];
}

bool ParseHexBytes(const char* text, std::vector<uint8_t>& out, std::string& bad) {
    out.clear();
    const char* p = text;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ' ') ++p;
        std::string token(start, p);
        if (token.size() > 2 ||
            !std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isxdigit(c); })) {
            bad = token;
            return false;
        }
        out.push_back(static_cast<uint8_t>(std::strtoul(token.c_str(), nullptr, 16)));
    }
    return true;
}

} // namespace

void HexEditor::Render(const uint8_t* mem, size_t size, uint32_t baseAddr, bool wordSwapped) {
    if (!mem || size == 0) {
        ImGui::TextDisabled("No memory available.");
        return;
    }
    m_cursor = std::min(m_cursor, size - 1);

    DrawToolbar(mem, size, baseAddr, wordSwapped);
    ImGui::Separator();

    // Only the rows scroll; the toolbar stays in view.
    ImGui::BeginChild("##hexrows", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    DrawRows(mem, size, baseAddr, wordSwapped);
    ImGui::EndChild();
}

void HexEditor::DrawToolbar(const uint8_t* mem, size_t size, uint32_t baseAddr, bool wordSwapped) {
    ImGui::SetNextItemWidth(110);
    const bool gotoEntered = ImGui::InputTextWithHint("##goto", "80000000", m_gotoInput, sizeof(m_gotoInput),
        ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Go to") || gotoEntered) {
        const unsigned long addr = std::strtoul(m_gotoInput, nullptr, 16);
        // Accept either an absolute address or an offset into the region.
        size_t offset = SIZE_MAX;
        if (addr >= baseAddr && addr - baseAddr < size) offset = addr - baseAddr;
        else if (addr < size)                           offset = addr;
        if (offset != SIZE_MAX) {
            m_cursor = offset;
            m_scrollToCursor = true;
            m_status.clear();
        } else {
            m_status = "Address is outside this memory region.";
        }
    }

    ImGui::SameLine(0, 20);
    ImGui::SetNextItemWidth(170);
    const bool findEntered = ImGui::InputTextWithHint("##find", "bytes, e.g. 3C 1A 80", m_searchInput,
        sizeof(m_searchInput), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Find next") || findEntered) {
        std::vector<uint8_t> pattern;
        std::string bad;
        if (!ParseHexBytes(m_searchInput, pattern, bad)) {
            m_status = "Not a hex byte: \"" + bad + "\"";
        } else if (!pattern.empty() && pattern.size() <= size) {
            // Search forward from just after the cursor, wrapping once.
            const size_t last = size - pattern.size();
            size_t found = SIZE_MAX;
            for (size_t step = 1; step <= last + 1 && found == SIZE_MAX; ++step) {
                const size_t start = (m_cursor + step) % (last + 1);
                size_t k = 0;
                while (k < pattern.size() && ByteAt(mem, size, start + k, wordSwapped) == pattern[k]) ++k;
                if (k == pattern.size()) found = start;
            }
            if (found != SIZE_MAX) {
                m_cursor = found;
                m_scrollToCursor = true;
                m_status.clear();
            } else {
                m_status = "Not found.";
            }
        }
    }

    ImGui::SameLine(0, 20);
    ImGui::Checkbox("ASCII", &ShowAscii);

    // Selected byte, and the 32-bit word containing it.
    const size_t wordStart = m_cursor & ~size_t{3};
    ImGui::Text("Address %08X   Byte %02X", static_cast<unsigned>(baseAddr + m_cursor),
                ByteAt(mem, size, m_cursor, wordSwapped));
    if (wordStart + 4 <= size) {
        uint32_t word = 0;
        for (size_t k = 0; k < 4; ++k) word = (word << 8) | ByteAt(mem, size, wordStart + k, wordSwapped);
        ImGui::SameLine();
        ImGui::Text("  Word %08X", word);
    }
    if (!m_status.empty()) {
        ImGui::SameLine(0, 20);
        ImGui::TextColored(ImVec4(0.75f, 0.25f, 0.15f, 1.0f), "%s", m_status.c_str());
    }
}

void HexEditor::DrawRows(const uint8_t* mem, size_t size, uint32_t baseAddr, bool wordSwapped) {
    const int    bpr       = std::max(1, BytesPerRow);
    const size_t rows      = (size + bpr - 1) / bpr;
    const float  rowHeight = ImGui::GetTextLineHeightWithSpacing();

    if (m_scrollToCursor) {
        ImGui::SetScrollY(static_cast<float>(m_cursor / bpr) * rowHeight);
        m_scrollToCursor = false;
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows), rowHeight);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const size_t rowBase = static_cast<size_t>(row) * bpr;
            ImGui::TextColored(ImVec4(0.35f, 0.35f, 0.60f, 1.0f), "%08X",
                               static_cast<unsigned>(baseAddr + rowBase));

            for (int col = 0; col < bpr && rowBase + col < size; ++col) {
                const size_t i = rowBase + col;
                ImGui::SameLine(0, col == 0 ? 12.0f : (col % 4 == 0 ? 8.0f : 3.0f));

                char label[24];
                std::snprintf(label, sizeof(label), "%02X##%zu", ByteAt(mem, size, i, wordSwapped), i);
                const bool selected = (i == m_cursor);
                if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                else          ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                if (ImGui::SmallButton(label)) m_cursor = i;
                ImGui::PopStyleColor();
            }

            if (ShowAscii) {
                char ascii[65] = {};
                int n = 0;
                for (; n < bpr && n < 64 && rowBase + n < size; ++n) {
                    const uint8_t b = ByteAt(mem, size, rowBase + n, wordSwapped);
                    ascii[n] = (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
                }
                ImGui::SameLine(0, 16);
                ImGui::TextUnformatted(ascii, ascii + n);
            }
        }
    }

    if (ImGui::IsWindowFocused()) {
        size_t next = m_cursor;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && m_cursor + 1 < size)            next = m_cursor + 1;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)  && m_cursor > 0)                   next = m_cursor - 1;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)  && m_cursor + bpr < size)          next = m_cursor + bpr;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)    && m_cursor >= static_cast<size_t>(bpr)) next = m_cursor - bpr;
        if (next != m_cursor) {
            m_cursor = next;
            const float y = static_cast<float>(m_cursor / bpr) * rowHeight;
            if (y < ImGui::GetScrollY() || y + rowHeight > ImGui::GetScrollY() + ImGui::GetWindowHeight())
                m_scrollToCursor = true;
        }
    }
}

} // namespace NovaUI
