/**
 * @file RTBVH.cpp
 * @brief CPU BVH builder implementation (median split on longest centroid axis).
 */
// SPDX-License-Identifier: MIT
#include "RTBVH.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Nova64::Video::RT {

namespace {
constexpr uint32_t kLeafThreshold = 2;    // max triangles per leaf
constexpr uint32_t kMaxDepth      = 48;   // safety bound on recursion

inline void TriBounds(const SceneTriangle& t,
                      float outMin[3], float outMax[3], float outCentroid[3]) {
    for (int a = 0; a < 3; ++a) {
        float p0 = t.v[0].worldPos[a];
        float p1 = t.v[1].worldPos[a];
        float p2 = t.v[2].worldPos[a];
        outMin[a] = std::min(p0, std::min(p1, p2));
        outMax[a] = std::max(p0, std::max(p1, p2));
        outCentroid[a] = (p0 + p1 + p2) * (1.0f / 3.0f);
    }
}

inline GpuRTTri Pack(const SceneTriangle& t, float sceneDiagSq) {
    GpuRTTri g{};
    auto cp4 = [](float dst[4], const float* src, float w) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = w;
    };
    cp4(g.v0, t.v[0].worldPos, 1.0f);
    cp4(g.v1, t.v[1].worldPos, 1.0f);
    cp4(g.v2, t.v[2].worldPos, 1.0f);
    cp4(g.n0, t.v[0].normal, 0.0f);
    cp4(g.n1, t.v[1].normal, 0.0f);
    cp4(g.n2, t.v[2].normal, 0.0f);
    g.c0[0] = t.v[0].color[0]; g.c0[1] = t.v[0].color[1]; g.c0[2] = t.v[0].color[2]; g.c0[3] = t.v[0].color[3];
    g.c1[0] = t.v[1].color[0]; g.c1[1] = t.v[1].color[1]; g.c1[2] = t.v[1].color[2]; g.c1[3] = t.v[1].color[3];
    g.c2[0] = t.v[2].color[0]; g.c2[1] = t.v[2].color[1]; g.c2[2] = t.v[2].color[2]; g.c2[3] = t.v[2].color[3];

    uint32_t mat = 0;
    if (t.isCutout) mat |= RT_MAT_CUTOUT;
    if (t.isTranslucent) {
        // Blended translucents split by TRIANGLE SIZE: water surfaces are
        // lake-scale triangles; particles and drop-shadow decals are tiny
        // alpha quads that must be ray-invisible like cutouts (any special
        // treatment seams against the relit background, and horizontal decals
        // would even pass the shader's water test and grow reflections).
        const float ex[3] = { t.v[1].worldPos[0] - t.v[0].worldPos[0],
                              t.v[1].worldPos[1] - t.v[0].worldPos[1],
                              t.v[1].worldPos[2] - t.v[0].worldPos[2] };
        const float ey[3] = { t.v[2].worldPos[0] - t.v[0].worldPos[0],
                              t.v[2].worldPos[1] - t.v[0].worldPos[1],
                              t.v[2].worldPos[2] - t.v[0].worldPos[2] };
        const float cx = ex[1]*ey[2] - ex[2]*ey[1];
        const float cy = ex[2]*ey[0] - ex[0]*ey[2];
        const float cz = ex[0]*ey[1] - ex[1]*ey[0];
        const float area = 0.5f * std::sqrt(cx*cx + cy*cy + cz*cz);
        // Large translucents ALSO need to be roughly up/down-facing to count
        // as water (same eye-space ns.y test the shader uses to gate water
        // reflections — N64 cameras stay near-upright, so eye-space "up" is a
        // reasonable proxy for world "up"). Without this, big CAMERA-FACING
        // alpha billboards (the moon/sun sky sprite: soft round texture on an
        // oversized quad, easily bigger than a real lake) pass the pure size
        // check and get treated as solid, primary-ray-hittable "water" — the
        // BVH has no texture alpha, so the sprite's transparent soft edges
        // become a hard opaque rectangle sitting in the sky, visible to rays
        // that should have passed straight through to the actual scene.
        const float ny = (area > 1e-8f) ? (cy / (2.0f * area)) : 0.0f;
        if (area > 1e-3f * sceneDiagSq && std::fabs(ny) > 0.55f)
            mat |= RT_MAT_TRANSLUCENT;   // water
        else
            mat |= RT_MAT_CUTOUT;        // decal / sky billboard
    }
    // Reflective = N64 environment mapping (G_TEXTURE_GEN / _LINEAR). This is the
    // hardware "chrome/metal/shiny" signal — the metal cap, shiny objects, etc.
    // (Values from N64Types.hpp: G_TEXTURE_GEN=0x00040000, _LINEAR=0x00080000.)
    constexpr uint32_t kTexGenBits = 0x00040000u | 0x00080000u;
    if (t.geometryMode & kTexGenBits) mat |= RT_MAT_REFLECTIVE;
    if (t.usesShadeInFinalColor) mat |= RT_MAT_USES_SHADE;
    g.material = mat;
    g.lights = (t.sunLight   == 0xFF ? 15u : (t.sunLight   & 15u))
             | (t.localLight == 0xFF ? 15u : (t.localLight & 15u)) << 4;
    return g;
}
} // namespace

void RTBVH::Build(const SceneFrameData& frame) {
    m_nodes.clear();
    m_tris.clear();
    m_src = &frame;

    const uint32_t triCount = frame.TriangleCount();
    if (triCount == 0) { m_src = nullptr; return; }

    // Build primitive list (centroid + bounds), index into source triangles.
    // Also gather the global scene bounds — Pack() needs the scene scale to
    // size-classify translucent triangles (water vs decal/particle).
    std::vector<BuildPrim> prims(triCount);
    float gmn[3] = {  std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max() };
    float gmx[3] = { -std::numeric_limits<float>::max(),
                     -std::numeric_limits<float>::max(),
                     -std::numeric_limits<float>::max() };
    for (uint32_t i = 0; i < triCount; ++i) {
        TriBounds(frame.triangles[i], prims[i].aabbMin, prims[i].aabbMax, prims[i].centroid);
        prims[i].srcIndex = i;
        for (int a = 0; a < 3; ++a) {
            gmn[a] = std::min(gmn[a], prims[i].aabbMin[a]);
            gmx[a] = std::max(gmx[a], prims[i].aabbMax[a]);
        }
    }
    const float gdx = gmx[0] - gmn[0], gdy = gmx[1] - gmn[1], gdz = gmx[2] - gmn[2];
    m_diagSq = gdx*gdx + gdy*gdy + gdz*gdz;

    // A binary BVH over N prims has at most 2N-1 nodes.
    m_nodes.reserve(2 * triCount);
    m_tris.reserve(triCount);
    m_nodes.emplace_back();                 // root at index 0
    Subdivide(0, prims, 0, triCount, 0);

    m_src = nullptr;
}

void RTBVH::UpdateNodeBounds(GpuBVHNode& node, const std::vector<BuildPrim>& prims,
                             uint32_t start, uint32_t count) const {
    float mn[3] = {  std::numeric_limits<float>::max(),
                     std::numeric_limits<float>::max(),
                     std::numeric_limits<float>::max() };
    float mx[3] = { -std::numeric_limits<float>::max(),
                    -std::numeric_limits<float>::max(),
                    -std::numeric_limits<float>::max() };
    for (uint32_t i = start; i < start + count; ++i) {
        for (int a = 0; a < 3; ++a) {
            mn[a] = std::min(mn[a], prims[i].aabbMin[a]);
            mx[a] = std::max(mx[a], prims[i].aabbMax[a]);
        }
    }
    node.aabbMin[0] = mn[0]; node.aabbMin[1] = mn[1]; node.aabbMin[2] = mn[2];
    node.aabbMax[0] = mx[0]; node.aabbMax[1] = mx[1]; node.aabbMax[2] = mx[2];
}

void RTBVH::Subdivide(uint32_t nodeIdx, std::vector<BuildPrim>& prims,
                      uint32_t start, uint32_t count, uint32_t depth) {
    // NOTE: m_nodes may reallocate inside this function (children emplace_back),
    // so we never hold a GpuBVHNode& across a push_back — re-index each time.
    UpdateNodeBounds(m_nodes[nodeIdx], prims, start, count);

    auto makeLeaf = [&]() {
        uint32_t first = static_cast<uint32_t>(m_tris.size());
        for (uint32_t i = start; i < start + count; ++i)
            m_tris.push_back(Pack(m_src->triangles[prims[i].srcIndex], m_diagSq));
        m_nodes[nodeIdx].leftFirst = first;
        m_nodes[nodeIdx].triCount  = count;
    };

    if (count <= kLeafThreshold || depth >= kMaxDepth) { makeLeaf(); return; }

    // Split on the axis with the largest centroid extent, at the midpoint.
    const GpuBVHNode& nb = m_nodes[nodeIdx];
    float extent[3] = { nb.aabbMax[0] - nb.aabbMin[0],
                        nb.aabbMax[1] - nb.aabbMin[1],
                        nb.aabbMax[2] - nb.aabbMin[2] };
    int axis = 0;
    if (extent[1] > extent[axis]) axis = 1;
    if (extent[2] > extent[axis]) axis = 2;
    float splitPos = nb.aabbMin[axis] + extent[axis] * 0.5f;

    // In-place partition: prims with centroid < splitPos go left.
    uint32_t mid = start;
    for (uint32_t i = start; i < start + count; ++i) {
        if (prims[i].centroid[axis] < splitPos)
            std::swap(prims[i], prims[mid++]);
    }

    // Degenerate split (all on one side) → fall back to median split by count.
    if (mid == start || mid == start + count) {
        std::nth_element(prims.begin() + start,
                         prims.begin() + start + count / 2,
                         prims.begin() + start + count,
                         [axis](const BuildPrim& a, const BuildPrim& b) {
                             return a.centroid[axis] < b.centroid[axis];
                         });
        mid = start + count / 2;
    }

    uint32_t leftCount  = mid - start;
    uint32_t rightCount = count - leftCount;

    // Allocate two children; left at leftIdx, right at leftIdx+1.
    uint32_t leftIdx = static_cast<uint32_t>(m_nodes.size());
    m_nodes.emplace_back();
    m_nodes.emplace_back();
    m_nodes[nodeIdx].leftFirst = leftIdx;
    m_nodes[nodeIdx].triCount  = 0;        // internal node

    Subdivide(leftIdx,     prims, start, leftCount,  depth + 1);
    Subdivide(leftIdx + 1, prims, mid,   rightCount, depth + 1);
}

} // namespace Nova64::Video::RT
