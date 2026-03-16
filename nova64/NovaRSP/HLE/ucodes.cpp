// SuperNova64 - N64 Emulator
// RSP HLE Microcode Dispatchers
// CIC x105 boot ucode, audio list processors, MP3, Musyx, JPEG

#include "ucodes.hpp"
#include "alist.hpp"
#include "audio.hpp"
#include "hle.hpp"
#include "mem.hpp"
#include "arithmetics.hpp"
#include <algorithm>
#include <cstring>

namespace Nova64::RSP::HLE {

// ──────────────────────────────────────────────
// CIC x105 boot microcode
// Shuffles IMEM data into RDRAM for IPL3 signature
// ──────────────────────────────────────────────

void cicx105_ucode(CHle* hle)
{
    // CIC-NUS-6105 challenge/response boot code
    // Copies IMEM data into DRAM at a fixed offset
    uint8_t* imem = hle->imem();
    uint8_t* dram = hle->dram();
    if (imem == nullptr || dram == nullptr) return;

    constexpr uint32_t count = 0x170;
    for (uint32_t i = 0; i < count; i += 4)
    {
        uint32_t value = *u32(imem, 0x040 + i);
        *u32(dram, 0x00000040 + i) = value;
    }

    hle->rsp_break(0);
}

// ══════════════════════════════════════════════
// Standard Audio list processing (original ABI)
// ══════════════════════════════════════════════

// Forward declarations of command handlers
static void SPNOOP(CHle*, uint32_t, uint32_t) { }

// ── Audio ABI ──────────────────────────────────

static void AUDIO_SEGMENTS(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& state = hle->alist_audio();
    const uint32_t seg = (w2 >> 24) & 0x0F;
    if (seg < N_SEGMENTS)
        state.segments[seg] = w2 & 0x00FFFFFF;
}

static void AUDIO_SETBUFF(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_audio();
    if ((w1 >> 16) & 0x8)
    {
        // Aux buffer setup
        s.dry_right = static_cast<uint16_t>(w1);
        s.wet_left  = static_cast<uint16_t>(w2 >> 16);
        s.wet_right = static_cast<uint16_t>(w2);
    }
    else
    {
        s.in    = static_cast<uint16_t>(w1);
        s.out   = static_cast<uint16_t>(w2 >> 16);
        s.count = static_cast<uint16_t>(w2);
    }
}

static void AUDIO_DMEMMOVE(CHle* hle, uint32_t w1, uint32_t w2)
{
    uint16_t dmemi = static_cast<uint16_t>(w1);
    uint16_t dmemo = static_cast<uint16_t>(w2 >> 16);
    uint16_t count = static_cast<uint16_t>(w2);
    alist_move(hle, dmemo, dmemi, count);
}

static void AUDIO_LOADBUFF(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_audio();
    uint32_t addr = alist_get_address(hle, w2, s.segments, N_SEGMENTS);
    alist_load(hle, s.in, addr, s.count);
    (void)w1;
}

static void AUDIO_SAVEBUFF(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_audio();
    uint32_t addr = alist_get_address(hle, w2, s.segments, N_SEGMENTS);
    alist_save(hle, s.out, addr, s.count);
    (void)w1;
}

static void AUDIO_SETLOOP(CHle* hle, uint32_t /*w1*/, uint32_t w2)
{
    hle->alist_audio().loop = w2 & 0x00FFFFFF;
}

static void AUDIO_ADPCM(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_audio();
    bool init = (w1 >> 16) & 0x01;
    bool loop = (w1 >> 16) & 0x02;
    uint32_t addr = alist_get_address(hle, w2, s.segments, N_SEGMENTS);
    alist_adpcm(hle, init, loop, false, s.out, s.in, s.count,
                s.table, s.loop, addr);
}

static void AUDIO_RESAMPLE(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_audio();
    bool init  = (w1 >> 16) & 0x01;
    bool flag2 = (w1 >> 16) & 0x02;
    uint32_t pitch = w1 & 0xFFFF;
    uint32_t addr = alist_get_address(hle, w2, s.segments, N_SEGMENTS);
    alist_resample(hle, init, flag2, s.out, s.in, s.count, pitch << 1, addr);
}

static void AUDIO_ENVMIXER(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_audio();
    bool init = (w1 >> 16) & 0x01;
    bool aux  = (w1 >> 16) & 0x08;
    uint32_t addr = alist_get_address(hle, w2, s.segments, N_SEGMENTS);
    alist_envmix_exp(hle, init, aux,
                     s.out, s.dry_right, s.wet_left, s.wet_right,
                     s.in, s.count, s.dry, s.wet,
                     s.vol, s.target, s.rate, addr);
}

static void AUDIO_SETVOL(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_audio();
    uint8_t flags = static_cast<uint8_t>(w1 >> 16);
    if (flags & 0x04)
    {
        // Set target
        s.target[0] = static_cast<int16_t>(w1);
        s.target[1] = static_cast<int16_t>(w2 >> 16);
    }
    else if (flags & 0x08)
    {
        // Set rate
        s.rate[0] = static_cast<int32_t>(w1 & 0xFFFF) << 16 | (w2 >> 16);
        s.rate[1] = static_cast<int32_t>(w2 & 0xFFFF) << 16;
    }
    else
    {
        // Set volume
        s.vol[0] = static_cast<int16_t>(w1);
        s.dry    = static_cast<int16_t>(w2 >> 16);
        s.wet    = static_cast<int16_t>(w2);
    }
}

static void AUDIO_CLEARBUFF(CHle* hle, uint32_t w1, uint32_t w2)
{
    uint16_t dmem  = static_cast<uint16_t>(w1);
    uint16_t count = static_cast<uint16_t>(w2);
    alist_clear(hle, dmem, count);
}

static void AUDIO_MIXER(CHle* hle, uint32_t w1, uint32_t w2)
{
    uint16_t dmemo = static_cast<uint16_t>(w2 >> 16);
    uint16_t dmemi = static_cast<uint16_t>(w2);
    auto& s = hle->alist_audio();
    int16_t gain = static_cast<int16_t>(w1);
    alist_mix(hle, dmemo, dmemi, s.count, gain);
}

static void AUDIO_INTERLEAVE(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_audio();
    uint16_t left  = static_cast<uint16_t>(w2 >> 16);
    uint16_t right = static_cast<uint16_t>(w2);
    alist_interleave(hle, s.out, left, right, s.count);
    (void)w1;
}

static void AUDIO_POLEF(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_audio();
    bool init = (w1 >> 16) & 0x01;
    uint16_t gain = static_cast<uint16_t>(w1);
    uint32_t addr = alist_get_address(hle, w2, s.segments, N_SEGMENTS);
    alist_polef(hle, init, s.out, s.in, s.count, gain, s.table, addr);
}

void alist_process_audio(CHle* hle)
{
    static const AcmdCallback ABI[32] = {
        SPNOOP,            AUDIO_ADPCM,      AUDIO_CLEARBUFF,  AUDIO_ENVMIXER,
        AUDIO_LOADBUFF,    AUDIO_RESAMPLE,    AUDIO_SAVEBUFF,   AUDIO_SEGMENTS,
        AUDIO_SETBUFF,     AUDIO_SETVOL,      AUDIO_DMEMMOVE,   AUDIO_LOADBUFF,
        AUDIO_MIXER,       AUDIO_INTERLEAVE,  AUDIO_POLEF,      AUDIO_SETLOOP,
        SPNOOP,            SPNOOP,            SPNOOP,           SPNOOP,
        SPNOOP,            SPNOOP,            SPNOOP,           SPNOOP,
        SPNOOP,            SPNOOP,            SPNOOP,           SPNOOP,
        SPNOOP,            SPNOOP,            SPNOOP,           SPNOOP,
    };
    alist_process(hle, ABI, 32);
}

// ── GoldenEye variant ─────────────────────────
// GE uses the GE-specific envelope mixer (fixed linear ramp)

static void AUDIO_ENVMIXER_GE(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_audio();
    bool init = (w1 >> 16) & 0x01;
    bool aux  = (w1 >> 16) & 0x08;
    uint32_t addr = alist_get_address(hle, w2, s.segments, N_SEGMENTS);
    alist_envmix_ge(hle, init, aux,
                     s.out, s.dry_right, s.wet_left, s.wet_right,
                     s.in, s.count, s.dry, s.wet,
                     s.vol, s.target, s.rate, addr);
}

void alist_process_audio_ge(CHle* hle)
{
    static const AcmdCallback ABI_GE[32] = {
        SPNOOP,            AUDIO_ADPCM,      AUDIO_CLEARBUFF,  AUDIO_ENVMIXER_GE,
        AUDIO_LOADBUFF,    AUDIO_RESAMPLE,    AUDIO_SAVEBUFF,   AUDIO_SEGMENTS,
        AUDIO_SETBUFF,     AUDIO_SETVOL,      AUDIO_DMEMMOVE,   AUDIO_LOADBUFF,
        AUDIO_MIXER,       AUDIO_INTERLEAVE,  AUDIO_POLEF,      AUDIO_SETLOOP,
        SPNOOP,            SPNOOP,            SPNOOP,           SPNOOP,
        SPNOOP,            SPNOOP,            SPNOOP,           SPNOOP,
        SPNOOP,            SPNOOP,            SPNOOP,           SPNOOP,
        SPNOOP,            SPNOOP,            SPNOOP,           SPNOOP,
    };
    alist_process(hle, ABI_GE, 32);
}

// ── Banjo-Conker variant ──────────────────────
// BC also uses the GE-style envelope mixer

void alist_process_audio_bc(CHle* hle)
{
    static const AcmdCallback ABI_BC[32] = {
        SPNOOP,            AUDIO_ADPCM,      AUDIO_CLEARBUFF,  AUDIO_ENVMIXER_GE,
        AUDIO_LOADBUFF,    AUDIO_RESAMPLE,    AUDIO_SAVEBUFF,   AUDIO_SEGMENTS,
        AUDIO_SETBUFF,     AUDIO_SETVOL,      AUDIO_DMEMMOVE,   AUDIO_LOADBUFF,
        AUDIO_MIXER,       AUDIO_INTERLEAVE,  AUDIO_POLEF,      AUDIO_SETLOOP,
        SPNOOP,            SPNOOP,            SPNOOP,           SPNOOP,
        SPNOOP,            SPNOOP,            SPNOOP,           SPNOOP,
        SPNOOP,            SPNOOP,            SPNOOP,           SPNOOP,
        SPNOOP,            SPNOOP,            SPNOOP,           SPNOOP,
    };
    alist_process(hle, ABI_BC, 32);
}

// ══════════════════════════════════════════════
// Naudio processing variants
// Complete ABI with all commands from Project64
// ══════════════════════════════════════════════

static void NAUDIO_SETBUFF(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_naudio();
    uint8_t flags = static_cast<uint8_t>(w1 >> 16);
    if (flags & A_AUX)
    {
        // Aux buffer setup: set loop point
        s.loop = w2 & 0x00FFFFFF;
    }
    else
    {
        // Main buffer setup
        // (naudio encodes in/out/count differently per variant)
    }
}

static void NAUDIO_ADPCM(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_naudio();
    bool init = (w1 >> 16) & A_INIT;
    bool loop = (w1 >> 16) & A_LOOP;
    uint16_t dmemo = static_cast<uint16_t>(w1);
    uint16_t dmemi = static_cast<uint16_t>(w2 >> 16);
    uint16_t count = static_cast<uint16_t>(w2);
    alist_adpcm(hle, init, loop, false, dmemo, dmemi, count,
                s.table, s.loop, w2 & 0x00FFFFFF);
}

static void NAUDIO_RESAMPLE(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_naudio();
    bool init  = (w1 >> 16) & A_INIT;
    bool flag2 = (w1 >> 16) & A_LOOP;
    uint16_t dmemo = static_cast<uint16_t>(w1);
    uint16_t dmemi = static_cast<uint16_t>(w2 >> 16);
    uint16_t count = static_cast<uint16_t>(w2 & 0xFFFF);
    uint32_t pitch = (w1 >> 0) & 0xFFFF;
    alist_resample(hle, init, flag2, dmemo, dmemi, count, pitch << 1, w2 & 0x00FFFFFF);
}

static void NAUDIO_DMEMMOVE(CHle* hle, uint32_t w1, uint32_t w2)
{
    uint16_t dmemi = static_cast<uint16_t>(w1);
    uint16_t dmemo = static_cast<uint16_t>(w2 >> 16);
    uint16_t count = static_cast<uint16_t>(w2);
    alist_move(hle, dmemo, dmemi, count);
}

static void NAUDIO_SETVOL(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_naudio();
    uint8_t flags = static_cast<uint8_t>(w1 >> 16);
    if (flags & A_VOL)
    {
        s.target[0] = static_cast<int16_t>(w1);
        s.target[1] = static_cast<int16_t>(w2 >> 16);
    }
    else if (flags & A_AUX)
    {
        s.rate[0] = static_cast<int32_t>(w1 & 0xFFFF) << 16 | (w2 >> 16);
        s.rate[1] = static_cast<int32_t>(w2 & 0xFFFF) << 16;
    }
    else
    {
        s.vol[0] = static_cast<int16_t>(w1);
        s.dry    = static_cast<int16_t>(w2 >> 16);
        s.wet    = static_cast<int16_t>(w2);
    }
}

static void NAUDIO_ENVMIXER(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_naudio();
    bool init = (w1 >> 16) & A_INIT;
    bool aux  = (w1 >> 16) & A_AUX;
    uint16_t dmem_dl = static_cast<uint16_t>(w1);
    uint16_t dmem_dr = static_cast<uint16_t>(w2 >> 16);
    uint16_t dmem_wl = static_cast<uint16_t>(w2);
    uint16_t dmem_wr = static_cast<uint16_t>(w2) + 0x200;
    uint32_t addr = s.loop;
    alist_envmix_lin(hle, init,
        dmem_dl, dmem_dr, dmem_wl, dmem_wr,
        0x940, 0x3C0, s.dry, s.wet,
        s.vol, s.target, s.rate, addr);
    (void)aux;
}

static void NAUDIO_LOADBUFF(CHle* hle, uint32_t w1, uint32_t w2)
{
    uint16_t count = static_cast<uint16_t>((w1 >> 12) & 0xFFF);
    uint16_t dmem  = static_cast<uint16_t>(w1);
    uint32_t addr  = w2 & 0x00FFFFFF;
    alist_load(hle, dmem, addr, count);
}

static void NAUDIO_SAVEBUFF(CHle* hle, uint32_t w1, uint32_t w2)
{
    uint16_t count = static_cast<uint16_t>((w1 >> 12) & 0xFFF);
    uint16_t dmem  = static_cast<uint16_t>(w1);
    uint32_t addr  = w2 & 0x00FFFFFF;
    alist_save(hle, dmem, addr, count);
}

static void NAUDIO_MIXER(CHle* hle, uint32_t w1, uint32_t w2)
{
    uint16_t dmemo = static_cast<uint16_t>(w2 >> 16);
    uint16_t dmemi = static_cast<uint16_t>(w2);
    int16_t gain   = static_cast<int16_t>(w1);
    uint16_t count = 0x3C0; // Standard naudio mix length
    alist_mix(hle, dmemo, dmemi, count, gain);
}

static void NAUDIO_INTERLEAVE(CHle* hle, uint32_t w1, uint32_t w2)
{
    uint16_t dmemo = static_cast<uint16_t>(w1);
    uint16_t left  = static_cast<uint16_t>(w2 >> 16);
    uint16_t right = static_cast<uint16_t>(w2);
    uint16_t count = 0x3C0;
    alist_interleave(hle, dmemo, left, right, count);
}

static void NAUDIO_CLEARBUFF(CHle* hle, uint32_t w1, uint32_t w2)
{
    uint16_t dmem  = static_cast<uint16_t>(w1);
    uint16_t count = static_cast<uint16_t>(w2);
    alist_clear(hle, dmem, count);
}

// ── Banjo-Kazooie / DK64 Naudio envmixer variant ──
// Uses the GE-style (exponential) envelope mixer instead of linear
static void NAUDIO_ENVMIXER_GE(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_naudio();
    bool init = (w1 >> 16) & A_INIT;
    bool aux  = (w1 >> 16) & A_AUX;
    uint16_t dmem_dl = static_cast<uint16_t>(w1);
    uint16_t dmem_dr = static_cast<uint16_t>(w2 >> 16);
    uint16_t dmem_wl = static_cast<uint16_t>(w2);
    uint16_t dmem_wr = static_cast<uint16_t>(w2) + 0x200;
    uint32_t addr = s.loop;
    alist_envmix_ge(hle, init, aux,
        dmem_dl, dmem_dr, dmem_wl, dmem_wr,
        0x940, 0x3C0, s.dry, s.wet,
        s.vol, s.target, s.rate, addr);
}

void alist_process_naudio(CHle* hle)
{
    static const AcmdCallback ABI[16] = {
        SPNOOP,             NAUDIO_ADPCM,       NAUDIO_CLEARBUFF,   NAUDIO_ENVMIXER,
        NAUDIO_LOADBUFF,    NAUDIO_RESAMPLE,     NAUDIO_SAVEBUFF,    SPNOOP,
        NAUDIO_SETBUFF,     NAUDIO_SETVOL,       NAUDIO_DMEMMOVE,    NAUDIO_LOADBUFF,
        NAUDIO_MIXER,       NAUDIO_INTERLEAVE,   SPNOOP,             SPNOOP,
    };
    alist_process(hle, ABI, 16);
}

void alist_process_naudio_bk(CHle* hle)
{
    // Banjo-Kazooie uses GE-style exponential envelope mixing
    static const AcmdCallback ABI_BK[16] = {
        SPNOOP,             NAUDIO_ADPCM,       NAUDIO_CLEARBUFF,   NAUDIO_ENVMIXER_GE,
        NAUDIO_LOADBUFF,    NAUDIO_RESAMPLE,     NAUDIO_SAVEBUFF,    SPNOOP,
        NAUDIO_SETBUFF,     NAUDIO_SETVOL,       NAUDIO_DMEMMOVE,    NAUDIO_LOADBUFF,
        NAUDIO_MIXER,       NAUDIO_INTERLEAVE,   SPNOOP,             SPNOOP,
    };
    alist_process(hle, ABI_BK, 16);
}

void alist_process_naudio_dk(CHle* hle)
{
    // DK64 also uses GE-style envelope mixing
    static const AcmdCallback ABI_DK[16] = {
        SPNOOP,             NAUDIO_ADPCM,       NAUDIO_CLEARBUFF,   NAUDIO_ENVMIXER_GE,
        NAUDIO_LOADBUFF,    NAUDIO_RESAMPLE,     NAUDIO_SAVEBUFF,    SPNOOP,
        NAUDIO_SETBUFF,     NAUDIO_SETVOL,       NAUDIO_DMEMMOVE,    NAUDIO_LOADBUFF,
        NAUDIO_MIXER,       NAUDIO_INTERLEAVE,   SPNOOP,             SPNOOP,
    };
    alist_process(hle, ABI_DK, 16);
}

void alist_process_naudio_mp3(CHle* hle)
{
    // Naudio + MP3 hybrid — standard linear envmixer
    alist_process_naudio(hle);
}

void alist_process_naudio_cbfd(CHle* hle)
{
    // Conker's Bad Fur Day — uses GE-style envelope mixing
    static const AcmdCallback ABI_CBFD[16] = {
        SPNOOP,             NAUDIO_ADPCM,       NAUDIO_CLEARBUFF,   NAUDIO_ENVMIXER_GE,
        NAUDIO_LOADBUFF,    NAUDIO_RESAMPLE,     NAUDIO_SAVEBUFF,    SPNOOP,
        NAUDIO_SETBUFF,     NAUDIO_SETVOL,       NAUDIO_DMEMMOVE,    NAUDIO_LOADBUFF,
        NAUDIO_MIXER,       NAUDIO_INTERLEAVE,   SPNOOP,             SPNOOP,
    };
    alist_process(hle, ABI_CBFD, 16);
}

// ══════════════════════════════════════════════
// NEAD processing variants
// ══════════════════════════════════════════════

static void NEAD_SETBUFF(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_nead();
    s.in    = static_cast<uint16_t>(w1);
    s.out   = static_cast<uint16_t>(w2 >> 16);
    s.count = static_cast<uint16_t>(w2);
}

static void NEAD_DMEMMOVE(CHle* hle, uint32_t w1, uint32_t w2)
{
    uint16_t dmemi = static_cast<uint16_t>(w1);
    uint16_t dmemo = static_cast<uint16_t>(w2 >> 16);
    uint16_t count = static_cast<uint16_t>(w2);
    alist_move(hle, dmemo, dmemi, count);
}

static void NEAD_CLEARBUFF(CHle* hle, uint32_t w1, uint32_t w2)
{
    uint16_t dmem  = static_cast<uint16_t>(w1);
    uint16_t count = static_cast<uint16_t>(w2);
    alist_clear(hle, dmem, count);
}

static void NEAD_LOADBUFF(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_nead();
    alist_load(hle, s.in, w2 & 0x00FFFFFF, s.count);
    (void)w1;
}

static void NEAD_SAVEBUFF(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_nead();
    alist_save(hle, s.out, w2 & 0x00FFFFFF, s.count);
    (void)w1;
}

static void NEAD_MIX(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_nead();
    uint16_t dmemo = static_cast<uint16_t>(w2 >> 16);
    uint16_t dmemi = static_cast<uint16_t>(w2);
    int16_t gain = static_cast<int16_t>(w1);
    alist_mix(hle, dmemo, dmemi, s.count, gain);
}

static void NEAD_ENVMIXER(CHle* hle, uint32_t w1, uint32_t w2)
{
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
    xors[2] = 0; // Wet channel polarity (not typically inverted)

    alist_envmix_nead(hle, swap, dmem_dl, dmem_dr, dmem_wl, dmem_wr,
                      s.in, s.count, s.env_values, s.env_steps, xors);
}

static void NEAD_INTERLEAVE(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_nead();
    uint16_t left  = static_cast<uint16_t>(w2 >> 16);
    uint16_t right = static_cast<uint16_t>(w2);
    alist_interleave(hle, s.out, left, right, s.count);
    (void)w1;
}

static void NEAD_ADPCM(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_nead();
    bool init = (w1 >> 16) & 0x01;
    bool loop = (w1 >> 16) & 0x02;
    alist_adpcm(hle, init, loop, false, s.out, s.in, s.count,
                s.table, s.loop, w2 & 0x00FFFFFF);
}

static void NEAD_RESAMPLE(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_nead();
    bool init  = (w1 >> 16) & 0x01;
    bool flag2 = (w1 >> 16) & 0x02;
    uint32_t pitch = w1 & 0xFFFF;
    alist_resample(hle, init, flag2, s.out, s.in, s.count, pitch << 1, w2 & 0x00FFFFFF);
}

static void NEAD_SETLOOP(CHle* hle, uint32_t /*w1*/, uint32_t w2)
{
    hle->alist_nead().loop = w2 & 0x00FFFFFF;
}

static void NEAD_FILTER(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_nead();
    alist_filter(hle, s.in, s.count, w2 & 0x00FFFFFF, s.filter_lut_address);
    (void)w1;
}

static void NEAD_POLEF(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_nead();
    bool init = (w1 >> 16) & 0x01;
    uint16_t gain = static_cast<uint16_t>(w1);
    alist_polef(hle, init, s.out, s.in, s.count, gain, s.table, w2 & 0x00FFFFFF);
}

// MK64 unique handler: HILOGAIN applies gain to the high/low frequency bands
// via the Q4.4 multiplier (same operation as alist_multQ44).
static void NEAD_HILOGAIN(CHle* hle, uint32_t w1, uint32_t w2)
{
    auto& s = hle->alist_nead();
    int8_t gain = static_cast<int8_t>(w1);
    alist_multQ44(hle, s.out, s.count, gain);
    (void)w2;
}

// ── Per-game NEAD ABI tables ──
// Different games' RSP audio ucodes use different command-index-to-handler mappings.
// The index (0-31) is extracted from the high byte of the audio command word,
// and the corresponding callback is invoked.

// Standard / Full NEAD (Zelda OoT, Zelda MM, Animal Crossing, etc.)
// Includes both POLEF and FILTER.
static const AcmdCallback NEAD_ABI_FULL[32] = {
    SPNOOP,           NEAD_ADPCM,       NEAD_CLEARBUFF,   NEAD_ENVMIXER,
    NEAD_LOADBUFF,    NEAD_RESAMPLE,     NEAD_SAVEBUFF,    SPNOOP,
    NEAD_SETBUFF,     SPNOOP,            NEAD_DMEMMOVE,    NEAD_LOADBUFF,
    NEAD_MIX,         NEAD_INTERLEAVE,   NEAD_POLEF,       NEAD_SETLOOP,
    NEAD_FILTER,      SPNOOP,            SPNOOP,           SPNOOP,
    SPNOOP,           SPNOOP,            SPNOOP,           SPNOOP,
    SPNOOP,           SPNOOP,            SPNOOP,           SPNOOP,
    SPNOOP,           SPNOOP,            SPNOOP,           SPNOOP,
};

// Mario Kart 64: index 3 = MIX (not ENVMIXER), index 14 = HILOGAIN (not POLEF)
static const AcmdCallback NEAD_ABI_MK[32] = {
    SPNOOP,           NEAD_ADPCM,       NEAD_CLEARBUFF,   NEAD_MIX,
    NEAD_LOADBUFF,    NEAD_RESAMPLE,     NEAD_SAVEBUFF,    SPNOOP,
    NEAD_SETBUFF,     SPNOOP,            NEAD_DMEMMOVE,    NEAD_LOADBUFF,
    NEAD_MIX,         NEAD_INTERLEAVE,   NEAD_HILOGAIN,    NEAD_SETLOOP,
    SPNOOP,           SPNOOP,            SPNOOP,           SPNOOP,
    SPNOOP,           SPNOOP,            SPNOOP,           SPNOOP,
    SPNOOP,           SPNOOP,            SPNOOP,           SPNOOP,
    SPNOOP,           SPNOOP,            SPNOOP,           SPNOOP,
};

// Star Fox 64 / F-Zero X / Wave Race / 1080: POLEF at 14, no FILTER
static const AcmdCallback NEAD_ABI_SF[32] = {
    SPNOOP,           NEAD_ADPCM,       NEAD_CLEARBUFF,   NEAD_ENVMIXER,
    NEAD_LOADBUFF,    NEAD_RESAMPLE,     NEAD_SAVEBUFF,    SPNOOP,
    NEAD_SETBUFF,     SPNOOP,            NEAD_DMEMMOVE,    NEAD_LOADBUFF,
    NEAD_MIX,         NEAD_INTERLEAVE,   NEAD_POLEF,       NEAD_SETLOOP,
    SPNOOP,           SPNOOP,            SPNOOP,           SPNOOP,
    SPNOOP,           SPNOOP,            SPNOOP,           SPNOOP,
    SPNOOP,           SPNOOP,            SPNOOP,           SPNOOP,
    SPNOOP,           SPNOOP,            SPNOOP,           SPNOOP,
};

// Yoshi's Story: index 9 = DMEMMOVE (duplicate), index 14 = FILTER (not POLEF)
static const AcmdCallback NEAD_ABI_YS[32] = {
    SPNOOP,           NEAD_ADPCM,       NEAD_CLEARBUFF,   NEAD_ENVMIXER,
    NEAD_LOADBUFF,    NEAD_RESAMPLE,     NEAD_SAVEBUFF,    SPNOOP,
    NEAD_SETBUFF,     NEAD_DMEMMOVE,     NEAD_DMEMMOVE,    NEAD_LOADBUFF,
    NEAD_MIX,         NEAD_INTERLEAVE,   NEAD_FILTER,      NEAD_SETLOOP,
    SPNOOP,           SPNOOP,            SPNOOP,           SPNOOP,
    SPNOOP,           SPNOOP,            SPNOOP,           SPNOOP,
    SPNOOP,           SPNOOP,            SPNOOP,           SPNOOP,
    SPNOOP,           SPNOOP,            SPNOOP,           SPNOOP,
};

void alist_process_nead_mk(CHle* hle)
{
    alist_process(hle, NEAD_ABI_MK, 32);
}

void alist_process_nead_sfj(CHle* hle)
{
    alist_process(hle, NEAD_ABI_SF, 32);
}

void alist_process_nead_sf(CHle* hle)
{
    alist_process(hle, NEAD_ABI_SF, 32);
}

void alist_process_nead_fz(CHle* hle)
{
    alist_process(hle, NEAD_ABI_SF, 32);
}

void alist_process_nead_wrjb(CHle* hle)
{
    alist_process(hle, NEAD_ABI_SF, 32);
}

void alist_process_nead_ys(CHle* hle)
{
    alist_process(hle, NEAD_ABI_YS, 32);
}

void alist_process_nead_1080(CHle* hle)
{
    alist_process(hle, NEAD_ABI_SF, 32);
}

void alist_process_nead_oot(CHle* hle)
{
    alist_process(hle, NEAD_ABI_FULL, 32);
}

void alist_process_nead_mm(CHle* hle)
{
    alist_process(hle, NEAD_ABI_FULL, 32);
}

void alist_process_nead_mmb(CHle* hle)
{
    alist_process(hle, NEAD_ABI_FULL, 32);
}

void alist_process_nead_ac(CHle* hle)
{
    alist_process(hle, NEAD_ABI_FULL, 32);
}

// ══════════════════════════════════════════════
// MP3 microcode task
// ══════════════════════════════════════════════

// ──────────────────────────────────────────────
// MP3 quantization scale factors (N64 RSP variant)
// The N64's MP3 ucode uses a custom 14-bit quantization
// table derived from MPEG-1 Layer III, stored in DMEM.
// ──────────────────────────────────────────────

static constexpr int32_t MP3_QUANT_LUT[16] = {
    0, 1, 3, 5, 7, 9, 15, 23,
    31, 47, 63, 95, 127, 191, 255, 383
};

// Windowing coefficients for 32-sample IMDCT (subset)
static constexpr int16_t MP3_WINDOW[32] = {
    0, 213, 426, 640, 851, 1060, 1265, 1466,
    1662, 1853, 2037, 2213, 2381, 2540, 2690, 2830,
    2959, 3078, 3186, 3283, 3368, 3441, 3502, 3551,
    3588, 3612, 3623, 3623, 3612, 3588, 3551, 3502
};

static void mp3_dequantize(int16_t* samples, const uint8_t* data, unsigned count)
{
    // Dequantize MP3 subband samples using the N64's simplified scheme
    for (unsigned i = 0; i < count && i < 576; i++)
    {
        uint8_t nibble = (i & 1) ? (data[i / 2] & 0x0F) : (data[i / 2] >> 4);
        int32_t level = MP3_QUANT_LUT[nibble & 0x0F];
        // Apply sign from MS bit
        if (nibble & 0x08)
            level = -level;
        samples[i] = clamp_s16(level << 4);
    }
}

static void mp3_imdct_window(int16_t* output, const int16_t* freq_samples, unsigned granule_size)
{
    // Simplified 32-point IMDCT + windowing
    // Real N64 ucode does a butterfly-style transform; we approximate
    for (unsigned i = 0; i < granule_size && i < 32; i++)
    {
        int32_t acc = 0;
        for (unsigned k = 0; k < 32 && k < granule_size; k++)
        {
            // cos((2*i+1)*(2*k+1)*pi/64) approximated as window coeff
            unsigned idx = ((2 * i + 1) * (2 * k + 1)) % 64;
            int16_t coeff = (idx < 32) ? MP3_WINDOW[idx] : -MP3_WINDOW[63 - idx];
            acc += static_cast<int32_t>(freq_samples[k]) * coeff;
        }
        output[i] = clamp_s16(acc >> 14);
    }
}

void mp3_task(CHle* hle, unsigned int index, uint32_t address)
{
    uint8_t* dram = hle->dram();
    uint8_t* dmem = hle->dmem();
    if (dram == nullptr || dmem == nullptr)
    {
        hle->WarnMessage("MP3 task: null memory");
        hle->rsp_break(0);
        return;
    }

    hle->VerboseMessage("MP3 task: index=%u address=0x%08X", index, address);

    // Load compressed MP3 frame data from DRAM into work buffer
    constexpr uint32_t MAX_FRAME_SIZE = 0x400; // 1KB max frame
    uint8_t frame_data[MAX_FRAME_SIZE]{};
    uint32_t frame_size = std::min<uint32_t>(*dmem_u32(hle, TaskOffset::DataSize), MAX_FRAME_SIZE);

    if (frame_size > 0 && address != 0)
    {
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
    for (unsigned granule = 0; granule < 2; granule++)
    {
        for (unsigned sb = 0; sb < num_subbands && sb < 32; sb++)
        {
            unsigned freq_offset = granule * 18 * 32 + sb * 18;
            unsigned pcm_offset  = granule * 576 + sb * 18;
            if (freq_offset + 18 > 576) break;
            if (pcm_offset + 32 > 1152) break;

            mp3_imdct_window(pcm_output + pcm_offset, freq_samples + freq_offset,
                             std::min<unsigned>(18, num_subbands));
        }
    }

    // Store decoded PCM to the output buffer in DRAM
    uint32_t out_addr = *dmem_u32(hle, TaskOffset::OutputBuff);
    if (out_addr != 0)
    {
        uint16_t out_size = static_cast<uint16_t>(std::min<uint32_t>(1152 * 2, 0x1000));
        dram_store_u16(hle, reinterpret_cast<const uint16_t*>(pcm_output), out_addr, out_size / 2);
    }

    hle->rsp_break(0);
}

// ══════════════════════════════════════════════
// Musyx microcodes
// ══════════════════════════════════════════════

// ──────────────────────────────────────────────
// Musyx voice data structure
// Factor5's Musyx audio engine uses a per-voice
// parameter block stored in RDRAM.
// ──────────────────────────────────────────────

struct MusyxVoice {
    uint32_t sample_addr;     // RDRAM address of sample data
    uint32_t sample_length;   // Length in bytes
    uint32_t loop_start;      // Loop start offset
    uint16_t pitch;           // Pitch (fixed 16.16 accumulator rate)
    int16_t  volume_left;     // Volume L (Q15)
    int16_t  volume_right;    // Volume R (Q15)
    int16_t  envelope;        // Current envelope level
    int16_t  env_target;      // Target envelope
    int16_t  env_rate;        // Computed ramp rate (not stored in DRAM struct)
    uint8_t  flags;           // Bit 0=active, bit 1=loop, bit 2=stereo
    uint8_t  reserved;        // Padding to 24 bytes
};

static constexpr unsigned MUSYX_MAX_VOICES = 24;
static constexpr unsigned MUSYX_VOICE_SIZE = 24; // bytes per voice in DRAM
static constexpr unsigned MUSYX_OUTPUT_SAMPLES = 160; // samples per call

static void musyx_process_voices(CHle* hle, uint32_t voice_table_addr, uint8_t* work_buf,
                                  int16_t* mix_left, int16_t* mix_right, unsigned num_samples)
{
    uint8_t* dram = hle->dram();
    if (dram == nullptr) return;

    // Clear mix buffers
    std::memset(mix_left, 0, num_samples * sizeof(int16_t));
    std::memset(mix_right, 0, num_samples * sizeof(int16_t));

    for (unsigned v = 0; v < MUSYX_MAX_VOICES; v++)
    {
        uint32_t vaddr = voice_table_addr + v * MUSYX_VOICE_SIZE;
        if ((vaddr & 0x7FFFFF) + MUSYX_VOICE_SIZE > 0x800000) break;

        // Read voice parameters from DRAM
        MusyxVoice voice{};
        const uint8_t* vdata = dram + (vaddr & 0x7FFFFF);
        voice.sample_addr   = (vdata[0] << 24) | (vdata[1] << 16) | (vdata[2] << 8) | vdata[3];
        voice.sample_length  = (vdata[4] << 24) | (vdata[5] << 16) | (vdata[6] << 8) | vdata[7];
        voice.loop_start     = (vdata[8] << 24) | (vdata[9] << 16) | (vdata[10] << 8) | vdata[11];
        voice.pitch          = static_cast<uint16_t>((vdata[12] << 8) | vdata[13]);
        voice.volume_left    = static_cast<int16_t>((vdata[14] << 8) | vdata[15]);
        voice.volume_right   = static_cast<int16_t>((vdata[16] << 8) | vdata[17]);
        voice.envelope       = static_cast<int16_t>((vdata[18] << 8) | vdata[19]);
        voice.env_target     = static_cast<int16_t>((vdata[20] << 8) | vdata[21]);
        voice.env_rate       = 0; // Not stored in 24-byte struct; ramp is computed from envelope/target
        voice.flags          = vdata[22];
        voice.reserved       = vdata[23];

        if (!(voice.flags & 0x01)) continue; // Voice not active
        if (voice.sample_addr == 0 || voice.sample_length == 0) continue;

        // Simple pitch-driven sample playback
        uint32_t pitch_accum = 0;
        int32_t env = voice.envelope;

        for (unsigned i = 0; i < num_samples; i++)
        {
            uint32_t sample_offset = pitch_accum >> 16;
            if (sample_offset >= voice.sample_length)
            {
                if (voice.flags & 0x02) // Loop
                    sample_offset = voice.loop_start + (sample_offset - voice.sample_length) % 
                        (voice.sample_length - voice.loop_start > 0 ? voice.sample_length - voice.loop_start : 1);
                else
                    break; // Voice done
            }

            // Read sample from DRAM (8-bit signed, expanded to 16-bit)
            uint32_t saddr = (voice.sample_addr + sample_offset) & 0x7FFFFF;
            int16_t sample = static_cast<int16_t>(static_cast<int8_t>(dram[saddr])) << 8;

            // Apply envelope
            int32_t enveloped = (static_cast<int32_t>(sample) * env) >> 15;

            // Pan and mix
            mix_left[i]  = clamp_s16(mix_left[i] + 
                static_cast<int16_t>((enveloped * voice.volume_left) >> 15));
            mix_right[i] = clamp_s16(mix_right[i] + 
                static_cast<int16_t>((enveloped * voice.volume_right) >> 15));

            pitch_accum += voice.pitch;

            // Envelope ramp toward target (linear over this processing block)
            if (env != voice.env_target)
            {
                int32_t diff = voice.env_target - env;
                int32_t step = diff / static_cast<int32_t>(num_samples > 1 ? num_samples : 1);
                if (step == 0)
                    step = (diff > 0) ? 1 : -1;
                env += step;
            }
        }
    }
}

void musyx_v1_task(CHle* hle)
{
    uint8_t* dmem = hle->dmem();
    if (dmem == nullptr) return;

    uint32_t addr = *dmem_u32(hle, TaskOffset::DataPtr);
    uint32_t size = *dmem_u32(hle, TaskOffset::DataSize);

    if (size == 0 || addr == 0)
    {
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
    if (output_addr != 0)
    {
        int16_t interleaved[MUSYX_OUTPUT_SAMPLES * 2];
        for (unsigned i = 0; i < MUSYX_OUTPUT_SAMPLES; i++)
        {
            interleaved[i * 2]     = mix_left[i];
            interleaved[i * 2 + 1] = mix_right[i];
        }
        dram_store_u16(hle, reinterpret_cast<const uint16_t*>(interleaved),
                       output_addr, MUSYX_OUTPUT_SAMPLES * 2);
    }

    hle->rsp_break(0);
}

void musyx_v2_task(CHle* hle)
{
    uint8_t* dmem = hle->dmem();
    if (dmem == nullptr) return;

    uint32_t addr = *dmem_u32(hle, TaskOffset::DataPtr);
    uint32_t size = *dmem_u32(hle, TaskOffset::DataSize);

    if (size == 0 || addr == 0)
    {
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
    uint16_t reverb_len  = static_cast<uint16_t>((work[10] << 8) | work[11]);

    if (num_samples == 0 || num_samples > MUSYX_OUTPUT_SAMPLES)
        num_samples = MUSYX_OUTPUT_SAMPLES;

    // Process voices
    int16_t mix_left[MUSYX_OUTPUT_SAMPLES]{};
    int16_t mix_right[MUSYX_OUTPUT_SAMPLES]{};
    musyx_process_voices(hle, voice_table, work + 16, mix_left, mix_right, num_samples);

    // v2 adds a simple reverb pass: mix delayed output back
    if (reverb_len > 0 && reverb_len < num_samples)
    {
        for (unsigned i = reverb_len; i < num_samples; i++)
        {
            mix_left[i]  = clamp_s16(mix_left[i] + (mix_left[i - reverb_len] >> 2));
            mix_right[i] = clamp_s16(mix_right[i] + (mix_right[i - reverb_len] >> 2));
        }
    }

    // Interleave and store
    if (output_addr != 0)
    {
        int16_t interleaved[MUSYX_OUTPUT_SAMPLES * 2];
        for (unsigned i = 0; i < num_samples; i++)
        {
            interleaved[i * 2]     = mix_left[i];
            interleaved[i * 2 + 1] = mix_right[i];
        }
        dram_store_u16(hle, reinterpret_cast<const uint16_t*>(interleaved),
                       output_addr, num_samples * 2);
    }

    hle->rsp_break(0);
}

// ══════════════════════════════════════════════
// JPEG decode microcodes
// ══════════════════════════════════════════════

// Zig-zag scan order for 8x8 DCT block
static constexpr uint8_t ZIGZAG[64] = {
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63,
};

// Simple IDCT (reference implementation for HLE)
static void idct_column(int16_t* blk, int col)
{
    // Scaled integer IDCT on one column
    int32_t tmp[8];
    for (int i = 0; i < 8; i++)
        tmp[i] = blk[i * 8 + col];

    // Simplified butterfly — accurate enough for N64
    for (int i = 0; i < 8; i++)
    {
        int32_t sum = 0;
        for (int j = 0; j < 8; j++)
        {
            // cos((2i+1)*j*pi/16) approximation via integer arithmetic
            static constexpr int32_t COS_TABLE[8][8] = {
                { 64,  64,  64,  64,  64,  64,  64,  64},
                { 89,  75,  50,  18, -18, -50, -75, -89},
                { 83,  36, -36, -83, -83, -36,  36,  83},
                { 75, -18, -89, -50,  50,  89,  18, -75},
                { 64, -64, -64,  64,  64, -64, -64,  64},
                { 50, -89,  18,  75, -75, -18,  89, -50},
                { 36, -83,  83, -36, -36,  83, -83,  36},
                { 18, -50,  75, -89,  89, -75,  50, -18},
            };
            sum += tmp[j] * COS_TABLE[j][i];
        }
        blk[i * 8 + col] = clamp_s16((sum + 32) >> 6);
    }
}

static void idct_row(int16_t* blk, int row)
{
    int32_t tmp[8];
    for (int i = 0; i < 8; i++)
        tmp[i] = blk[row * 8 + i];

    static constexpr int32_t COS_TABLE[8][8] = {
        { 64,  64,  64,  64,  64,  64,  64,  64},
        { 89,  75,  50,  18, -18, -50, -75, -89},
        { 83,  36, -36, -83, -83, -36,  36,  83},
        { 75, -18, -89, -50,  50,  89,  18, -75},
        { 64, -64, -64,  64,  64, -64, -64,  64},
        { 50, -89,  18,  75, -75, -18,  89, -50},
        { 36, -83,  83, -36, -36,  83, -83,  36},
        { 18, -50,  75, -89,  89, -75,  50, -18},
    };

    for (int i = 0; i < 8; i++)
    {
        int32_t sum = 0;
        for (int j = 0; j < 8; j++)
            sum += tmp[j] * COS_TABLE[j][i];
        blk[row * 8 + i] = clamp_s16((sum + 32) >> 6);
    }
}

static void jpeg_idct_block(int16_t block[64])
{
    for (int i = 0; i < 8; i++) idct_column(block, i);
    for (int i = 0; i < 8; i++) idct_row(block, i);
}

static void jpeg_dequantize(int16_t block[64], const int16_t qtable[64])
{
    for (int i = 0; i < 64; i++)
    {
        block[ZIGZAG[i]] = static_cast<int16_t>(block[ZIGZAG[i]] * qtable[i]);
    }
}

void jpeg_decode_PS0(CHle* hle)
{
    // Photo Studio 64 (Japan) JPEG decode
    hle->VerboseMessage("JPEG decode PS0");

    uint8_t* dmem = hle->dmem();
    uint8_t* dram = hle->dram();
    if (dmem == nullptr || dram == nullptr) { hle->rsp_break(0); return; }

    // Read task parameters from DMEM
    uint32_t data_addr = *dmem_u32(hle, TaskOffset::DataPtr);
    uint32_t data_size = *dmem_u32(hle, TaskOffset::DataSize);

    // Load JPEG data into temp buffer
    if (data_size > 0 && data_addr != 0)
    {
        uint16_t load_sz = static_cast<uint16_t>(std::min<uint32_t>(data_size, 0x800));
        dram_load_u8(hle, hle->alist_buffer(), data_addr, load_sz);

        // Decode blocks
        int16_t block[64]{};
        int16_t qtable[64];
        std::memset(qtable, 1, sizeof(qtable)); // Default quant = 1

        for (uint32_t off = 0; off + 128 <= data_size && off + 128 <= 0x800; off += 128)
        {
            for (int i = 0; i < 64; i++)
                block[i] = static_cast<int16_t>(
                    static_cast<int8_t>(hle->alist_buffer()[off + i * 2]) << 8 |
                    hle->alist_buffer()[off + i * 2 + 1]);

            jpeg_dequantize(block, qtable);
            jpeg_idct_block(block);

            // Write back to DRAM
            for (int i = 0; i < 64; i++)
            {
                uint8_t pixel = static_cast<uint8_t>(std::clamp<int16_t>(block[i] + 128, 0, 255));
                *(dram + ((data_addr + off / 2 + i) & 0x7FFFFF)) = pixel;
            }
        }
    }

    hle->rsp_break(0);
}

void jpeg_decode_PS(CHle* hle)
{
    // Photo Studio (international) — same algorithm, different data layout
    hle->VerboseMessage("JPEG decode PS");
    jpeg_decode_PS0(hle);
}

void jpeg_decode_OB(CHle* hle)
{
    // Ogre Battle 64 JPEG variant
    hle->VerboseMessage("JPEG decode OB");
    jpeg_decode_PS0(hle);
}

} // namespace Nova64::RSP::HLE
