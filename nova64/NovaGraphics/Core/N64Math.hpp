/**
 * @file N64Math.hpp
 * @brief Fixed-point math, matrix operations, and vector math for N64 emulation
 *
 * Provides all the mathematical operations needed for vertex transformation,
 * lighting, texture coordinate generation, and clipping in the N64 RDP pipeline.
 * Includes both scalar and SIMD-accelerated implementations.
 */

#pragma once

#include "N64Types.hpp"
#include "../Common/Platform.hpp"

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <array>

#ifdef _MSC_VER
    #include <intrin.h>
#endif

#if defined(__SSE2__)
    #include <emmintrin.h>
#endif
#if defined(__SSE4_1__)
    #include <smmintrin.h>
#endif
#if defined(__AVX__)
    #include <immintrin.h>
#endif

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════════════════
//  Fixed-Point Conversion Helpers
// ═══════════════════════════════════════════════════════════════════════════

namespace FixedPoint {

/// Convert s15.16 fixed-point integer to float
constexpr float S15_16_ToFloat(int32_t value) {
    return static_cast<float>(value) / 65536.0f;
}

/// Convert float to s15.16 fixed-point
constexpr int32_t FloatTo_S15_16(float value) {
    return static_cast<int32_t>(value * 65536.0f);
}

/// Convert s10.5 fixed-point to float (texture coordinates)
constexpr float S10_5_ToFloat(int16_t value) {
    return static_cast<float>(value) / 32.0f;
}

/// Convert s10.2 fixed-point to float (scissor/rect coordinates)
constexpr float S10_2_ToFloat(uint16_t value) {
    return static_cast<float>(value) / 4.0f;
}

/// Convert s.15 for VP scale/translate in Vp_t (2-byte, /4)
constexpr float VP_ToFloat(int16_t value) {
    return static_cast<float>(value) / 4.0f;
}

/// Convert 5.10 unsigned fixed-point (texture scale)
constexpr float U5_10_ToFloat(uint16_t value) {
    return static_cast<float>(value) / 1024.0f;
}

/// Convert s11.2 (triangle edge coefficients)
constexpr float S11_2_ToFloat(int32_t value) {
    return static_cast<float>(value) / 4.0f;
}

/// Convert 16.16 unsigned (DsDx, DtDy for texture rect)
constexpr float U16_16_ToFloat(int32_t value) {
    return static_cast<float>(value) / 65536.0f;
}

} // namespace FixedPoint

// ═══════════════════════════════════════════════════════════════════════════
//  Vector Math (float4)
// ═══════════════════════════════════════════════════════════════════════════

struct Vec2f {
    float x, y;
    
    Vec2f() : x(0), y(0) {}
    Vec2f(float x_, float y_) : x(x_), y(y_) {}
    
    Vec2f operator+(const Vec2f& o) const { return { x+o.x, y+o.y }; }
    Vec2f operator-(const Vec2f& o) const { return { x-o.x, y-o.y }; }
    Vec2f operator*(float s) const { return { x*s, y*s }; }
    
    float Dot(const Vec2f& o) const { return x*o.x + y*o.y; }
    float Cross(const Vec2f& o) const { return x*o.y - y*o.x; }
    float Length() const { return std::sqrt(x*x + y*y); }
};

struct Vec3f {
    float x, y, z;
    
    Vec3f() : x(0), y(0), z(0) {}
    Vec3f(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    
    Vec3f operator+(const Vec3f& o) const { return { x+o.x, y+o.y, z+o.z }; }
    Vec3f operator-(const Vec3f& o) const { return { x-o.x, y-o.y, z-o.z }; }
    Vec3f operator*(float s) const { return { x*s, y*s, z*s }; }
    
    float Dot(const Vec3f& o) const { return x*o.x + y*o.y + z*o.z; }
    Vec3f Cross(const Vec3f& o) const {
        return { y*o.z - z*o.y, z*o.x - x*o.z, x*o.y - y*o.x };
    }
    float Length() const { return std::sqrt(x*x + y*y + z*z); }
    Vec3f Normalized() const {
        float len = Length();
        if (len < 1e-7f) return { 0, 0, 0 };
        float inv = 1.0f / len;
        return { x*inv, y*inv, z*inv };
    }
};

struct Vec4f {
    float x, y, z, w;
    
    Vec4f() : x(0), y(0), z(0), w(1) {}
    Vec4f(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    Vec4f(const Vec3f& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    
    Vec4f operator+(const Vec4f& o) const { return { x+o.x, y+o.y, z+o.z, w+o.w }; }
    Vec4f operator-(const Vec4f& o) const { return { x-o.x, y-o.y, z-o.z, w-o.w }; }
    Vec4f operator*(float s) const { return { x*s, y*s, z*s, w*s }; }
    
    float Dot(const Vec4f& o) const { return x*o.x + y*o.y + z*o.z + w*o.w; }
    
    Vec3f XYZ() const { return { x, y, z }; }
    
    /// Perspective divide: returns (x/w, y/w, z/w)
    Vec3f PerspectiveDivide() const {
        if (std::abs(w) < 1e-7f) return { 0, 0, 0 };
        float inv = 1.0f / w;
        return { x*inv, y*inv, z*inv };
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Matrix Operations
// ═══════════════════════════════════════════════════════════════════════════

namespace MatrixOps {

/// Multiply two 4x4 matrices: result = A * B
void Multiply(const Matrix4x4& a, const Matrix4x4& b, Matrix4x4& result);

/// Multiply with SIMD acceleration (SSE4.1)
void MultiplySSE(const Matrix4x4& a, const Matrix4x4& b, Matrix4x4& result);

/// Transpose a 4x4 matrix
void Transpose(const Matrix4x4& src, Matrix4x4& dst);

/// Load N64 matrix from RDRAM (interleaved integer + fraction format)
/// N64 stores matrices as:
///   Bytes 0-31:  integer parts  (4x4 x int16)
///   Bytes 32-63: fraction parts (4x4 x uint16)
void LoadFromN64(const uint8_t* rdram, uint32_t address, Matrix4x4& out);

/// Transform a vertex through modelview-projection matrix
Vec4f TransformVertex(const Matrix4x4& mvp, const Vec3f& position);

/// Transform a normal through model matrix (upper 3x3 only)
Vec3f TransformNormal(const Matrix4x4& model, const Vec3f& normal);

/// Batch transform N vertices (scalar fallback)
void BatchTransformVertices(
    const Matrix4x4& mvp,
    const int16_t* positions,   // Packed xyz, stride = 16 bytes (N64Vertex)
    float* outClipSpace,        // xyzw per vertex
    int count,
    int srcStride = sizeof(N64Vertex)
);

/// Batch transform with SSE (4 vertices at a time)
void BatchTransformVerticesSSE(
    const Matrix4x4& mvp,
    const int16_t* positions,
    float* outClipSpace,
    int count,
    int srcStride = sizeof(N64Vertex)
);

/// Batch transform with AVX (8 components at a time)
void BatchTransformVerticesAVX(
    const Matrix4x4& mvp,
    const int16_t* positions,
    float* outClipSpace,
    int count,
    int srcStride = sizeof(N64Vertex)
);

/// Runtime dispatch: picks the best SIMD path
void BatchTransformVerticesDispatch(
    const Matrix4x4& mvp,
    const int16_t* positions,
    float* outClipSpace,
    int count,
    int srcStride = sizeof(N64Vertex)
);

} // namespace MatrixOps

// ═══════════════════════════════════════════════════════════════════════════
//  Clipping
// ═══════════════════════════════════════════════════════════════════════════

namespace Clipping {

/// Compute clip flags for a vertex in clip space (before W divide)
uint32_t ComputeClipFlags(const Vec4f& clipPos);

/// Sutherland-Hodgman clip against all 6 frustum planes + W > 0
/// Returns the number of output vertices (0 = fully clipped)
int ClipTriangle(
    const TransformedVertex& v0,
    const TransformedVertex& v1,
    const TransformedVertex& v2,
    TransformedVertex* output,
    int maxOutput = 10
);

/// Clip against a single plane defined by (nx, ny, nz, nw) · vertex >= 0
int ClipAgainstPlane(
    const TransformedVertex* input, int inputCount,
    TransformedVertex* output,
    float nx, float ny, float nz, float nw
);

/// Linearly interpolate two transformed vertices along plane intersection
TransformedVertex LerpVertex(
    const TransformedVertex& a,
    const TransformedVertex& b,
    float t
);

/// Compute screen-space triangle area (2x signed area, for winding check)
float TriangleArea2D(float x0, float y0, float x1, float y1, float x2, float y2);

/// Check if triangle is front-facing (for backface culling)
bool IsFrontFacing(const TransformedVertex& v0, const TransformedVertex& v1,
                   const TransformedVertex& v2);

} // namespace Clipping

// ═══════════════════════════════════════════════════════════════════════════
//  Lighting
// ═══════════════════════════════════════════════════════════════════════════

namespace Lighting {

/// Compute vertex color from N64 lights using normals
void ComputeLighting(
    const Vec3f& normal,            // Object-space normal (normalized)
    const N64Light* lights,         // Array of lights 
    int numLights,                  // Number of directional lights (excl ambient)
    const N64Light& ambient,        // Ambient light
    float& outR, float& outG, float& outB
);

/// Batch lighting for multiple vertices
void BatchLighting(
    const Vec3f* normals,
    int count,
    const Matrix4x4& modelView,     // To transform normals
    const N64Light* lights,
    int numLights,
    const N64Light& ambient,
    float* outColors                 // RGB triplets
);

} // namespace Lighting

// ═══════════════════════════════════════════════════════════════════════════
//  Texture Coordinate Generation
// ═══════════════════════════════════════════════════════════════════════════

namespace TexGen {

/// Environment mapping: compute S,T from transformed normal
void EnvironmentMap(
    const Vec3f& eyeSpaceNormal,
    float& outS, float& outT
);

/// Linear environment mapping (G_TEXTURE_GEN_LINEAR)
void LinearEnvironmentMap(
    const Vec3f& eyeSpaceNormal,
    float& outS, float& outT
);

/// Apply texture scale from gSPTexture
void ApplyTextureScale(
    float& s, float& t,
    uint16_t scaleS, uint16_t scaleT
);

/// Apply tile shift
void ApplyTileShift(
    float& s, float& t,
    uint8_t shiftS, uint8_t shiftT
);

/// Clamp/Mirror/Wrap texture coordinate
float WrapCoord(float coord, uint8_t clamp, uint8_t mirror, uint8_t mask);

} // namespace TexGen

// ═══════════════════════════════════════════════════════════════════════════
//  Fog
// ═══════════════════════════════════════════════════════════════════════════

namespace FogMath {

/// Compute fog factor from vertex W coordinate
/// Returns 0.0 (no fog) to 1.0 (full fog)
float ComputeFogFactor(float w, int16_t fogMultiplier, int16_t fogOffset);

/// Apply fog to a color
void ApplyFog(float& r, float& g, float& b,
              float fogFactor,
              const RDPColor& fogColor);

} // namespace FogMath

// ═══════════════════════════════════════════════════════════════════════════
//  Depth
// ═══════════════════════════════════════════════════════════════════════════

namespace DepthMath {

/// Convert clip-space Z to N64 Z-buffer value (18-bit: 15.3)
uint32_t ClipZToN64Z(float clipZ, float clipW);

/// Convert N64 Z-buffer value back to float depth
float N64ZToFloat(uint32_t n64z);

/// Compute delta Z for polygon rasterization
uint16_t ComputeDeltaZ(float dzDx, float dzDy, float dzDe);

/// Z-buffer compare function
bool ZCompare(uint32_t newZ, uint32_t oldZ, uint16_t newDZ, uint16_t oldDZ, ZMode mode);

} // namespace DepthMath

// ═══════════════════════════════════════════════════════════════════════════
//  Look-Up Tables
// ═══════════════════════════════════════════════════════════════════════════

namespace MathLUT {

/// Initialize all lookup tables
void Initialize();

/// Reciprocal table for W division (similar to RSP's VRCPH/VRCPL)
extern float ReciprocalTable[1024];

/// Inverse square root table
extern float InvSqrtTable[512];

/// Z encode/decode tables
extern uint32_t ZEncodeTable[0x40000];
extern float    ZDecodeTable[0x40000];

/// Fast 1/x for values 0-2047 (11-bit, used in texture coords)
extern float FastRecipTable[2048];

} // namespace MathLUT

} // namespace Nova64::Video
