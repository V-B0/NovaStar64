/**
 * @file LauncherInternal.hpp
 * @brief Helpers shared by the launcher's source files. Not part of its API.
 */
#pragma once

#include "NovaLauncher.hpp"

#include <string>

namespace NovaUI::detail {

constexpr const char* kLibraryPath  = "settings/library.txt";
constexpr const char* kLauncherPath = "settings/launcher.ini";
constexpr const char* kGraphicsPath = "settings/NovaGraphics.ini";

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

/// Copies a ROM into roms/ (skipped when an identical-size copy is already
/// there) and returns the path to use; the original path on failure.
std::string CopyIntoRomsFolder(const std::string& romPath);

} // namespace NovaUI::detail
