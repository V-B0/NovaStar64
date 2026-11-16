/**
 * @file audio.cpp
 * @brief RSP HLE Audio ADPCM/Resample Implementation.
 *
 * Derived from Project64 (https://github.com/project64/project64), whose RSP HLE code in turn
 * comes from mupen64plus-rsp-hle (https://github.com/mupen64plus/mupen64plus-rsp-hle),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 *
 * Copyright(C) 2001-2021 Project64
 * Copyright(C) 2014 Bobby Smiles
 */

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
    (int16_t)0x0c39, (int16_t)0x66ad, (int16_t)0x0d46, (int16_t)0xffdf, (int16_t)0x0b39, (int16_t)0x6696,
    (int16_t)0x0e5f, (int16_t)0xffd8, (int16_t)0x0a44, (int16_t)0x6669, (int16_t)0x0f83, (int16_t)0xffd0,
    (int16_t)0x095a, (int16_t)0x6626, (int16_t)0x10b4, (int16_t)0xffc8, (int16_t)0x087d, (int16_t)0x65cd,
    (int16_t)0x11f0, (int16_t)0xffbf, (int16_t)0x07ab, (int16_t)0x655e, (int16_t)0x1338, (int16_t)0xffb6,
    (int16_t)0x06e4, (int16_t)0x64d9, (int16_t)0x148c, (int16_t)0xffac, (int16_t)0x0628, (int16_t)0x643f,
    (int16_t)0x15eb, (int16_t)0xffa1, (int16_t)0x0577, (int16_t)0x638f, (int16_t)0x1756, (int16_t)0xff96,
    (int16_t)0x04d1, (int16_t)0x62cb, (int16_t)0x18cb, (int16_t)0xff8a, (int16_t)0x0435, (int16_t)0x61f3,
    (int16_t)0x1a4c, (int16_t)0xff7e, (int16_t)0x03a4, (int16_t)0x6106, (int16_t)0x1bd7, (int16_t)0xff71,
    (int16_t)0x031c, (int16_t)0x6007, (int16_t)0x1d6c, (int16_t)0xff64, (int16_t)0x029f, (int16_t)0x5ef5,
    (int16_t)0x1f0b, (int16_t)0xff56, (int16_t)0x022a, (int16_t)0x5dd0, (int16_t)0x20b3, (int16_t)0xff48,
    (int16_t)0x01be, (int16_t)0x5c9a, (int16_t)0x2264, (int16_t)0xff3a, (int16_t)0x015b, (int16_t)0x5b53,
    (int16_t)0x241e, (int16_t)0xff2c, (int16_t)0x0101, (int16_t)0x59fc, (int16_t)0x25e0, (int16_t)0xff1e,
    (int16_t)0x00ae, (int16_t)0x5896, (int16_t)0x27a9, (int16_t)0xff10, (int16_t)0x0063, (int16_t)0x5720,
    (int16_t)0x297a, (int16_t)0xff02, (int16_t)0x001f, (int16_t)0x559d, (int16_t)0x2b50, (int16_t)0xfef4,
    (int16_t)0xffe2, (int16_t)0x540d, (int16_t)0x2d2c, (int16_t)0xfee8, (int16_t)0xffac, (int16_t)0x5270,
    (int16_t)0x2f0d, (int16_t)0xfedb, (int16_t)0xff7c, (int16_t)0x50c7, (int16_t)0x30f3, (int16_t)0xfed0,
    (int16_t)0xff53, (int16_t)0x4f14, (int16_t)0x32dc, (int16_t)0xfec6, (int16_t)0xff2e, (int16_t)0x4d57,
    (int16_t)0x34c8, (int16_t)0xfebd, (int16_t)0xff0f, (int16_t)0x4b91, (int16_t)0x36b6, (int16_t)0xfeb6,
    (int16_t)0xfef5, (int16_t)0x49c2, (int16_t)0x38a5, (int16_t)0xfeb0, (int16_t)0xfedf, (int16_t)0x47ed,
    (int16_t)0x3a95, (int16_t)0xfeac, (int16_t)0xfece, (int16_t)0x4611, (int16_t)0x3c85, (int16_t)0xfeab,
    (int16_t)0xfec0, (int16_t)0x4430, (int16_t)0x3e74, (int16_t)0xfeac, (int16_t)0xfeb6, (int16_t)0x424a,
    (int16_t)0x4060, (int16_t)0xfeaf, (int16_t)0xfeaf, (int16_t)0x4060, (int16_t)0x424a, (int16_t)0xfeb6,
    (int16_t)0xfeac, (int16_t)0x3e74, (int16_t)0x4430, (int16_t)0xfec0, (int16_t)0xfeab, (int16_t)0x3c85,
    (int16_t)0x4611, (int16_t)0xfece, (int16_t)0xfeac, (int16_t)0x3a95, (int16_t)0x47ed, (int16_t)0xfedf,
    (int16_t)0xfeb0, (int16_t)0x38a5, (int16_t)0x49c2, (int16_t)0xfef5, (int16_t)0xfeb6, (int16_t)0x36b6,
    (int16_t)0x4b91, (int16_t)0xff0f, (int16_t)0xfebd, (int16_t)0x34c8, (int16_t)0x4d57, (int16_t)0xff2e,
    (int16_t)0xfec6, (int16_t)0x32dc, (int16_t)0x4f14, (int16_t)0xff53, (int16_t)0xfed0, (int16_t)0x30f3,
    (int16_t)0x50c7, (int16_t)0xff7c, (int16_t)0xfedb, (int16_t)0x2f0d, (int16_t)0x5270, (int16_t)0xffac,
    (int16_t)0xfee8, (int16_t)0x2d2c, (int16_t)0x540d, (int16_t)0xffe2, (int16_t)0xfef4, (int16_t)0x2b50,
    (int16_t)0x559d, (int16_t)0x001f, (int16_t)0xff02, (int16_t)0x297a, (int16_t)0x5720, (int16_t)0x0063,
    (int16_t)0xff10, (int16_t)0x27a9, (int16_t)0x5896, (int16_t)0x00ae, (int16_t)0xff1e, (int16_t)0x25e0,
    (int16_t)0x59fc, (int16_t)0x0101, (int16_t)0xff2c, (int16_t)0x241e, (int16_t)0x5b53, (int16_t)0x015b,
    (int16_t)0xff3a, (int16_t)0x2264, (int16_t)0x5c9a, (int16_t)0x01be, (int16_t)0xff48, (int16_t)0x20b3,
    (int16_t)0x5dd0, (int16_t)0x022a, (int16_t)0xff56, (int16_t)0x1f0b, (int16_t)0x5ef5, (int16_t)0x029f,
    (int16_t)0xff64, (int16_t)0x1d6c, (int16_t)0x6007, (int16_t)0x031c, (int16_t)0xff71, (int16_t)0x1bd7,
    (int16_t)0x6106, (int16_t)0x03a4, (int16_t)0xff7e, (int16_t)0x1a4c, (int16_t)0x61f3, (int16_t)0x0435,
    (int16_t)0xff8a, (int16_t)0x18cb, (int16_t)0x62cb, (int16_t)0x04d1, (int16_t)0xff96, (int16_t)0x1756,
    (int16_t)0x638f, (int16_t)0x0577, (int16_t)0xffa1, (int16_t)0x15eb, (int16_t)0x643f, (int16_t)0x0628,
    (int16_t)0xffac, (int16_t)0x148c, (int16_t)0x64d9, (int16_t)0x06e4, (int16_t)0xffb6, (int16_t)0x1338,
    (int16_t)0x655e, (int16_t)0x07ab, (int16_t)0xffbf, (int16_t)0x11f0, (int16_t)0x65cd, (int16_t)0x087d,
    (int16_t)0xffc8, (int16_t)0x10b4, (int16_t)0x6626, (int16_t)0x095a, (int16_t)0xffd0, (int16_t)0x0f83,
    (int16_t)0x6669, (int16_t)0x0a44, (int16_t)0xffd8, (int16_t)0x0e5f, (int16_t)0x6696, (int16_t)0x0b39,
    (int16_t)0xffdf, (int16_t)0x0d46, (int16_t)0x66ad, (int16_t)0x0c39,
};

// ──────────────────────────────────────────────
// Dot product for audio filtering
// Matches mupen64plus: y += n, then x++ * --y
// ──────────────────────────────────────────────

int32_t rdot(size_t n, const int16_t* x, const int16_t* y) {
    int32_t accu = 0;
    y += n;
    while (n != 0) {
        accu += *(x++) * *(--y);
        --n;
    }
    return accu;
}

// ──────────────────────────────────────────────
// ADPCM residual computation
// Matches mupen64plus exactly:
//   accu = src[i] << 11
//   accu += book1[i]*l1 + book2[i]*l2 + rdot(i, book2, src)
//   dst[i] = clamp_s16(accu >> 11)
// ──────────────────────────────────────────────

void adpcm_compute_residuals(int16_t* dst, const int16_t* src, const int16_t* cb_entry, const int16_t* last_samples,
                             size_t count) {
    const int16_t* const book1 = cb_entry;
    const int16_t* const book2 = cb_entry + 8;

    const int16_t l1 = last_samples[0];
    const int16_t l2 = last_samples[1];

    for (size_t i = 0; i < count; ++i) {
        int32_t accu = static_cast<int32_t>(src[i]) << 11;
        accu += book1[i] * l1 + book2[i] * l2 + rdot(i, book2, src);
        dst[i] = clamp_s16(accu >> 11);
    }
}

} // namespace Nova64::RSP::HLE
