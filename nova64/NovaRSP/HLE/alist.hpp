/**
 * @file alist.hpp
 * @brief RSP HLE Audio List Operations.
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
#include <cstddef>

namespace Nova64::RSP::HLE {

class CHle;

// ──────────────────────────────────────────────
// Audio command callback type
// ──────────────────────────────────────────────
using AcmdCallback = void (*)(CHle* hle, uint32_t w1, uint32_t w2);

// ──────────────────────────────────────────────
// Core audio list processing
// ──────────────────────────────────────────────
void alist_process(CHle* hle, const AcmdCallback abi[], unsigned int abi_size);

// ──────────────────────────────────────────────
// Segment address resolution
// ──────────────────────────────────────────────
uint32_t alist_get_address(CHle* hle, uint32_t so, const uint32_t* segments, size_t n);
void alist_set_address(CHle* hle, uint32_t so, uint32_t* segments, size_t n);

// ──────────────────────────────────────────────
// Audio buffer operations
// ──────────────────────────────────────────────
void alist_clear(CHle* hle, uint16_t dmem, uint16_t count);
void alist_load(CHle* hle, uint16_t dmem, uint32_t address, uint16_t count);
void alist_save(CHle* hle, uint16_t dmem, uint32_t address, uint16_t count);
void alist_move(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint16_t count);
void alist_copy_every_other_sample(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint16_t count);
void alist_repeat64(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint8_t count);
void alist_copy_blocks(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint16_t block_size, uint8_t count);
void alist_interleave(CHle* hle, uint16_t dmemo, uint16_t left, uint16_t right, uint16_t count);

// ──────────────────────────────────────────────
// Envelope mixing operations
// ──────────────────────────────────────────────
void alist_envmix_exp(CHle* hle, bool init, bool aux, uint16_t dmem_dl, uint16_t dmem_dr, uint16_t dmem_wl,
                      uint16_t dmem_wr, uint16_t dmemi, uint16_t count, int16_t dry, int16_t wet, const int16_t* vol,
                      const int16_t* target, const int32_t* rate, uint32_t address);

void alist_envmix_ge(CHle* hle, bool init, bool aux, uint16_t dmem_dl, uint16_t dmem_dr, uint16_t dmem_wl,
                     uint16_t dmem_wr, uint16_t dmemi, uint16_t count, int16_t dry, int16_t wet, const int16_t* vol,
                     const int16_t* target, const int32_t* rate, uint32_t address);

void alist_envmix_lin(CHle* hle, bool init, uint16_t dmem_dl, uint16_t dmem_dr, uint16_t dmem_wl, uint16_t dmem_wr,
                      uint16_t dmemi, uint16_t count, int16_t dry, int16_t wet, const int16_t* vol,
                      const int16_t* target, const int32_t* rate, uint32_t address);

void alist_envmix_nead(CHle* hle, bool swap_wet_LR, uint16_t dmem_dl, uint16_t dmem_dr, uint16_t dmem_wl,
                       uint16_t dmem_wr, uint16_t dmemi, unsigned count, uint16_t* env_values, uint16_t* env_steps,
                       const int16_t* xors);

// ──────────────────────────────────────────────
// DSP operations
// ──────────────────────────────────────────────
void alist_mix(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint16_t count, int16_t gain);
void alist_multQ44(CHle* hle, uint16_t dmem, uint16_t count, int8_t gain);
void alist_add(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint16_t count);

void alist_adpcm(CHle* hle, bool init, bool loop, bool two_bit_per_sample, uint16_t dmemo, uint16_t dmemi,
                 uint16_t count, const int16_t* codebook, uint32_t loop_address, uint32_t last_frame_address);

void alist_resample(CHle* hle, bool init, bool flag2, uint16_t dmemo, uint16_t dmemi, uint16_t count, uint32_t pitch,
                    uint32_t address);

void alist_polef(CHle* hle, bool init, uint16_t dmemo, uint16_t dmemi, uint16_t count, uint16_t gain, int16_t* table,
                 uint32_t address);

void alist_iirf(CHle* hle, bool init, uint16_t dmemo, uint16_t dmemi, uint16_t count, int16_t* table, uint32_t address);

void alist_resample_zoh(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint16_t count, uint32_t pitch, uint32_t pitch_accu);

void alist_filter(CHle* hle, uint16_t dmem, uint16_t count, uint32_t address, const uint32_t* lut_address);

// ──────────────────────────────────────────────
// Extended DSP operations
// ──────────────────────────────────────────────

// De-interleave stereo to separate L/R buffers
void alist_deinterleave(CHle* hle, uint16_t dmem_left, uint16_t dmem_right, uint16_t dmem_src, uint16_t count);

// Dynamic range compressor (limiter)
void alist_compress(CHle* hle, uint16_t dmem, uint16_t count, int16_t threshold, int16_t ratio, int16_t attack,
                    int16_t release);

// Chorus effect (modulated delay)
void alist_chorus(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint16_t count, uint16_t delay_base, uint16_t depth,
                  uint16_t rate_hz, int16_t wet_gain);

// Simple delay line (echo)
void alist_delay(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint16_t count, uint16_t delay_samples, int16_t feedback,
                 int16_t wet_gain);

// High-pass / low-pass single-pole filter
enum class FilterType : uint8_t { LowPass = 0, HighPass = 1 };
void alist_hilopass(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint16_t count, int16_t cutoff_coeff, FilterType type,
                    uint32_t state_address);

// Stereo pan with crossfade
void alist_pan(CHle* hle, uint16_t dmem_left, uint16_t dmem_right, uint16_t count, int16_t pan_left, int16_t pan_right);

// DC offset removal
void alist_dc_remove(CHle* hle, uint16_t dmem, uint16_t count, uint32_t state_address);

// ──────────────────────────────────────────────
// Audio flags
// ──────────────────────────────────────────────
inline constexpr uint32_t A_INIT = 0x01;
inline constexpr uint32_t A_CONTINUE = 0x00;
inline constexpr uint32_t A_LOOP = 0x02;
inline constexpr uint32_t A_OUT = 0x02;
inline constexpr uint32_t A_LEFT = 0x02;
inline constexpr uint32_t A_RIGHT = 0x00;
inline constexpr uint32_t A_VOL = 0x04;
inline constexpr uint32_t A_RATE = 0x00;
inline constexpr uint32_t A_AUX = 0x08;
inline constexpr uint32_t A_NOAUX = 0x00;
inline constexpr uint32_t A_MAIN = 0x00;
inline constexpr uint32_t A_MIX = 0x10;

} // namespace Nova64::RSP::HLE
