/**
 * @file ClipMath.hpp
 * @brief N64-specific clipping, culling and viewport transform routines for the RDP pipeline.
 */
// SPDX-License-Identifier: MIT

#pragma once

#include "VectorMath.hpp"
#include "SIMDMath.hpp"
#include <cstdint>
#include <array>

namespace Nova64 {
namespace Video {
namespace Clip {

using Math::Color4f;
using Math::Vec2f;
using Math::Vec3f;
using SIMD::Mat4x4;
using SIMD::Vec4f;

// --- Clip Flags ---
enum ClipFlag : uint32_t {
    CLIP_NONE = 0x00,
    CLIP_POSX = 0x01, // x > +w
    CLIP_NEGX = 0x02, // x < -w
    CLIP_POSY = 0x04, // y > +w
    CLIP_NEGY = 0x08, // y < -w
    CLIP_POSZ = 0x10, // z > +w (far)
    CLIP_NEGZ = 0x20, // z < -w (near, or z < 0 for N64)
    CLIP_W = 0x40,    // w <= 0 (behind camera)
    CLIP_FAR = 0x80,  // additional far clip
    CLIP_ALL = 0x3F   // standard 6-plane clip
};

// --- Clip Planes ---
enum ClipPlane : int {
    PLANE_NEAR = 0,     // z >= -w  (GL near plane convention)
    PLANE_FAR = 1,      // z <= w
    PLANE_LEFT = 2,     // x >= -w
    PLANE_RIGHT = 3,    // x <= w
    PLANE_BOTTOM = 4,   // y >= -w
    PLANE_TOP = 5,      // y <= w
    PLANE_W_NEAR = 6,   // w >= epsilon (behind-camera guard plane)
    PLANE_NEAR_N64 = 7, // z >= 0 (N64 NoN near plane — F3DEX2_NoN/F3DZEX)
    PLANE_COUNT = 8
};

// --- Clipped Vertex (full interpolation data) ---
struct ClipVertex {
    Vec4f position;     // clip-space XYZW
    Vec4f color;        // RGBA [0..1]
    Vec2f texCoord;     // S, T  (tile 0)
    Vec2f texCoord1;    // S1, T1 (tile 1)
    float fog;          // fog factor [0..1]
    uint32_t clipFlags; // combined ClipFlag bits

    ClipVertex() : fog(0.0f), clipFlags(0) {}
};

// --- Clipped Polygon Result ---
static constexpr int MAX_CLIPPED_VERTS = 12; // 3 inputs + at most 1 per clip plane (7) = 10, rounded up

struct ClippedPolygon {
    ClipVertex vertices[MAX_CLIPPED_VERTS];
    int vertexCount;

    ClippedPolygon() : vertexCount(0) {}
};

// --- Scissor Rectangle ---
struct ScissorRect {
    int x0, y0, x1, y1;

    ScissorRect() : x0(0), y0(0), x1(320), y1(240) {}
    ScissorRect(int _x0, int _y0, int _x1, int _y1) : x0(_x0), y0(_y0), x1(_x1), y1(_y1) {}

    int Width() const { return x1 - x0; }
    int Height() const { return y1 - y0; }
    bool IsEmpty() const { return x0 >= x1 || y0 >= y1; }

    bool Contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }

    ScissorRect Intersect(const ScissorRect& other) const {
        return ScissorRect(std::max(x0, other.x0), std::max(y0, other.y0), std::min(x1, other.x1),
                           std::min(y1, other.y1));
    }
};

// --- CLIP FLAG COMPUTATION ---

/// Clip flags of a clip-space vertex, with the near plane at z = -w.
uint32_t ComputeClipFlags(const Vec4f& clipPos);

// --- TRIVIAL REJECTION / ACCEPTANCE ---

/// True if all three vertices are outside the same clip plane.
inline bool TrivialReject(uint32_t flagV0, uint32_t flagV1, uint32_t flagV2) { return (flagV0 & flagV1 & flagV2) != 0; }

/// True if all three vertices are inside every clip plane.
inline bool TrivialAccept(uint32_t flagV0, uint32_t flagV1, uint32_t flagV2) { return (flagV0 | flagV1 | flagV2) == 0; }

// --- SUTHERLAND-HODGMAN POLYGON CLIPPING ---

/// Clips a triangle against the frustum (z >= 0 near plane, w near guard, far, and the four sides).
/// `result` receives the clipped convex polygon; the return value is the number of triangles it
/// fans into (0 if nothing remains). `clipRatio` scales the W bound of the X/Y planes to widen them
/// into a guard band (1.0 = exact frustum).
int ClipTriangle(const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2, ClippedPolygon& result,
                 float clipRatio = 1.0f);

/// Clips a polygon against a single plane and returns the vertex count of the result (0 if fewer
/// than 3 vertices come in). `clipRatio` scales the W comparand for the X/Y planes (guard band).
int ClipPolygonAgainstPlane(const ClipVertex* vertsIn, int vertCount, ClipVertex* vertsOut, ClipPlane plane,
                            float clipRatio = 1.0f);

// --- GUARD BAND CLIPPING ---

// A guard band lets triangles slightly outside the screen pass without clipping; the
// rasterizer or hardware scissor cuts them instead.
struct GuardBand {
    float left, right, top, bottom;

    GuardBand() : left(-4096.0f), right(4096.0f), top(-4096.0f), bottom(4096.0f) {}
    GuardBand(float l, float r, float t, float b) : left(l), right(r), top(t), bottom(b) {}

    static GuardBand ForResolution(int screenWidth, int screenHeight) {
        // Standard guard band: 2x screen dimensions
        float gbW = static_cast<float>(screenWidth);
        float gbH = static_cast<float>(screenHeight);
        return GuardBand(-gbW, gbW * 2.0f, -gbH, gbH * 2.0f);
    }
};

// --- SCREEN-SPACE CLIPPING (2D) ---

struct ScreenVertex {
    Vec2f position; // screen-space X, Y
    float depth;    // for z-buffer
    float invW;     // 1/W for perspective correction
    Color4f color;
    Vec2f texCoord;
    float fog;
};

static constexpr int MAX_SCREEN_CLIPPED = 12;

// --- BACKFACE CULLING ---

// --- PERSPECTIVE DIVISION AND VIEWPORT TRANSFORM ---

// --- BRANCH_Z TEST (N64 specific) ---

// --- COMPLETE TRIANGLE PIPELINE ---

static constexpr int MAX_OUTPUT_TRIANGLES = 8; // a clipped polygon of up to 10 vertices fans into 8 triangles

} // namespace Clip
} // namespace Video
} // namespace Nova64
