/**
 * @file ClipMath.cpp
 * @brief Triangle clipping, culling and viewport transform routines declared in ClipMath.hpp.
 */
// SPDX-License-Identifier: MIT

#include "ClipMath.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace Nova64 {
namespace Video {
namespace Clip {

// --- CLIP FLAG COMPUTATION ---

uint32_t ComputeClipFlags(const Vec4f& clipPos) {
    float x = clipPos.f[0], y = clipPos.f[1], z = clipPos.f[2], w = clipPos.f[3];
    uint32_t flags = CLIP_NONE;

    if (x > w) flags |= CLIP_POSX;
    if (x < -w) flags |= CLIP_NEGX;
    if (y > w) flags |= CLIP_POSY;
    if (y < -w) flags |= CLIP_NEGY;
    if (z > w) flags |= CLIP_POSZ;
    if (z < -w) flags |= CLIP_NEGZ;
    if (w <= 0.0f) flags |= CLIP_W;

    return flags;
}

// --- INTERPOLATION HELPER ---

static ClipVertex LerpClipVertex(const ClipVertex& a, const ClipVertex& b, float t) {
    ClipVertex result;
    float ot = 1.0f - t;

    result.position = Vec4f(a.position.f[0] * ot + b.position.f[0] * t, a.position.f[1] * ot + b.position.f[1] * t,
                            a.position.f[2] * ot + b.position.f[2] * t, a.position.f[3] * ot + b.position.f[3] * t);

    result.color = Vec4f(a.color.f[0] * ot + b.color.f[0] * t, a.color.f[1] * ot + b.color.f[1] * t,
                         a.color.f[2] * ot + b.color.f[2] * t, a.color.f[3] * ot + b.color.f[3] * t);

    result.texCoord = Vec2f(a.texCoord.x * ot + b.texCoord.x * t, a.texCoord.y * ot + b.texCoord.y * t);

    result.texCoord1 = Vec2f(a.texCoord1.x * ot + b.texCoord1.x * t, a.texCoord1.y * ot + b.texCoord1.y * t);

    result.fog = a.fog * ot + b.fog * t;
    result.clipFlags = ComputeClipFlags(result.position);

    return result;
}

// Signed distance from a point to a clip plane (>= 0 is inside).
// clipRatio scales the W comparand for the X/Y planes (guard band).
static float PlaneDistance(const Vec4f& pos, ClipPlane plane, float clipRatio = 1.0f) {
    float x = pos.f[0], y = pos.f[1], z = pos.f[2], w = pos.f[3];
    float wg = w * clipRatio;         // guard-band scaled W (only used for X/Y)
    switch (plane) {
    case PLANE_NEAR:   return z + w;  // z >= -w
    case PLANE_FAR:    return w - z;  // z <= w
    case PLANE_LEFT:   return x + wg; // x >= -w*ratio
    case PLANE_RIGHT:  return wg - x; // x <= w*ratio
    case PLANE_BOTTOM: return y + wg; // y >= -w*ratio
    case PLANE_TOP:
        return wg - y;                // y <= w*ratio
    // Clip against w = 0.01 directly, not w = 0 followed by a clamp: interpolating
    // the intersection vertex's xyz to the w = 0 crossing and then forcing w to
    // 0.01 leaves xyz ~100x too large for the stored w. The perspective divide
    // then throws the vertex across the screen (bounded only by the X/Y guard
    // band) and rasterizes a sliver with squished interpolated texcoords: the
    // "duplicate squished tree texture" wedge whenever a billboard crossed the
    // camera plane. Clipping at 0.01 keeps xyz consistent with the final w.
    case PLANE_W_NEAR:
        return w - 0.01f; // w >= 0.01
    // N64 NoN near plane: z >= 0 in clip space. For standard N64 projections
    // z_clip = 0 is the game's actual near plane and every vertex with z > 0
    // also has w >= w(near) > 0, so clipping here instead of at the w = 0.01
    // camera plane never produces the z/w explosion the camera-plane cut causes
    // for far-plane-hugging geometry (Zelda skyboxes: z/w is about 0.5 at the
    // original vertices but about 50 at a w = 0.01 cut, which the far-plane pass
    // then deleted entirely, leaving missing sky cells).
    case PLANE_NEAR_N64: return z; // z >= 0
    default:             return 0.0f;
    }
}

// --- SUTHERLAND-HODGMAN POLYGON CLIPPING ---

int ClipPolygonAgainstPlane(const ClipVertex* vertsIn, int vertCount, ClipVertex* vertsOut, ClipPlane plane,
                            float clipRatio) {
    if (vertCount < 3) return 0;

    int outCount = 0;

    for (int i = 0; i < vertCount; i++) {
        int next = (i + 1) % vertCount;
        const ClipVertex& curr = vertsIn[i];
        const ClipVertex& nxt = vertsIn[next];

        float distCurr = PlaneDistance(curr.position, plane, clipRatio);
        float distNext = PlaneDistance(nxt.position, plane, clipRatio);
        bool currInside = distCurr >= 0.0f;
        bool nextInside = distNext >= 0.0f;

        if (currInside) {
            if (outCount < MAX_CLIPPED_VERTS) {
                vertsOut[outCount++] = curr;
            }
        }

        // Edge crosses plane
        if (currInside != nextInside) {
            float denom = distCurr - distNext;
            if (std::fabs(denom) > Math::kEpsilonSq) {
                float t = distCurr / denom;
                t = std::max(0.0f, std::min(1.0f, t));
                if (outCount < MAX_CLIPPED_VERTS) {
                    ClipVertex& nv = vertsOut[outCount++];
                    nv = LerpClipVertex(curr, nxt, t);
                    // GLideN64 W-near workaround: when clipping against the
                    // camera plane (w=0), the new vertex's w is exactly 0
                    // which would explode 1/w downstream.  Clamp to 0.01 to
                    // match GLideN64 (DepthBufferRender/ClipPolygon.cpp).
                    if (plane == PLANE_W_NEAR && nv.position.f[3] < 0.01f) {
                        nv.position.f[3] = 0.01f;
                    }
                }
            }
        }
    }

    return outCount;
}

int ClipTriangle(const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2, ClippedPolygon& result,
                 float clipRatio) {
    // Trivial rejection. Same-side rejection on X/Y/Z is only valid when all
    // three vertices have w > 0: for a vertex behind the camera the +-w*ratio
    // bounds are negative and its X/Y flags fire spuriously, so a triangle
    // straddling the camera plane (whose near-clipped part may be fully on
    // screen, like the Zelda skybox's overhead cells) would share a spurious bit
    // with a genuinely flagged front vertex and be dropped instead of clipped.
    // Only CLIP_W on all three vertices (all behind the camera) is an
    // unconditional reject; otherwise fall through and let the W-near plane pass
    // sort it out geometrically.
    uint32_t andFlags = v0.clipFlags & v1.clipFlags & v2.clipFlags;
    bool anyBehind = ((v0.clipFlags | v1.clipFlags | v2.clipFlags) & CLIP_W) != 0;
    if ((andFlags & CLIP_W) != 0 || (!anyBehind && andFlags != 0)) {
        result.vertexCount = 0;
        return 0;
    }

    uint32_t orFlags = v0.clipFlags | v1.clipFlags | v2.clipFlags;
    if (orFlags == 0) {
        result.vertices[0] = v0;
        result.vertices[1] = v1;
        result.vertices[2] = v2;
        result.vertexCount = 3;
        return 1;
    }

    // Sutherland-Hodgman clipping against each plane
    ClipVertex bufA[MAX_CLIPPED_VERTS];
    ClipVertex bufB[MAX_CLIPPED_VERTS];
    bufA[0] = v0;
    bufA[1] = v1;
    bufA[2] = v2;
    int count = 3;

    ClipVertex* input = bufA;
    ClipVertex* output = bufB;

    // Only clip against planes that are needed.
    // NEAR_N64 (z >= 0) runs first for camera-plane-straddling triangles:
    // cutting at the game's real near plane keeps w >= w(near) on every produced
    // vertex, so z/w stays sane. Cutting far-plane-hugging geometry at the camera
    // plane (W_NEAR at w = 0.01) instead leaves the interpolated vertex with z of
    // about 0.5-0.8 but w collapsed to 0.01, so z/w is about 50 and the FAR pass
    // deletes the whole polygon. That discarded every Zelda skybox cell whose
    // triangle straddled w = 0 (the dome arcs overhead through the camera plane),
    // giving a black "shattered glass" sky. W_NEAR stays as a residual guard (a
    // no-op once the z >= 0 clip has run).
    // FAR is clipped because the renderer clamps beyond-far z to 0.99999 instead
    // of using GPU depth clamping: a triangle straddling the far plane (long
    // fence or ribbon geometry sweeping behind the camera) would keep its
    // beyond-far part, which rasterizes as a giant sliver against the sky-cleared
    // depth buffer (the brown "tree wedge" on the castle grounds). Real RSP
    // clipping cuts at far too. Triangles entirely beyond far are already
    // rejected via CLIP_POSZ in GBIHandler.
    static const ClipPlane planes[] = {PLANE_NEAR_N64, PLANE_W_NEAR, PLANE_FAR, PLANE_LEFT,
                                       PLANE_RIGHT,    PLANE_BOTTOM, PLANE_TOP};
    static const uint32_t planeFlags[] = {CLIP_W, CLIP_W, CLIP_POSZ, CLIP_NEGX, CLIP_POSX, CLIP_NEGY, CLIP_POSY};
    static const int NUM_CLIP_PLANES = 7;

    for (int p = 0; p < NUM_CLIP_PLANES; p++) {
        // Skip if no vertex crosses this plane
        if ((orFlags & planeFlags[p]) == 0) continue;

        count = ClipPolygonAgainstPlane(input, count, output, planes[p], clipRatio);
        if (count < 3) {
            result.vertexCount = 0;
            return 0;
        }

        ClipVertex* tmp = input;
        input = output;
        output = tmp;
    }

    result.vertexCount = std::min(count, MAX_CLIPPED_VERTS);
    for (int i = 0; i < result.vertexCount; i++) {
        result.vertices[i] = input[i];
    }

    // Number of triangles after fan triangulation
    return std::max(0, result.vertexCount - 2);
}

// --- GUARD BAND CLIPPING ---

// --- SCREEN-SPACE SCISSOR CLIPPING ---

// --- BACKFACE CULLING ---

// --- PERSPECTIVE DIVISION AND VIEWPORT TRANSFORM ---

// --- BRANCH_Z TEST ---

// --- COMPLETE TRIANGLE PIPELINE ---

} // namespace Clip
} // namespace Video
} // namespace Nova64
