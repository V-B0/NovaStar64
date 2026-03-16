/**
 * @file N64Math.cpp
 * @brief Implementation of fixed-point math, matrix operations, clipping, and lighting
 */

#include "N64Math.hpp"
#include "../Common/RDRAMAccess.hpp"

#include <cstring>
#include <cmath>
#include <algorithm>

#ifdef _MSC_VER
    #include <intrin.h>
#endif

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
    #include <emmintrin.h>
    #define NOVA_HAS_SSE2 1
#else
    #define NOVA_HAS_SSE2 0
#endif

#if defined(__SSE4_1__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64)))
    #include <smmintrin.h>
    #define NOVA_HAS_SSE41 1
#else
    #define NOVA_HAS_SSE41 0
#endif

#if defined(__AVX__) || (defined(_MSC_VER) && defined(__AVX__))
    #include <immintrin.h>
    #define NOVA_HAS_AVX 1
#else
    #define NOVA_HAS_AVX 0
#endif

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════════════════
//  Matrix4x4::LoadFromN64
// ═══════════════════════════════════════════════════════════════════════════

void Matrix4x4::LoadFromN64(const uint8_t* rdram, uint32_t address) {
    // N64 matrix format: 64 bytes total
    // Bytes 0-31: 4x4 int16 (integer parts)
    // Bytes 32-63: 4x4 uint16 (fraction parts)
    // RDRAM uses 32-bit word-swapped storage
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            uint32_t intOff = address + (i * 8) + (j * 2);
            uint32_t fracOff = address + 32 + (i * 8) + (j * 2);
            
            int16_t intPart = RDRAMReadHalfSigned(rdram, intOff);
            uint16_t fracPart = RDRAMReadHalf(rdram, fracOff);
            
            // Combine: integer + fraction/65536
            m[i][j] = static_cast<float>(intPart) + static_cast<float>(fracPart) / 65536.0f;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Matrix Operations
// ═══════════════════════════════════════════════════════════════════════════

namespace MatrixOps {

void Multiply(const Matrix4x4& a, const Matrix4x4& b, Matrix4x4& result) {
    Matrix4x4 temp;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            temp.m[i][j] = a.m[i][0]*b.m[0][j] + a.m[i][1]*b.m[1][j] +
                           a.m[i][2]*b.m[2][j] + a.m[i][3]*b.m[3][j];
        }
    }
    result = temp;
}

#if NOVA_HAS_SSE41
void MultiplySSE(const Matrix4x4& a, const Matrix4x4& b, Matrix4x4& result) {
    // Load B columns
    __m128 bCol0 = _mm_setr_ps(b.m[0][0], b.m[1][0], b.m[2][0], b.m[3][0]);
    __m128 bCol1 = _mm_setr_ps(b.m[0][1], b.m[1][1], b.m[2][1], b.m[3][1]);
    __m128 bCol2 = _mm_setr_ps(b.m[0][2], b.m[1][2], b.m[2][2], b.m[3][2]);
    __m128 bCol3 = _mm_setr_ps(b.m[0][3], b.m[1][3], b.m[2][3], b.m[3][3]);
    
    for (int i = 0; i < 4; i++) {
        __m128 aRow = _mm_loadu_ps(a.m[i]);
        
        __m128 r0 = _mm_dp_ps(aRow, bCol0, 0xF1);
        __m128 r1 = _mm_dp_ps(aRow, bCol1, 0xF2);
        __m128 r2 = _mm_dp_ps(aRow, bCol2, 0xF4);
        __m128 r3 = _mm_dp_ps(aRow, bCol3, 0xF8);
        
        __m128 row = _mm_or_ps(_mm_or_ps(r0, r1), _mm_or_ps(r2, r3));
        _mm_storeu_ps(result.m[i], row);
    }
}
#else
void MultiplySSE(const Matrix4x4& a, const Matrix4x4& b, Matrix4x4& result) {
    Multiply(a, b, result); // Fallback
}
#endif

void Transpose(const Matrix4x4& src, Matrix4x4& dst) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            dst.m[i][j] = src.m[j][i];
}

void LoadFromN64(const uint8_t* rdram, uint32_t address, Matrix4x4& out) {
    out.LoadFromN64(rdram, address);
}

Vec4f TransformVertex(const Matrix4x4& mvp, const Vec3f& position) {
    float in[4] = { position.x, position.y, position.z, 1.0f };
    float out[4];
    mvp.Transform(in, out);
    return { out[0], out[1], out[2], out[3] };
}

Vec3f TransformNormal(const Matrix4x4& model, const Vec3f& normal) {
    // Use upper-left 3x3 of model matrix
    float x = model.m[0][0]*normal.x + model.m[0][1]*normal.y + model.m[0][2]*normal.z;
    float y = model.m[1][0]*normal.x + model.m[1][1]*normal.y + model.m[1][2]*normal.z;
    float z = model.m[2][0]*normal.x + model.m[2][1]*normal.y + model.m[2][2]*normal.z;
    
    // Normalize result
    float len = std::sqrt(x*x + y*y + z*z);
    if (len > 1e-7f) {
        float inv = 1.0f / len;
        return { x*inv, y*inv, z*inv };
    }
    return { 0, 0, 0 };
}

void BatchTransformVertices(
    const Matrix4x4& mvp,
    const int16_t* positions,
    float* outClipSpace,
    int count,
    int srcStride)
{
    const uint8_t* src = reinterpret_cast<const uint8_t*>(positions);
    
    for (int i = 0; i < count; i++) {
        const int16_t* pos = reinterpret_cast<const int16_t*>(src + i * srcStride);
        float x = static_cast<float>(pos[0]);
        float y = static_cast<float>(pos[1]);
        float z = static_cast<float>(pos[2]);
        
        float* out = outClipSpace + i * 4;
        out[0] = mvp.m[0][0]*x + mvp.m[0][1]*y + mvp.m[0][2]*z + mvp.m[0][3];
        out[1] = mvp.m[1][0]*x + mvp.m[1][1]*y + mvp.m[1][2]*z + mvp.m[1][3];
        out[2] = mvp.m[2][0]*x + mvp.m[2][1]*y + mvp.m[2][2]*z + mvp.m[2][3];
        out[3] = mvp.m[3][0]*x + mvp.m[3][1]*y + mvp.m[3][2]*z + mvp.m[3][3];
    }
}

#if NOVA_HAS_SSE2
void BatchTransformVerticesSSE(
    const Matrix4x4& mvp,
    const int16_t* positions,
    float* outClipSpace,
    int count,
    int srcStride)
{
    // Load matrix rows
    __m128 row0 = _mm_loadu_ps(mvp.m[0]);
    __m128 row1 = _mm_loadu_ps(mvp.m[1]);
    __m128 row2 = _mm_loadu_ps(mvp.m[2]);
    __m128 row3 = _mm_loadu_ps(mvp.m[3]);
    
    const uint8_t* src = reinterpret_cast<const uint8_t*>(positions);
    
    for (int i = 0; i < count; i++) {
        const int16_t* pos = reinterpret_cast<const int16_t*>(src + i * srcStride);
        
        __m128 x = _mm_set1_ps(static_cast<float>(pos[0]));
        __m128 y = _mm_set1_ps(static_cast<float>(pos[1]));
        __m128 z = _mm_set1_ps(static_cast<float>(pos[2]));
        __m128 w = _mm_set1_ps(1.0f);
        
        // Transpose multiply approach: dot each row with (x,y,z,1)
        __m128 r0 = _mm_mul_ps(row0, x);
        __m128 r1 = _mm_mul_ps(row1, y);
        __m128 r2 = _mm_mul_ps(row2, z);
        __m128 r3 = _mm_mul_ps(row3, w);
        
        // Actually this gives wrong results - we want row*col
        // Use the scalar approach but with SSE loads for the output
        float px = static_cast<float>(pos[0]);
        float py = static_cast<float>(pos[1]);
        float pz = static_cast<float>(pos[2]);
        
        __m128 vx = _mm_set1_ps(px);
        __m128 vy = _mm_set1_ps(py);
        __m128 vz = _mm_set1_ps(pz);
        
        // col0 * x + col1 * y + col2 * z + col3 * 1.0
        // We need columns, but matrix is stored as rows. Transpose on the fly:
        __m128 col0 = _mm_setr_ps(mvp.m[0][0], mvp.m[1][0], mvp.m[2][0], mvp.m[3][0]);
        __m128 col1 = _mm_setr_ps(mvp.m[0][1], mvp.m[1][1], mvp.m[2][1], mvp.m[3][1]);
        __m128 col2 = _mm_setr_ps(mvp.m[0][2], mvp.m[1][2], mvp.m[2][2], mvp.m[3][2]);
        __m128 col3 = _mm_setr_ps(mvp.m[0][3], mvp.m[1][3], mvp.m[2][3], mvp.m[3][3]);
        
        __m128 result = _mm_add_ps(
            _mm_add_ps(_mm_mul_ps(col0, vx), _mm_mul_ps(col1, vy)),
            _mm_add_ps(_mm_mul_ps(col2, vz), col3)
        );
        
        _mm_storeu_ps(outClipSpace + i * 4, result);
    }
}
#else
void BatchTransformVerticesSSE(const Matrix4x4& mvp, const int16_t* positions,
                                float* outClipSpace, int count, int srcStride) {
    BatchTransformVertices(mvp, positions, outClipSpace, count, srcStride);
}
#endif

void BatchTransformVerticesAVX(
    const Matrix4x4& mvp,
    const int16_t* positions,
    float* outClipSpace,
    int count,
    int srcStride)
{
#if NOVA_HAS_AVX
    // For AVX, process pairs of vertices with 256-bit registers
    const uint8_t* src = reinterpret_cast<const uint8_t*>(positions);
    
    // Broadcast matrix columns as 256-bit
    __m256 col0 = _mm256_setr_ps(mvp.m[0][0], mvp.m[1][0], mvp.m[2][0], mvp.m[3][0],
                                  mvp.m[0][0], mvp.m[1][0], mvp.m[2][0], mvp.m[3][0]);
    __m256 col1 = _mm256_setr_ps(mvp.m[0][1], mvp.m[1][1], mvp.m[2][1], mvp.m[3][1],
                                  mvp.m[0][1], mvp.m[1][1], mvp.m[2][1], mvp.m[3][1]);
    __m256 col2 = _mm256_setr_ps(mvp.m[0][2], mvp.m[1][2], mvp.m[2][2], mvp.m[3][2],
                                  mvp.m[0][2], mvp.m[1][2], mvp.m[2][2], mvp.m[3][2]);
    __m256 col3 = _mm256_setr_ps(mvp.m[0][3], mvp.m[1][3], mvp.m[2][3], mvp.m[3][3],
                                  mvp.m[0][3], mvp.m[1][3], mvp.m[2][3], mvp.m[3][3]);
    
    int i = 0;
    for (; i + 1 < count; i += 2) {
        const int16_t* p0 = reinterpret_cast<const int16_t*>(src + i * srcStride);
        const int16_t* p1 = reinterpret_cast<const int16_t*>(src + (i+1) * srcStride);
        
        __m256 vx = _mm256_setr_ps(
            static_cast<float>(p0[0]), static_cast<float>(p0[0]),
            static_cast<float>(p0[0]), static_cast<float>(p0[0]),
            static_cast<float>(p1[0]), static_cast<float>(p1[0]),
            static_cast<float>(p1[0]), static_cast<float>(p1[0])
        );
        __m256 vy = _mm256_setr_ps(
            static_cast<float>(p0[1]), static_cast<float>(p0[1]),
            static_cast<float>(p0[1]), static_cast<float>(p0[1]),
            static_cast<float>(p1[1]), static_cast<float>(p1[1]),
            static_cast<float>(p1[1]), static_cast<float>(p1[1])
        );
        __m256 vz = _mm256_setr_ps(
            static_cast<float>(p0[2]), static_cast<float>(p0[2]),
            static_cast<float>(p0[2]), static_cast<float>(p0[2]),
            static_cast<float>(p1[2]), static_cast<float>(p1[2]),
            static_cast<float>(p1[2]), static_cast<float>(p1[2])
        );
        
        __m256 result = _mm256_add_ps(
            _mm256_add_ps(_mm256_mul_ps(col0, vx), _mm256_mul_ps(col1, vy)),
            _mm256_add_ps(_mm256_mul_ps(col2, vz), col3)
        );
        
        _mm256_storeu_ps(outClipSpace + i * 4, result);
    }
    
    // Handle remaining vertex
    if (i < count) {
        const int16_t* pos = reinterpret_cast<const int16_t*>(src + i * srcStride);
        float px = static_cast<float>(pos[0]);
        float py = static_cast<float>(pos[1]);
        float pz = static_cast<float>(pos[2]);
        float* out = outClipSpace + i * 4;
        out[0] = mvp.m[0][0]*px + mvp.m[0][1]*py + mvp.m[0][2]*pz + mvp.m[0][3];
        out[1] = mvp.m[1][0]*px + mvp.m[1][1]*py + mvp.m[1][2]*pz + mvp.m[1][3];
        out[2] = mvp.m[2][0]*px + mvp.m[2][1]*py + mvp.m[2][2]*pz + mvp.m[2][3];
        out[3] = mvp.m[3][0]*px + mvp.m[3][1]*py + mvp.m[3][2]*pz + mvp.m[3][3];
    }
#else
    BatchTransformVertices(mvp, positions, outClipSpace, count, srcStride);
#endif
}

void BatchTransformVerticesDispatch(
    const Matrix4x4& mvp,
    const int16_t* positions,
    float* outClipSpace,
    int count,
    int srcStride)
{
    auto level = g_platform.GetBestSIMDLevel();
    
    if (level >= SIMDLevel::AVX) {
        BatchTransformVerticesAVX(mvp, positions, outClipSpace, count, srcStride);
    } else if (level >= SIMDLevel::SSE2) {
        BatchTransformVerticesSSE(mvp, positions, outClipSpace, count, srcStride);
    } else {
        BatchTransformVertices(mvp, positions, outClipSpace, count, srcStride);
    }
}

} // namespace MatrixOps

// ═══════════════════════════════════════════════════════════════════════════
//  Clipping
// ═══════════════════════════════════════════════════════════════════════════

namespace Clipping {

uint32_t ComputeClipFlags(const Vec4f& clipPos) {
    uint32_t flags = 0;
    if (clipPos.x < -clipPos.w) flags |= TransformedVertex::CLIP_LEFT;
    if (clipPos.x >  clipPos.w) flags |= TransformedVertex::CLIP_RIGHT;
    if (clipPos.y < -clipPos.w) flags |= TransformedVertex::CLIP_BOTTOM;
    if (clipPos.y >  clipPos.w) flags |= TransformedVertex::CLIP_TOP;
    if (clipPos.z < -clipPos.w) flags |= TransformedVertex::CLIP_NEAR;
    if (clipPos.z >  clipPos.w) flags |= TransformedVertex::CLIP_FAR;
    if (clipPos.w <= 0.0f)      flags |= TransformedVertex::CLIP_W;
    return flags;
}

TransformedVertex LerpVertex(const TransformedVertex& a, const TransformedVertex& b, float t) {
    TransformedVertex result;
    float invT = 1.0f - t;
    
    result.x = a.x * invT + b.x * t;
    result.y = a.y * invT + b.y * t;
    result.z = a.z * invT + b.z * t;
    result.w = a.w * invT + b.w * t;
    result.s = a.s * invT + b.s * t;
    result.t = a.t * invT + b.t * t;
    result.r = a.r * invT + b.r * t;
    result.g = a.g * invT + b.g * t;
    result.b = a.b * invT + b.b * t;
    result.a = a.a * invT + b.a * t;
    result.fogFactor = a.fogFactor * invT + b.fogFactor * t;
    result.clipFlags = 0;
    result.reject = false;
    
    return result;
}

int ClipAgainstPlane(
    const TransformedVertex* input, int inputCount,
    TransformedVertex* output,
    float nx, float ny, float nz, float nw)
{
    if (inputCount < 1) return 0;
    
    int outCount = 0;
    
    auto Distance = [nx, ny, nz, nw](const TransformedVertex& v) -> float {
        return nx * v.x + ny * v.y + nz * v.z + nw * v.w;
    };
    
    for (int i = 0; i < inputCount; i++) {
        int next = (i + 1) % inputCount;
        
        float dCurrent = Distance(input[i]);
        float dNext    = Distance(input[next]);
        
        bool currentInside = (dCurrent >= 0.0f);
        bool nextInside    = (dNext >= 0.0f);
        
        if (currentInside) {
            output[outCount++] = input[i];
            if (outCount >= 10) return outCount;
        }
        
        if (currentInside != nextInside) {
            // Edge crosses plane — compute intersection
            float t = dCurrent / (dCurrent - dNext);
            t = std::clamp(t, 0.0f, 1.0f);
            output[outCount++] = LerpVertex(input[i], input[next], t);
            if (outCount >= 10) return outCount;
        }
    }
    
    return outCount;
}

int ClipTriangle(
    const TransformedVertex& v0,
    const TransformedVertex& v1,
    const TransformedVertex& v2,
    TransformedVertex* output,
    int /*maxOutput*/)
{
    // Quick reject: all vertices outside same plane
    uint32_t andFlags = v0.clipFlags & v1.clipFlags & v2.clipFlags;
    if (andFlags != 0) return 0;
    
    // Quick accept: all vertices inside
    uint32_t orFlags = v0.clipFlags | v1.clipFlags | v2.clipFlags;
    if (orFlags == 0) {
        output[0] = v0;
        output[1] = v1;
        output[2] = v2;
        return 3;
    }
    
    // Sutherland-Hodgman clipping against 7 planes (6 frustum + w>0)
    TransformedVertex temp1[10];
    TransformedVertex temp2[10];
    
    temp1[0] = v0;
    temp1[1] = v1;
    temp1[2] = v2;
    int count = 3;
    
    // W > epsilon (near-zero W guard)
    if (orFlags & TransformedVertex::CLIP_W) {
        count = ClipAgainstPlane(temp1, count, temp2, 0, 0, 0, 1);
        if (count < 3) return 0;
        std::memcpy(temp1, temp2, count * sizeof(TransformedVertex));
    }
    
    // Left: x >= -w  →  (1, 0, 0, 1) · v >= 0
    if (orFlags & TransformedVertex::CLIP_LEFT) {
        count = ClipAgainstPlane(temp1, count, temp2, 1, 0, 0, 1);
        if (count < 3) return 0;
        std::memcpy(temp1, temp2, count * sizeof(TransformedVertex));
    }
    
    // Right: x <= w  →  (-1, 0, 0, 1) · v >= 0
    if (orFlags & TransformedVertex::CLIP_RIGHT) {
        count = ClipAgainstPlane(temp1, count, temp2, -1, 0, 0, 1);
        if (count < 3) return 0;
        std::memcpy(temp1, temp2, count * sizeof(TransformedVertex));
    }
    
    // Bottom: y >= -w
    if (orFlags & TransformedVertex::CLIP_BOTTOM) {
        count = ClipAgainstPlane(temp1, count, temp2, 0, 1, 0, 1);
        if (count < 3) return 0;
        std::memcpy(temp1, temp2, count * sizeof(TransformedVertex));
    }
    
    // Top: y <= w
    if (orFlags & TransformedVertex::CLIP_TOP) {
        count = ClipAgainstPlane(temp1, count, temp2, 0, -1, 0, 1);
        if (count < 3) return 0;
        std::memcpy(temp1, temp2, count * sizeof(TransformedVertex));
    }
    
    // Near: z >= -w
    if (orFlags & TransformedVertex::CLIP_NEAR) {
        count = ClipAgainstPlane(temp1, count, temp2, 0, 0, 1, 1);
        if (count < 3) return 0;
        std::memcpy(temp1, temp2, count * sizeof(TransformedVertex));
    }
    
    // Far: z <= w
    if (orFlags & TransformedVertex::CLIP_FAR) {
        count = ClipAgainstPlane(temp1, count, temp2, 0, 0, -1, 1);
        if (count < 3) return 0;
        std::memcpy(temp1, temp2, count * sizeof(TransformedVertex));
    }
    
    std::memcpy(output, temp1, count * sizeof(TransformedVertex));
    return count;
}

float TriangleArea2D(float x0, float y0, float x1, float y1, float x2, float y2) {
    return (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
}

bool IsFrontFacing(const TransformedVertex& v0, const TransformedVertex& v1,
                   const TransformedVertex& v2) {
    return TriangleArea2D(v0.screenX, v0.screenY, v1.screenX, v1.screenY,
                          v2.screenX, v2.screenY) > 0.0f;
}

} // namespace Clipping

// ═══════════════════════════════════════════════════════════════════════════
//  Lighting
// ═══════════════════════════════════════════════════════════════════════════

namespace Lighting {

void ComputeLighting(
    const Vec3f& normal,
    const N64Light* lights,
    int numLights,
    const N64Light& ambient,
    float& outR, float& outG, float& outB)
{
    // Start with ambient light
    outR = static_cast<float>(ambient.r) / 255.0f;
    outG = static_cast<float>(ambient.g) / 255.0f;
    outB = static_cast<float>(ambient.b) / 255.0f;
    
    // Add contribution from each directional light
    for (int i = 0; i < numLights; i++) {
        const N64Light& light = lights[i];
        
        // Light direction is signed byte normalized to [-128, 127] → [-1, 1]
        Vec3f dir{
            static_cast<float>(light.dirX) / 127.0f,
            static_cast<float>(light.dirY) / 127.0f,
            static_cast<float>(light.dirZ) / 127.0f
        };
        
        float dot = normal.Dot(dir);
        if (dot > 0.0f) {
            outR += dot * static_cast<float>(light.r) / 255.0f;
            outG += dot * static_cast<float>(light.g) / 255.0f;
            outB += dot * static_cast<float>(light.b) / 255.0f;
        }
    }
    
    // Clamp to [0, 1]
    outR = std::clamp(outR, 0.0f, 1.0f);
    outG = std::clamp(outG, 0.0f, 1.0f);
    outB = std::clamp(outB, 0.0f, 1.0f);
}

void BatchLighting(
    const Vec3f* normals,
    int count,
    const Matrix4x4& modelView,
    const N64Light* lights,
    int numLights,
    const N64Light& ambient,
    float* outColors)
{
    for (int i = 0; i < count; i++) {
        // Transform normal to eye space
        Vec3f eyeNormal = MatrixOps::TransformNormal(modelView, normals[i]);
        
        float r, g, b;
        ComputeLighting(eyeNormal, lights, numLights, ambient, r, g, b);
        
        outColors[i * 3 + 0] = r;
        outColors[i * 3 + 1] = g;
        outColors[i * 3 + 2] = b;
    }
}

} // namespace Lighting

// ═══════════════════════════════════════════════════════════════════════════
//  Texture Coordinate Generation
// ═══════════════════════════════════════════════════════════════════════════

namespace TexGen {

void EnvironmentMap(const Vec3f& eyeSpaceNormal, float& outS, float& outT) {
    // Environment mapping: S = acos(Nx)/π, T = acos(Ny)/π
    // Simplified: S = Nx * 0.5 + 0.5, T = Ny * 0.5 + 0.5
    outS = eyeSpaceNormal.x * 0.5f + 0.5f;
    outT = eyeSpaceNormal.y * 0.5f + 0.5f;
    
    // Scale to texture coordinate range
    outS *= 1024.0f;
    outT *= 1024.0f;
}

void LinearEnvironmentMap(const Vec3f& eyeSpaceNormal, float& outS, float& outT) {
    // Linear version: angle-based
    float nx = std::clamp(eyeSpaceNormal.x, -1.0f, 1.0f);
    float ny = std::clamp(eyeSpaceNormal.y, -1.0f, 1.0f);
    
    outS = std::acos(nx) * (1024.0f / 3.14159265f);
    outT = std::acos(ny) * (1024.0f / 3.14159265f);
}

void ApplyTextureScale(float& s, float& t, uint16_t scaleS, uint16_t scaleT) {
    s *= static_cast<float>(scaleS) / 65536.0f;
    t *= static_cast<float>(scaleT) / 65536.0f;
}

void ApplyTileShift(float& s, float& t, uint8_t shiftS, uint8_t shiftT) {
    if (shiftS > 0) {
        if (shiftS <= 10)
            s = std::ldexp(s, -static_cast<int>(shiftS));
        else
            s = std::ldexp(s, 16 - static_cast<int>(shiftS));
    }
    if (shiftT > 0) {
        if (shiftT <= 10)
            t = std::ldexp(t, -static_cast<int>(shiftT));
        else
            t = std::ldexp(t, 16 - static_cast<int>(shiftT));
    }
}

float WrapCoord(float coord, uint8_t clamp, uint8_t mirror, uint8_t mask) {
    if (mask == 0) return coord;  // No wrapping
    
    int icoord = static_cast<int>(coord);
    int size = 1 << mask;
    
    if (clamp) {
        icoord = std::clamp(icoord, 0, size - 1);
    } else if (mirror) {
        int wrapped = icoord & ((size << 1) - 1);
        if (wrapped >= size)
            wrapped = (size << 1) - 1 - wrapped;
        icoord = wrapped;
    } else {
        icoord = icoord & (size - 1);
    }
    
    return static_cast<float>(icoord);
}

} // namespace TexGen

// ═══════════════════════════════════════════════════════════════════════════
//  Fog
// ═══════════════════════════════════════════════════════════════════════════

namespace FogMath {

float ComputeFogFactor(float w, int16_t fogMultiplier, int16_t fogOffset) {
    // N64 fog formula: fog = clamp((w * multiplier + offset) / 255, 0, 255) / 255
    float fog = (w * static_cast<float>(fogMultiplier) + static_cast<float>(fogOffset)) / 255.0f;
    return std::clamp(fog, 0.0f, 1.0f);
}

void ApplyFog(float& r, float& g, float& b,
              float fogFactor,
              const RDPColor& fogColor)
{
    float fr = static_cast<float>(fogColor.r) / 255.0f;
    float fg = static_cast<float>(fogColor.g) / 255.0f;
    float fb = static_cast<float>(fogColor.b) / 255.0f;
    
    r = r * (1.0f - fogFactor) + fr * fogFactor;
    g = g * (1.0f - fogFactor) + fg * fogFactor;
    b = b * (1.0f - fogFactor) + fb * fogFactor;
}

} // namespace FogMath

// ═══════════════════════════════════════════════════════════════════════════
//  Depth
// ═══════════════════════════════════════════════════════════════════════════

namespace DepthMath {

uint32_t ClipZToN64Z(float clipZ, float clipW) {
    if (std::abs(clipW) < 1e-7f) return 0;
    
    // Perspective divide to NDC: z_ndc ∈ [-1, 1]
    float zNDC = clipZ / clipW;
    
    // Map to [0, 1]
    float z01 = (zNDC + 1.0f) * 0.5f;
    z01 = std::clamp(z01, 0.0f, 1.0f);
    
    // N64 Z-buffer is 18-bit (s15.3 format, range 0x0000-0x3FFFF)
    return static_cast<uint32_t>(z01 * 0x3FFFF);
}

float N64ZToFloat(uint32_t n64z) {
    return static_cast<float>(n64z & 0x3FFFF) / static_cast<float>(0x3FFFF);
}

uint16_t ComputeDeltaZ(float dzDx, float dzDy, float dzDe) {
    float maxDZ = std::max({std::abs(dzDx), std::abs(dzDy), std::abs(dzDe)});
    
    // Find highest set bit to determine dz value
    if (maxDZ < 1.0f) return 0;
    
    int exp = 0;
    float v = maxDZ;
    while (v >= 2.0f && exp < 15) {
        v /= 2.0f;
        exp++;
    }
    
    return static_cast<uint16_t>(1 << exp);
}

bool ZCompare(uint32_t newZ, uint32_t oldZ, uint16_t newDZ, uint16_t oldDZ, ZMode mode) {
    switch (mode) {
        case ZMode::Opaque:
            return newZ < oldZ;
            
        case ZMode::Interpenetrate: {
            uint16_t maxDZ = std::max(newDZ, oldDZ);
            return (newZ + maxDZ) >= oldZ;
        }
            
        case ZMode::Transparent:
            return newZ < oldZ;
            
        case ZMode::Decal: {
            uint16_t maxDZ = std::max(newDZ, oldDZ);
            int32_t diff = static_cast<int32_t>(newZ) - static_cast<int32_t>(oldZ);
            return std::abs(diff) <= maxDZ;
        }
            
        default:
            return newZ < oldZ;
    }
}

} // namespace DepthMath

// ═══════════════════════════════════════════════════════════════════════════
//  Look-Up Tables
// ═══════════════════════════════════════════════════════════════════════════

namespace MathLUT {

float ReciprocalTable[1024];
float InvSqrtTable[512];
uint32_t ZEncodeTable[0x40000];
float    ZDecodeTable[0x40000];
float    FastRecipTable[2048];

void Initialize() {
    // Reciprocal table (similar to RSP VRCPH/VRCPL)
    for (int i = 0; i < 1024; i++) {
        float val = static_cast<float>(i + 512);
        ReciprocalTable[i] = 1.0f / val;
    }
    
    // Inverse square root table
    for (int i = 0; i < 512; i++) {
        float val = static_cast<float>(i + 256);
        InvSqrtTable[i] = 1.0f / std::sqrt(val);
    }
    
    // Z encode/decode tables
    // N64's Z-buffer uses a piecewise linear encoding with 8 segments
    // Each segment has decreasing precision for larger Z values
    for (uint32_t i = 0; i < 0x40000; i++) {
        float z01 = static_cast<float>(i) / static_cast<float>(0x3FFFF);
        ZEncodeTable[i] = i;  // Identity for now; real HW uses non-linear encoding
        ZDecodeTable[i] = z01;
    }
    
    // Fast reciprocal for 11-bit values
    FastRecipTable[0] = 0.0f;
    for (int i = 1; i < 2048; i++) {
        FastRecipTable[i] = 1.0f / static_cast<float>(i);
    }
}

} // namespace MathLUT

} // namespace Nova64::Video
