/**
 * @file SIMDMath.cpp
 * @brief SIMD math implementations declared in SIMDMath.hpp: scalar, SSE2, SSE4.1, AVX, AVX2 and the dispatch tables.
 */
// SPDX-License-Identifier: MIT

#include "SIMDMath.hpp"
#include "RDRAMAccess.hpp"
#include <cassert>
#include <cstring>

namespace Nova64 {
namespace Video {
namespace SIMD {

// --- Global runtime SIMD level ---
SIMDLevel g_SIMDLevel = SIMDLevel::Scalar;

// --- Detection ---
SIMDLevel DetectSIMDLevel() {
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)
    int cpuInfo[4] = {0};

// Basic CPUID
#if defined(_MSC_VER)
    __cpuid(cpuInfo, 0);
#else
    __asm__ __volatile__("cpuid" : "=a"(cpuInfo[0]), "=b"(cpuInfo[1]), "=c"(cpuInfo[2]), "=d"(cpuInfo[3]) : "a"(0));
#endif

    int maxFunc = cpuInfo[0];
    if (maxFunc < 1) return SIMDLevel::Scalar;

#if defined(_MSC_VER)
    __cpuid(cpuInfo, 1);
#else
    __asm__ __volatile__("cpuid" : "=a"(cpuInfo[0]), "=b"(cpuInfo[1]), "=c"(cpuInfo[2]), "=d"(cpuInfo[3]) : "a"(1));
#endif

    bool hasSSE2 = (cpuInfo[3] & (1 << 26)) != 0;
    bool hasSSE41 = (cpuInfo[2] & (1 << 19)) != 0;
    bool hasAVX = (cpuInfo[2] & (1 << 28)) != 0;
    bool hasOSXSAVE = (cpuInfo[2] & (1 << 27)) != 0;
    bool hasFMA = (cpuInfo[2] & (1 << 12)) != 0;

    if (!hasSSE2) return SIMDLevel::Scalar;

    // Check OS supports AVX state saving
    bool osAVX = false;
    if (hasOSXSAVE) {
#if defined(_MSC_VER)
        unsigned long long xcr0 = _xgetbv(0);
#else
        unsigned int eax, edx;
        __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
        unsigned long long xcr0 = ((unsigned long long)edx << 32) | eax;
#endif
        osAVX = (xcr0 & 0x6) == 0x6; // XMM + YMM state
    }

    bool hasAVX2 = false;
    bool hasAVX512F = false;
    if (maxFunc >= 7) {
#if defined(_MSC_VER)
        __cpuidex(cpuInfo, 7, 0);
#else
        __asm__ __volatile__("cpuid"
                             : "=a"(cpuInfo[0]), "=b"(cpuInfo[1]), "=c"(cpuInfo[2]), "=d"(cpuInfo[3])
                             : "a"(7), "c"(0));
#endif
        hasAVX2 = (cpuInfo[1] & (1 << 5)) != 0;
        hasAVX512F = (cpuInfo[1] & (1 << 16)) != 0;
    }

    // Check for AVX-512 OS support
    bool osAVX512 = false;
    if (hasOSXSAVE && hasAVX512F) {
#if defined(_MSC_VER)
        unsigned long long xcr0 = _xgetbv(0);
#else
        unsigned int eax, edx;
        __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
        unsigned long long xcr0 = ((unsigned long long)edx << 32) | eax;
#endif
        osAVX512 = (xcr0 & 0xE6) == 0xE6; // ZMM state
    }

    if (osAVX512 && hasAVX512F) return SIMDLevel::AVX512;
    if (osAVX && hasAVX2 && hasFMA) return SIMDLevel::AVX2;
    if (osAVX && hasAVX) return SIMDLevel::AVX;
    if (hasSSE41) return SIMDLevel::SSE41;
    return SIMDLevel::SSE2;

#elif defined(__aarch64__)
    // ARM NEON is always available on AArch64
    return SIMDLevel::SSE2; // Map to SSE2 equivalent
#else
    return SIMDLevel::Scalar;
#endif
}

// --- Mat4x4 static methods ---
Mat4x4 Mat4x4::Identity() { return Mat4x4(); }

Mat4x4 Mat4x4::Zero() {
    Mat4x4 m;
    m.col[0] = Vec4f(0.0f);
    m.col[1] = Vec4f(0.0f);
    m.col[2] = Vec4f(0.0f);
    m.col[3] = Vec4f(0.0f);
    return m;
}

// --- SCALAR IMPLEMENTATIONS ---
namespace Scalar {

Vec4f Add(const Vec4f& a, const Vec4f& b) { return Vec4f(a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]); }

Vec4f Sub(const Vec4f& a, const Vec4f& b) { return Vec4f(a[0] - b[0], a[1] - b[1], a[2] - b[2], a[3] - b[3]); }

Vec4f Mul(const Vec4f& a, const Vec4f& b) { return Vec4f(a[0] * b[0], a[1] * b[1], a[2] * b[2], a[3] * b[3]); }

Vec4f Div(const Vec4f& a, const Vec4f& b) {
    return Vec4f(b[0] != 0.0f ? a[0] / b[0] : 0.0f, b[1] != 0.0f ? a[1] / b[1] : 0.0f,
                 b[2] != 0.0f ? a[2] / b[2] : 0.0f, b[3] != 0.0f ? a[3] / b[3] : 0.0f);
}

Vec4f MulScalar(const Vec4f& a, float s) { return Vec4f(a[0] * s, a[1] * s, a[2] * s, a[3] * s); }

Vec4f FMA(const Vec4f& a, const Vec4f& b, const Vec4f& c) {
    return Vec4f(a[0] * b[0] + c[0], a[1] * b[1] + c[1], a[2] * b[2] + c[2], a[3] * b[3] + c[3]);
}

float Dot3(const Vec4f& a, const Vec4f& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

float Dot4(const Vec4f& a, const Vec4f& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; }

Vec4f Cross3(const Vec4f& a, const Vec4f& b) {
    return Vec4f(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0], 0.0f);
}

float Length3(const Vec4f& v) { return std::sqrt(Dot3(v, v)); }

float Length4(const Vec4f& v) { return std::sqrt(Dot4(v, v)); }

Vec4f Normalize3(const Vec4f& v) {
    float len = Length3(v);
    if (len < 1e-10f) return Vec4f(0.0f);
    float inv = 1.0f / len;
    return Vec4f(v[0] * inv, v[1] * inv, v[2] * inv, v[3]);
}

Vec4f Normalize4(const Vec4f& v) {
    float len = Length4(v);
    if (len < 1e-10f) return Vec4f(0.0f);
    float inv = 1.0f / len;
    return Vec4f(v[0] * inv, v[1] * inv, v[2] * inv, v[3] * inv);
}

Vec4f Lerp(const Vec4f& a, const Vec4f& b, float t) {
    float ot = 1.0f - t;
    return Vec4f(a[0] * ot + b[0] * t, a[1] * ot + b[1] * t, a[2] * ot + b[2] * t, a[3] * ot + b[3] * t);
}

Vec4f Min(const Vec4f& a, const Vec4f& b) {
    return Vec4f(std::min(a[0], b[0]), std::min(a[1], b[1]), std::min(a[2], b[2]), std::min(a[3], b[3]));
}

Vec4f Max(const Vec4f& a, const Vec4f& b) {
    return Vec4f(std::max(a[0], b[0]), std::max(a[1], b[1]), std::max(a[2], b[2]), std::max(a[3], b[3]));
}

Vec4f Clamp(const Vec4f& v, const Vec4f& lo, const Vec4f& hi) { return Min(Max(v, lo), hi); }

Vec4f Abs(const Vec4f& v) { return Vec4f(std::fabs(v[0]), std::fabs(v[1]), std::fabs(v[2]), std::fabs(v[3])); }

Vec4f Negate(const Vec4f& v) { return Vec4f(-v[0], -v[1], -v[2], -v[3]); }

Vec4f Reciprocal(const Vec4f& v) {
    return Vec4f(v[0] != 0.0f ? 1.0f / v[0] : 0.0f, v[1] != 0.0f ? 1.0f / v[1] : 0.0f,
                 v[2] != 0.0f ? 1.0f / v[2] : 0.0f, v[3] != 0.0f ? 1.0f / v[3] : 0.0f);
}

Vec4f ReciprocalSqrt(const Vec4f& v) {
    return Vec4f(v[0] > 0.0f ? 1.0f / std::sqrt(v[0]) : 0.0f, v[1] > 0.0f ? 1.0f / std::sqrt(v[1]) : 0.0f,
                 v[2] > 0.0f ? 1.0f / std::sqrt(v[2]) : 0.0f, v[3] > 0.0f ? 1.0f / std::sqrt(v[3]) : 0.0f);
}

Vec4f Floor(const Vec4f& v) { return Vec4f(std::floor(v[0]), std::floor(v[1]), std::floor(v[2]), std::floor(v[3])); }

Vec4f Ceil(const Vec4f& v) { return Vec4f(std::ceil(v[0]), std::ceil(v[1]), std::ceil(v[2]), std::ceil(v[3])); }

Vec4f Round(const Vec4f& v) { return Vec4f(std::round(v[0]), std::round(v[1]), std::round(v[2]), std::round(v[3])); }

// --- Scalar Matrix Operations ---
Mat4x4 MatMul(const Mat4x4& a, const Mat4x4& b) {
    Mat4x4 r = Mat4x4::Zero();
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a(row, k) * b(k, c);
            }
            r(row, c) = sum;
        }
    }
    return r;
}

Vec4f MatVecMul(const Mat4x4& m, const Vec4f& v) {
    return Vec4f(m(0, 0) * v[0] + m(0, 1) * v[1] + m(0, 2) * v[2] + m(0, 3) * v[3],
                 m(1, 0) * v[0] + m(1, 1) * v[1] + m(1, 2) * v[2] + m(1, 3) * v[3],
                 m(2, 0) * v[0] + m(2, 1) * v[1] + m(2, 2) * v[2] + m(2, 3) * v[3],
                 m(3, 0) * v[0] + m(3, 1) * v[1] + m(3, 2) * v[2] + m(3, 3) * v[3]);
}

Mat4x4 MatTranspose(const Mat4x4& m) {
    Mat4x4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r(i, j) = m(j, i);
    return r;
}

// --- Scalar Batch Operations ---
void TransformVertices(const Mat4x4& mvp, const float* posIn, float* posOut, int count) {
    for (int i = 0; i < count; i++) {
        Vec4f v(posIn[i * 4], posIn[i * 4 + 1], posIn[i * 4 + 2], posIn[i * 4 + 3]);
        Vec4f r = MatVecMul(mvp, v);
        posOut[i * 4] = r[0];
        posOut[i * 4 + 1] = r[1];
        posOut[i * 4 + 2] = r[2];
        posOut[i * 4 + 3] = r[3];
    }
}

void TransformNormals(const Mat4x4& normalMat, const float* normIn, float* normOut, int count) {
    for (int i = 0; i < count; i++) {
        Vec4f n(normIn[i * 3], normIn[i * 3 + 1], normIn[i * 3 + 2], 0.0f);
        Vec4f r = MatVecMul(normalMat, n);
        float len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (len > 1e-10f) {
            float inv = 1.0f / len;
            normOut[i * 3] = r[0] * inv;
            normOut[i * 3 + 1] = r[1] * inv;
            normOut[i * 3 + 2] = r[2] * inv;
        } else {
            normOut[i * 3] = normOut[i * 3 + 1] = normOut[i * 3 + 2] = 0.0f;
        }
    }
}

void BatchLerp(const float* a, const float* b, float* out, float t, int count) {
    float ot = 1.0f - t;
    for (int i = 0; i < count; i++) {
        out[i] = a[i] * ot + b[i] * t;
    }
}

// --- Scalar Integer Operations ---
Vec4i AddI(const Vec4i& a, const Vec4i& b) { return Vec4i(a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]); }

Vec4i SubI(const Vec4i& a, const Vec4i& b) { return Vec4i(a[0] - b[0], a[1] - b[1], a[2] - b[2], a[3] - b[3]); }

Vec4i MulLoI(const Vec4i& a, const Vec4i& b) { return Vec4i(a[0] * b[0], a[1] * b[1], a[2] * b[2], a[3] * b[3]); }

Vec4i ShiftLeftI(const Vec4i& a, int bits) { return Vec4i(a[0] << bits, a[1] << bits, a[2] << bits, a[3] << bits); }

Vec4i ShiftRightI(const Vec4i& a, int bits) { return Vec4i(a[0] >> bits, a[1] >> bits, a[2] >> bits, a[3] >> bits); }

Vec4i ShiftRightLogI(const Vec4i& a, int bits) {
    return Vec4i((int32_t)((uint32_t)a[0] >> bits), (int32_t)((uint32_t)a[1] >> bits),
                 (int32_t)((uint32_t)a[2] >> bits), (int32_t)((uint32_t)a[3] >> bits));
}

Vec4i AndI(const Vec4i& a, const Vec4i& b) { return Vec4i(a[0] & b[0], a[1] & b[1], a[2] & b[2], a[3] & b[3]); }

Vec4i OrI(const Vec4i& a, const Vec4i& b) { return Vec4i(a[0] | b[0], a[1] | b[1], a[2] | b[2], a[3] | b[3]); }

Vec4i XorI(const Vec4i& a, const Vec4i& b) { return Vec4i(a[0] ^ b[0], a[1] ^ b[1], a[2] ^ b[2], a[3] ^ b[3]); }

Vec4i MinI(const Vec4i& a, const Vec4i& b) {
    return Vec4i(std::min(a[0], b[0]), std::min(a[1], b[1]), std::min(a[2], b[2]), std::min(a[3], b[3]));
}

Vec4i MaxI(const Vec4i& a, const Vec4i& b) {
    return Vec4i(std::max(a[0], b[0]), std::max(a[1], b[1]), std::max(a[2], b[2]), std::max(a[3], b[3]));
}

} // namespace Scalar

// --- SSE2 IMPLEMENTATIONS ---
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
namespace SSE2 {

Vec4f FMA(const Vec4f& a, const Vec4f& b, const Vec4f& c) {
    // SSE2 has no FMA instruction, so this is a separate multiply and add.
    return _mm_add_ps(_mm_mul_ps(a.v, b.v), c.v);
}

float Dot3(const Vec4f& a, const Vec4f& b) {
    __m128 prod = _mm_mul_ps(a.v, b.v);
    // Horizontal add: x*x + y*y + z*z
    __m128 shuf1 = _mm_shuffle_ps(prod, prod, _MM_SHUFFLE(0, 0, 0, 1));
    __m128 shuf2 = _mm_shuffle_ps(prod, prod, _MM_SHUFFLE(0, 0, 0, 2));
    __m128 sum = _mm_add_ss(prod, shuf1);
    sum = _mm_add_ss(sum, shuf2);
    return _mm_cvtss_f32(sum);
}

float Dot4(const Vec4f& a, const Vec4f& b) {
    __m128 prod = _mm_mul_ps(a.v, b.v);
    __m128 shuf = _mm_shuffle_ps(prod, prod, _MM_SHUFFLE(2, 3, 0, 1));
    __m128 sums = _mm_add_ps(prod, shuf);
    shuf = _mm_movehl_ps(shuf, sums);
    sums = _mm_add_ss(sums, shuf);
    return _mm_cvtss_f32(sums);
}

Vec4f Cross3(const Vec4f& a, const Vec4f& b) {
    // a.yzx * b.zxy - a.zxy * b.yzx
    __m128 a_yzx = _mm_shuffle_ps(a.v, a.v, _MM_SHUFFLE(3, 0, 2, 1));
    __m128 b_zxy = _mm_shuffle_ps(b.v, b.v, _MM_SHUFFLE(3, 1, 0, 2));
    __m128 a_zxy = _mm_shuffle_ps(a.v, a.v, _MM_SHUFFLE(3, 1, 0, 2));
    __m128 b_yzx = _mm_shuffle_ps(b.v, b.v, _MM_SHUFFLE(3, 0, 2, 1));
    return _mm_sub_ps(_mm_mul_ps(a_yzx, b_zxy), _mm_mul_ps(a_zxy, b_yzx));
}

float Length3(const Vec4f& v) { return std::sqrt(Dot3(v, v)); }

float Length4(const Vec4f& v) { return std::sqrt(Dot4(v, v)); }

Vec4f Normalize3(const Vec4f& v) {
    float d = Dot3(v, v);
    if (d < 1e-20f) return Vec4f(0.0f);
    __m128 len = _mm_set1_ps(1.0f / std::sqrt(d));
    // Zero out w component
    __m128 mask = _mm_castsi128_ps(_mm_set_epi32(0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF));
    return _mm_and_ps(_mm_mul_ps(v.v, len), mask);
}

Vec4f Normalize4(const Vec4f& v) {
    float d = Dot4(v, v);
    if (d < 1e-20f) return Vec4f(0.0f);
    return _mm_mul_ps(v.v, _mm_set1_ps(1.0f / std::sqrt(d)));
}

Vec4f Lerp(const Vec4f& a, const Vec4f& b, float t) {
    __m128 vt = _mm_set1_ps(t);
    __m128 ot = _mm_set1_ps(1.0f - t);
    return _mm_add_ps(_mm_mul_ps(a.v, ot), _mm_mul_ps(b.v, vt));
}

Vec4f Min(const Vec4f& a, const Vec4f& b) { return _mm_min_ps(a.v, b.v); }
Vec4f Max(const Vec4f& a, const Vec4f& b) { return _mm_max_ps(a.v, b.v); }

Vec4f Clamp(const Vec4f& v, const Vec4f& lo, const Vec4f& hi) { return _mm_min_ps(_mm_max_ps(v.v, lo.v), hi.v); }

Vec4f Abs(const Vec4f& v) {
    __m128 signMask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    return _mm_and_ps(v.v, signMask);
}

Vec4f Negate(const Vec4f& v) { return _mm_sub_ps(_mm_setzero_ps(), v.v); }

Vec4f Reciprocal(const Vec4f& v) {
    // rcpps estimate refined with one Newton-Raphson step.
    __m128 est = _mm_rcp_ps(v.v);
    __m128 two = _mm_set1_ps(2.0f);
    // x1 = est * (2 - v * est)
    return _mm_mul_ps(est, _mm_sub_ps(two, _mm_mul_ps(v.v, est)));
}

Vec4f ReciprocalSqrt(const Vec4f& v) {
    // rsqrtps estimate refined with one Newton-Raphson step.
    __m128 est = _mm_rsqrt_ps(v.v);
    __m128 half = _mm_set1_ps(0.5f);
    __m128 three = _mm_set1_ps(3.0f);
    // x1 = 0.5 * est * (3 - v * est * est)
    __m128 estSq = _mm_mul_ps(est, est);
    return _mm_mul_ps(_mm_mul_ps(half, est), _mm_sub_ps(three, _mm_mul_ps(v.v, estSq)));
}

// --- SSE2 Matrix Operations ---
Mat4x4 MatMul(const Mat4x4& a, const Mat4x4& b) {
    Mat4x4 result;
    for (int c = 0; c < 4; c++) {
        // result.col[c] = a.col[0]*b(0,c) + a.col[1]*b(1,c) + a.col[2]*b(2,c) + a.col[3]*b(3,c)
        __m128 bc0 = _mm_set1_ps(b.col[c][0]);
        __m128 bc1 = _mm_set1_ps(b.col[c][1]);
        __m128 bc2 = _mm_set1_ps(b.col[c][2]);
        __m128 bc3 = _mm_set1_ps(b.col[c][3]);
        __m128 r = _mm_mul_ps(a.col[0].v, bc0);
        r = _mm_add_ps(r, _mm_mul_ps(a.col[1].v, bc1));
        r = _mm_add_ps(r, _mm_mul_ps(a.col[2].v, bc2));
        r = _mm_add_ps(r, _mm_mul_ps(a.col[3].v, bc3));
        result.col[c] = r;
    }
    return result;
}

Vec4f MatVecMul(const Mat4x4& m, const Vec4f& v) {
    __m128 vx = _mm_shuffle_ps(v.v, v.v, _MM_SHUFFLE(0, 0, 0, 0));
    __m128 vy = _mm_shuffle_ps(v.v, v.v, _MM_SHUFFLE(1, 1, 1, 1));
    __m128 vz = _mm_shuffle_ps(v.v, v.v, _MM_SHUFFLE(2, 2, 2, 2));
    __m128 vw = _mm_shuffle_ps(v.v, v.v, _MM_SHUFFLE(3, 3, 3, 3));
    __m128 r = _mm_mul_ps(m.col[0].v, vx);
    r = _mm_add_ps(r, _mm_mul_ps(m.col[1].v, vy));
    r = _mm_add_ps(r, _mm_mul_ps(m.col[2].v, vz));
    r = _mm_add_ps(r, _mm_mul_ps(m.col[3].v, vw));
    return r;
}

Mat4x4 MatTranspose(const Mat4x4& m) {
    Mat4x4 r;
    __m128 t0 = _mm_unpacklo_ps(m.col[0].v, m.col[1].v); // a00,b00,a01,b01
    __m128 t1 = _mm_unpackhi_ps(m.col[0].v, m.col[1].v); // a02,b02,a03,b03
    __m128 t2 = _mm_unpacklo_ps(m.col[2].v, m.col[3].v); // c00,d00,c01,d01
    __m128 t3 = _mm_unpackhi_ps(m.col[2].v, m.col[3].v); // c02,d02,c03,d03
    r.col[0] = _mm_movelh_ps(t0, t2);
    r.col[1] = _mm_movehl_ps(t2, t0);
    r.col[2] = _mm_movelh_ps(t1, t3);
    r.col[3] = _mm_movehl_ps(t3, t1);
    return r;
}

// --- SSE2 Batch Operations ---
void TransformVertices(const Mat4x4& mvp, const float* posIn, float* posOut, int count) {
    for (int i = 0; i < count; i++) {
        __m128 v = _mm_loadu_ps(posIn + i * 4);
        __m128 vx = _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
        __m128 vy = _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
        __m128 vz = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));
        __m128 vw = _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3));
        __m128 r = _mm_mul_ps(mvp.col[0].v, vx);
        r = _mm_add_ps(r, _mm_mul_ps(mvp.col[1].v, vy));
        r = _mm_add_ps(r, _mm_mul_ps(mvp.col[2].v, vz));
        r = _mm_add_ps(r, _mm_mul_ps(mvp.col[3].v, vw));
        _mm_storeu_ps(posOut + i * 4, r);
    }
}

void TransformNormals(const Mat4x4& normalMat, const float* normIn, float* normOut, int count) {
    for (int i = 0; i < count; i++) {
        // Load 3 floats (pad w=0)
        __m128 n = _mm_set_ps(0.0f, normIn[i * 3 + 2], normIn[i * 3 + 1], normIn[i * 3]);
        __m128 nx = _mm_shuffle_ps(n, n, _MM_SHUFFLE(0, 0, 0, 0));
        __m128 ny = _mm_shuffle_ps(n, n, _MM_SHUFFLE(1, 1, 1, 1));
        __m128 nz = _mm_shuffle_ps(n, n, _MM_SHUFFLE(2, 2, 2, 2));
        __m128 r = _mm_mul_ps(normalMat.col[0].v, nx);
        r = _mm_add_ps(r, _mm_mul_ps(normalMat.col[1].v, ny));
        r = _mm_add_ps(r, _mm_mul_ps(normalMat.col[2].v, nz));
        __m128 dp = _mm_mul_ps(r, r);
        __m128 shuf1 = _mm_shuffle_ps(dp, dp, _MM_SHUFFLE(0, 0, 0, 1));
        __m128 shuf2 = _mm_shuffle_ps(dp, dp, _MM_SHUFFLE(0, 0, 0, 2));
        __m128 sum = _mm_add_ss(dp, shuf1);
        sum = _mm_add_ss(sum, shuf2);
        float len2 = _mm_cvtss_f32(sum);
        if (len2 > 1e-20f) {
            __m128 inv = _mm_set1_ps(1.0f / std::sqrt(len2));
            r = _mm_mul_ps(r, inv);
        }
        float tmp[4];
        _mm_storeu_ps(tmp, r);
        normOut[i * 3] = tmp[0];
        normOut[i * 3 + 1] = tmp[1];
        normOut[i * 3 + 2] = tmp[2];
    }
}

void BatchLerp(const float* a, const float* b, float* out, float t, int count) {
    __m128 vt = _mm_set1_ps(t);
    __m128 ot = _mm_set1_ps(1.0f - t);
    int i = 0;
    for (; i + 3 < count; i += 4) {
        __m128 va = _mm_loadu_ps(a + i);
        __m128 vb = _mm_loadu_ps(b + i);
        __m128 r = _mm_add_ps(_mm_mul_ps(va, ot), _mm_mul_ps(vb, vt));
        _mm_storeu_ps(out + i, r);
    }
    for (; i < count; i++) {
        out[i] = a[i] * (1.0f - t) + b[i] * t;
    }
}

Vec4i MulLoI(const Vec4i& a, const Vec4i& b) {
    // _mm_mullo_epi32 is SSE4.1; emulate it with two widening multiplies.
    __m128i t0 = _mm_mul_epu32(a.v, b.v);                                       // 0,2
    __m128i t1 = _mm_mul_epu32(_mm_srli_si128(a.v, 4), _mm_srli_si128(b.v, 4)); // 1,3
    __m128i r = _mm_unpacklo_epi32(_mm_shuffle_epi32(t0, _MM_SHUFFLE(0, 0, 2, 0)),
                                   _mm_shuffle_epi32(t1, _MM_SHUFFLE(0, 0, 2, 0)));
    return r;
}

Vec4i WrapTexCoord(const Vec4i& coord, const Vec4i& mask, const Vec4i& mirror, const Vec4i& clampMax) {
    // Per lane: if mirror is set and the bit just above the mask is set, invert; then apply the mask and clamp.
    Vec4i result;
    for (int i = 0; i < 4; i++) {
        int c = coord.i[i];
        int m = mask.i[i];
        int maskBits = (1 << m) - 1;
        int mirrorBit = 1 << m;

        if (mirror.i[i] && (c & mirrorBit)) {
            c = maskBits - (c & maskBits);
        } else {
            c = c & maskBits;
        }

        if (clampMax.i[i] > 0) {
            c = std::min(c, clampMax.i[i]);
            c = std::max(c, 0);
        }
        result.i[i] = c;
    }
    return result;
}

Vec4i PackRGBA5551(const Vec4i& r, const Vec4i& g, const Vec4i& b, const Vec4i& a) {
    // RGBA5551: R(5) G(5) B(5) A(1) = 16 bits.
    __m128i rv = _mm_and_si128(_mm_slli_epi32(r.v, 11), _mm_set1_epi32(0xF800));
    __m128i gv = _mm_and_si128(_mm_slli_epi32(g.v, 6), _mm_set1_epi32(0x07C0));
    __m128i bv = _mm_and_si128(_mm_slli_epi32(b.v, 1), _mm_set1_epi32(0x003E));
    __m128i av = _mm_and_si128(a.v, _mm_set1_epi32(0x0001));
    return _mm_or_si128(_mm_or_si128(rv, gv), _mm_or_si128(bv, av));
}

} // namespace SSE2
#endif

// --- SSE4.1 IMPLEMENTATIONS ---
#if defined(__SSE4_1__) || (defined(_MSC_VER) && defined(__AVX__))
namespace SSE41 {

float Dot3(const Vec4f& a, const Vec4f& b) {
    __m128 dp = _mm_dp_ps(a.v, b.v, 0x71); // mask: x*x + y*y + z*z -> lane 0
    return _mm_cvtss_f32(dp);
}

float Dot4(const Vec4f& a, const Vec4f& b) {
    __m128 dp = _mm_dp_ps(a.v, b.v, 0xF1); // all lanes -> lane 0
    return _mm_cvtss_f32(dp);
}

Vec4f Floor(const Vec4f& v) { return _mm_floor_ps(v.v); }

Vec4f Ceil(const Vec4f& v) { return _mm_ceil_ps(v.v); }

Vec4f Round(const Vec4f& v) { return _mm_round_ps(v.v, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); }

Vec4i MinI(const Vec4i& a, const Vec4i& b) { return _mm_min_epi32(a.v, b.v); }

Vec4i MaxI(const Vec4i& a, const Vec4i& b) { return _mm_max_epi32(a.v, b.v); }

Vec4i MulLoI(const Vec4i& a, const Vec4i& b) { return _mm_mullo_epi32(a.v, b.v); }

Vec4f BlendByMask(const Vec4f& a, const Vec4f& b, const Vec4f& mask) { return _mm_blendv_ps(a.v, b.v, mask.v); }

} // namespace SSE41
#endif

// --- AVX IMPLEMENTATIONS ---
#if defined(__AVX__)
namespace AVX {

Vec8f FMA(const Vec8f& a, const Vec8f& b, const Vec8f& c) {
    // Separate multiply and add (AVX has no FMA).
    Vec8f r;
    r.v = _mm256_add_ps(_mm256_mul_ps(a.v, b.v), c.v);
    return r;
}

void TransformVertices8(const Mat4x4& mvp, const float* posIn, float* posOut, int count) {
    // Broadcast each matrix column into both 128-bit halves.
    __m256 col0 = _mm256_broadcast_ps(&mvp.col[0].v);
    __m256 col1 = _mm256_broadcast_ps(&mvp.col[1].v);
    __m256 col2 = _mm256_broadcast_ps(&mvp.col[2].v);
    __m256 col3 = _mm256_broadcast_ps(&mvp.col[3].v);

    int i = 0;
    for (; i + 1 < count; i += 2) {
        // Two vertices per iteration: [x0,y0,z0,w0, x1,y1,z1,w1].
        __m256 v = _mm256_loadu_ps(posIn + i * 4);

        // Splat each component within its vertex: vx = [x0,x0,x0,x0, x1,x1,x1,x1], etc.
        __m256 vx = _mm256_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
        __m256 vy = _mm256_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
        __m256 vz = _mm256_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));
        __m256 vw = _mm256_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3));

        __m256 r = _mm256_mul_ps(col0, vx);
        r = _mm256_add_ps(r, _mm256_mul_ps(col1, vy));
        r = _mm256_add_ps(r, _mm256_mul_ps(col2, vz));
        r = _mm256_add_ps(r, _mm256_mul_ps(col3, vw));

        _mm256_storeu_ps(posOut + i * 4, r);
    }
    // Odd vertex count: transform the last vertex on its own.
    if (i < count) {
        Vec4f v(posIn[i * 4], posIn[i * 4 + 1], posIn[i * 4 + 2], posIn[i * 4 + 3]);
        Vec4f r = SSE2::MatVecMul(mvp, v);
        std::memcpy(posOut + i * 4, r.f, 16);
    }
}

void BatchEdgeSetup(const float* v0, const float* v1, const float* v2, float* edgeA, float* edgeB, float* edgeC,
                    int triCount) {
    // Edge equation A*x + B*y + C = 0 for edge (p0, p1):
    // A = p0.y - p1.y, B = p1.x - p0.x, C = p0.x*p1.y - p1.x*p0.y.
    // The inputs are structure-of-arrays: all x values first, then all y values.
    int i = 0;
    for (; i + 7 < triCount; i += 8) {
        __m256 v0x = _mm256_loadu_ps(v0 + i);
        __m256 v0y = _mm256_loadu_ps(v0 + triCount + i);
        __m256 v1x = _mm256_loadu_ps(v1 + i);
        __m256 v1y = _mm256_loadu_ps(v1 + triCount + i);

        __m256 a = _mm256_sub_ps(v0y, v1y);
        __m256 b = _mm256_sub_ps(v1x, v0x);
        __m256 c = _mm256_sub_ps(_mm256_mul_ps(v0x, v1y), _mm256_mul_ps(v1x, v0y));

        _mm256_storeu_ps(edgeA + i, a);
        _mm256_storeu_ps(edgeB + i, b);
        _mm256_storeu_ps(edgeC + i, c);
    }
    // Scalar remainder
    for (; i < triCount; i++) {
        float ax = v0[i], ay = v0[triCount + i];
        float bx = v1[i], by = v1[triCount + i];
        edgeA[i] = ay - by;
        edgeB[i] = bx - ax;
        edgeC[i] = ax * by - bx * ay;
    }
}

} // namespace AVX
#endif

// --- AVX2 IMPLEMENTATIONS ---
#if defined(__AVX2__)
namespace AVX2 {

void TransformVerticesFMA(const Mat4x4& mvp, const float* posIn, float* posOut, int count) {
    __m256 col0 = _mm256_broadcast_ps(&mvp.col[0].v);
    __m256 col1 = _mm256_broadcast_ps(&mvp.col[1].v);
    __m256 col2 = _mm256_broadcast_ps(&mvp.col[2].v);
    __m256 col3 = _mm256_broadcast_ps(&mvp.col[3].v);

    int i = 0;
    for (; i + 1 < count; i += 2) {
        __m256 v = _mm256_loadu_ps(posIn + i * 4);
        __m256 vx = _mm256_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
        __m256 vy = _mm256_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
        __m256 vz = _mm256_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));
        __m256 vw = _mm256_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3));

        __m256 r = _mm256_mul_ps(col0, vx);
        r = _mm256_fmadd_ps(col1, vy, r);
        r = _mm256_fmadd_ps(col2, vz, r);
        r = _mm256_fmadd_ps(col3, vw, r);

        _mm256_storeu_ps(posOut + i * 4, r);
    }
    if (i < count) {
        Vec4f v(posIn[i * 4], posIn[i * 4 + 1], posIn[i * 4 + 2], posIn[i * 4 + 3]);
        Vec4f r = SSE2::MatVecMul(mvp, v);
        std::memcpy(posOut + i * 4, r.f, 16);
    }
}

void BatchTexCoordWrap(const int32_t* coordS, const int32_t* coordT, int32_t* outS, int32_t* outT, int maskS, int maskT,
                       bool mirrorS, bool mirrorT, int clampMaxS, int clampMaxT, int count) {
    __m256i vMaskBitsS = _mm256_set1_epi32((1 << maskS) - 1);
    __m256i vMaskBitsT = _mm256_set1_epi32((1 << maskT) - 1);
    __m256i vMirrorBitS = _mm256_set1_epi32(1 << maskS);
    __m256i vMirrorBitT = _mm256_set1_epi32(1 << maskT);
    __m256i vClampMaxS = _mm256_set1_epi32(clampMaxS);
    __m256i vClampMaxT = _mm256_set1_epi32(clampMaxT);
    __m256i vZero = _mm256_setzero_si256();

    int i = 0;
    for (; i + 7 < count; i += 8) {
        __m256i s = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(coordS + i));
        __m256i t = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(coordT + i));

        __m256i maskedS = _mm256_and_si256(s, vMaskBitsS);
        __m256i maskedT = _mm256_and_si256(t, vMaskBitsT);

        // Mirror: invert when the mirror bit is set.
        if (mirrorS) {
            __m256i mirrorFlag = _mm256_and_si256(s, vMirrorBitS);
            __m256i cmp = _mm256_cmpeq_epi32(mirrorFlag, vMirrorBitS);
            __m256i inverted = _mm256_sub_epi32(vMaskBitsS, maskedS);
            maskedS = _mm256_blendv_epi8(maskedS, inverted, cmp);
        }
        if (mirrorT) {
            __m256i mirrorFlag = _mm256_and_si256(t, vMirrorBitT);
            __m256i cmp = _mm256_cmpeq_epi32(mirrorFlag, vMirrorBitT);
            __m256i inverted = _mm256_sub_epi32(vMaskBitsT, maskedT);
            maskedT = _mm256_blendv_epi8(maskedT, inverted, cmp);
        }

        maskedS = _mm256_max_epi32(vZero, _mm256_min_epi32(maskedS, vClampMaxS));
        maskedT = _mm256_max_epi32(vZero, _mm256_min_epi32(maskedT, vClampMaxT));

        _mm256_storeu_si256(reinterpret_cast<__m256i*>(outS + i), maskedS);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(outT + i), maskedT);
    }
    // Scalar remainder
    for (; i < count; i++) {
        int s = coordS[i] & ((1 << maskS) - 1);
        int tt = coordT[i] & ((1 << maskT) - 1);
        if (mirrorS && (coordS[i] & (1 << maskS))) s = ((1 << maskS) - 1) - s;
        if (mirrorT && (coordT[i] & (1 << maskT))) tt = ((1 << maskT) - 1) - tt;
        outS[i] = std::max(0, std::min(s, clampMaxS));
        outT[i] = std::max(0, std::min(tt, clampMaxT));
    }
}

void BatchColorConvert_RGBA16_to_Float(const uint16_t* src, float* dst, int count) {
    const __m256 invScale = _mm256_set1_ps(1.0f / 31.0f);
    int i = 0;
    for (; i + 7 < count; i += 8) {
        __m128i raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        __m256i wide = _mm256_cvtepu16_epi32(raw);

        __m256i r = _mm256_and_si256(_mm256_srli_epi32(wide, 11), _mm256_set1_epi32(0x1F));
        __m256i g = _mm256_and_si256(_mm256_srli_epi32(wide, 6), _mm256_set1_epi32(0x1F));
        __m256i b = _mm256_and_si256(_mm256_srli_epi32(wide, 1), _mm256_set1_epi32(0x1F));
        __m256i a = _mm256_and_si256(wide, _mm256_set1_epi32(0x01));

        __m256 rf = _mm256_mul_ps(_mm256_cvtepi32_ps(r), invScale);
        __m256 gf = _mm256_mul_ps(_mm256_cvtepi32_ps(g), invScale);
        __m256 bf = _mm256_mul_ps(_mm256_cvtepi32_ps(b), invScale);
        __m256 af = _mm256_cvtepi32_ps(a); // 0 or 1

        // Transpose from one register per channel to interleaved RGBA for the 8 pixels.
        for (int j = 0; j < 8; j++) {
            float tmp_r[8], tmp_g[8], tmp_b[8], tmp_a[8];
            _mm256_storeu_ps(tmp_r, rf);
            _mm256_storeu_ps(tmp_g, gf);
            _mm256_storeu_ps(tmp_b, bf);
            _mm256_storeu_ps(tmp_a, af);
            for (int k = 0; k < 8; k++) {
                dst[(i + k) * 4 + 0] = tmp_r[k];
                dst[(i + k) * 4 + 1] = tmp_g[k];
                dst[(i + k) * 4 + 2] = tmp_b[k];
                dst[(i + k) * 4 + 3] = tmp_a[k];
            }
            break; // Only need one iteration
        }
    }
    // Scalar remainder
    for (; i < count; i++) {
        uint16_t px = src[i];
        dst[i * 4 + 0] = ((px >> 11) & 0x1F) / 31.0f;
        dst[i * 4 + 1] = ((px >> 6) & 0x1F) / 31.0f;
        dst[i * 4 + 2] = ((px >> 1) & 0x1F) / 31.0f;
        dst[i * 4 + 3] = static_cast<float>(px & 1);
    }
}

Vec8i GatherTMEM(const uint8_t* tmem, const Vec8i& addresses) {
    // AVX2 has no byte gather: gather 32-bit words at byte stride and mask to the low byte.
    __m256i gathered = _mm256_i32gather_epi32(reinterpret_cast<const int*>(tmem), addresses.v,
                                              1 // byte stride
    );
    return _mm256_and_si256(gathered, _mm256_set1_epi32(0xFF));
}

} // namespace AVX2
#endif

// --- DISPATCH IMPLEMENTATION ---
namespace Dispatch {

// Function pointer types
using FnVec4fBinary = Vec4f (*)(const Vec4f&, const Vec4f&);
using FnVec4fScalarMul = Vec4f (*)(const Vec4f&, float);
using FnFloat2Vec = float (*)(const Vec4f&, const Vec4f&);
using FnVec4fUnary = Vec4f (*)(const Vec4f&);
using FnMatMul = Mat4x4 (*)(const Mat4x4&, const Mat4x4&);
using FnMatVec = Vec4f (*)(const Mat4x4&, const Vec4f&);
using FnMatUnary = Mat4x4 (*)(const Mat4x4&);
using FnBatch = void (*)(const Mat4x4&, const float*, float*, int);
using FnLerp = Vec4f (*)(const Vec4f&, const Vec4f&, float);
using FnFMA = Vec4f (*)(const Vec4f&, const Vec4f&, const Vec4f&);
using FnVec4iBinary = Vec4i (*)(const Vec4i&, const Vec4i&);

// Dispatch table
static FnVec4fBinary s_Add = nullptr;
static FnVec4fBinary s_Sub = nullptr;
static FnVec4fBinary s_Mul = nullptr;
static FnVec4fBinary s_Div = nullptr;
static FnVec4fScalarMul s_MulScalar = nullptr;
static FnFMA s_FMA = nullptr;
static FnFloat2Vec s_Dot3 = nullptr;
static FnFloat2Vec s_Dot4 = nullptr;
static FnVec4fBinary s_Cross3 = nullptr;
static FnLerp s_Lerp = nullptr;
static FnVec4fBinary s_Min = nullptr;
static FnVec4fBinary s_Max = nullptr;
static FnMatMul s_MatMul = nullptr;
static FnMatVec s_MatVecMul = nullptr;
static FnMatUnary s_MatTranspose = nullptr;
static FnBatch s_TransformVerts = nullptr;
static FnBatch s_TransformNorms = nullptr;
static FnVec4iBinary s_AddI = nullptr;
static FnVec4iBinary s_MulLoI = nullptr;
static FnVec4iBinary s_MinI = nullptr;
static FnVec4iBinary s_MaxI = nullptr;

void Initialize() {
    g_SIMDLevel = DetectSIMDLevel();
    Initialize(g_SIMDLevel);
}

void Initialize(SIMDLevel level) {
    g_SIMDLevel = level;

    // Scalar defaults first; faster implementations overwrite them below.
    s_Add = Scalar::Add;
    s_Sub = Scalar::Sub;
    s_Mul = Scalar::Mul;
    s_Div = Scalar::Div;
    s_MulScalar = Scalar::MulScalar;
    s_FMA = Scalar::FMA;
    s_Dot3 = Scalar::Dot3;
    s_Dot4 = Scalar::Dot4;
    s_Cross3 = Scalar::Cross3;
    s_Lerp = Scalar::Lerp;
    s_Min = Scalar::Min;
    s_Max = Scalar::Max;
    s_MatMul = Scalar::MatMul;
    s_MatVecMul = Scalar::MatVecMul;
    s_MatTranspose = Scalar::MatTranspose;
    s_TransformVerts = Scalar::TransformVertices;
    s_TransformNorms = Scalar::TransformNormals;
    s_AddI = Scalar::AddI;
    s_MulLoI = Scalar::MulLoI;
    s_MinI = Scalar::MinI;
    s_MaxI = Scalar::MaxI;

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
    if (level >= SIMDLevel::SSE2) {
        s_Add = SSE2::Add;
        s_Sub = SSE2::Sub;
        s_Mul = SSE2::Mul;
        s_Div = SSE2::Div;
        s_MulScalar = SSE2::MulScalar;
        s_FMA = SSE2::FMA;
        s_Dot3 = SSE2::Dot3;
        s_Dot4 = SSE2::Dot4;
        s_Cross3 = SSE2::Cross3;
        s_Lerp = SSE2::Lerp;
        s_Min = SSE2::Min;
        s_Max = SSE2::Max;
        s_MatMul = SSE2::MatMul;
        s_MatVecMul = SSE2::MatVecMul;
        s_MatTranspose = SSE2::MatTranspose;
        s_TransformVerts = SSE2::TransformVertices;
        s_TransformNorms = SSE2::TransformNormals;
        s_AddI = SSE2::AddI;
        s_MulLoI = SSE2::MulLoI;
    }
#endif

#if defined(__SSE4_1__) || (defined(_MSC_VER) && defined(__AVX__))
    if (level >= SIMDLevel::SSE41) {
        s_Dot3 = SSE41::Dot3;
        s_Dot4 = SSE41::Dot4;
        s_MulLoI = SSE41::MulLoI;
        s_MinI = SSE41::MinI;
        s_MaxI = SSE41::MaxI;
    }
#endif

#if defined(__AVX__)
    if (level >= SIMDLevel::AVX) {
        s_TransformVerts = [](const Mat4x4& m, const float* in, float* out, int cnt) {
            AVX::TransformVertices8(m, in, out, cnt);
        };
    }
#endif

#if defined(__AVX2__)
    if (level >= SIMDLevel::AVX2) {
        s_TransformVerts = [](const Mat4x4& m, const float* in, float* out, int cnt) {
            AVX2::TransformVerticesFMA(m, in, out, cnt);
        };
    }
#endif
}

// Forwarding functions
Vec4f Add(const Vec4f& a, const Vec4f& b) { return s_Add(a, b); }
Vec4f Sub(const Vec4f& a, const Vec4f& b) { return s_Sub(a, b); }
Vec4f Mul(const Vec4f& a, const Vec4f& b) { return s_Mul(a, b); }
Vec4f Div(const Vec4f& a, const Vec4f& b) { return s_Div(a, b); }
Vec4f MulScalar(const Vec4f& a, float s) { return s_MulScalar(a, s); }
Vec4f FMA(const Vec4f& a, const Vec4f& b, const Vec4f& c) { return s_FMA(a, b, c); }
float Dot3(const Vec4f& a, const Vec4f& b) { return s_Dot3(a, b); }
float Dot4(const Vec4f& a, const Vec4f& b) { return s_Dot4(a, b); }
Vec4f Cross3(const Vec4f& a, const Vec4f& b) { return s_Cross3(a, b); }
Vec4f Lerp(const Vec4f& a, const Vec4f& b, float t) { return s_Lerp(a, b, t); }
Vec4f Min(const Vec4f& a, const Vec4f& b) { return s_Min(a, b); }
Vec4f Max(const Vec4f& a, const Vec4f& b) { return s_Max(a, b); }

Vec4f Clamp(const Vec4f& v, const Vec4f& lo, const Vec4f& hi) { return Min(Max(v, lo), hi); }

float Length3(const Vec4f& v) { return std::sqrt(Dot3(v, v)); }

Vec4f Normalize3(const Vec4f& v) {
    float d = Dot3(v, v);
    if (d < 1e-20f) return Vec4f(0.0f);
    float inv = 1.0f / std::sqrt(d);
    return MulScalar(v, inv);
}

Mat4x4 MatMul(const Mat4x4& a, const Mat4x4& b) { return s_MatMul(a, b); }
Vec4f MatVecMul(const Mat4x4& m, const Vec4f& v) { return s_MatVecMul(m, v); }
Mat4x4 MatTranspose(const Mat4x4& m) { return s_MatTranspose(m); }

void TransformVertices(const Mat4x4& mvp, const float* posIn, float* posOut, int count) {
    s_TransformVerts(mvp, posIn, posOut, count);
}

void TransformNormals(const Mat4x4& normalMat, const float* normIn, float* normOut, int count) {
    s_TransformNorms(normalMat, normIn, normOut, count);
}

void BatchLerp(const float* a, const float* b, float* out, float t, int count) {
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
    if (g_SIMDLevel >= SIMDLevel::SSE2) {
        SSE2::BatchLerp(a, b, out, t, count);
        return;
    }
#endif
    Scalar::BatchLerp(a, b, out, t, count);
}

Vec4i AddI(const Vec4i& a, const Vec4i& b) { return s_AddI(a, b); }
Vec4i MulLoI(const Vec4i& a, const Vec4i& b) { return s_MulLoI(a, b); }
Vec4i MinI(const Vec4i& a, const Vec4i& b) { return s_MinI(a, b); }
Vec4i MaxI(const Vec4i& a, const Vec4i& b) { return s_MaxI(a, b); }

} // namespace Dispatch

// --- N64-SPECIFIC SIMD UTILITIES ---
namespace N64SIMD {

Vec4i ComputeClipFlags(const float* clipSpaceVerts4) {
    Vec4i flags;
    for (int i = 0; i < 4; i++) {
        const float* v = clipSpaceVerts4 + i * 4;
        float x = v[0], y = v[1], z = v[2], w = v[3];
        int f = 0;
        if (x > w) f |= 0x01;  // CLIP_POSX
        if (x < -w) f |= 0x02; // CLIP_NEGX
        if (y > w) f |= 0x04;  // CLIP_POSY
        if (y < -w) f |= 0x08; // CLIP_NEGY
        if (z > w) f |= 0x10;  // CLIP_POSZ
        if (z < -w) f |= 0x20; // CLIP_NEGZ
        flags.i[i] = f;
    }
    return flags;
}

int ClipPolygonAgainstPlane(const float* vertsIn, float* vertsOut, int vertCount, int planeIndex, int floatsPerVert) {
    if (vertCount < 3) return 0;

    // Plane definitions: planeIndex 0-5 = +X,-X,+Y,-Y,+Z,-Z
    auto getDist = [planeIndex](const float* v) -> float {
        float x = v[0], y = v[1], z = v[2], w = v[3];
        switch (planeIndex) {
        case 0:  return w - x; // +X: x <= w
        case 1:  return w + x; // -X: x >= -w
        case 2:  return w - y; // +Y: y <= w
        case 3:  return w + y; // -Y: y >= -w
        case 4:  return w - z; // +Z: z <= w
        case 5:  return w + z; // -Z: z >= -w (near)
        default: return 0.0f;
        }
    };

    int outCount = 0;

    for (int i = 0; i < vertCount; i++) {
        int next = (i + 1) % vertCount;
        const float* curr = vertsIn + i * floatsPerVert;
        const float* nxt = vertsIn + next * floatsPerVert;

        float distCurr = getDist(curr);
        float distNext = getDist(nxt);
        bool currInside = distCurr >= 0.0f;
        bool nextInside = distNext >= 0.0f;

        if (currInside) {
            std::memcpy(vertsOut + outCount * floatsPerVert, curr, floatsPerVert * sizeof(float));
            outCount++;
        }

        // Edge crosses the plane: emit the intersection.
        if (currInside != nextInside) {
            float t = distCurr / (distCurr - distNext);
            float* out = vertsOut + outCount * floatsPerVert;
            for (int j = 0; j < floatsPerVert; j++) {
                out[j] = curr[j] + t * (nxt[j] - curr[j]);
            }
            outCount++;
        }
    }

    return outCount;
}

uint32_t CalculateDXT(uint32_t width, uint32_t size) {
    // DXT = number of 64-bit TMEM words per row
    uint32_t bitsPerTexel = 0;
    switch (size) {
    case 0: bitsPerTexel = 4; break;  // 4b
    case 1: bitsPerTexel = 8; break;  // 8b
    case 2: bitsPerTexel = 16; break; // 16b
    case 3: bitsPerTexel = 32; break; // 32b
    }
    uint32_t bitsPerRow = width * bitsPerTexel;
    uint32_t wordsPerRow = (bitsPerRow + 63) / 64;
    if (wordsPerRow == 0) return 0;
    // DXT is a 1.11 fixed-point value: (1 << 11) / wordsPerRow
    return (1 << 11) / wordsPerRow;
}

} // namespace N64SIMD

} // namespace SIMD
} // namespace Video
} // namespace Nova64
