/**
 * @file MicrocodeRegistry.hpp
 * @brief Microcode Registry - Maps ucode CRC32s to HLE handlers.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#pragma once
#include "ucodes.hpp"
#include <cstdint>
#include <string_view>

namespace Nova64::RSP::HLE {

class CHle;

// --- Microcode categories ---
enum class UcodeType : uint8_t {
    Unknown = 0,

    // Audio microcodes
    AudioStandard,  // Original Nintendo ABI
    AudioGE,        // GoldenEye / Perfect Dark
    AudioBC,        // Banjo-Kazooie / Conker
    NaudioStandard, // Naudio (newer Nintendo ABI)
    NaudioBK,       // Naudio Banjo variant
    NaudioDK,       // Naudio DK64 variant
    NaudioMP3,      // Naudio + MP3
    NaudioCBFD,     // Naudio Conker variant
    NeadMK,         // NEAD Mario Kart 64
    NeadSFJ,        // NEAD Star Fox (J)
    NeadSF,         // NEAD Star Fox 64
    NeadFZ,         // NEAD F-Zero X
    NeadWRJB,       // NEAD Wave Race / Jet Force
    NeadYS,         // NEAD Yoshi's Story
    Nead1080,       // NEAD 1080 Snowboarding
    NeadOOT,        // NEAD Ocarina of Time
    NeadMM,         // NEAD Majora's Mask
    NeadMMB,        // NEAD Majora's Mask (B)
    NeadAC,         // NEAD Animal Crossing

    // Music synthesizer microcodes
    MusyxV1, // MusyX v1 (Factor 5)
    MusyxV2, // MusyX v2 (Factor 5)
    MP3,     // Pure MP3 decoder

    // Image microcodes
    JPEG_PS0, // JPEG (Pokemon Snap variant)
    JPEG_PS,  // JPEG (standard)
    JPEG_OB,  // JPEG (Ogre Battle variant)

    // Boot/utility
    CICx105, // CIC x105 bootstrap
};

// --- Audio handler function pointer type ---
using UcodeHandler = void (*)(CHle*);

// --- Microcode registry entry ---
struct MicrocodeEntry {
    uint32_t ucode_crc;      // CRC32 of ucode data
    uint32_t ucode_data_crc; // CRC32 of ucode data segment (0 = don't check)
    UcodeType type;
    UcodeHandler handler;
    std::string_view name;
};

// Known microcode database
// CRC values from Project64's RSP HLE and Mupen64Plus HLE RSP
inline constexpr MicrocodeEntry kMicrocodeDatabase[] = {
    // ── Standard Audio ABI ──
    {0x1DC6CB36, 0, UcodeType::AudioStandard, alist_process_audio, "ABI Standard 1.0"},
    {0x1A13A51A, 0, UcodeType::AudioStandard, alist_process_audio, "ABI Standard 1.0 (SM64)"},
    {0x4A90240B, 0, UcodeType::AudioStandard, alist_process_audio, "ABI Standard 1.1"},
    {0xB9D5BEBB, 0, UcodeType::AudioStandard, alist_process_audio, "ABI Standard 1.2 (SM64)"},
    {0xA1A44F91, 0, UcodeType::AudioStandard, alist_process_audio, "ABI Standard 1.3 (MK64)"},
    {0x1E26E925, 0, UcodeType::AudioStandard, alist_process_audio, "ABI Standard (WR64)"},

    // ── GoldenEye Audio ──
    {0x1AE8143C, 0, UcodeType::AudioGE, alist_process_audio_ge, "ABI GoldenEye"},

    // ── Banjo / Conker Audio ──
    {0xB7C2E69B, 0, UcodeType::AudioBC, alist_process_audio_bc, "ABI Banjo-Conker"},

    // ── Naudio ──
    {0xAE044800, 0, UcodeType::NaudioStandard, alist_process_naudio, "Naudio Standard"},
    {0xA740E404, 0, UcodeType::NaudioStandard, alist_process_naudio, "Naudio Standard 1.1"},

    // ── Naudio Banjo ──
    {0x20394D79, 0, UcodeType::NaudioBK, alist_process_naudio_bk, "Naudio Banjo-Kazooie"},

    // ── Naudio DK64 ──
    {0x42253F38, 0, UcodeType::NaudioDK, alist_process_naudio_dk, "Naudio DK64"},

    // ── Naudio MP3 ──
    {0x4B836E04, 0, UcodeType::NaudioMP3, alist_process_naudio_mp3, "Naudio MP3"},

    // ── Naudio CBFD ──
    {0x2E902AB4, 0, UcodeType::NaudioCBFD, alist_process_naudio_cbfd, "Naudio CBFD"},

    // ── NEAD variants ──
    {0xE8A20B42, 0, UcodeType::NeadMK, alist_process_nead_mk, "NEAD MK64"},
    {0x8681F3E3, 0, UcodeType::NeadSFJ, alist_process_nead_sfj, "NEAD Star Fox J"},
    {0x6D218E4C, 0, UcodeType::NeadSF, alist_process_nead_sf, "NEAD Star Fox 64"},
    {0x6CB4B508, 0, UcodeType::NeadFZ, alist_process_nead_fz, "NEAD F-Zero X"},
    {0x0C50EB20, 0, UcodeType::NeadWRJB, alist_process_nead_wrjb, "NEAD Wave Race"},
    {0xA7AF12CF, 0, UcodeType::NeadYS, alist_process_nead_ys, "NEAD Yoshi's Story"},
    {0xEB5D5FD2, 0, UcodeType::Nead1080, alist_process_nead_1080, "NEAD 1080"},
    {0x1C463ABB, 0, UcodeType::NeadOOT, alist_process_nead_oot, "NEAD OOT"},
    {0xB94B80C7, 0, UcodeType::NeadOOT, alist_process_nead_oot, "NEAD OOT (alt data CRC)"},
    {0xDBC54543, 0, UcodeType::NeadOOT, alist_process_nead_oot, "NEAD OOT (alt text CRC)"},
    {0x4FC2E19E, 0, UcodeType::NeadMM, alist_process_nead_mm, "NEAD Majora's Mask"},
    {0x3083B431, 0x9CA82D44, UcodeType::NeadMM, alist_process_nead_mm, "NEAD Majora's Mask (alt CRC pair)"},
    {0x68EC50E1, 0, UcodeType::NeadMMB, alist_process_nead_mmb, "NEAD Majora's Mask B"},
    {0x15EF3C70, 0, UcodeType::NeadAC, alist_process_nead_ac, "NEAD Animal Crossing"},

    // ── MusyX (Factor 5 games: Rogue Squadron etc.) ──
    {0xDD560AC4, 0, UcodeType::MusyxV1, musyx_v1_task, "MusyX v1"},
    {0x8D29B33E, 0, UcodeType::MusyxV2, musyx_v2_task, "MusyX v2"},

    // ── JPEG ──
    {0xC824C504, 0, UcodeType::JPEG_PS0, jpeg_decode_PS0, "JPEG PS0"},
    {0xD7B4860C, 0, UcodeType::JPEG_PS, jpeg_decode_PS, "JPEG PS"},
    {0x8C2D11B4, 0, UcodeType::JPEG_OB, jpeg_decode_OB, "JPEG OB"},
};

inline constexpr size_t kMicrocodeCount = sizeof(kMicrocodeDatabase) / sizeof(kMicrocodeDatabase[0]);

// Boot ROM identification checksums
// (Legacy additive checksums of first 44 IMEM words)
namespace BootChecksum {
inline constexpr uint32_t Standard = 0xB4C62BFC;  // Standard boot microcode
inline constexpr uint32_t Alternate = 0xCAB15710; // Alternate boot microcode (CIC-specific)
} // namespace BootChecksum

// --- Lookup functions ---

// Find a microcode entry by CRC32
[[nodiscard]] inline const MicrocodeEntry* FindMicrocode(uint32_t crc) noexcept {
    for (size_t i = 0; i < kMicrocodeCount; ++i) {
        if (kMicrocodeDatabase[i].ucode_crc == crc) return &kMicrocodeDatabase[i];
    }
    return nullptr;
}

// Find by both ucode CRC and data CRC (for disambiguation)
[[nodiscard]] inline const MicrocodeEntry* FindMicrocode(uint32_t crc, uint32_t data_crc) noexcept {
    const MicrocodeEntry* fallback = nullptr;
    for (size_t i = 0; i < kMicrocodeCount; ++i) {
        if (kMicrocodeDatabase[i].ucode_crc == crc) {
            if (kMicrocodeDatabase[i].ucode_data_crc == 0) {
                fallback = &kMicrocodeDatabase[i];
            } else if (kMicrocodeDatabase[i].ucode_data_crc == data_crc) {
                return &kMicrocodeDatabase[i];
            }
        }
    }
    return fallback;
}

// Check if a microcode type is audio
[[nodiscard]] inline constexpr bool IsAudioUcode(UcodeType type) noexcept {
    return type >= UcodeType::AudioStandard && type <= UcodeType::MP3;
}

// Check if a microcode type is graphics
[[nodiscard]] inline constexpr bool IsGraphicsUcode(UcodeType type) noexcept {
    // Graphics task type=1 is now handled via GfxProcessor
    // This still returns false since graphics ucodes are in a separate DB (GraphicsUcodes.hpp)
    // Audio-only microcode types are tracked here.
    return false;
}

} // namespace Nova64::RSP::HLE
