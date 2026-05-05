/**
 * @file alist.cpp
 * @brief RSP HLE Audio List Processing Implementation.
 */
// SuperNova64 - N64 Emulator
// RSP HLE Audio List Processing Implementation
// ADPCM, resampling, envelope mixing, interleaving

#include "alist.hpp"
#include "arithmetics.hpp"
#include "audio.hpp"
#include "hle.hpp"
#include "mem.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace Nova64::RSP::HLE {

// ──────────────────────────────────────────────
// Endian-aware buffer access helpers (mupen64plus compatible)
// alist_buffer now stores raw DRAM byte order (memcpy'd).
// S=1 on LE: pair-swap 16-bit indices within 32-bit words.
// S8=3 on LE: byte-swap within 32-bit words.
// S16=2 on LE: pair-swap byte addresses within 32-bit words.
// ──────────────────────────────────────────────
#ifdef __BIG_ENDIAN__
static constexpr unsigned S   = 0;
static constexpr unsigned S8  = 0;
static constexpr unsigned S16 = 0;
#else
static constexpr unsigned S   = 1;   // 16-bit element swap within 32-bit word
static constexpr unsigned S8  = 3;   // byte swap within 32-bit word
static constexpr unsigned S16 = 2;   // byte-addr swap for 16-bit within 32-bit
#endif

// Access a byte in alist_buffer by DMEM byte address (matching mupen64plus alist_u8)
static inline uint8_t* alist_u8(CHle* hle, uint16_t dmem) {
    return reinterpret_cast<uint8_t*>(hle->alist_buffer() + ((dmem ^ S8) & 0xFFF));
}

// Access a 16-bit sample in alist_buffer by DMEM byte address (matching mupen64plus alist_s16)
static inline int16_t* alist_s16(CHle* hle, uint16_t dmem) {
    return reinterpret_cast<int16_t*>(hle->alist_buffer() + ((dmem ^ S16) & 0xFFF));
}

// Access a 16-bit sample in alist_buffer by element index (matching mupen64plus sample())
static inline int16_t* sample(CHle* hle, unsigned pos) {
    return reinterpret_cast<int16_t*>(hle->alist_buffer()) + ((pos ^ S) & 0xFFF);
}

// ──────────────────────────────────────────────
// Audio command list dispatcher
// ──────────────────────────────────────────────

void alist_process(CHle* hle, const AcmdCallback abi[], unsigned int abi_size)
{
    if (hle->dram() == nullptr) return;

    uint32_t dataPtr  = *dmem_u32(hle, static_cast<uint16_t>(TaskOffset::DataPtr));
    uint32_t dataSize = *dmem_u32(hle, static_cast<uint16_t>(TaskOffset::DataSize));

    if (dataPtr == 0 || dataSize == 0) return;

    const uint32_t base = dataPtr & 0x7FFFFF;
    const uint32_t maxBytes = dataSize & 0xFFFF;
    uint32_t offset = 0;

    while (offset + 8 <= maxBytes)
    {
        uint32_t w1 = *dram_u32(hle, base + offset);
        uint32_t w2 = *dram_u32(hle, base + offset + 4);
        offset += 8;

        const uint32_t cmd = (w1 >> 24) & 0x7F;
        if (cmd >= abi_size) continue;
        if (abi[cmd] == nullptr) continue;

        abi[cmd](hle, w1, w2);
    }
}

// ──────────────────────────────────────────────
// Segment address resolution
// ──────────────────────────────────────────────

uint32_t alist_get_address(CHle* hle, uint32_t so, const uint32_t* segments, size_t n)
{
    // Upper 4 bits = segment index, lower 28 bits = offset
    const uint32_t seg = (so >> 24) & 0x0F;
    const uint32_t off = so & 0x00FFFFFF;

    if (seg < n)
        return segments[seg] + off;
    return off;
}

void alist_set_address(CHle* /*hle*/, uint32_t so, uint32_t* segments, size_t n)
{
    const uint32_t seg = (so >> 24) & 0x0F;
    const uint32_t addr = so & 0x00FFFFFF;

    if (seg < n)
        segments[seg] = addr;
}

// ──────────────────────────────────────────────
// Basic DMEM operations
// ──────────────────────────────────────────────

void alist_clear(CHle* hle, uint16_t dmem, uint16_t count)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;
    // Clear with byte-swap access matching mupen64plus
    while (count != 0) {
        *alist_u8(hle, dmem++) = 0;
        --count;
    }
}

void alist_load(CHle* hle, uint16_t dmem, uint32_t address, uint16_t count)
{
    // Enforce DMA alignment constraints (matching N64 SP DMA)
    dmem    &= ~3;
    address &= ~7u;
    count    = static_cast<uint16_t>(align(count, 8));
    // Raw memcpy from DRAM to alist_buffer (mupen64plus style).
    // Buffer now holds DRAM byte order; DSP functions use ^S/^S8/^S16 for access.
    std::memcpy(hle->alist_buffer() + dmem, hle->dram() + (address & 0x7FFFFF), count);
}

void alist_save(CHle* hle, uint16_t dmem, uint32_t address, uint16_t count)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr || hle->dram() == nullptr) return;
    // Enforce DMA alignment constraints
    dmem    &= ~3;
    address &= ~7u;
    count    = static_cast<uint16_t>(align(count, 8));
    // Raw memcpy from alist_buffer to DRAM (mupen64plus style).
    std::memcpy(hle->dram() + (address & 0x7FFFFF), buf + dmem, count);
}

void alist_move(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint16_t count)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;
    // Forward byte-by-byte copy with ^S8, matching mupen64plus alist_move
    while (count != 0) {
        *alist_u8(hle, dmemo++) = *alist_u8(hle, dmemi++);
        --count;
    }
}

void alist_copy_every_other_sample(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint16_t count)
{
    // Matching mupen64plus: copy every other 16-bit sample using alist_s16
    while (count != 0) {
        *alist_s16(hle, dmemo) = *alist_s16(hle, dmemi);
        dmemo += 2;
        dmemi += 4;
        --count;
    }
}

void alist_repeat64(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint8_t count)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    uint8_t tmp[128];
    std::memcpy(tmp, buf + (dmemi & 0xFFF), 128);
    uint8_t* dst = buf + (dmemo & 0xFFF);

    for (uint8_t i = 0; i < count; i++)
    {
        std::memcpy(dst, tmp, 128);
        dst += 128;
    }
}

void alist_copy_blocks(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint16_t block_size, uint8_t count)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    for (uint8_t i = 0; i < count; i++)
    {
        uint16_t doff = (dmemo + i * block_size) & 0xFFF;
        uint16_t soff = (dmemi + i * block_size) & 0xFFF;
        uint16_t safe = std::min<uint16_t>(block_size, std::min<uint16_t>(0x1000 - doff, 0x1000 - soff));
        std::memcpy(buf + doff, buf + soff, safe);
    }
}

void alist_interleave(CHle* hle, uint16_t dmemo, uint16_t left, uint16_t right, uint16_t count)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    uint16_t* dst  = reinterpret_cast<uint16_t*>(buf + dmemo);
    const uint16_t* srcL = reinterpret_cast<const uint16_t*>(buf + left);
    const uint16_t* srcR = reinterpret_cast<const uint16_t*>(buf + right);

    count >>= 2;

    while (count != 0) {
        uint16_t l1 = *(srcL++);
        uint16_t l2 = *(srcL++);
        uint16_t r1 = *(srcR++);
        uint16_t r2 = *(srcR++);

#ifdef __BIG_ENDIAN__
        *(dst++) = l1;
        *(dst++) = r1;
        *(dst++) = l2;
        *(dst++) = r2;
#else
        *(dst++) = r2;
        *(dst++) = l2;
        *(dst++) = r1;
        *(dst++) = l1;
#endif
        --count;
    }
}

// ──────────────────────────────────────────────
// Ramp helpers (matching mupen64plus alist.c)
// ──────────────────────────────────────────────
namespace {
struct ramp_t {
    int64_t value;
    int64_t step;
    int64_t target;
};

inline int16_t ramp_step(ramp_t* ramp)
{
    ramp->value += ramp->step;
    bool target_reached = (ramp->step <= 0)
        ? (ramp->value <= ramp->target)
        : (ramp->value >= ramp->target);
    if (target_reached) {
        ramp->value = ramp->target;
        ramp->step  = 0;
    }
    return static_cast<int16_t>(ramp->value >> 16);
}

inline void sample_mix(int16_t* dst, int16_t src, int16_t gain)
{
    *dst = clamp_s16(*dst + ((static_cast<int32_t>(src) * gain) >> 15));
}

inline void alist_envmix_mix_n(size_t n, int16_t** dst, const int16_t* gains, int16_t src)
{
    for (size_t i = 0; i < n; ++i)
        sample_mix(dst[i], src, gains[i]);
}
} // anonymous namespace

// ──────────────────────────────────────────────
// Envelope mixing (exponential) — port of mupen64plus alist_envmix_exp
// ──────────────────────────────────────────────

void alist_envmix_exp(CHle* hle, bool init, bool aux,
    uint16_t dmem_dl, uint16_t dmem_dr, uint16_t dmem_wl, uint16_t dmem_wr,
    uint16_t dmemi, uint16_t count, int16_t dry, int16_t wet,
    const int16_t* vol, const int16_t* target, const int32_t* rate, uint32_t address)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    const size_t n = aux ? 4 : 2;

    int16_t* const in = reinterpret_cast<int16_t*>(buf + dmemi);
    int16_t* const dl = reinterpret_cast<int16_t*>(buf + dmem_dl);
    int16_t* const dr = reinterpret_cast<int16_t*>(buf + dmem_dr);
    int16_t* const wl = reinterpret_cast<int16_t*>(buf + dmem_wl);
    int16_t* const wr = reinterpret_cast<int16_t*>(buf + dmem_wr);

    ramp_t ramps[2];
    int32_t exp_seq[2];
    int32_t exp_rates[2];

    int16_t save_buffer[40];
    dram_load_u16(hle, reinterpret_cast<uint16_t*>(save_buffer), address, 40);

    if (init) {
        ramps[0].value  = (int64_t)vol[0] << 16;
        ramps[1].value  = (int64_t)vol[1] << 16;
        ramps[0].target = (int64_t)target[0] << 16;
        ramps[1].target = (int64_t)target[1] << 16;
        exp_rates[0]    = rate[0];
        exp_rates[1]    = rate[1];
        exp_seq[0]      = static_cast<int32_t>(vol[0] * rate[0]);
        exp_seq[1]      = static_cast<int32_t>(vol[1] * rate[1]);
    } else {
        wet             = *(int16_t*)(save_buffer +  0);
        dry             = *(int16_t*)(save_buffer +  2);
        ramps[0].target = *(int32_t*)(save_buffer +  4);
        ramps[1].target = *(int32_t*)(save_buffer +  6);
        exp_rates[0]    = *(int32_t*)(save_buffer +  8);
        exp_rates[1]    = *(int32_t*)(save_buffer + 10);
        exp_seq[0]      = *(int32_t*)(save_buffer + 12);
        exp_seq[1]      = *(int32_t*)(save_buffer + 14);
        ramps[0].value  = *(int32_t*)(save_buffer + 16);
        ramps[1].value  = *(int32_t*)(save_buffer + 18);
    }

    ramps[0].step = ramps[0].target - ramps[0].value;
    ramps[1].step = ramps[1].target - ramps[1].value;

    uint32_t ptr = 0;
    for (int y = 0; y < count; y += 16) {
        if (ramps[0].step != 0) {
            exp_seq[0] = static_cast<int32_t>(((int64_t)exp_seq[0] * (int64_t)exp_rates[0]) >> 16);
            ramps[0].step = (exp_seq[0] - ramps[0].value) >> 3;
        }
        if (ramps[1].step != 0) {
            exp_seq[1] = static_cast<int32_t>(((int64_t)exp_seq[1] * (int64_t)exp_rates[1]) >> 16);
            ramps[1].step = (exp_seq[1] - ramps[1].value) >> 3;
        }

        for (int x = 0; x < 8; ++x) {
            int16_t  gains[4];
            int16_t* buffers[4];
            int16_t l_vol = ramp_step(&ramps[0]);
            int16_t r_vol = ramp_step(&ramps[1]);

            buffers[0] = dl + (ptr ^ S);
            buffers[1] = dr + (ptr ^ S);
            buffers[2] = wl + (ptr ^ S);
            buffers[3] = wr + (ptr ^ S);

            gains[0] = clamp_s16((l_vol * dry + 0x4000) >> 15);
            gains[1] = clamp_s16((r_vol * dry + 0x4000) >> 15);
            gains[2] = clamp_s16((l_vol * wet + 0x4000) >> 15);
            gains[3] = clamp_s16((r_vol * wet + 0x4000) >> 15);

            alist_envmix_mix_n(n, buffers, gains, in[ptr ^ S]);
            ++ptr;
        }
    }

    *(int16_t*)(save_buffer +  0) = wet;
    *(int16_t*)(save_buffer +  2) = dry;
    *(int32_t*)(save_buffer +  4) = static_cast<int32_t>(ramps[0].target);
    *(int32_t*)(save_buffer +  6) = static_cast<int32_t>(ramps[1].target);
    *(int32_t*)(save_buffer +  8) = exp_rates[0];
    *(int32_t*)(save_buffer + 10) = exp_rates[1];
    *(int32_t*)(save_buffer + 12) = exp_seq[0];
    *(int32_t*)(save_buffer + 14) = exp_seq[1];
    *(int32_t*)(save_buffer + 16) = static_cast<int32_t>(ramps[0].value);
    *(int32_t*)(save_buffer + 18) = static_cast<int32_t>(ramps[1].value);
    dram_store_u16(hle, reinterpret_cast<const uint16_t*>(save_buffer), address, 40);
}

// ──────────────────────────────────────────────
// Envelope mixing (GoldenEye GE) — port of mupen64plus alist_envmix_ge
// ──────────────────────────────────────────────

void alist_envmix_ge(CHle* hle, bool init, bool aux,
    uint16_t dmem_dl, uint16_t dmem_dr, uint16_t dmem_wl, uint16_t dmem_wr,
    uint16_t dmemi, uint16_t count, int16_t dry, int16_t wet,
    const int16_t* vol, const int16_t* target, const int32_t* rate, uint32_t address)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    const size_t n = aux ? 4 : 2;

    int16_t* const in = reinterpret_cast<int16_t*>(buf + dmemi);
    int16_t* const dl = reinterpret_cast<int16_t*>(buf + dmem_dl);
    int16_t* const dr = reinterpret_cast<int16_t*>(buf + dmem_dr);
    int16_t* const wl = reinterpret_cast<int16_t*>(buf + dmem_wl);
    int16_t* const wr = reinterpret_cast<int16_t*>(buf + dmem_wr);

    ramp_t ramps[2];
    int16_t save_buffer[40];
    dram_load_u16(hle, reinterpret_cast<uint16_t*>(save_buffer), address, 40);

    if (init) {
        ramps[0].value  = (int64_t)vol[0] << 16;
        ramps[1].value  = (int64_t)vol[1] << 16;
        ramps[0].target = (int64_t)target[0] << 16;
        ramps[1].target = (int64_t)target[1] << 16;
        ramps[0].step   = rate[0] / 8;
        ramps[1].step   = rate[1] / 8;
    } else {
        wet             = *(int16_t*)(save_buffer +  0);
        dry             = *(int16_t*)(save_buffer +  2);
        ramps[0].target = *(int32_t*)(save_buffer +  4);
        ramps[1].target = *(int32_t*)(save_buffer +  6);
        ramps[0].step   = *(int32_t*)(save_buffer +  8);
        ramps[1].step   = *(int32_t*)(save_buffer + 10);
        ramps[0].value  = *(int32_t*)(save_buffer + 16);
        ramps[1].value  = *(int32_t*)(save_buffer + 18);
    }

    const unsigned samples = count >> 1;
    for (unsigned k = 0; k < samples; ++k) {
        int16_t  gains[4];
        int16_t* buffers[4];
        int16_t l_vol = ramp_step(&ramps[0]);
        int16_t r_vol = ramp_step(&ramps[1]);

        buffers[0] = dl + (k ^ S);
        buffers[1] = dr + (k ^ S);
        buffers[2] = wl + (k ^ S);
        buffers[3] = wr + (k ^ S);

        gains[0] = clamp_s16((l_vol * dry + 0x4000) >> 15);
        gains[1] = clamp_s16((r_vol * dry + 0x4000) >> 15);
        gains[2] = clamp_s16((l_vol * wet + 0x4000) >> 15);
        gains[3] = clamp_s16((r_vol * wet + 0x4000) >> 15);

        alist_envmix_mix_n(n, buffers, gains, in[k ^ S]);
    }

    *(int16_t*)(save_buffer +  0) = wet;
    *(int16_t*)(save_buffer +  2) = dry;
    *(int32_t*)(save_buffer +  4) = static_cast<int32_t>(ramps[0].target);
    *(int32_t*)(save_buffer +  6) = static_cast<int32_t>(ramps[1].target);
    *(int32_t*)(save_buffer +  8) = static_cast<int32_t>(ramps[0].step);
    *(int32_t*)(save_buffer + 10) = static_cast<int32_t>(ramps[1].step);
    *(int32_t*)(save_buffer + 16) = static_cast<int32_t>(ramps[0].value);
    *(int32_t*)(save_buffer + 18) = static_cast<int32_t>(ramps[1].value);
    dram_store_u16(hle, reinterpret_cast<const uint16_t*>(save_buffer), address, 40);
}

// ──────────────────────────────────────────────
// Envelope mixing (linear ramp) — port of mupen64plus alist_envmix_lin
// ──────────────────────────────────────────────

void alist_envmix_lin(CHle* hle, bool init,
    uint16_t dmem_dl, uint16_t dmem_dr, uint16_t dmem_wl, uint16_t dmem_wr,
    uint16_t dmemi, uint16_t count, int16_t dry, int16_t wet,
    const int16_t* vol, const int16_t* target, const int32_t* rate, uint32_t address)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    int16_t* const in = reinterpret_cast<int16_t*>(buf + dmemi);
    int16_t* const dl = reinterpret_cast<int16_t*>(buf + dmem_dl);
    int16_t* const dr = reinterpret_cast<int16_t*>(buf + dmem_dr);
    int16_t* const wl = reinterpret_cast<int16_t*>(buf + dmem_wl);
    int16_t* const wr = reinterpret_cast<int16_t*>(buf + dmem_wr);

    ramp_t ramps[2]{};
    int16_t save_buffer[40];
    dram_load_u16(hle, reinterpret_cast<uint16_t*>(save_buffer), address, 40);

    if (init) {
        ramps[0].step   = rate[0] / 8;
        ramps[0].value  = (int64_t)vol[0] << 16;
        ramps[0].target = (int64_t)target[0] << 16;
        ramps[1].step   = rate[1] / 8;
        ramps[1].value  = (int64_t)vol[1] << 16;
        ramps[1].target = (int64_t)target[1] << 16;
    } else {
        wet             = *(int16_t*)(save_buffer +  0);
        dry             = *(int16_t*)(save_buffer +  2);
        ramps[0].target = (int64_t)(*(int16_t*)(save_buffer +  4)) << 16;
        ramps[1].target = (int64_t)(*(int16_t*)(save_buffer +  6)) << 16;
        ramps[0].step   = *(int32_t*)(save_buffer +  8);
        ramps[1].step   = *(int32_t*)(save_buffer + 10);
        ramps[0].value  = *(int32_t*)(save_buffer + 16);
        ramps[1].value  = *(int32_t*)(save_buffer + 18);
    }

    const unsigned samples = count >> 1;
    for (unsigned k = 0; k < samples; ++k) {
        int16_t  gains[4];
        int16_t* buffers[4];
        int16_t l_vol = ramp_step(&ramps[0]);
        int16_t r_vol = ramp_step(&ramps[1]);

        buffers[0] = dl + (k ^ S);
        buffers[1] = dr + (k ^ S);
        buffers[2] = wl + (k ^ S);
        buffers[3] = wr + (k ^ S);

        gains[0] = clamp_s16((l_vol * dry + 0x4000) >> 15);
        gains[1] = clamp_s16((r_vol * dry + 0x4000) >> 15);
        gains[2] = clamp_s16((l_vol * wet + 0x4000) >> 15);
        gains[3] = clamp_s16((r_vol * wet + 0x4000) >> 15);

        alist_envmix_mix_n(4, buffers, gains, in[k ^ S]);
    }

    *(int16_t*)(save_buffer +  0) = wet;
    *(int16_t*)(save_buffer +  2) = dry;
    *(int16_t*)(save_buffer +  4) = static_cast<int16_t>(ramps[0].target >> 16);
    *(int16_t*)(save_buffer +  6) = static_cast<int16_t>(ramps[1].target >> 16);
    *(int32_t*)(save_buffer +  8) = static_cast<int32_t>(ramps[0].step);
    *(int32_t*)(save_buffer + 10) = static_cast<int32_t>(ramps[1].step);
    *(int32_t*)(save_buffer + 16) = static_cast<int32_t>(ramps[0].value);
    *(int32_t*)(save_buffer + 18) = static_cast<int32_t>(ramps[1].value);
    dram_store_u16(hle, reinterpret_cast<const uint16_t*>(save_buffer), address, 40);
}

// ──────────────────────────────────────────────
// Envelope mixing (NEAD variant)
// ──────────────────────────────────────────────

void alist_envmix_nead(CHle* hle, bool swap_wet_LR,
    uint16_t dmem_dl, uint16_t dmem_dr, uint16_t dmem_wl, uint16_t dmem_wr,
    uint16_t dmemi, unsigned count,
    uint16_t* env_values, uint16_t* env_steps, const int16_t* xors)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    int16_t* in = reinterpret_cast<int16_t*>(buf + (dmemi & 0xFFF));
    int16_t* dl = reinterpret_cast<int16_t*>(buf + (dmem_dl & 0xFFF));
    int16_t* dr = reinterpret_cast<int16_t*>(buf + (dmem_dr & 0xFFF));
    int16_t* wl = reinterpret_cast<int16_t*>(buf + (dmem_wl & 0xFFF));
    int16_t* wr = reinterpret_cast<int16_t*>(buf + (dmem_wr & 0xFFF));

    // Make sure count is a multiple of 8
    count = align(count, 8);

    if (swap_wet_LR)
        std::swap(wl, wr);

    while (count != 0)
    {
        for (size_t i = 0; i < 8; ++i)
        {
            int16_t l  = static_cast<int16_t>(
                (static_cast<int32_t>(in[i^S]) * static_cast<uint32_t>(env_values[0])) >> 16) ^ xors[0];
            int16_t r  = static_cast<int16_t>(
                (static_cast<int32_t>(in[i^S]) * static_cast<uint32_t>(env_values[1])) >> 16) ^ xors[1];

            int16_t l2 = static_cast<int16_t>(
                (static_cast<int32_t>(l) * static_cast<uint32_t>(env_values[2])) >> 16) ^ xors[2];
            int16_t r2 = static_cast<int16_t>(
                (static_cast<int32_t>(r) * static_cast<uint32_t>(env_values[2])) >> 16) ^ xors[3];

            dl[i^S] = clamp_s16(dl[i^S] + l);
            dr[i^S] = clamp_s16(dr[i^S] + r);
            wl[i^S] = clamp_s16(wl[i^S] + l2);
            wr[i^S] = clamp_s16(wr[i^S] + r2);
        }

        // Envelope steps once per 8-sample group
        env_values[0] += env_steps[0];
        env_values[1] += env_steps[1];
        env_values[2] += env_steps[2];

        dl += 8;
        dr += 8;
        wl += 8;
        wr += 8;
        in += 8;
        count -= 8;
    }

}

// ──────────────────────────────────────────────
// Mix / multiply / add
// ──────────────────────────────────────────────

void alist_mix(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint16_t count, int16_t gain)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    int16_t* dst = reinterpret_cast<int16_t*>(buf + dmemo);
    const int16_t* src = reinterpret_cast<const int16_t*>(buf + dmemi);

    count >>= 1;
    while (count != 0) {
        *dst = clamp_s16(*dst + ((static_cast<int32_t>(*src) * gain) >> 15));
        ++dst;
        ++src;
        --count;
    }
}

void alist_multQ44(CHle* hle, uint16_t dmem, uint16_t count, int8_t gain)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    int16_t* dst = reinterpret_cast<int16_t*>(buf + dmem);
    count >>= 1;
    while (count != 0) {
        *dst = clamp_s16(*dst * gain >> 4);
        ++dst;
        --count;
    }
}

void alist_add(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint16_t count)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    int16_t* dst = reinterpret_cast<int16_t*>(buf + dmemo);
    const int16_t* src = reinterpret_cast<const int16_t*>(buf + dmemi);

    count >>= 1;
    while (count != 0) {
        *dst = clamp_s16(*dst + *src);
        ++dst;
        ++src;
        --count;
    }
}

// ──────────────────────────────────────────────
// ADPCM decode
// ──────────────────────────────────────────────

void alist_adpcm(CHle* hle, bool init, bool loop, bool two_bit_per_sample,
    uint16_t dmemo, uint16_t dmemi, uint16_t count,
    const int16_t* codebook, uint32_t loop_address, uint32_t last_frame_address)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;
    if (count == 0) return;

    int16_t last_frame[16]{};

    if (init)
        std::memset(last_frame, 0, sizeof(last_frame));
    else if (loop)
        dram_load_u16(hle, reinterpret_cast<uint16_t*>(last_frame), loop_address, 16);
    else
        dram_load_u16(hle, reinterpret_cast<uint16_t*>(last_frame), last_frame_address, 16);

    // Write initial 16 history samples to output using alist_s16 (^S16)
    for (size_t i = 0; i < 16; ++i, dmemo += 2)
        *alist_s16(hle, dmemo) = last_frame[i];

    // count is in BYTES of output; each ADPCM frame produces 32 bytes (16 samples).
    uint16_t frames = static_cast<uint16_t>((count + 31u) >> 5);
    while (frames != 0)
    {
        // Read header byte using alist_u8 (^S8)
        uint8_t code = *alist_u8(hle, dmemi++);

        unsigned char scaleVal = (code >> 4) & 0x0F;
        const int16_t* cb = codebook + ((code & 0x0F) << 4);

        int16_t samples[16];

        if (!two_bit_per_sample)
        {
            // 4-bit mode: 8 bytes -> 16 samples
            unsigned rshift = (scaleVal < 12) ? (12 - scaleVal) : 0;
            for (unsigned i = 0; i < 8; ++i)
            {
                uint8_t byte = *alist_u8(hle, dmemi++);
                samples[i * 2 + 0] = adpcm_predict_sample(byte, 0xF0, 8, rshift);
                samples[i * 2 + 1] = adpcm_predict_sample(byte, 0x0F, 12, rshift);
            }
        }
        else
        {
            // 2-bit mode: 4 bytes -> 16 samples
            unsigned rshift = (scaleVal < 14) ? (14 - scaleVal) : 0;
            for (unsigned i = 0; i < 4; ++i)
            {
                uint8_t byte = *alist_u8(hle, dmemi++);
                samples[i * 4 + 0] = adpcm_predict_sample(byte, 0xC0, 8, rshift);
                samples[i * 4 + 1] = adpcm_predict_sample(byte, 0x30, 10, rshift);
                samples[i * 4 + 2] = adpcm_predict_sample(byte, 0x0C, 12, rshift);
                samples[i * 4 + 3] = adpcm_predict_sample(byte, 0x03, 14, rshift);
            }
        }

        // Apply predictor filter
        adpcm_compute_residuals(last_frame    , samples    , cb, last_frame + 14, 8);
        adpcm_compute_residuals(last_frame + 8, samples + 8, cb, last_frame + 6, 8);

        // Write decoded frame to output using alist_s16 (^S16)
        for (size_t i = 0; i < 16; ++i, dmemo += 2)
            *alist_s16(hle, dmemo) = last_frame[i];

        --frames;
    }

    // Save last frame state
    dram_store_u16(hle, reinterpret_cast<const uint16_t*>(last_frame), last_frame_address, 16);

}

// ──────────────────────────────────────────────
// Resampling
// ──────────────────────────────────────────────

void alist_resample(CHle* hle, bool init, bool flag2,
    uint16_t dmemo, uint16_t dmemi, uint16_t count,
    uint32_t pitch, uint32_t address)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    uint32_t pitch_accu;
    uint16_t ipos = dmemi >> 1;
    uint16_t opos = dmemo >> 1;
    count >>= 1;
    ipos -= 4;

    if (flag2)
        ; // TODO: check which ABI supports it

    if (init)
    {
        for (unsigned k = 0; k < 4; ++k)
            *sample(hle, ipos + k) = 0;
        pitch_accu = 0;
    }
    else
    {
        // Load state from DRAM (matching mupen64plus alist_resample_load)
        *sample(hle, ipos + 0) = *dram_u16(hle, address + 0);
        *sample(hle, ipos + 1) = *dram_u16(hle, address + 2);
        *sample(hle, ipos + 2) = *dram_u16(hle, address + 4);
        *sample(hle, ipos + 3) = *dram_u16(hle, address + 6);
        pitch_accu = *dram_u16(hle, address + 8);
    }

    while (count != 0)
    {
        const int16_t* lut = RESAMPLE_LUT + ((pitch_accu & 0xfc00) >> 8);

        *sample(hle, opos++) = clamp_s16((
            (*sample(hle, ipos    ) * lut[0]) +
            (*sample(hle, ipos + 1) * lut[1]) +
            (*sample(hle, ipos + 2) * lut[2]) +
            (*sample(hle, ipos + 3) * lut[3]) ) >> 15);

        pitch_accu += pitch;
        ipos += static_cast<uint16_t>(pitch_accu >> 16);
        pitch_accu &= 0xffff;
        --count;
    }

    // Save state to DRAM (matching mupen64plus alist_resample_save)
    *dram_u16(hle, address + 0) = *sample(hle, ipos + 0);
    *dram_u16(hle, address + 2) = *sample(hle, ipos + 1);
    *dram_u16(hle, address + 4) = *sample(hle, ipos + 2);
    *dram_u16(hle, address + 6) = *sample(hle, ipos + 3);
    *dram_u16(hle, address + 8) = static_cast<int16_t>(pitch_accu);

    (void)flag2;
}

// ──────────────────────────────────────────────
// Pole filter (IIR)
// ──────────────────────────────────────────────

void alist_polef(CHle* hle, bool init,
    uint16_t dmemo, uint16_t dmemi, uint16_t count, uint16_t gain,
    int16_t* table, uint32_t address)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    int16_t* dst = reinterpret_cast<int16_t*>(buf + dmemo);

    const int16_t* const h1 = table;
    int16_t* const h2 = table + 8;

    unsigned i;
    int16_t l1, l2;
    int16_t h2_before[8];
    count = align(count, 16);

    if (init) {
        l1 = 0;
        l2 = 0;
    } else {
        l1 = *dram_u16(hle, address + 4);
        l2 = *dram_u16(hle, address + 6);
    }

    for (i = 0; i < 8; ++i) {
        h2_before[i] = h2[i];
        h2[i] = static_cast<int16_t>((static_cast<int32_t>(h2[i]) * gain) >> 14);
    }

    do {
        int16_t frame[8];

        for (i = 0; i < 8; ++i, dmemi += 2)
            frame[i] = *alist_s16(hle, dmemi);

        for (i = 0; i < 8; ++i) {
            int32_t accu = frame[i] * gain;
            accu += h1[i] * l1 + h2_before[i] * l2 + rdot(i, h2, frame);
            dst[i ^ S] = clamp_s16(accu >> 14);
        }

        l1 = dst[6 ^ S];
        l2 = dst[7 ^ S];

        dst += 8;
        count -= 16;
    } while (count != 0);

    dram_store_u32(hle, reinterpret_cast<uint32_t*>(dst - 4), address, 2);
}

// ──────────────────────────────────────────────
// IIR filter
// ──────────────────────────────────────────────

void alist_iirf(CHle* hle, bool init,
    uint16_t dmemo, uint16_t dmemi, uint16_t count,
    int16_t* table, uint32_t address)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    int16_t* dst = reinterpret_cast<int16_t*>(buf + dmemo);
    int32_t prev;
    int16_t frame[8];
    int16_t ibuf[4];
    uint16_t index = 7;
    count = align(count, 16);

    if (init) {
        for (int i = 0; i < 8; ++i)
            frame[i] = 0;
        ibuf[1] = 0;
        ibuf[2] = 0;
    } else {
        frame[6] = *dram_u16(hle, address + 4);
        frame[7] = *dram_u16(hle, address + 6);
        ibuf[1] = static_cast<int16_t>(*dram_u16(hle, address + 8));
        ibuf[2] = static_cast<int16_t>(*dram_u16(hle, address + 10));
    }

    prev = vmulf(table[9], frame[6]) * 2;
    do {
        for (int i = 0; i < 8; ++i) {
            int32_t accu;
            ibuf[index & 3] = *alist_s16(hle, dmemi);
            accu = prev + vmulf(table[0], ibuf[index & 3])
                        + vmulf(table[1], ibuf[(index - 1) & 3])
                        + vmulf(table[0], ibuf[(index - 2) & 3]);
            accu += vmulf(table[8], frame[index]) * 2;
            prev = vmulf(table[9], frame[index]) * 2;
            dst[i ^ S] = frame[i] = static_cast<int16_t>(accu);

            index = (index + 1) & 7;
            dmemi += 2;
        }
        dst += 8;
        count -= 0x10;
    } while (count > 0);

    dram_store_u16(hle, reinterpret_cast<uint16_t*>(&frame[6]), address + 4, 2);
    dram_store_u16(hle, reinterpret_cast<uint16_t*>(&ibuf[(index - 2) & 3]), address + 8, 1);
    dram_store_u16(hle, reinterpret_cast<uint16_t*>(&ibuf[(index - 1) & 3]), address + 10, 1);
}

// ──────────────────────────────────────────────
// Zero-order hold resample
// ──────────────────────────────────────────────

void alist_resample_zoh(CHle* hle,
    uint16_t dmemo, uint16_t dmemi, uint16_t count,
    uint32_t pitch, uint32_t pitch_accu)
{
    uint16_t ipos = dmemi >> 1;
    uint16_t opos = dmemo >> 1;
    count >>= 1;

    while (count != 0) {
        *sample(hle, opos++) = *sample(hle, ipos);
        pitch_accu += pitch;
        ipos += static_cast<uint16_t>(pitch_accu >> 16);
        pitch_accu &= 0xffff;
        --count;
    }
}

// ──────────────────────────────────────────────
// Generic filter
// ──────────────────────────────────────────────

void alist_filter(CHle* hle,
    uint16_t dmem, uint16_t count, uint32_t address,
    const uint32_t* lut_address)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr || hle->dram() == nullptr) return;
    if (lut_address == nullptr) return;

    int x;
    int16_t outbuff[0x3c0];
    int16_t* outp = outbuff;

    // Direct DRAM pointers for LUT coefficients (matching mupen64plus)
    int16_t* const lutt6 = reinterpret_cast<int16_t*>(hle->dram() + lut_address[0]);
    int16_t* const lutt5 = reinterpret_cast<int16_t*>(hle->dram() + lut_address[1]);

    // History from DRAM (direct pointer)
    int16_t* in1 = reinterpret_cast<int16_t*>(hle->dram() + address);
    // Current data from alist_buffer
    int16_t* in2 = reinterpret_cast<int16_t*>(buf + dmem);

    // Average the two coefficient LUTs
    for (x = 0; x < 8; ++x) {
        int32_t v = (lutt5[x] + lutt6[x]) >> 1;
        lutt5[x] = lutt6[x] = static_cast<int16_t>(v);
    }

    // FIR filter with hardcoded indices matching RSP vector unit element ordering
    for (x = 0; x < count; x += 16) {
        int32_t v[8];

        v[1] =  in1[0] * lutt6[6];
        v[1] += in1[3] * lutt6[7];
        v[1] += in1[2] * lutt6[4];
        v[1] += in1[5] * lutt6[5];
        v[1] += in1[4] * lutt6[2];
        v[1] += in1[7] * lutt6[3];
        v[1] += in1[6] * lutt6[0];
        v[1] += in2[1] * lutt6[1];

        v[0] =  in1[3] * lutt6[6];
        v[0] += in1[2] * lutt6[7];
        v[0] += in1[5] * lutt6[4];
        v[0] += in1[4] * lutt6[5];
        v[0] += in1[7] * lutt6[2];
        v[0] += in1[6] * lutt6[3];
        v[0] += in2[1] * lutt6[0];
        v[0] += in2[0] * lutt6[1];

        v[3] =  in1[2] * lutt6[6];
        v[3] += in1[5] * lutt6[7];
        v[3] += in1[4] * lutt6[4];
        v[3] += in1[7] * lutt6[5];
        v[3] += in1[6] * lutt6[2];
        v[3] += in2[1] * lutt6[3];
        v[3] += in2[0] * lutt6[0];
        v[3] += in2[3] * lutt6[1];

        v[2] =  in1[5] * lutt6[6];
        v[2] += in1[4] * lutt6[7];
        v[2] += in1[7] * lutt6[4];
        v[2] += in1[6] * lutt6[5];
        v[2] += in2[1] * lutt6[2];
        v[2] += in2[0] * lutt6[3];
        v[2] += in2[3] * lutt6[0];
        v[2] += in2[2] * lutt6[1];

        v[5] =  in1[4] * lutt6[6];
        v[5] += in1[7] * lutt6[7];
        v[5] += in1[6] * lutt6[4];
        v[5] += in2[1] * lutt6[5];
        v[5] += in2[0] * lutt6[2];
        v[5] += in2[3] * lutt6[3];
        v[5] += in2[2] * lutt6[0];
        v[5] += in2[5] * lutt6[1];

        v[4] =  in1[7] * lutt6[6];
        v[4] += in1[6] * lutt6[7];
        v[4] += in2[1] * lutt6[4];
        v[4] += in2[0] * lutt6[5];
        v[4] += in2[3] * lutt6[2];
        v[4] += in2[2] * lutt6[3];
        v[4] += in2[5] * lutt6[0];
        v[4] += in2[4] * lutt6[1];

        v[7] =  in1[6] * lutt6[6];
        v[7] += in2[1] * lutt6[7];
        v[7] += in2[0] * lutt6[4];
        v[7] += in2[3] * lutt6[5];
        v[7] += in2[2] * lutt6[2];
        v[7] += in2[5] * lutt6[3];
        v[7] += in2[4] * lutt6[0];
        v[7] += in2[7] * lutt6[1];

        v[6] =  in2[1] * lutt6[6];
        v[6] += in2[0] * lutt6[7];
        v[6] += in2[3] * lutt6[4];
        v[6] += in2[2] * lutt6[5];
        v[6] += in2[5] * lutt6[2];
        v[6] += in2[4] * lutt6[3];
        v[6] += in2[7] * lutt6[0];
        v[6] += in2[6] * lutt6[1];

        outp[1] = static_cast<int16_t>((v[1] + 0x4000) >> 15);
        outp[0] = static_cast<int16_t>((v[0] + 0x4000) >> 15);
        outp[3] = static_cast<int16_t>((v[3] + 0x4000) >> 15);
        outp[2] = static_cast<int16_t>((v[2] + 0x4000) >> 15);
        outp[5] = static_cast<int16_t>((v[5] + 0x4000) >> 15);
        outp[4] = static_cast<int16_t>((v[4] + 0x4000) >> 15);
        outp[7] = static_cast<int16_t>((v[7] + 0x4000) >> 15);
        outp[6] = static_cast<int16_t>((v[6] + 0x4000) >> 15);
        in1 = in2;
        in2 += 8;
        outp += 8;
    }

    // Save last input block as new history to DRAM
    std::memcpy(hle->dram() + address, in2 - 8, 16);
    // Copy filtered output back to alist_buffer
    std::memcpy(buf + dmem, outbuff, count);
}

// ──────────────────────────────────────────────
// De-interleave (stereo → separate L/R)
// ──────────────────────────────────────────────

void alist_deinterleave(CHle* hle,
    uint16_t dmem_left, uint16_t dmem_right, uint16_t dmem_src, uint16_t count)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    const int16_t* src = reinterpret_cast<const int16_t*>(buf + (dmem_src & 0xFFF));
    int16_t* left  = reinterpret_cast<int16_t*>(buf + (dmem_left & 0xFFF));
    int16_t* right = reinterpret_cast<int16_t*>(buf + (dmem_right & 0xFFF));

    // Interleaved format: L0 R0 L1 R1 ...
    const uint16_t pairs = count / 4; // count is in bytes, 4 bytes per stereo pair
    for (uint16_t i = 0; i < pairs; i++)
    {
        left[i]  = src[i * 2];
        right[i] = src[i * 2 + 1];
    }
}

// ──────────────────────────────────────────────
// Dynamic range compressor (soft-knee limiter)
// ──────────────────────────────────────────────

void alist_compress(CHle* hle,
    uint16_t dmem, uint16_t count,
    int16_t threshold, int16_t ratio, int16_t attack, int16_t release)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    int16_t* data = reinterpret_cast<int16_t*>(buf + (dmem & 0xFFF));
    const int32_t thresh = static_cast<int32_t>(threshold > 0 ? threshold : 1);
    const int32_t ratio_inv = static_cast<int32_t>(ratio > 0 ? ratio : 1);

    // Envelope state (per-call, not persisted — suitable for block processing)
    int32_t envelope = thresh;

    for (uint16_t i = 0; i < count / 2; i++)
    {
        int32_t sample = data[i];
        int32_t abs_sample = sample < 0 ? -sample : sample;

        // Envelope follower: attack ramps up, release ramps down
        if (abs_sample > envelope)
        {
            // Attack: fast ramp toward peak
            int32_t diff = abs_sample - envelope;
            envelope += static_cast<int16_t>((diff * attack) >> 15);
        }
        else
        {
            // Release: slow ramp back to threshold
            int32_t diff = envelope - abs_sample;
            envelope -= static_cast<int16_t>((diff * release) >> 15);
            if (envelope < thresh) envelope = thresh;
        }

        // Apply gain reduction when envelope exceeds threshold
        if (envelope > thresh)
        {
            // Gain = threshold + (envelope - threshold) / ratio
            int32_t excess = envelope - thresh;
            int32_t compressed_level = thresh + excess / ratio_inv;
            // Scale sample by ratio of compressed/envelope
            sample = static_cast<int32_t>((static_cast<int64_t>(sample) * compressed_level) / envelope);
        }

        data[i] = clamp_s16(sample);
    }
}

// ──────────────────────────────────────────────
// Chorus effect (modulated delay line)
// ──────────────────────────────────────────────

void alist_chorus(CHle* hle,
    uint16_t dmemo, uint16_t dmemi, uint16_t count,
    uint16_t delay_base, uint16_t depth, uint16_t rate_hz, int16_t wet_gain)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    const int16_t* src = reinterpret_cast<const int16_t*>(buf + (dmemi & 0xFFF));
    int16_t* dst = reinterpret_cast<int16_t*>(buf + (dmemo & 0xFFF));

    const uint16_t samples = count / 2;
    if (samples == 0) return;

    // Fixed-point LFO phase accumulator (16.16)
    // rate_hz is in Hz * 256 format, normalized to sample rate ~ 32000 Hz
    const uint32_t phase_inc = (static_cast<uint32_t>(rate_hz) << 16) / 32000;
    uint32_t phase = 0;

    for (uint16_t i = 0; i < samples; i++)
    {
        // LFO generates a triangle-ish modulation via sine approximation
        // Use simple triangle: phase [0..0xFFFF] → [-depth..+depth]
        int32_t lfo;
        uint16_t ph16 = static_cast<uint16_t>(phase >> 16);
        if (ph16 < 0x8000)
            lfo = (static_cast<int32_t>(ph16) * depth) >> 14;  // 0..+depth
        else
            lfo = (static_cast<int32_t>(0xFFFF - ph16) * depth) >> 14;
        lfo -= depth / 2; // center around 0

        // Modulated delay tap
        int32_t tap = static_cast<int32_t>(delay_base) + lfo;
        if (tap < 0) tap = 0;
        if (tap >= samples) tap = samples - 1;

        int32_t delayed_idx = static_cast<int32_t>(i) - tap;
        int16_t delayed_sample = 0;
        if (delayed_idx >= 0 && delayed_idx < samples)
            delayed_sample = src[delayed_idx];

        // Mix: dry + wet * delayed
        int32_t wet = (static_cast<int32_t>(delayed_sample) * wet_gain) >> 15;
        dst[i] = clamp_s16(static_cast<int32_t>(src[i]) + wet);

        phase += phase_inc;
    }
}

// ──────────────────────────────────────────────
// Simple delay line (echo / feedback)
// ──────────────────────────────────────────────

void alist_delay(CHle* hle,
    uint16_t dmemo, uint16_t dmemi, uint16_t count,
    uint16_t delay_samples, int16_t feedback, int16_t wet_gain)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    const int16_t* src = reinterpret_cast<const int16_t*>(buf + (dmemi & 0xFFF));
    int16_t* dst = reinterpret_cast<int16_t*>(buf + (dmemo & 0xFFF));

    const uint16_t samples = count / 2;
    if (samples == 0 || delay_samples == 0) return;

    // Simple feedback delay: out[i] = in[i] + wet * out[i - delay]
    // Process in-place is fine since we read behind the write cursor
    for (uint16_t i = 0; i < samples; i++)
    {
        int32_t dry = src[i];
        int32_t delayed = 0;

        if (i >= delay_samples)
        {
            // Read from the already-written output
            delayed = dst[i - delay_samples];
        }

        // Apply feedback to the delayed signal
        int32_t fb = (delayed * static_cast<int32_t>(feedback)) >> 15;
        // Mix wet delayed + feedback into output
        int32_t wet = ((delayed + fb) * static_cast<int32_t>(wet_gain)) >> 15;
        dst[i] = clamp_s16(dry + wet);
    }
}

// ──────────────────────────────────────────────
// High-pass / Low-pass single-pole IIR
// ──────────────────────────────────────────────

void alist_hilopass(CHle* hle,
    uint16_t dmemo, uint16_t dmemi, uint16_t count,
    int16_t cutoff_coeff, FilterType type, uint32_t state_address)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    const int16_t* src = reinterpret_cast<const int16_t*>(buf + (dmemi & 0xFFF));
    int16_t* dst = reinterpret_cast<int16_t*>(buf + (dmemo & 0xFFF));

    // Load persisted filter state from DRAM (1 sample)
    int16_t state[2]{}; // [0] = y_prev, [1] = x_prev
    if (state_address != 0)
    {
        dram_load_u16(hle, reinterpret_cast<uint16_t*>(state), state_address, 2);
    }

    int32_t y_prev = state[0];
    int32_t x_prev = state[1];
    const int32_t alpha = static_cast<int32_t>(cutoff_coeff); // Q15 coefficient

    for (uint16_t i = 0; i < count / 2; i++)
    {
        int32_t x = src[i];

        if (type == FilterType::LowPass)
        {
            // y[n] = y[n-1] + alpha * (x[n] - y[n-1])
            y_prev += (alpha * (x - y_prev)) >> 15;
        }
        else
        {
            // High-pass: y[n] = alpha * (y[n-1] + x[n] - x[n-1])
            y_prev = (alpha * (y_prev + x - x_prev)) >> 15;
        }

        x_prev = x;
        dst[i] = clamp_s16(y_prev);
    }

    // Save state back
    state[0] = static_cast<int16_t>(y_prev);
    state[1] = static_cast<int16_t>(x_prev);
    if (state_address != 0)
    {
        dram_store_u16(hle, reinterpret_cast<const uint16_t*>(state), state_address, 2);
    }
}

// ──────────────────────────────────────────────
// Stereo pan with crossfade
// ──────────────────────────────────────────────

void alist_pan(CHle* hle,
    uint16_t dmem_left, uint16_t dmem_right, uint16_t count,
    int16_t pan_left, int16_t pan_right)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    int16_t* left  = reinterpret_cast<int16_t*>(buf + (dmem_left & 0xFFF));
    int16_t* right = reinterpret_cast<int16_t*>(buf + (dmem_right & 0xFFF));

    // pan_left / pan_right are Q15 gain values
    for (uint16_t i = 0; i < count / 2; i++)
    {
        int32_t L = left[i];
        int32_t R = right[i];

        // Cross-fade: each output = L * pan_L + R * pan_R
        // For center pan: pan_left=0x5A82, pan_right=0x5A82 (≈0.707 ≈ -3dB)
        left[i]  = clamp_s16((L * pan_left + R * (0x7FFF - pan_right)) >> 15);
        right[i] = clamp_s16((R * pan_right + L * (0x7FFF - pan_left)) >> 15);
    }
}

// ──────────────────────────────────────────────
// DC offset removal (single-pole high-pass at ~20 Hz)
// ──────────────────────────────────────────────

void alist_dc_remove(CHle* hle,
    uint16_t dmem, uint16_t count, uint32_t state_address)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    int16_t* data = reinterpret_cast<int16_t*>(buf + (dmem & 0xFFF));

    // Load state: [0] = x_prev, [1] = y_prev
    int16_t state[2]{};
    if (state_address != 0)
    {
        dram_load_u16(hle, reinterpret_cast<uint16_t*>(state), state_address, 2);
    }

    int32_t x_prev = state[0];
    int32_t y_prev = state[1];

    // HP filter coefficient ≈ 0.995 in Q15 = 32604
    constexpr int32_t DC_ALPHA = 32604;

    for (uint16_t i = 0; i < count / 2; i++)
    {
        int32_t x = data[i];
        // y[n] = alpha * (y[n-1] + x[n] - x[n-1])
        y_prev = (DC_ALPHA * (y_prev + x - x_prev)) >> 15;
        x_prev = x;
        data[i] = clamp_s16(y_prev);
    }

    state[0] = static_cast<int16_t>(x_prev);
    state[1] = static_cast<int16_t>(y_prev);
    if (state_address != 0)
    {
        dram_store_u16(hle, reinterpret_cast<const uint16_t*>(state), state_address, 2);
    }
}

} // namespace Nova64::RSP::HLE
