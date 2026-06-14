/**
 * @file HexEditor.hpp
 * @brief ImGui-based memory hex editor widget.
 *
 * Usage:
 *   static HexEditor g_hexEd;
 *   g_hexEd.Render("Memory##hex", rdram, rdramSize, 0x80000000u);
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace NovaUI {

class HexEditor {
public:
    HexEditor() = default;

    /// Render the hex editor as an ImGui child region.
    /// @param label    ImGui window ID label (must be unique)
    /// @param mem      Pointer to the raw memory region
    /// @param size     Size of the region in bytes
    /// @param baseAddr Virtual base address to show in the address column
    void Render(const char* label, uint8_t* mem, size_t size, uint32_t baseAddr = 0);

    // Config
    int  BytesPerRow   = 16;
    bool ShowAscii     = true;
    bool ReadOnly      = false;
    bool HighlightEdits= true;

private:
    size_t   m_cursorByte  = 0;
    bool     m_editNibble  = false;  ///< true = editing low nibble
    uint32_t m_gotoAddr    = 0;
    char     m_gotoInput[12] = {};
    char     m_searchBuf[64] = {};
    size_t   m_searchHit   = SIZE_MAX;

    void DrawToolbar(uint8_t* mem, size_t size, uint32_t baseAddr);
    void DrawRows(uint8_t* mem, size_t size, uint32_t baseAddr);
};

} // namespace NovaUI
