/**
 * @file GameFiles.hpp
 * @brief The names of the files kept for one game (its setup, its workbench, its screenshot) and a safe way to write them.
 */

#pragma once

#include <string>

namespace NovaUI {

/// A short name for a game's files, taken from its ROM's file name: letters, digits, '-' and '_' are kept and
/// anything else becomes '_'. When that changed the name, a hash of the original is added, so "Zelda (U).z64" and
/// "Zelda [U].z64" do not share files, nor do two names in a script the file system's letters do not cover.
std::string GameKey(const std::string& romPath);

/// The file name of a path, and the name without its extension, as UTF-8 text. Unlike path::string() these do not
/// throw on Windows for a name the ANSI code page cannot show.
std::string FileName(const std::string& path);
std::string FileStem(const std::string& path);

/// `text` in ASCII lower case.
std::string Lowercase(std::string text);

/// Writes `text` to `path` through a temporary file that is renamed over it, creating the folder if needed. A full
/// disk or a crash leaves the old file as it was. Returns false if anything went wrong.
bool WriteFileAtomically(const std::string& path, const std::string& text);

} // namespace NovaUI
