/**
 * @file SIMDMath.hpp
 * @brief SIMD math primitives (scalar, SSE2, SSE4.1, AVX, AVX2) with runtime dispatch and N64 helpers.
 *
 * AVX-512 is detected by DetectSIMDLevel() but has no kernels of its own.
 */
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <cmath>
#include <cstring>
#include <algorithm>

// --- Platform SIMD Headers ---
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif
#endif

namespace Nova64 {
namespace Video {
namespace SIMD {

// --- Compile-time capability detection ---

#if defined(__AVX512F__)
static constexpr int kMaxSIMDWidth = 512;
#elif defined(__AVX2__)
static constexpr int kMaxSIMDWidth = 256;
#elif defined(__AVX__)
static constexpr int kMaxSIMDWidth = 256;
#elif defined(__SSE4_1__)
static constexpr int kMaxSIMDWidth = 128;
#elif defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
static constexpr int kMaxSIMDWidth = 128;
#else
static constexpr int kMaxSIMDWidth = 0;
#endif

// --- Runtime SIMD level (set during initialization) ---
enum class SIMDLevel : uint8_t { Scalar = 0, SSE2 = 1, SSE41 = 2, AVX = 3, AVX2 = 4, AVX512 = 5 };

SIMDLevel DetectSIMDLevel();

extern SIMDLevel g_SIMDLevel;

// --- 128-bit Vector Wrapper (SSE2) ---
struct alignas(16) Vec4f {
    union {
        float f[4];
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
        __m128 v;
#endif
    };

    Vec4f() : f{0, 0, 0, 0} {}
    Vec4f(float x, float y, float z, float w) : f{x, y, z, w} {}
    explicit Vec4f(float s) : f{s, s, s, s} {}
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
    Vec4f(__m128 _v) : v(_v) {}
    operator __m128() const { return v; }
#endif

    float& operator[](int i) { return f[i]; }
    float operator[](int i) const { return f[i]; }

    float x() const { return f[0]; }
    float y() const { return f[1]; }
    float z() const { return f[2]; }
    float w() const { return f[3]; }
};

struct alignas(16) Vec4i {
    union {
        int32_t i[4];
        uint32_t u[4];
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
        __m128i v;
#endif
    };

    Vec4i() : i{0, 0, 0, 0} {}
    Vec4i(int32_t a, int32_t b, int32_t c, int32_t d) : i{a, b, c, d} {}
    explicit Vec4i(int32_t s) : i{s, s, s, s} {}
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
    Vec4i(__m128i _v) : v(_v) {}
    operator __m128i() const { return v; }
#endif

    int32_t& operator[](int idx) { return i[idx]; }
    int32_t operator[](int idx) const { return i[idx]; }
};

// --- 256-bit Vector Wrapper (AVX) ---
struct alignas(32) Vec8f {
    union {
        float f[8];
#if defined(__AVX__)
        __m256 v;
#endif
    };

    Vec8f() : f{0, 0, 0, 0, 0, 0, 0, 0} {}
    explicit Vec8f(float s) : f{s, s, s, s, s, s, s, s} {}
#if defined(__AVX__)
    Vec8f(__m256 _v) : v(_v) {}
    operator __m256() const { return v; }
#endif

    float& operator[](int i) { return f[i]; }
    float operator[](int i) const { return f[i]; }
};

struct alignas(32) Vec8i {
    union {
        int32_t i[8];
        uint32_t u[8];
#if defined(__AVX2__)
        __m256i v;
#endif
    };

    Vec8i() : i{0, 0, 0, 0, 0, 0, 0, 0} {}
    explicit Vec8i(int32_t s) : i{s, s, s, s, s, s, s, s} {}
#if defined(__AVX2__)
    Vec8i(__m256i _v) : v(_v) {}
    operator __m256i() const { return v; }
#endif

    int32_t& operator[](int idx) { return i[idx]; }
    int32_t operator[](int idx) const { return i[idx]; }
};

// --- 4x4 Matrix (column-major, SSE-friendly layout) ---
struct alignas(16) Mat4x4 {
    Vec4f col[4]; // column-major: col[0] = first column

    Mat4x4() {
        col[0] = Vec4f(1, 0, 0, 0);
        col[1] = Vec4f(0, 1, 0, 0);
        col[2] = Vec4f(0, 0, 1, 0);
        col[3] = Vec4f(0, 0, 0, 1);
    }

    Mat4x4(const Vec4f& c0, const Vec4f& c1, const Vec4f& c2, const Vec4f& c3) {
        col[0] = c0;
        col[1] = c1;
        col[2] = c2;
        col[3] = c3;
    }

    float& operator()(int row, int c) { return col[c].f[row]; }
    float operator()(int row, int c) const { return col[c].f[row]; }

    static Mat4x4 Identity();
    static Mat4x4 Zero();

    // Row access (gathers from columns)
    Vec4f Row(int r) const { return Vec4f(col[0][r], col[1][r], col[2][r], col[3][r]); }

    const float* Data() const { return col[0].f; }
    float* Data() { return col[0].f; }
};

// --- SCALAR MATH OPERATIONS (fallback for all platforms) ---
namespace Scalar {

// --- Vector Arithmetic ---
Vec4f Add(const Vec4f& a, const Vec4f& b);
Vec4f Sub(const Vec4f& a, const Vec4f& b);
Vec4f Mul(const Vec4f& a, const Vec4f& b);
Vec4f Div(const Vec4f& a, const Vec4f& b);
Vec4f MulScalar(const Vec4f& a, float s);
Vec4f FMA(const Vec4f& a, const Vec4f& b, const Vec4f& c); // a*b+c

// --- Vector Operations ---
float Dot3(const Vec4f& a, const Vec4f& b);
float Dot4(const Vec4f& a, const Vec4f& b);
Vec4f Cross3(const Vec4f& a, const Vec4f& b);
float Length3(const Vec4f& v);
float Length4(const Vec4f& v);
Vec4f Normalize3(const Vec4f& v);
Vec4f Normalize4(const Vec4f& v);
Vec4f Lerp(const Vec4f& a, const Vec4f& b, float t);
Vec4f Min(const Vec4f& a, const Vec4f& b);
Vec4f Max(const Vec4f& a, const Vec4f& b);
Vec4f Clamp(const Vec4f& v, const Vec4f& lo, const Vec4f& hi);
Vec4f Abs(const Vec4f& v);
Vec4f Negate(const Vec4f& v);
Vec4f Reciprocal(const Vec4f& v);
Vec4f ReciprocalSqrt(const Vec4f& v);
Vec4f Floor(const Vec4f& v);
Vec4f Ceil(const Vec4f& v);
Vec4f Round(const Vec4f& v);

// --- Matrix Operations ---
Mat4x4 MatMul(const Mat4x4& a, const Mat4x4& b);
Vec4f MatVecMul(const Mat4x4& m, const Vec4f& v);
Mat4x4 MatTranspose(const Mat4x4& m);

// --- Batch Operations ---
void TransformVertices(const Mat4x4& mvp, const float* posIn, float* posOut, int count);
void TransformNormals(const Mat4x4& normalMat, const float* normIn, float* normOut, int count);
void BatchLerp(const float* a, const float* b, float* out, float t, int count);

// --- Integer Operations ---
Vec4i AddI(const Vec4i& a, const Vec4i& b);
Vec4i SubI(const Vec4i& a, const Vec4i& b);
Vec4i MulLoI(const Vec4i& a, const Vec4i& b); // low 32 bits of each product
Vec4i ShiftLeftI(const Vec4i& a, int bits);
Vec4i ShiftRightI(const Vec4i& a, int bits);
Vec4i ShiftRightLogI(const Vec4i& a, int bits);
Vec4i AndI(const Vec4i& a, const Vec4i& b);
Vec4i OrI(const Vec4i& a, const Vec4i& b);
Vec4i XorI(const Vec4i& a, const Vec4i& b);
Vec4i MinI(const Vec4i& a, const Vec4i& b);
Vec4i MaxI(const Vec4i& a, const Vec4i& b);

} // namespace Scalar

// --- SSE2 MATH OPERATIONS ---
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
namespace SSE2 {

// --- Vector Arithmetic ---
inline Vec4f Add(const Vec4f& a, const Vec4f& b) { return _mm_add_ps(a.v, b.v); }
inline Vec4f Sub(const Vec4f& a, const Vec4f& b) { return _mm_sub_ps(a.v, b.v); }
inline Vec4f Mul(const Vec4f& a, const Vec4f& b) { return _mm_mul_ps(a.v, b.v); }
inline Vec4f Div(const Vec4f& a, const Vec4f& b) { return _mm_div_ps(a.v, b.v); }
inline Vec4f MulScalar(const Vec4f& a, float s) { return _mm_mul_ps(a.v, _mm_set1_ps(s)); }

Vec4f FMA(const Vec4f& a, const Vec4f& b, const Vec4f& c);

// --- Vector Operations ---
float Dot3(const Vec4f& a, const Vec4f& b);
float Dot4(const Vec4f& a, const Vec4f& b);
Vec4f Cross3(const Vec4f& a, const Vec4f& b);
float Length3(const Vec4f& v);
float Length4(const Vec4f& v);
Vec4f Normalize3(const Vec4f& v);
Vec4f Normalize4(const Vec4f& v);
Vec4f Lerp(const Vec4f& a, const Vec4f& b, float t);
Vec4f Min(const Vec4f& a, const Vec4f& b);
Vec4f Max(const Vec4f& a, const Vec4f& b);
Vec4f Clamp(const Vec4f& v, const Vec4f& lo, const Vec4f& hi);
Vec4f Abs(const Vec4f& v);
Vec4f Negate(const Vec4f& v);
Vec4f Reciprocal(const Vec4f& v);
Vec4f ReciprocalSqrt(const Vec4f& v);

// --- Matrix Operations ---
Mat4x4 MatMul(const Mat4x4& a, const Mat4x4& b);
Vec4f MatVecMul(const Mat4x4& m, const Vec4f& v);
Mat4x4 MatTranspose(const Mat4x4& m);

// --- Batch Operations ---
void TransformVertices(const Mat4x4& mvp, const float* posIn, float* posOut, int count);
void TransformNormals(const Mat4x4& normalMat, const float* normIn, float* normOut, int count);
void BatchLerp(const float* a, const float* b, float* out, float t, int count);

// --- Integer Operations ---
inline Vec4i AddI(const Vec4i& a, const Vec4i& b) { return _mm_add_epi32(a.v, b.v); }
inline Vec4i SubI(const Vec4i& a, const Vec4i& b) { return _mm_sub_epi32(a.v, b.v); }
Vec4i MulLoI(const Vec4i& a, const Vec4i& b);
inline Vec4i ShiftLeftI(const Vec4i& a, int bits) { return _mm_slli_epi32(a.v, bits); }
inline Vec4i ShiftRightI(const Vec4i& a, int bits) { return _mm_srai_epi32(a.v, bits); }
inline Vec4i ShiftRightLogI(const Vec4i& a, int b) { return _mm_srli_epi32(a.v, b); }
inline Vec4i AndI(const Vec4i& a, const Vec4i& b) { return _mm_and_si128(a.v, b.v); }
inline Vec4i OrI(const Vec4i& a, const Vec4i& b) { return _mm_or_si128(a.v, b.v); }
inline Vec4i XorI(const Vec4i& a, const Vec4i& b) { return _mm_xor_si128(a.v, b.v); }

// Texture coordinate wrapping (4 coords at once)
Vec4i WrapTexCoord(const Vec4i& coord, const Vec4i& mask, const Vec4i& mirror, const Vec4i& clampMax);
// Color packing (4 RGBA values to 4 packed RGBA5551)
Vec4i PackRGBA5551(const Vec4i& r, const Vec4i& g, const Vec4i& b, const Vec4i& a);

} // namespace SSE2
#endif

// --- SSE4.1 MATH OPERATIONS ---
#if defined(__SSE4_1__) || (defined(_MSC_VER) && defined(__AVX__))
namespace SSE41 {

float Dot3(const Vec4f& a, const Vec4f& b);
float Dot4(const Vec4f& a, const Vec4f& b);
Vec4f Floor(const Vec4f& v);
Vec4f Ceil(const Vec4f& v);
Vec4f Round(const Vec4f& v);
Vec4i MinI(const Vec4i& a, const Vec4i& b);
Vec4i MaxI(const Vec4i& a, const Vec4i& b);
Vec4i MulLoI(const Vec4i& a, const Vec4i& b);

// Blend operations (used for triangle interpolation)
Vec4f BlendByMask(const Vec4f& a, const Vec4f& b, const Vec4f& mask);

} // namespace SSE41
#endif

// --- AVX MATH OPERATIONS (8-wide float) ---
#if defined(__AVX__)
namespace AVX {

inline Vec8f Add(const Vec8f& a, const Vec8f& b) { return _mm256_add_ps(a.v, b.v); }
inline Vec8f Sub(const Vec8f& a, const Vec8f& b) { return _mm256_sub_ps(a.v, b.v); }
inline Vec8f Mul(const Vec8f& a, const Vec8f& b) { return _mm256_mul_ps(a.v, b.v); }
inline Vec8f Div(const Vec8f& a, const Vec8f& b) { return _mm256_div_ps(a.v, b.v); }
inline Vec8f Min(const Vec8f& a, const Vec8f& b) { return _mm256_min_ps(a.v, b.v); }
inline Vec8f Max(const Vec8f& a, const Vec8f& b) { return _mm256_max_ps(a.v, b.v); }

Vec8f FMA(const Vec8f& a, const Vec8f& b, const Vec8f& c);

// Batch vertex transform, two vertices per 256-bit register.
void TransformVertices8(const Mat4x4& mvp, const float* posIn, float* posOut, int count);

// Batch triangle setup (edge equations for 8 triangles at once)
void BatchEdgeSetup(const float* v0, const float* v1, const float* v2, float* edgeA, float* edgeB, float* edgeC,
                    int triCount);

} // namespace AVX
#endif

// --- AVX2 MATH OPERATIONS (8-wide integer + FMA) ---
#if defined(__AVX2__)
namespace AVX2 {

inline Vec8i AddI(const Vec8i& a, const Vec8i& b) { return _mm256_add_epi32(a.v, b.v); }
inline Vec8i SubI(const Vec8i& a, const Vec8i& b) { return _mm256_sub_epi32(a.v, b.v); }
inline Vec8i MulLoI(const Vec8i& a, const Vec8i& b) { return _mm256_mullo_epi32(a.v, b.v); }
inline Vec8i AndI(const Vec8i& a, const Vec8i& b) { return _mm256_and_si256(a.v, b.v); }
inline Vec8i OrI(const Vec8i& a, const Vec8i& b) { return _mm256_or_si256(a.v, b.v); }
inline Vec8i XorI(const Vec8i& a, const Vec8i& b) { return _mm256_xor_si256(a.v, b.v); }
inline Vec8i ShiftLeftI(const Vec8i& a, int bits) { return _mm256_slli_epi32(a.v, bits); }
inline Vec8i ShiftRightI(const Vec8i& a, int bits) { return _mm256_srai_epi32(a.v, bits); }
inline Vec8i MinI(const Vec8i& a, const Vec8i& b) { return _mm256_min_epi32(a.v, b.v); }
inline Vec8i MaxI(const Vec8i& a, const Vec8i& b) { return _mm256_max_epi32(a.v, b.v); }

// FMA for AVX2 (uses FMA3 instructions)
inline Vec8f FMA(const Vec8f& a, const Vec8f& b, const Vec8f& c) {
    Vec8f r;
    r.v = _mm256_fmadd_ps(a.v, b.v, c.v);
    return r;
}

// 8-wide vertex transform with FMA
void TransformVerticesFMA(const Mat4x4& mvp, const float* posIn, float* posOut, int count);

// 8-wide texture coordinate processing
void BatchTexCoordWrap(const int32_t* coordS, const int32_t* coordT, int32_t* outS, int32_t* outT, int maskS, int maskT,
                       bool mirrorS, bool mirrorT, int clampMaxS, int clampMaxT, int count);

// Batch color conversion: 8 RGBA16 -> 8 RGBA float
void BatchColorConvert_RGBA16_to_Float(const uint16_t* src, float* dst, int count);

// Gather-based TMEM access (8 texels at once)
Vec8i GatherTMEM(const uint8_t* tmem, const Vec8i& addresses);

} // namespace AVX2
#endif

// --- DISPATCH LAYER - Selects best implementation at runtime ---
namespace Dispatch {

// Selects the implementations for the detected SIMD level, or for `forceLevel`.
void Initialize();
void Initialize(SIMDLevel forceLevel);

// --- Vector Operations (use function pointers internally) ---
Vec4f Add(const Vec4f& a, const Vec4f& b);
Vec4f Sub(const Vec4f& a, const Vec4f& b);
Vec4f Mul(const Vec4f& a, const Vec4f& b);
Vec4f Div(const Vec4f& a, const Vec4f& b);
Vec4f MulScalar(const Vec4f& a, float s);
Vec4f FMA(const Vec4f& a, const Vec4f& b, const Vec4f& c);
float Dot3(const Vec4f& a, const Vec4f& b);
float Dot4(const Vec4f& a, const Vec4f& b);
Vec4f Cross3(const Vec4f& a, const Vec4f& b);
float Length3(const Vec4f& v);
Vec4f Normalize3(const Vec4f& v);
Vec4f Lerp(const Vec4f& a, const Vec4f& b, float t);
Vec4f Min(const Vec4f& a, const Vec4f& b);
Vec4f Max(const Vec4f& a, const Vec4f& b);
Vec4f Clamp(const Vec4f& v, const Vec4f& lo, const Vec4f& hi);

// --- Matrix Operations ---
Mat4x4 MatMul(const Mat4x4& a, const Mat4x4& b);
Vec4f MatVecMul(const Mat4x4& m, const Vec4f& v);
Mat4x4 MatTranspose(const Mat4x4& m);

// --- Batch Operations ---
void TransformVertices(const Mat4x4& mvp, const float* posIn, float* posOut, int count);
void TransformNormals(const Mat4x4& normalMat, const float* normIn, float* normOut, int count);
void BatchLerp(const float* a, const float* b, float* out, float t, int count);

// --- Integer Operations ---
Vec4i AddI(const Vec4i& a, const Vec4i& b);
Vec4i MulLoI(const Vec4i& a, const Vec4i& b);
Vec4i MinI(const Vec4i& a, const Vec4i& b);
Vec4i MaxI(const Vec4i& a, const Vec4i& b);

} // namespace Dispatch

// --- N64-SPECIFIC SIMD UTILITIES ---
namespace N64SIMD {

// --- Fixed-Point Conversion ---

// --- Vertex Processing ---

// --- Lighting ---
// vertexCount * 4 floats (RGBA)

// --- Clipping ---
// Compute clip flags for 4 vertices at once
// Returns packed clip flags: bits [POSX, NEGX, POSY, NEGY, POSZ, NEGZ]
Vec4i ComputeClipFlags(const float* clipSpaceVerts4); // 4 verts * 4 components

// Sutherland-Hodgman clip against single plane (SIMD-accelerated lerp)
int ClipPolygonAgainstPlane(const float* vertsIn, float* vertsOut, int vertCount, int planeIndex, int floatsPerVert);

// --- Texture Coordinate Processing ---

// --- Color Format Conversion ---

// --- Depth Buffer ---

// --- DXT Calculation ---
// Calculate DXT value for LoadBlock command
uint32_t CalculateDXT(uint32_t width, uint32_t size);

// --- CRC32 (for texture hashing) ---

} // namespace N64SIMD

} // namespace SIMD
} // namespace Video
} // namespace Nova64
