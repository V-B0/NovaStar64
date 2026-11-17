/**
 * @file RomInfo.hpp
 * @brief What the header of an N64 ROM file says about the game.
 */

#pragma once

#include <string>

namespace NovaUI::detail {

/// What a ROM's header says about it.
struct RomInfo {
    bool valid = false;
    std::string title;       ///< the internal name
    std::string gameId;      ///< the two-letter game code, e.g. "SM"
    char regionCode = '?';   ///< the header's country byte, e.g. 'E'
    const char* region = "Unknown";
    bool pal = false;        ///< a 50 Hz game
    const char* format = ""; ///< "z64 (big-endian)", "v64 (byte-swapped)" or "n64 (little-endian)"
    unsigned long long size = 0;
    unsigned crc1 = 0;
    unsigned crc2 = 0;
    int revision = 0;
};

/// Reads the header of a ROM file; `valid` is false if it is missing or is not an N64 ROM.
RomInfo ReadRomInfo(const std::string& path);

} // namespace NovaUI::detail
