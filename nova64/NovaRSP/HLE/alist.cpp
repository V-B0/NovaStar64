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
#include <cstring>
#include <cmath>

namespace Nova64::RSP::HLE {

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
    // Clear with wrapping around 4KB DMEM boundary (matching mupen64plus)
    while (count != 0) {
        buf[dmem & 0xFFF] = 0;
        ++dmem;
        --count;
    }
}

void alist_load(CHle* hle, uint16_t dmem, uint32_t address, uint16_t count)
{
    // Enforce DMA alignment constraints (matching N64 SP DMA)
    dmem    &= ~3;
    address &= ~7u;
    count    = static_cast<uint16_t>(align(count, 8));
    uint16_t off = dmem & 0xFFF;
    uint16_t safe = std::min<uint16_t>(count, 0x1000 - off);
    // Load as uint16_t so DMEM contains host-native int16_t values.
    // All audio processing reads int16_t via reinterpret_cast, so this
    // must produce correct host-native values.
    dram_load_u16(hle, reinterpret_cast<uint16_t*>(hle->alist_buffer() + off), address, safe / 2);
}

void alist_save(CHle* hle, uint16_t dmem, uint32_t address, uint16_t count)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr || hle->dram() == nullptr) return;
    // Enforce DMA alignment constraints
    dmem    &= ~3;
    address &= ~7u;
    count    = static_cast<uint16_t>(align(count, 8));
    uint16_t off = dmem & 0xFFF;
    uint16_t safe = std::min<uint16_t>(count, 0x1000 - off);

    // Store as uint16_t to write host-native int16_t back to RDRAM's
    // XOR byte layout, ensuring correct round-trip with alist_load.
    dram_store_u16(hle, reinterpret_cast<const uint16_t*>(buf + off), address, safe / 2);
}

void alist_move(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint16_t count)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;
    uint16_t ooff = dmemo & 0xFFF;
    uint16_t ioff = dmemi & 0xFFF;
    uint16_t safe = std::min<uint16_t>(count, std::min<uint16_t>(0x1000 - ooff, 0x1000 - ioff));
    std::memmove(buf + ooff, buf + ioff, safe);
}

void alist_copy_every_other_sample(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint16_t count)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    const int16_t* src = reinterpret_cast<const int16_t*>(buf + (dmemi & 0xFFF));
    int16_t* dst = reinterpret_cast<int16_t*>(buf + (dmemo & 0xFFF));

    for (uint16_t i = 0; i < count; i++)
    {
        dst[i] = src[i * 2];
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

    uint16_t ooff = dmemo & 0xFFF;
    uint16_t loff = left & 0xFFF;
    uint16_t roff = right & 0xFFF;
    // Clamp sample count so reads and interleaved writes stay within DMEM
    uint16_t maxSamples = std::min<uint16_t>((0x1000 - ooff) / 4,
                          std::min<uint16_t>((0x1000 - loff) / 2, (0x1000 - roff) / 2));
    uint16_t samples = std::min<uint16_t>(count / 2, maxSamples);

    const int16_t* srcL = reinterpret_cast<const int16_t*>(buf + loff);
    const int16_t* srcR = reinterpret_cast<const int16_t*>(buf + roff);
    int16_t* dst = reinterpret_cast<int16_t*>(buf + ooff);

    for (uint16_t i = 0; i < samples; i++)
    {
        dst[i * 2 + 0] = srcL[i];
        dst[i * 2 + 1] = srcR[i];
    }
}

// ──────────────────────────────────────────────
// Envelope mixing (exponential)
// ──────────────────────────────────────────────

void alist_envmix_exp(CHle* hle, bool init, bool aux,
    uint16_t dmem_dl, uint16_t dmem_dr, uint16_t dmem_wl, uint16_t dmem_wr,
    uint16_t dmemi, uint16_t count, int16_t dry, int16_t wet,
    const int16_t* vol, const int16_t* target, const int32_t* rate, uint32_t address)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    int16_t volL = vol[0], volR = vol[1];
    const int16_t targetL = target[0], targetR = target[1];
    const int32_t rateL = rate[0], rateR = rate[1];

    int16_t save_buffer[40];
    if (init)
    {
        std::memset(save_buffer, 0, sizeof(save_buffer));
    }
    else
    {
        dram_load_u16(hle, reinterpret_cast<uint16_t*>(save_buffer), address, 40);
    }

    const int16_t* src = reinterpret_cast<const int16_t*>(buf + (dmemi & 0xFFF));
    int16_t* dryL = reinterpret_cast<int16_t*>(buf + (dmem_dl & 0xFFF));
    int16_t* dryR = reinterpret_cast<int16_t*>(buf + (dmem_dr & 0xFFF));
    int16_t* wetL = aux ? reinterpret_cast<int16_t*>(buf + (dmem_wl & 0xFFF)) : nullptr;
    int16_t* wetR = aux ? reinterpret_cast<int16_t*>(buf + (dmem_wr & 0xFFF)) : nullptr;

    const uint16_t samples = count / 2;
    for (uint16_t i = 0; i < samples; i++)
    {
        int32_t sample = src[i];

        // Apply envelope
        int32_t outL = clamp_s16(static_cast<int32_t>((sample * volL) >> 16));
        int32_t outR = clamp_s16(static_cast<int32_t>((sample * volR) >> 16));

        // Mix into dry buffers
        dryL[i] = clamp_s16(dryL[i] + ((outL * dry) >> 16));
        dryR[i] = clamp_s16(dryR[i] + ((outR * dry) >> 16));

        // Mix into wet buffers if aux
        if (wetL != nullptr) wetL[i] = clamp_s16(wetL[i] + ((outL * wet) >> 16));
        if (wetR != nullptr) wetR[i] = clamp_s16(wetR[i] + ((outR * wet) >> 16));

        // Exponential envelope ramp
        int32_t diffL = targetL - volL;
        int32_t diffR = targetR - volR;
        volL += static_cast<int16_t>((diffL * rateL) >> 16);
        volR += static_cast<int16_t>((diffR * rateR) >> 16);
    }

    // Save state back to DRAM
    save_buffer[0] = volL;
    save_buffer[1] = volR;
    dram_store_u16(hle, reinterpret_cast<const uint16_t*>(save_buffer), address, 40);
}

// ──────────────────────────────────────────────
// Envelope mixing (GoldenEye / GE variant)
// GE uses a fixed-step linear ramp computed once
// at init, rather than the iterative exponential
// approach of the standard envmixer.
// ──────────────────────────────────────────────

void alist_envmix_ge(CHle* hle, bool init, bool aux,
    uint16_t dmem_dl, uint16_t dmem_dr, uint16_t dmem_wl, uint16_t dmem_wr,
    uint16_t dmemi, uint16_t count, int16_t dry, int16_t wet,
    const int16_t* vol, const int16_t* target, const int32_t* rate, uint32_t address)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    int16_t volL, volR;
    int32_t stepL, stepR;

    int16_t save_buffer[40];
    if (init)
    {
        volL = vol[0];
        volR = vol[1];
        // GE computes a fixed linear step from rate and (target - current)
        stepL = static_cast<int32_t>((static_cast<int64_t>(target[0] - volL) * rate[0]) >> 16);
        stepR = static_cast<int32_t>((static_cast<int64_t>(target[1] - volR) * rate[1]) >> 16);
        std::memset(save_buffer, 0, sizeof(save_buffer));
    }
    else
    {
        dram_load_u16(hle, reinterpret_cast<uint16_t*>(save_buffer), address, 40);
        volL  = save_buffer[0];
        volR  = save_buffer[1];
        stepL = static_cast<int32_t>(save_buffer[2]);
        stepR = static_cast<int32_t>(save_buffer[3]);
    }

    const int16_t* src = reinterpret_cast<const int16_t*>(buf + (dmemi & 0xFFF));
    int16_t* dryL = reinterpret_cast<int16_t*>(buf + (dmem_dl & 0xFFF));
    int16_t* dryR = reinterpret_cast<int16_t*>(buf + (dmem_dr & 0xFFF));
    int16_t* wetL = aux ? reinterpret_cast<int16_t*>(buf + (dmem_wl & 0xFFF)) : nullptr;
    int16_t* wetR = aux ? reinterpret_cast<int16_t*>(buf + (dmem_wr & 0xFFF)) : nullptr;

    const uint16_t samples = count / 2;
    for (uint16_t i = 0; i < samples; i++)
    {
        int32_t sample = src[i];

        int32_t outL = clamp_s16(static_cast<int32_t>((sample * volL) >> 16));
        int32_t outR = clamp_s16(static_cast<int32_t>((sample * volR) >> 16));

        dryL[i] = clamp_s16(dryL[i] + ((outL * dry) >> 16));
        dryR[i] = clamp_s16(dryR[i] + ((outR * dry) >> 16));

        if (wetL != nullptr) wetL[i] = clamp_s16(wetL[i] + ((outL * wet) >> 16));
        if (wetR != nullptr) wetR[i] = clamp_s16(wetR[i] + ((outR * wet) >> 16));

        // GE: fixed linear step ramp (step computed once at init)
        volL = clamp_s16(static_cast<int32_t>(volL) + stepL);
        volR = clamp_s16(static_cast<int32_t>(volR) + stepR);
    }

    // Save state back to DRAM
    save_buffer[0] = volL;
    save_buffer[1] = volR;
    save_buffer[2] = static_cast<int16_t>(stepL);
    save_buffer[3] = static_cast<int16_t>(stepR);
    dram_store_u16(hle, reinterpret_cast<const uint16_t*>(save_buffer), address, 40);
}

// ──────────────────────────────────────────────
// Envelope mixing (linear ramp)
// ──────────────────────────────────────────────

void alist_envmix_lin(CHle* hle, bool init,
    uint16_t dmem_dl, uint16_t dmem_dr, uint16_t dmem_wl, uint16_t dmem_wr,
    uint16_t dmemi, uint16_t count, int16_t dry, int16_t wet,
    const int16_t* vol, const int16_t* target, const int32_t* rate, uint32_t address)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    int16_t volL, volR;
    const int16_t stepL = static_cast<int16_t>(rate[0] >> 16);
    const int16_t stepR = static_cast<int16_t>(rate[1] >> 16);

    // Load persisted volume state if not initializing
    if (init)
    {
        volL = vol[0];
        volR = vol[1];
    }
    else if (address != 0)
    {
        int16_t saved[4]{};
        dram_load_u16(hle, reinterpret_cast<uint16_t*>(saved), address, 4);
        volL = saved[0];
        volR = saved[1];
    }
    else
    {
        volL = vol[0];
        volR = vol[1];
    }

    const int16_t* src = reinterpret_cast<const int16_t*>(buf + (dmemi & 0xFFF));
    int16_t* dryL = reinterpret_cast<int16_t*>(buf + (dmem_dl & 0xFFF));
    int16_t* dryR = reinterpret_cast<int16_t*>(buf + (dmem_dr & 0xFFF));
    int16_t* wetL = reinterpret_cast<int16_t*>(buf + (dmem_wl & 0xFFF));
    int16_t* wetR = reinterpret_cast<int16_t*>(buf + (dmem_wr & 0xFFF));

    const uint16_t samples = count / 2;
    for (uint16_t i = 0; i < samples; i++)
    {
        int32_t sample = src[i];
        int32_t outL = clamp_s16((sample * volL) >> 16);
        int32_t outR = clamp_s16((sample * volR) >> 16);

        dryL[i] = clamp_s16(dryL[i] + ((outL * dry) >> 16));
        dryR[i] = clamp_s16(dryR[i] + ((outR * dry) >> 16));
        wetL[i] = clamp_s16(wetL[i] + ((outL * wet) >> 16));
        wetR[i] = clamp_s16(wetR[i] + ((outR * wet) >> 16));

        // Linear ramp: clamp at target
        if ((stepL > 0 && volL < target[0]) || (stepL < 0 && volL > target[0]))
            volL += stepL;
        else
            volL = target[0];
        if ((stepR > 0 && volR < target[1]) || (stepR < 0 && volR > target[1]))
            volR += stepR;
        else
            volR = target[1];
    }

    // Save volume state for next call
    if (address != 0)
    {
        int16_t save_state[4]{};
        save_state[0] = volL;
        save_state[1] = volR;
        dram_store_u16(hle, reinterpret_cast<const uint16_t*>(save_state), address, 4);
    }
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
            // Dry: sample * unsigned env, then XOR result for polarity
            int16_t l  = static_cast<int16_t>(
                (static_cast<int32_t>(in[i]) * static_cast<uint32_t>(env_values[0])) >> 16) ^ xors[0];
            int16_t r  = static_cast<int16_t>(
                (static_cast<int32_t>(in[i]) * static_cast<uint32_t>(env_values[1])) >> 16) ^ xors[1];

            // Wet: uses dry L/R results as input, not raw sample
            int16_t l2 = static_cast<int16_t>(
                (static_cast<int32_t>(l) * static_cast<uint32_t>(env_values[2])) >> 16) ^ xors[2];
            int16_t r2 = static_cast<int16_t>(
                (static_cast<int32_t>(r) * static_cast<uint32_t>(env_values[2])) >> 16) ^ xors[3];

            dl[i] = clamp_s16(dl[i] + l);
            dr[i] = clamp_s16(dr[i] + r);
            wl[i] = clamp_s16(wl[i] + l2);
            wr[i] = clamp_s16(wr[i] + r2);
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

    uint16_t ooff = dmemo & 0xFFF;
    uint16_t ioff = dmemi & 0xFFF;
    uint16_t maxBytes = std::min<uint16_t>(0x1000 - ooff, 0x1000 - ioff);
    uint16_t samples = std::min<uint16_t>(count / 2, maxBytes / 2);

    const int16_t* src = reinterpret_cast<const int16_t*>(buf + ioff);
    int16_t* dst = reinterpret_cast<int16_t*>(buf + ooff);

    for (uint16_t i = 0; i < samples; i++)
    {
        dst[i] = clamp_s16(dst[i] + ((static_cast<int32_t>(src[i]) * gain) >> 15));
    }
}

void alist_multQ44(CHle* hle, uint16_t dmem, uint16_t count, int8_t gain)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    uint16_t off = dmem & 0xFFF;
    uint16_t samples = std::min<uint16_t>(count / 2, (0x1000 - off) / 2);
    int16_t* data = reinterpret_cast<int16_t*>(buf + off);
    for (uint16_t i = 0; i < samples; i++)
    {
        data[i] = clamp_s16((static_cast<int32_t>(data[i]) * gain) >> 4);
    }
}

void alist_add(CHle* hle, uint16_t dmemo, uint16_t dmemi, uint16_t count)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    const int16_t* src = reinterpret_cast<const int16_t*>(buf + (dmemi & 0xFFF));
    int16_t* dst = reinterpret_cast<int16_t*>(buf + (dmemo & 0xFFF));

    for (uint16_t i = 0; i < count / 2; i++)
    {
        dst[i] = clamp_s16(static_cast<int32_t>(dst[i]) + src[i]);
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

    int16_t* dst = reinterpret_cast<int16_t*>(buf + (dmemo & 0xFFF));

    // Write initial 16 history samples to output (matches N64 RSP microcode)
    std::memcpy(dst, last_frame, 16 * sizeof(int16_t));
    dst += 16;

    const uint8_t* src = buf + (dmemi & 0xFFF);
    unsigned pos = 0;

    for (uint16_t frame = 0; frame < count; frame++)
    {
        // Read header byte (^1 for byte swap within host-native halfword)
        uint8_t code = src[pos ^ 1];
        pos++;

        unsigned char scaleVal = (code >> 4) & 0x0F;
        const int16_t* cb = codebook + ((code & 0x0F) << 4);

        int16_t samples[16];

        if (!two_bit_per_sample)
        {
            // 4-bit mode: 8 bytes -> 16 samples (matches mupen64plus adpcm_predict_frame_4bits)
            unsigned rshift = (scaleVal < 12) ? (12 - scaleVal) : 0;
            for (unsigned i = 0; i < 8; ++i)
            {
                uint8_t byte = src[(pos + i) ^ 1];
                samples[i * 2 + 0] = adpcm_predict_sample(byte, 0xF0, 8, rshift);
                samples[i * 2 + 1] = adpcm_predict_sample(byte, 0x0F, 12, rshift);
            }
            pos += 8;
        }
        else
        {
            // 2-bit mode: 4 bytes -> 16 samples (matches mupen64plus adpcm_predict_frame_2bits)
            unsigned rshift = (scaleVal < 14) ? (14 - scaleVal) : 0;
            for (unsigned i = 0; i < 4; ++i)
            {
                uint8_t byte = src[(pos + i) ^ 1];
                samples[i * 4 + 0] = adpcm_predict_sample(byte, 0xC0, 8, rshift);
                samples[i * 4 + 1] = adpcm_predict_sample(byte, 0x30, 10, rshift);
                samples[i * 4 + 2] = adpcm_predict_sample(byte, 0x0C, 12, rshift);
                samples[i * 4 + 3] = adpcm_predict_sample(byte, 0x03, 14, rshift);
            }
            pos += 4;
        }

        // Apply predictor filter: two half-blocks of 8 samples (matching mupen64plus)
        adpcm_compute_residuals(last_frame    , samples    , cb, last_frame + 14, 8);
        adpcm_compute_residuals(last_frame + 8, samples + 8, cb, last_frame + 6, 8);

        // Write decoded frame to output
        std::memcpy(dst + frame * 16, last_frame, 16 * sizeof(int16_t));
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

    // Access DMEM as int16_t array with wrapping (4096 bytes = 2048 samples)
    int16_t* const samples = reinterpret_cast<int16_t*>(buf);
    auto smpl = [samples](uint16_t pos) -> int16_t& {
        return samples[pos & 0x7FF];
    };

    uint32_t pitch_accu;
    uint16_t ipos = dmemi >> 1;
    uint16_t opos = dmemo >> 1;
    count >>= 1;
    ipos -= 4;

    if (init)
    {
        smpl(ipos) = 0; smpl(ipos + 1) = 0; smpl(ipos + 2) = 0; smpl(ipos + 3) = 0;
        pitch_accu = 0;
    }
    else
    {
        uint16_t sb[5];
        dram_load_u16(hle, sb, address, 5);
        smpl(ipos)     = static_cast<int16_t>(sb[0]);
        smpl(ipos + 1) = static_cast<int16_t>(sb[1]);
        smpl(ipos + 2) = static_cast<int16_t>(sb[2]);
        smpl(ipos + 3) = static_cast<int16_t>(sb[3]);
        pitch_accu = sb[4];
    }

    while (count != 0)
    {
        const int16_t* lut = RESAMPLE_LUT + ((pitch_accu & 0xfc00) >> 8);

        int32_t s = static_cast<int32_t>(smpl(ipos))     * lut[0]
                  + static_cast<int32_t>(smpl(ipos + 1)) * lut[1]
                  + static_cast<int32_t>(smpl(ipos + 2)) * lut[2]
                  + static_cast<int32_t>(smpl(ipos + 3)) * lut[3];

        smpl(opos++) = clamp_s16(s >> 15);

        pitch_accu += pitch;
        ipos += static_cast<uint16_t>(pitch_accu >> 16);
        pitch_accu &= 0xffff;
        --count;
    }

    // Save state: 4 samples at final ipos + pitch_accu
    uint16_t sb[5];
    sb[0] = static_cast<uint16_t>(smpl(ipos));
    sb[1] = static_cast<uint16_t>(smpl(ipos + 1));
    sb[2] = static_cast<uint16_t>(smpl(ipos + 2));
    sb[3] = static_cast<uint16_t>(smpl(ipos + 3));
    sb[4] = static_cast<uint16_t>(pitch_accu);
    dram_store_u16(hle, sb, address, 5);

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

    int16_t state[4]{};
    if (!init)
    {
        dram_load_u16(hle, reinterpret_cast<uint16_t*>(state), address, 4);
    }

    const int16_t* src = reinterpret_cast<const int16_t*>(buf + (dmemi & 0xFFF));
    int16_t* dst = reinterpret_cast<int16_t*>(buf + (dmemo & 0xFFF));

    for (uint16_t i = 0; i < count / 2; i++)
    {
        int32_t acc = static_cast<int32_t>(src[i]) * gain;
        acc += static_cast<int32_t>(state[0]) * table[0];
        acc += static_cast<int32_t>(state[1]) * table[1];
        acc >>= 14;
        dst[i] = clamp_s16(acc);
        state[1] = state[0];
        state[0] = dst[i];
    }

    dram_store_u16(hle, reinterpret_cast<const uint16_t*>(state), address, 4);
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

    int16_t state[4]{};
    if (!init)
    {
        dram_load_u16(hle, reinterpret_cast<uint16_t*>(state), address, 4);
    }

    const int16_t* src = reinterpret_cast<const int16_t*>(buf + (dmemi & 0xFFF));
    int16_t* dst = reinterpret_cast<int16_t*>(buf + (dmemo & 0xFFF));

    for (uint16_t i = 0; i < count / 2; i++)
    {
        int32_t acc = static_cast<int32_t>(src[i]) * table[0];
        acc += static_cast<int32_t>(state[0]) * table[1];
        acc += static_cast<int32_t>(state[1]) * table[2];
        acc += static_cast<int32_t>(state[2]) * table[3];
        acc >>= 14;
        state[2] = state[1];
        state[1] = state[0];
        state[0] = src[i];
        dst[i] = clamp_s16(acc);
    }

    dram_store_u16(hle, reinterpret_cast<const uint16_t*>(state), address, 4);
}

// ──────────────────────────────────────────────
// Zero-order hold resample
// ──────────────────────────────────────────────

void alist_resample_zoh(CHle* hle,
    uint16_t dmemo, uint16_t dmemi, uint16_t count,
    uint32_t pitch, uint32_t pitch_accu)
{
    uint8_t* buf = hle->alist_buffer();
    if (buf == nullptr) return;

    const int16_t* src = reinterpret_cast<const int16_t*>(buf + (dmemi & 0xFFF));
    int16_t* dst = reinterpret_cast<int16_t*>(buf + (dmemo & 0xFFF));

    for (uint16_t i = 0; i < count / 2; i++)
    {
        uint32_t idx = pitch_accu >> 16;
        dst[i] = src[idx];
        pitch_accu += pitch;
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

    // Load and average the two coefficient LUTs (8 int16_t each)
    int16_t lutt6[8], lutt5[8];
    dram_load_u16(hle, reinterpret_cast<uint16_t*>(lutt6), lut_address[0], 8);
    dram_load_u16(hle, reinterpret_cast<uint16_t*>(lutt5), lut_address[1], 8);

    for (int x = 0; x < 8; ++x) {
        int32_t v = (static_cast<int32_t>(lutt5[x]) + static_cast<int32_t>(lutt6[x])) >> 1;
        lutt5[x] = lutt6[x] = static_cast<int16_t>(v);
    }

    // Store averaged coefficients back to DRAM
    dram_store_u16(hle, reinterpret_cast<uint16_t*>(lutt6), lut_address[0], 8);
    dram_store_u16(hle, reinterpret_cast<uint16_t*>(lutt5), lut_address[1], 8);

    // Load 8-sample history from DRAM
    int16_t hist[8];
    dram_load_u16(hle, reinterpret_cast<uint16_t*>(hist), address, 8);

    int16_t* in1 = hist;
    int16_t* in2 = reinterpret_cast<int16_t*>(buf + (dmem & 0xFFF));
    int16_t outbuff[0x3c0];
    int16_t* outp = outbuff;

    for (int x = 0; x < count; x += 16) {
        // 8-tap FIR filter per block of 8 samples.
        // out[j] = sum_{k=0}^{7} combined[j+1+k] * coeff[7-k]
        // where combined[0..7] = in1 (history), combined[8..15] = in2 (current).
        for (int j = 0; j < 8; ++j) {
            int32_t acc = 0;
            for (int k = 0; k < 8; ++k) {
                int idx = j + 1 + k;
                int16_t s = (idx < 8) ? in1[idx] : in2[idx - 8];
                acc += static_cast<int32_t>(s) * static_cast<int32_t>(lutt6[7 - k]);
            }
            outp[j] = static_cast<int16_t>((acc + 0x4000) >> 15);
        }
        in1 = in2;
        in2 += 8;
        outp += 8;
    }

    // Save last input block as new history to DRAM
    dram_store_u16(hle, reinterpret_cast<uint16_t*>(in2 - 8), address, 8);

    // Copy filtered output back to DMEM
    std::memcpy(buf + (dmem & 0xFFF), outbuff, count);
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
