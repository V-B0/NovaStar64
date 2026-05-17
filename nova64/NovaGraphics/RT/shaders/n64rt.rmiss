// n64rt.rmiss — N64 hardware raytracing: miss shader (fog/sky color)
// SPDX-License-Identifier: MIT
#version 460
#extension GL_EXT_ray_tracing : require

struct RayPayload {
    vec4 color;
    bool hit;
};
layout(location = 0) rayPayloadInEXT RayPayload g_payload;

void main() {
    // Return a neutral fog color for rays that miss all geometry.
    g_payload.color = vec4(0.05, 0.05, 0.1, 1.0);
    g_payload.hit   = false;
}
