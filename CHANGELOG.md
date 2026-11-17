# Changelog

All notable changes to NovaStar64 are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [1.0.0] - 2026-11-17

The first release.

### Added
- A Nintendo 64 emulator with a dynamic recompiler for x86-64 and ARM64 and a MIPS interpreter. The CPU core and the
  RSP began as a fork of Project64 and are credited as such.
- A high-level graphics pipeline that models the RDP in generated shaders, on Vulkan (MoltenVK on macOS) and OpenGL,
  with internal resolution from 1x to 8x, Rice-format texture packs and an experimental compute ray tracer.
- A launcher with a game library (Home, Library, Recent, Favorites, About), four save-state slots per game with
  pictures, a rewind strip, and a pause menu. Games play inside the launcher window or full screen.
- Settings as rows with a line of help each, saved at once, with a "Restore defaults" button for each page.
- The Simulator: a per-game workbench where a Nintendo 64, a TV, controllers, Rumble Paks and the cartridge are wired
  together. A game can have its own speed, CPU core, graphics driver, resolution, accuracy, save chip and Rumble Pak
  per player.
- Game controllers for up to four players, with rebindable buttons, an N64-shaped analog range and player slots.
  A controller also drives the launcher's menus. A Rumble Pak shakes the window a little, and you set how much.
- An interface size of 100%, 125%, 150% or 200% that scales text, icons and spacing together.
- EEPROM, SRAM and FlashRAM saves, a Rumble Pak and partial N64DD support. NTSC and PAL timing.
- A Lua scripting console and a memory viewer.
- Installers: a `.dmg` for macOS (Apple Silicon), a setup wizard and a portable `.zip` for Windows, and an AppImage for
  Linux. Each comes with a `.sha256` checksum. An installed copy keeps its settings, games and saves in the user's own
  data folder.

### Known limitations
- Only Super Mario 64, Ocarina of Time and Majora's Mask (NTSC-U) have been tested. Majora's Mask has some incomplete
  visual effects.
- There is no Controller Pak or Transfer Pak, no netplay and no RetroAchievements support yet.
- The macOS and Windows installers are not code-signed, so the system shows a one-time warning. Intel Macs build from
  source. The Linux AppImage has not been tried on a Linux machine yet.
- Text is slightly soft at an interface size of 125%.
- Moving through the launcher's menus with a controller has not been tried on real hardware yet.
- See [Known limitations](README.md#known-limitations) in the README for the full list.

[1.0.0]: https://github.com/V-B0/NovaStar64/releases/tag/v1.0.0
