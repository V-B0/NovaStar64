/**
 * @file LauncherInternal.hpp
 * @brief Helpers shared by the launcher's source files. Not part of its API.
 */
#pragma once

#include "NovaLauncher.hpp"

#include <string>

namespace NovaUI::detail {

constexpr const char* kLibraryPath = "settings/library.txt";
constexpr const char* kLauncherPath = "settings/launcher.ini";
constexpr const char* kGraphicsPath = "settings/NovaGraphics.ini";
/// The graphics settings of the game that is running, when it has a setup of its own.
constexpr const char* kRunningGraphicsPath = "settings/running-game.ini";

/// Height of the bar above a running game.
constexpr float kGameBarHeight = 48.0f;

/// Where the screenshot of a game's last session is kept (shots/<name>.png in the data folder).
std::string ShotPathFor(const std::string& romPath);

/// Native "open file" dialog. Returns "" if cancelled or none is available.
std::string PickRomFileNative();

/// Native "open file" dialog for a cover picture (PNG or JPEG). Returns "" if cancelled or none is available.
std::string PickImageFileNative();

/// Copies a cover picture into covers/ and returns its path relative to the data
/// folder; "" if it couldn't be copied.
std::string ImportCoverImage(const std::string& source, const std::string& gameName);

/// True if the file starts with an N64 ROM header in any byte order. When it
/// does and `internalName` is non-null, receives the trimmed title from the header.
bool ReadN64RomHeader(const std::string& path, std::string* internalName);

/// The space a row of options keeps to the left of its control for the Reset button.
constexpr float kResetWidth = 64.0f;

/// How wide and tall the control of an option is.
float OptionControlWidth(const OptionDef& option);
float OptionControlHeight(const OptionDef& option);
/// Draws the control for an option; `value` is read and written in place. Returns true when it changed.
/// `defaultValue` is marked in a dropdown.
bool DrawOptionControl(const OptionDef& option, int* value, int defaultValue);

/// Copies a ROM into roms/ (skipped when an identical-size copy is already
/// there) and returns the path to use; the original path on failure.
std::string CopyIntoRomsFolder(const std::string& romPath);

} // namespace NovaUI::detail
