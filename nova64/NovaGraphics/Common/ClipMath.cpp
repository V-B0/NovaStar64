/**
 * @file ClipMath.cpp
 * @brief Clipping Mathematics.
 */
// SPDX-License-Identifier: MIT
// NovaGraphics - Clipping Mathematics Implementation

#include "ClipMath.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace Nova64 { namespace Video { namespace Clip {

// ═══════════════════════════════════════════════════════════════════════════════
// CLIP FLAG COMPUTATION
// ═══════════════════════════════════════════════════════════════════════════════

uint32_t ComputeClipFlags(const Vec4f& clipPos) {
    float x = clipPos.f[0], y = clipPos.f[1], z = clipPos.f[2], w = clipPos.f[3];
    uint32_t flags = CLIP_NONE;

    if (x >  w) flags |= CLIP_POSX;
    if (x < -w) flags |= CLIP_NEGX;
    if (y >  w) flags |= CLIP_POSY;
    if (y < -w) flags |= CLIP_NEGY;
    if (z >  w) flags |= CLIP_POSZ;
    if (z < -w) flags |= CLIP_NEGZ;
    if (w <= 0.0f) flags |= CLIP_W;

    return flags;
}

uint32_t ComputeClipFlagsN64(const Vec4f& clipPos) {
    float x = clipPos.f[0], y = clipPos.f[1], z = clipPos.f[2], w = clipPos.f[3];
    uint32_t flags = CLIP_NONE;

    if (x >  w) flags |= CLIP_POSX;
    if (x < -w) flags |= CLIP_NEGX;
    if (y >  w) flags |= CLIP_POSY;
    if (y < -w) flags |= CLIP_NEGY;
    if (z >  w) flags |= CLIP_POSZ;
    if (z <  0) flags |= CLIP_NEGZ;  // N64 near plane at z=0
    if (w <= 0.0f) flags |= CLIP_W;

    return flags;
}

void ComputeClipFlagsBatch(const Vec4f* clipPositions, uint32_t* flags, int count) {
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
    // SSE2 optimized path
    for (int i = 0; i < count; i++) {
        __m128 pos = clipPositions[i].v;
        __m128 w = _mm_shuffle_ps(pos, pos, _MM_SHUFFLE(3,3,3,3));
        __m128 negW = _mm_sub_ps(_mm_setzero_ps(), w);

        // Compare: x>w, x<-w, y>w, y<-w, z>w, z<-w
        __m128 x = _mm_shuffle_ps(pos, pos, _MM_SHUFFLE(0,0,0,0));
        __m128 y = _mm_shuffle_ps(pos, pos, _MM_SHUFFLE(1,1,1,1));
        __m128 z = _mm_shuffle_ps(pos, pos, _MM_SHUFFLE(2,2,2,2));

        uint32_t f = 0;
        if (_mm_comigt_ss(x, w))    f |= CLIP_POSX;
        if (_mm_comilt_ss(x, negW)) f |= CLIP_NEGX;
        if (_mm_comigt_ss(y, w))    f |= CLIP_POSY;
        if (_mm_comilt_ss(y, negW)) f |= CLIP_NEGY;
        if (_mm_comigt_ss(z, w))    f |= CLIP_POSZ;
        if (_mm_comilt_ss(z, negW)) f |= CLIP_NEGZ;
        if (_mm_comile_ss(w, _mm_setzero_ps())) f |= CLIP_W;

        flags[i] = f;
    }
#else
    for (int i = 0; i < count; i++) {
        flags[i] = ComputeClipFlags(clipPositions[i]);
    }
#endif
}

bool CullVertexRange(const uint32_t* clipFlags, int startIdx, int endIdx) {
    if (startIdx >= endIdx) return true;

    uint32_t andFlags = 0xFFFFFFFF;
    for (int i = startIdx; i < endIdx; i++) {
        andFlags &= clipFlags[i];
    }
    return (andFlags & CLIP_ALL) != 0;
}

// ═══════════════════════════════════════════════════════════════════════════════
// INTERPOLATION HELPER
// ═══════════════════════════════════════════════════════════════════════════════

static ClipVertex LerpClipVertex(const ClipVertex& a, const ClipVertex& b, float t) {
    ClipVertex result;
    float ot = 1.0f - t;

    result.position = Vec4f(
        a.position.f[0]*ot + b.position.f[0]*t,
        a.position.f[1]*ot + b.position.f[1]*t,
        a.position.f[2]*ot + b.position.f[2]*t,
        a.position.f[3]*ot + b.position.f[3]*t
    );

    result.color = Vec4f(
        a.color.f[0]*ot + b.color.f[0]*t,
        a.color.f[1]*ot + b.color.f[1]*t,
        a.color.f[2]*ot + b.color.f[2]*t,
        a.color.f[3]*ot + b.color.f[3]*t
    );

    result.texCoord = Vec2f(
        a.texCoord.x*ot + b.texCoord.x*t,
        a.texCoord.y*ot + b.texCoord.y*t
    );

    result.texCoord1 = Vec2f(
        a.texCoord1.x*ot + b.texCoord1.x*t,
        a.texCoord1.y*ot + b.texCoord1.y*t
    );

    result.fog = a.fog*ot + b.fog*t;
    result.clipFlags = ComputeClipFlags(result.position);

    return result;
}

// Get signed distance from point to clip plane
// clipRatio scales the W comparand for X/Y planes (guard band)
static float PlaneDistance(const Vec4f& pos, ClipPlane plane, float clipRatio = 1.0f) {
    float x = pos.f[0], y = pos.f[1], z = pos.f[2], w = pos.f[3];
    float wg = w * clipRatio;  // guard-band scaled W (only used for X/Y)
    switch (plane) {
        case PLANE_NEAR:   return z + w;        // z >= -w
        case PLANE_FAR:    return w - z;        // z <= w
        case PLANE_LEFT:   return x + wg;       // x >= -w*ratio
        case PLANE_RIGHT:  return wg - x;       // x <= w*ratio
        case PLANE_BOTTOM: return y + wg;       // y >= -w*ratio
        case PLANE_TOP:    return wg - y;       // y <= w*ratio
        // Clip against w = 0.01 DIRECTLY (not w = 0 with a post-clamp): the
        // old scheme interpolated the intersection vertex's xyz to the w=0
        // crossing and then forced w to 0.01, leaving xyz ~100× too large for
        // the stored w. The perspective divide blew the vertex across the
        // screen (bounded only by the X/Y guard band), rasterizing a sliver
        // with squished interpolated texcoords — the "duplicate squished tree
        // texture" wedge, emitted whenever a billboard crossed the camera
        // plane. Clipping at 0.01 keeps xyz consistent with the final w.
        case PLANE_W_NEAR: return w - 0.01f;   // w >= 0.01
        // N64 NoN near plane: z >= 0 in clip space. For standard N64
        // projections, z_clip = 0 is the game's actual near plane and every
        // vertex with z > 0 also has w >= w(near) > 0 — so clipping here
        // (instead of at the w=0.01 camera plane) never produces the
        // pathological z/w explosion that the camera-plane cut creates for
        // far-plane-hugging geometry (Zelda skyboxes: z/w ≈ 0.5 at the
        // original vertices, z/w ≈ 50 at a w=0.01 cut, which the far-plane
        // pass then deleted entirely — the missing sky cells).
        case PLANE_NEAR_N64: return z;         // z >= 0
        default:           return 0.0f;
    }
}

// N64 near plane: z >= 0 (instead of z >= -w)
static float PlaneDistanceN64Near(const Vec4f& pos) {
    return pos.f[2]; // z >= 0
}

// ═══════════════════════════════════════════════════════════════════════════════
// SUTHERLAND-HODGMAN POLYGON CLIPPING
// ═══════════════════════════════════════════════════════════════════════════════

int ClipPolygonAgainstPlane(const ClipVertex* vertsIn, int vertCount,
                            ClipVertex* vertsOut, ClipPlane plane,
                            float clipRatio) {
    if (vertCount < 3) return 0;

    int outCount = 0;

    for (int i = 0; i < vertCount; i++) {
        int next = (i + 1) % vertCount;
        const ClipVertex& curr = vertsIn[i];
        const ClipVertex& nxt  = vertsIn[next];

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
            // Compute intersection parameter
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

int ClipTriangle(const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2,
                 ClippedPolygon& result, float clipRatio) {
    // Early trivial rejection.
    // X/Y/Z same-side rejection is only valid when all three vertices have
    // w > 0: for a vertex behind the camera the ±w·ratio bounds are
    // negative and its X/Y flags fire spuriously, so a camera-plane-
    // straddling triangle (whose near-clipped part may be fully on screen —
    // the Zelda skybox's overhead cells) would share a spurious bit with a
    // genuinely-flagged front vertex and be dropped instead of clipped.
    // Only CLIP_W itself (all three behind the camera) is an unconditional
    // reject; otherwise fall through and let the W-near plane pass sort it
    // out geometrically.
    uint32_t andFlags = v0.clipFlags & v1.clipFlags & v2.clipFlags;
    bool anyBehind = ((v0.clipFlags | v1.clipFlags | v2.clipFlags) & CLIP_W) != 0;
    if ((andFlags & CLIP_W) != 0 || (!anyBehind && andFlags != 0)) {
        result.vertexCount = 0;
        return 0;
    }

    // Early trivial acceptance
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
    bufA[0] = v0; bufA[1] = v1; bufA[2] = v2;
    int count = 3;

    ClipVertex* input = bufA;
    ClipVertex* output = bufB;

    // Only clip against planes that are needed.
    // NEAR_N64 (z >= 0) runs FIRST for camera-plane-straddling triangles:
    // cutting at the game's real near plane keeps w >= w(near) on every
    // produced vertex, so z/w stays sane. The previous order (W_NEAR at
    // w = 0.01 only) cut far-plane-hugging geometry at the camera plane,
    // where the interpolated vertex kept z ≈ 0.5-0.8 but w collapsed to
    // 0.01 → z/w ≈ 50 → the subsequent FAR pass deleted the entire
    // polygon. That silently discarded every Zelda skybox cell whose
    // triangle straddled w = 0 (the dome arcs overhead through the camera
    // plane) — the black "shattered glass" sky. W_NEAR stays as a residual
    // guard (no-op once z >= 0 clipping has run).
    // FAR is CLIPPED: the renderer clamps beyond-far z to 0.99999 instead of
    // GPU depth-clamping, so a triangle STRADDLING the far plane (long fence/
    // ribbon geometry sweeping behind the camera) kept its beyond-far part,
    // which rasterized as a giant sliver visible against the sky-cleared
    // depth buffer — the brown "tree wedge" on the castle grounds. Real RSP
    // clipping cuts at far; so do we. (Fully-beyond-far triangles are already
    // trivially rejected via CLIP_POSZ in GBIHandler.)
    static const ClipPlane planes[] = {
        PLANE_NEAR_N64, PLANE_W_NEAR, PLANE_FAR, PLANE_LEFT, PLANE_RIGHT, PLANE_BOTTOM, PLANE_TOP
    };
    static const uint32_t planeFlags[] = {
        CLIP_W, CLIP_W, CLIP_POSZ, CLIP_NEGX, CLIP_POSX, CLIP_NEGY, CLIP_POSY
    };
    static const int NUM_CLIP_PLANES = 7;

    for (int p = 0; p < NUM_CLIP_PLANES; p++) {
        // Skip if no vertex crosses this plane
        if ((orFlags & planeFlags[p]) == 0) continue;

        count = ClipPolygonAgainstPlane(input, count, output, planes[p], clipRatio);
        if (count < 3) {
            result.vertexCount = 0;
            return 0;
        }

        // Swap buffers
        ClipVertex* tmp = input;
        input = output;
        output = tmp;
    }

    // Copy result to output
    result.vertexCount = std::min(count, MAX_CLIPPED_VERTS);
    for (int i = 0; i < result.vertexCount; i++) {
        result.vertices[i] = input[i];
    }

    // Number of triangles after fan triangulation
    return std::max(0, result.vertexCount - 2);
}

int ClipTriangleNearPlane(const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2,
                          ClippedPolygon& result) {
    // Only clip against near plane (most common operation)
    uint32_t nearFlag = CLIP_NEGZ;
    uint32_t andFlags = v0.clipFlags & v1.clipFlags & v2.clipFlags;
    if (andFlags & nearFlag) { result.vertexCount = 0; return 0; }

    uint32_t orFlags = v0.clipFlags | v1.clipFlags | v2.clipFlags;
    if ((orFlags & nearFlag) == 0) {
        result.vertices[0] = v0;
        result.vertices[1] = v1;
        result.vertices[2] = v2;
        result.vertexCount = 3;
        return 1;
    }

    ClipVertex input[MAX_CLIPPED_VERTS] = {v0, v1, v2};
    result.vertexCount = ClipPolygonAgainstPlane(input, 3, result.vertices, PLANE_NEAR);

    return std::max(0, result.vertexCount - 2);
}

int ClipTriangleN64Near(const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2,
                        ClippedPolygon& result) {
    // N64 near plane at z = 0
    ClipVertex input[MAX_CLIPPED_VERTS] = {v0, v1, v2};
    int inCount = 3;
    int outCount = 0;

    for (int i = 0; i < inCount; i++) {
        int next = (i + 1) % inCount;
        float distCurr = PlaneDistanceN64Near(input[i].position);
        float distNext = PlaneDistanceN64Near(input[next].position);
        bool currIn = distCurr >= 0.0f;
        bool nextIn = distNext >= 0.0f;

        if (currIn) {
            if (outCount < MAX_CLIPPED_VERTS)
                result.vertices[outCount++] = input[i];
        }

        if (currIn != nextIn) {
            float denom = distCurr - distNext;
            if (std::fabs(denom) > Math::kEpsilonSq) {
                float t = distCurr / denom;
                t = std::max(0.0f, std::min(1.0f, t));
                if (outCount < MAX_CLIPPED_VERTS)
                    result.vertices[outCount++] = LerpClipVertex(input[i], input[next], t);
            }
        }
    }

    result.vertexCount = outCount;
    return std::max(0, outCount - 2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// GUARD BAND CLIPPING
// ═══════════════════════════════════════════════════════════════════════════════

bool InsideGuardBand(const Vec4f& clipPos, const GuardBand& gb) {
    if (clipPos.f[3] <= 0.0f) return false;
    float invW = 1.0f / clipPos.f[3];
    float screenX = clipPos.f[0] * invW;
    float screenY = clipPos.f[1] * invW;
    return screenX >= gb.left && screenX <= gb.right
        && screenY >= gb.top && screenY <= gb.bottom;
}

uint32_t ComputeGuardBandFlags(const Vec4f& clipPos, const GuardBand& gb) {
    // Guard band uses scaled X/Y planes; Z uses standard clip
    float x = clipPos.f[0], y = clipPos.f[1], z = clipPos.f[2], w = clipPos.f[3];

    uint32_t flags = 0;

    // Standard near/far clip
    if (z > w)  flags |= CLIP_POSZ;
    if (z < -w) flags |= CLIP_NEGZ;
    if (w <= 0) flags |= CLIP_W;

    // Guard band X/Y: only flag if outside guard band
    float gbScale = std::fabs(w);
    if (x > gb.right * gbScale / 320.0f)  flags |= CLIP_POSX;
    if (x < gb.left * gbScale / 320.0f)   flags |= CLIP_NEGX;
    if (y > gb.bottom * gbScale / 240.0f) flags |= CLIP_POSY;
    if (y < gb.top * gbScale / 240.0f)    flags |= CLIP_NEGY;

    return flags;
}

int ClipTriangleGuardBand(const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2,
                          ClippedPolygon& result, const GuardBand& gb) {
    // Recompute flags with guard band
    uint32_t f0 = ComputeGuardBandFlags(v0.position, gb);
    uint32_t f1 = ComputeGuardBandFlags(v1.position, gb);
    uint32_t f2 = ComputeGuardBandFlags(v2.position, gb);

    // Trivial reject
    if (f0 & f1 & f2) { result.vertexCount = 0; return 0; }

    // Trivial accept (inside guard band)
    if ((f0 | f1 | f2) == 0) {
        result.vertices[0] = v0;
        result.vertices[1] = v1;
        result.vertices[2] = v2;
        result.vertexCount = 3;
        return 1;
    }

    // Need to clip: use standard clipping (near/far + X/Y if outside guard band)
    return ClipTriangle(v0, v1, v2, result);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SCREEN-SPACE SCISSOR CLIPPING
// ═══════════════════════════════════════════════════════════════════════════════

static ScreenVertex LerpScreenVertex(const ScreenVertex& a, const ScreenVertex& b, float t) {
    ScreenVertex r;
    float ot = 1.0f - t;
    r.position = Vec2f(a.position.x*ot + b.position.x*t,
                       a.position.y*ot + b.position.y*t);
    r.depth = a.depth*ot + b.depth*t;
    r.invW = a.invW*ot + b.invW*t;
    r.color = Color4f(a.color.r*ot + b.color.r*t,
                      a.color.g*ot + b.color.g*t,
                      a.color.b*ot + b.color.b*t,
                      a.color.a*ot + b.color.a*t);
    r.texCoord = Vec2f(a.texCoord.x*ot + b.texCoord.x*t,
                       a.texCoord.y*ot + b.texCoord.y*t);
    r.fog = a.fog*ot + b.fog*t;
    return r;
}

// Clip polygon against a single axis-aligned line
static int ClipScreenPolyAgainstEdge(const ScreenVertex* in, int count,
                                      ScreenVertex* out,
                                      int axis, float value, bool clipGreater) {
    if (count < 3) return 0;
    int outCount = 0;

    for (int i = 0; i < count; i++) {
        int next = (i + 1) % count;
        float currVal = (axis == 0) ? in[i].position.x : in[i].position.y;
        float nextVal = (axis == 0) ? in[next].position.x : in[next].position.y;

        bool currInside = clipGreater ? (currVal <= value) : (currVal >= value);
        bool nextInside = clipGreater ? (nextVal <= value) : (nextVal >= value);

        if (currInside) {
            if (outCount < MAX_SCREEN_CLIPPED)
                out[outCount++] = in[i];
        }

        if (currInside != nextInside) {
            float denom = nextVal - currVal;
            if (std::fabs(denom) > Math::kEpsilonSq) {
                float t = (value - currVal) / denom;
                t = std::max(0.0f, std::min(1.0f, t));
                if (outCount < MAX_SCREEN_CLIPPED)
                    out[outCount++] = LerpScreenVertex(in[i], in[next], t);
            }
        }
    }

    return outCount;
}

int ClipToScissor(const ScreenVertex* vertsIn, int vertCount,
                  ScreenVertex* vertsOut, const ScissorRect& scissor) {
    ScreenVertex bufA[MAX_SCREEN_CLIPPED];
    ScreenVertex bufB[MAX_SCREEN_CLIPPED];

    // Start with input
    std::memcpy(bufA, vertsIn, vertCount * sizeof(ScreenVertex));
    int count = vertCount;

    ScreenVertex* input = bufA;
    ScreenVertex* output = bufB;

    // Clip left (x >= x0)
    count = ClipScreenPolyAgainstEdge(input, count, output, 0, static_cast<float>(scissor.x0), false);
    std::swap(input, output);
    if (count < 3) return 0;

    // Clip right (x <= x1)
    count = ClipScreenPolyAgainstEdge(input, count, output, 0, static_cast<float>(scissor.x1), true);
    std::swap(input, output);
    if (count < 3) return 0;

    // Clip top (y >= y0)
    count = ClipScreenPolyAgainstEdge(input, count, output, 1, static_cast<float>(scissor.y0), false);
    std::swap(input, output);
    if (count < 3) return 0;

    // Clip bottom (y <= y1)
    count = ClipScreenPolyAgainstEdge(input, count, output, 1, static_cast<float>(scissor.y1), true);
    std::swap(input, output);

    // Copy result
    if (count >= 3) {
        std::memcpy(vertsOut, input, count * sizeof(ScreenVertex));
    }
    return count;
}

// ═══════════════════════════════════════════════════════════════════════════════
// BACKFACE CULLING
// ═══════════════════════════════════════════════════════════════════════════════

bool IsBackFacing(const Vec2f& v0, const Vec2f& v1, const Vec2f& v2, bool frontFaceCCW) {
    // 2D cross product: (v1-v0) x (v2-v0)
    float cross = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
    // positive cross = CCW, negative = CW
    if (frontFaceCCW) {
        return cross <= 0.0f; // CW is back-facing
    } else {
        return cross >= 0.0f; // CCW is back-facing
    }
}

bool IsBackFacingClipSpace(const Vec4f& v0, const Vec4f& v1, const Vec4f& v2,
                           bool frontFaceCCW) {
    // Cross product in clip space (accounting for W)
    float x0 = v0.f[0], y0 = v0.f[1], w0 = v0.f[3];
    float x1 = v1.f[0], y1 = v1.f[1], w1 = v1.f[3];
    float x2 = v2.f[0], y2 = v2.f[1], w2 = v2.f[3];

    // Screen space cross product without actually dividing by W:
    // (x1/w1 - x0/w0) * (y2/w2 - y0/w0) - (x2/w2 - x0/w0) * (y1/w1 - y0/w0)
    // Multiply through by w0*w1*w2 to avoid division:
    float cross = (x1*w0 - x0*w1) * (y2*w0 - y0*w2)
                - (x2*w0 - x0*w2) * (y1*w0 - y0*w1);

    // The sign of w0*w1*w2 may flip the winding
    float wProduct = w0 * w1 * w2;
    if (wProduct < 0.0f) cross = -cross;

    if (frontFaceCCW) {
        return cross <= 0.0f;
    } else {
        return cross >= 0.0f;
    }
}

bool ShouldCullTriangle(const Vec2f& v0, const Vec2f& v1, const Vec2f& v2,
                        uint32_t geometryMode) {
    // N64 geometry mode culling bits
    static constexpr uint32_t G_CULL_FRONT = 0x00000200;
    static constexpr uint32_t G_CULL_BACK  = 0x00000400;

    bool cullFront = (geometryMode & G_CULL_FRONT) != 0;
    bool cullBack  = (geometryMode & G_CULL_BACK) != 0;

    if (!cullFront && !cullBack) return false;
    if (cullFront && cullBack)   return true; // Cull everything

    float cross = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);

    // On N64, CW winding = front face (screen space with Y-down)
    if (cullBack && cross > 0.0f)  return true;  // CW back-face = positive cross in Y-down
    if (cullFront && cross < 0.0f) return true;   // CCW front-face

    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// PERSPECTIVE DIVISION AND VIEWPORT TRANSFORM
// ═══════════════════════════════════════════════════════════════════════════════

Vec3f PerspectiveDivide(const Vec4f& clipPos) {
    if (std::fabs(clipPos.f[3]) < Math::kEpsilon) {
        return Vec3f(0.0f);
    }
    float invW = 1.0f / clipPos.f[3];
    return Vec3f(clipPos.f[0] * invW, clipPos.f[1] * invW, clipPos.f[2] * invW);
}

ScreenVertex ViewportTransform::Transform(const ClipVertex& cv) const {
    ScreenVertex sv;

    if (std::fabs(cv.position.f[3]) < Math::kEpsilon) {
        sv.position = Vec2f(transX, transY);
        sv.depth = transZ;
        sv.invW = 0.0f;
    } else {
        float invW = 1.0f / cv.position.f[3];
        float ndcX = cv.position.f[0] * invW;
        float ndcY = cv.position.f[1] * invW;
        float ndcZ = cv.position.f[2] * invW;

        sv.position = Vec2f(ndcX * scaleX + transX, ndcY * scaleY + transY);
        sv.depth = ndcZ * scaleZ + transZ;
        sv.invW = invW;
    }

    sv.color = Color4f(cv.color.f[0], cv.color.f[1], cv.color.f[2], cv.color.f[3]);
    sv.texCoord = cv.texCoord;
    sv.fog = cv.fog;

    return sv;
}

void ViewportTransform::TransformBatch(const ClipVertex* clipVerts,
                                        ScreenVertex* screenVerts, int count) const {
    for (int i = 0; i < count; i++) {
        screenVerts[i] = Transform(clipVerts[i]);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// BRANCH_Z TEST
// ═══════════════════════════════════════════════════════════════════════════════

bool BranchZTest(const Vec4f& clipPos, const ViewportTransform& vt,
                 float zThreshold, float wThreshold) {
    if (std::fabs(clipPos.f[3]) < Math::kEpsilon) return false;

    float invW = 1.0f / clipPos.f[3];
    float screenZ = clipPos.f[2] * invW * vt.scaleZ + vt.transZ;

    // Compare against threshold, factoring in W
    float scaledThreshold = zThreshold / wThreshold;
    return screenZ < scaledThreshold;
}

// ═══════════════════════════════════════════════════════════════════════════════
// COMPLETE TRIANGLE PIPELINE
// ═══════════════════════════════════════════════════════════════════════════════

TrianglePipelineResult ProcessTriangle(
    const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2,
    const TrianglePipelineConfig& config)
{
    TrianglePipelineResult result;
    result.triangleCount = 0;

    // Step 1: Trivial rejection
    if (TrivialReject(v0.clipFlags, v1.clipFlags, v2.clipFlags)) {
        return result;
    }

    // Step 2: Clip the triangle
    ClippedPolygon clipped;
    int clippedTriCount;

    if (config.useGuardBand) {
        clippedTriCount = ClipTriangleGuardBand(v0, v1, v2, clipped, config.guardBand);
    } else if (config.useNearClipN64) {
        clippedTriCount = ClipTriangleN64Near(v0, v1, v2, clipped);
    } else {
        clippedTriCount = ClipTriangle(v0, v1, v2, clipped);
    }

    if (clippedTriCount == 0) return result;

    // Step 3: Viewport transform the clipped vertices
    ScreenVertex screenVerts[MAX_CLIPPED_VERTS];
    config.viewport.TransformBatch(clipped.vertices, screenVerts, clipped.vertexCount);

    // Step 4: Fan triangulation of clipped polygon
    for (int i = 0; i < clipped.vertexCount - 2 && result.triangleCount < MAX_OUTPUT_TRIANGLES; i++) {
        const ScreenVertex& sv0 = screenVerts[0];
        const ScreenVertex& sv1 = screenVerts[i + 1];
        const ScreenVertex& sv2 = screenVerts[i + 2];

        // Step 5: Backface culling (screen space)
        if (ShouldCullTriangle(sv0.position, sv1.position, sv2.position, config.geometryMode)) {
            continue;
        }

        // Step 6: Degenerate triangle check
        float area = Math::TriangleUtil::SignedArea2D(sv0.position, sv1.position, sv2.position);
        if (std::fabs(area) < Math::kEpsilonSq) {
            continue;
        }

        // Output triangle
        ScreenTriangle& tri = result.triangles[result.triangleCount];
        tri.v[0] = sv0;
        tri.v[1] = sv1;
        tri.v[2] = sv2;
        result.triangleCount++;
    }

    return result;
}

int ProcessTriangleBatch(const ClipVertex* vertices,
                         const uint16_t* indices, int triCount,
                         ScreenTriangle* outTriangles, int maxOutput,
                         const TrianglePipelineConfig& config) {
    int totalOutput = 0;

    for (int t = 0; t < triCount && totalOutput < maxOutput; t++) {
        const ClipVertex& v0 = vertices[indices[t * 3 + 0]];
        const ClipVertex& v1 = vertices[indices[t * 3 + 1]];
        const ClipVertex& v2 = vertices[indices[t * 3 + 2]];

        TrianglePipelineResult result = ProcessTriangle(v0, v1, v2, config);

        for (int i = 0; i < result.triangleCount && totalOutput < maxOutput; i++) {
            outTriangles[totalOutput++] = result.triangles[i];
        }
    }

    return totalOutput;
}

}}} // namespace Nova64::Video::Clip
