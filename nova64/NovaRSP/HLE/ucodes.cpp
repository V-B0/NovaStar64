/**
 * @file ucodes.cpp
 * @brief RSP HLE Microcode Dispatchers.
 *
 * Derived from Project64 (https://github.com/project64/project64), whose RSP HLE code in turn
 * comes from mupen64plus-rsp-hle (https://github.com/mupen64plus/mupen64plus-rsp-hle),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 *
 * Copyright(C) 2001-2021 Project64
 * Copyright(C) 2014 Bobby Smiles
 */

#include "ucodes.hpp"
#include "alist.hpp"
#include "audio.hpp"
#include "hle.hpp"
#include "mem.hpp"
#include "arithmetics.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Nova64::RSP::HLE {

// CIC x105 boot microcode
// Used during IPL3 stage of CIC-NUS-6105 games (Banjo-Tooie, Zelda OoT/MM,
// Perfect Dark, ...). The RSP performs DMA transactions needed for booting.
// Reference: Mupen64Plus mupen64plus-rsp-hle/src/cicx105.c

void cicx105_ucode(CHle* hle) {
    uint8_t* imem = hle->imem();
    uint8_t* dram = hle->dram();
    if (imem == nullptr || dram == nullptr) return;

    // NovaStar64 stores RDRAM/IMEM with the same byte-swap convention as
    // Mupen (XOR_S8=3 on LE host => native 32-bit word order). For copies
    // whose source/destination addresses and length are 4-byte aligned, a
    // raw memcpy preserves the N64-visible byte sequence correctly.

    // dma_read(0x1120, 0x1e8, 0x1e8): IMEM[0x120 .. 0x310) <- DRAM[0x1e8 .. 0x3d8)
    std::memcpy(imem + 0x120, dram + 0x1e8, 0x1f0);

    // dma_write(0x1120, 0x2fb1f0, 0xfe817000): scatter 24 * 8 bytes from
    // IMEM[0x120..0x1e0) into DRAM with destination stride 0xff0.
    uint8_t* dst = dram + 0x2fb1f0;
    uint8_t* src = imem + 0x120;
    for (unsigned i = 0; i < 24; ++i) {
        std::memcpy(dst, src, 8);
        dst += 0xff0;
        src += 0x8;
    }

    hle->rsp_break(0);
}

// ══════════════════════════════════════════════
// Standard Audio list processing (original ABI)
// ══════════════════════════════════════════════

// Forward declarations of command handlers
static void SPNOOP(CHle*, uint32_t, uint32_t) {}

// ── Audio ABI ──────────────────────────────────

static void AUDIO_SEGMENTS(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& state = hle->alist_audio();
    const uint32_t seg = (w2 >> 24) & 0x0F;
    if (seg < N_SEGMENTS) state.segments[seg] = w2 & 0x00FFFFFF;
}

// SM64 / standard audio ABI uses DMEM_BASE offset for all buffer addresses
// (matches mupen64plus-rsp-hle alist_audio.c).
static constexpr uint16_t DMEM_BASE = 0x5C0;

static void AUDIO_SETBUFF(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_audio();
    if ((w1 >> 16) & A_AUX) {
        // Aux buffer setup
        s.dry_right = static_cast<uint16_t>(w1) + DMEM_BASE;
        s.wet_left = static_cast<uint16_t>(w2 >> 16) + DMEM_BASE;
        s.wet_right = static_cast<uint16_t>(w2) + DMEM_BASE;
    } else {
        s.in = static_cast<uint16_t>(w1) + DMEM_BASE;
        s.out = static_cast<uint16_t>(w2 >> 16) + DMEM_BASE;
        s.count = static_cast<uint16_t>(w2);
    }
}

static void AUDIO_DMEMMOVE(CHle* hle, uint32_t w1, uint32_t w2) {
    uint16_t dmemi = static_cast<uint16_t>(w1) + DMEM_BASE;
    uint16_t dmemo = static_cast<uint16_t>(w2 >> 16) + DMEM_BASE;
    uint16_t count = static_cast<uint16_t>(w2);
    if (count == 0) return;
    alist_move(hle, dmemo, dmemi, static_cast<uint16_t>(align(count, 16)));
}

static void AUDIO_LOADBUFF(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_audio();
    uint32_t addr = alist_get_address(hle, w2, s.segments, N_SEGMENTS);
    alist_load(hle, s.in, addr, s.count);
    (void)w1;
}

// ABI Standard (GBI0/ABI1) LOADADPCM — loads ADPCM codebook from DRAM into s.table.
// count = low 16 bits of w1 (byte count), address = w2 & 0xFFFFFF.
// Pattern mirrors NEAD_OOT_LOADADPCM.
static void AUDIO_LOADADPCM(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_audio();
    uint16_t count = static_cast<uint16_t>(w1);
    uint32_t address = alist_get_address(hle, w2, s.segments, N_SEGMENTS);
    dram_load_u16(hle, reinterpret_cast<uint16_t*>(s.table), address, static_cast<uint16_t>(align(count, 8)) >> 1);
}

static void AUDIO_SAVEBUFF(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_audio();
    uint32_t addr = alist_get_address(hle, w2, s.segments, N_SEGMENTS);
    alist_save(hle, s.out, addr, s.count);
    (void)w1;
}

static void AUDIO_SETLOOP(CHle* hle, uint32_t /*w1*/, uint32_t w2) {
    auto& s = hle->alist_audio();
    s.loop = alist_get_address(hle, w2, s.segments, N_SEGMENTS);
}

static void AUDIO_ADPCM(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_audio();
    bool init = (w1 >> 16) & A_INIT;
    bool loop = (w1 >> 16) & A_LOOP;
    uint32_t addr = alist_get_address(hle, w2, s.segments, N_SEGMENTS);
    alist_adpcm(hle, init, loop, false, s.out, s.in, static_cast<uint16_t>(align(s.count, 32)), s.table, s.loop, addr);
}

static void AUDIO_RESAMPLE(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_audio();
    bool init = (w1 >> 16) & A_INIT;
    bool flag2 = (w1 >> 16) & 0x02;
    uint32_t pitch = w1 & 0xFFFF;
    uint32_t addr = alist_get_address(hle, w2, s.segments, N_SEGMENTS);
    alist_resample(hle, init, flag2, s.out, s.in, static_cast<uint16_t>(align(s.count, 16)), pitch << 1, addr);
}

static void AUDIO_ENVMIXER(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_audio();
    bool init = (w1 >> 16) & A_INIT;
    bool aux = (w1 >> 16) & A_AUX;
    uint32_t addr = alist_get_address(hle, w2, s.segments, N_SEGMENTS);
    alist_envmix_exp(hle, init, aux, s.out, s.dry_right, s.wet_left, s.wet_right, s.in, s.count, s.dry, s.wet, s.vol,
                     s.target, s.rate, addr);
}

static void AUDIO_SETVOL(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_audio();
    uint8_t flags = static_cast<uint8_t>(w1 >> 16);

    // Matches mupen64plus-rsp-hle alist_audio.c::SETVOL exactly.
    // dry/wet/vol/target are stored in 16-bit fields (cast truncates),
    // rate is the full 32-bit w2 (signed).
    if (flags & A_AUX) {
        s.dry = static_cast<int16_t>(w1);
        s.wet = static_cast<int16_t>(w2);
    } else {
        unsigned lr = (flags & A_LEFT) ? 0u : 1u;
        if (flags & A_VOL) {
            s.vol[lr] = static_cast<int16_t>(w1);
        } else {
            s.target[lr] = static_cast<int16_t>(w1);
            s.rate[lr] = static_cast<int32_t>(w2);
        }
    }
}

static void AUDIO_CLEARBUFF(CHle* hle, uint32_t w1, uint32_t w2) {
    uint16_t dmem = static_cast<uint16_t>(w1) + DMEM_BASE;
    uint16_t count = static_cast<uint16_t>(w2 & 0xFFF);
    if (count == 0) return;
    alist_clear(hle, dmem, static_cast<uint16_t>(align(count, 16)));
}

static void AUDIO_MIXER(CHle* hle, uint32_t w1, uint32_t w2) {
    // mupen reference: dmemi = (w2>>16) + DMEM_BASE, dmemo = w2 + DMEM_BASE
    uint16_t dmemi = static_cast<uint16_t>(w2 >> 16) + DMEM_BASE;
    uint16_t dmemo = static_cast<uint16_t>(w2) + DMEM_BASE;
    auto& s = hle->alist_audio();
    int16_t gain = static_cast<int16_t>(w1);
    if (s.count == 0) return;
    alist_mix(hle, dmemo, dmemi, static_cast<uint16_t>(align(s.count, 32)), gain);
}

static void AUDIO_INTERLEAVE(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_audio();
    uint16_t left = static_cast<uint16_t>(w2 >> 16) + DMEM_BASE;
    uint16_t right = static_cast<uint16_t>(w2) + DMEM_BASE;
    if (s.count == 0) return;
    alist_interleave(hle, s.out, left, right, static_cast<uint16_t>(align(s.count, 16)));
    (void)w1;
}

static void AUDIO_POLEF(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_audio();
    bool init = (w1 >> 16) & 0x01;
    uint16_t gain = static_cast<uint16_t>(w1);
    uint32_t addr = alist_get_address(hle, w2, s.segments, N_SEGMENTS);
    alist_polef(hle, init, s.out, s.in, s.count, gain, s.table, addr);
}

void alist_process_audio(CHle* hle) {
    static const AcmdCallback ABI[32] = {
        SPNOOP,         AUDIO_ADPCM,      AUDIO_CLEARBUFF, AUDIO_ENVMIXER, AUDIO_LOADBUFF, AUDIO_RESAMPLE,
        AUDIO_SAVEBUFF, AUDIO_SEGMENTS,   AUDIO_SETBUFF,   AUDIO_SETVOL,   AUDIO_DMEMMOVE, AUDIO_LOADADPCM,
        AUDIO_MIXER,    AUDIO_INTERLEAVE, AUDIO_POLEF,     AUDIO_SETLOOP,  SPNOOP,         SPNOOP,
        SPNOOP,         SPNOOP,           SPNOOP,          SPNOOP,         SPNOOP,         SPNOOP,
        SPNOOP,         SPNOOP,           SPNOOP,          SPNOOP,         SPNOOP,         SPNOOP,
        SPNOOP,         SPNOOP,
    };
    alist_process(hle, ABI, 32);
}

// ── GoldenEye variant ─────────────────────────
// GE uses the GE-specific envelope mixer (fixed linear ramp)

static void AUDIO_ENVMIXER_GE(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_audio();
    bool init = (w1 >> 16) & 0x01;
    bool aux = (w1 >> 16) & 0x08;
    uint32_t addr = alist_get_address(hle, w2, s.segments, N_SEGMENTS);
    alist_envmix_ge(hle, init, aux, s.out, s.dry_right, s.wet_left, s.wet_right, s.in, s.count, s.dry, s.wet, s.vol,
                    s.target, s.rate, addr);
}

void alist_process_audio_ge(CHle* hle) {
    static const AcmdCallback ABI_GE[32] = {
        SPNOOP,         AUDIO_ADPCM,      AUDIO_CLEARBUFF, AUDIO_ENVMIXER_GE,
        AUDIO_LOADBUFF, AUDIO_RESAMPLE,   AUDIO_SAVEBUFF,  AUDIO_SEGMENTS,
        AUDIO_SETBUFF,  AUDIO_SETVOL,     AUDIO_DMEMMOVE,  AUDIO_LOADADPCM,
        AUDIO_MIXER,    AUDIO_INTERLEAVE, AUDIO_POLEF,     AUDIO_SETLOOP,
        SPNOOP,         SPNOOP,           SPNOOP,          SPNOOP,
        SPNOOP,         SPNOOP,           SPNOOP,          SPNOOP,
        SPNOOP,         SPNOOP,           SPNOOP,          SPNOOP,
        SPNOOP,         SPNOOP,           SPNOOP,          SPNOOP,
    };
    alist_process(hle, ABI_GE, 32);
}

// ── Banjo-Conker variant ──────────────────────
// BC also uses the GE-style envelope mixer

void alist_process_audio_bc(CHle* hle) {
    static const AcmdCallback ABI_BC[32] = {
        SPNOOP,         AUDIO_ADPCM,      AUDIO_CLEARBUFF, AUDIO_ENVMIXER_GE,
        AUDIO_LOADBUFF, AUDIO_RESAMPLE,   AUDIO_SAVEBUFF,  AUDIO_SEGMENTS,
        AUDIO_SETBUFF,  AUDIO_SETVOL,     AUDIO_DMEMMOVE,  AUDIO_LOADADPCM,
        AUDIO_MIXER,    AUDIO_INTERLEAVE, AUDIO_POLEF,     AUDIO_SETLOOP,
        SPNOOP,         SPNOOP,           SPNOOP,          SPNOOP,
        SPNOOP,         SPNOOP,           SPNOOP,          SPNOOP,
        SPNOOP,         SPNOOP,           SPNOOP,          SPNOOP,
        SPNOOP,         SPNOOP,           SPNOOP,          SPNOOP,
    };
    alist_process(hle, ABI_BC, 32);
}

// ══════════════════════════════════════════════
// Naudio processing variants
// Complete ABI with all commands from Project64
// ══════════════════════════════════════════════

static void NAUDIO_SETBUFF(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_naudio();
    uint8_t flags = static_cast<uint8_t>(w1 >> 16);
    if (flags & A_AUX) {
        // Aux buffer setup: set loop point
        s.loop = w2 & 0x00FFFFFF;
    } else {
        // Main buffer setup
        // (naudio encodes in/out/count differently per variant)
    }
}

static void NAUDIO_ADPCM(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_naudio();
    bool init = (w1 >> 16) & A_INIT;
    bool loop = (w1 >> 16) & A_LOOP;
    uint16_t dmemo = static_cast<uint16_t>(w1);
    uint16_t dmemi = static_cast<uint16_t>(w2 >> 16);
    uint16_t count = static_cast<uint16_t>(w2);
    alist_adpcm(hle, init, loop, false, dmemo, dmemi, count, s.table, s.loop, w2 & 0x00FFFFFF);
}

static void NAUDIO_RESAMPLE(CHle* hle, uint32_t w1, uint32_t w2) {
    [[maybe_unused]] auto& s = hle->alist_naudio();
    bool init = (w1 >> 16) & A_INIT;
    bool flag2 = (w1 >> 16) & A_LOOP;
    uint16_t dmemo = static_cast<uint16_t>(w1);
    uint16_t dmemi = static_cast<uint16_t>(w2 >> 16);
    uint16_t count = static_cast<uint16_t>(w2 & 0xFFFF);
    uint32_t pitch = (w1 >> 0) & 0xFFFF;
    alist_resample(hle, init, flag2, dmemo, dmemi, count, pitch << 1, w2 & 0x00FFFFFF);
}

static void NAUDIO_DMEMMOVE(CHle* hle, uint32_t w1, uint32_t w2) {
    uint16_t dmemi = static_cast<uint16_t>(w1);
    uint16_t dmemo = static_cast<uint16_t>(w2 >> 16);
    uint16_t count = static_cast<uint16_t>(w2);
    alist_move(hle, dmemo, dmemi, count);
}

static void NAUDIO_SETVOL(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_naudio();
    uint8_t flags = static_cast<uint8_t>(w1 >> 16);
    if (flags & A_VOL) {
        s.target[0] = static_cast<int16_t>(w1);
        s.target[1] = static_cast<int16_t>(w2 >> 16);
    } else if (flags & A_AUX) {
        s.rate[0] = static_cast<int32_t>(w1 & 0xFFFF) << 16 | (w2 >> 16);
        s.rate[1] = static_cast<int32_t>(w2 & 0xFFFF) << 16;
    } else {
        s.vol[0] = static_cast<int16_t>(w1);
        s.dry = static_cast<int16_t>(w2 >> 16);
        s.wet = static_cast<int16_t>(w2);
    }
}

static void NAUDIO_ENVMIXER(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_naudio();
    bool init = (w1 >> 16) & A_INIT;
    bool aux = (w1 >> 16) & A_AUX;
    uint16_t dmem_dl = static_cast<uint16_t>(w1);
    uint16_t dmem_dr = static_cast<uint16_t>(w2 >> 16);
    uint16_t dmem_wl = static_cast<uint16_t>(w2);
    uint16_t dmem_wr = static_cast<uint16_t>(w2) + 0x200;
    uint32_t addr = s.loop;
    alist_envmix_lin(hle, init, dmem_dl, dmem_dr, dmem_wl, dmem_wr, 0x940, 0x3C0, s.dry, s.wet, s.vol, s.target, s.rate,
                     addr);
    (void)aux;
}

static void NAUDIO_LOADBUFF(CHle* hle, uint32_t w1, uint32_t w2) {
    uint16_t count = static_cast<uint16_t>((w1 >> 12) & 0xFFF);
    uint16_t dmem = static_cast<uint16_t>(w1);
    uint32_t addr = w2 & 0x00FFFFFF;
    alist_load(hle, dmem, addr, count);
}

static void NAUDIO_SAVEBUFF(CHle* hle, uint32_t w1, uint32_t w2) {
    uint16_t count = static_cast<uint16_t>((w1 >> 12) & 0xFFF);
    uint16_t dmem = static_cast<uint16_t>(w1);
    uint32_t addr = w2 & 0x00FFFFFF;
    alist_save(hle, dmem, addr, count);
}

static void NAUDIO_MIXER(CHle* hle, uint32_t w1, uint32_t w2) {
    uint16_t dmemo = static_cast<uint16_t>(w2 >> 16);
    uint16_t dmemi = static_cast<uint16_t>(w2);
    int16_t gain = static_cast<int16_t>(w1);
    uint16_t count = 0x3C0; // Standard naudio mix length
    alist_mix(hle, dmemo, dmemi, count, gain);
}

static void NAUDIO_INTERLEAVE(CHle* hle, uint32_t w1, uint32_t w2) {
    uint16_t dmemo = static_cast<uint16_t>(w1);
    uint16_t left = static_cast<uint16_t>(w2 >> 16);
    uint16_t right = static_cast<uint16_t>(w2);
    uint16_t count = 0x3C0;
    alist_interleave(hle, dmemo, left, right, count);
}

static void NAUDIO_CLEARBUFF(CHle* hle, uint32_t w1, uint32_t w2) {
    uint16_t dmem = static_cast<uint16_t>(w1);
    uint16_t count = static_cast<uint16_t>(w2);
    alist_clear(hle, dmem, count);
}

// ── Banjo-Kazooie / DK64 Naudio envmixer variant ──
// Uses the GE-style (exponential) envelope mixer instead of linear
static void NAUDIO_ENVMIXER_GE(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_naudio();
    bool init = (w1 >> 16) & A_INIT;
    bool aux = (w1 >> 16) & A_AUX;
    uint16_t dmem_dl = static_cast<uint16_t>(w1);
    uint16_t dmem_dr = static_cast<uint16_t>(w2 >> 16);
    uint16_t dmem_wl = static_cast<uint16_t>(w2);
    uint16_t dmem_wr = static_cast<uint16_t>(w2) + 0x200;
    uint32_t addr = s.loop;
    alist_envmix_ge(hle, init, aux, dmem_dl, dmem_dr, dmem_wl, dmem_wr, 0x940, 0x3C0, s.dry, s.wet, s.vol, s.target,
                    s.rate, addr);
}

void alist_process_naudio(CHle* hle) {
    static const AcmdCallback ABI[16] = {
        SPNOOP, NAUDIO_ADPCM,   NAUDIO_CLEARBUFF, NAUDIO_ENVMIXER, NAUDIO_LOADBUFF, NAUDIO_RESAMPLE, NAUDIO_SAVEBUFF,
        SPNOOP, NAUDIO_SETBUFF, NAUDIO_SETVOL,    NAUDIO_DMEMMOVE, NAUDIO_LOADBUFF, NAUDIO_MIXER,    NAUDIO_INTERLEAVE,
        SPNOOP, SPNOOP,
    };
    alist_process(hle, ABI, 16);
}

void alist_process_naudio_bk(CHle* hle) {
    // Banjo-Kazooie uses GE-style exponential envelope mixing
    static const AcmdCallback ABI_BK[16] = {
        SPNOOP,          NAUDIO_ADPCM,      NAUDIO_CLEARBUFF, NAUDIO_ENVMIXER_GE,
        NAUDIO_LOADBUFF, NAUDIO_RESAMPLE,   NAUDIO_SAVEBUFF,  SPNOOP,
        NAUDIO_SETBUFF,  NAUDIO_SETVOL,     NAUDIO_DMEMMOVE,  NAUDIO_LOADBUFF,
        NAUDIO_MIXER,    NAUDIO_INTERLEAVE, SPNOOP,           SPNOOP,
    };
    alist_process(hle, ABI_BK, 16);
}

void alist_process_naudio_dk(CHle* hle) {
    // DK64 also uses GE-style envelope mixing
    static const AcmdCallback ABI_DK[16] = {
        SPNOOP,          NAUDIO_ADPCM,      NAUDIO_CLEARBUFF, NAUDIO_ENVMIXER_GE,
        NAUDIO_LOADBUFF, NAUDIO_RESAMPLE,   NAUDIO_SAVEBUFF,  SPNOOP,
        NAUDIO_SETBUFF,  NAUDIO_SETVOL,     NAUDIO_DMEMMOVE,  NAUDIO_LOADBUFF,
        NAUDIO_MIXER,    NAUDIO_INTERLEAVE, SPNOOP,           SPNOOP,
    };
    alist_process(hle, ABI_DK, 16);
}

void alist_process_naudio_mp3(CHle* hle) {
    // Naudio + MP3 hybrid — standard linear envmixer
    alist_process_naudio(hle);
}

void alist_process_naudio_cbfd(CHle* hle) {
    // Conker's Bad Fur Day — uses GE-style envelope mixing
    static const AcmdCallback ABI_CBFD[16] = {
        SPNOOP,          NAUDIO_ADPCM,      NAUDIO_CLEARBUFF, NAUDIO_ENVMIXER_GE,
        NAUDIO_LOADBUFF, NAUDIO_RESAMPLE,   NAUDIO_SAVEBUFF,  SPNOOP,
        NAUDIO_SETBUFF,  NAUDIO_SETVOL,     NAUDIO_DMEMMOVE,  NAUDIO_LOADBUFF,
        NAUDIO_MIXER,    NAUDIO_INTERLEAVE, SPNOOP,           SPNOOP,
    };
    alist_process(hle, ABI_CBFD, 16);
}

// ══════════════════════════════════════════════
// NEAD processing variants
// ══════════════════════════════════════════════

static void NEAD_SETBUFF(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_nead();
    s.in = static_cast<uint16_t>(w1);
    s.out = static_cast<uint16_t>(w2 >> 16);
    s.count = static_cast<uint16_t>(w2);
}

static void NEAD_DMEMMOVE(CHle* hle, uint32_t w1, uint32_t w2) {
    uint16_t dmemi = static_cast<uint16_t>(w1);
    uint16_t dmemo = static_cast<uint16_t>(w2 >> 16);
    uint16_t count = static_cast<uint16_t>(w2);
    if (count == 0) return;
    alist_move(hle, dmemo, dmemi, (count + 3) & ~3);
}

static void NEAD_CLEARBUFF(CHle* hle, uint32_t w1, uint32_t w2) {
    uint16_t dmem = static_cast<uint16_t>(w1);
    uint16_t count = static_cast<uint16_t>(w2 & 0xFFF);
    if (count == 0) return;
    alist_clear(hle, dmem, count);
}

static void NEAD_LOADBUFF(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_nead();
    alist_load(hle, s.in, w2 & 0x00FFFFFF, s.count);
    (void)w1;
}

static void NEAD_SAVEBUFF(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_nead();
    alist_save(hle, s.out, w2 & 0x00FFFFFF, s.count);
    (void)w1;
}

static void NEAD_MIX(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_nead();
    uint16_t dmemo = static_cast<uint16_t>(w2 >> 16);
    uint16_t dmemi = static_cast<uint16_t>(w2);
    int16_t gain = static_cast<int16_t>(w1);
    alist_mix(hle, dmemo, dmemi, s.count, gain);
}

static void NEAD_ENVMIXER(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_nead();
    bool swap = (w1 >> 16) & 0x01;
    uint16_t dmem_dl = static_cast<uint16_t>(w1);
    uint16_t dmem_dr = static_cast<uint16_t>(w2 >> 16);
    uint16_t dmem_wl = static_cast<uint16_t>(w2);
    uint16_t dmem_wr = static_cast<uint16_t>(w2) + s.count;

    // Extract XOR masks from command word flag bits
    // These control envelope polarity inversion for surround effects
    int16_t xors[3]{};
    xors[0] = ((w1 >> 16) & 0x02) ? static_cast<int16_t>(0xFFFF) : 0; // Dry left polarity
    xors[1] = ((w1 >> 16) & 0x04) ? static_cast<int16_t>(0xFFFF) : 0; // Dry right polarity
    xors[2] = 0;                                                      // Wet channel polarity (not typically inverted)

    alist_envmix_nead(hle, swap, dmem_dl, dmem_dr, dmem_wl, dmem_wr, s.in, s.count, s.env_values, s.env_steps, xors);
}

static void NEAD_INTERLEAVE(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_nead();
    uint16_t left = static_cast<uint16_t>(w2 >> 16);
    uint16_t right = static_cast<uint16_t>(w2);
    alist_interleave(hle, s.out, left, right, s.count);
    (void)w1;
}

static void NEAD_ADPCM(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_nead();
    uint8_t flags = static_cast<uint8_t>(w1 >> 16);
    bool init = flags & 0x01;
    bool loop = flags & 0x02;
    bool two_bit = flags & 0x04;
    alist_adpcm(hle, init, loop, two_bit, s.out, s.in, static_cast<uint16_t>(align(s.count, 32)), s.table, s.loop,
                w2 & 0x00FFFFFF);
}

static void NEAD_RESAMPLE(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_nead();
    bool init = (w1 >> 16) & 0x01;
    bool flag2 = (w1 >> 16) & 0x02;
    uint32_t pitch = w1 & 0xFFFF;
    // Round count up to next multiple of 16 (matching mupen64plus)
    alist_resample(hle, init, flag2, s.out, s.in, static_cast<uint16_t>((s.count + 0xf) & ~0xf), pitch << 1,
                   w2 & 0x00FFFFFF);
}

static void NEAD_SETLOOP(CHle* hle, uint32_t /*w1*/, uint32_t w2) { hle->alist_nead().loop = w2 & 0x00FFFFFF; }

static void NEAD_FILTER(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_nead();
    alist_filter(hle, s.in, s.count, w2 & 0x00FFFFFF, s.filter_lut_address);
    (void)w1;
}

static void NEAD_POLEF(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_nead();
    bool init = (w1 >> 16) & 0x01;
    uint16_t gain = static_cast<uint16_t>(w1);
    alist_polef(hle, init, s.out, s.in, s.count, gain, s.table, w2 & 0x00FFFFFF);
}

// MK64 unique handler: HILOGAIN applies gain to the high/low frequency bands
// via the Q4.4 multiplier (same operation as alist_multQ44).
static void NEAD_HILOGAIN(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_nead();
    int8_t gain = static_cast<int8_t>(w1);
    alist_multQ44(hle, s.out, s.count, gain);
    (void)w2;
}

// ── OoT NEAD-specific command handlers ──
// These handlers extract parameters from the command words (w1/w2) rather
// than from the SETBUFF state struct, matching the actual N64 RSP microcode.

static void NEAD_OOT_ENVSETUP1(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_nead();
    s.env_values[2] = (w1 >> 8) & 0xFF00;
    s.env_steps[2] = static_cast<uint16_t>(w1);
    s.env_steps[0] = static_cast<uint16_t>(w2 >> 16);
    s.env_steps[1] = static_cast<uint16_t>(w2);
}

static void NEAD_OOT_ENVSETUP2(CHle* hle, uint32_t /*w1*/, uint32_t w2) {
    auto& s = hle->alist_nead();
    s.env_values[0] = static_cast<uint16_t>(w2 >> 16);
    s.env_values[1] = static_cast<uint16_t>(w2);
}

static void NEAD_OOT_LOADADPCM(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_nead();
    uint16_t count = static_cast<uint16_t>(w1);
    uint32_t address = w2 & 0x00FFFFFF;
    dram_load_u16(hle, reinterpret_cast<uint16_t*>(s.table), address, count >> 1);
}

static void NEAD_OOT_LOADBUFF(CHle* hle, uint32_t w1, uint32_t w2) {
    uint16_t count = (w1 >> 12) & 0xFFF;
    uint16_t dmem = w1 & 0xFFF;
    uint32_t address = w2 & 0x00FFFFFF;
    alist_load(hle, dmem, address, count);
}

static void NEAD_OOT_SAVEBUFF(CHle* hle, uint32_t w1, uint32_t w2) {
    uint16_t count = (w1 >> 12) & 0xFFF;
    uint16_t dmem = w1 & 0xFFF;
    uint32_t address = w2 & 0x00FFFFFF;
    alist_save(hle, dmem, address, count);
}

static void NEAD_OOT_ENVMIXER(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_nead();

    uint16_t dmemi = (w1 >> 12) & 0xFF0;
    uint8_t count = (w1 >> 8) & 0xFF;
    bool swap_wet = (w1 >> 4) & 0x1;
    uint16_t dmem_dl = (w2 >> 20) & 0xFF0;
    uint16_t dmem_dr = (w2 >> 12) & 0xFF0;
    uint16_t dmem_wl = (w2 >> 4) & 0xFF0;
    uint16_t dmem_wr = (w2 << 4) & 0xFF0;

    int16_t xors[4]{};
    xors[2] = 0 - static_cast<int16_t>((w1 & 0x8) >> 1);
    xors[3] = 0 - static_cast<int16_t>((w1 & 0x4) >> 1);
    xors[0] = 0 - static_cast<int16_t>((w1 & 0x2) >> 1);
    xors[1] = 0 - static_cast<int16_t>((w1 & 0x1));

    // Pass raw count (number of samples to process, aligned to 8 internally)
    alist_envmix_nead(hle, swap_wet, dmem_dl, dmem_dr, dmem_wl, dmem_wr, dmemi, static_cast<unsigned>(count),
                      s.env_values, s.env_steps, xors);
}

static void NEAD_OOT_ADDMIXER(CHle* hle, uint32_t w1, uint32_t w2) {
    uint16_t count = (w1 >> 12) & 0xFF0;
    uint16_t dmemi = static_cast<uint16_t>(w2 >> 16);
    uint16_t dmemo = static_cast<uint16_t>(w2);
    alist_add(hle, dmemo, dmemi, count);
}

static void NEAD_OOT_DUPLICATE(CHle* hle, uint32_t w1, uint32_t w2) {
    uint8_t count = static_cast<uint8_t>(w1 >> 16);
    uint16_t dmemi = static_cast<uint16_t>(w1);
    uint16_t dmemo = static_cast<uint16_t>(w2 >> 16);
    alist_repeat64(hle, dmemo, dmemi, count);
}

static void NEAD_OOT_INTERL(CHle* hle, uint32_t w1, uint32_t w2) {
    uint16_t count = static_cast<uint16_t>(w1);
    uint16_t dmemi = static_cast<uint16_t>(w2 >> 16);
    uint16_t dmemo = static_cast<uint16_t>(w2);
    alist_copy_every_other_sample(hle, dmemo, dmemi, count);
}

static void NEAD_OOT_NEAD16(CHle* hle, uint32_t w1, uint32_t w2) {
    uint8_t count = static_cast<uint8_t>(w1 >> 16);
    uint16_t dmemi = static_cast<uint16_t>(w1);
    uint16_t dmemo = static_cast<uint16_t>(w2 >> 16);
    uint16_t block_size = static_cast<uint16_t>(w2);
    alist_copy_blocks(hle, dmemo, dmemi, block_size, count);
}

static void NEAD_OOT_MIXER(CHle* hle, uint32_t w1, uint32_t w2) {
    uint16_t count = (w1 >> 12) & 0xFF0;
    int16_t gain = static_cast<int16_t>(w1);
    uint16_t dmemi = static_cast<uint16_t>(w2 >> 16);
    uint16_t dmemo = static_cast<uint16_t>(w2);
    alist_mix(hle, dmemo, dmemi, count, gain);
}

static void NEAD_OOT_INTERLEAVE(CHle* hle, uint32_t w1, uint32_t w2) {
    uint16_t count = (w1 >> 12) & 0xFF0;
    uint16_t dmemo = static_cast<uint16_t>(w1);
    uint16_t left = static_cast<uint16_t>(w2 >> 16);
    uint16_t right = static_cast<uint16_t>(w2);
    alist_interleave(hle, dmemo, left, right, count);
}

static void NEAD_OOT_HILOGAIN(CHle* hle, uint32_t w1, uint32_t w2) {
    int8_t gain = static_cast<int8_t>(w1 >> 16);
    uint16_t count = w1 & 0xFFF;
    uint16_t dmem = static_cast<uint16_t>(w2 >> 16);
    alist_multQ44(hle, dmem, count, gain);
}

static void NEAD_OOT_RESAMPLE_ZOH(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_nead();
    uint16_t pitch = static_cast<uint16_t>(w1);
    uint16_t pitch_accu = static_cast<uint16_t>(w2);
    alist_resample_zoh(hle, s.out, s.in, s.count, pitch << 1, pitch_accu);
}

static void NEAD_OOT_FILTER(CHle* hle, uint32_t w1, uint32_t w2) {
    auto& s = hle->alist_nead();
    uint8_t flags = static_cast<uint8_t>(w1 >> 16);
    uint32_t address = w2 & 0x00FFFFFF;

    if (flags > 1) {
        s.filter_count = static_cast<uint16_t>(w1);
        s.filter_lut_address[0] = address;
    } else {
        uint16_t dmem = static_cast<uint16_t>(w1);
        s.filter_lut_address[1] = address + 0x10;
        alist_filter(hle, dmem, s.filter_count, address, s.filter_lut_address);
    }
}

// ── Per-game NEAD ABI tables ──
// Different games' RSP audio ucodes use different command-index-to-handler mappings.
// The index (0-23) is extracted from the high byte of the audio command word,
// and the corresponding callback is invoked.

// OoT / MM / AC NEAD ABI table (matching mupen64plus alist_nead.c)
static const AcmdCallback NEAD_ABI_OOT[24] = {
    SPNOOP,                // 0:  UNKNOWN
    NEAD_ADPCM,            // 1:  ADPCM
    NEAD_CLEARBUFF,        // 2:  CLEARBUFF
    SPNOOP,                // 3:  UNKNOWN
    NEAD_OOT_ADDMIXER,     // 4:  ADDMIXER
    NEAD_RESAMPLE,         // 5:  RESAMPLE
    NEAD_OOT_RESAMPLE_ZOH, // 6:  RESAMPLE_ZOH
    NEAD_OOT_FILTER,       // 7:  FILTER
    NEAD_SETBUFF,          // 8:  SETBUFF
    NEAD_OOT_DUPLICATE,    // 9:  DUPLICATE
    NEAD_DMEMMOVE,         // 10: DMEMMOVE
    NEAD_OOT_LOADADPCM,    // 11: LOADADPCM
    NEAD_OOT_MIXER,        // 12: MIXER
    NEAD_OOT_INTERLEAVE,   // 13: INTERLEAVE
    NEAD_OOT_HILOGAIN,     // 14: HILOGAIN
    NEAD_SETLOOP,          // 15: SETLOOP
    NEAD_OOT_NEAD16,       // 16: NEAD_16
    NEAD_OOT_INTERL,       // 17: INTERL
    NEAD_OOT_ENVSETUP1,    // 18: ENVSETUP1
    NEAD_OOT_ENVMIXER,     // 19: ENVMIXER
    NEAD_OOT_LOADBUFF,     // 20: LOADBUFF
    NEAD_OOT_SAVEBUFF,     // 21: SAVEBUFF
    NEAD_OOT_ENVSETUP2,    // 22: ENVSETUP2
    SPNOOP,                // 23: UNKNOWN
};

// Mario Kart 64: index 3 = MIX (not ENVMIXER), index 14 = HILOGAIN (not POLEF)
static const AcmdCallback NEAD_ABI_MK[32] = {
    SPNOOP,        NEAD_ADPCM,   NEAD_CLEARBUFF, NEAD_MIX,      NEAD_LOADBUFF, NEAD_RESAMPLE, NEAD_SAVEBUFF,
    SPNOOP,        NEAD_SETBUFF, SPNOOP,         NEAD_DMEMMOVE, NEAD_LOADBUFF, NEAD_MIX,      NEAD_INTERLEAVE,
    NEAD_HILOGAIN, NEAD_SETLOOP, SPNOOP,         SPNOOP,        SPNOOP,        SPNOOP,        SPNOOP,
    SPNOOP,        SPNOOP,       SPNOOP,         SPNOOP,        SPNOOP,        SPNOOP,        SPNOOP,
    SPNOOP,        SPNOOP,       SPNOOP,         SPNOOP,
};

// Star Fox 64 / F-Zero X / Wave Race / 1080: POLEF at 14, no FILTER
static const AcmdCallback NEAD_ABI_SF[32] = {
    SPNOOP,     NEAD_ADPCM,   NEAD_CLEARBUFF, NEAD_ENVMIXER, NEAD_LOADBUFF, NEAD_RESAMPLE, NEAD_SAVEBUFF,
    SPNOOP,     NEAD_SETBUFF, SPNOOP,         NEAD_DMEMMOVE, NEAD_LOADBUFF, NEAD_MIX,      NEAD_INTERLEAVE,
    NEAD_POLEF, NEAD_SETLOOP, SPNOOP,         SPNOOP,        SPNOOP,        SPNOOP,        SPNOOP,
    SPNOOP,     SPNOOP,       SPNOOP,         SPNOOP,        SPNOOP,        SPNOOP,        SPNOOP,
    SPNOOP,     SPNOOP,       SPNOOP,         SPNOOP,
};

// Yoshi's Story: index 9 = DMEMMOVE (duplicate), index 14 = FILTER (not POLEF)
static const AcmdCallback NEAD_ABI_YS[32] = {
    SPNOOP,      NEAD_ADPCM,   NEAD_CLEARBUFF, NEAD_ENVMIXER, NEAD_LOADBUFF, NEAD_RESAMPLE, NEAD_SAVEBUFF,
    SPNOOP,      NEAD_SETBUFF, NEAD_DMEMMOVE,  NEAD_DMEMMOVE, NEAD_LOADBUFF, NEAD_MIX,      NEAD_INTERLEAVE,
    NEAD_FILTER, NEAD_SETLOOP, SPNOOP,         SPNOOP,        SPNOOP,        SPNOOP,        SPNOOP,
    SPNOOP,      SPNOOP,       SPNOOP,         SPNOOP,        SPNOOP,        SPNOOP,        SPNOOP,
    SPNOOP,      SPNOOP,       SPNOOP,         SPNOOP,
};

void alist_process_nead_mk(CHle* hle) { alist_process(hle, NEAD_ABI_MK, 32); }

void alist_process_nead_sfj(CHle* hle) { alist_process(hle, NEAD_ABI_SF, 32); }

void alist_process_nead_sf(CHle* hle) { alist_process(hle, NEAD_ABI_SF, 32); }

void alist_process_nead_fz(CHle* hle) { alist_process(hle, NEAD_ABI_SF, 32); }

void alist_process_nead_wrjb(CHle* hle) { alist_process(hle, NEAD_ABI_SF, 32); }

void alist_process_nead_ys(CHle* hle) { alist_process(hle, NEAD_ABI_YS, 32); }

void alist_process_nead_1080(CHle* hle) { alist_process(hle, NEAD_ABI_SF, 32); }

void alist_process_nead_oot(CHle* hle) { alist_process(hle, NEAD_ABI_OOT, 24); }

void alist_process_nead_mm(CHle* hle) { alist_process(hle, NEAD_ABI_OOT, 24); }

void alist_process_nead_mmb(CHle* hle) { alist_process(hle, NEAD_ABI_OOT, 24); }

void alist_process_nead_ac(CHle* hle) { alist_process(hle, NEAD_ABI_OOT, 24); }

// ══════════════════════════════════════════════
// MP3 microcode task
// ══════════════════════════════════════════════

// MP3 quantization scale factors (N64 RSP variant)
// The N64's MP3 ucode uses a custom 14-bit quantization
// table derived from MPEG-1 Layer III, stored in DMEM.

static constexpr int32_t MP3_QUANT_LUT[16] = {0, 1, 3, 5, 7, 9, 15, 23, 31, 47, 63, 95, 127, 191, 255, 383};

// Windowing coefficients for 32-sample IMDCT (subset)
static constexpr int16_t MP3_WINDOW[32] = {0,    213,  426,  640,  851,  1060, 1265, 1466, 1662, 1853, 2037,
                                           2213, 2381, 2540, 2690, 2830, 2959, 3078, 3186, 3283, 3368, 3441,
                                           3502, 3551, 3588, 3612, 3623, 3623, 3612, 3588, 3551, 3502};

static void mp3_dequantize(int16_t* samples, const uint8_t* data, unsigned count) {
    // Dequantize MP3 subband samples using the N64's simplified scheme
    for (unsigned i = 0; i < count && i < 576; i++) {
        uint8_t nibble = (i & 1) ? (data[i / 2] & 0x0F) : (data[i / 2] >> 4);
        int32_t level = MP3_QUANT_LUT[nibble & 0x0F];
        // Apply sign from MS bit
        if (nibble & 0x08) level = -level;
        samples[i] = clamp_s16(level << 4);
    }
}

static void mp3_imdct_window(int16_t* output, const int16_t* freq_samples, unsigned granule_size) {
    // Simplified 32-point IMDCT + windowing
    // Real N64 ucode does a butterfly-style transform; we approximate
    for (unsigned i = 0; i < granule_size && i < 32; i++) {
        int32_t acc = 0;
        for (unsigned k = 0; k < 32 && k < granule_size; k++) {
            // cos((2*i+1)*(2*k+1)*pi/64) approximated as window coeff
            unsigned idx = ((2 * i + 1) * (2 * k + 1)) % 64;
            int16_t coeff = (idx < 32) ? MP3_WINDOW[idx] : -MP3_WINDOW[63 - idx];
            acc += static_cast<int32_t>(freq_samples[k]) * coeff;
        }
        output[i] = clamp_s16(acc >> 14);
    }
}

void mp3_task(CHle* hle, unsigned int index, uint32_t address) {
    uint8_t* dram = hle->dram();
    uint8_t* dmem = hle->dmem();
    if (dram == nullptr || dmem == nullptr) {
        hle->WarnMessage("MP3 task: null memory");
        hle->rsp_break(0);
        return;
    }

    hle->VerboseMessage("MP3 task: index=%u address=0x%08X", index, address);

    // Load compressed MP3 frame data from DRAM into work buffer
    constexpr uint32_t MAX_FRAME_SIZE = 0x400; // 1KB max frame
    uint8_t frame_data[MAX_FRAME_SIZE]{};
    uint32_t frame_size = std::min<uint32_t>(*dmem_u32(hle, TaskOffset::DataSize), MAX_FRAME_SIZE);

    if (frame_size > 0 && address != 0) {
        dram_load_u8(hle, frame_data, address, static_cast<uint16_t>(frame_size));
    }

    // Decode header: the N64 MP3 ucode stores subband count at offset 0
    uint8_t num_subbands = (frame_size >= 4) ? (frame_data[0] >> 4) : 0;
    if (num_subbands == 0) num_subbands = 32;

    // Dequantize frequency-domain samples
    int16_t freq_samples[576]{};
    mp3_dequantize(freq_samples, frame_data + 4, frame_size > 4 ? (frame_size - 4) * 2 : 0);

    // Apply IMDCT + windowing per granule
    // N64 typically does 2 granules of 18 subbands × 32 samples
    int16_t pcm_output[1152]{};
    for (unsigned granule = 0; granule < 2; granule++) {
        for (unsigned sb = 0; sb < num_subbands && sb < 32; sb++) {
            unsigned freq_offset = granule * 18 * 32 + sb * 18;
            unsigned pcm_offset = granule * 576 + sb * 18;
            if (freq_offset + 18 > 576) break;
            if (pcm_offset + 32 > 1152) break;

            mp3_imdct_window(pcm_output + pcm_offset, freq_samples + freq_offset, std::min<unsigned>(18, num_subbands));
        }
    }

    // Store decoded PCM to the output buffer in DRAM
    uint32_t out_addr = *dmem_u32(hle, TaskOffset::OutputBuff);
    if (out_addr != 0) {
        uint16_t out_size = static_cast<uint16_t>(std::min<uint32_t>(1152 * 2, 0x1000));
        dram_store_u16(hle, reinterpret_cast<const uint16_t*>(pcm_output), out_addr, out_size / 2);
    }

    hle->rsp_break(0);
}

// ══════════════════════════════════════════════
// Musyx microcodes
// ══════════════════════════════════════════════

// Musyx voice data structure
// Factor5's Musyx audio engine uses a per-voice
// parameter block stored in RDRAM.

struct MusyxVoice {
    uint32_t sample_addr;   // RDRAM address of sample data
    uint32_t sample_length; // Length in bytes
    uint32_t loop_start;    // Loop start offset
    uint16_t pitch;         // Pitch (fixed 16.16 accumulator rate)
    int16_t volume_left;    // Volume L (Q15)
    int16_t volume_right;   // Volume R (Q15)
    int16_t envelope;       // Current envelope level
    int16_t env_target;     // Target envelope
    int16_t env_rate;       // Computed ramp rate (not stored in DRAM struct)
    uint8_t flags;          // Bit 0=active, bit 1=loop, bit 2=stereo
    uint8_t reserved;       // Padding to 24 bytes
};

static constexpr unsigned MUSYX_MAX_VOICES = 24;
static constexpr unsigned MUSYX_VOICE_SIZE = 24;      // bytes per voice in DRAM
static constexpr unsigned MUSYX_OUTPUT_SAMPLES = 160; // samples per call

static void musyx_process_voices(CHle* hle, uint32_t voice_table_addr, uint8_t* work_buf, int16_t* mix_left,
                                 int16_t* mix_right, unsigned num_samples) {
    uint8_t* dram = hle->dram();
    if (dram == nullptr) return;

    // Clear mix buffers
    std::memset(mix_left, 0, num_samples * sizeof(int16_t));
    std::memset(mix_right, 0, num_samples * sizeof(int16_t));

    for (unsigned v = 0; v < MUSYX_MAX_VOICES; v++) {
        uint32_t vaddr = voice_table_addr + v * MUSYX_VOICE_SIZE;
        if ((vaddr & 0x7FFFFF) + MUSYX_VOICE_SIZE > 0x800000) break;

        // Read voice parameters from DRAM
        MusyxVoice voice{};
        const uint8_t* vdata = dram + (vaddr & 0x7FFFFF);
        voice.sample_addr = (vdata[0] << 24) | (vdata[1] << 16) | (vdata[2] << 8) | vdata[3];
        voice.sample_length = (vdata[4] << 24) | (vdata[5] << 16) | (vdata[6] << 8) | vdata[7];
        voice.loop_start = (vdata[8] << 24) | (vdata[9] << 16) | (vdata[10] << 8) | vdata[11];
        voice.pitch = static_cast<uint16_t>((vdata[12] << 8) | vdata[13]);
        voice.volume_left = static_cast<int16_t>((vdata[14] << 8) | vdata[15]);
        voice.volume_right = static_cast<int16_t>((vdata[16] << 8) | vdata[17]);
        voice.envelope = static_cast<int16_t>((vdata[18] << 8) | vdata[19]);
        voice.env_target = static_cast<int16_t>((vdata[20] << 8) | vdata[21]);
        voice.env_rate = 0; // Not stored in 24-byte struct; ramp is computed from envelope/target
        voice.flags = vdata[22];
        voice.reserved = vdata[23];

        if (!(voice.flags & 0x01)) continue; // Voice not active
        if (voice.sample_addr == 0 || voice.sample_length == 0) continue;

        // Simple pitch-driven sample playback
        uint32_t pitch_accum = 0;
        int32_t env = voice.envelope;

        for (unsigned i = 0; i < num_samples; i++) {
            uint32_t sample_offset = pitch_accum >> 16;
            if (sample_offset >= voice.sample_length) {
                if (voice.flags & 0x02) // Loop
                    sample_offset =
                        voice.loop_start +
                        (sample_offset - voice.sample_length) %
                            (voice.sample_length - voice.loop_start > 0 ? voice.sample_length - voice.loop_start : 1);
                else break; // Voice done
            }

            // Read sample from DRAM (8-bit signed, expanded to 16-bit)
            uint32_t saddr = (voice.sample_addr + sample_offset) & 0x7FFFFF;
            int16_t sample = static_cast<int16_t>(static_cast<int8_t>(dram[saddr])) << 8;

            // Apply envelope
            int32_t enveloped = (static_cast<int32_t>(sample) * env) >> 15;

            // Pan and mix
            mix_left[i] = clamp_s16(mix_left[i] + static_cast<int16_t>((enveloped * voice.volume_left) >> 15));
            mix_right[i] = clamp_s16(mix_right[i] + static_cast<int16_t>((enveloped * voice.volume_right) >> 15));

            pitch_accum += voice.pitch;

            // Envelope ramp toward target (linear over this processing block)
            if (env != voice.env_target) {
                int32_t diff = voice.env_target - env;
                int32_t step = diff / static_cast<int32_t>(num_samples > 1 ? num_samples : 1);
                if (step == 0) step = (diff > 0) ? 1 : -1;
                env += step;
            }
        }
    }
}

void musyx_v1_task(CHle* hle) {
    uint8_t* dmem = hle->dmem();
    if (dmem == nullptr) return;

    uint32_t addr = *dmem_u32(hle, TaskOffset::DataPtr);
    uint32_t size = *dmem_u32(hle, TaskOffset::DataSize);

    if (size == 0 || addr == 0) {
        hle->VerboseMessage("Musyx v1: no data");
        hle->rsp_break(0);
        return;
    }

    // Load Musyx control data from DRAM
    uint8_t work[0x1000]{};
    dram_load_u8(hle, work, addr, std::min<uint32_t>(size, 0x1000));

    hle->VerboseMessage("Musyx v1: addr=0x%08X size=0x%X", addr, size);

    // Parse header: first 4 bytes = voice table address, next 4 = output address
    uint32_t voice_table = (work[0] << 24) | (work[1] << 16) | (work[2] << 8) | work[3];
    uint32_t output_addr = (work[4] << 24) | (work[5] << 16) | (work[6] << 8) | work[7];

    // Process all active voices
    int16_t mix_left[MUSYX_OUTPUT_SAMPLES]{};
    int16_t mix_right[MUSYX_OUTPUT_SAMPLES]{};
    musyx_process_voices(hle, voice_table, work + 8, mix_left, mix_right, MUSYX_OUTPUT_SAMPLES);

    // Interleave and store output to DRAM
    if (output_addr != 0) {
        int16_t interleaved[MUSYX_OUTPUT_SAMPLES * 2];
        for (unsigned i = 0; i < MUSYX_OUTPUT_SAMPLES; i++) {
            interleaved[i * 2] = mix_left[i];
            interleaved[i * 2 + 1] = mix_right[i];
        }
        dram_store_u16(hle, reinterpret_cast<const uint16_t*>(interleaved), output_addr, MUSYX_OUTPUT_SAMPLES * 2);
    }

    hle->rsp_break(0);
}

void musyx_v2_task(CHle* hle) {
    uint8_t* dmem = hle->dmem();
    if (dmem == nullptr) return;

    uint32_t addr = *dmem_u32(hle, TaskOffset::DataPtr);
    uint32_t size = *dmem_u32(hle, TaskOffset::DataSize);

    if (size == 0 || addr == 0) {
        hle->VerboseMessage("Musyx v2: no data");
        hle->rsp_break(0);
        return;
    }

    // v2 has an extended header: 16 bytes of config before voice table
    uint8_t work[0x1000]{};
    dram_load_u8(hle, work, addr, std::min<uint32_t>(size, 0x1000));

    hle->VerboseMessage("Musyx v2: addr=0x%08X size=0x%X", addr, size);

    // v2 header: [0..3] voice_table, [4..7] output, [8..9] num_samples, [10..11] reverb_size
    uint32_t voice_table = (work[0] << 24) | (work[1] << 16) | (work[2] << 8) | work[3];
    uint32_t output_addr = (work[4] << 24) | (work[5] << 16) | (work[6] << 8) | work[7];
    uint16_t num_samples = static_cast<uint16_t>((work[8] << 8) | work[9]);
    uint16_t reverb_len = static_cast<uint16_t>((work[10] << 8) | work[11]);

    if (num_samples == 0 || num_samples > MUSYX_OUTPUT_SAMPLES) num_samples = MUSYX_OUTPUT_SAMPLES;

    // Process voices
    int16_t mix_left[MUSYX_OUTPUT_SAMPLES]{};
    int16_t mix_right[MUSYX_OUTPUT_SAMPLES]{};
    musyx_process_voices(hle, voice_table, work + 16, mix_left, mix_right, num_samples);

    // v2 adds a simple reverb pass: mix delayed output back
    if (reverb_len > 0 && reverb_len < num_samples) {
        for (unsigned i = reverb_len; i < num_samples; i++) {
            mix_left[i] = clamp_s16(mix_left[i] + (mix_left[i - reverb_len] >> 2));
            mix_right[i] = clamp_s16(mix_right[i] + (mix_right[i - reverb_len] >> 2));
        }
    }

    // Interleave and store
    if (output_addr != 0) {
        int16_t interleaved[MUSYX_OUTPUT_SAMPLES * 2];
        for (unsigned i = 0; i < num_samples; i++) {
            interleaved[i * 2] = mix_left[i];
            interleaved[i * 2 + 1] = mix_right[i];
        }
        dram_store_u16(hle, reinterpret_cast<const uint16_t*>(interleaved), output_addr, num_samples * 2);
    }

    hle->rsp_break(0);
}

// ══════════════════════════════════════════════
// JPEG decode microcodes
// Based on mupen64plus-rsp-hle jpeg.c
// ══════════════════════════════════════════════

static constexpr unsigned int SUBBLOCK_SIZE = 64;

// Zig-zag scan order for 8x8 DCT block
static constexpr unsigned int ZIGZAG_TABLE[SUBBLOCK_SIZE] = {
    0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42, 3,  8,  12, 17, 25, 30,
    41, 43, 9,  11, 18, 24, 31, 40, 44, 53, 10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38,
    46, 51, 55, 60, 21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63,
};

// Transposition indices
static constexpr unsigned int TRANSPOSE_TABLE[SUBBLOCK_SIZE] = {
    0,  8,  16, 24, 32, 40, 48, 56, 1,  9,  17, 25, 33, 41, 49, 57, 2,  10, 18, 26, 34, 42,
    50, 58, 3,  11, 19, 27, 35, 43, 51, 59, 4,  12, 20, 28, 36, 44, 52, 60, 5,  13, 21, 29,
    37, 45, 53, 61, 6,  14, 22, 30, 38, 46, 54, 62, 7,  15, 23, 31, 39, 47, 55, 63};

// Standard JPEG quantization table (for Ogre Battle variant)
static constexpr int16_t DEFAULT_QTABLE[SUBBLOCK_SIZE] = {
    16,  12,  14,  14,  18, 24, 49, 72, 11,  12,  13,  17,  22, 35, 64, 92, 10,  14,  16,  22, 37, 55,
    78,  95,  16,  19,  24, 29, 56, 64, 87,  98,  24,  26,  40, 51, 68, 81, 103, 112, 40,  58, 57, 87,
    109, 104, 121, 100, 51, 60, 69, 80, 103, 113, 120, 103, 61, 55, 56, 62, 77,  92,  101, 99};

// IDCT constants: Cn = alpha * cos(n * PI / 16)
static constexpr float IDCT_C3 = 1.175875602f;
static constexpr float IDCT_C6 = 0.541196100f;
static constexpr float IDCT_K[10] = {
    0.765366865f,  //  C2-C6
    -1.847759065f, // -C2-C6
    -0.390180644f, //  C5-C3
    -1.961570561f, // -C5-C3
    1.501321110f,  //  C1+C3-C5-C7
    2.053119869f,  //  C1+C3-C5+C7
    3.072711027f,  //  C1+C3+C5-C7
    0.298631336f,  // -C1+C3+C5-C7
    -0.899976223f, //  C7-C3
    -2.562915448f  // -C1-C3
};

// ── Pixel helpers ──

static uint8_t clamp_u8(int16_t x) { return (x & 0xff00) ? ((-x) >> 15) & 0xff : static_cast<uint8_t>(x); }

static int16_t clamp_s12(int16_t x) {
    if (x < -0x800) x = -0x800;
    else if (x > 0x7f0) x = 0x7f0;
    return x;
}

static uint16_t clamp_RGBA_component(int16_t x) {
    if (x > 0xff0) x = 0xff0;
    else if (x < 0) x = 0;
    return static_cast<uint16_t>(x & 0xf80);
}

static uint32_t GetUYVY(int16_t y1, int16_t y2, int16_t u, int16_t v) {
    return static_cast<uint32_t>(clamp_u8(u)) << 24 | static_cast<uint32_t>(clamp_u8(y1)) << 16 |
           static_cast<uint32_t>(clamp_u8(v)) << 8 | static_cast<uint32_t>(clamp_u8(y2));
}

static uint16_t GetRGBA(int16_t y, int16_t u, int16_t v) {
    const float fY = static_cast<float>(y) + 2048.0f;
    const float fU = static_cast<float>(u);
    const float fV = static_cast<float>(v);

    const uint16_t r = clamp_RGBA_component(static_cast<int16_t>(fY + 1.4025f * fV));
    const uint16_t g = clamp_RGBA_component(static_cast<int16_t>(fY - 0.3443f * fU - 0.7144f * fV));
    const uint16_t b = clamp_RGBA_component(static_cast<int16_t>(fY + 1.7729f * fU));

    return static_cast<uint16_t>((r << 4) | (g >> 1) | (b >> 6) | 1);
}

// ── Tile line emitters ──

static void EmitYUVTileLine(CHle* hle, const int16_t* y, const int16_t* u, uint32_t address) {
    uint32_t uyvy[8];
    const int16_t* v = u + SUBBLOCK_SIZE;
    const int16_t* y2 = y + SUBBLOCK_SIZE;

    uyvy[0] = GetUYVY(y[0], y[1], u[0], v[0]);
    uyvy[1] = GetUYVY(y[2], y[3], u[1], v[1]);
    uyvy[2] = GetUYVY(y[4], y[5], u[2], v[2]);
    uyvy[3] = GetUYVY(y[6], y[7], u[3], v[3]);
    uyvy[4] = GetUYVY(y2[0], y2[1], u[4], v[4]);
    uyvy[5] = GetUYVY(y2[2], y2[3], u[5], v[5]);
    uyvy[6] = GetUYVY(y2[4], y2[5], u[6], v[6]);
    uyvy[7] = GetUYVY(y2[6], y2[7], u[7], v[7]);

    dram_store_u32(hle, uyvy, address, 8);
}

static void EmitRGBATileLine(CHle* hle, const int16_t* y, const int16_t* u, uint32_t address) {
    uint16_t rgba[16];
    const int16_t* v = u + SUBBLOCK_SIZE;
    const int16_t* y2 = y + SUBBLOCK_SIZE;

    rgba[0] = GetRGBA(y[0], u[0], v[0]);
    rgba[1] = GetRGBA(y[1], u[0], v[0]);
    rgba[2] = GetRGBA(y[2], u[1], v[1]);
    rgba[3] = GetRGBA(y[3], u[1], v[1]);
    rgba[4] = GetRGBA(y[4], u[2], v[2]);
    rgba[5] = GetRGBA(y[5], u[2], v[2]);
    rgba[6] = GetRGBA(y[6], u[3], v[3]);
    rgba[7] = GetRGBA(y[7], u[3], v[3]);
    rgba[8] = GetRGBA(y2[0], u[4], v[4]);
    rgba[9] = GetRGBA(y2[1], u[4], v[4]);
    rgba[10] = GetRGBA(y2[2], u[5], v[5]);
    rgba[11] = GetRGBA(y2[3], u[5], v[5]);
    rgba[12] = GetRGBA(y2[4], u[6], v[6]);
    rgba[13] = GetRGBA(y2[5], u[6], v[6]);
    rgba[14] = GetRGBA(y2[6], u[7], v[7]);
    rgba[15] = GetRGBA(y2[7], u[7], v[7]);

    dram_store_u16(hle, rgba, address, 16);
}

// ── Tile emitters ──

using tile_line_emitter_t = void (*)(CHle*, const int16_t*, const int16_t*, uint32_t);
using subblock_transform_t = void (*)(int16_t*, const int16_t*);

static void EmitTilesMode0(CHle* hle, tile_line_emitter_t emit_line, const int16_t* macroblock, uint32_t address) {
    unsigned int y_offset = 0;
    unsigned int u_offset = 2 * SUBBLOCK_SIZE;

    for (unsigned int i = 0; i < 8; ++i) {
        emit_line(hle, &macroblock[y_offset], &macroblock[u_offset], address);
        y_offset += 8;
        u_offset += 8;
        address += 32;
    }
}

static void EmitTilesMode2(CHle* hle, tile_line_emitter_t emit_line, const int16_t* macroblock, uint32_t address) {
    unsigned int y_offset = 0;
    unsigned int u_offset = 4 * SUBBLOCK_SIZE;

    for (unsigned int i = 0; i < 8; ++i) {
        emit_line(hle, &macroblock[y_offset], &macroblock[u_offset], address);
        emit_line(hle, &macroblock[y_offset + 8], &macroblock[u_offset], address + 32);

        y_offset += (i == 3) ? SUBBLOCK_SIZE + 16 : 16;
        u_offset += 8;
        address += 64;
    }
}

// ── Subblock operations ──

static void ReorderSubBlock(int16_t* dst, const int16_t* src, const unsigned int* table) {
    for (unsigned int i = 0; i < SUBBLOCK_SIZE; ++i) dst[i] = src[table[i]];
}

static void TransposeSubBlock(int16_t* dst, const int16_t* src) { ReorderSubBlock(dst, src, TRANSPOSE_TABLE); }

static void ZigZagSubBlock(int16_t* dst, const int16_t* src) { ReorderSubBlock(dst, src, ZIGZAG_TABLE); }

static void MultSubBlocks(int16_t* dst, const int16_t* src1, const int16_t* src2, unsigned int shift) {
    for (unsigned int i = 0; i < SUBBLOCK_SIZE; ++i) {
        int32_t v = static_cast<int32_t>(src1[i]) * static_cast<int32_t>(src2[i]);
        dst[i] = clamp_s16(v) << shift;
    }
}

static void ScaleSubBlock(int16_t* dst, const int16_t* src, int16_t scale) {
    for (unsigned int i = 0; i < SUBBLOCK_SIZE; ++i) {
        int32_t v = static_cast<int32_t>(src[i]) * static_cast<int32_t>(scale);
        dst[i] = clamp_s16(v);
    }
}

static void RShiftSubBlock(int16_t* dst, const int16_t* src, unsigned int shift) {
    for (unsigned int i = 0; i < SUBBLOCK_SIZE; ++i) dst[i] = src[i] >> shift;
}

// ── IDCT ──

static void InverseDCT1D(const float* x, float* dst, unsigned int stride) {
    float e[4], f[4];

    const float x15 = IDCT_K[2] * (x[1] + x[5]);
    const float x37 = IDCT_K[3] * (x[3] + x[7]);
    const float x17 = IDCT_K[8] * (x[1] + x[7]);
    const float x35 = IDCT_K[9] * (x[3] + x[5]);
    const float x1357 = IDCT_C3 * (x[1] + x[3] + x[5] + x[7]);
    const float x26 = IDCT_C6 * (x[2] + x[6]);

    f[0] = x[0] + x[4];
    f[1] = x[0] - x[4];
    f[2] = x26 + IDCT_K[0] * x[2];
    f[3] = x26 + IDCT_K[1] * x[6];

    e[0] = x1357 + x15 + IDCT_K[4] * x[1] + x17;
    e[1] = x1357 + x37 + IDCT_K[6] * x[3] + x35;
    e[2] = x1357 + x15 + IDCT_K[5] * x[5] + x35;
    e[3] = x1357 + x37 + IDCT_K[7] * x[7] + x17;

    *dst = f[0] + f[2] + e[0];
    dst += stride;
    *dst = f[1] + f[3] + e[1];
    dst += stride;
    *dst = f[1] - f[3] + e[2];
    dst += stride;
    *dst = f[0] - f[2] + e[3];
    dst += stride;
    *dst = f[0] - f[2] - e[3];
    dst += stride;
    *dst = f[1] - f[3] - e[2];
    dst += stride;
    *dst = f[1] + f[3] - e[1];
    dst += stride;
    *dst = f[0] + f[2] - e[0];
}

static void InverseDCTSubBlock(int16_t* dst, const int16_t* src) {
    float x[8];
    float block[SUBBLOCK_SIZE];

    // IDCT 1D on rows (+transposition)
    for (unsigned int i = 0; i < 8; ++i) {
        for (unsigned int j = 0; j < 8; ++j) x[j] = static_cast<float>(src[i * 8 + j]);
        InverseDCT1D(x, &block[i], 8);
    }

    // IDCT 1D on columns (thanks to previous transposition)
    for (unsigned int i = 0; i < 8; ++i) {
        InverseDCT1D(&block[i * 8], x, 1);
        // C4 = 1 normalization implies a division by 8
        for (unsigned int j = 0; j < 8; ++j) dst[i + j * 8] = static_cast<int16_t>(x[j]) >> 3;
    }
}

// ── Rescale transforms (for PS0 / Pokemon Stadium JP) ──

static void RescaleYSubBlock(int16_t* dst, const int16_t* src) {
    for (unsigned int i = 0; i < SUBBLOCK_SIZE; ++i)
        dst[i] = static_cast<int16_t>(((static_cast<uint32_t>(clamp_s12(src[i]) + 0x800) * 0xdb0) >> 16) + 0x10);
}

static void RescaleUVSubBlock(int16_t* dst, const int16_t* src) {
    for (unsigned int i = 0; i < SUBBLOCK_SIZE; ++i)
        dst[i] = static_cast<int16_t>(((static_cast<int32_t>(clamp_s12(src[i])) * 0xe00) >> 16) + 0x80);
}

// ── Macroblock decoders ──

static void decode_macroblock_std(subblock_transform_t transform_luma, subblock_transform_t transform_chroma,
                                  int16_t* macroblock, unsigned int subblock_count,
                                  const int16_t qtables[3][SUBBLOCK_SIZE]) {
    unsigned int q = 0;

    for (unsigned int sb = 0; sb < subblock_count; ++sb) {
        int16_t tmp_sb[SUBBLOCK_SIZE];
        const bool isChroma = (subblock_count - sb <= 2);

        if (isChroma) ++q;

        MultSubBlocks(macroblock, macroblock, qtables[q], 4);
        ZigZagSubBlock(tmp_sb, macroblock);
        InverseDCTSubBlock(macroblock, tmp_sb);

        if (isChroma) {
            if (transform_chroma != nullptr) transform_chroma(macroblock, macroblock);
        } else {
            if (transform_luma != nullptr) transform_luma(macroblock, macroblock);
        }

        macroblock += SUBBLOCK_SIZE;
    }
}

static void decode_macroblock_ob(int16_t* macroblock, int32_t* y_dc, int32_t* u_dc, int32_t* v_dc,
                                 const int16_t* qtable) {
    for (int sb = 0; sb < 6; ++sb) {
        int16_t tmp_sb[SUBBLOCK_SIZE];

        // Update DC prediction
        int32_t dc = static_cast<int32_t>(macroblock[0]);
        switch (sb) {
        case 0:
        case 1:
        case 2:
        case 3:
            *y_dc += dc;
            macroblock[0] = static_cast<int16_t>(*y_dc & 0xffff);
            break;
        case 4:
            *u_dc += dc;
            macroblock[0] = static_cast<int16_t>(*u_dc & 0xffff);
            break;
        case 5:
            *v_dc += dc;
            macroblock[0] = static_cast<int16_t>(*v_dc & 0xffff);
            break;
        }

        ZigZagSubBlock(tmp_sb, macroblock);
        if (qtable != nullptr) MultSubBlocks(tmp_sb, tmp_sb, qtable, 0);
        TransposeSubBlock(macroblock, tmp_sb);
        InverseDCTSubBlock(macroblock, macroblock);

        macroblock += SUBBLOCK_SIZE;
    }
}

// ── Standard JPEG decoder (used by PS and PS0) ──

static void jpeg_decode_std(CHle* hle, const char* version, subblock_transform_t transform_luma,
                            subblock_transform_t transform_chroma, tile_line_emitter_t emit_line) {
    int16_t qtables[3][SUBBLOCK_SIZE];
    int16_t macroblock[6 * SUBBLOCK_SIZE];

    if (*dmem_u32(hle, TaskOffset::Flags) & 0x1) {
        hle->WarnMessage("jpeg_decode_%s: task yielding not implemented", version);
        return;
    }

    uint32_t data_ptr = *dmem_u32(hle, TaskOffset::DataPtr);
    uint32_t address = *dram_u32(hle, data_ptr);
    uint32_t macroblock_count = *dram_u32(hle, data_ptr + 4);
    uint32_t mode = *dram_u32(hle, data_ptr + 8);
    uint32_t qtableY_ptr = *dram_u32(hle, data_ptr + 12);
    uint32_t qtableU_ptr = *dram_u32(hle, data_ptr + 16);
    uint32_t qtableV_ptr = *dram_u32(hle, data_ptr + 20);

    hle->VerboseMessage("jpeg_decode_%s: *buffer=%x, #MB=%d, mode=%d, *Qy=%x, *Qu=%x, *Qv=%x", version, address,
                        macroblock_count, mode, qtableY_ptr, qtableU_ptr, qtableV_ptr);

    if (mode != 0 && mode != 2) {
        hle->WarnMessage("jpeg_decode_%s: invalid mode %d", version, mode);
        return;
    }

    unsigned int subblock_count = mode + 4;
    unsigned int macroblock_size = subblock_count * SUBBLOCK_SIZE;

    dram_load_u16(hle, reinterpret_cast<uint16_t*>(qtables[0]), qtableY_ptr, SUBBLOCK_SIZE);
    dram_load_u16(hle, reinterpret_cast<uint16_t*>(qtables[1]), qtableU_ptr, SUBBLOCK_SIZE);
    dram_load_u16(hle, reinterpret_cast<uint16_t*>(qtables[2]), qtableV_ptr, SUBBLOCK_SIZE);

    for (uint32_t mb = 0; mb < macroblock_count; ++mb) {
        dram_load_u16(hle, reinterpret_cast<uint16_t*>(macroblock), address, macroblock_size);
        decode_macroblock_std(transform_luma, transform_chroma, macroblock, subblock_count, qtables);

        if (mode == 0) EmitTilesMode0(hle, emit_line, macroblock, address);
        else EmitTilesMode2(hle, emit_line, macroblock, address);

        address += 2 * macroblock_size;
    }
}

// ── Public JPEG decode entry points ──

void jpeg_decode_PS0(CHle* hle) {
    // Pokemon Stadium (Japan) - YUV output with rescaling
    hle->VerboseMessage("JPEG decode PS0");
    jpeg_decode_std(hle, "PS0", RescaleYSubBlock, RescaleUVSubBlock, EmitYUVTileLine);
    hle->rsp_break(0);
}

void jpeg_decode_PS(CHle* hle) {
    // Ocarina of Time, Pokemon Stadium 1/2 - RGBA output
    hle->VerboseMessage("JPEG decode PS");
    jpeg_decode_std(hle, "PS", nullptr, nullptr, EmitRGBATileLine);
    hle->rsp_break(0);
}

void jpeg_decode_OB(CHle* hle) {
    // Ogre Battle 64, Bottom of the 9th - YUV with DC prediction
    hle->VerboseMessage("JPEG decode OB");

    int16_t qtable[SUBBLOCK_SIZE];
    int32_t y_dc = 0, u_dc = 0, v_dc = 0;

    uint32_t address = *dmem_u32(hle, TaskOffset::DataPtr);
    uint32_t macroblock_count = *dmem_u32(hle, TaskOffset::DataSize);
    int32_t qscale = static_cast<int32_t>(*dmem_u32(hle, TaskOffset::YieldDataSize));

    hle->VerboseMessage("jpeg_decode_OB: *buffer=%x, #MB=%d, qscale=%d", address, macroblock_count, qscale);

    if (qscale != 0) {
        if (qscale > 0) ScaleSubBlock(qtable, DEFAULT_QTABLE, static_cast<int16_t>(qscale));
        else RShiftSubBlock(qtable, DEFAULT_QTABLE, static_cast<unsigned int>(-qscale));
    }

    for (uint32_t mb = 0; mb < macroblock_count; ++mb) {
        int16_t macroblock[6 * SUBBLOCK_SIZE];

        dram_load_u16(hle, reinterpret_cast<uint16_t*>(macroblock), address, 6 * SUBBLOCK_SIZE);
        decode_macroblock_ob(macroblock, &y_dc, &u_dc, &v_dc, (qscale != 0) ? qtable : nullptr);
        EmitTilesMode2(hle, EmitYUVTileLine, macroblock, address);

        address += 2 * 6 * SUBBLOCK_SIZE;
    }

    hle->rsp_break(0);
}

} // namespace Nova64::RSP::HLE
