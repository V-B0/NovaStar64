/**
 * @file GameDB.hpp
 * @brief RSP Per-Game Settings Database.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#pragma once
#include <cstdint>
#include <optional>

namespace Nova64::RSP {

// ──────────────────────────────────────────────
// Per-game override flags
// ──────────────────────────────────────────────
struct GameOverrides {
    const char*             name{nullptr};
    std::optional<bool>     graphicsHle;     // Override graphics HLE on/off
    std::optional<bool>     audioHle;        // Override audio HLE on/off
    std::optional<uint32_t> cpuMethod;       // Override CPU method (0=interp, 2=recomp, 3=HLE)
};

// ──────────────────────────────────────────────
// Game database entry: CRC1 + CRC2 → overrides
// ──────────────────────────────────────────────
struct GameDBEntry {
    uint32_t      crc1;
    uint32_t      crc2;
    GameOverrides overrides;
};

// ──────────────────────────────────────────────
// Known game overrides
//
// Games that need specific RSP settings to work correctly.
// CRC values are from the ROM header (big-endian at 0x10/0x14).
//
// Categories:
//   - Audio-only HLE: Games where graphics HLE causes issues
//   - LLE required: Games with custom RSP microcodes
//   - Full HLE safe: Games known to work with both audio + graphics HLE
// ──────────────────────────────────────────────

inline constexpr GameDBEntry kGameDatabase[] = {
    // ── Games needing LLE for audio (custom audio microcodes) ──

    // Rogue Squadron (Factor 5 custom RSP microcode — no standard audio ucode)
    { 0x66A24BEC, 0xF3E1C252, { "Star Wars: Rogue Squadron (U)", std::nullopt, false, std::nullopt } },
    { 0xFF24FBEA, 0xFF8ED278, { "Star Wars: Rogue Squadron (E)", std::nullopt, false, std::nullopt } },

    // World Driver Championship (Boss Game Studios custom microcode)
    { 0xC7E71A73, 0x2BDB4E96, { "World Driver Championship (U)", std::nullopt, false, std::nullopt } },

    // Indiana Jones and the Infernal Machine (Factor 5 engine)
    { 0x3C3AA14B, 0x0D19C6E0, { "Indiana Jones: Infernal Machine (U)", std::nullopt, false, std::nullopt } },

    // ── Games needing graphics LLE (custom graphics microcodes) ──

    // Conker's Bad Fur Day (unique RSP geometry processing)
    { 0x30C7E1BF, 0x087E92B7, { "Conker's Bad Fur Day (U)", false, std::nullopt, std::nullopt } },

    // Perfect Dark (complex custom rendering pipeline)
    { 0xB3267AE0, 0x6FEA0B32, { "Perfect Dark (U)", false, std::nullopt, std::nullopt } },
    { 0xFCA3F820, 0x15775C10, { "Perfect Dark (E)", false, std::nullopt, std::nullopt } },

    // GoldenEye 007 (turbo3D variant)
    { 0xDCBC50D1, 0x09FD1AA3, { "GoldenEye 007 (U)", false, std::nullopt, std::nullopt } },
    { 0x69CF1A4C, 0x47F5BFBA, { "GoldenEye 007 (E)", false, std::nullopt, std::nullopt } },

    // ── Games safe for full HLE ──

    // Super Mario 64
    { 0x635A42C5, 0xB66C0B72, { "Super Mario 64 (U)", true, true, 3 } },
    { 0x4EAA3D0E, 0x74757D1C, { "Super Mario 64 (E)", true, true, 3 } },
    { 0x2CF451A8, 0x6AE67867, { "Super Mario 64 (J)", true, true, 3 } },

    // Mario Kart 64
    { 0x3E5055B6, 0x2F036E31, { "Mario Kart 64 (U)", true, true, 3 } },

    // The Legend of Zelda: Ocarina of Time
    { 0xCD16C529, 0xB98CFF96, { "Zelda: Ocarina of Time (U) v1.0", true, true, 3 } },
    { 0x09465AC8, 0xB7BB0B5C, { "Zelda: Ocarina of Time (U) v1.2", true, true, 3 } },

    // The Legend of Zelda: Majora's Mask
    // Audio HLE re-enabled: MM uses NEAD audio ucode (CRC 0x4FC2E19E)
    // which we DO have registered (alist_process_nead_mm). The previous
    // false setting silently dropped audio tasks without signaling
    // SP_INTR, blocking MM's audio thread and causing downstream state
    // corruption (Link instant-dies after item-get / cutscene events
    // because the audio thread block stalls game-state update propagation).
    { 0x5354631C, 0x03A2DEF0, { "Zelda: Majora's Mask (U)", true, true, 3 } },

    // Banjo-Kazooie
    { 0xCD7559AC, 0x34A80170, { "Banjo-Kazooie (U)", true, true, 3 } },

    // Banjo-Tooie
    { 0xC2E9AA9A, 0x475D70AA, { "Banjo-Tooie (U)", true, true, 3 } },

    // Paper Mario
    { 0x65EEE53A, 0xED7D733C, { "Paper Mario (U)", true, true, 3 } },
};

inline constexpr size_t kGameDatabaseSize = sizeof(kGameDatabase) / sizeof(kGameDatabase[0]);

// ──────────────────────────────────────────────
// Lookup function
// Returns nullptr if no match found
// ──────────────────────────────────────────────
inline const GameOverrides* LookupGameOverrides(uint32_t crc1, uint32_t crc2)
{
    for (size_t i = 0; i < kGameDatabaseSize; ++i)
    {
        if (kGameDatabase[i].crc1 == crc1 && kGameDatabase[i].crc2 == crc2)
            return &kGameDatabase[i].overrides;
    }
    return nullptr;
}

} // namespace Nova64::RSP
