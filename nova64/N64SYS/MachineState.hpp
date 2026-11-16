/**
 * @file MachineState.hpp
 * @brief Complete snapshots of the emulated machine, and the save-state file format.
 */
#pragma once

#include <array>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

class CN64System;

namespace Nova64 {

/// A small picture of the screen, RGBA with the top row first.
struct StateThumbnail {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

using RdramPage = std::array<uint8_t, 0x1000>;
using RomHeader = std::array<uint8_t, 0x40>;

/// Everything needed to put the machine back where it was: CPU and hardware
/// state, plugin state and RDRAM. Cartridge save data (EEPROM, SRAM and
/// FlashRAM contents) and Controller Paks are left out on purpose: going back
/// in time shouldn't undo a save the player made.
struct MachineSnapshot {
    uint64_t frame = 0;           ///< video field count when it was taken
    std::vector<uint8_t> core;    ///< CPU, timers, TLB and hardware registers
    std::vector<uint8_t> plugins; ///< graphics and audio plugin state
    uint32_t rdramSize = 0;
    /// RDRAM in 4 KB pages. Pages that didn't change since the previous
    /// snapshot are shared with it, which keeps a rewind history small.
    std::vector<std::shared_ptr<const RdramPage>> pages;
    std::shared_ptr<const StateThumbnail> thumbnail;
};

/// Takes a snapshot. Must run on the emulator thread at an instruction
/// boundary (a system event); returns false if the CPU is between a branch and
/// its delay slot. Pages equal to `previous`'s are shared with it.
bool CaptureMachineState(CN64System& system, MachineSnapshot& out, const MachineSnapshot* previous);

/// Puts the machine into the snapshot's state, under the same threading rule.
/// Returns false without changing anything if the snapshot doesn't fit this
/// build or this game's memory size.
bool RestoreMachineState(CN64System& system, const MachineSnapshot& snapshot);

/// Bytes a snapshot keeps alive that `previous` doesn't already hold.
size_t SnapshotMemoryUse(const MachineSnapshot& snapshot, const MachineSnapshot* previous);

/// The loaded ROM's header, which ties a state file to its game.
bool GetRomHeader(const CN64System& system, RomHeader& out);

bool WriteStateFile(const std::string& path, const MachineSnapshot& snapshot, const RomHeader& rom, std::string* error);
bool ReadStateFile(const std::string& path, const RomHeader& rom, MachineSnapshot& out, std::string* error);

/// Reads only the time and thumbnail of a state file, for showing a slot.
bool ReadStateFileSummary(const std::string& path, std::time_t& savedAt,
                          std::shared_ptr<const StateThumbnail>& thumbnail);

} // namespace Nova64
