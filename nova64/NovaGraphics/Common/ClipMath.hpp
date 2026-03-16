// SPDX-License-Identifier: MIT
// NovaGraphics - Clipping Mathematics
// N64-specific clipping routines for the RDP pipeline

#pragma once

#include "VectorMath.hpp"
#include "SIMDMath.hpp"
#include <cstdint>
#include <array>

namespace Nova64 { namespace Video { namespace Clip {

using Math::Vec2f;
using Math::Vec3f;
using Math::Color4f;
using SIMD::Vec4f;
using SIMD::Mat4x4;

// ─── Clip Flags ──────────────────────────────────────────────────────────────
enum ClipFlag : uint32_t {
    CLIP_NONE   = 0x00,
    CLIP_POSX   = 0x01,  // x > +w
    CLIP_NEGX   = 0x02,  // x < -w
    CLIP_POSY   = 0x04,  // y > +w
    CLIP_NEGY   = 0x08,  // y < -w
    CLIP_POSZ   = 0x10,  // z > +w (far)
    CLIP_NEGZ   = 0x20,  // z < -w (near, or z < 0 for N64)
    CLIP_W      = 0x40,  // w <= 0 (behind camera)
    CLIP_FAR    = 0x80,  // additional far clip
    CLIP_ALL    = 0x3F   // standard 6-plane clip
};

// ─── Clip Planes ─────────────────────────────────────────────────────────────
enum ClipPlane : int {
    PLANE_NEAR   = 0,  // z >= -w  (or z >= 0 for N64 near plane convention)
    PLANE_FAR    = 1,  // z <= w
    PLANE_LEFT   = 2,  // x >= -w
    PLANE_RIGHT  = 3,  // x <= w
    PLANE_BOTTOM = 4,  // y >= -w
    PLANE_TOP    = 5,  // y <= w
    PLANE_COUNT  = 6
};

// ─── Clipped Vertex (full interpolation data) ────────────────────────────────
struct ClipVertex {
    Vec4f position;     // clip-space XYZW
    Vec4f color;        // RGBA [0..1]
    Vec2f texCoord;     // S, T  (tile 0)
    Vec2f texCoord1;    // S1, T1 (tile 1)
    float fog;          // fog factor [0..1]
    uint32_t clipFlags; // combined ClipFlag bits

    ClipVertex() : fog(0.0f), clipFlags(0) {}
};

// ─── Clipped Polygon Result ──────────────────────────────────────────────────
static constexpr int MAX_CLIPPED_VERTS = 12; // 3 input + up to 6 planes = max 9, round to 12

struct ClippedPolygon {
    ClipVertex vertices[MAX_CLIPPED_VERTS];
    int vertexCount;

    ClippedPolygon() : vertexCount(0) {}
};

// ─── Scissor Rectangle ──────────────────────────────────────────────────────
struct ScissorRect {
    int x0, y0, x1, y1;

    ScissorRect() : x0(0), y0(0), x1(320), y1(240) {}
    ScissorRect(int _x0, int _y0, int _x1, int _y1) : x0(_x0), y0(_y0), x1(_x1), y1(_y1) {}

    int Width() const  { return x1 - x0; }
    int Height() const { return y1 - y0; }
    bool IsEmpty() const { return x0 >= x1 || y0 >= y1; }

    bool Contains(int x, int y) const {
        return x >= x0 && x < x1 && y >= y0 && y < y1;
    }

    ScissorRect Intersect(const ScissorRect& other) const {
        return ScissorRect(
            std::max(x0, other.x0), std::max(y0, other.y0),
            std::min(x1, other.x1), std::min(y1, other.y1)
        );
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
// CLIP FLAG COMPUTATION
// ═══════════════════════════════════════════════════════════════════════════════

// Compute clip flags for a single clip-space vertex
uint32_t ComputeClipFlags(const Vec4f& clipPos);

// Compute clip flags for N64 convention (near plane at z=0 instead of z=-w)
uint32_t ComputeClipFlagsN64(const Vec4f& clipPos);

// Compute clip flags for a batch of vertices
void ComputeClipFlagsBatch(const Vec4f* clipPositions, uint32_t* flags, int count);

// ═══════════════════════════════════════════════════════════════════════════════
// TRIVIAL REJECTION / ACCEPTANCE
// ═══════════════════════════════════════════════════════════════════════════════

// Triangle trivial rejection: all vertices on same side of at least one plane
inline bool TrivialReject(uint32_t flagV0, uint32_t flagV1, uint32_t flagV2) {
    return (flagV0 & flagV1 & flagV2) != 0;
}

// Triangle trivial acceptance: all vertices inside all planes
inline bool TrivialAccept(uint32_t flagV0, uint32_t flagV1, uint32_t flagV2) {
    return (flagV0 | flagV1 | flagV2) == 0;
}

// Cull display list range: if all vertices share a clip flag, entire DL can be skipped
bool CullVertexRange(const uint32_t* clipFlags, int startIdx, int endIdx);

// ═══════════════════════════════════════════════════════════════════════════════
// SUTHERLAND-HODGMAN POLYGON CLIPPING
// ═══════════════════════════════════════════════════════════════════════════════

// Clip a triangle against all 6 frustum planes
// Returns number of output triangles (0 if fully clipped, 1+ if clipped polygon needs triangulation)
int ClipTriangle(const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2,
                 ClippedPolygon& result);

// Clip a polygon against a single plane
// Returns vertex count of clipped polygon
int ClipPolygonAgainstPlane(const ClipVertex* vertsIn, int vertCount,
                            ClipVertex* vertsOut, ClipPlane plane);

// Clip against near plane only (most common clip operation)
int ClipTriangleNearPlane(const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2,
                          ClippedPolygon& result);

// Clip against N64 near plane (z = 0 instead of z = -w)
int ClipTriangleN64Near(const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2,
                        ClippedPolygon& result);

// ═══════════════════════════════════════════════════════════════════════════════
// GUARD BAND CLIPPING
// ═══════════════════════════════════════════════════════════════════════════════

// Guard band allows triangles slightly outside screen to pass without clipping
// (they'll be clipped by the rasterizer/hardware scissor instead)
struct GuardBand {
    float left, right, top, bottom;

    GuardBand() : left(-4096.0f), right(4096.0f), top(-4096.0f), bottom(4096.0f) {}
    GuardBand(float l, float r, float t, float b) : left(l), right(r), top(t), bottom(b) {}

    // Configure guard band based on screen resolution
    static GuardBand ForResolution(int screenWidth, int screenHeight) {
        // Standard guard band: 2x screen dimensions
        float gbW = static_cast<float>(screenWidth);
        float gbH = static_cast<float>(screenHeight);
        return GuardBand(-gbW, gbW * 2.0f, -gbH, gbH * 2.0f);
    }
};

// Test if vertex is inside guard band (only needs X/Y clip, not Z)
bool InsideGuardBand(const Vec4f& clipPos, const GuardBand& gb);

// Compute guard band clip flags (for X/Y; Z uses standard clip)
uint32_t ComputeGuardBandFlags(const Vec4f& clipPos, const GuardBand& gb);

// Clip triangle against guard band (only clips X/Y planes if outside guard band)
// Returns number of triangles in output
int ClipTriangleGuardBand(const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2,
                          ClippedPolygon& result, const GuardBand& gb);

// ═══════════════════════════════════════════════════════════════════════════════
// SCREEN-SPACE CLIPPING (2D)
// ═══════════════════════════════════════════════════════════════════════════════

struct ScreenVertex {
    Vec2f position;      // screen-space X, Y
    float depth;         // for z-buffer
    float invW;          // 1/W for perspective correction
    Color4f color;
    Vec2f texCoord;
    float fog;
};

static constexpr int MAX_SCREEN_CLIPPED = 12;

struct ScreenClippedPolygon {
    ScreenVertex vertices[MAX_SCREEN_CLIPPED];
    int vertexCount;

    ScreenClippedPolygon() : vertexCount(0) {}
};

// Clip screen-space polygon against scissor rectangle
int ClipToScissor(const ScreenVertex* vertsIn, int vertCount,
                  ScreenVertex* vertsOut, const ScissorRect& scissor);

// ═══════════════════════════════════════════════════════════════════════════════
// BACKFACE CULLING
// ═══════════════════════════════════════════════════════════════════════════════

// Screen-space backface culling (2D cross product)
// Returns true if triangle should be culled (is back-facing given CW/CCW winding)
bool IsBackFacing(const Vec2f& v0, const Vec2f& v1, const Vec2f& v2, bool frontFaceCCW = true);

// Clip-space backface culling (pre-perspective-division)
bool IsBackFacingClipSpace(const Vec4f& v0, const Vec4f& v1, const Vec4f& v2,
                           bool frontFaceCCW = true);

// N64 geometry mode backface culling
// geometryMode contains G_CULL_FRONT / G_CULL_BACK flags
bool ShouldCullTriangle(const Vec2f& v0, const Vec2f& v1, const Vec2f& v2,
                        uint32_t geometryMode);

// ═══════════════════════════════════════════════════════════════════════════════
// PERSPECTIVE DIVISION AND VIEWPORT TRANSFORM
// ═══════════════════════════════════════════════════════════════════════════════

// Perform perspective division: clipSpace -> NDC
Vec3f PerspectiveDivide(const Vec4f& clipPos);

// Full viewport transform: clipSpace -> screen coordinates
struct ViewportTransform {
    float scaleX, scaleY, scaleZ;
    float transX, transY, transZ;

    ViewportTransform() : scaleX(160), scaleY(-120), scaleZ(0.5f),
                          transX(160), transY(120), transZ(0.5f) {}

    // Configure from N64 viewport parameters
    void SetFromN64(float vpScaleX, float vpScaleY, float vpScaleZ,
                    float vpTransX, float vpTransY, float vpTransZ) {
        scaleX = vpScaleX;
        scaleY = vpScaleY; // Usually negative (Y flip)
        scaleZ = vpScaleZ;
        transX = vpTransX;
        transY = vpTransY;
        transZ = vpTransZ;
    }

    // Transform clip-space to screen-space
    ScreenVertex Transform(const ClipVertex& cv) const;

    // Batch transform
    void TransformBatch(const ClipVertex* clipVerts, ScreenVertex* screenVerts, int count) const;
};

// ═══════════════════════════════════════════════════════════════════════════════
// BRANCH_Z TEST (N64 specific)
// ═══════════════════════════════════════════════════════════════════════════════

// Branch_Z: compare vertex Z in screen coordinates against a threshold
// Used by some games for LOD switching in display lists
bool BranchZTest(const Vec4f& clipPos, const ViewportTransform& vt,
                 float zThreshold, float wThreshold);

// ═══════════════════════════════════════════════════════════════════════════════
// COMPLETE TRIANGLE PIPELINE
// ═══════════════════════════════════════════════════════════════════════════════

// Full clip + cull + viewport transform pipeline for a triangle
// Returns number of screen-space triangles generated (0 = fully culled/clipped)
struct TrianglePipelineConfig {
    ViewportTransform viewport;
    ScissorRect scissor;
    GuardBand guardBand;
    uint32_t geometryMode;
    bool useGuardBand;
    bool useNearClipN64;  // z=0 near clip instead of z=-w

    TrianglePipelineConfig()
        : geometryMode(0), useGuardBand(true), useNearClipN64(true) {}
};

// Output triangle (screen space, ready for rasterization)
struct ScreenTriangle {
    ScreenVertex v[3];
};

static constexpr int MAX_OUTPUT_TRIANGLES = 8; // One input triangle -> max ~8 after clipping

struct TrianglePipelineResult {
    ScreenTriangle triangles[MAX_OUTPUT_TRIANGLES];
    int triangleCount;

    TrianglePipelineResult() : triangleCount(0) {}
};

// Process a single triangle through the full pipeline
TrianglePipelineResult ProcessTriangle(
    const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2,
    const TrianglePipelineConfig& config);

// Process a batch of triangles (index buffer)
// Returns total number of output triangles written to outTriangles
int ProcessTriangleBatch(const ClipVertex* vertices,
                         const uint16_t* indices, int triCount,
                         ScreenTriangle* outTriangles, int maxOutput,
                         const TrianglePipelineConfig& config);

}}} // namespace Nova64::Video::Clip
