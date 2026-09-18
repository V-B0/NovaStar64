/**
 * @file HexEditor.hpp
 * @brief Read-only ImGui memory viewer.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace NovaUI {

class HexEditor {
public:
    /// Draws the viewer into the current window.
    ///
    /// @param baseAddr    Address shown for byte 0 (e.g. 0x80000000 for RDRAM).
    /// @param wordSwapped The buffer holds 32-bit words in host byte order, which
    ///                    is how the emulator stores RDRAM. Bytes are then shown
    ///                    in the N64's big-endian order.
    void Render(const uint8_t* mem, size_t size, uint32_t baseAddr, bool wordSwapped);

    int  BytesPerRow = 16;
    bool ShowAscii   = true;

private:
    size_t      m_cursor         = 0;
    bool        m_scrollToCursor = false;
    char        m_gotoInput[12]  = {};
    char        m_searchInput[64] = {};
    std::string m_status;

    void DrawToolbar(const uint8_t* mem, size_t size, uint32_t baseAddr, bool wordSwapped);
    void DrawRows(const uint8_t* mem, size_t size, uint32_t baseAddr, bool wordSwapped);
};

} // namespace NovaUI
