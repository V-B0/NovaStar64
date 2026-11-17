/**
 * @file GameInfo.hpp
 * @brief What the launcher shows about a well-known game besides its name: publisher, year, a short description.
 */

#pragma once

#include <string>

namespace NovaUI {

struct GameInfo {
    const char* title;              ///< the full title
    const char* publisher;          ///< the publisher or developer
    int year;                       ///< release year (first release)
    const char* blurb;              ///< one or two sentences
    const char* status;             ///< how well it runs here: "Playable", or "Untested"
    bool needsExpansionPak = false; ///< the game does not run, or loses its main content, on 4 MB
};

/// The entry for a game, matched by the name in the library, or null if it is not in the table.
const GameInfo* FindGameInfo(const std::string& libraryName);

} // namespace NovaUI
