/**
 * @file RTBVH.hpp
 * @brief CPU BVH builder for the compute-based ray tracer (MoltenVK / Apple Silicon).
 *
 * MoltenVK does not expose VK_KHR_ray_tracing_pipeline / acceleration_structure,
 * so we cannot use hardware BLAS/TLAS. Instead we build a compact BVH on the CPU
 * each frame and traverse it inside a Vulkan compute shader.
 *
 * The node layout is GPU-friendly (32 bytes, matches the std430 struct in the
 * traversal compute shader) using the classic Wald/Bikker compact form: each
 * node is either internal (triCount == 0, leftFirst = left child index; right
 * child is left+1) or a leaf (triCount > 0, leftFirst = first triangle index
 * into the reordered triangle array).
 */
// SPDX-License-Identifier: MIT
#pragma once

#include "SceneTypes.hpp"
#include <cstdint>
#include <vector>

namespace Nova64::Video::RT {

// 32-byte BVH node — must match the BVHNode std430 layout in the traversal shaders (RTComputeShaders.hpp).
struct GpuBVHNode {
    float aabbMin[3];
    uint32_t leftFirst; // internal: left child node index; leaf: first tri index
    float aabbMax[3];
    uint32_t triCount;  // 0 = internal node, >0 = leaf with this many triangles
};
static_assert(sizeof(GpuBVHNode) == 32, "GpuBVHNode must be 32 bytes for std430");

// Packed, GPU-friendly triangle (eye-space). Matches RTTri in RTComputeShaders.hpp.
// 3 positions + 3 normals + 3 colors + material word, padded to vec4 lanes.
struct GpuRTTri {
    float v0[4]; // xyz eye-space position, w = pad
    float v1[4];
    float v2[4];
    float n0[4]; // xyz eye-space normal,   w = pad
    float n1[4];
    float n2[4];
    float c0[4]; // rgba shade color
    float c1[4];
    float c2[4];
    uint32_t material; // packed flags (translucent, reflective, …)
    uint32_t lights;   // light-table indices: bits 0-3 directional, 4-7 point (15 = none)
    uint32_t pad1, pad2;
};
static_assert(sizeof(GpuRTTri) % 16 == 0, "GpuRTTri must be 16-byte aligned");

// Material flag bits packed into GpuRTTri::material.
enum RTMaterialFlags : uint32_t {
    RT_MAT_TRANSLUCENT = 1u << 0, // blended (water/glass): hit, never occlude
    RT_MAT_REFLECTIVE = 1u << 1,  // env-mapped metal
    RT_MAT_CUTOUT = 1u << 2,      // texture-alpha billboard: invisible to rays
    RT_MAT_USES_SHADE = 1u << 3,  // combiner's final color multiplicatively
                                  // references SHADE — only these surfaces
                                  // are valid de-light/re-light candidates
};

/**
 * Builds a compact BVH from a frame's eye-space triangles.
 *
 * Output (valid until the next Build()):
 *   - Nodes():    flat node array, root at index 0.
 *   - Triangles(): triangles reordered so each leaf's range is contiguous.
 * Both are uploaded verbatim to GPU storage buffers by the compute raytracer.
 */
class RTBVH {
public:
    // Rebuild from scratch for this frame. Safe to call with 0 triangles.
    void Build(const SceneFrameData& frame);

    const std::vector<GpuBVHNode>& Nodes() const { return m_nodes; }
    const std::vector<GpuRTTri>& Triangles() const { return m_tris; }

    uint32_t NodeCount() const { return static_cast<uint32_t>(m_nodes.size()); }
    uint32_t TriangleCount() const { return static_cast<uint32_t>(m_tris.size()); }
    bool Empty() const { return m_tris.empty(); }

private:
    // Per-build scratch.
    struct BuildPrim {
        float centroid[3];
        float aabbMin[3];
        float aabbMax[3];
        uint32_t srcIndex; // index into the source SceneTriangle array
    };

    void Subdivide(uint32_t nodeIdx, std::vector<BuildPrim>& prims, uint32_t start, uint32_t count, uint32_t depth);
    void UpdateNodeBounds(GpuBVHNode& node, const std::vector<BuildPrim>& prims, uint32_t start, uint32_t count) const;

    std::vector<GpuBVHNode> m_nodes;
    std::vector<GpuRTTri> m_tris;
    const SceneFrameData* m_src = nullptr; // borrowed during Build only
    float m_diagSq = 0.0f;                 // scene AABB diagonal², for Pack()
};

} // namespace Nova64::Video::RT
