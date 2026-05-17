// n64rt.rchit — N64 hardware raytracing: closest-hit shader
// SPDX-License-Identifier: MIT
#version 460
#extension GL_EXT_ray_tracing          : require
#extension GL_EXT_nonuniform_qualifier : require

struct RayPayload {
    vec4 color;
    bool hit;
};
layout(location = 0) rayPayloadInEXT RayPayload g_payload;

// Vertex attribute buffer — flat float3 positions interleaved
// (could be extended with UVs / normals in a later stage).
// For now we return a simple shading based on hit distance and geometry.

hitAttributeEXT vec2 g_barycentrics;

void main() {
    // Barycentrics from hardware interpolation.
    const vec3 bary = vec3(1.0 - g_barycentrics.x - g_barycentrics.y,
                           g_barycentrics.x,
                           g_barycentrics.y);

    // Simple shading: tint by barycentric weights (stand-in for texture sampling).
    const vec3 color = bary;  // rgb = (u, v, w)

    // Attenuate by hit distance for approximate depth fog.
    const float fogFactor = clamp(1.0 - gl_HitTEXT * 0.01, 0.0, 1.0);
    g_payload.color = vec4(color * fogFactor, 1.0);
    g_payload.hit   = true;
}
