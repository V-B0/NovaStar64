// SuperNova64 - N64 Emulator
// RSP HLE Audio ADPCM/Resample Implementation
// Resample lookup table + ADPCM residual computation

#include "audio.hpp"
#include "arithmetics.hpp"
#include <cstddef>

namespace Nova64::RSP::HLE {

// ──────────────────────────────────────────────
// Resample LUT
// 64 entries × 4 taps = 256 int16_t values
// Windowed-sinc interpolation coefficients for
// fractional-sample audio resampling
// ──────────────────────────────────────────────

const int16_t RESAMPLE_LUT[64 * 4] = {
    // frac=0
    0x0000, 0x0000, 0x7FFF, 0x0000,
    // frac=1
    0xFFF1, 0x0033, 0x7FE4, 0xFFF8,
    // frac=2
    0xFFE4, 0x0064, 0x7FBB, 0xFFEE,
    // frac=3
    0xFFD8, 0x0093, 0x7F85, 0xFFE1,
    // frac=4
    0xFFCE, 0x00C0, 0x7F41, 0xFFD2,
    // frac=5
    0xFFC5, 0x00EB, 0x7EF0, 0xFFC1,
    // frac=6
    0xFFBE, 0x0114, 0x7E92, 0xFFAE,
    // frac=7
    0xFFB8, 0x013B, 0x7E27, 0xFF99,
    // frac=8
    0xFFB3, 0x0160, 0x7DB0, 0xFF82,
    // frac=9
    0xFFB0, 0x0183, 0x7D2D, 0xFF69,
    // frac=10
    0xFFAE, 0x01A4, 0x7C9D, 0xFF4F,
    // frac=11
    0xFFAD, 0x01C4, 0x7C02, 0xFF33,
    // frac=12
    0xFFAD, 0x01E1, 0x7B5A, 0xFF16,
    // frac=13
    0xFFAF, 0x01FC, 0x7AA7, 0xFEF7,
    // frac=14
    0xFFB1, 0x0216, 0x79E9, 0xFED7,
    // frac=15
    0xFFB5, 0x022D, 0x7920, 0xFEB6,
    // frac=16
    0xFFBA, 0x0243, 0x784C, 0xFE93,
    // frac=17
    0xFFC0, 0x0256, 0x776E, 0xFE6F,
    // frac=18
    0xFFC7, 0x0268, 0x7685, 0xFE4B,
    // frac=19
    0xFFCF, 0x0278, 0x7592, 0xFE25,
    // frac=20
    0xFFD8, 0x0286, 0x7496, 0xFDFF,
    // frac=21
    0xFFE2, 0x0292, 0x7390, 0xFDD7,
    // frac=22
    0xFFED, 0x029D, 0x7282, 0xFDAF,
    // frac=23
    0xFFF9, 0x02A5, 0x716C, 0xFD87,
    // frac=24
    0x0005, 0x02AC, 0x704D, 0xFD5D,
    // frac=25
    0x0013, 0x02B1, 0x6F27, 0xFD33,
    // frac=26
    0x0021, 0x02B4, 0x6DFA, 0xFD09,
    // frac=27
    0x002F, 0x02B5, 0x6CC6, 0xFCDE,
    // frac=28
    0x003E, 0x02B5, 0x6B8C, 0xFCB3,
    // frac=29
    0x004E, 0x02B3, 0x6A4C, 0xFC88,
    // frac=30
    0x005F, 0x02AF, 0x6907, 0xFC5C,
    // frac=31
    0x006F, 0x02A9, 0x67BD, 0xFC31,
    // frac=32 (midpoint)
    0x0080, 0x02A3, 0x666F, 0xFC06,
    // frac=33
    0x0091, 0x029A, 0x651C, 0xFBDB,
    // frac=34
    0x00A3, 0x0290, 0x63C6, 0xFBB0,
    // frac=35
    0x00B4, 0x0284, 0x626C, 0xFB85,
    // frac=36
    0x00C6, 0x0277, 0x610F, 0xFB5B,
    // frac=37
    0x00D8, 0x0268, 0x5FB0, 0xFB31,
    // frac=38
    0x00EA, 0x0258, 0x5E4F, 0xFB07,
    // frac=39
    0x00FC, 0x0247, 0x5CEC, 0xFADE,
    // frac=40
    0x010D, 0x0234, 0x5B87, 0xFAB5,
    // frac=41
    0x011F, 0x0220, 0x5A22, 0xFA8D,
    // frac=42
    0x0130, 0x020B, 0x58BC, 0xFA66,
    // frac=43
    0x0141, 0x01F5, 0x5756, 0xFA3F,
    // frac=44
    0x0151, 0x01DE, 0x55F0, 0xFA19,
    // frac=45
    0x0161, 0x01C5, 0x548A, 0xF9F3,
    // frac=46
    0x0170, 0x01AC, 0x5325, 0xF9CE,
    // frac=47
    0x017F, 0x0192, 0x51C1, 0xF9AB,
    // frac=48
    0x018D, 0x0177, 0x505F, 0xF988,
    // frac=49
    0x019A, 0x015C, 0x4EFF, 0xF966,
    // frac=50
    0x01A6, 0x0140, 0x4DA1, 0xF945,
    // frac=51
    0x01B2, 0x0123, 0x4C46, 0xF925,
    // frac=52
    0x01BC, 0x0106, 0x4AEE, 0xF906,
    // frac=53
    0x01C5, 0x00E9, 0x499A, 0xF8E8,
    // frac=54
    0x01CD, 0x00CB, 0x4849, 0xF8CB,
    // frac=55
    0x01D4, 0x00AE, 0x46FD, 0xF8B0,
    // frac=56
    0x01D9, 0x0090, 0x45B5, 0xF896,
    // frac=57
    0x01DD, 0x0072, 0x4473, 0xF87D,
    // frac=58
    0x01DF, 0x0055, 0x4336, 0xF866,
    // frac=59
    0x01E0, 0x0038, 0x41FE, 0xF850,
    // frac=60
    0x01DF, 0x001B, 0x40CC, 0xF83C,
    // frac=61
    0x01DC, 0x0000, 0x3FA1, 0xF829,
    // frac=62
    0x01D7, 0xFFE5, 0x3E7B, 0xF818,
    // frac=63
    0x01D1, 0xFFCC, 0x3D5D, 0xF809,
};

// ──────────────────────────────────────────────
// Dot product for audio filtering
// Computes sum(x[i] * y[i]) >> 15 for n pairs
// ──────────────────────────────────────────────

int32_t rdot(size_t n, const int16_t* x, const int16_t* y)
{
    int64_t acc = 0;
    for (size_t i = 0; i < n; i++)
    {
        acc += static_cast<int32_t>(x[i]) * static_cast<int32_t>(y[i]);
    }
    return static_cast<int32_t>(acc >> 15);
}

// ──────────────────────────────────────────────
// ADPCM residual computation
// Applies prediction filter from codebook entry
// to decoded samples, producing final PCM output
// ──────────────────────────────────────────────

void adpcm_compute_residuals(
    int16_t* dst,
    const int16_t* src,
    const int16_t* cb_entry,
    const int16_t* last_samples,
    size_t count)
{
    // N64 ADPCM uses a dual 8-coefficient predictor stored in the codebook.
    // For predictor index p, the codebook entry has:
    //   book1 = cb_entry[0..7]  — weights for the -2 history sample
    //   book2 = cb_entry[8..15] — weights for the -1 history sample + within-block prediction
    //
    // Frames are processed in two half-blocks of 8 samples each.
    // Within each half-block, already-decoded samples contribute to prediction
    // of subsequent samples via book2 coefficients.

    const int16_t* book1 = cb_entry;        // 8 coefficients
    const int16_t* book2 = cb_entry + 8;    // 8 coefficients

    // History: last 2 samples from previous frame
    int16_t l2 = (count >= 16) ? last_samples[14] : (count >= 2 ? last_samples[count - 2] : 0);
    int16_t l1 = (count >= 16) ? last_samples[15] : (count >= 1 ? last_samples[count - 1] : 0);

    // First half-block (up to 8 samples)
    const size_t half = (count > 8) ? 8 : count;

    for (size_t i = 0; i < half; i++)
    {
        int32_t acc = static_cast<int32_t>(l2) * book1[i];
        acc += static_cast<int32_t>(l1) * book2[i];

        // Within-block prediction: already-decoded samples from this half-block
        for (size_t j = 0; j < i; j++)
        {
            acc += static_cast<int32_t>(dst[j]) * book2[i - 1 - j];
        }

        acc >>= 11;
        dst[i] = clamp_s16(acc + src[i]);
    }

    if (count <= 8)
        return;

    // Second half-block: uses last 2 samples from first half as history
    l2 = dst[half - 2];
    l1 = dst[half - 1];

    const size_t second_half = (count > 16) ? 8 : (count - 8);

    for (size_t i = 0; i < second_half; i++)
    {
        int32_t acc = static_cast<int32_t>(l2) * book1[i];
        acc += static_cast<int32_t>(l1) * book2[i];

        // Within-block prediction from this half-block's decoded samples
        for (size_t j = 0; j < i; j++)
        {
            acc += static_cast<int32_t>(dst[half + j]) * book2[i - 1 - j];
        }

        acc >>= 11;
        dst[half + i] = clamp_s16(acc + src[half + i]);
    }
}

} // namespace Nova64::RSP::HLE
