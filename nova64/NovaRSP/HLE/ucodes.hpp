/**
 * @file ucodes.hpp
 * @brief RSP HLE Audio Ucodes.
 *
 * Derived from Project64 (https://github.com/project64/project64), whose RSP HLE code in turn
 * comes from mupen64plus-rsp-hle (https://github.com/mupen64plus/mupen64plus-rsp-hle),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 *
 * Copyright(C) 2001-2021 Project64
 * Copyright(C) 2014 Bobby Smiles
 */

#pragma once
#include <cstdint>

namespace Nova64::RSP::HLE {

class CHle;

// ──────────────────────────────────────────────
// CIC x105 microcode (boot)
// ──────────────────────────────────────────────
void cicx105_ucode(CHle* hle);

// ──────────────────────────────────────────────
// Audio segment count
// ──────────────────────────────────────────────
inline constexpr uint32_t N_SEGMENTS = 16;

// ──────────────────────────────────────────────
// Audio List State Structures
// ──────────────────────────────────────────────
struct AlistAudio {
    uint32_t segments[N_SEGMENTS]{};
    uint16_t in{0}, out{0}, count{0};
    uint16_t dry_right{0}, wet_left{0}, wet_right{0};
    int16_t  dry{0}, wet{0};
    int16_t  vol[2]{};
    int16_t  target[2]{};
    int32_t  rate[2]{};
    uint32_t loop{0};
    int16_t  table[16 * 8]{};
};

struct AlistNaudio {
    int16_t  dry{0}, wet{0};
    int16_t  vol[2]{};
    int16_t  target[2]{};
    int32_t  rate[2]{};
    uint32_t loop{0};
    int16_t  table[16 * 8]{};
};

struct AlistNead {
    uint16_t in{0}, out{0}, count{0};
    uint16_t env_values[3]{};
    uint16_t env_steps[3]{};
    uint32_t loop{0};
    int16_t  table[16 * 8]{};
    uint16_t filter_count{0};
    uint32_t filter_lut_address[2]{};
};

// ──────────────────────────────────────────────
// Audio processing functions
// ──────────────────────────────────────────────

// Standard audio processing
void alist_process_audio(CHle* hle);
void alist_process_audio_ge(CHle* hle);
void alist_process_audio_bc(CHle* hle);

// Naudio variants
void alist_process_naudio(CHle* hle);
void alist_process_naudio_bk(CHle* hle);
void alist_process_naudio_dk(CHle* hle);
void alist_process_naudio_mp3(CHle* hle);
void alist_process_naudio_cbfd(CHle* hle);

// Nead variants
void alist_process_nead_mk(CHle* hle);
void alist_process_nead_sfj(CHle* hle);
void alist_process_nead_sf(CHle* hle);
void alist_process_nead_fz(CHle* hle);
void alist_process_nead_wrjb(CHle* hle);
void alist_process_nead_ys(CHle* hle);
void alist_process_nead_1080(CHle* hle);
void alist_process_nead_oot(CHle* hle);
void alist_process_nead_mm(CHle* hle);
void alist_process_nead_mmb(CHle* hle);
void alist_process_nead_ac(CHle* hle);

// MP3 microcode
void mp3_task(CHle* hle, unsigned int index, uint32_t address);

// Musyx microcodes
void musyx_v1_task(CHle* hle);
void musyx_v2_task(CHle* hle);

// JPEG microcodes
void jpeg_decode_PS0(CHle* hle);
void jpeg_decode_PS(CHle* hle);
void jpeg_decode_OB(CHle* hle);

} // namespace Nova64::RSP::HLE
